#include "fabric_internal.hpp"

#include "apf/codec.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace apf {
namespace {

/// Checked per-dimension multiplication of a resource vector.
///
/// A *demand* is not a capacity: three partitions of a profile that owns 3/7 of
/// the device require 9/7 of it, which is a meaningful request that simply
/// cannot be satisfied. The share-scale clamp that protects capacity arithmetic
/// therefore does not apply here; overflow still does.
Status multiply_resource(const ResourceVector& value, std::uint32_t count, ResourceVector& out) {
  out = ResourceVector{};
  if (count == 0) {
    return success();
  }
  std::uint32_t mask = 0;
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const auto dimension = static_cast<ResourceDimension>(index);
    if (!value.has(dimension)) {
      continue;
    }
    const std::uint64_t amount = value.get(dimension);
    if (amount != 0 && count != 0 && amount > UINT64_MAX / count) {
      return failure(ErrorCode::Overflow, "requested demand overflows a resource dimension",
                     to_string(dimension));
    }
    out.mutable_raw()[index] = amount * count;
    mask |= 1u << static_cast<std::uint32_t>(index);
  }
  out.set_mask(mask);
  return success();
}

/// Largest number of instances of a per-instance vector that fits in a pool.
/// Dimensions present in the requirement but absent from the pool are treated
/// as unsatisfiable rather than as zero.
std::uint32_t instances_fitting(const ResourceVector& pool, const ResourceVector& per_instance) {
  std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
  bool constrained = false;
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const auto dimension = static_cast<ResourceDimension>(index);
    if (!per_instance.has(dimension)) {
      continue;
    }
    const std::uint64_t need = per_instance.get(dimension);
    if (need == 0) {
      continue;
    }
    if (!pool.has(dimension)) {
      return 0;
    }
    const std::uint64_t available = pool.get(dimension);
    const std::uint64_t fits = available / need;
    constrained = true;
    if (fits > std::numeric_limits<std::uint32_t>::max()) {
      continue;
    }
    best = std::min(best, static_cast<std::uint32_t>(fits));
  }
  if (!constrained) {
    return 0;
  }
  return best;
}

/// Integer capacity waste, expressed in whole GiB and whole percent so that the
/// ranking never depends on floating point.
std::int64_t capacity_waste_units(const ResourceVector& free) {
  std::int64_t units = 0;
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const auto dimension = static_cast<ResourceDimension>(index);
    if (!free.has(dimension)) {
      continue;
    }
    const std::uint64_t value = free.get(dimension);
    if (is_share_dimension(dimension)) {
      units += static_cast<std::int64_t>(value / 10'000ull);
    } else {
      units += static_cast<std::int64_t>(value / (1ull << 30));
    }
  }
  return units;
}

std::int64_t health_penalty(HealthState state) noexcept {
  switch (state) {
    case HealthState::Healthy: return 0;
    case HealthState::Recovering: return 1;
    case HealthState::Degraded: return 2;
    case HealthState::Unknown: return 3;
    case HealthState::Unhealthy: return 4;
  }
  return 3;
}

PlanOutcome outcome_for_classification(FragmentationClass classification) {
  switch (classification) {
    case FragmentationClass::None:
      return PlanOutcome::FeasibleNow;
    case FragmentationClass::ProfileShape:
    case FragmentationClass::SliceNotContiguous:
    case FragmentationClass::EngineGroup:
    case FragmentationClass::MemorySegment:
    case FragmentationClass::IncompatibleCombination:
    case FragmentationClass::StrandedCapacity:
      return PlanOutcome::Fragmented;
    case FragmentationClass::MaxPartitionCount:
      return PlanOutcome::MaxPartitionCountReached;
    case FragmentationClass::TrappedBehindActive:
      return PlanOutcome::InsufficientCapacity;
    case FragmentationClass::ReconfigurationRequired:
      return PlanOutcome::PhysicallyImpossible;
    case FragmentationClass::ExceedsDeviceCapacity:
      return PlanOutcome::PhysicallyImpossible;
    default:
      return PlanOutcome::Fragmented;
  }
}

/// Severity order used to pick the deterministic headline failure when no
/// candidate is eligible. Lower is more specific and therefore reported first.
int failure_severity(PlanOutcome outcome) {
  switch (outcome) {
    case PlanOutcome::InvalidRequest: return 0;
    case PlanOutcome::LimitExceeded: return 1;
    case PlanOutcome::DuplicateRequest: return 2;
    case PlanOutcome::CapabilityUnsupported: return 3;
    case PlanOutcome::CapabilityUnknown: return 4;
    case PlanOutcome::PolicyRejected: return 5;
    case PlanOutcome::IsolationUnsatisfied: return 6;
    case PlanOutcome::StaleEvidence: return 7;
    case PlanOutcome::RevalidationRequired: return 8;
    case PlanOutcome::MaxPartitionCountReached: return 9;
    case PlanOutcome::Fragmented: return 10;
    case PlanOutcome::InsufficientCapacity: return 11;
    case PlanOutcome::PhysicallyImpossible: return 12;
    case PlanOutcome::NoEligibleAccelerator: return 13;
    default: return 14;
  }
}

