#pragma once

#include "apf/explain.hpp"
#include "apf/id.hpp"
#include "apf/isolation.hpp"
#include "apf/limits.hpp"
#include "apf/profile.hpp"
#include "apf/resource.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apf {

/// Hard target selection for a planning request. Every field is a hard filter:
/// an ineligible accelerator can never win through score.
struct DeviceSelector {
  std::string backend;
  std::string vendor;
  std::string device_family;
  std::string locality_domain;
  std::vector<AcceleratorId> allowed;
  std::vector<AcceleratorId> excluded;

  bool admits(const AcceleratorId& id) const noexcept;
};

/// A request to allocate partitions.
struct PartitionRequest {
  PartitionProfileId profile{};
  std::uint32_t count{1};
  /// Additional minimum resources beyond what the profile already guarantees.
  ResourceVector minimum_resources{};
  IsolationRequirement isolation{};
  bool exclusive{false};
  bool allow_drain{true};
  bool allow_destructive_reconfiguration{false};
  bool allow_degraded_device{false};
  bool allow_shared_device{true};
  DeviceSelector selector{};
  std::string policy_class;
  /// 0 means use the policy default freshness requirement.
  std::uint64_t max_evidence_age_ms{0};
  std::string requester;
  /// All-or-nothing: a partial allocation is rolled back completely.
  bool all_or_nothing{true};
  std::string request_tag;

  Status validate(const Limits& limits) const;
};

/// A workload's partition requirements. Admission is not a scheduler decision:
/// it is the partition-side gate for one workload against one partition.
struct WorkloadRequirement {
  std::string workload_id;
  std::string tenant_id;
  /// Invalid means "any profile that satisfies the resource minimums".
  PartitionProfileId required_profile{};
  ResourceVector minimum_resources{};
  IsolationRequirement isolation{};
  bool exclusive{false};
  bool allow_reconfiguration{false};
  bool allow_drain{false};
  std::string backend;
  std::string device_family;
  std::string locality_domain;
  /// 0 means use the policy default.
  std::uint64_t required_freshness_ms{0};
  std::string policy_class;
  std::uint32_t requested_partition_count{1};

  Status validate(const Limits& limits) const;
};

/// Explicit admission outcomes. A workload is admitted only to a current
/// partition whose authority is still valid at commit time.
enum class AdmissionOutcome : std::uint8_t {
  Admitted = 0,
  RejectedNoCandidatePartition = 1,
  RejectedInsufficientResources = 2,
  RejectedIsolation = 3,
  RejectedStaleAuthority = 4,
  RejectedPolicy = 5,
  RejectedProfile = 6,
  RejectedDraining = 7,
  RejectedCapability = 8,
  RejectedExclusiveConflict = 9,
  RejectedRevalidationRequired = 10,
  RejectedDeviceUnavailable = 11,
  RejectedLimit = 12,
  RejectedInvalidRequest = 13,
  Count = 14,
};

const char* to_string(AdmissionOutcome outcome) noexcept;
bool admission_admitted(AdmissionOutcome outcome) noexcept;

struct AdmissionDecision {
  AdmissionOutcome outcome{AdmissionOutcome::RejectedNoCandidatePartition};
  PartitionId partition{};
  PartitionGeneration partition_generation{};
  PartitionAssignmentId assignment{};
  AcceleratorId accelerator{};
  AcceleratorGeneration accelerator_generation{};
  std::uint32_t candidates_considered{0};
  /// The projection the decision was evaluated against; the caller can detect
  /// that the decision was made against superseded state.
  StateGeneration state_generation{};
  CoordinatorEpoch coordinator_epoch{};
  PolicyGeneration policy_generation{};
  Explanation explanation{};
  std::string message;

  bool admitted() const noexcept { return admission_admitted(outcome); }
};

/// A projection produced by the admission engine so that a caller may inspect
/// what would have happened without binding authority.
struct AdmissionProbe {
  AdmissionOutcome outcome{AdmissionOutcome::RejectedNoCandidatePartition};
  PartitionId partition{};
  PartitionProfileId profile{};
  ResourceVector resources{};
  std::uint32_t candidates_considered{0};
  Explanation explanation{};
};

}  // namespace apf
