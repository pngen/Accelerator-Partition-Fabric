#include "apf/lifecycle.hpp"

#include <array>

namespace apf {
namespace {

constexpr std::array<const char*, kPartitionStateCount> kStateNames{{
    "DISCOVERED",
    "UNPARTITIONED",
    "PLAN_PENDING",
    "RESERVED",
    "CREATING",
    "ACTIVE",
    "DRAINING",
    "RECONFIGURATION_REQUIRED",
    "DESTROYING",
    "REVALIDATION_REQUIRED",
    "DEGRADED",
    "OFFLINE",
    "RETIRED",
    "FAILED",
}};

constexpr std::array<const char*, kPartitionStateCount> kStateDescriptions{{
    "physical accelerator observed, no partition layout published yet",
    "partition-capable device with no logical partitions",
    "a plan targets this device but no capacity is reserved",
    "capacity and identity reserved against a specific plan",
    "physical mutation attempt registered, not yet verified",
    "partition exists physically and its authority is current",
    "existing assignments honoured, new assignments rejected",
    "layout must change before this identity can be authoritative",
    "destruction attempt registered",
    "durable metadata exists but current physical authority is not established",
    "partition exists with reduced or uncertain guarantees",
    "backing device or partition is not currently reachable",
    "terminal, authority can never be resurrected",
    "terminal until administrative repair and revalidation",
}};

/// Legal transition matrix. Index from-state, bit for to-state.
std::array<std::uint32_t, kPartitionStateCount> build_transition_table() {
  std::array<std::uint32_t, kPartitionStateCount> table{};
  const auto allow = [&table](PartitionState from, PartitionState to) {
    table[static_cast<std::size_t>(from)] |= 1u << static_cast<std::uint32_t>(to);
  };

  allow(PartitionState::Discovered, PartitionState::Discovered);
  allow(PartitionState::Discovered, PartitionState::Unpartitioned);
  allow(PartitionState::Discovered, PartitionState::PlanPending);
  allow(PartitionState::Discovered, PartitionState::RevalidationRequired);
  allow(PartitionState::Discovered, PartitionState::Offline);
  allow(PartitionState::Discovered, PartitionState::Retired);
  allow(PartitionState::Discovered, PartitionState::Failed);

  allow(PartitionState::Unpartitioned, PartitionState::Unpartitioned);
  allow(PartitionState::Unpartitioned, PartitionState::Discovered);
  allow(PartitionState::Unpartitioned, PartitionState::PlanPending);
  allow(PartitionState::Unpartitioned, PartitionState::Reserved);
  allow(PartitionState::Unpartitioned, PartitionState::RevalidationRequired);
  allow(PartitionState::Unpartitioned, PartitionState::Offline);
  allow(PartitionState::Unpartitioned, PartitionState::Degraded);
  allow(PartitionState::Unpartitioned, PartitionState::Retired);
  allow(PartitionState::Unpartitioned, PartitionState::Failed);

  allow(PartitionState::PlanPending, PartitionState::PlanPending);
  allow(PartitionState::PlanPending, PartitionState::Reserved);
  allow(PartitionState::PlanPending, PartitionState::Unpartitioned);
  allow(PartitionState::PlanPending, PartitionState::RevalidationRequired);
  allow(PartitionState::PlanPending, PartitionState::Failed);
  allow(PartitionState::PlanPending, PartitionState::Offline);
  allow(PartitionState::PlanPending, PartitionState::Retired);

  allow(PartitionState::Reserved, PartitionState::Reserved);
  allow(PartitionState::Reserved, PartitionState::Creating);
  allow(PartitionState::Reserved, PartitionState::Active);
  allow(PartitionState::Reserved, PartitionState::Unpartitioned);
  allow(PartitionState::Reserved, PartitionState::RevalidationRequired);
  allow(PartitionState::Reserved, PartitionState::Destroying);
  allow(PartitionState::Reserved, PartitionState::Failed);
  allow(PartitionState::Reserved, PartitionState::Offline);
  allow(PartitionState::Reserved, PartitionState::Retired);

  allow(PartitionState::Creating, PartitionState::Active);
  // A creation attempt that the backend positively refused without applying
  // anything returns the partition to its reserved state, where it can be
  // retried under the same authority.
  allow(PartitionState::Creating, PartitionState::Reserved);
  allow(PartitionState::Creating, PartitionState::Failed);
  allow(PartitionState::Creating, PartitionState::RevalidationRequired);
  allow(PartitionState::Creating, PartitionState::Offline);
  allow(PartitionState::Creating, PartitionState::Retired);
  allow(PartitionState::Creating, PartitionState::Destroying);

  allow(PartitionState::Active, PartitionState::Active);
  allow(PartitionState::Active, PartitionState::Draining);
  allow(PartitionState::Active, PartitionState::Destroying);
  allow(PartitionState::Active, PartitionState::ReconfigurationRequired);
  allow(PartitionState::Active, PartitionState::Degraded);
  allow(PartitionState::Active, PartitionState::Offline);
  allow(PartitionState::Active, PartitionState::Failed);
  allow(PartitionState::Active, PartitionState::Retired);
  allow(PartitionState::Active, PartitionState::RevalidationRequired);

  allow(PartitionState::Draining, PartitionState::Draining);
  allow(PartitionState::Draining, PartitionState::Active);
  allow(PartitionState::Draining, PartitionState::Destroying);
  allow(PartitionState::Draining, PartitionState::ReconfigurationRequired);
  allow(PartitionState::Draining, PartitionState::Degraded);
  allow(PartitionState::Draining, PartitionState::Offline);
  allow(PartitionState::Draining, PartitionState::Failed);
  allow(PartitionState::Draining, PartitionState::Retired);
  allow(PartitionState::Draining, PartitionState::RevalidationRequired);

  allow(PartitionState::ReconfigurationRequired, PartitionState::ReconfigurationRequired);
  allow(PartitionState::ReconfigurationRequired, PartitionState::Draining);
  allow(PartitionState::ReconfigurationRequired, PartitionState::Active);
  allow(PartitionState::ReconfigurationRequired, PartitionState::Destroying);
  allow(PartitionState::ReconfigurationRequired, PartitionState::RevalidationRequired);
  allow(PartitionState::ReconfigurationRequired, PartitionState::Offline);
  allow(PartitionState::ReconfigurationRequired, PartitionState::Failed);
  allow(PartitionState::ReconfigurationRequired, PartitionState::Retired);

  allow(PartitionState::Destroying, PartitionState::Retired);
  allow(PartitionState::Destroying, PartitionState::Failed);
  allow(PartitionState::Destroying, PartitionState::RevalidationRequired);
  allow(PartitionState::Destroying, PartitionState::Offline);

  allow(PartitionState::RevalidationRequired, PartitionState::RevalidationRequired);
  allow(PartitionState::RevalidationRequired, PartitionState::Active);
  allow(PartitionState::RevalidationRequired, PartitionState::Unpartitioned);
  allow(PartitionState::RevalidationRequired, PartitionState::Degraded);
  allow(PartitionState::RevalidationRequired, PartitionState::Draining);
  allow(PartitionState::RevalidationRequired, PartitionState::ReconfigurationRequired);
  allow(PartitionState::RevalidationRequired, PartitionState::Destroying);
  allow(PartitionState::RevalidationRequired, PartitionState::Offline);
  allow(PartitionState::RevalidationRequired, PartitionState::Failed);
  allow(PartitionState::RevalidationRequired, PartitionState::Retired);

  allow(PartitionState::Degraded, PartitionState::Degraded);
  allow(PartitionState::Degraded, PartitionState::Active);
  allow(PartitionState::Degraded, PartitionState::Draining);
  allow(PartitionState::Degraded, PartitionState::Destroying);
  allow(PartitionState::Degraded, PartitionState::RevalidationRequired);
  allow(PartitionState::Degraded, PartitionState::Offline);
  allow(PartitionState::Degraded, PartitionState::Failed);
  allow(PartitionState::Degraded, PartitionState::Retired);

  allow(PartitionState::Offline, PartitionState::Offline);
  allow(PartitionState::Offline, PartitionState::RevalidationRequired);
  allow(PartitionState::Offline, PartitionState::Retired);
  allow(PartitionState::Offline, PartitionState::Failed);

  // Retired is terminal: no transition out of it is legal, which is what
  // prevents a stale request from resurrecting retired authority.
  allow(PartitionState::Failed, PartitionState::Retired);

  return table;
}

const std::array<std::uint32_t, kPartitionStateCount>& transition_table() {
  static const std::array<std::uint32_t, kPartitionStateCount> table = build_transition_table();
  return table;
}

}  // namespace

const char* to_string(PartitionState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  if (index >= kPartitionStateCount) {
    return "UNKNOWN";
  }
  return kStateNames[index];
}

const char* describe_state(PartitionState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  if (index >= kPartitionStateCount) {
    return "unrecognised partition state";
  }
  return kStateDescriptions[index];
}

Result<PartitionState> partition_state_from_string(std::string_view name) {
  for (std::size_t index = 0; index < kPartitionStateCount; ++index) {
    if (name == kStateNames[index]) {
      return static_cast<PartitionState>(index);
    }
  }
  return make_error(ErrorCode::InvalidArgument, "unknown partition state", std::string(name));
}

bool is_terminal_state(PartitionState state) noexcept {
  return state == PartitionState::Retired || state == PartitionState::Failed;
}

bool is_transient_state(PartitionState state) noexcept {
  switch (state) {
    case PartitionState::PlanPending:
    case PartitionState::Reserved:
    case PartitionState::Creating:
    case PartitionState::Destroying:
      return true;
    default:
      return false;
  }
}

bool holds_capacity(PartitionState state) noexcept {
  switch (state) {
    case PartitionState::Reserved:
    case PartitionState::Creating:
    case PartitionState::Active:
    case PartitionState::Draining:
    case PartitionState::ReconfigurationRequired:
    case PartitionState::Degraded:
    case PartitionState::RevalidationRequired:
    case PartitionState::Destroying:
    case PartitionState::Offline:
      return true;
    default:
      return false;
  }
}

bool holds_reservation(PartitionState state) noexcept {
  switch (state) {
    case PartitionState::Reserved:
    case PartitionState::Creating:
      return true;
    default:
      return false;
  }
}

bool allows_new_assignment(PartitionState state) noexcept { return state == PartitionState::Active; }

bool is_live_state(PartitionState state) noexcept {
  switch (state) {
    case PartitionState::Active:
    case PartitionState::Draining:
    case PartitionState::Degraded:
    case PartitionState::ReconfigurationRequired:
    case PartitionState::RevalidationRequired:
      return true;
    default:
      return false;
  }
}

bool requires_revalidation_after_restart(PartitionState state) noexcept {
  return holds_capacity(state) || is_live_state(state);
}

Status validate_transition(PartitionState from, PartitionState to) noexcept {
  const auto from_index = static_cast<std::size_t>(from);
  const auto to_index = static_cast<std::size_t>(to);
  if (from_index >= kPartitionStateCount || to_index >= kPartitionStateCount) {
    return failure(ErrorCode::InvalidArgument, "partition state out of range");
  }
  if (from == to) {
    return is_idempotent_transition(from, to)
               ? success()
               : failure(ErrorCode::InvalidTransition,
                         "repeating this transition is not idempotent",
                         std::string(to_string(from)) + " -> " + to_string(to));
  }
  // Deterministic precedence: terminal, then transient, then matrix.
  if (is_terminal_state(from) && from != PartitionState::Failed) {
    return failure(ErrorCode::InvalidTransition, "retired authority can never be resurrected",
                   std::string(to_string(from)) + " -> " + to_string(to));
  }
  const std::uint32_t allowed = transition_table()[from_index];
  if ((allowed & (1u << static_cast<std::uint32_t>(to_index))) == 0) {
    return failure(ErrorCode::InvalidTransition, "illegal partition lifecycle transition",
                   std::string(to_string(from)) + " -> " + to_string(to));
  }
  return success();
}

bool is_idempotent_transition(PartitionState from, PartitionState to) noexcept {
  if (from != to) {
    return false;
  }
  return !is_transient_state(from);
}

}  // namespace apf
