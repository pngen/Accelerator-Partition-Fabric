#include "fabric_internal.hpp"

#include "apf/codec.hpp"
#include "apf/process.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace apf {

// Defined below with the capability publication logic; declared here because
// accelerator registration publishes capability atomically with identity.
Result<CapabilityGeneration> publish_capability_locked(FabricState& state,
                                                       const AcceleratorId& accelerator_id,
                                                       PartitionCapability capability,
                                                       std::uint64_t now_ms,
                                                       bool force_new_generation);

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

void record_event(FabricState& state, ExplanationCode code, std::string subject,
                  std::string detail) {
  FabricEvent event;
  event.code = code;
  event.subject = std::move(subject);
  event.detail = std::move(detail);
  event.at_ms = state.clock ? state.clock->now_ms() : system_now_ms();
  event.sequence = ++state.event_sequence;
  state.events.push_back(std::move(event));
  while (state.events.size() > state.limits.max_events) {
    state.events.pop_front();
  }
}

void bump_state_generation(FabricState& state) {
  const Result<StateGeneration> next = state.state_generation.next();
  if (next.ok()) {
    state.state_generation = next.value();
  }
}

/// True for states that deliberately preserve whichever bucket already holds
/// the partition's capacity. This is what makes restart and ambiguity handling
/// conservative: capacity is not handed back until the runtime can prove the
/// partition is gone.
bool state_keeps_current_bucket(PartitionState state) {
  switch (state) {
    case PartitionState::RevalidationRequired:
    case PartitionState::Offline:
    case PartitionState::Destroying:
      return true;
    default:
      return false;
  }
}

std::optional<CapacityBucket> bucket_for_state(PartitionState state) {
  switch (state) {
    case PartitionState::Reserved:
    case PartitionState::Creating:
      return CapacityBucket::Reserved;
    case PartitionState::Active:
    case PartitionState::Degraded:
      return CapacityBucket::Active;
    case PartitionState::Draining:
      return CapacityBucket::Draining;
    case PartitionState::ReconfigurationRequired:
      return CapacityBucket::ReconfigurationHeld;
    default:
      return std::nullopt;
  }
}

Status apply_capacity_transition(CapacityLedger& ledger, PartitionRecord& record,
                                 PartitionState target_state) {
  if (state_keeps_current_bucket(target_state)) {
    return success();
  }
  const std::optional<CapacityBucket> target = bucket_for_state(target_state);
  if (target == record.held_bucket) {
    return success();
  }
  if (target.has_value() && !record.held_bucket.has_value()) {
    // A record whose capacity the ledger does not currently attribute — an
    // observed partition recorded during reconciliation — takes its capacity
    // from free when it becomes authoritative again. If free capacity is not
    // available the transition fails instead of inventing capacity.
    const Status status = ledger.move_between(CapacityBucket::Free, *target, record.resources);
    if (!status.ok()) {
      return failure(ErrorCode::InsufficientCapacity,
                     "capacity cannot be accounted for this partition", record.id.str());
    }
    record.held_bucket = target;
    return success();
  }
  if (!target.has_value()) {
    if (!record.held_bucket.has_value()) {
      return success();
    }
    const Status status =
        ledger.move_between(*record.held_bucket, CapacityBucket::Free, record.resources);
    if (!status.ok()) {
      return status;
    }
    record.held_bucket.reset();
    return success();
  }
  const Status status = ledger.move_between(*record.held_bucket, *target, record.resources);
  if (!status.ok()) {
    return status;
  }
  record.held_bucket = target;
  return success();
}

Status transition_partition(FabricState& state, PartitionRecord& record, PartitionState to,
                            std::string reason) {
  const PartitionState from = record.state;
  const Status allowed = validate_transition(from, to);
  if (!allowed.ok()) {
    return failure(allowed.error().code, allowed.error().message,
                   std::string(to_string(from)) + " -> " + to_string(to) + " (" +
                       record.id.str() + ")");
  }
  if (from == to) {
    return success();
  }
  CapacityLedger* ledger = state.find_ledger(record.accelerator);
  if (ledger == nullptr) {
    return failure(ErrorCode::CorruptState, "partition has no capacity ledger", record.id.str());
  }
  const Status capacity = apply_capacity_transition(*ledger, record, to);
  if (!capacity.ok()) {
    return capacity;
  }
  const std::uint64_t now = state.clock ? state.clock->now_ms() : system_now_ms();
  record.state = to;
  record.last_transition_at_ms = now;
  record.last_transition_reason = std::move(reason);
  const Status validation = record.validate();
  if (!validation.ok()) {
    return validation;
  }
  const Status accounting = ledger->validate();
  if (!accounting.ok()) {
    return failure(ErrorCode::CorruptState, "accounting does not close after a transition",
                   record.id.str() + ": " + to_string(accounting.error()));
  }
  return success();
}

Status validate_plan_binding(const FabricState& state, const PartitionPlan& plan,
                             std::uint64_t now_ms, bool require_fresh_evidence) {
  if (plan.binding.policy_generation != state.policy_generation) {
    return failure(ErrorCode::PolicyChanged, "policy generation changed after the plan was built");
  }
  if (plan.terminal) {
    return failure(ErrorCode::StalePlan, "plan has already been discarded or consumed");
  }
  if (plan.binding.expired_at(now_ms)) {
    return failure(ErrorCode::StalePlan, "plan validity window has expired",
                   std::to_string(plan.binding.valid_until_ms));
  }
  if (!plan.binding.accelerator.valid()) {
    return failure(ErrorCode::StalePlan, "plan does not bind an accelerator");
  }
  const AcceleratorRecord* accelerator = state.find_accelerator(plan.binding.accelerator);
  if (accelerator == nullptr) {
    return failure(ErrorCode::StalePlan, "planned accelerator is no longer registered");
  }
  if (accelerator->generation != plan.binding.accelerator_generation) {
    return failure(ErrorCode::StaleGeneration, "device generation changed after the plan was built");
  }
  if (accelerator->boot_id != plan.binding.accelerator_boot) {
    return failure(ErrorCode::StaleGeneration,
                   "device incarnation changed after the plan was built");
  }
  if (accelerator->capability.generation != plan.binding.capability_generation) {
    return failure(ErrorCode::CapabilityChanged,
                   "capability generation changed after the plan was built");
  }
  if (require_fresh_evidence && !accelerator->evidence.is_fresh_at(now_ms)) {
    return failure(ErrorCode::StaleEvidence, "device evidence is no longer fresh");
  }
  // Layout binding: any partition on the device that appeared, disappeared, or
  // changed generation invalidates the plan.
  std::size_t seen = 0;
  for (const auto& entry : state.partitions) {
    const PartitionRecord& record = entry.second;
    if (record.accelerator != plan.binding.accelerator) {
      continue;
    }
    if (is_terminal_state(record.state) || record.state == PartitionState::Unpartitioned) {
      continue;
    }
    bool matched = false;
    for (const auto& bound : plan.binding.layout) {
      if (bound.first == record.id && bound.second == record.generation) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return failure(ErrorCode::StalePlan,
                     "device layout contains a partition the plan did not bind",
                     record.id.str() + " generation=" +
                         std::to_string(record.generation.value()));
    }
    ++seen;
  }
  if (seen != plan.binding.layout.size()) {
    return failure(ErrorCode::StalePlan,
                   "a partition the plan bound is no longer present on the device",
                   std::to_string(plan.binding.layout.size() - seen) + " missing");
  }
  return success();
}

std::shared_ptr<const FabricSnapshot> build_snapshot(const FabricState& state,
                                                     std::uint64_t now_ms) {
  auto snapshot = std::make_shared<FabricSnapshot>();
  snapshot->generation = state.snapshot_generation;
  snapshot->state_generation = state.state_generation;
  snapshot->coordinator_epoch = state.coordinator_epoch;
  snapshot->created_at_ms = now_ms;
  snapshot->closed = state.closed;
  snapshot->instance_id = state.instance_id;
  snapshot->policy = state.policy;
  for (const auto& entry : state.accelerators) {
    AcceleratorView view;
    view.accelerator = entry.second;
    const auto ledger = state.ledgers.find(entry.first);
    if (ledger != state.ledgers.end()) {
      view.ledger = ledger->second;
    }
    view.freshness = view.accelerator.evidence.freshness_at(now_ms);
    for (const auto& partition_entry : state.partitions) {
      const PartitionRecord& record = partition_entry.second;
      if (record.accelerator != entry.second.id) {
        continue;
      }
      view.partitions.push_back(record.id);
      if (record.state == PartitionState::Active) {
        ++view.active_partitions;
      } else if (record.state == PartitionState::Draining) {
        ++view.draining_partitions;
      }
      if (record.evidence.freshness_at(now_ms) != EvidenceFreshness::Fresh) {
        ++view.stale_partitions;
      }
    }
    snapshot->accelerators.push_back(std::move(view));
  }
  for (const auto& entry : state.profiles) {
    snapshot->profiles.push_back(entry.second);
  }
  for (const auto& entry : state.partitions) {
    snapshot->partitions.push_back(entry.second);
  }
  for (const auto& entry : state.reservations) {
    snapshot->reservations.push_back(entry.second);
  }
  for (const auto& entry : state.attempts) {
    snapshot->attempts.push_back(entry.second);
  }
  for (const auto& entry : state.assignments) {
    snapshot->assignments.push_back(entry.second);
  }
  for (const auto& entry : state.workers) {
    snapshot->workers.push_back(entry.second);
  }
  return snapshot;
}

std::uint64_t snapshot_digest(const FabricSnapshot& snapshot) noexcept {
  const std::string rendered = snapshot.render_logical();
  std::uint64_t hash = 1469598103934665603ull;
  for (const char ch : rendered) {
    hash ^= static_cast<std::uint8_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

namespace {

Status check_open(const FabricState& state) {
  if (state.closed) {
    return failure(ErrorCode::Closed, "runtime instance is closed");
  }
  return success();
}

void ensure_evidence(EvidenceStamp& stamp, EvidenceProvenance provenance, const std::string& source,
                     std::uint64_t ttl_ms, std::uint64_t now_ms, EvidenceGeneration generation) {
  if (stamp.is_observed()) {
    if (stamp.ttl_ms == 0) {
      stamp.ttl_ms = ttl_ms;
    }
    return;
  }
  stamp.observed = true;
  stamp.generation = generation;
  stamp.observed_at_ms = now_ms;
  stamp.ttl_ms = ttl_ms;
  stamp.source = source;
  if (stamp.provenance == EvidenceProvenance::Unknown) {
    stamp.provenance = provenance;
  }
}

void prune_plans(FabricState& state) {
  if (state.plans.size() <= state.limits.max_pending_plans) {
    return;
  }
  for (auto it = state.plans.begin(); it != state.plans.end() &&
                                      state.plans.size() > state.limits.max_pending_plans;) {
    if (it->second.terminal) {
      it = state.plans.erase(it);
    } else {
      ++it;
    }
  }
}

void prune_attempts(FabricState& state) {
  if (state.attempts.size() <= state.limits.max_attempt_history) {
    return;
  }
  for (auto it = state.attempts.begin();
       it != state.attempts.end() && state.attempts.size() > state.limits.max_attempt_history;) {
    if (attempt_settled(it->second.state)) {
      it = state.attempts.erase(it);
    } else {
      ++it;
    }
  }
}

bool is_worker_bound(const PartitionReservation& reservation, const WorkerId& worker,
                     const WorkerBootId& boot) {
  return reservation.worker == worker && reservation.worker_boot == boot;
}

}  // namespace

struct PartitionFabric::Impl {
  mutable std::mutex mutex;
  FabricState state;
  std::shared_ptr<AcceleratorBackend> local_backend;
  std::vector<std::shared_ptr<const FabricSnapshot>> snapshots;
  std::size_t snapshot_retention{8};
  bool require_worker_for_mutation{false};

  std::uint64_t now() const {
    return state.clock ? state.clock->now_ms() : system_now_ms();
  }
};

// ---------------------------------------------------------------------------
// Construction and lifecycle
// ---------------------------------------------------------------------------

PartitionFabric::PartitionFabric(FabricOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->state.limits = options.limits;
  impl_->state.clock = options.clock ? options.clock : make_system_clock();
  impl_->state.policy = make_default_policy(PolicyGeneration::first());
  impl_->state.policy_generation = PolicyGeneration::first();
  impl_->state.coordinator_epoch =
      options.coordinator_epoch.known() ? options.coordinator_epoch : CoordinatorEpoch::first();
  impl_->state.instance_id = options.instance_id.empty() ? "apf-instance" : options.instance_id;
  impl_->state.opened_at_ms = impl_->now();
  impl_->local_backend = options.local_backend;
  impl_->require_worker_for_mutation = options.require_worker_for_mutation;
  impl_->snapshot_retention =
      options.snapshot_retention == 0 ? 1
                                      : std::min(options.snapshot_retention,
                                                 options.limits.max_snapshot_retention);
}

PartitionFabric::~PartitionFabric() = default;

const Limits& PartitionFabric::limits() const noexcept { return impl_->state.limits; }

const std::string& PartitionFabric::instance_id() const noexcept { return impl_->state.instance_id; }

CoordinatorEpoch PartitionFabric::coordinator_epoch() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.coordinator_epoch;
}

StateGeneration PartitionFabric::state_generation() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.state_generation;
}

bool PartitionFabric::closed() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.closed;
}

Status PartitionFabric::close() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  if (state.closed) {
    return success();
  }
  const std::uint64_t now = impl_->now();
  // Pending reservations release exactly once; committed partitions keep their
  // authoritative capacity.
  for (auto& entry : state.reservations) {
    PartitionReservation& reservation = entry.second;
    if (!reservation.holds_capacity()) {
      continue;
    }
    CapacityLedger* ledger = state.find_ledger(reservation.accelerator);
    if (ledger != nullptr) {
      const Status released = ledger->release_reserved(reservation.resources);
      if (!released.ok()) {
        record_event(state, ExplanationCode::AccountingViolation, reservation.id.str(),
                     to_string(released.error()));
      }
    }
    reservation.lifecycle = ReservationLifecycle::Released;
    reservation.reason = "runtime closed before the reservation was committed";
    reservation.updated_at_ms = now;
  }
  for (auto& entry : state.partitions) {
    PartitionRecord& record = entry.second;
    if (record.state == PartitionState::Reserved || record.state == PartitionState::Creating ||
        record.state == PartitionState::PlanPending) {
      if (record.held_bucket.has_value()) {
        CapacityLedger* ledger = state.find_ledger(record.accelerator);
        if (ledger != nullptr) {
          (void)ledger->move_between(*record.held_bucket, CapacityBucket::Free, record.resources);
        }
        record.held_bucket.reset();
      }
      supersede_partition_generation(state, record);
      record.state = PartitionState::Retired;
      record.last_transition_at_ms = now;
      record.last_transition_reason = "runtime closed before creation was verified";
    }
  }
  // Pending physical attempts are classified, never asserted as cancelled.
  for (auto& entry : state.attempts) {
    PartitionAttempt& attempt = entry.second;
    if (attempt.state == AttemptState::Registered || attempt.state == AttemptState::Dispatched) {
      attempt.state = AttemptState::OutcomeUnknown;
      attempt.settled_at_ms = now;
      attempt.detail = "runtime closed while the attempt was in flight";
      record_event(state, ExplanationCode::OutcomeUnknown, attempt.id.str(),
                   "attempt classified as outcome unknown at shutdown");
    }
  }
  for (auto& entry : state.workers) {
    WorkerRecord& worker = entry.second;
    worker.alive = false;
  }
  bool accounting_closed = true;
  std::string violation;
  for (const auto& entry : state.ledgers) {
    const Status valid = entry.second.validate();
    if (!valid.ok()) {
      accounting_closed = false;
      violation = to_string(valid.error());
      break;
    }
  }
  state.closed = true;
  state.closed_at_ms = now;
  bump_state_generation(state);
  record_event(state, ExplanationCode::Shutdown, state.instance_id,
               "runtime closed and accounting returned to a valid baseline");
  if (!accounting_closed) {
    return failure(ErrorCode::CorruptState,
                   "capacity accounting does not close after shutdown", violation);
  }
  return success();
}

Status PartitionFabric::reopen() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->state.closed = false;
  impl_->state.opened_at_ms = impl_->now();
  bump_state_generation(impl_->state);
  record_event(impl_->state, ExplanationCode::Shutdown, impl_->state.instance_id,
               "runtime reopened for a new operating session");
  return success();
}

Status PartitionFabric::verify_accounting() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const auto& entry : impl_->state.ledgers) {
    const Status status = entry.second.validate();
    if (!status.ok()) {
      return failure(ErrorCode::CorruptState,
                     "capacity accounting does not close for accelerator " +
                         std::to_string(entry.first),
                     to_string(status.error()));
    }
  }
  return success();
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

Status PartitionFabric::set_policy(const PlanningPolicy& policy) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Status valid = policy.validate();
  if (!valid.ok()) {
    return valid;
  }
  FabricState& state = impl_->state;
  const Result<PolicyGeneration> next = state.policy_generation.next();
  if (!next.ok()) {
    return next.error();
  }
  state.policy = policy;
  state.policy_generation = next.value();
  state.policy.generation = next.value();
  // Plans built under the previous policy are invalidated; active reservations
  // keep their capacity but must be revalidated before they mutate hardware.
  for (auto& entry : state.plans) {
    if (entry.second.binding.policy_generation != state.policy_generation) {
      entry.second.terminal = true;
      entry.second.explanation.add(ExplanationCode::PolicyGenerationChanged, entry.second.id.str(),
                                   "policy changed after this plan was built");
    }
  }
  bump_state_generation(state);
  record_event(state, ExplanationCode::PolicyGenerationChanged, "policy",
               state.policy.name + " generation=" + std::to_string(state.policy_generation.value()));
  return success();
}

PlanningPolicy PartitionFabric::policy() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.policy;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

Status PartitionFabric::register_profile(const PartitionProfile& profile) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Status status = check_open(state);
  if (!status.ok()) {
    return status;
  }
  const Status valid = profile.validate(state.limits);
  if (!valid.ok()) {
    return valid;
  }
  const auto existing = state.profiles.find(profile.id.value());
  if (existing != state.profiles.end() &&
      existing->second.generation.value() >= profile.generation.value() &&
      existing->second.name != profile.name) {
    return failure(ErrorCode::GenerationRegression,
                   "profile republication would regress the profile generation", profile.name);
  }
  if (state.profiles.size() >= state.limits.max_profiles && existing == state.profiles.end()) {
    return failure(ErrorCode::LimitExceeded, "profile registry is full");
  }
  state.profiles[profile.id.value()] = profile;
  for (const PartitionProfileId excluded : profile.mutually_exclusive_with) {
    const auto other = state.profiles.find(excluded.value());
    if (other != state.profiles.end()) {
      other->second.mutually_exclusive_with.push_back(profile.id);
    }
  }
  bump_state_generation(state);
  record_event(state, ExplanationCode::CapabilityGenerationChanged, profile.name,
               "profile registered for backend " + profile.backend);
  return success();
}

