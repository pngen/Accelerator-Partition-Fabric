#include "apf/client.hpp"

#include <utility>

namespace apf {

CoordinatorClient::CoordinatorClient() = default;

CoordinatorClient::~CoordinatorClient() { close(); }

Result<std::unique_ptr<CoordinatorClient>> CoordinatorClient::connect(const Endpoint& endpoint,
                                                                      Limits limits) {
  const Status valid = limits.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  Result<TcpSocket> socket = connect_to(endpoint);
  if (!socket.ok()) {
    return socket.error();
  }
  auto client = std::unique_ptr<CoordinatorClient>(new CoordinatorClient());
  client->endpoint_ = endpoint;
  client->limits_ = limits;
  client->channel_ = std::make_unique<FramedChannel>(std::move(socket.value()), limits);
  return client;
}

bool CoordinatorClient::connected() const { return channel_ != nullptr && channel_->valid(); }

void CoordinatorClient::close() {
  if (channel_ != nullptr) {
    channel_->close();
  }
}

Result<CommandResponseMessage> CoordinatorClient::command(const CommandRequestMessage& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel_ == nullptr || !channel_->valid()) {
    return make_error(ErrorCode::Closed, "client is not connected to a coordinator");
  }
  CommandRequestMessage outbound = request;
  if (!command_is_read_only(outbound.kind)) {
    outbound.confirmed = true;
  }
  Frame frame;
  frame.type = MessageType::CommandRequest;
  frame.flags = to_flags(FrameFlag::Request);
  frame.sequence = ++sequence_;
  frame.payload = pack_payload(outbound, limits_);
  const Status sent = channel_->send(frame);
  if (!sent.ok()) {
    close();
    return sent.error();
  }
  for (;;) {
    Result<Frame> reply = channel_->receive();
    if (!reply.ok()) {
      close();
      return reply.error();
    }
    if (reply.value().type == MessageType::CommandResponse) {
      Result<CommandResponseMessage> response = unpack_payload<CommandResponseMessage>(
          reply.value().payload.data(), reply.value().payload.size(), limits_);
      if (!response.ok()) {
        return response.error();
      }
      return response.value();
    }
    if (reply.value().type == MessageType::ErrorResponse) {
      Result<ErrorResponseMessage> error = unpack_payload<ErrorResponseMessage>(
          reply.value().payload.data(), reply.value().payload.size(), limits_);
      if (!error.ok()) {
        return error.error();
      }
      return make_error(error.value().code, error.value().message, error.value().detail);
    }
    return make_error(ErrorCode::ProtocolViolation,
                      "coordinator sent an unexpected message type",
                      to_string(reply.value().type));
  }
}

Result<CommandResponseMessage> CoordinatorClient::inspect_snapshot() {
  CommandRequestMessage request;
  request.kind = CommandKind::InspectSnapshot;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::inspect_accelerators() {
  CommandRequestMessage request;
  request.kind = CommandKind::InspectAccelerators;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::inspect_workers() {
  CommandRequestMessage request;
  request.kind = CommandKind::InspectWorkers;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::inspect_partitions() {
  CommandRequestMessage request;
  request.kind = CommandKind::InspectPartitions;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::query_layout(std::string_view stable_key) {
  CommandRequestMessage request;
  request.kind = CommandKind::QueryLayout;
  request.stable_key = std::string(stable_key);
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::plan(const PartitionRequest& plan_request) {
  CommandRequestMessage request;
  request.kind = CommandKind::Plan;
  request.request = plan_request;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::reserve(const PartitionPlanId& plan_id,
                                                          const WorkerId& worker,
                                                          const WorkerBootId& worker_boot) {
  CommandRequestMessage request;
  request.kind = CommandKind::Reserve;
  request.plan = plan_id;
  request.worker = worker;
  request.worker_boot = worker_boot;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::create(
    const PartitionReservationId& reservation) {
  CommandRequestMessage request;
  request.kind = CommandKind::Create;
  request.reservation = reservation;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::destroy(const PartitionId& partition) {
  CommandRequestMessage request;
  request.kind = CommandKind::Destroy;
  request.partition = partition;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::drain(const PartitionId& partition,
                                                        std::string reason) {
  CommandRequestMessage request;
  request.kind = CommandKind::Drain;
  request.partition = partition;
  request.reason = std::move(reason);
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::complete_drain(const PartitionId& partition) {
  CommandRequestMessage request;
  request.kind = CommandKind::CompleteDrain;
  request.partition = partition;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::cancel_drain(const PartitionId& partition) {
  CommandRequestMessage request;
  request.kind = CommandKind::CancelDrain;
  request.partition = partition;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::release_reservation(
    const PartitionReservationId& reservation, std::string reason) {
  CommandRequestMessage request;
  request.kind = CommandKind::ReleaseReservation;
  request.reservation = reservation;
  request.reason = std::move(reason);
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::reconcile(const WorkerId& worker,
                                                            const WorkerBootId& worker_boot,
                                                            std::string_view stable_key) {
  CommandRequestMessage request;
  request.kind = CommandKind::Reconcile;
  request.worker = worker;
  request.worker_boot = worker_boot;
  request.stable_key = std::string(stable_key);
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::fence_worker(const WorkerId& worker,
                                                               const WorkerBootId& worker_boot,
                                                               std::string reason) {
  CommandRequestMessage request;
  request.kind = CommandKind::FenceWorker;
  request.worker = worker;
  request.worker_boot = worker_boot;
  request.reason = std::move(reason);
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::advance_epoch() {
  CommandRequestMessage request;
  request.kind = CommandKind::AdvanceEpoch;
  return command(request);
}

Result<CommandResponseMessage> CoordinatorClient::persist_state() {
  CommandRequestMessage request;
  request.kind = CommandKind::PersistState;
  return command(request);
}

}  // namespace apf
