#pragma once

#include "apf/accounting.hpp"
#include "apf/evidence.hpp"
#include "apf/id.hpp"
#include "apf/isolation.hpp"
#include "apf/lifecycle.hpp"
#include "apf/profile.hpp"
#include "apf/resource.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace apf {

/// Backend-native identity of a physical partition. Opaque to generic logic but
/// persisted and re-verified, because a logical record surviving restart does
/// not make the physical partition still present.
struct PartitionNativeIdentity {
  std::string backend;
  std::string native_id;
  std::string parent_native_id;
  std::string instance_uuid;

  bool empty() const noexcept { return native_id.empty(); }
};

enum class AssignmentState : std::uint8_t {
  Unassigned = 0,
  Reserved = 1,
  Assigned = 2,
  Releasing = 3,
  Count = 4,
};

const char* to_string(AssignmentState state) noexcept;
AssignmentState assignment_state_from_string(std::string_view name);

enum class DrainState : std::uint8_t {
  NotDraining = 0,
  Draining = 1,
  DrainComplete = 2,
  /// Draining cannot complete because work cannot be displaced by this runtime.
  DrainBlocked = 3,
  Count = 4,
};

const char* to_string(DrainState state) noexcept;
DrainState drain_state_from_string(std::string_view name);

struct DrainProgress {
  DrainState state{DrainState::NotDraining};
  std::uint32_t outstanding_assignments{0};
  std::uint32_t total_assignments{0};
  std::uint64_t started_at_ms{0};
  std::uint64_t completed_at_ms{0};
  std::string blocked_reason;
  /// A stable, deterministic identifier of the blocking cause, if any.
  std::string blocker;

  bool complete() const noexcept { return state == DrainState::DrainComplete; }
  bool active() const noexcept { return state == DrainState::Draining; }

  /// Destructive mutation is only safe once drain completed and nothing
  /// outstanding remains.
  bool destructive_safe() const noexcept {
    return state == DrainState::DrainComplete && outstanding_assignments == 0;
  }
};

/// A logical partition identity with exactly one current generation.
struct PartitionRecord {
  PartitionId id{};
  PartitionGeneration generation{};
  PartitionProfileId profile{};
  PartitionProfileGeneration profile_generation{};
  PartitionMechanism mechanism{PartitionMechanism::None};
  AcceleratorId accelerator{};
  AcceleratorGeneration accelerator_generation{};
  AcceleratorBootId accelerator_boot{};
  ResourceVector resources;
  PartitionState state{PartitionState::Discovered};
  PartitionNativeIdentity native_identity{};
  IsolationDomain isolation{};
  /// Exactly which ledger bucket currently holds this partition's capacity.
  /// Absent means the partition holds no capacity at all, which is what makes
  /// the accounting invariant auditable per partition rather than inferred.
  std::optional<CapacityBucket> held_bucket{};
  AssignmentState assignment_state{AssignmentState::Unassigned};
  DrainProgress drain{};
  PartitionReservationId reservation{};
  PartitionAttemptId last_attempt{};
  /// Every previously authoritative generation of this logical partition
  /// identity. Used to fence stale frames and stale releases.
  std::vector<PartitionGeneration> superseded_generations;
  EvidenceStamp evidence{};
  EvidenceProvenance provenance{EvidenceProvenance::Unknown};
  std::uint64_t created_at_ms{0};
  std::uint64_t last_transition_at_ms{0};
  std::string last_transition_reason;
  /// Set when reconciliation established this record from observed hardware
  /// rather than from an authorised mutation.
  bool externally_observed{false};

  bool authoritative() const noexcept { return state == PartitionState::Active; }
  bool supersedes(PartitionGeneration other) const noexcept;
  Status validate() const;
};

/// Authority for a workload to occupy a partition at the partition boundary.
/// This is not a scheduler: it is the partition-side admission gate.
struct PartitionAssignment {
  PartitionAssignmentId id{};
  PartitionId partition{};
  PartitionGeneration partition_generation{};
  AcceleratorId accelerator{};
  AcceleratorGeneration accelerator_generation{};
  std::string workload_id;
  std::string tenant_id;
  IsolationRequirement isolation{};
  PolicyGeneration policy_generation{};
  CoordinatorEpoch coordinator_epoch{};
  EvidenceGeneration evidence_generation{};
  bool exclusive{false};
  std::uint64_t bound_at_ms{0};
  std::uint64_t last_validated_at_ms{0};

  Status validate() const;
};

}  // namespace apf