Result<AcceleratorId> PartitionFabric::register_backend_accelerator(
    const BackendAccelerator& accelerator) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Status status = check_open(state);
  if (!status.ok()) {
    return status.error();
  }
  const Status valid = validate_backend_accelerator(accelerator, state.limits);
  if (!valid.ok()) {
    return valid.error();
  }
  const std::uint64_t now = impl_->now();

  AcceleratorId id{};
  AcceleratorGeneration generation = AcceleratorGeneration::first();
  const auto known = state.key_index.find(accelerator.stable_key);
  const bool existing = known != state.key_index.end();
  if (existing) {
    id = known->second;
  } else {
    if (state.accelerators.size() >= state.limits.max_accelerators) {
      return make_error(ErrorCode::LimitExceeded, "accelerator registry is full");
    }
    const Result<AcceleratorId> allocated = state.ids.allocate<AcceleratorId>();
    if (!allocated.ok()) {
      return allocated.error();
    }
    id = allocated.value();
    state.key_index[accelerator.stable_key] = id;
    state.stable_keys[id.value()] = accelerator.stable_key;
  }

  bool device_reset = false;
  if (existing) {
    const AcceleratorRecord& previous = state.accelerators[id.value()];
    generation = previous.generation;
    if (previous.boot_id.valid() && accelerator.boot_id.valid() &&
        previous.boot_id != accelerator.boot_id) {
      const Result<AcceleratorGeneration> next = previous.generation.next();
      if (!next.ok()) {
        return next.error();
      }
      generation = next.value();
      device_reset = true;
    }
  }

  AcceleratorRecord record;
  if (existing) {
    record = state.accelerators[id.value()];
  } else {
    record.registered_at_ms = now;
  }
  record.id = id;
  record.generation = generation;
  record.backend = accelerator.backend;
  record.identifiers = accelerator.identifiers;
  record.physical_totals = accelerator.physical_totals;
  record.boot_id = accelerator.boot_id;
  record.locality = accelerator.locality;
  record.topology_generation = accelerator.capability.topology_generation;
  record.provenance = accelerator.provenance;
  record.unsupported_reason = accelerator.unsupported_reason;
  record.updated_at_ms = now;
  const AcceleratorRecord previous_record =
      existing ? state.accelerators[id.value()] : AcceleratorRecord{};
  EvidenceGeneration evidence_generation = EvidenceGeneration::first();
  if (existing && previous_record.evidence.generation.known()) {
    const Result<EvidenceGeneration> next = previous_record.evidence.generation.next();
    if (!next.ok()) {
      return next.error();
    }
    evidence_generation = next.value();
  }
  record.evidence = accelerator.evidence;
  ensure_evidence(record.evidence, accelerator.provenance,
                  accelerator.backend.empty() ? "backend" : accelerator.backend,
                  state.policy.max_evidence_age_ms == 0 ? 30'000 : state.policy.max_evidence_age_ms,
                  now, evidence_generation);
  record.health.state = accelerator.provenance == EvidenceProvenance::Synthetic
                            ? HealthState::Healthy
                            : HealthState::Unknown;
  if (existing) {
    record.health = previous_record.health;
  }

  // Profiles published by the backend are registered before capability is
  // published, because capability references them by identity.
  for (const PartitionProfile& profile : accelerator.profiles) {
    const Status profile_status = profile.validate(state.limits);
    if (!profile_status.ok()) {
      return profile_status.error();
    }
    if (state.profiles.size() >= state.limits.max_profiles &&
        state.profiles.find(profile.id.value()) == state.profiles.end()) {
      return make_error(ErrorCode::LimitExceeded, "profile registry is full");
    }
    state.profiles[profile.id.value()] = profile;
  }

  const bool was_known = existing;
  const AcceleratorGeneration previous_generation = previous_record.generation;
  state.accelerators[id.value()] = record;

  CapacityLedger& ledger = state.ledgers[id.value()];
  if (!was_known || ledger.total() != record.physical_totals) {
    const Status initialized = ledger.initialize_totals(record.physical_totals);
    if (!initialized.ok()) {
      if (!was_known) {
        state.accelerators.erase(id.value());
        state.key_index.erase(accelerator.stable_key);
        state.stable_keys.erase(id.value());
        state.ledgers.erase(id.value());
      } else {
        state.accelerators[id.value()].generation = previous_generation;
      }
      return initialized.error();
    }
  }

  PartitionCapability capability = accelerator.capability;
  capability.accelerator = id;
  capability.device_generation = generation;
  if (!capability.evidence.is_observed()) {
    capability.evidence = record.evidence;
  }
  const Result<CapabilityGeneration> published =
      publish_capability_locked(state, id, capability, now, device_reset);
  if (!published.ok()) {
    return published.error();
  }

  if (device_reset) {
    for (auto& entry : state.partitions) {
      PartitionRecord& partition = entry.second;
      if (partition.accelerator != id || is_terminal_state(partition.state)) {
        continue;
      }
      const Status moved = transition_partition(state, partition,
                                                PartitionState::RevalidationRequired,
                                                "device incarnation changed");
      if (!moved.ok()) {
        record_event(state, ExplanationCode::ReconciledDeviceReset, partition.id.str(),
                     to_string(moved.error()));
      }
    }
    for (auto& entry : state.reservations) {
      PartitionReservation& reservation = entry.second;
      if (reservation.accelerator == id && !reservation_is_terminal(reservation.lifecycle)) {
        reservation.lifecycle = ReservationLifecycle::Fenced;
        reservation.reason = "device incarnation changed";
        reservation.updated_at_ms = now;
      }
    }
    record_event(state, ExplanationCode::ReconciledDeviceReset, id.str(),
                 "device incarnation advanced to " + accelerator.boot_id.hex());
  }
  bump_state_generation(state);
  return id;
}

Result<CapabilityGeneration> publish_capability_locked(FabricState& state,
                                                       const AcceleratorId& accelerator_id,
                                                       PartitionCapability capability,
                                                       std::uint64_t now_ms,
                                                       bool force_new_generation) {
  AcceleratorRecord* record = state.find_accelerator(accelerator_id);
  if (record == nullptr) {
    return make_error(ErrorCode::NotFound, "accelerator is not registered", accelerator_id.str());
  }
  capability.accelerator = accelerator_id;
  if (!capability.device_generation.known()) {
    capability.device_generation = record->generation;
  }
  if (capability.device_generation != record->generation) {
    return make_error(ErrorCode::StaleGeneration,
                      "capability was published against a different device generation",
                      accelerator_id.str());
  }
  const Status valid = capability.validate(state.limits);
  if (!valid.ok()) {
    return valid.error();
  }
  if (!capability.evidence.is_observed()) {
    capability.evidence = record->evidence;
  }
  ensure_evidence(capability.evidence, record->provenance, record->backend,
                  state.policy.max_evidence_age_ms == 0 ? 30'000 : state.policy.max_evidence_age_ms,
                  now_ms, capability.evidence.generation.known()
                              ? capability.evidence.generation
                              : EvidenceGeneration::first());

  const bool changed = force_new_generation ||
                       !record->capability.generation.known() ||
                       capability_digest(record->capability) != capability_digest(capability);
  if (!changed) {
    return record->capability.generation;
  }
  CapabilityGeneration generation = CapabilityGeneration::first();
  if (record->capability.generation.known()) {
    const Result<CapabilityGeneration> next = record->capability.generation.next();
    if (!next.ok()) {
      return next.error();
    }
    generation = next.value();
  }
  capability.generation = generation;
  record->capability = capability;
  record->topology_generation = capability.topology_generation;
  record->updated_at_ms = now_ms;

  // Plans and reservations that depended on the previous capability are fenced.
  for (auto& entry : state.reservations) {
    PartitionReservation& reservation = entry.second;
    if (reservation.accelerator != accelerator_id || reservation_is_terminal(reservation.lifecycle)) {
      continue;
    }
    if (reservation.capability_generation != generation) {
      reservation.lifecycle = ReservationLifecycle::Fenced;
      reservation.reason = "capability generation changed before the reservation was committed";
      reservation.updated_at_ms = now_ms;
      CapacityLedger* ledger = state.find_ledger(accelerator_id);
      bool still_held = false;
      for (const PartitionId partition_id : reservation.partitions) {
        PartitionRecord* partition = state.find_partition(partition_id);
        if (partition != nullptr && partition->held_bucket.has_value()) {
          still_held = true;
        }
      }
      if (ledger != nullptr && !still_held) {
        const Status released = ledger->release_reserved(reservation.resources);
        if (!released.ok()) {
          record_event(state, ExplanationCode::AccountingViolation, reservation.id.str(),
                       to_string(released.error()));
        }
      }
    }
  }
  for (auto& entry : state.partitions) {
    PartitionRecord& partition = entry.second;
    if (partition.accelerator != accelerator_id || is_terminal_state(partition.state)) {
      continue;
    }
    if (!capability.supports_profile(partition.profile) &&
        partition.state != PartitionState::RevalidationRequired) {
      const Status moved = transition_partition(
          state, partition, PartitionState::RevalidationRequired,
          "capability no longer publishes the partition profile");
      if (!moved.ok()) {
        record_event(state, ExplanationCode::RevalidationRequired, partition.id.str(),
                     to_string(moved.error()));
      }
    }
  }
  record_event(state, ExplanationCode::CapabilityGenerationChanged, accelerator_id.str(),
               "capability generation=" + std::to_string(generation.value()) +
                   " support=" + to_string(capability.support));
  bump_state_generation(state);
  return generation;
}

Result<CapabilityGeneration> PartitionFabric::publish_capability(
    const PartitionCapability& capability) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Status status = check_open(impl_->state);
  if (!status.ok()) {
    return status.error();
  }
  return publish_capability_locked(impl_->state, capability.accelerator, capability, impl_->now(),
                                   false);
}

Status PartitionFabric::register_accelerator(const AcceleratorRecord& record) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Status status = check_open(state);
  if (!status.ok()) {
    return status;
  }
  const Status valid = record.validate(state.limits);
  if (!valid.ok()) {
    return valid;
  }
  const auto existing = state.accelerators.find(record.id.value());
  if (existing != state.accelerators.end() &&
      record.generation < existing->second.generation) {
    return failure(ErrorCode::GenerationRegression,
                   "accelerator registration would regress the device generation", record.id.str());
  }
  if (existing == state.accelerators.end() && state.accelerators.size() >= state.limits.max_accelerators) {
    return failure(ErrorCode::LimitExceeded, "accelerator registry is full");
  }
  state.accelerators[record.id.value()] = record;
  CapacityLedger& ledger = state.ledgers[record.id.value()];
  if (ledger.total() != record.physical_totals) {
    const Status initialized = ledger.initialize_totals(record.physical_totals);
    if (!initialized.ok()) {
      if (existing == state.accelerators.end()) {
        state.accelerators.erase(record.id.value());
      }
      return initialized;
    }
  }
  if (record.capability.generation.known()) {
    const Result<CapabilityGeneration> published =
        publish_capability_locked(state, record.id, record.capability, impl_->now(), false);
    if (!published.ok()) {
      return published.error();
    }
  }
  bump_state_generation(state);
  return success();
}

Status PartitionFabric::update_health(const AcceleratorId& accelerator,
                                      const HealthEvidence& health) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  AcceleratorRecord* record = state.find_accelerator(accelerator);
  if (record == nullptr) {
    return failure(ErrorCode::NotFound, "accelerator is not registered", accelerator.str());
  }
  record->health = health;
  if (!record->health.stamp.is_observed() && record->health.state != HealthState::Unknown) {
    record->health.stamp = record->evidence;
    record->health.stamp.source = health.source.empty() ? "health-provider" : health.source;
  }
  bump_state_generation(state);
  record_event(state, ExplanationCode::RankingFactor, accelerator.str(),
               std::string("health=") + to_string(health.state));
  return success();
}

Status PartitionFabric::mark_evidence_stale(const AcceleratorId& accelerator, std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  AcceleratorRecord* record = state.find_accelerator(accelerator);
  if (record == nullptr) {
    return failure(ErrorCode::NotFound, "accelerator is not registered", accelerator.str());
  }
  record->evidence.observed = false;
  record->evidence.ttl_ms = 0;
  record->health.state = HealthState::Unknown;
  record->health.stamp.observed = false;
  for (auto& entry : state.plans) {
    if (entry.second.binding.accelerator == accelerator) {
      entry.second.terminal = true;
    }
  }
  bump_state_generation(state);
  record_event(state, ExplanationCode::RejectedStaleEvidence, accelerator.str(),
               reason.empty() ? "evidence marked stale by the operator" : reason);
  return success();
}

Status PartitionFabric::discover(AcceleratorBackend& backend, std::vector<AcceleratorId>* found) {
  Result<std::vector<BackendAccelerator>> result = backend.discover();
  if (!result.ok()) {
    return failure(ErrorCode::BackendFailure, "backend discovery failed",
                   to_string(result.error()));
  }
  std::vector<AcceleratorId> discovered;
  for (const BackendAccelerator& accelerator : result.value()) {
    const Result<AcceleratorId> registered = register_backend_accelerator(accelerator);
    if (registered.ok()) {
      discovered.push_back(registered.value());
    } else {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      record_event(impl_->state, ExplanationCode::CandidateRejected, accelerator.stable_key,
                   to_string(registered.error()));
    }
  }
  if (found != nullptr) {
    *found = discovered;
  }
  return success();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::shared_ptr<const FabricSnapshot> PartitionFabric::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Result<SnapshotGeneration> next = state.snapshot_generation.next();
  if (next.ok()) {
    state.snapshot_generation = next.value();
  }
  std::shared_ptr<const FabricSnapshot> snapshot = build_snapshot(state, impl_->now());
  impl_->snapshots.push_back(snapshot);
  while (impl_->snapshots.size() > impl_->snapshot_retention) {
    impl_->snapshots.erase(impl_->snapshots.begin());
  }
  return snapshot;
}

Result<AcceleratorRecord> PartitionFabric::accelerator(const AcceleratorId& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const AcceleratorRecord* record = impl_->state.find_accelerator(id);
  if (record == nullptr) {
    return make_error(ErrorCode::NotFound, "accelerator is not registered", id.str());
  }
  return *record;
}

Result<PartitionRecord> PartitionFabric::partition(const PartitionId& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const PartitionRecord* record = impl_->state.find_partition(id);
  if (record == nullptr) {
    return make_error(ErrorCode::NotFound, "partition is not known", id.str());
  }
  return *record;
}

Result<CapacityLedger> PartitionFabric::ledger(const AcceleratorId& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const CapacityLedger* found = impl_->state.find_ledger(id);
  if (found == nullptr) {
    return make_error(ErrorCode::NotFound, "accelerator has no ledger", id.str());
  }
  return *found;
}

Result<PartitionPlan> PartitionFabric::plan_by_id(const PartitionPlanId& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->state.plans.find(id.value());
  if (it == impl_->state.plans.end()) {
    return make_error(ErrorCode::NotFound, "plan is not known", id.str());
  }
  return it->second;
}

Result<PartitionReservation> PartitionFabric::reservation(
    const PartitionReservationId& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const PartitionReservation* found = impl_->state.find_reservation(id);
  if (found == nullptr) {
    return make_error(ErrorCode::NotFound, "reservation is not known", id.str());
  }
  return *found;
}

Result<PartitionAttempt> PartitionFabric::attempt(const PartitionAttemptId& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const PartitionAttempt* found = impl_->state.find_attempt(id);
  if (found == nullptr) {
    return make_error(ErrorCode::NotFound, "attempt is not known", id.str());
  }
  return *found;
}

Result<PartitionAssignment> PartitionFabric::assignment(
    const PartitionAssignmentId& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->state.assignments.find(id.value());
  if (it == impl_->state.assignments.end()) {
    return make_error(ErrorCode::NotFound, "assignment is not known", id.str());
  }
  return it->second;
}

Status PartitionFabric::bind_accelerator_worker(const AcceleratorId& accelerator,
                                               const WorkerId& worker,
                                               const WorkerBootId& worker_boot) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const WorkerRecord* record = state.find_worker(worker);
  if (record == nullptr) {
    return failure(ErrorCode::StaleWorker, "worker is not registered", worker.str());
  }
  if (record->boot != worker_boot) {
    return failure(ErrorCode::StaleWorker, "worker incarnation is not current",
                   worker_boot.hex());
  }
  if (record->fenced) {
    return failure(ErrorCode::Fenced, "worker incarnation is fenced", worker_boot.hex());
  }
  if (state.find_accelerator(accelerator) == nullptr) {
    return failure(ErrorCode::NotFound, "accelerator is not registered", accelerator.str());
  }
  state.accelerator_owner[accelerator.value()] = *record;
  return success();
}

Result<WorkerRecord> PartitionFabric::accelerator_worker(const AcceleratorId& accelerator) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->state.accelerator_owner.find(accelerator.value());
  if (it == impl_->state.accelerator_owner.end()) {
    return make_error(ErrorCode::NotFound,
                      "no worker incarnation has published this accelerator",
                      accelerator.str());
  }
  return it->second;
}

Result<WorkerId> PartitionFabric::allocate_worker_id() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.ids.allocate<WorkerId>();
}

Result<std::string> PartitionFabric::accelerator_key(const AcceleratorId& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->state.stable_keys.find(id.value());
  if (it == impl_->state.stable_keys.end()) {
    return make_error(ErrorCode::NotFound, "accelerator has no backend key", id.str());
  }
  return it->second;
}

std::vector<FabricEvent> PartitionFabric::events(std::size_t max_events) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<FabricEvent> out;
  const std::deque<FabricEvent>& events = impl_->state.events;
  const std::size_t count = std::min(max_events, events.size());
  out.reserve(count);
  for (std::size_t index = events.size() - count; index < events.size(); ++index) {
    out.push_back(events[index]);
  }
  return out;
}

std::vector<AcceleratorId> PartitionFabric::accelerator_ids() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<AcceleratorId> out;
  for (const auto& entry : impl_->state.accelerators) {
    out.push_back(entry.second.id);
  }
  return out;
}

Result<FragmentationReport> PartitionFabric::analyze_fragmentation(
    const AcceleratorId& accelerator_id, const PartitionRequest& request) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const FabricState& state = impl_->state;
  const AcceleratorRecord* record = state.find_accelerator(accelerator_id);
  if (record == nullptr) {
    return make_error(ErrorCode::NotFound, "accelerator is not registered", accelerator_id.str());
  }
  const CapacityLedger* capacity = state.find_ledger(accelerator_id);
  if (capacity == nullptr) {
    return make_error(ErrorCode::NotFound, "accelerator has no capacity ledger",
                      accelerator_id.str());
  }
  return analyze_fragmentation_impl(state, *record, *capacity, request, impl_->now());
}

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

Result<PartitionPlan> PartitionFabric::plan(const PartitionRequest& request) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Status status = check_open(state);
  if (!status.ok()) {
    return status.error();
  }
  const std::uint64_t now = impl_->now();
  const std::uint64_t digest = request_digest(request);
  for (const auto& entry : state.plans) {
    const PartitionPlan& existing = entry.second;
    if (!existing.terminal && existing.request_digest == digest &&
        existing.binding.policy_generation == state.policy_generation) {
      const Status binding = validate_plan_binding(state, existing, now, false);
      if (binding.ok()) {
        record_event(state, ExplanationCode::RejectedDuplicateRequest, existing.id.str(),
                     "identical request is already planned against current state");
        return existing;
      }
    }
  }
  if (state.plans.size() >= state.limits.max_pending_plans) {
    prune_plans(state);
    if (state.plans.size() >= state.limits.max_pending_plans) {
      return make_error(ErrorCode::LimitExceeded, "pending plan registry is full");
    }
  }
  PartitionPlan plan = build_plan(state, request, now);
  const Result<PartitionPlanId> id = state.ids.allocate<PartitionPlanId>();
  if (!id.ok()) {
    return id.error();
  }
  plan.id = id.value();
  plan.generation = PartitionPlanGeneration::first();
  state.plans[plan.id.value()] = plan;
  record_event(state, plan.feasible() ? ExplanationCode::PlanFeasibleNow
                                      : ExplanationCode::PlanNoEligibleAccelerator,
               plan.id.str(), plan.describe());
  return plan;
}

Result<PartitionPlan> PartitionFabric::plan_on(const AcceleratorId& accelerator_id,
                                               const PartitionRequest& request) {
  PartitionRequest scoped = request;
  scoped.selector.allowed = {accelerator_id};
  scoped.selector.excluded.clear();
  Result<PartitionPlan> planned = plan(scoped);
  if (!planned.ok()) {
    return planned;
  }
  if (!planned.value().feasible() && planned.value().binding.accelerator != accelerator_id) {
    return make_error(ErrorCode::NotFound, "plan was not produced for the requested accelerator",
                      accelerator_id.str());
  }
  return planned;
}

