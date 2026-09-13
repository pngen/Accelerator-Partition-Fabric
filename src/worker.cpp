#include "apf/worker.hpp"

#include "apf/backend_synthetic.hpp"
#include "apf/process.hpp"
#include "apf/version.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace apf {

struct PartitionWorker::Impl {
  WorkerOptions options;
  Limits limits;
  ClockPtr clock;
  std::shared_ptr<AcceleratorBackend> backend;
  WorkerId id{};
  WorkerBootId boot{};
  CoordinatorEpoch epoch{};
  std::unique_ptr<FramedChannel> channel;
  std::mutex send_mutex;
  std::mutex state_mutex;
  std::uint64_t sequence{0};
  std::uint64_t applied{0};
  bool fenced{false};
  bool shutdown{false};
  std::atomic<bool> connected{false};
  bool ambiguous_next{false};

  SyntheticBackend* synthetic() {
    return dynamic_cast<SyntheticBackend*>(backend.get());
  }

  Status send(const Frame& frame) {
    std::lock_guard<std::mutex> lock(send_mutex);
    if (channel == nullptr || !channel->valid()) {
      return failure(ErrorCode::Closed, "worker is not connected to a coordinator");
    }
    Frame outbound = frame;
    if (outbound.sequence == 0) {
      outbound.sequence = ++sequence;
    }
    return channel->send(outbound);
  }

  Status checkpoint() {
    if (options.device_state_path.empty()) {
      return success();
    }
    SyntheticBackend* device_backend = synthetic();
    if (device_backend == nullptr) {
      // Only the synthetic backend publishes a serialisable physical model. A
      // real backend owns the hardware and has nothing to checkpoint.
      return success();
    }
    Result<std::string> state = device_backend->export_state();
    if (!state.ok()) {
      return state.error();
    }
    return write_file_atomic(options.device_state_path, state.value());
  }

  Result<BackendLayout> layout_of(const std::string& key) { return backend->query_layout(key); }
};

PartitionWorker::PartitionWorker() : impl_(std::make_unique<Impl>()) {}

PartitionWorker::~PartitionWorker() {
  if (impl_->connected.load()) {
    (void)stop();
  }
}

Result<std::unique_ptr<PartitionWorker>> PartitionWorker::create(WorkerOptions options,
                                                                 ClockPtr clock) {
  const Status valid = options.limits.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  if (options.backend == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "worker requires a hardware backend");
  }
  auto worker = std::unique_ptr<PartitionWorker>(new PartitionWorker());
  worker->impl_->options = options;
  worker->impl_->limits = options.limits;
  worker->impl_->clock = clock ? clock : make_system_clock();
  worker->impl_->backend = options.backend;
  worker->impl_->id = options.worker_id;
  worker->impl_->boot = options.worker_boot.valid()
                            ? options.worker_boot
                            : WorkerBootId::derive(entropy_seed());
  worker->impl_->ambiguous_next = options.ambiguous_next_mutation;
  if (!options.device_state_path.empty()) {
    if (SyntheticBackend* device_backend = worker->impl_->synthetic()) {
      std::error_code error;
      if (file_exists(options.device_state_path)) {
        Result<std::string> state = read_file(options.device_state_path);
        if (!state.ok()) {
          return state.error();
        }
        const Status imported = device_backend->import_state(state.value());
        if (!imported.ok()) {
          return imported.error();
        }
      }
    }
  }
  return worker;
}

const WorkerId& PartitionWorker::id() const noexcept { return impl_->id; }

const WorkerBootId& PartitionWorker::boot() const noexcept { return impl_->boot; }

bool PartitionWorker::connected() const noexcept { return impl_->connected.load(); }

CoordinatorEpoch PartitionWorker::epoch() const noexcept { return impl_->epoch; }

AcceleratorBackend& PartitionWorker::backend() { return *impl_->backend; }

bool PartitionWorker::shutdown_requested() const { return impl_->shutdown; }

Status PartitionWorker::checkpoint_device_state() { return impl_->checkpoint(); }