ExplanationCode rejection_code_for(PlanOutcome outcome) {
  switch (outcome) {
    case PlanOutcome::CapabilityUnsupported: return ExplanationCode::RejectedCapabilityUnsupported;
    case PlanOutcome::CapabilityUnknown: return ExplanationCode::RejectedCapabilityUnknown;
    case PlanOutcome::PolicyRejected: return ExplanationCode::RejectedPolicy;
    case PlanOutcome::IsolationUnsatisfied: return ExplanationCode::RejectedIsolation;
    case PlanOutcome::StaleEvidence: return ExplanationCode::RejectedStaleEvidence;
    case PlanOutcome::RevalidationRequired: return ExplanationCode::RejectedRevalidationRequired;
    case PlanOutcome::MaxPartitionCountReached: return ExplanationCode::RejectedMaxPartitionCount;
    case PlanOutcome::Fragmented: return ExplanationCode::RejectedFragmentedGeometry;
    case PlanOutcome::InsufficientCapacity: return ExplanationCode::RejectedInsufficientFreeCapacity;
    case PlanOutcome::PhysicallyImpossible: return ExplanationCode::PlanPhysicallyImpossible;
    default: return ExplanationCode::CandidateRejected;
  }
}

bool profile_conflicts_with_resident(const FabricState& state, const AcceleratorRecord& accelerator,
                                     const PartitionProfile& profile) {
  for (const auto& entry : state.partitions) {
    const PartitionRecord& record = entry.second;
    if (record.accelerator != accelerator.id) {
      continue;
    }
    if (is_terminal_state(record.state)) {
      continue;
    }
    if (record.profile == profile.id) {
      continue;
    }
    const PartitionProfile* resident = state.find_profile(record.profile);
    if (resident == nullptr) {
      continue;
    }
    if (profile.conflicts_with(*resident)) {
      return true;
    }
  }
  return false;
}

std::uint32_t resident_instances(const FabricState& state, const AcceleratorId& accelerator,
                                 const PartitionProfileId& profile) {
  std::uint32_t total = 0;
  for (const auto& entry : state.partitions) {
    const PartitionRecord& record = entry.second;
    if (record.accelerator != accelerator || record.profile != profile) {
      continue;
    }
    if (is_terminal_state(record.state) || record.state == PartitionState::Unpartitioned) {
      continue;
    }
    ++total;
  }
  return total;
}

std::uint32_t used_slices(const FabricState& state, const AcceleratorId& accelerator,
                          bool compute) {
  std::uint32_t total = 0;
  for (const auto& entry : state.partitions) {
    const PartitionRecord& record = entry.second;
    if (record.accelerator != accelerator || is_terminal_state(record.state)) {
      continue;
    }
    const PartitionProfile* profile = state.find_profile(record.profile);
    if (profile == nullptr) {
      continue;
    }
    total += compute ? profile->compute_slice_count : profile->memory_slice_count;
  }
  return total;
}

bool any_profile_fits(const FabricState& state, const AcceleratorRecord& accelerator,
                      std::uint32_t slots_free, const ResourceVector& pool) {
  for (const PartitionProfileId id : accelerator.capability.supported_profiles) {
    const PartitionProfile* profile = state.find_profile(id);
    if (profile == nullptr) {
      continue;
    }
    if (slots_free == 0) {
      return false;
    }
    if (!profile->resources.is_subset_of(pool)) {
      continue;
    }
    if (profile_conflicts_with_resident(state, accelerator, *profile)) {
      continue;
    }
    return true;
  }
  return false;
}

}  // namespace