Result<PartitionPlan> PartitionFabric::plan_reconfiguration(
    const AcceleratorId& accelerator_id, const std::vector<PlannedPartition>& desired,
    const PartitionRequest& context) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Status status = check_open(state);
  if (!status.ok()) {
    return status.error();
  }
  if (state.plans.size() >= state.limits.max_pending_plans) {
    prune_plans(state);
    if (state.plans.size() >= state.limits.max_pending_plans) {
      return make_error(ErrorCode::LimitExceeded, "pending plan registry is full");
    }
  }
  PartitionPlan plan = build_reconfiguration_plan(state, accelerator_id, desired, context,
                                                  impl_->now());
  const Result<PartitionPlanId> id = state.ids.allocate<PartitionPlanId>();
  if (!id.ok()) {
    return id.error();
  }
  plan.id = id.value();
  plan.generation = PartitionPlanGeneration::first();
  state.plans[plan.id.value()] = plan;
  record_event(state, ExplanationCode::ReconfigurationPlanned, plan.id.str(), plan.describe());
  return plan;
}

Status PartitionFabric::discard_plan(const PartitionPlanId& plan_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->state.plans.find(plan_id.value());
  if (it == impl_->state.plans.end()) {
    return failure(ErrorCode::NotFound, "plan is not known", plan_id.str());
  }
  it->second.terminal = true;
  bump_state_generation(impl_->state);
  return success();
}

// ---------------------------------------------------------------------------
// Mutation plumbing
// ---------------------------------------------------------------------------

namespace {

/// Reservation release and fencing are implemented as state operations so that
/// already-locked callers never re-enter the runtime mutex.
Status release_reservation_locked(FabricState& state, PartitionReservation& reservation,
                                  std::string reason, std::uint64_t now_ms) {
  if (reservation_is_terminal(reservation.lifecycle)) {
    return failure(ErrorCode::StaleReservation, "reservation is already terminal",
                   std::string(reservation.id.str()) + " is " +
                       to_string(reservation.lifecycle));
  }
  CapacityLedger* ledger = state.find_ledger(reservation.accelerator);
  if (ledger == nullptr) {
    return failure(ErrorCode::CorruptState, "reservation has no capacity ledger");
  }
  // A physical attempt that may already have been dispatched must be reconciled
  // before its capacity is handed back: releasing it here would hand out
  // capacity that a real partition may already own.
  for (const PartitionId partition_id : reservation.partitions) {
    const PartitionRecord* record = state.find_partition(partition_id);
    if (record != nullptr && record->held_bucket.has_value() &&
        record->state != PartitionState::Reserved) {
      return failure(ErrorCode::ReconciliationRequired,
                     "reservation cannot be released while a physical attempt is in flight",
                     partition_id.str() + " is " + to_string(record->state));
    }
  }
  const Status released = ledger->release_reserved(reservation.resources);
  if (!released.ok()) {
    return released;
  }
  for (const PartitionId partition_id : reservation.partitions) {
    PartitionRecord* record = state.find_partition(partition_id);
    if (record == nullptr) {
      continue;
    }
    if (record->state == PartitionState::Reserved && record->held_bucket.has_value()) {
      record->held_bucket.reset();
      supersede_partition_generation(state, *record);
      record->state = PartitionState::Retired;
      record->last_transition_at_ms = now_ms;
      record->last_transition_reason = "reservation released before creation";
    }
  }
  reservation.lifecycle = ReservationLifecycle::Released;
  reservation.reason = std::move(reason);
  reservation.updated_at_ms = now_ms;
  bump_state_generation(state);
  record_event(state, ExplanationCode::ReleaseAccepted, reservation.id.str(), reservation.reason);
  return success();
}

Status fence_reservation_locked(FabricState& state, PartitionReservation& reservation,
                                std::string reason, std::uint64_t now_ms) {
  if (reservation_is_terminal(reservation.lifecycle)) {
    return failure(ErrorCode::StaleReservation, "reservation is already terminal",
                   to_string(reservation.lifecycle));
  }
  CapacityLedger* ledger = state.find_ledger(reservation.accelerator);
  bool partitions_hold = false;
  for (const PartitionId partition_id : reservation.partitions) {
    const PartitionRecord* record = state.find_partition(partition_id);
    // Only a partition that never left its reserved state can be given back:
    // anything else may already exist physically, so its capacity stays held
    // until reconciliation proves otherwise.
    if (record != nullptr && record->held_bucket.has_value() &&
        record->state != PartitionState::Reserved) {
      partitions_hold = true;
    }
  }
  if (ledger != nullptr && !partitions_hold) {
    const Status released = ledger->release_reserved(reservation.resources);
    if (!released.ok()) {
      return released;
    }
    for (const PartitionId partition_id : reservation.partitions) {
      PartitionRecord* record = state.find_partition(partition_id);
      if (record == nullptr) {
        continue;
      }
      record->held_bucket.reset();
      supersede_partition_generation(state, *record);
      record->state = PartitionState::Retired;
      record->last_transition_at_ms = now_ms;
      record->last_transition_reason = "reservation fenced: " + reason;
    }
  }
  reservation.lifecycle = ReservationLifecycle::Fenced;
  reservation.reason = std::move(reason);
  reservation.updated_at_ms = now_ms;
  bump_state_generation(state);
  record_event(state, ExplanationCode::StaleReservationRejected, reservation.id.str(),
               reservation.reason);
  return success();
}

/// Fences a worker incarnation: its reservations lose authority and every
/// attempt it owned becomes an explicit unknown outcome.
Status fence_worker_locked(FabricState& state, const WorkerId& worker, const WorkerBootId& boot,
                           std::uint64_t now_ms, std::string reason) {
  WorkerRecord* record = state.find_worker(worker);
  if (record == nullptr) {
    return failure(ErrorCode::NotFound, "worker is not registered", worker.str());
  }
  if (record->boot != boot) {
    record_event(state, ExplanationCode::StaleWorkerRejected, worker.str(),
                 "fence request targets a superseded boot identity");
    return success();
  }
  record->fenced = true;
  record->alive = false;
  record->fenced_at_ms = now_ms;
  record->fence_reason = reason;
  for (auto& entry : state.reservations) {
    PartitionReservation& reservation = entry.second;
    if (reservation_is_terminal(reservation.lifecycle)) {
      continue;
    }
    if (is_worker_bound(reservation, worker, boot)) {
      (void)fence_reservation_locked(state, reservation,
                                     "owning worker incarnation was fenced: " + reason, now_ms);
    }
  }
  for (auto& entry : state.attempts) {
    PartitionAttempt& attempt = entry.second;
    if (attempt_settled(attempt.state)) {
      continue;
    }
    if (attempt.worker == worker && attempt.worker_boot == boot) {
      attempt.state = AttemptState::OutcomeUnknown;
      attempt.settled_at_ms = now_ms;
      attempt.detail = "worker incarnation died or was fenced before acknowledging";
      for (const PartitionId partition_id : attempt.partitions) {
        PartitionRecord* partition = state.find_partition(partition_id);
        if (partition != nullptr && partition->state != PartitionState::RevalidationRequired) {
          (void)transition_partition(state, *partition, PartitionState::RevalidationRequired,
                                     attempt.detail);
        }
      }
      record_event(state, ExplanationCode::OutcomeUnknown, attempt.id.str(), attempt.detail);
    }
  }
  bump_state_generation(state);
  record_event(state, ExplanationCode::WorkerFenced, worker.str(), reason);
  return success();
}

}  // namespace

void supersede_partition_generation(FabricState& state, PartitionRecord& record) {
  if (record.generation.known()) {
    record.superseded_generations.push_back(record.generation);
  }
  const Result<PartitionGeneration> next = record.generation.next();
  if (next.ok()) {
    record.generation = next.value();
  } else {
    record_event(state, ExplanationCode::StalePartitionGenerationRejected, record.id.str(),
                 "partition generation space is exhausted; the identity stays terminal");
  }
}

void stamp_partition_evidence(FabricState& state, PartitionRecord& record,
                              const AcceleratorRecord& accelerator, std::uint64_t now_ms,
                              EvidenceProvenance provenance, const std::string& source) {
  const Result<EvidenceGeneration> next = state.evidence_generation.next();
  if (next.ok()) {
    state.evidence_generation = next.value();
  }
  record.evidence.generation = state.evidence_generation;
  record.evidence.provenance = provenance;
  record.evidence.observed_at_ms = now_ms;
  record.evidence.ttl_ms =
      state.policy.max_evidence_age_ms == 0 ? 30'000 : state.policy.max_evidence_age_ms;
  record.evidence.source = source.empty() ? accelerator.backend : source;
  record.evidence.observed = true;
  record.provenance = provenance;
}

Result<PartitionAttempt> register_attempt_locked(FabricState& state,
                                                 PartitionReservation& reservation,
                                                 AttemptKind kind, const WorkerId& worker,
                                                 const WorkerBootId& worker_boot,
                                                 std::uint64_t now_ms) {
  if (state.attempts.size() >= state.limits.max_attempt_history) {
    prune_attempts(state);
    if (state.attempts.size() >= state.limits.max_attempt_history) {
      return make_error(ErrorCode::LimitExceeded, "physical attempt history is full");
    }
  }
  std::size_t pending = 0;
  for (const auto& entry : state.attempts) {
    if (!attempt_settled(entry.second.state)) {
      ++pending;
    }
  }
  if (pending >= state.limits.max_pending_attempts) {
    return make_error(ErrorCode::LimitExceeded, "too many physical attempts are in flight");
  }
  const Result<PartitionAttemptId> id = state.ids.allocate<PartitionAttemptId>();
  if (!id.ok()) {
    return id.error();
  }
  PartitionAttempt attempt;
  attempt.id = id.value();
  attempt.kind = kind;
  attempt.reservation = reservation.id;
  attempt.accelerator = reservation.accelerator;
  attempt.accelerator_generation = reservation.accelerator_generation;
  attempt.accelerator_boot = reservation.accelerator_boot;
  attempt.capability_generation = reservation.capability_generation;
  attempt.coordinator_epoch = reservation.coordinator_epoch;
  attempt.worker = worker.valid() ? worker : reservation.worker;
  attempt.worker_boot = worker_boot.valid() ? worker_boot : reservation.worker_boot;
  attempt.state = AttemptState::Registered;
  attempt.token = id.value().value();
  attempt.registered_at_ms = now_ms;
  attempt.partitions = reservation.partitions;

  if (kind == AttemptKind::CreatePartition) {
    for (std::size_t index = 0; index < reservation.partitions.size(); ++index) {
      const PartitionId partition_id = reservation.partitions[index];
      PartitionRecord* record = state.find_partition(partition_id);
      if (record == nullptr) {
        return make_error(ErrorCode::StalePartitionGeneration,
                          "reserved partition identity is no longer known", partition_id.str());
      }
      const PartitionGeneration expected = index < reservation.partition_generations.size()
                                               ? reservation.partition_generations[index]
                                               : PartitionGeneration{};
      if (record->generation != expected) {
        return make_error(ErrorCode::StalePartitionGeneration,
                          "reserved partition generation is no longer current", partition_id.str());
      }
      const Status moved = transition_partition(state, *record, PartitionState::Creating,
                                                "physical creation attempt registered");
      if (!moved.ok()) {
        return moved.error();
      }
    }
  } else if (kind == AttemptKind::DestroyPartition) {
    for (const PartitionId partition_id : reservation.partitions) {
      PartitionRecord* record = state.find_partition(partition_id);
      if (record == nullptr) {
        return make_error(ErrorCode::StalePartitionGeneration,
                          "reserved partition identity is no longer known", partition_id.str());
      }
      const Status moved = transition_partition(state, *record, PartitionState::Destroying,
                                                "physical destruction attempt registered");
      if (!moved.ok()) {
        return moved.error();
      }
    }
  }

  const auto plan = state.plans.find(reservation.plan.value());
  if (plan != state.plans.end()) {
    if (kind == AttemptKind::CreatePartition) {
      for (std::size_t index = 0; index < plan->second.planned_partitions.size() &&
                                  index < reservation.partitions.size();
           ++index) {
        attempt.desired.push_back(plan->second.planned_partitions[index]);
      }
    } else if (kind == AttemptKind::ReconfigureLayout) {
      // The target layout of a destructive reconfiguration is the plan itself.
      attempt.desired = plan->second.planned_partitions;
    }
  }
  attempt.state = AttemptState::Dispatched;
  attempt.dispatched_at_ms = now_ms;
  const Status validation = attempt.validate();
  if (!validation.ok()) {
    return validation.error();
  }
  state.attempts[attempt.id.value()] = attempt;
  reservation.attempt = attempt.id;
  reservation.updated_at_ms = now_ms;
  record_event(state, ExplanationCode::AttemptRegistered, attempt.id.str(),
               std::string(to_string(kind)) + " registered before dispatch");
  bump_state_generation(state);
  return attempt;
}

