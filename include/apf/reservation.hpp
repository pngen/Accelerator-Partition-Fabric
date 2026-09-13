#pragma once

#include "apf/accounting.hpp"
#include "apf/id.hpp"
#include "apf/plan.hpp"
#include "apf/resource.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apf {

/// Reservation lifecycle. Capacity is held exactly once between Reserve and
/// Release/Commit; double release and release from a superseded generation are
/// rejected, not ignored.
enum class ReservationLifecycle : std::uint8_t {
  Pending = 0,
  Active = 1,
  Committed = 2,
  Released = 3,
  RolledBack = 4,
  Expired = 5,
  Fenced = 6,
  Failed = 7,
  Count = 8,
};

const char* to_string(ReservationLifecycle value) noexcept;
ReservationLifecycle reservation_lifecycle_from_string(std::string_view name);
bool reservation_holds_capacity(ReservationLifecycle value) noexcept;
bool reservation_is_terminal(ReservationLifecycle value) noexcept;

/// A reservation binds every piece of authority it was derived from. Any
/// change to those bindings fences the reservation.
struct PartitionReservation {
  PartitionReservationId id{};
  PartitionPlanId plan{};
  PartitionPlanGeneration plan_generation{};
  AcceleratorId accelerator{};
  AcceleratorGeneration accelerator_generation{};
  AcceleratorBootId accelerator_boot{};
  CapabilityGeneration capability_generation{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};
  CoordinatorEpoch coordinator_epoch{};
  WorkerId worker{};
  WorkerBootId worker_boot{};
  ReservationLifecycle lifecycle{ReservationLifecycle::Pending};
  std::vector<PartitionId> partitions;
  std::vector<PartitionGeneration> partition_generations;
  ResourceVector resources{};
  PartitionAttemptId attempt{};
  std::uint32_t planned_mutations{0};
  bool destructive{false};
  std::uint64_t created_at_ms{0};
  std::uint64_t updated_at_ms{0};
  std::uint64_t expires_at_ms{0};
  std::uint32_t rollback_count{0};
  std::string reason;

  Status validate() const;
  bool holds_capacity() const noexcept { return reservation_holds_capacity(lifecycle); }
};

enum class AttemptKind : std::uint8_t {
  CreatePartition = 0,
  DestroyPartition = 1,
  ReconfigureLayout = 2,
  Count = 3,
};

const char* to_string(AttemptKind kind) noexcept;
AttemptKind attempt_kind_from_string(std::string_view name);

/// A physical mutation attempt. It is registered before dispatch so that a fast
/// completion cannot race state registration, and it is settled only after the
/// physical result has been verified.
enum class AttemptState : std::uint8_t {
  Registered = 0,
  Dispatched = 1,
  Succeeded = 2,
  Failed = 3,
  /// The mutation may have happened but no acknowledgement was received.
  OutcomeUnknown = 4,
  Reconciled = 5,
  Count = 6,
};

const char* to_string(AttemptState state) noexcept;
AttemptState attempt_state_from_string(std::string_view name);
bool attempt_settled(AttemptState state) noexcept;

struct PartitionAttempt {
  PartitionAttemptId id{};
  AttemptKind kind{AttemptKind::CreatePartition};
  PartitionReservationId reservation{};
  AcceleratorId accelerator{};
  AcceleratorGeneration accelerator_generation{};
  AcceleratorBootId accelerator_boot{};
  CapabilityGeneration capability_generation{};
  CoordinatorEpoch coordinator_epoch{};
  WorkerId worker{};
  WorkerBootId worker_boot{};
  std::vector<PartitionId> partitions{};
  std::vector<PlannedPartition> desired{};
  AttemptState state{AttemptState::Registered};
  /// Idempotency token handed to the backend so that a reconciled replay can
  /// be recognised rather than re-applied.
  std::uint64_t token{0};
  std::uint64_t registered_at_ms{0};
  std::uint64_t dispatched_at_ms{0};
  std::uint64_t settled_at_ms{0};
  std::string detail;
  std::string observed_native_layout_digest;

  Status validate() const;
};

}  // namespace apf