FragmentationReport analyze_fragmentation_impl(const FabricState& state,
                                               const AcceleratorRecord& accelerator,
                                               const CapacityLedger& ledger,
                                               const PartitionRequest& request,
                                               std::uint64_t now_ms) {
  FragmentationReport report;
  report.accelerator = accelerator.id;
  report.accelerator_generation = accelerator.generation;
  report.capability_generation = accelerator.capability.generation;
  report.profile = request.profile;
  report.requested_count = request.count;
  report.free_capacity = ledger.bucket(CapacityBucket::Free);

  const PartitionProfile* profile = state.find_profile(request.profile);
  if (profile == nullptr) {
    report.classification = FragmentationClass::ProfileShape;
    report.blockers.push_back("profile-unknown");
    report.explanation.add(ExplanationCode::RejectedCapabilityUnknown, accelerator.id.str(),
                           "requested profile is not registered");
    report.explanation.set_summary("requested partition profile is not registered");
    return report;
  }
  report.profile_supported = accelerator.capability.supports_profile(profile->id);

  // Aggregated capacity already committed to partitions on this device.
  ResourceVector committed;
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const auto dimension = static_cast<ResourceDimension>(index);
    if (!ledger.total().has(dimension)) {
      continue;
    }
    const std::uint64_t value = ledger.bucket(CapacityBucket::Active).get(dimension) +
                                ledger.bucket(CapacityBucket::Draining).get(dimension) +
                                ledger.bucket(CapacityBucket::ReconfigurationHeld).get(dimension);
    const Status status = committed.set(dimension, value);
    if (!status.ok()) {
      report.blockers.push_back("committed-capacity-overflow");
    }
  }
  report.capacity_trapped_behind_active = committed;

  ResourceVector needed;
  const Status multiply = multiply_resource(profile->resources, request.count, needed);
  if (!multiply.ok()) {
    report.classification = FragmentationClass::ProfileShape;
    report.blockers.push_back("request-magnitude-overflow");
    report.explanation.add(ExplanationCode::RejectedInsufficientFreeCapacity, accelerator.id.str(),
                           "requested partition count overflows representable capacity");
    return report;
  }

  const std::uint32_t max_partitions = accelerator.capability.max_partition_count;
  const std::uint32_t used_partitions =
      static_cast<std::uint32_t>(state.count_partitions_in_states(
          accelerator.id, {PartitionState::Reserved, PartitionState::Creating,
                           PartitionState::Active, PartitionState::Draining,
                           PartitionState::ReconfigurationRequired, PartitionState::Degraded,
                           PartitionState::RevalidationRequired, PartitionState::Destroying,
                           PartitionState::Offline}));
  report.partition_slots_free =
      max_partitions > used_partitions ? max_partitions - used_partitions : 0;

  const std::uint32_t same_profile = resident_instances(state, accelerator.id, profile->id);
  std::uint32_t multiplicity_cap = std::numeric_limits<std::uint32_t>::max();
  if (profile->max_multiplicity > 0) {
    multiplicity_cap = profile->max_multiplicity > same_profile
                           ? profile->max_multiplicity - same_profile
                           : 0;
  }
  if (const ProfileCombinationRule* rule = accelerator.capability.find_rule(profile->id)) {
    if (rule->max_instances > 0) {
      const std::uint32_t room =
          rule->max_instances > same_profile ? rule->max_instances - same_profile : 0;
      multiplicity_cap = std::min(multiplicity_cap, room);
    }
    if (rule->requires_exclusive_device && used_partitions > 0) {
      multiplicity_cap = 0;
      report.blockers.push_back("profile-requires-exclusive-device");
    }
  }
  if (state.policy.max_instances_per_profile > 0) {
    const std::uint32_t room = state.policy.max_instances_per_profile > same_profile
                                   ? state.policy.max_instances_per_profile - same_profile
                                   : 0;
    multiplicity_cap = std::min(multiplicity_cap, room);
  }

  const bool conflict = profile_conflicts_with_resident(state, accelerator, *profile);
  if (conflict) {
    report.blockers.push_back("incompatible-profile-combination");
  }

  std::uint32_t by_capacity = instances_fitting(report.free_capacity, profile->resources);
  by_capacity = std::min(by_capacity, report.partition_slots_free);
  by_capacity = std::min(by_capacity, multiplicity_cap);

  // Slice-shape feasibility, only when the backend published device geometry.
  bool slice_limited = false;
  if (accelerator.capability.total_compute_slices > 0 || accelerator.capability.total_memory_slices > 0) {
    if (accelerator.capability.total_compute_slices > 0) {
      const std::uint32_t used = used_slices(state, accelerator.id, true);
      const std::uint32_t free_slices =
          accelerator.capability.total_compute_slices > used
              ? accelerator.capability.total_compute_slices - used
              : 0;
      if (profile->compute_slice_count > 0) {
        const std::uint32_t fits = free_slices / profile->compute_slice_count;
        if (fits < by_capacity) {
          slice_limited = true;
        }
        by_capacity = std::min(by_capacity, fits);
      }
    }
    if (accelerator.capability.total_memory_slices > 0) {
      const std::uint32_t used = used_slices(state, accelerator.id, false);
      const std::uint32_t free_slices =
          accelerator.capability.total_memory_slices > used
              ? accelerator.capability.total_memory_slices - used
              : 0;
      if (profile->memory_slice_count > 0) {
        const std::uint32_t fits = free_slices / profile->memory_slice_count;
        if (fits < by_capacity) {
          slice_limited = true;
        }
        by_capacity = std::min(by_capacity, fits);
      }
    }
  }
  report.max_instances_now = conflict ? 0 : by_capacity;

  const bool aggregate_ok = needed.is_subset_of(report.free_capacity);
  report.aggregate_capacity_sufficient = aggregate_ok;
  report.feasible_now = !conflict && report.max_instances_now >= request.count;

  if (report.feasible_now) {
    report.classification = FragmentationClass::None;
    report.explanation.add(ExplanationCode::PlanFeasibleNow, accelerator.id.str(),
                           "free geometry satisfies the request", static_cast<std::int64_t>(request.count));
  } else if (report.partition_slots_free < request.count) {
    report.classification = FragmentationClass::MaxPartitionCount;
    report.explanation.add(ExplanationCode::RejectedMaxPartitionCount, accelerator.id.str(),
                           "device partition slots are exhausted",
                           static_cast<std::int64_t>(report.partition_slots_free));
  } else if (conflict) {
    report.classification = FragmentationClass::IncompatibleCombination;
    report.explanation.add(ExplanationCode::RejectedIncompatibleProfileCombination,
                           accelerator.id.str(),
                           "a resident partition profile is mutually exclusive with the request");
  } else if (aggregate_ok) {
    report.classification = slice_limited ? FragmentationClass::ProfileShape
                                          : FragmentationClass::ProfileShape;
    report.explanation.add(ExplanationCode::RejectedFragmentedGeometry, accelerator.id.str(),
                           slice_limited
                               ? "aggregate capacity fits but slice geometry cannot form the profile"
                               : "aggregate capacity fits but the requested profile shape cannot be "
                                 "assembled",
                           static_cast<std::int64_t>(by_capacity));
  } else {
    // Aggregate capacity is insufficient now. Determine whether drain or a
    // destructive reconfiguration could recover enough.
    ResourceVector recovered = report.free_capacity;
    Status status = recovered.add_assign(ledger.bucket(CapacityBucket::Draining));
    if (status.ok()) {
      status = recovered.add_assign(ledger.bucket(CapacityBucket::ReconfigurationHeld));
    }
    const bool drain_helps = status.ok() && needed.is_subset_of(recovered);
    report.feasible_after_drain = drain_helps;
    report.max_instances_after_drain = instances_fitting(recovered, profile->resources);
    report.max_instances_after_drain = std::min(report.max_instances_after_drain,
                                                report.partition_slots_free);

    ResourceVector whole = ledger.total();
    const Status whole_status = whole.sub_assign(ledger.bucket(CapacityBucket::Unavailable));
    const bool whole_fits = whole_status.ok() && needed.is_subset_of(whole);
    if (!whole_fits) {
      // The request exceeds what the device can ever hold, which is not a
      // fragmentation statement at all.
      report.classification = FragmentationClass::ExceedsDeviceCapacity;
      report.destructive_reconfiguration_required = false;
      report.reconfiguration_required = false;
      report.reconfiguration_can_help = false;
      report.explanation.add(ExplanationCode::PlanPhysicallyImpossible, accelerator.id.str(),
                             "requested demand exceeds the physical device capacity: " +
                                 needed.format());
      report.explanation.set_summary(report.summary());
      report.explanation.canonicalize(state.limits.max_explanations);
      return report;
    }
    report.destructive_reconfiguration_required = true;
    report.reconfiguration_required = true;
    report.reconfiguration_can_help = whole_fits && !drain_helps;

    if (whole_fits) {
      report.classification = FragmentationClass::TrappedBehindActive;
      report.explanation.add(ExplanationCode::RejectedInsufficientFreeCapacity, accelerator.id.str(),
                             "capacity exists but is held by active partitions", 0);
      if (drain_helps) {
        report.explanation.add(ExplanationCode::PlanFeasibleAfterDrain, accelerator.id.str(),
                               "draining partitions would release enough capacity");
      } else {
        report.explanation.add(ExplanationCode::RejectedDestructiveReconfiguration,
                               accelerator.id.str(),
                               "only a destructive device reconfiguration would release capacity");
      }
    } else {
      report.classification = FragmentationClass::StrandedCapacity;
      report.explanation.add(ExplanationCode::PlanPhysicallyImpossible, accelerator.id.str(),
                             "requested geometry exceeds the physical device capacity");
    }
  }

  // Capacity that no legal profile could ever consume.
  const std::uint32_t slots_free = report.partition_slots_free;
  if (!report.free_capacity.empty() &&
      !any_profile_fits(state, accelerator, slots_free, report.free_capacity)) {
    report.stranded_capacity = report.free_capacity;
    if (!report.feasible_now && report.classification == FragmentationClass::None) {
      report.classification = FragmentationClass::StrandedCapacity;
    }
    report.explanation.add(ExplanationCode::RejectedFragmentedGeometry, accelerator.id.str(),
                           "free capacity cannot form any supported profile");
  }

  ResourceVector recoverable;
  Status recoverable_status = success();
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const auto dimension = static_cast<ResourceDimension>(index);
    if (!ledger.total().has(dimension)) {
      continue;
    }
    if (!ledger.bucket(CapacityBucket::Active).has(dimension) &&
        !ledger.bucket(CapacityBucket::Draining).has(dimension)) {
      continue;
    }
    const std::uint64_t value = ledger.bucket(CapacityBucket::Active).get(dimension) +
                                ledger.bucket(CapacityBucket::Draining).get(dimension);
    recoverable_status = recoverable.set(dimension, value);
    if (!recoverable_status.ok()) {
      break;
    }
  }
  if (recoverable_status.ok()) {
    report.capacity_recoverable_by_reconfiguration = recoverable;
  }

  report.blocking_active_partitions = static_cast<std::uint32_t>(state.count_partitions_in_states(
      accelerator.id, {PartitionState::Active, PartitionState::Draining}));
  for (const auto& entry : state.partitions) {
    const PartitionRecord& record = entry.second;
    if (record.accelerator != accelerator.id) {
      continue;
    }
    const bool blocks_drain =
        record.state == PartitionState::Active || record.state == PartitionState::Degraded;
    if (blocks_drain) {
      report.partitions_to_drain.push_back(record.id);
    }
  }
  std::sort(report.partitions_to_drain.begin(), report.partitions_to_drain.end());

  report.estimated_drain_ms = accelerator.capability.drain_estimate_ms;
  report.estimated_reconfiguration_downtime_ms =
      accelerator.capability.reconfiguration_estimate_ms;
  report.estimated_partition_mutations =
      static_cast<std::uint32_t>(report.partitions_to_drain.size() + request.count);

  report.explanation.add(ExplanationCode::RankingFactor, accelerator.id.str(),
                         "free=" + report.free_capacity.format());
  report.explanation.set_summary(report.summary());
  report.explanation.canonicalize(state.limits.max_explanations);
  (void)now_ms;
  return report;
}