namespace {

const BackendNativePartition* find_native(const BackendLayout& layout,
                                          const std::string& native_id) {
  for (const BackendNativePartition& partition : layout.partitions) {
    if (!native_id.empty() && partition.native_id == native_id) {
      return &partition;
    }
  }
  return nullptr;
}

const BackendNativePartition* find_native_for_spec(const BackendLayout& layout,
                                                   const NativePartitionSpec& spec) {
  for (const BackendNativePartition& partition : layout.partitions) {
    if (!spec.vendor_native_profile.empty() &&
        partition.vendor_native_profile != spec.vendor_native_profile) {
      continue;
    }
    if (!spec.resources.empty() && !(partition.resources == spec.resources)) {
      continue;
    }
    return &partition;
  }
  return nullptr;
}

/// Finds an observed partition matching a desired spec that has not already
/// been claimed in this commit. Two logical partitions must never be bound to
/// the same physical partition identity.
const BackendNativePartition* find_unclaimed_native_for_spec(
    const BackendLayout& layout, const NativePartitionSpec& spec,
    const std::vector<std::string>& claimed) {
  for (const BackendNativePartition& partition : layout.partitions) {
    bool already_claimed = false;
    for (const std::string& native_id : claimed) {
      if (native_id == partition.native_id) {
        already_claimed = true;
        break;
      }
    }
    if (already_claimed) {
      continue;
    }
    if (!spec.vendor_native_profile.empty() &&
        partition.vendor_native_profile != spec.vendor_native_profile) {
      continue;
    }
    if (!spec.resources.empty() && !(partition.resources == spec.resources)) {
      continue;
    }
    return &partition;
  }
  return nullptr;
}

/// Exact multiset match between an observed layout and a desired target layout.
bool layout_matches_exactly(const BackendLayout& layout,
                            const std::vector<NativePartitionSpec>& desired) {
  if (layout.partitions.size() != desired.size()) {
    return false;
  }
  std::vector<bool> used(layout.partitions.size(), false);
  for (const NativePartitionSpec& spec : desired) {
    bool matched = false;
    for (std::size_t index = 0; index < layout.partitions.size(); ++index) {
      if (used[index]) {
        continue;
      }
      const BackendNativePartition& candidate = layout.partitions[index];
      const bool profile_matches = spec.vendor_native_profile.empty() ||
                                   candidate.vendor_native_profile == spec.vendor_native_profile;
      const bool resources_match =
          spec.resources.empty() || (candidate.resources == spec.resources);
      if (profile_matches && resources_match) {
        used[index] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }
  return true;
}

std::vector<NativePartitionSpec> desired_specs_of(const PartitionPlan& plan) {
  std::vector<NativePartitionSpec> desired;
  for (const PlannedPartition& planned : plan.planned_partitions) {
    NativePartitionSpec spec;
    spec.vendor_native_profile = planned.vendor_native_profile;
    spec.profile = planned.profile;
    spec.resources = planned.resources;
    desired.push_back(std::move(spec));
  }
  return desired;
}

}  // namespace


MutationOutcome apply_mutation_result_locked(FabricState& state,
                                             const PartitionAttemptId& attempt_id,
                                             const Result<BackendMutationResult>& mutation,
                                             const BackendLayout* observed, std::uint64_t now_ms) {
  MutationOutcome outcome;
  outcome.attempt = attempt_id;
  PartitionAttempt* attempt = state.find_attempt(attempt_id);
  if (attempt == nullptr) {
    outcome.message = "attempt is not known";
    outcome.explanation.add(ExplanationCode::CommitRejected, attempt_id.str(),
                            "attempt is not known");
    return outcome;
  }
  outcome.reservation = attempt->reservation;
  outcome.accelerator = attempt->accelerator;
  outcome.partitions = attempt->partitions;
  PartitionReservation* reservation = state.find_reservation(attempt->reservation);
  if (reservation == nullptr) {
    outcome.message = "reservation is not known";
    outcome.explanation.add(ExplanationCode::CommitRejected, attempt->reservation.str(),
                            "reservation is not known");
    return outcome;
  }
  outcome.plan = reservation->plan;
  if (attempt_settled(attempt->state)) {
    outcome.state = attempt->state;
    outcome.committed = attempt->state == AttemptState::Succeeded;
    outcome.message = "attempt already settled: " + attempt->detail;
    outcome.explanation.add(ExplanationCode::CommitRejected, attempt_id.str(), outcome.message);
    return outcome;
  }
  if (attempt->state == AttemptState::OutcomeUnknown) {
    outcome.state = AttemptState::OutcomeUnknown;
    outcome.outcome_unknown = true;
    outcome.reconciliation_required = true;
    outcome.message = "attempt outcome is unknown and requires reconciliation";
    outcome.explanation.add(ExplanationCode::OutcomeUnknown, attempt_id.str(), attempt->detail);
    return outcome;
  }

  bool accepted = false;
  bool ambiguous = false;
  std::vector<std::string> created_native_ids;
  std::string detail;
  if (mutation.ok()) {
    accepted = mutation.value().accepted;
    ambiguous = mutation.value().ambiguous;
    created_native_ids = mutation.value().created_native_ids;
    detail = mutation.value().detail;
  } else {
    // A backend call that failed after dispatch may still have applied the
    // change: the runtime never assumes otherwise.
    ambiguous = true;
    detail = to_string(mutation.error());
  }

  AcceleratorRecord* accelerator = state.find_accelerator(attempt->accelerator);
  if (accelerator == nullptr) {
    attempt->state = AttemptState::Failed;
    attempt->settled_at_ms = now_ms;
    attempt->detail = "accelerator disappeared before the result could be applied";
    outcome.state = AttemptState::Failed;
    outcome.message = attempt->detail;
    outcome.explanation.add(ExplanationCode::PhysicalVerificationFailed, attempt_id.str(),
                            attempt->detail);
    return outcome;
  }

  bool verified = false;
  std::string verification_detail;
  if (attempt->kind == AttemptKind::ReconfigureLayout) {
    if (!accepted) {
      verification_detail = "backend rejected the reconfiguration: " + detail;
    } else if (observed == nullptr) {
      verification_detail = "no fresh physical observation is available to verify the result";
      ambiguous = true;
    } else if (!observed->device_present) {
      verification_detail = "device is no longer present after the mutation";
      ambiguous = true;
    } else {
      const auto plan = state.plans.find(reservation->plan.value());
      std::vector<NativePartitionSpec> desired;
      if (plan != state.plans.end()) {
        desired = desired_specs_of(plan->second);
      }
      verified = layout_matches_exactly(*observed, desired);
      verification_detail = verified ? "observed layout matches the planned geometry"
                                     : "observed layout does not match the planned geometry";
      if (!verified) {
        ambiguous = true;
      }
    }
  } else if (attempt->kind == AttemptKind::CreatePartition) {
    if (!accepted) {
      verification_detail = "backend rejected the mutation: " + detail;
    } else if (observed == nullptr) {
      verification_detail = "no fresh physical observation is available to verify the result";
      ambiguous = true;
    } else if (!observed->device_present) {
      verification_detail = "device is no longer present after the mutation";
      ambiguous = true;
    } else {
      bool ids_present = true;
      for (const std::string& native_id : created_native_ids) {
        if (find_native(*observed, native_id) == nullptr) {
          ids_present = false;
          break;
        }
      }
      bool specs_present = true;
      for (const PlannedPartition& planned : attempt->desired) {
        NativePartitionSpec spec;
        spec.vendor_native_profile = planned.vendor_native_profile;
        spec.profile = planned.profile;
        spec.resources = planned.resources;
        if (find_native_for_spec(*observed, spec) == nullptr) {
          specs_present = false;
          break;
        }
      }
      verified = ids_present && specs_present;
      verification_detail = verified
                                ? "every requested partition exists in freshly observed state"
                                : "observed layout does not contain the requested partitions";
      if (!verified) {
        ambiguous = true;
      }
    }
  } else {
    if (!accepted) {
      verification_detail = "backend rejected the destruction: " + detail;
    } else if (observed == nullptr) {
      verification_detail = "no fresh physical observation is available to verify destruction";
      ambiguous = true;
    } else {
      bool all_gone = true;
      for (const PartitionId partition_id : attempt->partitions) {
        const PartitionRecord* record = state.find_partition(partition_id);
        if (record == nullptr) {
          continue;
        }
        if (!record->native_identity.native_id.empty() &&
            find_native(*observed, record->native_identity.native_id) != nullptr) {
          all_gone = false;
          break;
        }
      }
      verified = all_gone;
      verification_detail = verified ? "no destroyed partition remains in observed state"
                                     : "a destroyed partition still exists in observed state";
      if (!verified) {
        ambiguous = true;
      }
    }
  }

  const EvidenceProvenance provenance =
      observed != nullptr ? observed->evidence.provenance : accelerator->provenance;
  const std::string evidence_source =
      observed != nullptr ? observed->evidence.source : accelerator->backend;

  if (verified && attempt->kind == AttemptKind::ReconfigureLayout) {
    // Device-wide reconfiguration: the whole layout is replaced in one
    // transactional accounting step.
    const auto plan = state.plans.find(reservation->plan.value());
    CapacityLedger* ledger = state.find_ledger(attempt->accelerator);
    if (ledger == nullptr || plan == state.plans.end()) {
      attempt->state = AttemptState::Failed;
      attempt->settled_at_ms = now_ms;
      attempt->detail = "reconfiguration cannot be committed without a ledger and a plan";
      outcome.state = attempt->state;
      outcome.message = attempt->detail;
      return outcome;
    }
    CapacityLedger working = *ledger;
    Status transaction = success();
    if (reservation->holds_capacity() && !reservation->resources.empty()) {
      transaction = working.release_reserved(reservation->resources);
    }
    for (const PartitionId partition_id : attempt->partitions) {
      const PartitionRecord* record = state.find_partition(partition_id);
      if (record == nullptr || !record->held_bucket.has_value()) {
        continue;
      }
      const Status released =
          working.move_between(*record->held_bucket, CapacityBucket::Free, record->resources);
      if (!released.ok()) {
        transaction = released;
        break;
      }
    }
    if (transaction.ok()) {
      for (const PlannedPartition& planned : plan->second.planned_partitions) {
        const Status taken =
            working.move_between(CapacityBucket::Free, CapacityBucket::Active, planned.resources);
        if (!taken.ok()) {
          transaction = taken;
          break;
        }
      }
    }
    if (transaction.ok()) {
      transaction = working.validate();
    }
    if (!transaction.ok()) {
      attempt->state = AttemptState::Failed;
      attempt->settled_at_ms = now_ms;
      attempt->detail =
          "reconfiguration accounting did not close: " + to_string(transaction.error());
      outcome.state = attempt->state;
      outcome.message = attempt->detail;
      outcome.explanation.add(ExplanationCode::AccountingViolation, attempt->id.str(),
                              attempt->detail);
      for (const PartitionId partition_id : attempt->partitions) {
        PartitionRecord* record = state.find_partition(partition_id);
        if (record != nullptr) {
          (void)transition_partition(state, *record, PartitionState::RevalidationRequired,
                                     "reconfiguration accounting did not close");
        }
      }
      bump_state_generation(state);
      return outcome;
    }

    *ledger = working;
    // The outcome reports the partitions this mutation produced, not the set it
    // replaced.
    outcome.partitions.clear();
    outcome.generations.clear();
    std::vector<PartitionGeneration> fenced;
    for (const PartitionId partition_id : attempt->partitions) {
      PartitionRecord* record = state.find_partition(partition_id);
      if (record == nullptr) {
        continue;
      }
      fenced.push_back(record->generation);
      supersede_partition_generation(state, *record);
      record->held_bucket.reset();
      record->state = PartitionState::Retired;
      record->last_transition_at_ms = now_ms;
      record->last_transition_reason = "superseded by a verified device reconfiguration";
      record->assignment_state = AssignmentState::Unassigned;
      record->drain.state = DrainState::NotDraining;
      record->drain.outstanding_assignments = 0;
    }
    std::vector<PartitionAssignmentId> orphaned;
    for (const auto& entry : state.assignments) {
      for (const PartitionId partition_id : attempt->partitions) {
        if (entry.second.partition == partition_id) {
          orphaned.push_back(entry.second.id);
          break;
        }
      }
    }
    for (const PartitionAssignmentId& assignment_id : orphaned) {
      state.assignments.erase(assignment_id.value());
      record_event(state, ExplanationCode::ReconciledAssignmentOrphaned, assignment_id.str(),
                   "assignment removed because its partition generation was superseded");
    }

    std::vector<std::string> claimed_native_ids;
    for (const PlannedPartition& planned : plan->second.planned_partitions) {
      const PartitionProfile* profile = state.find_profile(planned.profile);
      if (profile == nullptr) {
        continue;
      }
      const Result<PartitionId> partition_id = state.ids.allocate<PartitionId>();
      if (!partition_id.ok()) {
        break;
      }
      PartitionRecord record;
      record.id = partition_id.value();
      record.generation = PartitionGeneration::first();
      record.profile = profile->id;
      record.profile_generation = profile->generation;
      record.mechanism = accelerator->capability.mechanism;
      record.accelerator = accelerator->id;
      record.accelerator_generation = accelerator->generation;
      record.accelerator_boot = accelerator->boot_id;
      record.resources = planned.resources;
      record.state = PartitionState::Active;
      record.held_bucket = CapacityBucket::Active;
      record.assignment_state = AssignmentState::Unassigned;
      record.provenance = provenance;
      record.externally_observed = false;
      record.created_at_ms = now_ms;
      record.last_transition_at_ms = now_ms;
      record.last_transition_reason = "created by a verified device reconfiguration";
      NativePartitionSpec spec;
      spec.vendor_native_profile = planned.vendor_native_profile;
      spec.profile = planned.profile;
      spec.resources = planned.resources;
      if (observed != nullptr) {
        if (const BackendNativePartition* native =
                find_unclaimed_native_for_spec(*observed, spec, claimed_native_ids)) {
          claimed_native_ids.push_back(native->native_id);
          record.native_identity.backend = accelerator->backend;
          record.native_identity.native_id = native->native_id;
          record.native_identity.instance_uuid = native->instance_uuid;
          record.native_identity.parent_native_id = observed->stable_key;
          record.isolation.name = native->isolation_domain;
          record.isolation.guarantees = native->isolation;
          record.isolation.backend_defined = !native->isolation_domain.empty();
        }
      }
      stamp_partition_evidence(state, record, *accelerator, now_ms, provenance, evidence_source);
      outcome.partitions.push_back(record.id);
      outcome.generations.push_back(record.generation);
      state.partitions[record.id.value()] = record;
    }
    attempt->state = AttemptState::Succeeded;
    attempt->settled_at_ms = now_ms;
    attempt->detail = verification_detail;
    reservation->lifecycle = ReservationLifecycle::Committed;
    reservation->updated_at_ms = now_ms;
    outcome.state = AttemptState::Succeeded;
    outcome.committed = true;
    outcome.verified_physically = true;
    outcome.message = "device reconfiguration verified and committed";
    outcome.explanation.add(ExplanationCode::ReconfigurationVerified, attempt->id.str(),
                            verification_detail);
    for (const PartitionGeneration generation : fenced) {
      outcome.explanation.add(ExplanationCode::StalePartitionGenerationRejected, attempt->id.str(),
                              "fenced superseded partition generation " +
                                  std::to_string(generation.value()));
    }
    bump_state_generation(state);
    record_event(state, ExplanationCode::ReconfigurationExecuted, attempt->id.str(),
                 outcome.message);
    return outcome;
  }

  if (verified) {
    if (attempt->kind == AttemptKind::CreatePartition) {
      std::vector<std::string> claimed_native_ids;
      for (std::size_t index = 0; index < attempt->partitions.size(); ++index) {
        PartitionRecord* record = state.find_partition(attempt->partitions[index]);
        if (record == nullptr) {
          continue;
        }
        if (index < attempt->desired.size() && observed != nullptr) {
          NativePartitionSpec spec;
          spec.vendor_native_profile = attempt->desired[index].vendor_native_profile;
          spec.profile = attempt->desired[index].profile;
          spec.resources = attempt->desired[index].resources;
          if (const BackendNativePartition* native =
                  find_unclaimed_native_for_spec(*observed, spec, claimed_native_ids)) {
            claimed_native_ids.push_back(native->native_id);
            record->native_identity.backend = accelerator->backend;
            record->native_identity.native_id = native->native_id;
            record->native_identity.instance_uuid = native->instance_uuid;
            record->native_identity.parent_native_id = observed->stable_key;
            record->isolation.name = native->isolation_domain;
            record->isolation.guarantees = native->isolation;
            record->isolation.backend_defined = !native->isolation_domain.empty();
          }
        }
        stamp_partition_evidence(state, *record, *accelerator, now_ms, provenance, evidence_source);
        const Status moved = transition_partition(state, *record, PartitionState::Active,
                                                  "physical creation verified");
        if (!moved.ok()) {
          record_event(state, ExplanationCode::CommitRejected, record->id.str(),
                       to_string(moved.error()));
        }
        record->externally_observed = false;
      }
    } else if (attempt->kind == AttemptKind::DestroyPartition) {
      for (const PartitionId partition_id : attempt->partitions) {
        PartitionRecord* record = state.find_partition(partition_id);
        if (record == nullptr) {
          continue;
        }
        const Status moved = transition_partition(state, *record, PartitionState::Retired,
                                                  "physical destruction verified");
        if (!moved.ok()) {
          record_event(state, ExplanationCode::ReleaseRejected, record->id.str(),
                       to_string(moved.error()));
        } else {
          supersede_partition_generation(state, *record);
        }
      }
    }
    attempt->state = AttemptState::Succeeded;
    attempt->settled_at_ms = now_ms;
    attempt->detail = verification_detail;
    reservation->lifecycle = ReservationLifecycle::Committed;
    reservation->updated_at_ms = now_ms;
    outcome.state = AttemptState::Succeeded;
    outcome.committed = true;
    outcome.verified_physically = true;
    outcome.message = "mutation verified against freshly observed physical state";
    outcome.explanation.add(ExplanationCode::PhysicalVerificationPassed, attempt->id.str(),
                            verification_detail);
    bump_state_generation(state);
    record_event(state, ExplanationCode::CommitAccepted, attempt->id.str(), outcome.message);
    return outcome;
  }

  if (ambiguous) {
    attempt->state = AttemptState::OutcomeUnknown;
    attempt->settled_at_ms = now_ms;
    attempt->detail = verification_detail + " (" + detail + ")";
    if (!reservation_is_terminal(reservation->lifecycle)) {
      reservation->lifecycle = ReservationLifecycle::Fenced;
      reservation->reason = "physical outcome is unknown; reconciliation is required";
      reservation->updated_at_ms = now_ms;
    }
    for (const PartitionId partition_id : attempt->partitions) {
      PartitionRecord* record = state.find_partition(partition_id);
      if (record == nullptr || record->state == PartitionState::RevalidationRequired) {
        continue;
      }
      const Status moved = transition_partition(state, *record,
                                                PartitionState::RevalidationRequired,
                                                "physical outcome unknown");
      if (!moved.ok()) {
        record_event(state, ExplanationCode::OutcomeUnknown, record->id.str(),
                     to_string(moved.error()));
      }
    }
    outcome.state = AttemptState::OutcomeUnknown;
    outcome.outcome_unknown = true;
    outcome.reconciliation_required = true;
    outcome.message = "physical outcome is unknown; the runtime will not replay a destructive or "
                      "non-idempotent mutation without reconciliation";
    outcome.explanation.add(ExplanationCode::OutcomeUnknown, attempt->id.str(), attempt->detail);
    bump_state_generation(state);
    record_event(state, ExplanationCode::OutcomeUnknown, attempt->id.str(), attempt->detail);
    return outcome;
  }

  // Whether the reservation still holds capacity is decided from the lifecycle
  // it had before this attempt settled, not from the failure state itself.
  const bool held_before_failure = reservation->holds_capacity();
  attempt->state = AttemptState::Failed;
  attempt->settled_at_ms = now_ms;
  attempt->detail = verification_detail + " (" + detail + ")";
  reservation->lifecycle = ReservationLifecycle::Failed;
  reservation->reason = attempt->detail;
  reservation->updated_at_ms = now_ms;
  const bool reclaim = attempt->kind == AttemptKind::CreatePartition && held_before_failure;
  if (reclaim) {
    for (const PartitionId partition_id : attempt->partitions) {
      PartitionRecord* record = state.find_partition(partition_id);
      if (record == nullptr) {
        continue;
      }
      (void)transition_partition(state, *record, PartitionState::Reserved,
                                 "mutation failed without effect");
    }
    reservation->lifecycle = ReservationLifecycle::Active;
  }
  if (attempt->kind == AttemptKind::DestroyPartition) {
    for (const PartitionId partition_id : attempt->partitions) {
      PartitionRecord* record = state.find_partition(partition_id);
      if (record == nullptr) {
        continue;
      }
      (void)transition_partition(state, *record, PartitionState::RevalidationRequired,
                                 "destruction did not take effect");
    }
  }
  outcome.state = AttemptState::Failed;
  outcome.message = attempt->detail;
  outcome.explanation.add(ExplanationCode::PhysicalVerificationFailed, attempt->id.str(),
                          attempt->detail);
  bump_state_generation(state);
  record_event(state, ExplanationCode::CommitRejected, attempt->id.str(), attempt->detail);
  return outcome;
}

// ---------------------------------------------------------------------------
// Reservation
// ---------------------------------------------------------------------------

Result<PartitionReservation> PartitionFabric::reserve(const PartitionPlanId& plan_id) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Status status = check_open(state);
  if (!status.ok()) {
    return status.error();
  }
  auto plan_it = state.plans.find(plan_id.value());
  if (plan_it == state.plans.end()) {
    return make_error(ErrorCode::StalePlan, "plan is not known", plan_id.str());
  }
  PartitionPlan& plan = plan_it->second;
  const std::uint64_t now = impl_->now();
  if (!plan.feasible()) {
    return make_error(ErrorCode::PolicyRejected, "plan is not feasible and cannot be reserved",
                      to_string(plan.outcome));
  }
  const Status binding = validate_plan_binding(state, plan, now, state.policy.require_fresh_evidence);
  if (!binding.ok()) {
    plan.terminal = true;
    plan.explanation.add(ExplanationCode::StalePlanRejected, plan_id.str(),
                         to_string(binding.error()));
    record_event(state, ExplanationCode::StalePlanRejected, plan_id.str(),
                 to_string(binding.error()));
    return binding.error();
  }
  if (plan.outcome == PlanOutcome::FeasibleAfterReconfiguration) {
    return make_error(ErrorCode::ReconfigurationRequired,
                      "a destructive reconfiguration plan is committed by execute_reconfiguration",
                      plan_id.str());
  }
  std::size_t active_reservations = 0;
  for (const auto& entry : state.reservations) {
    if (entry.second.holds_capacity()) {
      ++active_reservations;
    }
  }
  if (active_reservations >= state.limits.max_active_reservations) {
    return make_error(ErrorCode::LimitExceeded, "too many reservations hold capacity");
  }
  if (state.reservations.size() >= state.limits.max_reservations) {
    for (auto it = state.reservations.begin();
         it != state.reservations.end() &&
         state.reservations.size() >= state.limits.max_reservations;) {
      if (reservation_is_terminal(it->second.lifecycle)) {
        it = state.reservations.erase(it);
      } else {
        ++it;
      }
    }
    if (state.reservations.size() >= state.limits.max_reservations) {
      return make_error(ErrorCode::LimitExceeded, "reservation registry is full");
    }
  }

  ResourceVector needed;
  for (const PlannedPartition& planned : plan.planned_partitions) {
    if (needed.empty()) {
      needed = planned.resources;
      continue;
    }
    const Status accumulated = needed.add_assign(planned.resources);
    if (!accumulated.ok()) {
      return accumulated.error();
    }
  }
  CapacityLedger& ledger = state.ledgers[plan.binding.accelerator.value()];
  const Status reserved = ledger.reserve(needed);
  if (!reserved.ok()) {
    plan.explanation.add(ExplanationCode::ReserveRejected, plan_id.str(),
                         to_string(reserved.error()));
    record_event(state, ExplanationCode::ReserveRejected, plan_id.str(),
                 to_string(reserved.error()));
    return reserved.error();
  }

  const Result<PartitionReservationId> reservation_id =
      state.ids.allocate<PartitionReservationId>();
  if (!reservation_id.ok()) {
    (void)ledger.release_reserved(needed);
    return reservation_id.error();
  }
  PartitionReservation reservation;
  reservation.id = reservation_id.value();
  reservation.plan = plan.id;
  reservation.plan_generation = plan.generation;
  reservation.accelerator = plan.binding.accelerator;
  reservation.accelerator_generation = plan.binding.accelerator_generation;
  reservation.accelerator_boot = plan.binding.accelerator_boot;
  reservation.capability_generation = plan.binding.capability_generation;
  reservation.topology_generation = plan.binding.topology_generation;
  reservation.policy_generation = state.policy_generation;
  reservation.coordinator_epoch = state.coordinator_epoch;
  reservation.lifecycle = ReservationLifecycle::Active;
  reservation.resources = needed;
  reservation.destructive = false;
  reservation.created_at_ms = now;
  reservation.updated_at_ms = now;
  reservation.expires_at_ms = plan.binding.valid_until_ms;
  reservation.planned_mutations = static_cast<std::uint32_t>(plan.planned_partitions.size());

  bool failed = false;
  Status failure_status = success();
  for (const PlannedPartition& planned : plan.planned_partitions) {
    const Result<PartitionId> partition_id = state.ids.allocate<PartitionId>();
    if (!partition_id.ok()) {
      failed = true;
      failure_status = partition_id.error();
      break;
    }
    const PartitionProfile* profile = state.find_profile(planned.profile);
    PartitionRecord record;
    record.id = partition_id.value();
    record.generation = PartitionGeneration::first();
    record.profile = planned.profile;
    record.profile_generation = planned.profile_generation;
    record.mechanism = profile != nullptr ? profile->mechanism : PartitionMechanism::None;
    record.accelerator = plan.binding.accelerator;
    record.accelerator_generation = plan.binding.accelerator_generation;
    record.accelerator_boot = plan.binding.accelerator_boot;
    record.resources = planned.resources;
    record.state = PartitionState::Reserved;
    record.held_bucket = CapacityBucket::Reserved;
    record.provenance = state.accelerators[plan.binding.accelerator.value()].provenance;
    record.created_at_ms = now;
    record.last_transition_at_ms = now;
    record.last_transition_reason = "capacity reserved against plan " + plan.id.str();
    const Status validation = record.validate();
    if (!validation.ok()) {
      failed = true;
      failure_status = validation;
      break;
    }
    state.partitions[record.id.value()] = record;
    reservation.partitions.push_back(record.id);
    reservation.partition_generations.push_back(record.generation);
  }

  if (failed) {
    for (const PartitionId partition_id : reservation.partitions) {
      state.partitions.erase(partition_id.value());
    }
    (void)ledger.release_reserved(needed);
    return failure_status.error();
  }

  const Status reservation_validation = reservation.validate();
  if (!reservation_validation.ok()) {
    for (const PartitionId partition_id : reservation.partitions) {
      state.partitions.erase(partition_id.value());
    }
    (void)ledger.release_reserved(needed);
    return reservation_validation.error();
  }
  state.reservations[reservation.id.value()] = reservation;
  // The reservation is part of the plan's own transition: rebind the plan to
  // the layout it now owns, so that later validation detects only changes this
  // plan did not cause.
  for (const PartitionId partition_id : reservation.partitions) {
    const PartitionRecord* record = state.find_partition(partition_id);
    if (record != nullptr) {
      plan.binding.layout.emplace_back(record->id, record->generation);
    }
  }
  bump_state_generation(state);
  record_event(state, ExplanationCode::ReserveAccepted, reservation.id.str(),
               "reserved " + needed.format());
  return reservation;
}

Result<PartitionReservation> PartitionFabric::reserve_for_worker(const PartitionPlanId& plan_id,
                                                                 const WorkerId& worker,
                                                                 const WorkerBootId& worker_boot,
                                                                 const CoordinatorEpoch& epoch) {
  {
    // Authority check before any capacity is held: a reservation may only be
    // bound to a worker incarnation that is current.
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const FabricState& state = impl_->state;
    const auto it = state.workers.find(worker.value());
    if (it == state.workers.end()) {
      return make_error(ErrorCode::StaleWorker,
                        "reservation cannot be bound to an unregistered worker", worker.str());
    }
    if (it->second.boot != worker_boot) {
      return make_error(ErrorCode::StaleWorker,
                        "reservation cannot be bound to a superseded worker incarnation",
                        worker_boot.hex());
    }
    if (it->second.fenced) {
      return make_error(ErrorCode::Fenced,
                        "reservation cannot be bound to a fenced worker incarnation",
                        worker_boot.hex());
    }
    if (epoch != state.coordinator_epoch) {
      return make_error(ErrorCode::StaleCoordinatorEpoch,
                        "reservation cannot be bound under a superseded coordinator epoch");
    }
  }
  Result<PartitionReservation> reserved = reserve(plan_id);
  if (!reserved.ok()) {
    return reserved;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  PartitionReservation* reservation = state.find_reservation(reserved.value().id);
  if (reservation == nullptr) {
    return make_error(ErrorCode::NotFound, "reservation disappeared while binding a worker");
  }
  reservation->worker = worker;
  reservation->worker_boot = worker_boot;
  reservation->coordinator_epoch = epoch;
  reservation->updated_at_ms = impl_->now();
  bump_state_generation(state);
  return *reservation;
}

Status PartitionFabric::release_reservation(const PartitionReservationId& reservation_id,
                                            std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  PartitionReservation* reservation = state.find_reservation(reservation_id);
  if (reservation == nullptr) {
    return failure(ErrorCode::NotFound, "reservation is not known", reservation_id.str());
  }
  return release_reservation_locked(state, *reservation, std::move(reason), impl_->now());
}

Status PartitionFabric::fence_reservation(const PartitionReservationId& reservation_id,
                                          std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  PartitionReservation* reservation = state.find_reservation(reservation_id);
  if (reservation == nullptr) {
    return failure(ErrorCode::NotFound, "reservation is not known", reservation_id.str());
  }
  return fence_reservation_locked(state, *reservation, std::move(reason), impl_->now());
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

Result<MutationOutcome> PartitionFabric::execute(const PartitionReservationId& reservation_id,
                                                 AcceleratorBackend* backend) {
  AcceleratorBackend* target = backend != nullptr ? backend : impl_->local_backend.get();
  if (target == nullptr) {
    return make_error(ErrorCode::InvalidArgument,
                      "no backend is available to execute the physical mutation");
  }
  std::unique_lock<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  Status status = check_open(state);
  if (!status.ok()) {
    return status.error();
  }
  PartitionReservation* reservation = state.find_reservation(reservation_id);
  if (reservation == nullptr) {
    return make_error(ErrorCode::NotFound, "reservation is not known", reservation_id.str());
  }
  if (reservation->lifecycle != ReservationLifecycle::Active) {
    return make_error(ErrorCode::StaleReservation, "reservation is not active",
                      std::string(reservation_id.str()) + " is " +
                          to_string(reservation->lifecycle));
  }
  if (impl_->require_worker_for_mutation && !reservation->worker.valid()) {
    return make_error(ErrorCode::StaleWorker,
                      "this deployment requires a worker incarnation to own physical mutation",
                      reservation_id.str());
  }
  const std::uint64_t now = impl_->now();
  auto plan_it = state.plans.find(reservation->plan.value());
  if (plan_it == state.plans.end()) {
    return make_error(ErrorCode::StalePlan, "the plan behind this reservation is gone");
  }
  status = validate_plan_binding(state, plan_it->second, now, state.policy.require_fresh_evidence);
  if (!status.ok()) {
    plan_it->second.terminal = true;
    (void)fence_reservation_locked(state, *reservation, to_string(status.error()), now);
    return status.error();
  }
  AcceleratorRecord* accelerator = state.find_accelerator(reservation->accelerator);
  if (accelerator == nullptr) {
    return make_error(ErrorCode::NotFound, "reserved accelerator is no longer registered");
  }
  if (accelerator->generation != reservation->accelerator_generation ||
      accelerator->boot_id != reservation->accelerator_boot) {
    (void)fence_reservation_locked(state, *reservation,
                                   "device generation changed before execution", now);
    return make_error(ErrorCode::StaleGeneration,
                      "device generation changed before the mutation was dispatched");
  }
  if (accelerator->capability.generation != reservation->capability_generation) {
    (void)fence_reservation_locked(state, *reservation,
                                   "capability generation changed before execution", now);
    return make_error(ErrorCode::CapabilityChanged,
                      "capability generation changed before the mutation was dispatched");
  }
  if (state.policy_generation != reservation->policy_generation) {
    (void)fence_reservation_locked(state, *reservation,
                                   "policy generation changed before execution", now);
    return make_error(ErrorCode::PolicyChanged,
                      "policy generation changed before the mutation was dispatched");
  }
  if (reservation->expires_at_ms != 0 && now > reservation->expires_at_ms) {
    (void)release_reservation_locked(state, *reservation,
                                     "reservation expired before execution", now);
    return make_error(ErrorCode::StaleReservation, "reservation expired before execution");
  }
  if (!accelerator->evidence.is_fresh_at(now) && state.policy.require_fresh_evidence) {
    return make_error(ErrorCode::StaleEvidence,
                      "device evidence is not fresh enough to authorise a physical mutation");
  }

  const Result<PartitionAttempt> registered =
      register_attempt_locked(state, *reservation, AttemptKind::CreatePartition, WorkerId{},
                              WorkerBootId{}, now);
  if (!registered.ok()) {
    return registered.error();
  }
  const PartitionAttempt attempt = registered.value();

  PartitionMutationRequest request;
  request.stable_key = state.stable_keys[accelerator->id.value()];
  request.expected_boot = accelerator->boot_id;
  request.expected_accelerator_generation = accelerator->generation;
  request.expected_capability_generation = accelerator->capability.generation;
  request.coordinator_epoch = state.coordinator_epoch;
  request.worker = attempt.worker;
  request.worker_boot = attempt.worker_boot;
  request.attempt = attempt.id;
  request.idempotency_token = attempt.token;
  request.destructive = false;
  request.full_device_reconfiguration = false;
  const std::vector<PlannedPartition> planned = plan_it->second.planned_partitions;
  const std::vector<PartitionId> partitions = attempt.partitions;
  for (std::size_t index = 0; index < partitions.size() && index < planned.size(); ++index) {
    NativePartitionSpec spec;
    spec.vendor_native_profile = planned[index].vendor_native_profile;
    spec.profile = planned[index].profile;
    spec.resources = planned[index].resources;
    spec.logical_partition = partitions[index];
    request.desired.push_back(std::move(spec));
  }
  const std::string stable_key = request.stable_key;

  lock.unlock();
  Result<BackendMutationResult> mutation = target->create_partitions(request);
  BackendLayout observed;
  bool have_observed = false;
  if (mutation.ok() && mutation.value().accepted) {
    Result<BackendLayout> layout = target->query_layout(stable_key);
    if (layout.ok()) {
      observed = layout.value();
      have_observed = true;
    }
  }
  lock.lock();

  const std::uint64_t settled_at = impl_->now();
  return apply_mutation_result_locked(state, attempt.id, mutation,
                                      have_observed ? &observed : nullptr, settled_at);
}

Result<MutationOutcome> PartitionFabric::apply_mutation_result(
    const PartitionAttemptId& attempt_id, const BackendMutationResult& result,
    const BackendLayout* observed) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Result<BackendMutationResult> mutation = result;
  return apply_mutation_result_locked(state, attempt_id, mutation, observed, impl_->now());
}

Status PartitionFabric::mark_attempt_outcome_unknown(const PartitionAttemptId& attempt_id,
                                                     std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  PartitionAttempt* attempt = state.find_attempt(attempt_id);
  if (attempt == nullptr) {
    return failure(ErrorCode::NotFound, "attempt is not known", attempt_id.str());
  }
  if (attempt_settled(attempt->state)) {
    return success();
  }
  attempt->state = AttemptState::OutcomeUnknown;
  attempt->settled_at_ms = impl_->now();
  attempt->detail = reason.empty() ? "acknowledgement was lost" : reason;
  PartitionReservation* reservation = state.find_reservation(attempt->reservation);
  if (reservation != nullptr && !reservation_is_terminal(reservation->lifecycle)) {
    reservation->lifecycle = ReservationLifecycle::Fenced;
    reservation->reason = attempt->detail;
    reservation->updated_at_ms = impl_->now();
  }
  if (attempt->kind != AttemptKind::DestroyPartition) {
    for (const PartitionId partition_id : attempt->partitions) {
      PartitionRecord* record = state.find_partition(partition_id);
      if (record == nullptr || record->state == PartitionState::RevalidationRequired) {
        continue;
      }
      (void)transition_partition(state, *record, PartitionState::RevalidationRequired,
                                 attempt->detail);
    }
  }
  bump_state_generation(state);
  record_event(state, ExplanationCode::OutcomeUnknown, attempt_id.str(), attempt->detail);
  return success();
}

Result<PartitionAttempt> PartitionFabric::register_attempt(
    const PartitionReservationId& reservation_id, AttemptKind kind, const WorkerId& worker,
    const WorkerBootId& worker_boot) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  PartitionReservation* reservation = state.find_reservation(reservation_id);
  if (reservation == nullptr) {
    return make_error(ErrorCode::NotFound, "reservation is not known", reservation_id.str());
  }
  if (reservation->lifecycle != ReservationLifecycle::Active) {
    return make_error(ErrorCode::StaleReservation, "reservation is not active",
                      to_string(reservation->lifecycle));
  }
  return register_attempt_locked(state, *reservation, kind, worker, worker_boot, impl_->now());
}

// ---------------------------------------------------------------------------
// Destruction and destructive reconfiguration
// ---------------------------------------------------------------------------

namespace {

/// Destruction is only safe when nothing authoritative still occupies the
/// partition. The runtime owns the partition-side drain gate; it never claims
/// to have migrated the workload.
bool destruction_safe(const FabricState& state, const PartitionRecord& record) {
  if (record.drain.destructive_safe()) {
    return true;
  }
  if (record.drain.state == DrainState::Draining ||
      record.drain.state == DrainState::DrainBlocked) {
    return false;
  }
  for (const auto& entry : state.assignments) {
    if (entry.second.partition == record.id) {
      return false;
    }
  }
  return record.assignment_state != AssignmentState::Assigned;
}

/// Allocates the plan and reservation records that give a destruction or a
/// destructive reconfiguration an explicit authority envelope.
Result<PartitionReservation> create_destructive_reservation_locked(
    FabricState& state, const AcceleratorRecord& accelerator,
    const std::vector<PartitionRecord*>& affected, const ResourceVector& hold, bool device_wide,
    std::uint64_t now_ms, PartitionPlanId* plan_out) {
  const Result<PartitionPlanId> plan_id = state.ids.allocate<PartitionPlanId>();
  if (!plan_id.ok()) {
    return plan_id.error();
  }
  const Result<PartitionReservationId> reservation_id =
      state.ids.allocate<PartitionReservationId>();
  if (!reservation_id.ok()) {
    return reservation_id.error();
  }
  PartitionPlan plan;
  plan.id = plan_id.value();
  plan.generation = PartitionPlanGeneration::first();
  plan.outcome = PlanOutcome::FeasibleAfterReconfiguration;
  plan.created_at_ms = now_ms;
  plan.binding.accelerator = accelerator.id;
  plan.binding.accelerator_generation = accelerator.generation;
  plan.binding.accelerator_boot = accelerator.boot_id;
  plan.binding.capability_generation = accelerator.capability.generation;
  plan.binding.topology_generation = accelerator.topology_generation;
  plan.binding.policy_generation = state.policy_generation;
  plan.binding.coordinator_epoch = state.coordinator_epoch;
  plan.binding.state_generation = state.state_generation;
  plan.binding.evidence_generation = accelerator.evidence.generation;
  plan.binding.planned_at_ms = now_ms;
  plan.binding.valid_until_ms =
      state.policy.max_evidence_age_ms == 0 ? 0 : now_ms + state.policy.max_evidence_age_ms;
  for (const PartitionRecord* record : affected) {
    plan.binding.layout.emplace_back(record->id, record->generation);
    PlannedPartition planned;
    planned.profile = record->profile;
    planned.profile_generation = record->profile_generation;
    planned.resources = record->resources;
    planned.ordinal = static_cast<std::uint32_t>(plan.binding.layout.size());
    const PartitionProfile* profile = state.find_profile(record->profile);
    planned.vendor_native_profile = profile != nullptr ? profile->vendor_native : std::string{};
    plan.planned_partitions.push_back(std::move(planned));
  }
  plan.explanation.add(ExplanationCode::ReconfigurationPlanned, accelerator.id.str(),
                       device_wide ? "device-wide destructive reconfiguration"
                                   : "destruction of a single partition");
  plan.explanation.set_summary(device_wide ? "destructive device reconfiguration"
                                           : "partition destruction");
  plan.explanation.canonicalize(state.limits.max_explanations);
  state.plans[plan.id.value()] = plan;

  PartitionReservation reservation;
  reservation.id = reservation_id.value();
  reservation.plan = plan.id;
  reservation.plan_generation = plan.generation;
  reservation.accelerator = accelerator.id;
  reservation.accelerator_generation = accelerator.generation;
  reservation.accelerator_boot = accelerator.boot_id;
  reservation.capability_generation = accelerator.capability.generation;
  reservation.topology_generation = accelerator.topology_generation;
  reservation.policy_generation = state.policy_generation;
  reservation.coordinator_epoch = state.coordinator_epoch;
  reservation.lifecycle = ReservationLifecycle::Active;
  reservation.destructive = true;
  reservation.created_at_ms = now_ms;
  reservation.updated_at_ms = now_ms;
  reservation.expires_at_ms = plan.binding.valid_until_ms;
  for (const PartitionRecord* record : affected) {
    reservation.partitions.push_back(record->id);
    reservation.partition_generations.push_back(record->generation);
  }
  CapacityLedger* ledger = state.find_ledger(accelerator.id);
  if (ledger == nullptr) {
    return make_error(ErrorCode::CorruptState, "accelerator has no capacity ledger");
  }
  if (!hold.empty()) {
    const Status reserved = ledger->reserve(hold);
    if (!reserved.ok()) {
      state.plans.erase(plan.id.value());
      return reserved.error();
    }
    reservation.resources = hold;
  }
  state.reservations[reservation.id.value()] = reservation;
  if (plan_out != nullptr) {
    *plan_out = plan.id;
  }
  bump_state_generation(state);
  return reservation;
}

}  // namespace

Result<MutationOutcome> PartitionFabric::destroy_partition(const PartitionId& partition_id,
                                                           AcceleratorBackend* backend) {
  AcceleratorBackend* target = backend != nullptr ? backend : impl_->local_backend.get();
  if (target == nullptr) {
    return make_error(ErrorCode::InvalidArgument,
                      "no backend is available to execute the physical mutation");
  }
  std::unique_lock<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  Status status = check_open(state);
  if (!status.ok()) {
    return status.error();
  }
  PartitionRecord* record = state.find_partition(partition_id);
  if (record == nullptr) {
    return make_error(ErrorCode::NotFound, "partition is not known", partition_id.str());
  }
  if (is_terminal_state(record->state)) {
    return make_error(ErrorCode::StalePartitionGeneration,
                      "partition generation is already retired",
                      std::string(partition_id.str()) + " is " + to_string(record->state));
  }
  if (record->native_identity.native_id.empty()) {
    return make_error(ErrorCode::RevalidationRequired,
                      "partition has no verified physical identity to destroy", partition_id.str());
  }
  if (!destruction_safe(state, *record)) {
    return make_error(ErrorCode::DrainRequired,
                      "destructive mutation is not safe while the partition is occupied",
                      partition_id.str());
  }
  AcceleratorRecord* accelerator = state.find_accelerator(record->accelerator);
  if (accelerator == nullptr) {
    return make_error(ErrorCode::NotFound, "partition accelerator is not registered");
  }
  if (accelerator->generation != record->accelerator_generation) {
    (void)transition_partition(state, *record, PartitionState::RevalidationRequired,
                               "device generation changed before destruction");
    return make_error(ErrorCode::StaleGeneration,
                      "device generation changed before destruction was dispatched");
  }
  const std::uint64_t now = impl_->now();
  PartitionPlanId plan_id{};
  const Result<PartitionReservation> reservation = create_destructive_reservation_locked(
      state, *accelerator, {record}, ResourceVector{}, false, now, &plan_id);
  if (!reservation.ok()) {
    return reservation.error();
  }
  PartitionReservation* stored = state.find_reservation(reservation.value().id);
  const Result<PartitionAttempt> registered =
      register_attempt_locked(state, *stored, AttemptKind::DestroyPartition, WorkerId{},
                              WorkerBootId{}, now);
  if (!registered.ok()) {
    return registered.error();
  }
  const PartitionAttempt attempt = registered.value();
  const std::string stable_key = state.stable_keys[accelerator->id.value()];
  const std::string native_id = record->native_identity.native_id;

  PartitionMutationRequest request;
  request.stable_key = stable_key;
  request.expected_boot = accelerator->boot_id;
  request.expected_accelerator_generation = accelerator->generation;
  request.expected_capability_generation = accelerator->capability.generation;
  request.coordinator_epoch = state.coordinator_epoch;
  request.worker = attempt.worker;
  request.worker_boot = attempt.worker_boot;
  request.attempt = attempt.id;
  request.idempotency_token = attempt.token;
  request.destructive = true;
  request.full_device_reconfiguration = false;
  request.remove_native_ids.push_back(native_id);

  lock.unlock();
  Result<BackendMutationResult> mutation = target->destroy_partitions(request);
  BackendLayout observed;
  bool have_observed = false;
  if (mutation.ok() && mutation.value().accepted) {
    Result<BackendLayout> layout = target->query_layout(stable_key);
    if (layout.ok()) {
      observed = layout.value();
      have_observed = true;
    }
  }
  lock.lock();
  return apply_mutation_result_locked(state, attempt.id, mutation,
                                      have_observed ? &observed : nullptr, impl_->now());
}

Result<MutationOutcome> PartitionFabric::execute_reconfiguration(const PartitionPlanId& plan_id,
                                                                 AcceleratorBackend* backend) {
  AcceleratorBackend* target = backend != nullptr ? backend : impl_->local_backend.get();
  if (target == nullptr) {
    return make_error(ErrorCode::InvalidArgument,
                      "no backend is available to execute the physical mutation");
  }
  std::unique_lock<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  Status status = check_open(state);
  if (!status.ok()) {
    return status.error();
  }
  const auto plan_it = state.plans.find(plan_id.value());
  if (plan_it == state.plans.end()) {
    return make_error(ErrorCode::StalePlan, "plan is not known", plan_id.str());
  }
  const PartitionPlan& plan = plan_it->second;
  if (plan.outcome != PlanOutcome::FeasibleAfterReconfiguration) {
    return make_error(ErrorCode::InvalidRequest,
                      "plan is not a destructive reconfiguration plan", to_string(plan.outcome));
  }
  const std::uint64_t now = impl_->now();
  // A stale destructive plan is never executed.
  status = validate_plan_binding(state, plan, now, state.policy.require_fresh_evidence);
  if (!status.ok()) {
    plan_it->second.terminal = true;
    record_event(state, ExplanationCode::ReconfigurationStale, plan_id.str(),
                 to_string(status.error()));
    return status.error();
  }
  AcceleratorRecord* accelerator = state.find_accelerator(plan.binding.accelerator);
  if (accelerator == nullptr) {
    return make_error(ErrorCode::NotFound, "planned accelerator is no longer registered");
  }
  if (!state.policy.allow_destructive_reconfiguration ||
      !plan.request.allow_destructive_reconfiguration) {
    return make_error(ErrorCode::PolicyRejected,
                      "destructive reconfiguration is not permitted by policy");
  }
  std::vector<PartitionRecord*> affected;
  for (auto& entry : state.partitions) {
    PartitionRecord& record = entry.second;
    if (record.accelerator != accelerator->id || is_terminal_state(record.state) ||
        record.state == PartitionState::Unpartitioned) {
      continue;
    }
    if (!destruction_safe(state, record)) {
      return make_error(ErrorCode::DrainRequired,
                        "destructive reconfiguration is not safe while a partition is occupied",
                        record.id.str());
    }
    affected.push_back(&record);
  }
  ResourceVector target_capacity;
  for (const PlannedPartition& planned : plan.planned_partitions) {
    if (target_capacity.empty()) {
      target_capacity = planned.resources;
      continue;
    }
    const Status accumulated = target_capacity.add_assign(planned.resources);
    if (!accumulated.ok()) {
      return accumulated.error();
    }
  }
  ResourceVector held;
  const CapacityLedger& current = *state.find_ledger(accelerator->id);
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const auto dimension = static_cast<ResourceDimension>(index);
    if (!current.total().has(dimension)) {
      continue;
    }
    const std::uint64_t value = current.bucket(CapacityBucket::Active).get(dimension) +
                                current.bucket(CapacityBucket::Draining).get(dimension) +
                                current.bucket(CapacityBucket::ReconfigurationHeld).get(dimension);
    const Status set = held.set(dimension, value);
    if (!set.ok()) {
      return set.error();
    }
  }
  ResourceVector hold;
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const auto dimension = static_cast<ResourceDimension>(index);
    if (!target_capacity.has(dimension)) {
      continue;
    }
    const std::uint64_t needed = target_capacity.get(dimension);
    const std::uint64_t available = held.get(dimension);
    const Status set = hold.set(dimension, needed > available ? needed - available : 0);
    if (!set.ok()) {
      return set.error();
    }
  }

  PartitionPlanId internal_plan{};
  const Result<PartitionReservation> reservation = create_destructive_reservation_locked(
      state, *accelerator, affected, hold, true, now, &internal_plan);
  if (!reservation.ok()) {
    return reservation.error();
  }
  PartitionReservation* stored = state.find_reservation(reservation.value().id);
  stored->plan = plan.id;
  stored->plan_generation = plan.generation;
  stored->planned_mutations =
      static_cast<std::uint32_t>(plan.planned_partitions.size() + affected.size());
  for (PartitionRecord* candidate : affected) {
    const Status moved = transition_partition(state, *candidate,
                                              PartitionState::ReconfigurationRequired,
                                              "device-wide reconfiguration attempt registered");
    if (!moved.ok()) {
      return moved.error();
    }
  }
  const Result<PartitionAttempt> registered =
      register_attempt_locked(state, *stored, AttemptKind::ReconfigureLayout, WorkerId{},
                              WorkerBootId{}, now);
  if (!registered.ok()) {
    return registered.error();
  }
  const PartitionAttempt attempt = registered.value();
  const std::string stable_key = state.stable_keys[accelerator->id.value()];

  PartitionMutationRequest request;
  request.stable_key = stable_key;
  request.expected_boot = accelerator->boot_id;
  request.expected_accelerator_generation = accelerator->generation;
  request.expected_capability_generation = accelerator->capability.generation;
  request.coordinator_epoch = state.coordinator_epoch;
  request.worker = attempt.worker;
  request.worker_boot = attempt.worker_boot;
  request.attempt = attempt.id;
  request.idempotency_token = attempt.token;
  request.destructive = true;
  request.full_device_reconfiguration = true;
  for (const PlannedPartition& planned : plan.planned_partitions) {
    NativePartitionSpec spec;
    spec.vendor_native_profile = planned.vendor_native_profile;
    spec.profile = planned.profile;
    spec.resources = planned.resources;
    request.desired.push_back(std::move(spec));
  }
  for (const PartitionRecord* record : affected) {
    if (!record->native_identity.native_id.empty()) {
      request.remove_native_ids.push_back(record->native_identity.native_id);
    }
  }
  plan_it->second.terminal = true;

  lock.unlock();
  Result<BackendMutationResult> mutation = target->reconfigure_layout(request);
  BackendLayout observed;
  bool have_observed = false;
  if (mutation.ok() && mutation.value().accepted) {
    Result<BackendLayout> layout = target->query_layout(stable_key);
    if (layout.ok()) {
      observed = layout.value();
      have_observed = true;
    }
  }
  lock.lock();
  MutationOutcome outcome = apply_mutation_result_locked(
      state, attempt.id, mutation, have_observed ? &observed : nullptr, impl_->now());
  outcome.plan = plan_id;
  return outcome;
}

