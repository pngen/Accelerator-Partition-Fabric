#include "apf/coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <thread>

namespace apf {
namespace {

/// One connection. A session is either a worker incarnation (identified by a
/// hello handshake) or an administrative client.
struct Session {
  Session(TcpSocket socket, Limits limits)
      : channel(std::move(socket), limits) {}

  FramedChannel channel;
  WorkerId worker{};
  WorkerBootId worker_boot{};
  std::string backend;
  bool worker_session{false};
  std::atomic<bool> closed{false};
  std::thread thread;
  std::mutex send_mutex;
  std::uint64_t sequence{0};

  Status send(const Frame& frame) {
    std::lock_guard<std::mutex> lock(send_mutex);
    Frame outbound = frame;
    if (outbound.sequence == 0) {
      outbound.sequence = ++sequence;
    }
    return channel.send(outbound);
  }
};

}  // namespace

namespace {

/// Attaches the profile definitions a capability references, so that a client
/// can reason about portable profile geometry without vendor syntax.
void attach_profiles(const FabricSnapshot& snapshot, BackendAccelerator& device) {
  for (const PartitionProfileId id : device.capability.supported_profiles) {
    const PartitionProfile* profile = snapshot.find_profile(id);
    if (profile != nullptr) {
      device.profiles.push_back(*profile);
    }
  }
}

}  // namespace

struct PartitionCoordinator::Impl {
  /// Back-pointer so that the session handler can reach the coordinator's
  /// public operations without duplicating their logic.
  PartitionCoordinator* owner{nullptr};
  CoordinatorOptions options;
  Limits limits;
  std::unique_ptr<PartitionFabric> fabric;
  TcpListener listener;
  std::thread accept_thread;
  std::atomic<bool> running{false};
  std::atomic<bool> stopping{false};
  std::mutex lifecycle_mutex;
  std::condition_variable lifecycle_cv;
  bool shutdown_flag{false};
  std::string shutdown_reason;
  mutable std::mutex sessions_mutex;
  std::vector<std::shared_ptr<Session>> sessions;
  std::mutex dispatch_mutex;
  std::condition_variable dispatch_cv;

  struct Waiting {
    PartitionAttemptId attempt{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    bool settled{false};
    MutationOutcome outcome{};
  };

  std::map<std::uint64_t, std::shared_ptr<Waiting>> waiting;
  /// One outstanding query per sequence. The session reader thread is the only
  /// reader of a session's socket; a caller that needs an answer from a worker
  /// registers a waiter and is woken by that thread.
  struct PendingQuery {
    bool settled{false};
    bool ok{false};
    std::weak_ptr<Session> session;
    QueryResponseMessage response{};
    Error error{};
  };
  std::atomic<std::uint64_t> next_query_sequence{1};
  std::map<std::uint64_t, std::shared_ptr<PendingQuery>> pending_queries;
  std::condition_variable query_cv;
  PersistenceStore store;

  void fail_pending_queries(const std::shared_ptr<Session>& session, const std::string& reason) {
    {
      std::lock_guard<std::mutex> guard(dispatch_mutex);
      for (auto& entry : pending_queries) {
        const std::shared_ptr<PendingQuery>& query = entry.second;
        if (query->settled) {
          continue;
        }
        const std::shared_ptr<Session> querying_session = query->session.lock();
        if (session != nullptr && querying_session != session) {
          continue;
        }
        query->settled = true;
        query->ok = false;
        query->error = make_error(ErrorCode::Closed, reason);
      }
    }
    query_cv.notify_all();
  }
  bool pending_shutdown{false};
  std::string pending_shutdown_reason;

  Status persist_now() {
    if (!options.persist || options.state_path.empty()) {
      return success();
    }
    if (store.path().empty()) {
      const Status configured = store.set_path(options.state_path);
      if (!configured.ok()) {
        return configured;
      }
    }
    return fabric->save(store);
  }

  /// Handles one inbound frame on a session.
  Status handle_session_frame(Session& session, const Frame& frame);
  /// Resolves the worker that owns a device when the caller did not name one,
  /// then asks it for the current layout.
  Result<BackendLayout> resolve_and_query_layout(const WorkerId& worker,
                                                 const WorkerBootId& worker_boot,
                                                 const std::string& stable_key);
  Status handle_command(Session& session, const Frame& frame, const CommandRequestMessage& request);
  Status handle_mutation_result(const Frame& frame);
  Status handle_register(Session& session, const Frame& frame, const RegisterWorkerMessage& message);
  Status handle_evidence(Session& session, const Frame& frame, const EvidencePublishMessage& message);
  Status handle_query(Session& session, const Frame& frame, const QueryRequestMessage& request);

  std::shared_ptr<Session> find_session(const WorkerId& worker, const WorkerBootId& boot) const {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    for (const std::shared_ptr<Session>& session : sessions) {
      if (session->worker_session && session->worker == worker &&
          session->worker_boot == boot && !session->closed.load()) {
        return session;
      }
    }
    return nullptr;
  }

  void settle(const PartitionAttemptId& attempt, const MutationOutcome& outcome) {
    std::shared_ptr<Waiting> waiter;
    {
      std::lock_guard<std::mutex> lock(dispatch_mutex);
      const auto it = waiting.find(attempt.value());
      if (it == waiting.end() || it->second->settled) {
        return;
      }
      it->second->outcome = outcome;
      it->second->settled = true;
      waiter = it->second;
    }
    dispatch_cv.notify_all();
    (void)waiter;
  }

