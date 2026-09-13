#include "apf/plan.hpp"

#include "apf/codec.hpp"

#include <algorithm>

namespace apf {

const char* to_string(PlanOutcome outcome) noexcept {
  switch (outcome) {
    case PlanOutcome::FeasibleNow: return "FEASIBLE_NOW";
    case PlanOutcome::FeasibleAfterDrain: return "FEASIBLE_AFTER_DRAIN";
    case PlanOutcome::FeasibleAfterReconfiguration: return "FEASIBLE_AFTER_RECONFIGURATION";
    case PlanOutcome::PhysicallyImpossible: return "PHYSICALLY_IMPOSSIBLE";
    case PlanOutcome::CapabilityUnsupported: return "CAPABILITY_UNSUPPORTED";
    case PlanOutcome::CapabilityUnknown: return "CAPABILITY_UNKNOWN";
    case PlanOutcome::InsufficientCapacity: return "INSUFFICIENT_CAPACITY";
    case PlanOutcome::Fragmented: return "FRAGMENTED";
    case PlanOutcome::PolicyRejected: return "POLICY_REJECTED";
    case PlanOutcome::StaleEvidence: return "STALE_EVIDENCE";
    case PlanOutcome::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case PlanOutcome::NoEligibleAccelerator: return "NO_ELIGIBLE_ACCELERATOR";
    case PlanOutcome::MaxPartitionCountReached: return "MAX_PARTITION_COUNT_REACHED";
    case PlanOutcome::IsolationUnsatisfied: return "ISOLATION_UNSATISFIED";
    case PlanOutcome::InvalidRequest: return "INVALID_REQUEST";
    case PlanOutcome::LimitExceeded: return "LIMIT_EXCEEDED";
    case PlanOutcome::DuplicateRequest: return "DUPLICATE_REQUEST";
    case PlanOutcome::Count: return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool plan_outcome_feasible(PlanOutcome outcome) noexcept {
  switch (outcome) {
    case PlanOutcome::FeasibleNow:
    case PlanOutcome::FeasibleAfterDrain:
    case PlanOutcome::FeasibleAfterReconfiguration:
      return true;
    default:
      return false;
  }
}

bool plan_outcome_requires_mutation(PlanOutcome outcome) noexcept {
  switch (outcome) {
    case PlanOutcome::FeasibleNow:
    case PlanOutcome::FeasibleAfterDrain:
    case PlanOutcome::FeasibleAfterReconfiguration:
      return true;
    default:
      return false;
  }
}

bool plan_outcome_destructive(PlanOutcome outcome) noexcept {
  return outcome == PlanOutcome::FeasibleAfterReconfiguration;
}

const char* to_string(PlanStepKind kind) noexcept {
  switch (kind) {
    case PlanStepKind::ReserveCapacity: return "RESERVE_CAPACITY";
    case PlanStepKind::BeginDrain: return "BEGIN_DRAIN";
    case PlanStepKind::AwaitDrainComplete: return "AWAIT_DRAIN_COMPLETE";
    case PlanStepKind::ReconfigureLayout: return "RECONFIGURE_LAYOUT";
    case PlanStepKind::CreatePartition: return "CREATE_PARTITION";
    case PlanStepKind::DestroyPartition: return "DESTROY_PARTITION";
    case PlanStepKind::VerifyPhysicalState: return "VERIFY_PHYSICAL_STATE";
    case PlanStepKind::CommitAuthority: return "COMMIT_AUTHORITY";
    case PlanStepKind::RollbackReservation: return "ROLLBACK_RESERVATION";
    case PlanStepKind::Count: return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::string PlanStep::describe() const {
  std::string out = std::to_string(index);
  out += ".";
  out += to_string(kind);
  if (accelerator.valid()) {
    out += " accel=";
    out += accelerator.str();
    out += "@";
    out += std::to_string(accelerator_generation.value());
  }
  if (partition.valid()) {
    out += " partition=";
    out += partition.str();
  }
  if (profile.valid()) {
    out += " profile=";
    out += profile.str();
  }
  if (destructive) {
    out += " destructive";
  }
  if (requires_reset) {
    out += " requires_reset";
  }
  if (estimated_duration_ms != 0) {
    out += " estimate_ms=";
    out += std::to_string(estimated_duration_ms);
  }
  if (!detail.empty()) {
    out += " ";
    out += detail;
  }
  return out;
}

const CandidateEvaluation* PartitionPlan::chosen() const noexcept {
  for (const CandidateEvaluation& candidate : candidates) {
    if (candidate.eligible && candidate.outcome == outcome) {
      return &candidate;
    }
  }
  for (const CandidateEvaluation& candidate : candidates) {
    if (candidate.eligible) {
      return &candidate;
    }
  }
  return nullptr;
}

std::string PartitionPlan::describe() const {
  std::string out = "plan ";
  out += id.str();
  out += " generation=";
  out += std::to_string(generation.value());
  out += " outcome=";
  out += to_string(outcome);
  out += " count=";
  out += std::to_string(request.count);
  out += " accelerator=";
  out += binding.accelerator.valid() ? binding.accelerator.str() : std::string("none");
  out += " steps=";
  out += std::to_string(steps.size());
  out += " candidates=";
  out += std::to_string(candidates.size());
  return out;
}

namespace {

void mix_bytes(std::uint64_t& hash, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
}

void mix_u64(std::uint64_t& hash, std::uint64_t value) {
  std::uint8_t buffer[8];
  for (int index = 0; index < 8; ++index) {
    buffer[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu);
  }
  mix_bytes(hash, buffer, sizeof(buffer));
}

void mix_text(std::uint64_t& hash, const std::string& value) {
  mix_u64(hash, value.size());
  mix_bytes(hash, value.data(), value.size());
  hash ^= 0x1Fu;
  hash *= 1099511628211ull;
}

void mix_vector(std::uint64_t& hash, const ResourceVector& vector) {
  mix_u64(hash, vector.mask());
  for (const std::uint64_t entry : vector.raw()) {
    mix_u64(hash, entry);
  }
}

}  // namespace

std::uint64_t request_digest(const PartitionRequest& request) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  mix_u64(hash, request.profile.value());
  mix_u64(hash, request.count);
  mix_vector(hash, request.minimum_resources);
  mix_u64(hash, request.isolation.required.mask());
  mix_u64(hash, request.isolation.policy.value());
  mix_u64(hash, request.isolation.policy_generation.value());
  mix_u64(hash, request.exclusive ? 1u : 0u);
  mix_u64(hash, request.allow_drain ? 1u : 0u);
  mix_u64(hash, request.allow_destructive_reconfiguration ? 1u : 0u);
  mix_u64(hash, request.allow_degraded_device ? 1u : 0u);
  mix_u64(hash, request.allow_shared_device ? 1u : 0u);
  mix_u64(hash, request.all_or_nothing ? 1u : 0u);
  mix_u64(hash, request.max_evidence_age_ms);
  mix_text(hash, request.selector.backend);
  mix_text(hash, request.selector.vendor);
  mix_text(hash, request.selector.device_family);
  mix_text(hash, request.selector.locality_domain);
  for (const AcceleratorId id : request.selector.allowed) {
    mix_u64(hash, id.value());
  }
  hash ^= 0xAAu;
  hash *= 1099511628211ull;
  for (const AcceleratorId id : request.selector.excluded) {
    mix_u64(hash, id.value());
  }
  mix_text(hash, request.policy_class);
  return hash;
}

std::uint64_t plan_digest(const PartitionPlan& plan) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  mix_u64(hash, plan.request_digest);
  mix_u64(hash, static_cast<std::uint64_t>(plan.outcome));
  mix_u64(hash, plan.binding.accelerator.value());
  mix_u64(hash, plan.binding.accelerator_generation.value());
  mix_u64(hash, plan.binding.capability_generation.value());
  mix_u64(hash, plan.binding.policy_generation.value());
  mix_u64(hash, plan.binding.state_generation.value());
  for (const auto& entry : plan.binding.layout) {
    mix_u64(hash, entry.first.value());
    mix_u64(hash, entry.second.value());
  }
  mix_u64(hash, plan.planned_partitions.size());
  for (const PlannedPartition& planned : plan.planned_partitions) {
    mix_u64(hash, planned.profile.value());
    mix_u64(hash, planned.ordinal);
    mix_vector(hash, planned.resources);
    mix_text(hash, planned.vendor_native_profile);
  }
  mix_u64(hash, plan.steps.size());
  for (const PlanStep& step : plan.steps) {
    mix_u64(hash, static_cast<std::uint64_t>(step.kind));
    mix_u64(hash, step.partition.value());
    mix_u64(hash, step.profile.value());
    mix_u64(hash, step.planned_index);
    mix_u64(hash, step.destructive ? 1u : 0u);
    mix_u64(hash, step.requires_reset ? 1u : 0u);
    mix_u64(hash, step.estimated_duration_ms);
  }
  mix_u64(hash, plan.candidates.size());
  for (const CandidateEvaluation& candidate : plan.candidates) {
    mix_u64(hash, candidate.accelerator.value());
    mix_u64(hash, candidate.accelerator_generation.value());
    mix_u64(hash, candidate.eligible ? 1u : 0u);
    mix_u64(hash, static_cast<std::uint64_t>(candidate.outcome));
    mix_u64(hash, static_cast<std::uint64_t>(candidate.score.total));
    mix_u64(hash, candidate.explanation.digest());
  }
  return hash;
}

}  // namespace apf