PartitionPlan build_plan(FabricState& state, const PartitionRequest& request,
                         std::uint64_t now_ms) {
  PartitionPlan plan;
  plan.request = request;
  plan.request_digest = request_digest(request);
  plan.created_at_ms = now_ms;
  plan.binding.policy_generation = state.policy_generation;
  plan.binding.coordinator_epoch = state.coordinator_epoch;
  plan.binding.state_generation = state.state_generation;
  plan.binding.planned_at_ms = now_ms;
  const std::uint64_t window = request.max_evidence_age_ms != 0 ? request.max_evidence_age_ms
                                                                : state.policy.max_evidence_age_ms;
  plan.binding.valid_until_ms = window == 0 ? 0 : now_ms + window;

  const Status request_status = request.validate(state.limits);
  if (!request_status.ok()) {
    plan.outcome = request_status.error().code == ErrorCode::LimitExceeded
                       ? PlanOutcome::LimitExceeded
                       : PlanOutcome::InvalidRequest;
    plan.explanation.add(ExplanationCode::CandidateRejected, "request",
                         to_string(request_status.error()));
    plan.explanation.set_summary(plan.outcome == PlanOutcome::InvalidRequest
                                     ? "request is not a valid planning request"
                                     : "request exceeds configured limits");
    plan.explanation.canonicalize(state.limits.max_explanations);
    return plan;
  }

  const PartitionProfile* requested_profile = state.find_profile(request.profile);
  if (requested_profile == nullptr) {
    plan.outcome = PlanOutcome::CapabilityUnknown;
    plan.explanation.add(ExplanationCode::RejectedCapabilityUnknown, "request",
                         "requested profile is not registered");
    plan.explanation.set_summary("requested partition profile is not registered");
    plan.explanation.canonicalize(state.limits.max_explanations);
    return plan;
  }

  const bool need_fresh = state.policy.require_fresh_evidence || request.max_evidence_age_ms != 0;

  int best_severity = std::numeric_limits<int>::max();
  PlanOutcome best_failure = PlanOutcome::NoEligibleAccelerator;
  AcceleratorId best_failure_accelerator{};
  bool have_eligible = false;
  std::int64_t best_eligible_score = 0;
  AcceleratorId best_eligible_accelerator{};

  for (auto& entry : state.accelerators) {
    const AcceleratorRecord& accelerator = entry.second;
    CandidateEvaluation candidate;
    candidate.accelerator = accelerator.id;
    candidate.accelerator_generation = accelerator.generation;

    const auto reject = [&](PlanOutcome outcome, ExplanationCode code, const std::string& detail) {
      candidate.eligible = false;
      candidate.outcome = outcome;
      candidate.explanation.add(code, accelerator.id.str(), detail);
      candidate.fragmentation.accelerator = accelerator.id;
      candidate.fragmentation.accelerator_generation = accelerator.generation;
      candidate.fragmentation.profile = request.profile;
      candidate.fragmentation.requested_count = request.count;
      candidate.fragmentation.explanation.add(code, accelerator.id.str(), detail);
      candidate.fragmentation.explanation.set_summary(detail);
      candidate.explanation.set_summary(detail);
      candidate.explanation.canonicalize(state.limits.max_explanations);
    };

    if (!request.selector.admits(accelerator.id)) {
      reject(PlanOutcome::PolicyRejected, ExplanationCode::CandidateRejected,
             "accelerator is excluded by the caller's device selector");
    } else if (!request.selector.backend.empty() && request.selector.backend != accelerator.backend) {
      reject(PlanOutcome::PolicyRejected, ExplanationCode::RejectedBackendMismatch,
             "accelerator backend does not match the selector");
    } else if (!request.selector.vendor.empty() &&
               request.selector.vendor != accelerator.identifiers.vendor) {
      reject(PlanOutcome::PolicyRejected, ExplanationCode::RejectedDeviceFamily,
             "accelerator vendor does not match the selector");
    } else if (!request.selector.device_family.empty() &&
               request.selector.device_family != accelerator.identifiers.model) {
      reject(PlanOutcome::PolicyRejected, ExplanationCode::RejectedDeviceFamily,
             "accelerator model does not match the selector");
    } else if (!request.selector.locality_domain.empty() &&
               request.selector.locality_domain != accelerator.locality.name) {
      reject(PlanOutcome::PolicyRejected, ExplanationCode::RejectedLocality,
             "accelerator locality domain does not match the selector");
    } else if (!accelerator.evidence.is_observed()) {
      reject(PlanOutcome::RevalidationRequired, ExplanationCode::RejectedRevalidationRequired,
             "accelerator has no physical evidence in this runtime generation");
    } else if (need_fresh && !accelerator.evidence.is_fresh_at(now_ms)) {
      reject(PlanOutcome::StaleEvidence, ExplanationCode::RejectedStaleEvidence,
             std::string("accelerator evidence is ") +
                 to_string(accelerator.evidence.freshness_at(now_ms)));
    } else if (!accelerator.is_governable_at(now_ms, need_fresh)) {
      reject(PlanOutcome::PolicyRejected, ExplanationCode::RejectedHealth,
             "accelerator is not governable under current health or readiness evidence");
    } else if (accelerator.capability.support == PartitionSupportState::Unknown ||
               !accelerator.capability.generation.known()) {
      reject(PlanOutcome::CapabilityUnknown, ExplanationCode::RejectedCapabilityUnknown,
             "partition capability has not been published for this device generation");
    } else if (!accelerator.capability.declares_supported()) {
      reject(PlanOutcome::CapabilityUnsupported, ExplanationCode::RejectedCapabilityUnsupported,
             accelerator.capability.unsupported_reason.empty()
                 ? std::string("device does not support partitioning")
                 : accelerator.capability.unsupported_reason);
    } else if (!accelerator.capability.supports_profile(request.profile)) {
      reject(PlanOutcome::CapabilityUnsupported, ExplanationCode::RejectedCapabilityUnsupported,
             "device capability does not publish the requested profile");
    } else if (!requested_profile->backend.empty() &&
               requested_profile->backend != accelerator.backend) {
      reject(PlanOutcome::PolicyRejected, ExplanationCode::RejectedBackendMismatch,
             "profile belongs to a different backend than the accelerator");
    } else if (!request.isolation.required.is_subset_of(accelerator.capability.isolation)) {
      reject(PlanOutcome::IsolationUnsatisfied, ExplanationCode::RejectedIsolation,
             "device cannot provide the required isolation properties");
    } else if (state.policy.require_healthy_device && accelerator.health.stamp.is_observed() &&
               accelerator.health.usable_at(now_ms) &&
               accelerator.health.state != HealthState::Healthy) {
      reject(PlanOutcome::PolicyRejected, ExplanationCode::RejectedHealth,
             "policy requires a healthy device");
    } else {
      const CapacityLedger* ledger = state.find_ledger(accelerator.id);
      if (ledger == nullptr) {
        reject(PlanOutcome::RevalidationRequired, ExplanationCode::RejectedRevalidationRequired,
               "accelerator has no capacity ledger");
      } else {
        candidate.fragmentation =
            analyze_fragmentation_impl(state, accelerator, *ledger, request, now_ms);
        candidate.eligible = candidate.fragmentation.feasible_now ||
                             candidate.fragmentation.feasible_after_drain ||
                             candidate.fragmentation.reconfiguration_can_help;
        if (candidate.fragmentation.feasible_after_reconfiguration) {
          candidate.eligible = true;
        }
        if (candidate.eligible) {
          if (candidate.fragmentation.feasible_now) {
            candidate.outcome = PlanOutcome::FeasibleNow;
          } else if (candidate.fragmentation.feasible_after_drain &&
                     state.policy.allow_drain && request.allow_drain) {
            candidate.outcome = PlanOutcome::FeasibleAfterDrain;
          } else if (candidate.fragmentation.reconfiguration_can_help &&
                     state.policy.allow_destructive_reconfiguration &&
                     request.allow_destructive_reconfiguration) {
            candidate.outcome = PlanOutcome::FeasibleAfterReconfiguration;
          } else if (candidate.fragmentation.feasible_after_drain && !state.policy.allow_drain) {
            candidate.outcome = PlanOutcome::PolicyRejected;
            candidate.eligible = false;
            candidate.explanation.add(ExplanationCode::RejectedDrainRequired, accelerator.id.str(),
                                      "request requires drain but policy forbids draining");
          } else {
            candidate.outcome = PlanOutcome::PolicyRejected;
            candidate.eligible = false;
            candidate.explanation.add(ExplanationCode::RejectedDestructiveReconfiguration,
                                      accelerator.id.str(),
                                      "request requires destructive reconfiguration which policy "
                                      "does not permit");
          }
        } else {
          candidate.outcome = outcome_for_classification(candidate.fragmentation.classification);
          candidate.explanation.append(candidate.fragmentation.explanation);
        }
      }
    }

    if (candidate.eligible) {
      // Hard constraints have all passed: only now does ranking run.
      candidate.explanation.add(ExplanationCode::CandidateEligible, accelerator.id.str(),
                                to_string(candidate.outcome));
      const CapacityLedger* ledger = state.find_ledger(accelerator.id);
      ResourceVector projected = ledger != nullptr ? ledger->bucket(CapacityBucket::Free)
                                                   : ResourceVector{};
      ResourceVector needed;
      if (multiply_resource(requested_profile->resources, request.count, needed).ok()) {
        const Status subtraction = projected.sub_assign(needed);
        if (!subtraction.ok()) {
          projected = ResourceVector{};
        }
      } else {
        projected = ResourceVector{};
      }

      const std::int64_t waste = capacity_waste_units(projected);
      const bool stranded_after =
          !projected.empty() &&
          !any_profile_fits(state, accelerator,
                            candidate.fragmentation.partition_slots_free > request.count
                                ? candidate.fragmentation.partition_slots_free - request.count
                                : 0,
                            projected);
      std::uint32_t optionality = 0;
      for (const PartitionProfileId id : accelerator.capability.supported_profiles) {
        const PartitionProfile* candidate_profile = state.find_profile(id);
        if (candidate_profile == nullptr) {
          continue;
        }
        optionality = std::max(
            optionality, instances_fitting(projected, candidate_profile->resources));
      }
      const bool consolidate_locality = request.selector.locality_domain.empty() &&
                                        !accelerator.locality.name.empty() &&
                                        candidate.fragmentation.partitions_to_drain.empty() &&
                                        resident_instances(state, accelerator.id, request.profile) > 0;
      const std::int64_t locality_penalty = consolidate_locality ? 0 : 1;

      const std::int64_t destructive =
          candidate.outcome == PlanOutcome::FeasibleAfterReconfiguration ? 1 : 0;
      const std::int64_t drain_term = candidate.outcome == PlanOutcome::FeasibleAfterDrain ? 1 : 0;
      const std::int64_t mutations =
          static_cast<std::int64_t>(request.count) +
          (destructive != 0
               ? static_cast<std::int64_t>(candidate.fragmentation.estimated_partition_mutations)
               : 0);

      const struct {
        ExplanationCode code;
        const char* subject;
        std::int64_t weight;
        std::int64_t raw;
      } terms[] = {
          {ExplanationCode::RankingFactor, "capacity_waste", state.policy.weights.capacity_waste,
           waste},
          {ExplanationCode::RankingFactor, "stranded_remainder",
           state.policy.weights.fragmentation, stranded_after ? 1 : 0},
          {ExplanationCode::RankingFactor, "destructive_reconfiguration",
           state.policy.weights.destructive_reconfiguration, destructive},
          {ExplanationCode::RankingFactor, "drain", state.policy.weights.drain, drain_term},
          {ExplanationCode::RankingFactor, "mutation_count",
           state.policy.weights.mutation_count, mutations},
          {ExplanationCode::RankingFactor, "health", state.policy.weights.health,
           health_penalty(accelerator.health.state)},
          {ExplanationCode::RankingFactor, "locality", state.policy.weights.locality,
           locality_penalty},
          {ExplanationCode::RankingFactor, "future_optionality",
           state.policy.weights.future_optionality, -static_cast<std::int64_t>(optionality)},
          {ExplanationCode::RankingFactor, "reconfiguration_cost",
           state.policy.weights.reconfiguration_cost,
           destructive != 0
               ? static_cast<std::int64_t>(candidate.fragmentation.estimated_reconfiguration_downtime_ms /
                                           1000ull)
               : 0},
          {ExplanationCode::RankingFactor, "live_reconfiguration",
           state.policy.weights.live_reconfiguration_bonus,
           (destructive != 0 && accelerator.capability.live_reconfiguration_supported) ? 1 : 0},
      };
      for (const auto& term : terms) {
        PlanScoreTerm score_term;
        score_term.code = term.code;
        score_term.subject = term.subject;
        score_term.weight = term.weight;
        score_term.raw = term.raw;
        score_term.contribution = term.weight * term.raw;
        candidate.score.total += score_term.contribution;
        candidate.score.terms.push_back(std::move(score_term));
        candidate.explanation.add(ExplanationCode::RankingFactor,
                                  std::string("score.") + term.subject,
                                  "weight=" + std::to_string(term.weight) +
                                      " raw=" + std::to_string(term.raw),
                                  term.weight * term.raw);
      }
      candidate.explanation.canonicalize(state.limits.max_explanations);
    } else {
      const int severity = failure_severity(candidate.outcome);
      if (severity < best_severity) {
        best_severity = severity;
        best_failure = candidate.outcome;
        best_failure_accelerator = accelerator.id;
      }
    }

    if (candidate.eligible &&
        (!have_eligible || candidate.score.total < best_eligible_score)) {
      have_eligible = true;
      best_eligible_score = candidate.score.total;
      best_eligible_accelerator = accelerator.id;
    }
    plan.candidates.push_back(std::move(candidate));
  }

  if (!have_eligible) {
    plan.outcome = best_severity == std::numeric_limits<int>::max()
                       ? PlanOutcome::NoEligibleAccelerator
                       : best_failure;
    // A rejected plan carries no binding at all: an accelerator identity
    // without its generation would invite a caller to treat the plan as usable.
    plan.binding = PlanBinding{};
    for (const CandidateEvaluation& candidate : plan.candidates) {
      if (candidate.accelerator == best_failure_accelerator) {
        plan.explanation.append(candidate.explanation);
      }
    }
    if (plan.explanation.empty()) {
      plan.explanation.add(ExplanationCode::PlanNoEligibleAccelerator, "fabric",
                           "no registered accelerator can satisfy the request");
    }
    plan.explanation.add(rejection_code_for(plan.outcome), "request", to_string(plan.outcome));
    plan.explanation.set_summary(std::string("plan rejected: ") + to_string(plan.outcome));
    plan.explanation.canonicalize(state.limits.max_explanations);
    return plan;
  }

  const CandidateEvaluation* chosen = nullptr;
  for (const CandidateEvaluation& candidate : plan.candidates) {
    if (candidate.accelerator == best_eligible_accelerator && candidate.eligible) {
      chosen = &candidate;
      break;
    }
  }
  plan.outcome = chosen->outcome;
  const AcceleratorRecord* accelerator = state.find_accelerator(best_eligible_accelerator);
  plan.binding.accelerator = best_eligible_accelerator;
  plan.binding.accelerator_generation = accelerator->generation;
  plan.binding.accelerator_boot = accelerator->boot_id;
  plan.binding.capability_generation = accelerator->capability.generation;
  plan.binding.topology_generation = accelerator->topology_generation;
  plan.binding.evidence_generation = accelerator->evidence.generation;
  for (const auto& entry : state.partitions) {
    if (entry.second.accelerator == best_eligible_accelerator) {
      plan.binding.layout.emplace_back(entry.second.id, entry.second.generation);
    }
  }

  for (std::uint32_t ordinal = 0; ordinal < request.count; ++ordinal) {
    PlannedPartition planned;
    planned.profile = requested_profile->id;
    planned.profile_generation = requested_profile->generation;
    planned.resources = requested_profile->resources;
    planned.ordinal = ordinal;
    planned.vendor_native_profile = requested_profile->vendor_native;
    plan.planned_partitions.push_back(std::move(planned));
  }

  // Detect a deterministic tie and record it.
  std::size_t eligible_count = 0;
  for (const CandidateEvaluation& candidate : plan.candidates) {
    if (candidate.eligible) {
      ++eligible_count;
    }
  }
  if (eligible_count > 1) {
    plan.explanation.add(ExplanationCode::TieBreak, best_eligible_accelerator.str(),
                         "lowest accelerator identity wins an exact score tie");
  }
  plan.explanation.append(chosen->explanation);
  plan.explanation.add(ExplanationCode::CandidateChosen, best_eligible_accelerator.str(),
                       to_string(plan.outcome),
                       static_cast<std::int64_t>(chosen->score.total));
  plan.explanation.set_summary(std::string("plan ") + to_string(plan.outcome) + " on accelerator " +
                               best_eligible_accelerator.str());

  std::uint32_t step_index = 0;
  const auto push_step = [&](PlanStepKind kind, bool destructive, bool requires_reset,
                             std::uint64_t estimate_ms, const std::string& detail,
                             std::uint32_t planned_index, const PartitionId& partition) {
    if (plan.steps.size() >= state.limits.max_plan_steps) {
      return;
    }
    PlanStep step;
    step.index = step_index++;
    step.kind = kind;
    step.accelerator = best_eligible_accelerator;
    step.accelerator_generation = plan.binding.accelerator_generation;
    step.accelerator_boot = plan.binding.accelerator_boot;
    step.partition = partition;
    step.profile = requested_profile->id;
    step.planned_index = planned_index;
    step.destructive = destructive;
    step.requires_reset = requires_reset;
    step.estimated_duration_ms = estimate_ms;
    step.detail = detail;
    plan.steps.push_back(std::move(step));
  };

  push_step(PlanStepKind::ReserveCapacity, false, false, 0, "hold capacity against the reservation", 0,
            PartitionId{});
  if (plan.outcome == PlanOutcome::FeasibleAfterDrain ||
      plan.outcome == PlanOutcome::FeasibleAfterReconfiguration) {
    for (const PartitionId partition : chosen->fragmentation.partitions_to_drain) {
      push_step(PlanStepKind::BeginDrain, false, false, accelerator->capability.drain_estimate_ms,
                "reject new assignments and track outstanding work", 0, partition);
      push_step(PlanStepKind::AwaitDrainComplete, false, false,
                accelerator->capability.drain_estimate_ms,
                "wait until destructive mutation is safe", 0, partition);
    }
  }
  if (plan.outcome == PlanOutcome::FeasibleAfterReconfiguration) {
    push_step(PlanStepKind::ReconfigureLayout, true,
              accelerator->capability.requires_reset_for_reconfiguration,
              accelerator->capability.reconfiguration_estimate_ms,
              "rebuild the physical layout under a registered attempt", 0, PartitionId{});
  }
  const bool creating_during_reconfiguration =
      plan.outcome == PlanOutcome::FeasibleAfterReconfiguration;
  for (std::uint32_t ordinal = 0; ordinal < request.count; ++ordinal) {
    // A plain creation is not a device reconfiguration: only a creation that is
    // part of a destructive layout change inherits its reset and downtime cost.
    push_step(PlanStepKind::CreatePartition, creating_during_reconfiguration,
              creating_during_reconfiguration &&
                  accelerator->capability.requires_reset_for_reconfiguration,
              creating_during_reconfiguration
                  ? accelerator->capability.reconfiguration_estimate_ms
                  : 0,
              "create physical partition", ordinal, PartitionId{});
  }
  push_step(PlanStepKind::VerifyPhysicalState, false, false, 0,
            "rediscover the device and verify the requested geometry exists", 0, PartitionId{});
  push_step(PlanStepKind::CommitAuthority, false, false, 0,
            "publish authoritative logical state bound to fresh evidence", 0, PartitionId{});
  plan.explanation.canonicalize(state.limits.max_explanations);
  return plan;
}

}  // namespace apf