  /// Marks every attempt owned by a dead or fenced worker incarnation as an
  /// explicit unknown outcome and wakes the callers waiting on them.
  void abandon_worker(const WorkerId& worker, const WorkerBootId& boot, const std::string& reason) {
    std::vector<std::shared_ptr<Waiting>> abandoned;
    {
      std::lock_guard<std::mutex> lock(dispatch_mutex);
      for (auto& entry : waiting) {
        std::shared_ptr<Waiting>& waiter = entry.second;
        if (waiter->settled) {
          continue;
        }
        if (waiter->worker == worker && waiter->worker_boot == boot) {
          waiter->settled = true;
          waiter->outcome.state = AttemptState::OutcomeUnknown;
          waiter->outcome.outcome_unknown = true;
          waiter->outcome.reconciliation_required = true;
          waiter->outcome.attempt = waiter->attempt;
          waiter->outcome.message = reason;
          waiter->outcome.explanation.add(ExplanationCode::OutcomeUnknown, waiter->attempt.str(),
                                          reason);
          abandoned.push_back(waiter);
        }
      }
    }
    dispatch_cv.notify_all();
  }
};

PartitionCoordinator::PartitionCoordinator() : impl_(std::make_unique<Impl>()) {}

PartitionCoordinator::~PartitionCoordinator() {
  if (impl_->running.load()) {
    (void)stop();
  }
}

Result<std::unique_ptr<PartitionCoordinator>> PartitionCoordinator::create(
    CoordinatorOptions options) {
  const Status limits_valid = options.limits.validate();
  if (!limits_valid.ok()) {
    return limits_valid.error();
  }
  auto coordinator = std::unique_ptr<PartitionCoordinator>(new PartitionCoordinator());
  coordinator->impl_->owner = coordinator.get();
  coordinator->impl_->options = options;
  coordinator->impl_->limits = options.limits;
  FabricOptions fabric_options = options.fabric;
  fabric_options.limits = options.limits;
  if (!fabric_options.clock) {
    fabric_options.clock = make_system_clock();
  }
  fabric_options.require_worker_for_mutation = true;
  coordinator->impl_->fabric = std::make_unique<PartitionFabric>(fabric_options);
  return coordinator;
}

const Limits& PartitionCoordinator::limits() const noexcept { return impl_->limits; }

PartitionFabric& PartitionCoordinator::fabric() { return *impl_->fabric; }

const PartitionFabric& PartitionCoordinator::fabric() const { return *impl_->fabric; }

const Endpoint& PartitionCoordinator::endpoint() const noexcept {
  return impl_->listener.endpoint();
}

CoordinatorEpoch PartitionCoordinator::epoch() const {
  return impl_->fabric->coordinator_epoch();
}

bool PartitionCoordinator::running() const { return impl_->running.load(); }

bool PartitionCoordinator::closed() const { return impl_->fabric->closed(); }

bool PartitionCoordinator::shutdown_requested() const { return impl_->shutdown_flag; }

std::vector<WorkerRecord> PartitionCoordinator::workers() const {
  const std::shared_ptr<const FabricSnapshot> snapshot = impl_->fabric->snapshot();
  return snapshot->workers;
}

Result<WorkerRecord> PartitionCoordinator::worker(const WorkerId& id) const {
  const std::shared_ptr<const FabricSnapshot> snapshot = impl_->fabric->snapshot();
  const WorkerRecord* record = snapshot->find_worker(id);
  if (record == nullptr) {
    return make_error(ErrorCode::NotFound, "worker is not registered", id.str());
  }
  return *record;
}

bool PartitionCoordinator::is_worker_live(const WorkerId& worker,
                                          const WorkerBootId& worker_boot) const {
  return impl_->find_session(worker, worker_boot) != nullptr;
}

Status PartitionCoordinator::persist() { return impl_->persist_now(); }

Status PartitionCoordinator::start() {
  if (impl_->running.load()) {
    return failure(ErrorCode::AlreadyExists, "coordinator is already running");
  }
  Result<TcpListener> listener = TcpListener::bind(impl_->options.listen, 32);
  if (!listener.ok()) {
    return listener.error();
  }
  impl_->listener = std::move(listener.value());
  if (impl_->options.persist && !impl_->options.state_path.empty()) {
    const Status configured = impl_->store.set_path(impl_->options.state_path);
    if (!configured.ok()) {
      return configured;
    }
    const Status loaded = impl_->fabric->load(impl_->store);
    if (!loaded.ok()) {
      return loaded;
    }
  }
  const Result<CoordinatorEpoch> epoch = impl_->fabric->advance_coordinator_epoch();
  if (!epoch.ok()) {
    return epoch.error();
  }
  impl_->running.store(true);
  impl_->stopping.store(false);
  impl_->accept_thread = std::thread([this]() {
    while (impl_->running.load()) {
      Result<TcpSocket> socket = impl_->listener.accept();
      if (!socket.ok()) {
        if (!impl_->running.load()) {
          break;
        }
        // A failed accept on a healthy listener is a transient condition; the
        // loop continues rather than tearing the coordinator down.
        continue;
      }
      auto session = std::make_shared<Session>(std::move(socket.value()), impl_->limits);
      {
        std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
        if (impl_->sessions.size() >= impl_->limits.max_workers) {
          session->channel.close();
          continue;
        }
        impl_->sessions.push_back(session);
      }
      std::thread thread([this, session]() {
        while (impl_->running.load() && !session->closed.load()) {
          Result<Frame> frame = session->channel.receive();
          if (!frame.ok()) {
            break;
          }
          const Status handled = impl_->handle_session_frame(*session, frame.value());
          if (!handled.ok()) {
            ErrorResponseMessage error;
            error.code = handled.error().code;
            error.message = handled.error().message;
            error.detail = handled.error().detail;
            error.sequence = frame.value().sequence;
            Frame response;
            response.type = MessageType::ErrorResponse;
            response.flags = FrameFlag::Error | FrameFlag::Response;
            response.coordinator_epoch = impl_->fabric->coordinator_epoch();
            response.payload = pack_payload(error, impl_->limits);
            (void)session->send(response);
            if (handled.error().code == ErrorCode::ProtocolViolation ||
                handled.error().code == ErrorCode::StaleWorker ||
                handled.error().code == ErrorCode::Fenced) {
              break;
            }
          }
        }
        session->closed.store(true);
        session->channel.close();
        impl_->fail_pending_queries(session, "the worker connection closed before answering");
        if (session->worker_session) {
          const WorkerId worker = session->worker;
          const WorkerBootId boot = session->worker_boot;
          (void)impl_->fabric->fence_worker(worker, boot, "worker connection closed");
          impl_->abandon_worker(worker, boot,
                                "worker incarnation disconnected before acknowledging");
        }
      });
      session->thread = std::move(thread);
    }
  });
  return success();
}

Status PartitionCoordinator::stop() {
  if (!impl_->running.exchange(false)) {
    return success();
  }
  impl_->stopping.store(true);
  impl_->listener.close();
  if (impl_->accept_thread.joinable()) {
    impl_->accept_thread.join();
  }
  std::vector<std::shared_ptr<Session>> sessions;
  {
    std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
    sessions = impl_->sessions;
  }
  for (const std::shared_ptr<Session>& session : sessions) {
    if (!session->closed.load()) {
      ShutdownMessage shutdown;
      shutdown.reason = "coordinator is shutting down";
      shutdown.fence = true;
      Frame frame;
      frame.type = MessageType::Shutdown;
      frame.flags = to_flags(FrameFlag::Final);
      frame.coordinator_epoch = impl_->fabric->coordinator_epoch();
      frame.payload = pack_payload(shutdown, impl_->limits);
      (void)session->send(frame);
    }
  }
  // Give the workers a chance to observe the shutdown before the sockets close,
  // then close every channel so no reader thread is left blocked.
  for (const std::shared_ptr<Session>& session : sessions) {
    session->channel.close();
  }
  for (const std::shared_ptr<Session>& session : sessions) {
    if (session->thread.joinable()) {
      session->thread.join();
    }
  }
  {
    std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
    impl_->sessions.clear();
  }
  for (const WorkerRecord& record : workers()) {
    if (!record.fenced) {
      (void)impl_->fabric->fence_worker(record.id, record.boot, "coordinator stopped");
    }
  }
  impl_->abandon_worker(WorkerId{}, WorkerBootId{}, "coordinator stopped");
  {
    std::lock_guard<std::mutex> lock(impl_->dispatch_mutex);
    for (auto& entry : impl_->waiting) {
      if (!entry.second->settled) {
        entry.second->settled = true;
        entry.second->outcome.outcome_unknown = true;
        entry.second->outcome.reconciliation_required = true;
        entry.second->outcome.message = "coordinator stopped while the attempt was in flight";
      }
    }
  }
  impl_->dispatch_cv.notify_all();
  (void)persist();
  return impl_->fabric->close();
}

void PartitionCoordinator::request_shutdown(std::string reason) {
  {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    impl_->shutdown_flag = true;
    impl_->shutdown_reason = std::move(reason);
  }
  impl_->listener.close();
  impl_->lifecycle_cv.notify_all();
}

Status PartitionCoordinator::serve_forever() {
  std::unique_lock<std::mutex> lock(impl_->lifecycle_mutex);
  impl_->lifecycle_cv.wait(lock, [this]() { return impl_->shutdown_flag; });
  lock.unlock();
  return stop();
}

// ---------------------------------------------------------------------------
// Worker-evidence publication and reconciliation
// ---------------------------------------------------------------------------

Status PartitionCoordinator::publish_evidence(const WorkerId& worker,
                                              const WorkerBootId& worker_boot,
                                              const std::vector<BackendAccelerator>& devices,
                                              const std::vector<BackendLayout>& layouts) {
  const Limits limits = impl_->limits;
  (void)limits;
  const std::vector<WorkerRecord> records = workers();
  bool known = false;
  for (const WorkerRecord& record : records) {
    if (record.id == worker && record.boot == worker_boot && !record.fenced) {
      known = true;
      break;
    }
  }
  if (!known) {
    return failure(ErrorCode::StaleWorker,
                   "evidence was published by a worker incarnation that is not current",
                   worker_boot.hex());
  }
  for (const BackendAccelerator& device : devices) {
    const Result<AcceleratorId> registered =
        impl_->fabric->register_backend_accelerator(device);
    if (!registered.ok()) {
      return registered.error();
    }
    const Status bound =
        impl_->fabric->bind_accelerator_worker(registered.value(), worker, worker_boot);
    if (!bound.ok()) {
      return bound;
    }
  }
  for (const BackendLayout& layout : layouts) {
    const Result<ReconciliationReport> report =
        impl_->fabric->reconcile_with_backend_key(layout);
    if (!report.ok()) {
      return report.error();
    }
  }
  return success();
}

Result<BackendLayout> PartitionCoordinator::Impl::resolve_and_query_layout(
    const WorkerId& worker, const WorkerBootId& worker_boot, const std::string& stable_key) {
  WorkerId resolved_worker = worker;
  WorkerBootId resolved_boot = worker_boot;
  if (!resolved_worker.valid() || !resolved_boot.valid()) {
    const Result<AcceleratorId> accelerator = fabric->accelerator_for_key(stable_key);
    if (!accelerator.ok()) {
      return accelerator.error();
    }
    const Result<WorkerRecord> owner_record = fabric->accelerator_worker(accelerator.value());
    if (!owner_record.ok()) {
      return owner_record.error();
    }
    resolved_worker = owner_record.value().id;
    resolved_boot = owner_record.value().boot;
  }
  return owner->query_worker_layout(resolved_worker, resolved_boot, stable_key);
}

Result<BackendLayout> PartitionCoordinator::query_worker_layout(const WorkerId& worker,
                                                               const WorkerBootId& worker_boot,
                                                               std::string_view stable_key) {
  const std::shared_ptr<Session> session = impl_->find_session(worker, worker_boot);
  if (session == nullptr) {
    return make_error(ErrorCode::StaleWorker, "worker incarnation is not connected",
                      worker_boot.hex());
  }
  QueryRequestMessage request;
  request.kind = QueryKind::Layout;
  request.key = std::string(stable_key);
  Frame frame;
  frame.type = MessageType::QueryRequest;
  frame.flags = to_flags(FrameFlag::Request);
  frame.sequence = impl_->next_query_sequence.fetch_add(1);
  frame.coordinator_epoch = impl_->fabric->coordinator_epoch();
  frame.worker = worker;
  frame.worker_boot = worker_boot;
  frame.payload = pack_payload(request, impl_->limits);

  auto pending = std::make_shared<Impl::PendingQuery>();
  pending->session = session;
  {
    std::lock_guard<std::mutex> lock(impl_->dispatch_mutex);
    if (impl_->pending_queries.size() >= impl_->limits.max_message_queue) {
      return make_error(ErrorCode::LimitExceeded, "too many outstanding worker queries");
    }
    impl_->pending_queries[frame.sequence] = pending;
  }
  const Status sent = session->send(frame);
  if (!sent.ok()) {
    std::lock_guard<std::mutex> lock(impl_->dispatch_mutex);
    impl_->pending_queries.erase(frame.sequence);
    return sent.error();
  }
  {
    std::unique_lock<std::mutex> lock(impl_->dispatch_mutex);
    impl_->query_cv.wait(lock, [&pending]() { return pending->settled; });
    impl_->pending_queries.erase(frame.sequence);
  }
  if (!pending->ok) {
    return pending->error;
  }
  if (!pending->response.found) {
    return make_error(ErrorCode::NotFound, "worker does not know the requested device",
                      std::string(stable_key));
  }
  return pending->response.layout;
}

Result<ReconciliationReport> PartitionCoordinator::reconcile_via_worker(
    const WorkerId& worker, const WorkerBootId& worker_boot, std::string_view stable_key) {
  Result<BackendLayout> layout = query_worker_layout(worker, worker_boot, stable_key);
  if (!layout.ok()) {
    return layout.error();
  }
  const Result<AcceleratorId> accelerator = impl_->fabric->accelerator_for_key(stable_key);
  if (!accelerator.ok()) {
    return accelerator.error();
  }
  return impl_->fabric->reconcile(accelerator.value(), layout.value());
}

// ---------------------------------------------------------------------------
// Mutation dispatch
// ---------------------------------------------------------------------------

Result<MutationOutcome> PartitionCoordinator::dispatch_mutation(
    const PartitionReservationId& reservation_id, AttemptKind kind) {
  const Result<PartitionReservation> reservation = impl_->fabric->reservation(reservation_id);
  if (!reservation.ok()) {
    return reservation.error();
  }
  if (!reservation.value().worker.valid() || !reservation.value().worker_boot.valid()) {
    return make_error(ErrorCode::StaleWorker,
                      "reservation is not bound to a worker incarnation");
  }
  const Result<PartitionAttempt> registered =
      impl_->fabric->register_attempt(reservation_id, kind, reservation.value().worker,
                                      reservation.value().worker_boot);
  if (!registered.ok()) {
    return registered.error();
  }
  const PartitionAttempt attempt = registered.value();
  MutationOutcome failed;
  failed.attempt = attempt.id;
  failed.reservation = reservation_id;
  failed.accelerator = attempt.accelerator;
  failed.partitions = attempt.partitions;

  const std::shared_ptr<Session> session =
      impl_->find_session(reservation.value().worker, reservation.value().worker_boot);
  if (session == nullptr) {
    (void)impl_->fabric->mark_attempt_outcome_unknown(
        attempt.id, "no live worker incarnation owns this reservation");
    failed.state = AttemptState::OutcomeUnknown;
    failed.outcome_unknown = true;
    failed.reconciliation_required = true;
    failed.message = "no live worker incarnation owns this reservation";
    failed.explanation.add(ExplanationCode::OutcomeUnknown, attempt.id.str(), failed.message);
    return failed;
  }

  PartitionMutationRequest request;
  request.coordinator_epoch = impl_->fabric->coordinator_epoch();
  request.worker = reservation.value().worker;
  request.worker_boot = reservation.value().worker_boot;
  request.attempt = attempt.id;
  request.idempotency_token = attempt.token;
  request.expected_boot = reservation.value().accelerator_boot;
  request.expected_accelerator_generation = reservation.value().accelerator_generation;
  request.expected_capability_generation = reservation.value().capability_generation;
  request.destructive = kind != AttemptKind::CreatePartition;
  request.full_device_reconfiguration = kind == AttemptKind::ReconfigureLayout;
  const Result<std::string> stable_key =
      impl_->fabric->accelerator_key(reservation.value().accelerator);
  if (!stable_key.ok()) {
    (void)impl_->fabric->mark_attempt_outcome_unknown(attempt.id,
                                                      "accelerator has no backend key");
    failed.state = AttemptState::OutcomeUnknown;
    failed.outcome_unknown = true;
    failed.message = "accelerator has no backend key";
    return failed;
  }
  request.stable_key = stable_key.value();
  const Result<PartitionPlan> plan = impl_->fabric->plan_by_id(reservation.value().plan);
  if (!plan.ok()) {
    (void)impl_->fabric->mark_attempt_outcome_unknown(attempt.id, "plan behind the reservation "
                                                                  "is gone");
    failed.state = AttemptState::OutcomeUnknown;
    failed.outcome_unknown = true;
    failed.message = "plan behind the reservation is gone";
    return failed;
  }
  if (kind == AttemptKind::CreatePartition || kind == AttemptKind::ReconfigureLayout) {
    const std::size_t count = std::min(plan.value().planned_partitions.size(),
                                       kind == AttemptKind::CreatePartition
                                           ? reservation.value().partitions.size()
                                           : plan.value().planned_partitions.size());
    for (std::size_t index = 0; index < count; ++index) {
      NativePartitionSpec spec;
      spec.vendor_native_profile = plan.value().planned_partitions[index].vendor_native_profile;
      spec.profile = plan.value().planned_partitions[index].profile;
      spec.resources = plan.value().planned_partitions[index].resources;
      if (kind == AttemptKind::CreatePartition) {
        spec.logical_partition = reservation.value().partitions[index];
      }
      request.desired.push_back(std::move(spec));
    }
  }
  if (kind == AttemptKind::DestroyPartition || kind == AttemptKind::ReconfigureLayout) {
    for (const PartitionId partition_id : reservation.value().partitions) {
      const Result<PartitionRecord> record = impl_->fabric->partition(partition_id);
      if (record.ok() && !record.value().native_identity.native_id.empty()) {
        request.remove_native_ids.push_back(record.value().native_identity.native_id);
      }
    }
  }

  auto waiter = std::make_shared<Impl::Waiting>();
  waiter->attempt = attempt.id;
  waiter->worker = reservation.value().worker;
  waiter->worker_boot = reservation.value().worker_boot;
  {
    std::lock_guard<std::mutex> lock(impl_->dispatch_mutex);
    if (impl_->waiting.size() >= impl_->limits.max_pending_attempts * 4) {
      return make_error(ErrorCode::LimitExceeded, "too many in-flight dispatches");
    }
    impl_->waiting[attempt.id.value()] = waiter;
  }

  MutationRequestMessage message;
  message.attempt = attempt.id;
  message.reservation = reservation_id;
  message.kind = kind;
  message.request = request;
  Frame frame;
  frame.type = MessageType::MutationRequest;
  frame.flags = to_flags(FrameFlag::Request);
  frame.coordinator_epoch = request.coordinator_epoch;
  frame.worker = request.worker;
  frame.worker_boot = request.worker_boot;
  frame.payload = pack_payload(message, impl_->limits);
  const Status sent = session->send(frame);
  if (!sent.ok()) {
    (void)impl_->fabric->mark_attempt_outcome_unknown(attempt.id,
                                                      "dispatch failed: " +
                                                          to_string(sent.error()));
    std::lock_guard<std::mutex> lock(impl_->dispatch_mutex);
    waiter->settled = true;
    waiter->outcome.outcome_unknown = true;
    waiter->outcome.reconciliation_required = true;
    waiter->outcome.attempt = attempt.id;
    waiter->outcome.reservation = reservation_id;
    waiter->outcome.state = AttemptState::OutcomeUnknown;
    waiter->outcome.message = "dispatch failed before the mutation was acknowledged";
    return waiter->outcome;
  }

  std::unique_lock<std::mutex> lock(impl_->dispatch_mutex);
  impl_->dispatch_cv.wait(lock, [&waiter]() { return waiter->settled; });
  MutationOutcome outcome = waiter->outcome;
  impl_->waiting.erase(attempt.id.value());
  lock.unlock();
  if (!outcome.attempt.valid()) {
    outcome.attempt = attempt.id;
  }
  if (!outcome.reservation.valid()) {
    outcome.reservation = reservation_id;
  }
  return outcome;
}

Result<MutationOutcome> PartitionCoordinator::create_partitions(const PartitionRequest& request,
                                                                const WorkerId& worker,
                                                                const WorkerBootId& worker_boot,
                                                                std::string_view stable_key) {
  const Result<AcceleratorId> accelerator = impl_->fabric->accelerator_for_key(stable_key);
  if (!accelerator.ok()) {
    return accelerator.error();
  }
  PartitionRequest scoped = request;
  scoped.selector.allowed = {accelerator.value()};
  scoped.selector.excluded.clear();
  Result<PartitionPlan> plan = impl_->fabric->plan(scoped);
  if (!plan.ok()) {
    return plan.error();
  }
  if (!plan.value().feasible()) {
    return make_error(ErrorCode::PolicyRejected,
                      "plan is not feasible: " + std::string(to_string(plan.value().outcome)),
                      plan.value().explanation.format());
  }
  Result<PartitionReservation> reservation = impl_->fabric->reserve_for_worker(
      plan.value().id, worker, worker_boot, impl_->fabric->coordinator_epoch());
  if (!reservation.ok()) {
    return reservation.error();
  }
  Result<MutationOutcome> outcome =
      dispatch_mutation(reservation.value().id, AttemptKind::CreatePartition);
  if (!outcome.ok()) {
    (void)impl_->fabric->release_reservation(reservation.value().id,
                                             "dispatch failed before the mutation was applied");
    return outcome;
  }
  if (impl_->options.persist) {
    (void)persist();
  }
  return outcome;
}

Result<MutationOutcome> PartitionCoordinator::destroy_partition(const PartitionId& partition) {
  const Result<PartitionRecord> record = impl_->fabric->partition(partition);
  if (!record.ok()) {
    return record.error();
  }
  const Result<PartitionReservation> reservation =
      impl_->fabric->create_destruction_reservation(partition);
  if (!reservation.ok()) {
    return reservation.error();
  }
  Result<MutationOutcome> outcome =
      dispatch_mutation(reservation.value().id, AttemptKind::DestroyPartition);
  if (impl_->options.persist) {
    (void)persist();
  }
  return outcome;
}


// ---------------------------------------------------------------------------
// Session frame handling
// ---------------------------------------------------------------------------

Status PartitionCoordinator::Impl::handle_register(Session& session, const Frame& frame,
                                                   const RegisterWorkerMessage& message) {
  if (!message.worker.id.valid() || !message.worker.boot.valid()) {
    return failure(ErrorCode::InvalidArgument,
                   "worker registration requires a worker and boot identity");
  }
  if (frame.coordinator_epoch != fabric->coordinator_epoch()) {
    return failure(ErrorCode::StaleCoordinatorEpoch,
                   "registration arrived under a superseded coordinator epoch",
                   std::to_string(frame.coordinator_epoch.value()));
  }
  session.worker = message.worker.id;
  session.worker_boot = message.worker.boot;
  session.backend = message.worker.backend;
  session.worker_session = true;
  WorkerRecord record = message.worker;
  record.coordinator_epoch = frame.coordinator_epoch;
  record.alive = true;
  record.registered_at_ms = message.started_at_ms;
  record.last_seen_ms = message.started_at_ms;
  const Status noted = fabric->note_worker(record);
  RegisterAckMessage ack;
  ack.epoch = fabric->coordinator_epoch();
  if (!noted.ok()) {
    ack.accepted = false;
    ack.reason = to_string(noted.error());
  } else {
    ack.accepted = true;
    for (const BackendAccelerator& device : message.devices) {
      const Result<AcceleratorId> registered = fabric->register_backend_accelerator(device);
      if (registered.ok()) {
        ack.registered_accelerators.push_back(registered.value());
      } else {
        ack.rejected_devices.push_back(device.stable_key + ": " +
                                      to_string(registered.error()));
      }
    }
  }
  Frame response;
  response.type = MessageType::RegisterAck;
  response.flags = to_flags(FrameFlag::Response);
  response.coordinator_epoch = ack.epoch;
  response.worker = session.worker;
  response.worker_boot = session.worker_boot;
  response.payload = pack_payload(ack, limits);
  return session.send(response);
}

Status PartitionCoordinator::Impl::handle_evidence(Session& session, const Frame& frame,
                                                   const EvidencePublishMessage& message) {
  if (!session.worker_session || session.worker != frame.worker ||
      session.worker_boot != frame.worker_boot) {
    return failure(ErrorCode::StaleWorker,
                   "evidence arrived on a session that is not bound to that incarnation");
  }
  if (frame.coordinator_epoch != fabric->coordinator_epoch()) {
    return failure(ErrorCode::StaleCoordinatorEpoch,
                   "evidence arrived under a superseded coordinator epoch");
  }
  EvidenceAckMessage ack;
  const Status published = owner->publish_evidence(session.worker, session.worker_boot,
                                                  message.devices, message.layouts);
  ack.accepted = published.ok();
  ack.reason = published.ok() ? std::string() : to_string(published.error());
  for (const BackendAccelerator& device : message.devices) {
    const Result<AcceleratorId> id = fabric->accelerator_for_key(device.stable_key);
    if (id.ok()) {
      ack.accelerators.push_back(id.value());
    }
  }
  Frame response;
  response.type = MessageType::EvidenceAck;
  response.flags = to_flags(FrameFlag::Response);
  response.coordinator_epoch = fabric->coordinator_epoch();
  response.worker = session.worker;
  response.worker_boot = session.worker_boot;
  response.payload = pack_payload(ack, limits);
  return session.send(response);
}

Status PartitionCoordinator::Impl::handle_mutation_result(const Frame& frame) {
  Result<MutationResultMessage> message = unpack_payload<MutationResultMessage>(
      frame.payload.data(), frame.payload.size(), limits);
  if (!message.ok()) {
    return message.error();
  }
  const Result<PartitionAttempt> attempt = fabric->attempt(message.value().attempt);
  if (!attempt.ok()) {
    return attempt.error();
  }
  if (!attempt.value().worker_boot.valid() ||
      attempt.value().worker_boot != frame.worker_boot) {
    return failure(ErrorCode::StaleWorker,
                   "mutation result arrived from a superseded worker incarnation",
                   frame.worker_boot.hex());
  }
  if (attempt.value().token != message.value().idempotency_token) {
    return failure(ErrorCode::StaleReservation,
                   "mutation result does not match the registered attempt token");
  }
  const BackendLayout* observed = message.value().result.observed_layout.device_present
                                      ? &message.value().result.observed_layout
                                      : nullptr;
  const Result<MutationOutcome> outcome = fabric->apply_mutation_result(
      message.value().attempt, message.value().result, observed);
  if (!outcome.ok()) {
    return outcome.error();
  }
  settle(message.value().attempt, outcome.value());
  return success();
}

Status PartitionCoordinator::Impl::handle_query(Session& session, const Frame& frame,
                                                const QueryRequestMessage& request) {
  (void)frame;
  QueryResponseMessage response;
  response.kind = request.kind;
  switch (request.kind) {
    case QueryKind::Layout: {
      response.found = !request.key.empty();
      response.detail = "layout requests are answered by the worker that owns the hardware";
      break;
    }
    case QueryKind::Accelerators: {
      const std::shared_ptr<const FabricSnapshot> snapshot = fabric->snapshot();
      response.found = true;
      for (const AcceleratorView& view : snapshot->accelerators) {
        BackendAccelerator device;
        device.stable_key = view.accelerator.identifiers.uuid;
        device.backend = view.accelerator.backend;
        device.identifiers = view.accelerator.identifiers;
        device.physical_totals = view.accelerator.physical_totals;
        device.capability = view.accelerator.capability;
        device.boot_id = view.accelerator.boot_id;
        device.locality = view.accelerator.locality;
        device.evidence = view.accelerator.evidence;
        device.provenance = view.accelerator.provenance;
        device.unsupported_reason = view.accelerator.unsupported_reason;
        attach_profiles(*snapshot, device);
        response.devices.push_back(std::move(device));
      }
      break;
    }
    case QueryKind::Workers: {
      response.found = true;
      response.workers = owner->workers();
      break;
    }
    case QueryKind::Partitions: {
      const std::shared_ptr<const FabricSnapshot> snapshot = fabric->snapshot();
      response.found = true;
      response.partitions = snapshot->partitions;
      break;
    }
    case QueryKind::Attempt: {
      const Result<PartitionAttempt> attempt = fabric->attempt(request.attempt);
      if (attempt.ok()) {
        response.found = true;
        response.attempt = attempt.value();
      } else {
        response.found = false;
        response.detail = to_string(attempt.error());
      }
      break;
    }
    case QueryKind::Snapshot: {
      const std::shared_ptr<const FabricSnapshot> snapshot = fabric->snapshot();
      response.found = true;
      response.snapshot_text = snapshot->render();
      break;
    }
    default:
      return failure(ErrorCode::InvalidRequest, "unknown query kind");
  }
  Frame reply;
  reply.type = MessageType::QueryResponse;
  reply.flags = to_flags(FrameFlag::Response);
  reply.coordinator_epoch = fabric->coordinator_epoch();
  reply.worker = session.worker;
  reply.worker_boot = session.worker_boot;
  reply.payload = pack_payload(response, limits);
  return session.send(reply);
}

Status PartitionCoordinator::Impl::handle_command(Session& session, const Frame& frame,
                                                  const CommandRequestMessage& request) {
  (void)frame;
  CommandResponseMessage response;
  response.coordinator_epoch = fabric->coordinator_epoch();
  if (!command_is_read_only(request.kind) && !request.confirmed) {
    response.accepted = false;
    response.code = ErrorCode::PolicyRejected;
    response.message = "administrative commands must be explicitly confirmed";
    Frame reply;
    reply.type = MessageType::CommandResponse;
    reply.flags = to_flags(FrameFlag::Response);
    reply.coordinator_epoch = response.coordinator_epoch;
    reply.payload = pack_payload(response, limits);
    return session.send(reply);
  }
  switch (request.kind) {
    case CommandKind::InspectSnapshot: {
      const std::shared_ptr<const FabricSnapshot> snapshot = fabric->snapshot();
      response.accepted = true;
      response.snapshot_text = snapshot->render();
      break;
    }
    case CommandKind::InspectAccelerators: {
      const std::shared_ptr<const FabricSnapshot> snapshot = fabric->snapshot();
      response.accepted = true;
      for (const AcceleratorView& view : snapshot->accelerators) {
        BackendAccelerator device;
        device.stable_key = view.accelerator.identifiers.uuid;
        device.backend = view.accelerator.backend;
        device.identifiers = view.accelerator.identifiers;
        device.physical_totals = view.accelerator.physical_totals;
        device.capability = view.accelerator.capability;
        device.boot_id = view.accelerator.boot_id;
        device.locality = view.accelerator.locality;
        device.evidence = view.accelerator.evidence;
        device.provenance = view.accelerator.provenance;
        device.unsupported_reason = view.accelerator.unsupported_reason;
        attach_profiles(*snapshot, device);
        response.devices.push_back(std::move(device));
      }
      break;
    }
    case CommandKind::InspectWorkers: {
      response.accepted = true;
      response.workers = owner->workers();
      for (WorkerRecord& record : response.workers) {
        record.alive = record.alive && find_session(record.id, record.boot) != nullptr;
      }
      break;
    }
    case CommandKind::InspectPartitions: {
      const std::shared_ptr<const FabricSnapshot> snapshot = fabric->snapshot();
      response.accepted = true;
      response.partition_records = snapshot->partitions;
      break;
    }
    case CommandKind::QueryLayout: {
      Result<BackendLayout> layout =
          resolve_and_query_layout(request.worker, request.worker_boot, request.stable_key);
      response.accepted = layout.ok();
      if (layout.ok()) {
        response.layout = layout.value();
      } else {
        response.code = layout.error().code;
        response.message = to_string(layout.error());
      }
      break;
    }
    case CommandKind::PublishEvidence: {
      Result<BackendLayout> layout =
          resolve_and_query_layout(request.worker, request.worker_boot, request.stable_key);
      if (!layout.ok()) {
        response.code = layout.error().code;
        response.message = to_string(layout.error());
        break;
      }
      const Result<AcceleratorId> accelerator = fabric->accelerator_for_key(request.stable_key);
      if (!accelerator.ok()) {
        response.code = accelerator.error().code;
        response.message = to_string(accelerator.error());
        break;
      }
      const Result<ReconciliationReport> report =
          fabric->reconcile(accelerator.value(), layout.value());
      response.accepted = report.ok();
      if (report.ok()) {
        response.matched = report.value().matched;
        response.missing = report.value().missing;
        response.unexpected = report.value().unexpected;
        response.adopted = report.value().adopted;
        response.revalidation_required = report.value().revalidation_required;
        response.message = report.value().summary();
      } else {
        response.code = report.error().code;
        response.message = to_string(report.error());
      }
      break;
    }
    case CommandKind::Plan:
    case CommandKind::PlanReconfiguration: {
      Result<PartitionPlan> plan = [&]() -> Result<PartitionPlan> {
        if (request.kind == CommandKind::Plan) {
          return fabric->plan(request.request);
        }
        // A reconfiguration command rebuilds the device so that it holds
        // exactly the requested number of partitions of the requested profile.
        const PartitionProfile* profile = nullptr;
        {
          const std::shared_ptr<const FabricSnapshot> snapshot = fabric->snapshot();
          profile = snapshot->find_profile(request.request.profile);
        }
        if (profile == nullptr) {
          return make_error(ErrorCode::UnknownCapability,
                            "reconfiguration target names an unregistered profile");
        }
        std::vector<PlannedPartition> desired;
        for (std::uint32_t ordinal = 0; ordinal < request.request.count; ++ordinal) {
          PlannedPartition planned;
          planned.profile = profile->id;
          planned.profile_generation = profile->generation;
          planned.resources = profile->resources;
          planned.ordinal = ordinal;
          planned.vendor_native_profile = profile->vendor_native;
          desired.push_back(std::move(planned));
        }
        return fabric->plan_reconfiguration(request.accelerator, desired, request.request);
      }();
      if (!plan.ok()) {
        response.code = plan.error().code;
        response.message = to_string(plan.error());
        break;
      }
      response.accepted = true;
      response.plan = plan.value().id;
      response.plan_generation = plan.value().generation;
      response.plan_outcome = plan.value().outcome;
      response.plan_steps = plan.value().steps;
      response.planned_partitions = plan.value().planned_partitions;
      response.candidates = plan.value().candidates;
      response.message = plan.value().describe();
      response.detail = plan.value().explanation.format();
      break;
    }
    case CommandKind::Reserve: {
      const CoordinatorEpoch epoch = fabric->coordinator_epoch();
      Result<PartitionReservation> reservation = request.worker.valid()
                                                     ? fabric->reserve_for_worker(
                                                           request.plan, request.worker,
                                                           request.worker_boot, epoch)
                                                     : fabric->reserve(request.plan);
      if (!reservation.ok()) {
        response.code = reservation.error().code;
        response.message = to_string(reservation.error());
        break;
      }
      response.accepted = true;
      response.reservation = reservation.value().id;
      response.reservation_lifecycle = reservation.value().lifecycle;
      response.message = "reservation accepted";
      break;
    }
    case CommandKind::Create: {
      Result<MutationOutcome> outcome =
          owner->dispatch_mutation(request.reservation, AttemptKind::CreatePartition);
      if (!outcome.ok()) {
        response.code = outcome.error().code;
        response.message = to_string(outcome.error());
        break;
      }
      response.accepted = outcome.value().committed;
      response.code = outcome.value().committed ? ErrorCode::Ok : ErrorCode::AmbiguousCompletion;
      response.message = outcome.value().message;
      response.detail = outcome.value().explanation.format();
      response.attempt = outcome.value().attempt;
      response.attempt_state = outcome.value().state;
      response.outcome_unknown = outcome.value().outcome_unknown;
      response.verified_physically = outcome.value().verified_physically;
      response.partitions = outcome.value().partitions;
      response.generations = outcome.value().generations;
      (void)persist_now();
      break;
    }
    case CommandKind::Destroy: {
      Result<MutationOutcome> outcome = owner->destroy_partition(request.partition);
      if (!outcome.ok()) {
        response.code = outcome.error().code;
        response.message = to_string(outcome.error());
        break;
      }
      response.accepted = outcome.value().committed;
      response.code = outcome.value().committed ? ErrorCode::Ok : ErrorCode::AmbiguousCompletion;
      response.message = outcome.value().message;
      response.detail = outcome.value().explanation.format();
      response.attempt = outcome.value().attempt;
      response.attempt_state = outcome.value().state;
      response.outcome_unknown = outcome.value().outcome_unknown;
      response.partitions = outcome.value().partitions;
      (void)persist_now();
      break;
    }
    case CommandKind::Drain: {
      const Status drained = fabric->begin_drain(request.partition, request.reason);
      response.accepted = drained.ok();
      response.code = drained.ok() ? ErrorCode::Ok : drained.error().code;
      response.message = drained.ok() ? "drain started" : to_string(drained.error());
      break;
    }
    case CommandKind::CompleteDrain: {
      const Status drained = fabric->complete_drain(request.partition);
      response.accepted = drained.ok();
      response.code = drained.ok() ? ErrorCode::Ok : drained.error().code;
      response.message = drained.ok() ? "drain complete" : to_string(drained.error());
      break;
    }
    case CommandKind::CancelDrain: {
      const Status cancelled = fabric->cancel_drain(request.partition);
      response.accepted = cancelled.ok();
      response.code = cancelled.ok() ? ErrorCode::Ok : cancelled.error().code;
      response.message = cancelled.ok() ? "drain cancelled" : to_string(cancelled.error());
      break;
    }
    case CommandKind::ReleaseReservation: {
      const Status released = fabric->release_reservation(request.reservation, request.reason);
      response.accepted = released.ok();
      response.code = released.ok() ? ErrorCode::Ok : released.error().code;
      response.message = released.ok() ? "reservation released" : to_string(released.error());
      break;
    }
    case CommandKind::Reconcile: {
      Result<ReconciliationReport> report =
          owner->reconcile_via_worker(request.worker, request.worker_boot, request.stable_key);
      response.accepted = report.ok();
      if (report.ok()) {
        response.matched = report.value().matched;
        response.missing = report.value().missing;
        response.unexpected = report.value().unexpected;
        response.adopted = report.value().adopted;
        response.revalidation_required = report.value().revalidation_required;
        response.message = report.value().summary();
      } else {
        response.code = report.error().code;
        response.message = to_string(report.error());
      }
      break;
    }
    case CommandKind::FenceWorker: {
      const Status fenced =
          owner->fence_worker(request.worker, request.worker_boot, request.reason);
      response.accepted = fenced.ok();
      response.code = fenced.ok() ? ErrorCode::Ok : fenced.error().code;
      response.message = fenced.ok() ? "worker fenced" : to_string(fenced.error());
      break;
    }
    case CommandKind::AdvanceEpoch: {
      const Result<CoordinatorEpoch> epoch = fabric->advance_coordinator_epoch();
      response.accepted = epoch.ok();
      response.coordinator_epoch = epoch.ok() ? epoch.value() : fabric->coordinator_epoch();
      response.message = epoch.ok() ? "epoch advanced" : to_string(epoch.error());
      break;
    }
    case CommandKind::PersistState: {
      const Status saved = persist_now();
      response.accepted = saved.ok();
      response.code = saved.ok() ? ErrorCode::Ok : saved.error().code;
      response.message = saved.ok() ? "durable state saved" : to_string(saved.error());
      break;
    }
    case CommandKind::ShutdownCoordinator: {
      // Acknowledged first, then stopped: the caller must be able to observe
      // the acknowledgement of the command that ends the process.
      response.accepted = true;
      response.message = "coordinator will shut down after this acknowledgement";
      pending_shutdown = true;
      pending_shutdown_reason = request.reason.empty() ? "administrative shutdown" : request.reason;
      break;
    }
    default:
      response.code = ErrorCode::InvalidRequest;
      response.message = "unsupported command";
      break;
  }
  Frame reply;
  reply.type = MessageType::CommandResponse;
  reply.flags = to_flags(FrameFlag::Response);
  reply.coordinator_epoch = fabric->coordinator_epoch();
  reply.payload = pack_payload(response, limits);
  return session.send(reply);
}

Status PartitionCoordinator::Impl::handle_session_frame(Session& session, const Frame& frame) {
  switch (frame.type) {
    case MessageType::Hello: {
      Result<HelloMessage> hello =
          unpack_payload<HelloMessage>(frame.payload.data(), frame.payload.size(), limits);
      if (!hello.ok()) {
        return hello.error();
      }
      if (hello.value().protocol_version != APF_PROTOCOL_VERSION) {
        return failure(ErrorCode::ProtocolViolation, "worker protocol version is not supported",
                       std::to_string(hello.value().protocol_version));
      }
      HelloAckMessage ack;
      ack.epoch = fabric->coordinator_epoch();
      ack.limits = limits;
      ack.session_nonce = hello.value().worker_boot.value();
      if (hello.value().known_epoch.known() && hello.value().known_epoch > ack.epoch) {
        ack.accepted = false;
        ack.reason = "worker believes it belongs to a newer coordinator epoch";
      } else {
        ack.accepted = true;
        if (hello.value().worker.valid()) {
          ack.assigned_worker = hello.value().worker;
        } else {
          // A worker that has no identity yet is assigned one by the
          // coordinator, which owns identity allocation.
          Result<WorkerId> allocated = fabric->allocate_worker_id();
          if (!allocated.ok()) {
            ack.accepted = false;
            ack.reason = to_string(allocated.error());
          } else {
            ack.assigned_worker = allocated.value();
          }
        }
      }
      Frame response;
      response.type = MessageType::HelloAck;
      response.flags = to_flags(FrameFlag::Response);
      response.coordinator_epoch = ack.epoch;
      response.payload = pack_payload(ack, limits);
      return session.send(response);
    }
    case MessageType::RegisterWorker: {
      Result<RegisterWorkerMessage> message = unpack_payload<RegisterWorkerMessage>(
          frame.payload.data(), frame.payload.size(), limits);
      if (!message.ok()) {
        return message.error();
      }
      return handle_register(session, frame, message.value());
    }
    case MessageType::EvidencePublish: {
      Result<EvidencePublishMessage> message = unpack_payload<EvidencePublishMessage>(
          frame.payload.data(), frame.payload.size(), limits);
      if (!message.ok()) {
        return message.error();
      }
      return handle_evidence(session, frame, message.value());
    }
    case MessageType::MutationResult:
      return handle_mutation_result(frame);
    case MessageType::QueryResponse: {
      Result<QueryResponseMessage> response = unpack_payload<QueryResponseMessage>(
          frame.payload.data(), frame.payload.size(), limits);
      if (!response.ok()) {
        return response.error();
      }
      {
        std::lock_guard<std::mutex> lock(dispatch_mutex);
        const auto it = pending_queries.find(frame.sequence);
        if (it != pending_queries.end() && !it->second->settled) {
          it->second->settled = true;
          it->second->ok = true;
          it->second->response = response.value();
        }
      }
      query_cv.notify_all();
      return success();
    }
    case MessageType::QueryRequest: {
      Result<QueryRequestMessage> request = unpack_payload<QueryRequestMessage>(
          frame.payload.data(), frame.payload.size(), limits);
      if (!request.ok()) {
        return request.error();
      }
      return handle_query(session, frame, request.value());
    }
    case MessageType::CommandRequest: {
      Result<CommandRequestMessage> request = unpack_payload<CommandRequestMessage>(
          frame.payload.data(), frame.payload.size(), limits);
      if (!request.ok()) {
        return request.error();
      }
      const Status handled = handle_command(session, frame, request.value());
      if (handled.ok() && pending_shutdown) {
        const std::string reason = pending_shutdown_reason;
        pending_shutdown = false;
        owner->request_shutdown(reason);
      }
      return handled;
    }
    case MessageType::Heartbeat: {
      Result<HeartbeatMessage> heartbeat = unpack_payload<HeartbeatMessage>(
          frame.payload.data(), frame.payload.size(), limits);
      if (!heartbeat.ok()) {
        return heartbeat.error();
      }
      if (session.worker_session) {
        (void)fabric->note_worker_seen(session.worker, session.worker_boot, heartbeat.value().sent_at_ms);
        HeartbeatAckMessage ack;
        ack.epoch = fabric->coordinator_epoch();
        ack.received_at_ms = heartbeat.value().sent_at_ms;
        Frame response;
        response.type = MessageType::HeartbeatAck;
        response.flags = to_flags(FrameFlag::Response);
        response.coordinator_epoch = ack.epoch;
        response.worker = session.worker;
        response.worker_boot = session.worker_boot;
        response.payload = pack_payload(ack, limits);
        return session.send(response);
      }
      return success();
    }
    case MessageType::FenceAck: {
      Result<FenceAckMessage> ack =
          unpack_payload<FenceAckMessage>(frame.payload.data(), frame.payload.size(), limits);
      if (!ack.ok()) {
        return ack.error();
      }
      return success();
    }
    case MessageType::ErrorResponse: {
      // A peer reported a failure that is already accounted for by whichever
      // request produced it; the session stays open.
      return success();
    }
    case MessageType::Shutdown:
      session.closed.store(true);
      return success();
    case MessageType::HelloAck:
    case MessageType::RegisterAck:
    case MessageType::EvidenceAck:
    case MessageType::MutationRequest:
    case MessageType::FenceWorker:
    case MessageType::HeartbeatAck:
    case MessageType::ReconcileRequest:
    case MessageType::ReconcileResponse:
    case MessageType::AdminRequest:
    case MessageType::AdminResponse:
    case MessageType::CommandResponse:
      return failure(ErrorCode::ProtocolViolation,
                     "the coordinator does not accept this message type", to_string(frame.type));
    case MessageType::Invalid:
    case MessageType::Count:
      return failure(ErrorCode::ProtocolViolation, "invalid message type");
  }
  return failure(ErrorCode::ProtocolViolation, "unhandled message type");
}

Status PartitionCoordinator::fence_worker(const WorkerId& worker, const WorkerBootId& boot,
                                          std::string reason) {
  const std::shared_ptr<Session> session = impl_->find_session(worker, boot);
  if (session != nullptr) {
    FenceMessage fence;
    fence.worker = worker;
    fence.worker_boot = boot;
    fence.epoch = impl_->fabric->coordinator_epoch();
    fence.reason = reason;
    Frame frame;
    frame.type = MessageType::FenceWorker;
    frame.flags = to_flags(FrameFlag::Request);
    frame.coordinator_epoch = fence.epoch;
    frame.worker = worker;
    frame.worker_boot = boot;
    frame.payload = pack_payload(fence, impl_->limits);
    (void)session->send(frame);
  }
  const Status fenced = impl_->fabric->fence_worker(worker, boot, reason);
  impl_->abandon_worker(worker, boot, "worker incarnation was fenced: " + reason);
  return fenced;
}

}  // namespace apf
