#include "fabric_internal.hpp"

#include <algorithm>
#include <limits>

namespace apf {
namespace {

Status accumulate(const std::vector<PlannedPartition>& desired, ResourceVector& out) {
  for (const PlannedPartition& planned : desired) {
    if (out.empty()) {
      out = planned.resources;
      continue;
    }
    const Status status = out.add_assign(planned.resources);
    if (!status.ok()) {
      return status;
    }
  }
  return success();
}

bool desired_conflicts(const FabricState& state, const std::vector<PlannedPartition>& desired) {
  for (std::size_t index = 0; index < desired.size(); ++index) {
    const PartitionProfile* left = state.find_profile(desired[index].profile);
    if (left == nullptr) {
      continue;
    }
    for (std::size_t other = index + 1; other < desired.size(); ++other) {
      const PartitionProfile* right = state.find_profile(desired[other].profile);
      if (right == nullptr) {
        continue;
      }
      if (left->conflicts_with(*right)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

PartitionPlan build_reconfiguration_plan(FabricState& state, const AcceleratorId& accelerator_id,
                                         const std::vector<PlannedPartition>& desired,
                                         const PartitionRequest& context, std::uint64_t now_ms) {
  PartitionPlan plan;
  plan.request = context;
  plan.request.count = static_cast<std::uint32_t>(desired.size());
  plan.request_digest = request_digest(plan.request);
  plan.created_at_ms = now_ms;
  plan.binding.policy_generation = state.policy_generation;
  plan.binding.coordinator_epoch = state.coordinator_epoch;
  plan.binding.state_generation = state.state_generation;
  plan.binding.planned_at_ms = now_ms;
  plan.binding.accelerator = accelerator_id;
  plan.planned_partitions = desired;
  plan.outcome = PlanOutcome::FeasibleAfterReconfiguration;

  const AcceleratorRecord* accelerator = state.find_accelerator(accelerator_id);
  if (accelerator == nullptr) {
    plan.outcome = PlanOutcome::NoEligibleAccelerator;
    plan.explanation.add(ExplanationCode::RejectedDeviceUnknown, accelerator_id.str(),
                         "accelerator is not registered");
    plan.explanation.set_summary("reconfiguration rejected: accelerator is not registered");
    return plan;
  }
  const CapacityLedger* ledger = state.find_ledger(accelerator_id);
  if (ledger == nullptr) {
    plan.outcome = PlanOutcome::RevalidationRequired;
    plan.explanation.add(ExplanationCode::RejectedRevalidationRequired, accelerator_id.str(),
                         "accelerator has no capacity ledger");
    plan.explanation.set_summary("reconfiguration rejected: accelerator has no capacity ledger");
    return plan;
  }

  plan.binding.accelerator_generation = accelerator->generation;
  plan.binding.accelerator_boot = accelerator->boot_id;
  plan.binding.capability_generation = accelerator->capability.generation;
  plan.binding.topology_generation = accelerator->topology_generation;
  plan.binding.evidence_generation = accelerator->evidence.generation;
  const std::uint64_t window = context.max_evidence_age_ms != 0 ? context.max_evidence_age_ms
                                                                : state.policy.max_evidence_age_ms;
  plan.binding.valid_until_ms = window == 0 ? 0 : now_ms + window;

  if (desired.empty()) {
    plan.outcome = PlanOutcome::InvalidRequest;
    plan.explanation.add(ExplanationCode::CandidateRejected, accelerator_id.str(),
                         "reconfiguration target layout is empty");
    plan.explanation.set_summary("reconfiguration rejected: target layout is empty");
    return plan;
  }
  if (desired.size() > state.limits.max_partitions_per_accelerator) {
    plan.outcome = PlanOutcome::LimitExceeded;
    plan.explanation.add(ExplanationCode::RejectedLimit, accelerator_id.str(),
                         "target layout exceeds the per-accelerator partition bound");
    plan.explanation.set_summary("reconfiguration rejected: target layout exceeds bounds");
    return plan;
  }
  if (!accelerator->capability.declares_supported()) {
    plan.outcome = PlanOutcome::CapabilityUnsupported;
    plan.explanation.add(ExplanationCode::RejectedCapabilityUnsupported, accelerator_id.str(),
                         accelerator->capability.unsupported_reason.empty()
                             ? std::string("device does not support partitioning")
                             : accelerator->capability.unsupported_reason);
    plan.explanation.set_summary("reconfiguration rejected: device does not support partitioning");
    return plan;
  }
  if (accelerator->capability.max_partition_count > 0 &&
      desired.size() > accelerator->capability.max_partition_count) {
    plan.outcome = PlanOutcome::MaxPartitionCountReached;
    plan.explanation.add(ExplanationCode::RejectedMaxPartitionCount, accelerator_id.str(),
                         "target layout exceeds the device partition count",
                         static_cast<std::int64_t>(accelerator->capability.max_partition_count));
    plan.explanation.set_summary("reconfiguration rejected: target layout exceeds the device "
                                 "partition count");
    return plan;
  }
  if (!state.policy.allow_destructive_reconfiguration ||
      !context.allow_destructive_reconfiguration) {
    plan.outcome = PlanOutcome::PolicyRejected;
    plan.explanation.add(ExplanationCode::RejectedDestructiveReconfiguration, accelerator_id.str(),
                         "destructive device reconfiguration is not permitted by policy");
    plan.explanation.set_summary("reconfiguration rejected: policy forbids destructive "
                                 "reconfiguration");
    return plan;
  }

  std::uint32_t compute_slices = 0;
  std::uint32_t memory_slices = 0;
  for (const PlannedPartition& planned : desired) {
    const PartitionProfile* profile = state.find_profile(planned.profile);
    if (profile == nullptr) {
      plan.outcome = PlanOutcome::CapabilityUnknown;
      plan.explanation.add(ExplanationCode::RejectedCapabilityUnknown, accelerator_id.str(),
                           "target layout names an unregistered profile "
                           + planned.profile.str());
      plan.explanation.set_summary("reconfiguration rejected: unknown profile in target layout");
      return plan;
    }
    if (!accelerator->capability.supports_profile(profile->id)) {
      plan.outcome = PlanOutcome::CapabilityUnsupported;
      plan.explanation.add(ExplanationCode::RejectedCapabilityUnsupported, accelerator_id.str(),
                           "target layout names a profile the device does not publish: "
                           + profile->name);
      plan.explanation.set_summary("reconfiguration rejected: unsupported profile in target "
                                   "layout");
      return plan;
    }
    if (!profile->backend.empty() && profile->backend != accelerator->backend) {
      plan.outcome = PlanOutcome::PolicyRejected;
      plan.explanation.add(ExplanationCode::RejectedBackendMismatch, accelerator_id.str(),
                           "target profile belongs to a different backend");
      plan.explanation.set_summary("reconfiguration rejected: profile backend mismatch");
      return plan;
    }
    if (compute_slices > std::numeric_limits<std::uint32_t>::max() - profile->compute_slice_count ||
        memory_slices > std::numeric_limits<std::uint32_t>::max() - profile->memory_slice_count) {
      plan.outcome = PlanOutcome::LimitExceeded;
      plan.explanation.add(ExplanationCode::RejectedLimit, accelerator_id.str(),
                           "target layout slice count overflows");
      plan.explanation.set_summary("reconfiguration rejected: slice count overflow");
      return plan;
    }
    compute_slices += profile->compute_slice_count;
    memory_slices += profile->memory_slice_count;
  }
  if (desired_conflicts(state, desired)) {
    plan.outcome = PlanOutcome::Fragmented;
    plan.explanation.add(ExplanationCode::RejectedIncompatibleProfileCombination,
                         accelerator_id.str(),
                         "target layout combines mutually exclusive profiles");
    plan.explanation.set_summary("reconfiguration rejected: target layout combines mutually "
                                 "exclusive profiles");
    return plan;
  }
  if (accelerator->capability.total_compute_slices > 0 &&
      compute_slices > accelerator->capability.total_compute_slices) {
    plan.outcome = PlanOutcome::PhysicallyImpossible;
    plan.explanation.add(ExplanationCode::PlanPhysicallyImpossible, accelerator_id.str(),
                         "target layout needs more compute slices than the device has",
                         static_cast<std::int64_t>(compute_slices));
    plan.explanation.set_summary("reconfiguration rejected: target layout exceeds device compute "
                                 "slice geometry");
    return plan;
  }
  if (accelerator->capability.total_memory_slices > 0 &&
      memory_slices > accelerator->capability.total_memory_slices) {
    plan.outcome = PlanOutcome::PhysicallyImpossible;
    plan.explanation.add(ExplanationCode::PlanPhysicallyImpossible, accelerator_id.str(),
                         "target layout needs more memory slices than the device has",
                         static_cast<std::int64_t>(memory_slices));
    plan.explanation.set_summary("reconfiguration rejected: target layout exceeds device memory "
                                 "slice geometry");
    return plan;
  }

  ResourceVector target;
  const Status accumulated = accumulate(desired, target);
  if (!accumulated.ok()) {
    plan.outcome = PlanOutcome::LimitExceeded;
    plan.explanation.add(ExplanationCode::RejectedLimit, accelerator_id.str(),
                         to_string(accumulated.error()));
    plan.explanation.set_summary("reconfiguration rejected: target layout overflows capacity");
    return plan;
  }
  ResourceVector usable = ledger->total();
  const Status usable_status = usable.sub_assign(ledger->bucket(CapacityBucket::Unavailable));
  if (!usable_status.ok()) {
    plan.outcome = PlanOutcome::RevalidationRequired;
    plan.explanation.add(ExplanationCode::AccountingViolation, accelerator_id.str(),
                         to_string(usable_status.error()));
    plan.explanation.set_summary("reconfiguration rejected: device capacity does not close");
    return plan;
  }
  if (!target.is_subset_of(usable)) {
    plan.outcome = PlanOutcome::InsufficientCapacity;
    plan.explanation.add(ExplanationCode::PlanInsufficientCapacity, accelerator_id.str(),
                         "target layout exceeds usable physical capacity: " + target.format());
    plan.explanation.set_summary("reconfiguration rejected: target layout exceeds usable physical "
                                 "capacity");
    return plan;
  }

  // Bind the exact layout the plan was derived from, and schedule the drain of
  // every partition that must be destroyed.
  std::vector<PartitionId> existing;
  for (const auto& entry : state.partitions) {
    const PartitionRecord& record = entry.second;
    if (record.accelerator != accelerator_id || is_terminal_state(record.state) ||
        record.state == PartitionState::Unpartitioned) {
      continue;
    }
    existing.push_back(record.id);
    plan.binding.layout.emplace_back(record.id, record.generation);
  }
  std::sort(existing.begin(), existing.end());

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
    step.accelerator = accelerator_id;
    step.accelerator_generation = plan.binding.accelerator_generation;
    step.accelerator_boot = plan.binding.accelerator_boot;
    step.partition = partition;
    step.profile = desired.empty() ? PartitionProfileId{} : desired.front().profile;
    step.planned_index = planned_index;
    step.destructive = destructive;
    step.requires_reset = requires_reset;
    step.estimated_duration_ms = estimate_ms;
    step.detail = detail;
    plan.steps.push_back(std::move(step));
  };

  push_step(PlanStepKind::ReserveCapacity, false, false, 0, "hold the target layout capacity", 0,
            PartitionId{});
  std::uint32_t blocking = 0;
  for (const PartitionId partition : existing) {
    const PartitionRecord* record = state.find_partition(partition);
    const bool needs_drain = record != nullptr && !record->drain.destructive_safe();
    push_step(PlanStepKind::BeginDrain, false, false, accelerator->capability.drain_estimate_ms,
              needs_drain ? "drain before destructive reconfiguration" : "drain already complete",
              0, partition);
    if (needs_drain) {
      ++blocking;
      push_step(PlanStepKind::AwaitDrainComplete, false, false,
                accelerator->capability.drain_estimate_ms,
                "destructive mutation is unsafe until outstanding work reaches zero", 0, partition);
    }
    push_step(PlanStepKind::DestroyPartition, true, false, 0,
              "destroy the superseded physical partition", 0, partition);
  }
  push_step(PlanStepKind::ReconfigureLayout, true,
            accelerator->capability.requires_reset_for_reconfiguration,
            accelerator->capability.reconfiguration_estimate_ms,
            "rebuild the physical layout under a registered attempt", 0, PartitionId{});
  for (std::uint32_t ordinal = 0; ordinal < desired.size(); ++ordinal) {
    push_step(PlanStepKind::CreatePartition, true,
              accelerator->capability.requires_reset_for_reconfiguration,
              accelerator->capability.reconfiguration_estimate_ms, "create physical partition",
              ordinal, PartitionId{});
  }
  push_step(PlanStepKind::VerifyPhysicalState, false, false, 0,
            "rediscover the device and verify the new geometry", 0, PartitionId{});
  push_step(PlanStepKind::CommitAuthority, false, false, 0,
            "publish new partition generations and fence the old ones", 0, PartitionId{});

  plan.explanation.add(ExplanationCode::ReconfigurationPlanned, accelerator_id.str(),
                       "target layout of " + std::to_string(desired.size()) + " partitions",
                       static_cast<std::int64_t>(desired.size()));
  plan.explanation.add(ExplanationCode::RejectedDestructiveReconfiguration, accelerator_id.str(),
                       "this plan destroys every existing partition on the device");
  if (blocking > 0) {
    plan.explanation.add(ExplanationCode::RejectedDrainRequired, accelerator_id.str(),
                         "drain must complete before the mutation is dispatched",
                         static_cast<std::int64_t>(blocking));
  }
  plan.explanation.add(ExplanationCode::RankingFactor, "target_capacity", target.format());
  plan.explanation.set_summary("reconfiguration plan to " + std::to_string(desired.size()) +
                               " partitions on accelerator " + accelerator_id.str());
  plan.explanation.canonicalize(state.limits.max_explanations);
  return plan;
}

}  // namespace apf