// ---------------------------------------------------------------------------
// Drain
// ---------------------------------------------------------------------------

Status PartitionFabric::begin_drain(const PartitionId& partition_id, std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  PartitionRecord* record = state.find_partition(partition_id);
  if (record == nullptr) {
    return failure(ErrorCode::NotFound, "partition is not known", partition_id.str());
  }
  if (record->state != PartitionState::Active && record->state != PartitionState::Degraded) {
    return failure(ErrorCode::InvalidTransition, "only an active partition can be drained",
                   std::string(partition_id.str()) + " is " + to_string(record->state));
  }
  std::uint32_t outstanding = 0;
  for (const auto& entry : state.assignments) {
    if (entry.second.partition == partition_id) {
      ++outstanding;
    }
  }
  const Status moved = transition_partition(state, *record, PartitionState::Draining,
                                            reason.empty() ? "drain requested" : reason);
  if (!moved.ok()) {
    return moved;
  }
  record->drain.state = DrainState::Draining;
  record->drain.outstanding_assignments = outstanding;
  record->drain.total_assignments = outstanding;
  record->drain.started_at_ms = impl_->now();
  record->drain.completed_at_ms = 0;
  record->drain.blocked_reason.clear();
  record->drain.blocker.clear();
  if (outstanding == 0) {
    record->drain.state = DrainState::DrainComplete;
    record->drain.completed_at_ms = impl_->now();
  }
  bump_state_generation(state);
  record_event(state, ExplanationCode::DrainStarted, partition_id.str(),
               "drain started with " + std::to_string(outstanding) + " outstanding assignments");
  return success();
}