Status PartitionWorker::republish_evidence() {
  if (!impl_->connected.load()) {
    return failure(ErrorCode::Closed, "worker is not connected");
  }
  Result<std::vector<BackendAccelerator>> devices = impl_->backend->discover();
  if (!devices.ok()) {
    return failure(ErrorCode::BackendFailure, "discovery failed",
                   to_string(devices.error()));
  }
  std::vector<BackendLayout> layouts;
  for (const BackendAccelerator& device : devices.value()) {
    Result<BackendLayout> layout = impl_->backend->query_layout(device.stable_key);
    if (layout.ok()) {
      layouts.push_back(layout.value());
    }
  }
  EvidencePublishMessage publish;
  publish.devices = devices.value();
  publish.layouts = layouts;
  publish.published_at_ms = impl_->clock->now_ms();
  publish.revalidate = true;
  Frame frame;
  frame.type = MessageType::EvidencePublish;
  frame.flags = to_flags(FrameFlag::Request);
  frame.coordinator_epoch = impl_->epoch;
  frame.worker = impl_->id;
  frame.worker_boot = impl_->boot;
  frame.payload = pack_payload(publish, impl_->limits);
  const Status sent = impl_->send(frame);
  if (!sent.ok()) {
    return sent;
  }
  Result<Frame> reply = impl_->channel->receive();
  if (!reply.ok()) {
    return reply.error();
  }
  if (reply.value().type != MessageType::EvidenceAck) {
    return failure(ErrorCode::ProtocolViolation, "coordinator did not acknowledge evidence",
                   to_string(reply.value().type));
  }
  Result<EvidenceAckMessage> ack = unpack_payload<EvidenceAckMessage>(
      reply.value().payload.data(), reply.value().payload.size(), impl_->limits);
  if (!ack.ok()) {
    return ack.error();
  }
  if (!ack.value().accepted) {
    return failure(ErrorCode::RevalidationRequired, "coordinator rejected evidence",
                   ack.value().reason);
  }
  return success();
}

Status PartitionWorker::start() {
  if (impl_->connected.load()) {
    return failure(ErrorCode::AlreadyExists, "worker is already connected");
  }
  Result<TcpSocket> socket = connect_to(impl_->options.coordinator);
  if (!socket.ok()) {
    return socket.error();
  }
  impl_->channel = std::make_unique<FramedChannel>(std::move(socket.value()), impl_->limits);

  HelloMessage hello;
  hello.protocol_version = APF_PROTOCOL_VERSION;
  hello.known_epoch = impl_->epoch;
  hello.worker = impl_->id;
  hello.worker_boot = impl_->boot;
  hello.backend = impl_->backend->name();
  hello.agent_version = version_string();
  hello.instance = impl_->options.description;
  Frame frame;
  frame.type = MessageType::Hello;
  frame.flags = to_flags(FrameFlag::Request);
  frame.coordinator_epoch = impl_->epoch;
  frame.worker = impl_->id;
  frame.worker_boot = impl_->boot;
  frame.payload = pack_payload(hello, impl_->limits);
  Status status = impl_->send(frame);
  if (!status.ok()) {
    return status;
  }
  Result<Frame> reply = impl_->channel->receive();
  if (!reply.ok()) {
    return reply.error();
  }
  if (reply.value().type != MessageType::HelloAck) {
    return failure(ErrorCode::ProtocolViolation, "coordinator did not answer the handshake",
                   to_string(reply.value().type));
  }
  Result<HelloAckMessage> ack = unpack_payload<HelloAckMessage>(
      reply.value().payload.data(), reply.value().payload.size(), impl_->limits);
  if (!ack.ok()) {
    return ack.error();
  }
  if (!ack.value().accepted) {
    return failure(ErrorCode::Fenced, "coordinator refused the worker handshake",
                   ack.value().reason);
  }
  impl_->epoch = ack.value().epoch;
  if (!impl_->id.valid()) {
    impl_->id = ack.value().assigned_worker;
  }
  if (!impl_->id.valid()) {
    return failure(ErrorCode::StaleWorker, "coordinator did not assign a worker identity");
  }

  Result<std::vector<BackendAccelerator>> devices = impl_->backend->discover();
  if (!devices.ok()) {
    return failure(ErrorCode::BackendFailure, "discovery failed", to_string(devices.error()));
  }
  RegisterWorkerMessage registration;
  registration.worker.id = impl_->id;
  registration.worker.boot = impl_->boot;
  registration.worker.backend = impl_->backend->name();
  registration.worker.endpoint = impl_->options.description;
  registration.worker.device_count = static_cast<std::uint32_t>(devices.value().size());
  registration.worker.alive = true;
  registration.devices = devices.value();
  registration.started_at_ms = impl_->clock->now_ms();
  Frame register_frame;
  register_frame.type = MessageType::RegisterWorker;
  register_frame.flags = to_flags(FrameFlag::Request);
  register_frame.coordinator_epoch = impl_->epoch;
  register_frame.worker = impl_->id;
  register_frame.worker_boot = impl_->boot;
  register_frame.payload = pack_payload(registration, impl_->limits);
  status = impl_->send(register_frame);
  if (!status.ok()) {
    return status;
  }
  Result<Frame> register_reply = impl_->channel->receive();
  if (!register_reply.ok()) {
    return register_reply.error();
  }
  if (register_reply.value().type != MessageType::RegisterAck) {
    return failure(ErrorCode::ProtocolViolation, "coordinator did not answer registration",
                   to_string(register_reply.value().type));
  }
  Result<RegisterAckMessage> register_ack = unpack_payload<RegisterAckMessage>(
      register_reply.value().payload.data(), register_reply.value().payload.size(),
      impl_->limits);
  if (!register_ack.ok()) {
    return register_ack.error();
  }
  if (!register_ack.value().accepted) {
    return failure(ErrorCode::Fenced, "coordinator refused the worker registration",
                   register_ack.value().reason);
  }
  impl_->connected.store(true);
  status = impl_->checkpoint();
  if (!status.ok()) {
    return status;
  }
  status = republish_evidence();
  if (!status.ok()) {
    return status;
  }
  if (impl_->options.die_after_register) {
    // Deterministic fault injection: the incarnation disappears immediately
    // after registering, without any graceful shutdown.
    std::_Exit(23);
  }
  return success();
}

