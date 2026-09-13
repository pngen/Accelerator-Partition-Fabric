#pragma once

#include "apf/result.hpp"

#include <cstdint>
#include <string_view>

namespace apf {

/// Explicit partition lifecycle. Booleans are never used to express lifecycle:
/// every state names a distinct systems condition with its own preconditions.
enum class PartitionState : std::uint8_t {
  /// The physical accelerator was seen, but no partition layout has been
  /// published for it yet.
  Discovered = 0,
  /// The device is partition-capable and currently has no logical partitions.
  Unpartitioned = 1,
  /// A plan targeting this device exists but is not yet reserved.
  PlanPending = 2,
  /// Capacity and this partition identity are reserved against a specific plan.
  Reserved = 3,
  /// A physical mutation attempt has been registered but not yet verified.
  Creating = 4,
  /// The partition exists physically and its authority is current.
  Active = 5,
  /// Existing assignments are honoured, new ones are rejected, and destructive
  /// mutation is not yet safe.
  Draining = 6,
  /// The layout must change before this partition identity can be authoritative.
  ReconfigurationRequired = 7,
  /// A destruction attempt has been registered.
  Destroying = 8,
  /// Durable metadata exists but current physical authority is not established.
  /// This is the conservative landing state after any restart or ambiguity.
  RevalidationRequired = 9,
  /// The partition exists but operates with reduced or uncertain guarantees.
  Degraded = 10,
  /// The backing device or partition is not currently reachable.
  Offline = 11,
  /// Terminal. Retired authority can never be resurrected.
  Retired = 12,
  /// Terminal until administrative repair and revalidation.
  Failed = 13,
  Count = 14,
};

inline constexpr std::size_t kPartitionStateCount = static_cast<std::size_t>(PartitionState::Count);

const char* to_string(PartitionState state) noexcept;
Result<PartitionState> partition_state_from_string(std::string_view name);

/// Terminal states never transition again.
bool is_terminal_state(PartitionState state) noexcept;
/// Transient states are mid-transaction; repeating the same transition is not
/// idempotent for them.
bool is_transient_state(PartitionState state) noexcept;
/// States in which the partition holds ledger capacity.
bool holds_capacity(PartitionState state) noexcept;
/// States in which the ledger holds capacity in the reserved bucket.
bool holds_reservation(PartitionState state) noexcept;
/// The single state in which a partition may host a new authoritative workload.
bool allows_new_assignment(PartitionState state) noexcept;
/// States whose authority is current enough to matter at all.
bool is_live_state(PartitionState state) noexcept;
/// True when a persisted record in this state must be revalidated after restart.
bool requires_revalidation_after_restart(PartitionState state) noexcept;

/// Validates a lifecycle transition. Illegal transitions fail deterministically
/// with ErrorCode::InvalidTransition.
Status validate_transition(PartitionState from, PartitionState to) noexcept;

/// True when from == to and the repetition is defined as safe.
bool is_idempotent_transition(PartitionState from, PartitionState to) noexcept;

/// Human-readable description of a state for tooling.
const char* describe_state(PartitionState state) noexcept;

}  // namespace apf