Status PartitionFabric::begin_drain_accelerator(const AcceleratorId& accelerator_id,
                                                std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  std::vector<PartitionId> targets;
  for (const auto& entry : state.partitions) {
    if (entry.second.accelerator != accelerator_id) {
      continue;
    }
    const PartitionState partition_state = entry.second.state;
    if (partition_state == PartitionState::Active || partition_state == PartitionState::Degraded) {
      targets.push_back(entry.second.id);
    }
  }
  if (targets.empty()) {
    return failure(ErrorCode::NotFound, "accelerator has no drainable partition",
                   accelerator_id.str());
  }
  for (const PartitionId partition_id : targets) {
    PartitionRecord* record = state.find_partition(partition_id);
    std::uint32_t outstanding = 0;
    for (const auto& entry : state.assignments) {
      if (entry.second.partition == partition_id) {
        ++outstanding;
      }
    }
    const Status moved = transition_partition(state, *record, PartitionState::Draining,
                                              reason.empty() ? "device drain requested" : reason);
    if (!moved.ok()) {
      return moved;
    }
    record->drain.state = outstanding == 0 ? DrainState::DrainComplete : DrainState::Draining;
    record->drain.outstanding_assignments = outstanding;
    record->drain.total_assignments = outstanding;
    record->drain.started_at_ms = impl_->now();
    record->drain.completed_at_ms = outstanding == 0 ? impl_->now() : 0;
    record_event(state, ExplanationCode::DrainStarted, partition_id.str(), "device drain started");
  }
  bump_state_generation(state);
  return success();
}

Status PartitionFabric::complete_drain(const PartitionId& partition_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  PartitionRecord* record = state.find_partition(partition_id);
  if (record == nullptr) {
    return failure(ErrorCode::NotFound, "partition is not known", partition_id.str());
  }
  if (record->state != PartitionState::Draining) {
    return failure(ErrorCode::InvalidTransition, "partition is not draining",
                   to_string(record->state));
  }
  std::uint32_t outstanding = 0;
  for (const auto& entry : state.assignments) {
    if (entry.second.partition == partition_id) {
      ++outstanding;
    }
  }
  record->drain.outstanding_assignments = outstanding;
  if (outstanding != 0) {
    record->drain.state = DrainState::DrainBlocked;
    record->drain.blocker = "outstanding-authoritative-assignments";
    record->drain.blocked_reason =
        "this runtime owns the partition-side drain gate, not workload migration";
    record_event(state, ExplanationCode::DrainBlocked, partition_id.str(),
                 record->drain.blocked_reason);
    return failure(ErrorCode::DrainRequired,
                   "drain cannot complete while assignments remain outstanding",
                   std::to_string(outstanding));
  }
  record->drain.state = DrainState::DrainComplete;
  record->drain.completed_at_ms = impl_->now();
  record->drain.blocker.clear();
  record->drain.blocked_reason.clear();
  bump_state_generation(state);
  record_event(state, ExplanationCode::DrainCompleted, partition_id.str(),
               "destructive mutation is now safe for this partition");
  return success();
}

Status PartitionFabric::cancel_drain(const PartitionId& partition_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  PartitionRecord* record = state.find_partition(partition_id);
  if (record == nullptr) {
    return failure(ErrorCode::NotFound, "partition is not known", partition_id.str());
  }
  if (record->state != PartitionState::Draining) {
    return failure(ErrorCode::InvalidTransition, "partition is not draining",
                   to_string(record->state));
  }
  if (!destruction_safe(state, *record) && record->drain.outstanding_assignments != 0) {
    return failure(ErrorCode::DrainRequired,
                   "drain cannot be cancelled while work still occupies the partition");
  }
  const Status moved = transition_partition(state, *record, PartitionState::Active,
                                            "drain cancelled");
  if (!moved.ok()) {
    return moved;
  }
  record->drain.state = DrainState::NotDraining;
  record->drain.started_at_ms = 0;
  record->drain.completed_at_ms = 0;
  record->drain.blocker.clear();
  record->drain.blocked_reason.clear();
  bump_state_generation(state);
  record_event(state, ExplanationCode::DrainProgressed, partition_id.str(), "drain cancelled");
  return success();
}

Result<DrainProgress> PartitionFabric::drain_progress(const PartitionId& partition_id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const PartitionRecord* record = impl_->state.find_partition(partition_id);
  if (record == nullptr) {
    return make_error(ErrorCode::NotFound, "partition is not known", partition_id.str());
  }
  return record->drain;
}

// ---------------------------------------------------------------------------
// Admission
// ---------------------------------------------------------------------------

Result<AdmissionProbe> PartitionFabric::probe_admission(
    const WorkloadRequirement& requirement) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return evaluate_admission(impl_->state, requirement, impl_->now());
}

Result<AdmissionDecision> PartitionFabric::admit(const WorkloadRequirement& requirement) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const std::uint64_t now = impl_->now();
  AdmissionDecision decision;
  decision.state_generation = state.state_generation;
  decision.coordinator_epoch = state.coordinator_epoch;
  decision.policy_generation = state.policy_generation;
  const Status open = check_open(state);
  if (!open.ok()) {
    decision.outcome = AdmissionOutcome::RejectedPolicy;
    decision.message = to_string(open.error());
    decision.explanation.add(ExplanationCode::RejectedPolicy, "runtime", decision.message);
    return decision;
  }
  const AdmissionProbe probe = evaluate_admission(state, requirement, now);
  decision.candidates_considered = probe.candidates_considered;
  decision.explanation = probe.explanation;
  if (probe.outcome != AdmissionOutcome::Admitted) {
    decision.outcome = probe.outcome;
    decision.message = to_string(probe.outcome);
    return decision;
  }
  // Revalidate immediately before authority is bound.
  PartitionRecord* record = state.find_partition(probe.partition);
  if (record == nullptr || record->state != PartitionState::Active) {
    decision.outcome = AdmissionOutcome::RejectedStaleAuthority;
    decision.message = "partition authority changed between evaluation and commit";
    decision.explanation.add(ExplanationCode::RejectedStaleEvidence, probe.partition.str(),
                             decision.message);
    return decision;
  }
  const AcceleratorRecord* accelerator = state.find_accelerator(record->accelerator);
  if (accelerator == nullptr) {
    decision.outcome = AdmissionOutcome::RejectedDeviceUnavailable;
    decision.message = "partition accelerator is no longer registered";
    return decision;
  }
  if (!accelerator->capability.supports_profile(record->profile)) {
    decision.outcome = AdmissionOutcome::RejectedCapability;
    decision.message = "device capability no longer publishes the partition profile";
    decision.explanation.add(ExplanationCode::RejectedCapabilityUnsupported, record->id.str(),
                             decision.message);
    return decision;
  }
  if (!requirement.isolation.required.is_subset_of(record->isolation.guarantees)) {
    decision.outcome = AdmissionOutcome::RejectedIsolation;
    decision.message = "partition isolation guarantees changed";
    return decision;
  }
  if (!requirement.minimum_resources.empty() &&
      !requirement.minimum_resources.is_subset_of(record->resources)) {
    decision.outcome = AdmissionOutcome::RejectedInsufficientResources;
    decision.message = "partition resources no longer satisfy the requirement";
    return decision;
  }
  if (state.assignments.size() >= state.limits.max_assignments) {
    decision.outcome = AdmissionOutcome::RejectedLimit;
    decision.message = "assignment registry is full";
    return decision;
  }
  std::size_t existing = 0;
  for (const auto& entry : state.assignments) {
    if (entry.second.partition == record->id) {
      ++existing;
    }
  }
  if (existing > 0 && (requirement.exclusive || state.policy.exclusive_by_default)) {
    decision.outcome = AdmissionOutcome::RejectedExclusiveConflict;
    decision.message = "partition already hosts an authoritative assignment";
    decision.explanation.add(ExplanationCode::RejectedPolicy, record->id.str(), decision.message);
    return decision;
  }
  const Result<PartitionAssignmentId> assignment_id = state.ids.allocate<PartitionAssignmentId>();
  if (!assignment_id.ok()) {
    decision.outcome = AdmissionOutcome::RejectedLimit;
    decision.message = to_string(assignment_id.error());
    return decision;
  }
  PartitionAssignment assignment;
  assignment.id = assignment_id.value();
  assignment.partition = record->id;
  assignment.partition_generation = record->generation;
  assignment.accelerator = record->accelerator;
  assignment.accelerator_generation = accelerator->generation;
  assignment.workload_id = requirement.workload_id;
  assignment.tenant_id = requirement.tenant_id;
  assignment.isolation = requirement.isolation;
  assignment.policy_generation = state.policy_generation;
  assignment.coordinator_epoch = state.coordinator_epoch;
  assignment.evidence_generation = record->evidence.generation;
  assignment.exclusive = requirement.exclusive || state.policy.exclusive_by_default;
  assignment.bound_at_ms = now;
  assignment.last_validated_at_ms = now;
  const Status validation = assignment.validate();
  if (!validation.ok()) {
    decision.outcome = AdmissionOutcome::RejectedInvalidRequest;
    decision.message = to_string(validation.error());
    return decision;
  }
  state.assignments[assignment.id.value()] = assignment;
  record->assignment_state = AssignmentState::Assigned;
  if (record->drain.state == DrainState::DrainComplete ||
      record->drain.state == DrainState::Draining) {
    record->drain.outstanding_assignments += 1;
  }
  decision.outcome = AdmissionOutcome::Admitted;
  decision.partition = record->id;
  decision.partition_generation = record->generation;
  decision.assignment = assignment.id;
  decision.accelerator = record->accelerator;
  decision.accelerator_generation = accelerator->generation;
  decision.message = "workload admitted to partition " + record->id.str();
  decision.explanation.add(ExplanationCode::CommitAccepted, assignment.id.str(), decision.message);
  bump_state_generation(state);
  record_event(state, ExplanationCode::CommitAccepted, assignment.id.str(), decision.message);
  return decision;
}

Status PartitionFabric::bind_assignment(const PartitionAssignment& assignment) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Status validation = assignment.validate();
  if (!validation.ok()) {
    return validation;
  }
  PartitionRecord* record = state.find_partition(assignment.partition);
  if (record == nullptr) {
    return failure(ErrorCode::NotFound, "partition is not known", assignment.partition.str());
  }
  if (record->generation != assignment.partition_generation) {
    return failure(ErrorCode::StalePartitionGeneration,
                   "assignment targets a superseded partition generation",
                   assignment.partition.str());
  }
  if (record->state != PartitionState::Active) {
    return failure(ErrorCode::RevalidationRequired,
                   "assignment targets a partition that is not authoritative",
                   to_string(record->state));
  }
  if (state.assignments.size() >= state.limits.max_assignments) {
    return failure(ErrorCode::LimitExceeded, "assignment registry is full");
  }
  PartitionAssignment stored = assignment;
  if (!stored.id.valid()) {
    const Result<PartitionAssignmentId> id = state.ids.allocate<PartitionAssignmentId>();
    if (!id.ok()) {
      return id.error();
    }
    stored.id = id.value();
  }
  state.assignments[stored.id.value()] = stored;
  record->assignment_state = AssignmentState::Assigned;
  bump_state_generation(state);
  return success();
}

Status PartitionFabric::unbind_assignment(const PartitionAssignmentId& assignment_id,
                                          std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const auto it = state.assignments.find(assignment_id.value());
  if (it == state.assignments.end()) {
    return failure(ErrorCode::NotFound, "assignment is not known", assignment_id.str());
  }
  const PartitionId partition_id = it->second.partition;
  state.assignments.erase(it);
  PartitionRecord* record = state.find_partition(partition_id);
  if (record != nullptr) {
    std::size_t remaining = 0;
    for (const auto& entry : state.assignments) {
      if (entry.second.partition == partition_id) {
        ++remaining;
      }
    }
    record->assignment_state =
        remaining == 0 ? AssignmentState::Unassigned : AssignmentState::Assigned;
    if (record->drain.state == DrainState::Draining) {
      record->drain.outstanding_assignments = static_cast<std::uint32_t>(remaining);
      if (remaining == 0) {
        record->drain.state = DrainState::DrainComplete;
        record->drain.completed_at_ms = impl_->now();
        record_event(state, ExplanationCode::DrainCompleted, partition_id.str(),
                     "drain completed after the last assignment was released");
      }
    }
  }
  bump_state_generation(state);
  record_event(state, ExplanationCode::ReleaseAccepted, assignment_id.str(),
               reason.empty() ? "assignment released" : reason);
  return success();
}

