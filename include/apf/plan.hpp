#pragma once

#include "apf/accounting.hpp"
#include "apf/explain.hpp"
#include "apf/fragmentation.hpp"
#include "apf/id.hpp"
#include "apf/policy.hpp"
#include "apf/request.hpp"
#include "apf/resource.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace apf {

/// Deterministic planning outcome. Outcomes that look similar to a human are
/// deliberately separate machine states.
enum class PlanOutcome : std::uint8_t {
  FeasibleNow = 0,
  FeasibleAfterDrain = 1,
  FeasibleAfterReconfiguration = 2,
  PhysicallyImpossible = 3,
  CapabilityUnsupported = 4,
  CapabilityUnknown = 5,
  InsufficientCapacity = 6,
  Fragmented = 7,
  PolicyRejected = 8,
  StaleEvidence = 9,
  RevalidationRequired = 10,
  NoEligibleAccelerator = 11,
  MaxPartitionCountReached = 12,
  IsolationUnsatisfied = 13,
  InvalidRequest = 14,
  LimitExceeded = 15,
  DuplicateRequest = 16,
  Count = 17,
};

const char* to_string(PlanOutcome outcome) noexcept;
bool plan_outcome_feasible(PlanOutcome outcome) noexcept;
/// True when executing the plan requires a physical mutation.
bool plan_outcome_requires_mutation(PlanOutcome outcome) noexcept;
/// True when executing the plan requires destructive reconfiguration.
bool plan_outcome_destructive(PlanOutcome outcome) noexcept;

enum class PlanStepKind : std::uint8_t {
  ReserveCapacity = 0,
  BeginDrain = 1,
  AwaitDrainComplete = 2,
  ReconfigureLayout = 3,
  CreatePartition = 4,
  DestroyPartition = 5,
  VerifyPhysicalState = 6,
  CommitAuthority = 7,
  RollbackReservation = 8,
  Count = 9,
};

const char* to_string(PlanStepKind kind) noexcept;

struct PlanStep {
  std::uint32_t index{0};
  PlanStepKind kind{PlanStepKind::VerifyPhysicalState};
  AcceleratorId accelerator{};
  AcceleratorGeneration accelerator_generation{};
  AcceleratorBootId accelerator_boot{};
  PartitionId partition{};
  PartitionProfileId profile{};
  /// Index into PartitionPlan::planned_partitions for per-partition steps.
  std::uint32_t planned_index{0};
  bool destructive{false};
  bool requires_reset{false};
  std::uint64_t estimated_duration_ms{0};
  std::string detail;

  std::string describe() const;
};

/// One partition the plan intends to own. Concrete partition identities are
/// allocated at reservation time, never by the planner, so that a discarded
/// plan cannot consume identity space.
struct PlannedPartition {
  PartitionProfileId profile{};
  PartitionProfileGeneration profile_generation{};
  ResourceVector resources{};
  std::uint32_t ordinal{0};
  std::string vendor_native_profile;
};

/// The exact state a plan was derived from. Any relevant change makes the plan
/// stale, and a stale destructive plan is never executed.
struct PlanBinding {
  AcceleratorId accelerator{};
  AcceleratorGeneration accelerator_generation{};
  AcceleratorBootId accelerator_boot{};
  CapabilityGeneration capability_generation{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};
  CoordinatorEpoch coordinator_epoch{};
  StateGeneration state_generation{};
  EvidenceGeneration evidence_generation{};
  std::uint64_t planned_at_ms{0};
  std::uint64_t valid_until_ms{0};
  /// The layout the plan was computed against, in canonical order.
  std::vector<std::pair<PartitionId, PartitionGeneration>> layout;

  bool expired_at(std::uint64_t now_ms) const noexcept {
    return valid_until_ms != 0 && now_ms > valid_until_ms;
  }
};

struct PlanScoreTerm {
  ExplanationCode code{ExplanationCode::RankingFactor};
  std::string subject;
  std::int64_t weight{0};
  std::int64_t raw{0};
  std::int64_t contribution{0};
};

struct PlanScore {
  std::int64_t total{0};
  std::vector<PlanScoreTerm> terms;
};

/// Per-candidate evaluation. Retained so that explanations can name the actual
/// decisive factors rather than returning an opaque scalar.
struct CandidateEvaluation {
  AcceleratorId accelerator{};
  AcceleratorGeneration accelerator_generation{};
  bool eligible{false};
  PlanOutcome outcome{PlanOutcome::NoEligibleAccelerator};
  FragmentationReport fragmentation{};
  PlanScore score{};
  Explanation explanation{};
};

struct PartitionPlan {
  PartitionPlanId id{};
  PartitionPlanGeneration generation{};
  PartitionRequest request{};
  PlanOutcome outcome{PlanOutcome::NoEligibleAccelerator};
  PlanBinding binding{};
  std::vector<PlannedPartition> planned_partitions;
  std::vector<PlanStep> steps;
  std::vector<CandidateEvaluation> candidates;
  Explanation explanation{};
  std::uint64_t created_at_ms{0};
  /// Canonical digest of the request that produced this plan.
  std::uint64_t request_digest{0};
  bool terminal{false};

  bool feasible() const noexcept { return plan_outcome_feasible(outcome); }
  const CandidateEvaluation* chosen() const noexcept;
  std::string describe() const;
};

/// Digest of a planning request, used for duplicate detection and for proving
/// that identical input produces identical plans.
std::uint64_t request_digest(const PartitionRequest& request) noexcept;

/// Digest of a materialised plan, used by determinism tests.
std::uint64_t plan_digest(const PartitionPlan& plan) noexcept;

}  // namespace apf