Status PartitionWorker::stop() {
  if (!impl_->connected.exchange(false)) {
    return success();
  }
  impl_->shutdown = true;
  (void)impl_->checkpoint();
  if (impl_->channel != nullptr) {
    impl_->channel->close();
  }
  return success();
}

Status PartitionWorker::serve_forever() {
  if (!impl_->connected.load()) {
    return failure(ErrorCode::Closed, "worker is not connected");
  }
  while (!impl_->shutdown) {
    Result<Frame> frame = impl_->channel->receive();
    if (!frame.ok()) {
      impl_->connected.store(false);
      // The coordinator went away or closed the connection: that ends this
      // incarnation, and it is not an error condition.
      if (frame.error().code == ErrorCode::Closed ||
          frame.error().code == ErrorCode::DeviceUnavailable) {
        (void)impl_->checkpoint();
        return success();
      }
      return frame.error();
    }
    switch (frame.value().type) {
      case MessageType::MutationRequest: {
        Result<MutationRequestMessage> request = unpack_payload<MutationRequestMessage>(
            frame.value().payload.data(), frame.value().payload.size(), impl_->limits);
        if (!request.ok()) {
          return request.error();
        }
        const PartitionMutationRequest& mutation = request.value().request;
        MutationResultMessage result;
        result.attempt = request.value().attempt;
        result.idempotency_token = mutation.idempotency_token;
        if (impl_->fenced) {
          result.result.accepted = false;
          result.result.detail = "worker incarnation is fenced and applies no mutation";
        } else if (frame.value().coordinator_epoch != impl_->epoch) {
          result.result.accepted = false;
          result.result.detail = "mutation request arrived under a superseded coordinator epoch";
        } else {
          Result<BackendMutationResult> applied =
              request.value().kind == AttemptKind::CreatePartition
                  ? impl_->backend->create_partitions(mutation)
                  : (request.value().kind == AttemptKind::DestroyPartition
                         ? impl_->backend->destroy_partitions(mutation)
                         : impl_->backend->reconfigure_layout(mutation));
          if (applied.ok()) {
            result.result = applied.value();
          } else {
            result.result.accepted = false;
            result.result.detail = to_string(applied.error());
          }
          impl_->applied += 1;
          (void)impl_->checkpoint();
          if (impl_->ambiguous_next) {
            // The physical change has happened and is checkpointed; this
            // incarnation now disappears before acknowledging it.
            impl_->ambiguous_next = false;
            std::_Exit(24);
          }
        }
        Frame reply;
        reply.type = MessageType::MutationResult;
        reply.flags = to_flags(FrameFlag::Response);
        reply.sequence = frame.value().sequence;
        reply.coordinator_epoch = impl_->epoch;
        reply.worker = impl_->id;
        reply.worker_boot = impl_->boot;
        reply.payload = pack_payload(result, impl_->limits);
        const Status sent = impl_->send(reply);
        if (!sent.ok()) {
          impl_->connected.store(false);
          return sent;
        }
        break;
      }
      case MessageType::QueryRequest: {
        Result<QueryRequestMessage> request = unpack_payload<QueryRequestMessage>(
            frame.value().payload.data(), frame.value().payload.size(), impl_->limits);
        if (!request.ok()) {
          return request.error();
        }
        QueryResponseMessage response;
        response.kind = request.value().kind;
        switch (request.value().kind) {
          case QueryKind::Layout: {
            Result<BackendLayout> layout = impl_->layout_of(request.value().key);
            response.found = layout.ok();
            if (layout.ok()) {
              response.layout = layout.value();
            } else {
              response.detail = to_string(layout.error());
            }
            break;
          }
          case QueryKind::Accelerators: {
            Result<std::vector<BackendAccelerator>> devices = impl_->backend->discover();
            response.found = devices.ok();
            if (devices.ok()) {
              response.devices = devices.value();
            } else {
              response.detail = to_string(devices.error());
            }
            break;
          }
          default:
            response.found = false;
            response.detail = "worker only answers layout and accelerator queries";
            break;
        }
        Frame reply;
        reply.type = MessageType::QueryResponse;
        reply.flags = to_flags(FrameFlag::Response);
        // The request sequence is echoed so the coordinator can correlate the
        // answer with exactly one outstanding query.
        reply.sequence = frame.value().sequence;
        reply.coordinator_epoch = impl_->epoch;
        reply.worker = impl_->id;
        reply.worker_boot = impl_->boot;
        reply.payload = pack_payload(response, impl_->limits);
        const Status sent = impl_->send(reply);
        if (!sent.ok()) {
          impl_->connected.store(false);
          return sent;
        }
        break;
      }
      case MessageType::FenceWorker: {
        Result<FenceMessage> fence = unpack_payload<FenceMessage>(
            frame.value().payload.data(), frame.value().payload.size(), impl_->limits);
        if (!fence.ok()) {
          return fence.error();
        }
        FenceAckMessage ack;
        ack.acknowledged = fence.value().worker_boot == impl_->boot;
        ack.reason = ack.acknowledged ? "incarnation fenced" : "fence targets another incarnation";
        if (ack.acknowledged) {
          impl_->fenced = true;
        }
        Frame reply;
        reply.type = MessageType::FenceAck;
        reply.flags = to_flags(FrameFlag::Response);
        reply.coordinator_epoch = impl_->epoch;
        reply.worker = impl_->id;
        reply.worker_boot = impl_->boot;
        reply.payload = pack_payload(ack, impl_->limits);
        const Status sent = impl_->send(reply);
        if (!sent.ok()) {
          impl_->connected.store(false);
          return sent;
        }
        break;
      }
      case MessageType::Heartbeat: {
        Result<HeartbeatMessage> heartbeat = unpack_payload<HeartbeatMessage>(
            frame.value().payload.data(), frame.value().payload.size(), impl_->limits);
        if (!heartbeat.ok()) {
          return heartbeat.error();
        }
        HeartbeatAckMessage ack;
        ack.epoch = impl_->epoch;
        ack.received_at_ms = impl_->clock->now_ms();
        ack.allowed_attempts = impl_->applied;
        Frame reply;
        reply.type = MessageType::HeartbeatAck;
        reply.flags = to_flags(FrameFlag::Response);
        reply.coordinator_epoch = impl_->epoch;
        reply.worker = impl_->id;
        reply.worker_boot = impl_->boot;
        reply.payload = pack_payload(ack, impl_->limits);
        (void)impl_->send(reply);
        break;
      }
      case MessageType::Shutdown: {
        impl_->shutdown = true;
        (void)impl_->checkpoint();
        impl_->connected.store(false);
        if (impl_->channel != nullptr) {
          impl_->channel->close();
        }
        return success();
      }
      case MessageType::ErrorResponse: {
        // The coordinator reported a failure for an earlier frame; the worker
        // keeps serving, because the failure is already classified there.
        Result<ErrorResponseMessage> error = unpack_payload<ErrorResponseMessage>(
            frame.value().payload.data(), frame.value().payload.size(), impl_->limits);
        if (!error.ok()) {
          return error.error();
        }
        break;
      }
      case MessageType::MutationResult:
      case MessageType::QueryResponse:
      case MessageType::RegisterWorker:
      case MessageType::EvidencePublish:
      case MessageType::FenceAck:
      case MessageType::HeartbeatAck:
      case MessageType::Hello:
      case MessageType::HelloAck:
      case MessageType::RegisterAck:
      case MessageType::EvidenceAck:
      case MessageType::CommandRequest:
      case MessageType::CommandResponse:
      case MessageType::ReconcileRequest:
      case MessageType::ReconcileResponse:
      case MessageType::AdminRequest:
      case MessageType::AdminResponse:
        return failure(ErrorCode::ProtocolViolation,
                       "the worker does not accept this message type",
                       to_string(frame.value().type));
      case MessageType::Invalid:
      case MessageType::Count:
        return failure(ErrorCode::ProtocolViolation, "invalid message type");
    }
  }
  impl_->connected.store(false);
  return success();
}

}  // namespace apf