std::vector<PartitionAssignment> PartitionFabric::assignments_of(
    const PartitionId& partition_id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<PartitionAssignment> out;
  for (const auto& entry : impl_->state.assignments) {
    if (entry.second.partition == partition_id) {
      out.push_back(entry.second);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Worker incarnation and coordinator epoch
// ---------------------------------------------------------------------------

Status PartitionFabric::note_worker(const WorkerRecord& worker) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  if (!worker.id.valid() || !worker.boot.valid()) {
    return failure(ErrorCode::InvalidArgument,
                   "worker registration requires both a worker identity and a boot identity");
  }
  auto existing = state.workers.find(worker.id.value());
  if (existing == state.workers.end() && state.workers.size() >= state.limits.max_workers) {
    return failure(ErrorCode::LimitExceeded, "worker registry is full");
  }
  if (existing != state.workers.end() && existing->second.boot != worker.boot) {
    // A replacement incarnation permanently fences the previous boot identity.
    const WorkerBootId previous_boot = existing->second.boot;
    existing->second.fenced = true;
    existing->second.alive = false;
    existing->second.fenced_at_ms = impl_->now();
    existing->second.fence_reason = "worker reincarnated under a fresh boot identity";
    record_event(state, ExplanationCode::WorkerFenced, worker.id.str(),
                 "previous boot " + previous_boot.hex() + " fenced");
    (void)fence_worker_locked(state, worker.id, previous_boot, impl_->now(),
                              "worker reincarnated under a fresh boot identity");
  }
  WorkerRecord stored = worker;
  stored.coordinator_epoch = state.coordinator_epoch;
  state.workers[stored.id.value()] = stored;
  bump_state_generation(state);
  record_event(state, ExplanationCode::RankingFactor, stored.id.str(),
               "worker registered with boot " + stored.boot.hex());
  return success();
}

Status PartitionFabric::note_worker_seen(const WorkerId& worker, const WorkerBootId& worker_boot,
                                         std::uint64_t at_ms) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  WorkerRecord* record = impl_->state.find_worker(worker);
  if (record == nullptr) {
    return failure(ErrorCode::NotFound, "worker is not registered", worker.str());
  }
  if (record->boot != worker_boot) {
    return failure(ErrorCode::StaleWorker, "worker boot identity is not current",
                   worker_boot.hex());
  }
  record->last_seen_ms = at_ms;
  record->alive = true;
  return success();
}

Status PartitionFabric::fence_worker(const WorkerId& worker, const WorkerBootId& worker_boot,
                                     std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return fence_worker_locked(impl_->state, worker, worker_boot, impl_->now(), std::move(reason));
}

Result<CoordinatorEpoch> PartitionFabric::advance_coordinator_epoch() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Result<CoordinatorEpoch> next = state.coordinator_epoch.next();
  if (!next.ok()) {
    return next.error();
  }
  const std::uint64_t now = impl_->now();
  state.coordinator_epoch = next.value();
  for (auto& entry : state.reservations) {
    PartitionReservation& reservation = entry.second;
    if (reservation_is_terminal(reservation.lifecycle)) {
      continue;
    }
    if (reservation.coordinator_epoch != state.coordinator_epoch) {
      (void)fence_reservation_locked(
          state, reservation,
          "coordinator epoch advanced; reservations from an older epoch lose authority", now);
    }
  }
  for (auto& entry : state.attempts) {
    PartitionAttempt& attempt = entry.second;
    if (attempt_settled(attempt.state)) {
      continue;
    }
    if (attempt.coordinator_epoch != state.coordinator_epoch) {
      attempt.state = AttemptState::OutcomeUnknown;
      attempt.settled_at_ms = now;
      attempt.detail = "coordinator epoch advanced while this attempt was in flight";
      for (const PartitionId partition_id : attempt.partitions) {
        PartitionRecord* record = state.find_partition(partition_id);
        if (record != nullptr && record->state != PartitionState::RevalidationRequired) {
          (void)transition_partition(state, *record, PartitionState::RevalidationRequired,
                                     attempt.detail);
        }
      }
      record_event(state, ExplanationCode::StaleEpochRejected, attempt.id.str(), attempt.detail);
    }
  }
  for (auto& entry : state.workers) {
    if (entry.second.coordinator_epoch != state.coordinator_epoch) {
      entry.second.alive = false;
    }
  }
  bump_state_generation(state);
  record_event(state, ExplanationCode::StaleEpochRejected, "coordinator",
               "epoch advanced to " + std::to_string(state.coordinator_epoch.value()));
  return state.coordinator_epoch;
}

Status PartitionFabric::install_coordinator_epoch(const CoordinatorEpoch& epoch) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  if (epoch < state.coordinator_epoch) {
    return failure(ErrorCode::GenerationRegression, "coordinator epoch would regress",
                   std::to_string(epoch.value()));
  }
  state.coordinator_epoch = epoch;
  bump_state_generation(state);
  return success();
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kReconciliationFindingCap = 4096;

void add_finding(ReconciliationReport& report, ReconciliationFindingKind kind,
                 ReconciliationSeverity severity, const AcceleratorId& accelerator,
                 const PartitionId& partition, PartitionGeneration generation,
                 PartitionState resulting_state, std::string native_id, std::string detail) {
  if (report.findings.size() >= kReconciliationFindingCap) {
    return;
  }
  ReconciliationFinding finding;
  finding.kind = kind;
  finding.severity = severity;
  finding.accelerator = accelerator;
  finding.partition = partition;
  finding.expected_generation = generation;
  finding.resulting_state = resulting_state;
  finding.native_id = std::move(native_id);
  finding.detail = std::move(detail);
  report.findings.push_back(std::move(finding));
}

const BackendNativePartition* observed_native(const BackendLayout& layout,
                                              const std::string& native_id) {
  for (const BackendNativePartition& partition : layout.partitions) {
    if (!native_id.empty() && partition.native_id == native_id) {
      return &partition;
    }
  }
  return nullptr;
}

bool layout_satisfies(const BackendLayout& layout,
                      const std::vector<NativePartitionSpec>& desired) {
  if (desired.empty()) {
    return false;
  }
  std::vector<bool> used(layout.partitions.size(), false);
  for (const NativePartitionSpec& spec : desired) {
    bool matched = false;
    for (std::size_t index = 0; index < layout.partitions.size(); ++index) {
      if (used[index]) {
        continue;
      }
      const BackendNativePartition& candidate = layout.partitions[index];
      const bool profile_matches = spec.vendor_native_profile.empty() ||
                                   candidate.vendor_native_profile == spec.vendor_native_profile;
      const bool resources_match = spec.resources.empty() || candidate.resources == spec.resources;
      if (profile_matches && resources_match) {
        used[index] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }
  return true;
}

}  // namespace

Result<ReconciliationReport> PartitionFabric::reconcile(const AcceleratorId& accelerator_id,
                                                       const BackendLayout& observed) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const std::uint64_t now = impl_->now();
  AcceleratorRecord* accelerator = state.find_accelerator(accelerator_id);
  if (accelerator == nullptr) {
    return make_error(ErrorCode::NotFound, "accelerator is not registered", accelerator_id.str());
  }
  const Status layout_valid = validate_backend_layout(observed, state.limits);
  if (!layout_valid.ok()) {
    return layout_valid.error();
  }
  const Result<ReconciliationId> report_id = state.ids.allocate<ReconciliationId>();
  if (!report_id.ok()) {
    return report_id.error();
  }
  ReconciliationReport report;
  report.id = report_id.value();
  report.accelerator = accelerator_id;
  report.accelerator_generation = accelerator->generation;
  report.capability_generation = accelerator->capability.generation;
  report.device_present = observed.device_present;

  if (!observed.device_present) {
    for (auto& entry : state.partitions) {
      PartitionRecord& record = entry.second;
      if (record.accelerator != accelerator_id || is_terminal_state(record.state) ||
          record.state == PartitionState::Offline) {
        continue;
      }
      const Status moved = transition_partition(state, record, PartitionState::Offline,
                                                "device is no longer present");
      add_finding(report, ReconciliationFindingKind::DeviceGone,
                  ReconciliationSeverity::Critical, accelerator_id, record.id, record.generation,
                  PartitionState::Offline, record.native_identity.native_id,
                  moved.ok() ? "device is not present; partition authority is offline"
                             : to_string(moved.error()));
    }
    report.revalidation_required = true;
    accelerator->evidence.observed = false;
    report.explanation.add(ExplanationCode::ReconciledDeviceGone, accelerator_id.str(),
                           "backend reported the device absent");
    report.explanation.set_summary(report.summary());
    bump_state_generation(state);
    record_event(state, ExplanationCode::ReconciledDeviceGone, accelerator_id.str(),
                 report.summary());
    return report;
  }

  if (observed.boot_id.valid() && accelerator->boot_id.valid() &&
      observed.boot_id != accelerator->boot_id) {
    const Result<AcceleratorGeneration> next = accelerator->generation.next();
    if (!next.ok()) {
      return next.error();
    }
    accelerator->generation = next.value();
    accelerator->boot_id = observed.boot_id;
    accelerator->capability.support = PartitionSupportState::Unknown;
    accelerator->capability.generation = CapabilityGeneration::unknown();
    report.device_generation_changed = true;
    report.capability_changed = true;
    add_finding(report, ReconciliationFindingKind::DeviceReset, ReconciliationSeverity::Critical,
                accelerator_id, PartitionId{}, PartitionGeneration{}, PartitionState::Offline, {},
                "device incarnation changed: " + observed.boot_id.hex());
  }
  if (!observed.physical_totals.empty() &&
      accelerator->physical_totals != observed.physical_totals) {
    add_finding(report, ReconciliationFindingKind::LedgerMismatch,
                ReconciliationSeverity::Critical, accelerator_id, PartitionId{},
                PartitionGeneration{}, PartitionState::Offline, {},
                "observed physical capacity differs from the registered capacity");
    report.capability_changed = true;
  }
  if (!accelerator->capability.declares_supported()) {
    add_finding(report, ReconciliationFindingKind::DeviceNotPartitionCapable,
                ReconciliationSeverity::Material, accelerator_id, PartitionId{},
                PartitionGeneration{}, PartitionState::Offline, {},
                "device is not partition capable under current capability evidence");
  }

  std::vector<std::string> claimed;
  for (auto& entry : state.partitions) {
    PartitionRecord& record = entry.second;
    if (record.accelerator != accelerator_id || is_terminal_state(record.state) ||
        record.state == PartitionState::Unpartitioned) {
      continue;
    }
    const BackendNativePartition* native =
        observed_native(observed, record.native_identity.native_id);
    if (record.native_identity.native_id.empty()) {
      if (accelerator->capability.declares_supported() &&
          record.state != PartitionState::RevalidationRequired) {
        const Status moved = transition_partition(state, record,
                                                  PartitionState::RevalidationRequired,
                                                  "partition generation cannot be correlated "
                                                  "with observed hardware");
        add_finding(report, ReconciliationFindingKind::GenerationUncorrelatable,
                    ReconciliationSeverity::Material, accelerator_id, record.id, record.generation,
                    PartitionState::RevalidationRequired, {},
                    moved.ok() ? "durable record carries no verified physical identity"
                               : to_string(moved.error()));
        report.revalidation_required = true;
      }
      continue;
    }
    if (native == nullptr) {
      CapacityLedger* ledger = state.find_ledger(accelerator_id);
      const std::optional<CapacityBucket> held = record.held_bucket;
      record.held_bucket.reset();
      if (ledger != nullptr && held.has_value()) {
        const Status released = ledger->move_between(*held, CapacityBucket::Free, record.resources);
        if (!released.ok()) {
          add_finding(report, ReconciliationFindingKind::LedgerMismatch,
                      ReconciliationSeverity::Critical, accelerator_id, record.id,
                      record.generation, PartitionState::RevalidationRequired, {},
                      to_string(released.error()));
        }
      }
      supersede_partition_generation(state, record);
      record.state = PartitionState::Retired;
      record.last_transition_at_ms = now;
      record.last_transition_reason = "observed hardware no longer contains this partition";
      record.assignment_state = AssignmentState::Unassigned;
      record.drain.state = DrainState::NotDraining;
      record.drain.outstanding_assignments = 0;
      ++report.missing;
      report.layout_changed = true;
      add_finding(report, ReconciliationFindingKind::PartitionMissing,
                  ReconciliationSeverity::Material, accelerator_id, record.id, record.generation,
                  PartitionState::Retired, record.native_identity.native_id,
                  "partition disappeared from observed hardware; capacity returned to free");
      std::vector<PartitionAssignmentId> orphaned;
      for (const auto& assignment : state.assignments) {
        if (assignment.second.partition == record.id) {
          orphaned.push_back(assignment.second.id);
        }
      }
      for (const PartitionAssignmentId& assignment_id : orphaned) {
        state.assignments.erase(assignment_id.value());
        add_finding(report, ReconciliationFindingKind::AssignmentOrphaned,
                    ReconciliationSeverity::Critical, accelerator_id, record.id,
                    record.generation, PartitionState::Retired, {},
                    "assignment " + assignment_id.str() + " removed with its partition");
      }
      continue;
    }
    claimed.push_back(native->native_id);
    const PartitionProfile* profile = state.find_profile(record.profile);
    const bool profile_matches = profile == nullptr || native->vendor_native_profile.empty() ||
                                 native->vendor_native_profile == profile->vendor_native;
    if (!profile_matches) {
      ++report.fenced;
      report.revalidation_required = true;
      std::string detail = "observed profile differs from the recorded profile";
      if (record.state != PartitionState::RevalidationRequired) {
        const Status moved = transition_partition(state, record,
                                                  PartitionState::RevalidationRequired, detail);
        if (!moved.ok()) {
          detail = to_string(moved.error());
        }
      }
      add_finding(report, ReconciliationFindingKind::ProfileChanged,
                  ReconciliationSeverity::Material, accelerator_id, record.id, record.generation,
                  PartitionState::RevalidationRequired, native->native_id, detail);
      continue;
    }
    ++report.matched;
    stamp_partition_evidence(state, record, *accelerator, now, observed.evidence.provenance,
                             observed.evidence.source);
    if (record.state == PartitionState::Offline) {
      const Status moved = transition_partition(state, record,
                                                PartitionState::RevalidationRequired,
                                                "device reappeared; authority must be revalidated");
      (void)moved;
      report.revalidation_required = true;
      add_finding(report, ReconciliationFindingKind::PartitionMatches,
                  ReconciliationSeverity::Attention, accelerator_id, record.id, record.generation,
                  PartitionState::RevalidationRequired, native->native_id,
                  "device reappeared; partition authority requires revalidation");
      continue;
    }
    if (record.state == PartitionState::RevalidationRequired) {
      if (state.policy.allow_external_adoption) {
        const Status moved = transition_partition(state, record, PartitionState::Active,
                                                  "adopted after reconciliation");
        if (moved.ok()) {
          record.externally_observed = true;
          ++report.adopted;
          report.revalidation_required = false;
          add_finding(report, ReconciliationFindingKind::PartitionMatches,
                      ReconciliationSeverity::Attention, accelerator_id, record.id,
                      record.generation, PartitionState::Active, native->native_id,
                      "externally observed partition adopted under policy");
          report.explanation.add(ExplanationCode::ExternalAdoption, record.id.str(),
                                 "adopted after reconciliation");
        } else {
          add_finding(report, ReconciliationFindingKind::PartitionMatches,
                      ReconciliationSeverity::Material, accelerator_id, record.id,
                      record.generation, PartitionState::RevalidationRequired, native->native_id,
                      to_string(moved.error()));
        }
      } else {
        report.revalidation_required = true;
        add_finding(report, ReconciliationFindingKind::PartitionMatches,
                    ReconciliationSeverity::Attention, accelerator_id, record.id,
                    record.generation, PartitionState::RevalidationRequired, native->native_id,
                    "physical partition matches, but current authority requires an explicit "
                    "authorised decision before it is restored");
      }
    } else {
      add_finding(report, ReconciliationFindingKind::PartitionMatches,
                  ReconciliationSeverity::Informational, accelerator_id, record.id,
                  record.generation, record.state, native->native_id,
                  "durable record matches observed hardware");
    }
  }

  // Resolve unsettled attempts against observed physical reality before any
  // unexplained partition is recorded. This is the only place where an
  // ambiguous mutation is decided, and each one is decided exactly once: the
  // attempt's own logical partition identity adopts the physical partition it
  // produced, so one physical partition is never represented twice.
  for (auto& entry : state.attempts) {
    PartitionAttempt& attempt = entry.second;
    if (attempt.accelerator != accelerator_id || attempt.state != AttemptState::OutcomeUnknown) {
      continue;
    }
    bool satisfied = false;
    if (attempt.kind == AttemptKind::DestroyPartition) {
      satisfied = true;
      for (const PartitionId partition_id : attempt.partitions) {
        const PartitionRecord* record = state.find_partition(partition_id);
        if (record == nullptr) {
          continue;
        }
        if (!record->native_identity.native_id.empty() &&
            observed_native(observed, record->native_identity.native_id) != nullptr) {
          satisfied = false;
          break;
        }
      }
    } else {
      // For a create attempt the stored desired set is the intended result; for
      // a device reconfiguration it is the whole target layout.
      std::vector<NativePartitionSpec> desired;
      for (const PlannedPartition& planned : attempt.desired) {
        NativePartitionSpec spec;
        spec.vendor_native_profile = planned.vendor_native_profile;
        spec.profile = planned.profile;
        spec.resources = planned.resources;
        desired.push_back(std::move(spec));
      }
      satisfied = layout_satisfies(observed, desired);
    }
    attempt.state = AttemptState::Reconciled;
    attempt.settled_at_ms = now;
    attempt.observed_native_layout_digest =
        observed.stable_key + ":" + std::to_string(observed.partitions.size());
    PartitionReservation* reservation = state.find_reservation(attempt.reservation);
    if (satisfied) {
      attempt.detail = "reconciled: the intended physical result is present";
      for (std::size_t index = 0; index < attempt.partitions.size(); ++index) {
        PartitionRecord* record = state.find_partition(attempt.partitions[index]);
        if (record == nullptr) {
          continue;
        }
        // Adopt the physical identity this attempt produced, and claim the
        // observed partition so that it is not recorded a second time.
        if (index < attempt.desired.size()) {
          NativePartitionSpec spec;
          spec.vendor_native_profile = attempt.desired[index].vendor_native_profile;
          spec.profile = attempt.desired[index].profile;
          spec.resources = attempt.desired[index].resources;
          if (const BackendNativePartition* native =
                  find_unclaimed_native_for_spec(observed, spec, claimed)) {
            record->native_identity.backend = accelerator->backend;
            record->native_identity.native_id = native->native_id;
            record->native_identity.instance_uuid = native->instance_uuid;
            record->native_identity.parent_native_id = observed.stable_key;
            record->isolation.name = native->isolation_domain;
            record->isolation.guarantees = native->isolation;
            record->isolation.backend_defined = !native->isolation_domain.empty();
            claimed.push_back(native->native_id);
          }
        }
        if (record->state == PartitionState::RevalidationRequired ||
            record->state == PartitionState::Creating) {
          const PartitionState target = attempt.kind == AttemptKind::DestroyPartition
                                            ? PartitionState::Retired
                                            : PartitionState::Active;
          const Status moved = transition_partition(state, *record, target,
                                                    "reconciled with observed hardware");
          if (moved.ok()) {
            if (target == PartitionState::Retired) {
              supersede_partition_generation(state, *record);
            } else {
              ++report.adopted;
            }
          } else {
            add_finding(report, ReconciliationFindingKind::GenerationUncorrelatable,
                        ReconciliationSeverity::Material, accelerator_id, record->id,
                        record->generation, record->state, record->native_identity.native_id,
                        to_string(moved.error()));
          }
        }
      }
      if (reservation != nullptr && !reservation_is_terminal(reservation->lifecycle)) {
        reservation->lifecycle = ReservationLifecycle::Committed;
        reservation->reason = "committed after reconciliation";
        reservation->updated_at_ms = now;
      }
      report.explanation.add(ExplanationCode::ReconciliationRequired, attempt.id.str(),
                             "ambiguous mutation resolved: intended result present");
    } else {
      attempt.detail = "reconciled: the intended physical result is absent";
      for (const PartitionId partition_id : attempt.partitions) {
        PartitionRecord* record = state.find_partition(partition_id);
        if (record == nullptr) {
          continue;
        }
        if (record->state == PartitionState::RevalidationRequired ||
            record->state == PartitionState::Creating ||
            record->state == PartitionState::Destroying) {
          const Status moved = transition_partition(state, *record, PartitionState::Unpartitioned,
                                                    "reconciled: the mutation did not take effect");
          if (moved.ok()) {
            supersede_partition_generation(state, *record);
            record->native_identity = PartitionNativeIdentity{};
          }
        }
      }
      if (reservation != nullptr && !reservation_is_terminal(reservation->lifecycle)) {
        reservation->lifecycle = ReservationLifecycle::Released;
        reservation->reason = "released after reconciliation";
        reservation->updated_at_ms = now;
        CapacityLedger* ledger = state.find_ledger(accelerator_id);
        if (ledger != nullptr && !reservation->resources.empty()) {
          (void)ledger->release_reserved(reservation->resources);
        }
      }
      report.explanation.add(ExplanationCode::ReconciliationRequired, attempt.id.str(),
                             "ambiguous mutation resolved: intended result absent");
    }
    report.reconciliation_required = true;
    record_event(state, ExplanationCode::ReconciliationRequired, attempt.id.str(), attempt.detail);
  }

  for (const BackendNativePartition& native : observed.partitions) {
    bool known = false;
    for (const std::string& claimed_id : claimed) {
      if (claimed_id == native.native_id) {
        known = true;
        break;
      }
    }
    if (known) {
      continue;
    }
    ++report.unexpected;
    report.layout_changed = true;
    const Result<PartitionId> partition_id = state.ids.allocate<PartitionId>();
    if (!partition_id.ok()) {
      break;
    }
    PartitionRecord record;
    record.id = partition_id.value();
    record.generation = PartitionGeneration::first();
    record.profile = native.profile;
    record.mechanism = accelerator->capability.mechanism;
    record.accelerator = accelerator_id;
    record.accelerator_generation = accelerator->generation;
    record.accelerator_boot = accelerator->boot_id;
    record.resources = native.resources;
    record.native_identity.backend = accelerator->backend;
    record.native_identity.native_id = native.native_id;
    record.native_identity.instance_uuid = native.instance_uuid;
    record.native_identity.parent_native_id = observed.stable_key;
    record.isolation.name = native.isolation_domain;
    record.isolation.guarantees = native.isolation;
    record.isolation.backend_defined = !native.isolation_domain.empty();
    record.provenance = observed.evidence.provenance;
    record.externally_observed = true;
    record.created_at_ms = now;
    record.last_transition_at_ms = now;
    record.last_transition_reason = "observed physical partition";
    const PartitionProfile* profile = state.find_profile(native.profile);
    if (profile != nullptr) {
      record.profile_generation = profile->generation;
    }
    record.state = state.policy.allow_external_adoption ? PartitionState::Active
                                                        : PartitionState::RevalidationRequired;
    CapacityLedger* ledger = state.find_ledger(accelerator_id);
    if (ledger != nullptr) {
      // Unexplained physical state is recorded, never silently deleted. Its
      // capacity is held conservatively so the ledger never hands it out.
      const Status held = ledger->move_between(CapacityBucket::Free,
                                               CapacityBucket::ReconfigurationHeld,
                                               native.resources);
      if (held.ok()) {
        record.held_bucket = CapacityBucket::ReconfigurationHeld;
        if (record.state == PartitionState::Active) {
          const Status promoted = ledger->move_between(
              CapacityBucket::ReconfigurationHeld, CapacityBucket::Active, native.resources);
          if (promoted.ok()) {
            record.held_bucket = CapacityBucket::Active;
          } else {
            record.state = PartitionState::RevalidationRequired;
          }
        }
      } else {
        add_finding(report, ReconciliationFindingKind::LedgerMismatch,
                    ReconciliationSeverity::Critical, accelerator_id, record.id,
                    record.generation, record.state, native.native_id,
                    "unexplained physical partition consumes capacity the ledger believed free");
      }
    }
    stamp_partition_evidence(state, record, *accelerator, now, observed.evidence.provenance,
                             observed.evidence.source);
    state.partitions[record.id.value()] = record;
    add_finding(report,
                ReconciliationFindingKind::PartitionUnexpected,
                state.policy.allow_external_adoption ? ReconciliationSeverity::Attention
                                                     : ReconciliationSeverity::Material,
                accelerator_id, record.id, record.generation, record.state, native.native_id,
                state.policy.allow_external_adoption
                    ? "externally created partition adopted under policy"
                    : "externally created partition is recorded but not authoritative");
    if (state.policy.allow_external_adoption) {
      ++report.adopted;
      report.explanation.add(ExplanationCode::ExternalAdoption, record.id.str(),
                             "adopted after reconciliation");
    } else {
      report.revalidation_required = true;
    }
  }
  accelerator->evidence = observed.evidence;
  const Result<EvidenceGeneration> evidence_next = accelerator->evidence.generation.known()
                                                        ? accelerator->evidence.generation.next()
                                                        : Result<EvidenceGeneration>(
                                                              EvidenceGeneration::first());
  ensure_evidence(accelerator->evidence, accelerator->provenance, accelerator->backend,
                  state.policy.max_evidence_age_ms == 0 ? 30'000 : state.policy.max_evidence_age_ms,
                  now, evidence_next.ok() ? evidence_next.value() : EvidenceGeneration::first());
  report.explanation.add(ExplanationCode::ReconciledMatch, accelerator_id.str(),
                         "matched=" + std::to_string(report.matched) + " missing=" +
                             std::to_string(report.missing) + " unexpected=" +
                             std::to_string(report.unexpected));
  report.explanation.set_summary(report.summary());
  bump_state_generation(state);
  record_event(state, ExplanationCode::ReconciledMatch, accelerator_id.str(), report.summary());
  return report;
}

Result<ReconciliationReport> PartitionFabric::reconcile_with_backend(AcceleratorBackend& backend,
                                                                    std::string_view stable_key) {
  const std::string key(stable_key);
  AcceleratorId accelerator_id{};
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->state.key_index.find(key);
    if (it == impl_->state.key_index.end()) {
      return make_error(ErrorCode::NotFound, "accelerator key is not registered", key);
    }
    accelerator_id = it->second;
  }
  Result<BackendLayout> layout = backend.query_layout(key);
  if (!layout.ok()) {
    return make_error(ErrorCode::BackendFailure, "layout query failed", to_string(layout.error()));
  }
  return reconcile(accelerator_id, layout.value());
}

Result<ReconciliationReport> PartitionFabric::refresh_layout(AcceleratorBackend& backend,
                                                             std::string_view stable_key) {
  return reconcile_with_backend(backend, stable_key);
}

Result<AcceleratorId> PartitionFabric::accelerator_for_key(std::string_view stable_key) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->state.key_index.find(std::string(stable_key));
  if (it == impl_->state.key_index.end()) {
    return make_error(ErrorCode::NotFound, "accelerator key is not registered",
                      std::string(stable_key));
  }
  return it->second;
}

Result<ReconciliationReport> PartitionFabric::reconcile_with_backend_key(
    const BackendLayout& observed) {
  Result<AcceleratorId> accelerator = accelerator_for_key(observed.stable_key);
  if (!accelerator.ok()) {
    return accelerator.error();
  }
  return reconcile(accelerator.value(), observed);
}

Result<PartitionReservation> PartitionFabric::create_destruction_reservation(
    const PartitionId& partition) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const Status open = check_open(state);
  if (!open.ok()) {
    return open.error();
  }
  PartitionRecord* record = state.find_partition(partition);
  if (record == nullptr) {
    return make_error(ErrorCode::NotFound, "partition is not known", partition.str());
  }
  if (is_terminal_state(record->state)) {
    return make_error(ErrorCode::StalePartitionGeneration,
                      "partition generation is already retired",
                      std::string(partition.str()) + " is " + to_string(record->state));
  }
  if (record->native_identity.native_id.empty()) {
    return make_error(ErrorCode::RevalidationRequired,
                      "partition has no verified physical identity to destroy", partition.str());
  }
  if (!destruction_safe(state, *record)) {
    return make_error(ErrorCode::DrainRequired,
                      "destructive mutation is not safe while the partition is occupied",
                      partition.str());
  }
  AcceleratorRecord* accelerator = state.find_accelerator(record->accelerator);
  if (accelerator == nullptr) {
    return make_error(ErrorCode::NotFound, "partition accelerator is not registered");
  }
  if (accelerator->generation != record->accelerator_generation) {
    (void)transition_partition(state, *record, PartitionState::RevalidationRequired,
                               "device generation changed before destruction");
    return make_error(ErrorCode::StaleGeneration,
                      "device generation changed before destruction was registered");
  }
  const std::uint64_t now = impl_->now();
  return create_destructive_reservation_locked(state, *accelerator, {record}, ResourceVector{},
                                               false, now, nullptr);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

Result<DurableState> PartitionFabric::export_durable_state() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const FabricState& state = impl_->state;
  DurableState durable;
  durable.format_version = APF_PERSISTENCE_FORMAT_VERSION;
  durable.instance_id = state.instance_id;
  durable.state_generation = state.state_generation;
  durable.coordinator_epoch = state.coordinator_epoch;
  durable.policy_generation = state.policy_generation;
  durable.next_identity = state.ids.raw();
  durable.saved_at_ms = impl_->now();
  durable.policy = state.policy;
  for (const auto& entry : state.profiles) {
    durable.profiles.push_back(entry.second);
  }
  for (const auto& entry : state.accelerators) {
    durable.accelerators.push_back(entry.second);
    const auto key = state.stable_keys.find(entry.first);
    if (key != state.stable_keys.end()) {
      durable.accelerator_keys.emplace_back(entry.second.id, key->second);
    }
    const auto ledger = state.ledgers.find(entry.first);
    if (ledger != state.ledgers.end()) {
      durable.ledgers.emplace_back(entry.second.id, ledger->second);
    }
  }
  for (const auto& entry : state.partitions) {
    durable.partitions.push_back(entry.second);
  }
  for (const auto& entry : state.reservations) {
    durable.reservations.push_back(entry.second);
  }
  for (const auto& entry : state.attempts) {
    durable.attempts.push_back(entry.second);
  }
  for (const auto& entry : state.assignments) {
    durable.assignments.push_back(entry.second);
  }
  for (const auto& entry : state.workers) {
    durable.workers.push_back(entry.second);
  }
  return durable;
}

Result<ReconciliationReport> PartitionFabric::import_durable_state(const DurableState& durable) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  FabricState& state = impl_->state;
  const std::uint64_t now = impl_->now();

  // Structural validation before anything is applied: duplicate identities,
  // impossible totals, broken parent relationships and generation regressions
  // are rejected without partially applying the state.
  std::map<std::uint64_t, bool> accelerator_ids;
  for (const AcceleratorRecord& record : durable.accelerators) {
    if (!record.id.valid()) {
      return make_error(ErrorCode::PersistenceCorruption, "durable accelerator has no identity");
    }
    if (accelerator_ids.count(record.id.value()) != 0) {
      return make_error(ErrorCode::PersistenceCorruption, "duplicate accelerator identity",
                        record.id.str());
    }
    accelerator_ids[record.id.value()] = true;
  }
  std::map<std::uint64_t, bool> partition_ids;
  for (const PartitionRecord& record : durable.partitions) {
    if (!record.id.valid()) {
      return make_error(ErrorCode::PersistenceCorruption, "durable partition has no identity");
    }
    if (partition_ids.count(record.id.value()) != 0) {
      return make_error(ErrorCode::PersistenceCorruption, "duplicate partition identity",
                        record.id.str());
    }
    partition_ids[record.id.value()] = true;
    if (accelerator_ids.count(record.accelerator.value()) == 0) {
      return make_error(ErrorCode::PersistenceCorruption,
                        "durable partition references an unknown accelerator", record.id.str());
    }
    const Status valid = record.validate();
    if (!valid.ok()) {
      return make_error(ErrorCode::PersistenceCorruption, to_string(valid.error()),
                        record.id.str());
    }
  }
  for (const auto& entry : durable.ledgers) {
    if (accelerator_ids.count(entry.first.value()) == 0) {
      return make_error(ErrorCode::PersistenceCorruption,
                        "durable ledger references an unknown accelerator", entry.first.str());
    }
    const Status valid = entry.second.validate();
    if (!valid.ok()) {
      return make_error(ErrorCode::PersistenceCorruption, to_string(valid.error()),
                        entry.first.str());
    }
  }
  if (durable.state_generation < state.state_generation) {
    return make_error(ErrorCode::GenerationRegression,
                      "durable state generation is behind the live instance",
                      std::to_string(durable.state_generation.value()));
  }

  state.profiles.clear();
  state.accelerators.clear();
  state.ledgers.clear();
  state.stable_keys.clear();
  state.key_index.clear();
  state.partitions.clear();
  state.reservations.clear();
  state.attempts.clear();
  state.assignments.clear();
  state.workers.clear();

  state.policy = durable.policy;
  state.policy_generation =
      durable.policy_generation.known() ? durable.policy_generation : PolicyGeneration::first();
  state.policy.generation = state.policy_generation;
  for (const PartitionProfile& profile : durable.profiles) {
    state.profiles[profile.id.value()] = profile;
  }
  for (const AcceleratorRecord& record : durable.accelerators) {
    AcceleratorRecord restored = record;
    // Persisted dynamic evidence never becomes current merely because it
    // survived a restart.
    restored.evidence.observed = false;
    restored.evidence.ttl_ms = 0;
    restored.health.stamp.observed = false;
    restored.capability.evidence.observed = false;
    restored.capability.evidence.ttl_ms = 0;
    state.accelerators[restored.id.value()] = restored;
  }
  for (const auto& entry : durable.accelerator_keys) {
    state.stable_keys[entry.first.value()] = entry.second;
    state.key_index[entry.second] = entry.first;
  }
  for (const auto& entry : durable.ledgers) {
    state.ledgers[entry.first.value()] = entry.second;
  }
  const Status floor = state.ids.restore_floor(durable.next_identity);
  if (!floor.ok()) {
    return floor.error();
  }
  if (durable.coordinator_epoch > state.coordinator_epoch) {
    state.coordinator_epoch = durable.coordinator_epoch;
  }
  const Result<StateGeneration> next_generation = durable.state_generation.next();
  if (!next_generation.ok()) {
    return next_generation.error();
  }
  state.state_generation = next_generation.value();

  ReconciliationReport report;
  const Result<ReconciliationId> report_id = state.ids.allocate<ReconciliationId>();
  if (report_id.ok()) {
    report.id = report_id.value();
  }

  for (const PartitionRecord& record : durable.partitions) {
    PartitionRecord restored = record;
    // One partition generation occupies a logical partition identity, and
    // anything that had live physical authority must be revalidated.
    if (requires_revalidation_after_restart(restored.state) &&
        restored.state != PartitionState::Unpartitioned &&
        restored.state != PartitionState::RevalidationRequired) {
      restored.state = PartitionState::RevalidationRequired;
      restored.last_transition_at_ms = now;
      restored.last_transition_reason = "conservative revalidation after restart";
      report.revalidation_required = true;
      add_finding(report, ReconciliationFindingKind::GenerationUncorrelatable,
                  ReconciliationSeverity::Attention, restored.accelerator, restored.id,
                  restored.generation, restored.state, restored.native_identity.native_id,
                  "durable partition requires fresh physical evidence before it is authoritative");
    }
    state.partitions[restored.id.value()] = restored;
  }
  for (const PartitionReservation& reservation : durable.reservations) {
    PartitionReservation restored = reservation;
    if (!reservation_is_terminal(restored.lifecycle)) {
      if (restored.lifecycle == ReservationLifecycle::Active ||
          restored.lifecycle == ReservationLifecycle::Pending) {
        CapacityLedger* ledger = state.find_ledger(restored.accelerator);
        bool partitions_hold = false;
        for (const PartitionId partition_id : restored.partitions) {
          const PartitionRecord* partition = state.find_partition(partition_id);
          if (partition != nullptr && partition->held_bucket.has_value() &&
              partition->state != PartitionState::Reserved &&
              partition->state != PartitionState::Creating) {
            partitions_hold = true;
          }
        }
        if (ledger != nullptr && !partitions_hold) {
          const Status released = ledger->release_reserved(restored.resources);
          if (!released.ok()) {
            return make_error(ErrorCode::PersistenceCorruption,
                              "durable reservation cannot release capacity",
                              to_string(released.error()));
          }
          for (const PartitionId partition_id : restored.partitions) {
            PartitionRecord* partition = state.find_partition(partition_id);
            if (partition != nullptr && partition->held_bucket.has_value()) {
              partition->held_bucket.reset();
              partition->state = PartitionState::Retired;
              partition->last_transition_at_ms = now;
              partition->last_transition_reason = "reservation invalidated by restart";
            }
          }
        }
      }
      restored.lifecycle = ReservationLifecycle::Fenced;
      restored.reason = "restart invalidated live reservation authority";
      restored.updated_at_ms = now;
      report.reconciliation_required = true;
    }
    state.reservations[restored.id.value()] = restored;
  }
  for (const PartitionAttempt& attempt : durable.attempts) {
    PartitionAttempt restored = attempt;
    if (!attempt_settled(restored.state)) {
      restored.state = AttemptState::OutcomeUnknown;
      restored.settled_at_ms = now;
      restored.detail = "restart invalidated in-flight attempt authority; reconciliation required";
      report.reconciliation_required = true;
      add_finding(report, ReconciliationFindingKind::GenerationUncorrelatable,
                  ReconciliationSeverity::Critical, restored.accelerator, PartitionId{},
                  PartitionGeneration{}, PartitionState::RevalidationRequired, {},
                  "in-flight physical attempt classified as outcome unknown after restart");
    }
    state.attempts[restored.id.value()] = restored;
  }
  for (const PartitionAssignment& assignment : durable.assignments) {
    const PartitionRecord* partition = state.find_partition(assignment.partition);
    if (partition == nullptr || partition->state != PartitionState::Active) {
      report.reconciliation_required = true;
      add_finding(report, ReconciliationFindingKind::AssignmentOrphaned,
                  ReconciliationSeverity::Critical, assignment.accelerator, assignment.partition,
                  assignment.partition_generation, PartitionState::RevalidationRequired, {},
                  "assignment dropped because its partition authority is not current after "
                  "restart");
      continue;
    }
    state.assignments[assignment.id.value()] = assignment;
  }
  for (const WorkerRecord& worker : durable.workers) {
    WorkerRecord restored = worker;
    restored.alive = false;
    restored.fenced = true;
    restored.fence_reason = "process incarnation does not survive a restart";
    restored.fenced_at_ms = now;
    state.workers[restored.id.value()] = restored;
  }
  report.explanation.set_summary(report.summary());
  bump_state_generation(state);
  record_event(state, ExplanationCode::RevalidationRequired, state.instance_id,
               "durable state imported: dynamic authority requires revalidation");
  return report;
}

Status PartitionFabric::save(PersistenceStore& store) {
  Result<DurableState> durable = export_durable_state();
  if (!durable.ok()) {
    return durable.error();
  }
  return store.save(durable.value(), impl_->state.limits);
}

Status PartitionFabric::load(PersistenceStore& store) {
  const Limits limits = impl_->state.limits;
  Result<DurableState> durable = store.load(limits);
  if (!durable.ok()) {
    if (durable.error().code == ErrorCode::NotFound) {
      return success();
    }
    return durable.error();
  }
  Result<ReconciliationReport> report = import_durable_state(durable.value());
  if (!report.ok()) {
    return report.error();
  }
  return success();
}

}  // namespace apf
