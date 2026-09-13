#include "apf/partition.hpp"

#include <algorithm>

namespace apf {

const char* to_string(AssignmentState state) noexcept {
  switch (state) {
    case AssignmentState::Unassigned: return "unassigned";
    case AssignmentState::Reserved: return "reserved";
    case AssignmentState::Assigned: return "assigned";
    case AssignmentState::Releasing: return "releasing";
    case AssignmentState::Count: return "count";
  }
  return "unassigned";
}

AssignmentState assignment_state_from_string(std::string_view name) {
  if (name == "reserved") return AssignmentState::Reserved;
  if (name == "assigned") return AssignmentState::Assigned;
  if (name == "releasing") return AssignmentState::Releasing;
  return AssignmentState::Unassigned;
}

const char* to_string(DrainState state) noexcept {
  switch (state) {
    case DrainState::NotDraining: return "not_draining";
    case DrainState::Draining: return "draining";
    case DrainState::DrainComplete: return "drain_complete";
    case DrainState::DrainBlocked: return "drain_blocked";
    case DrainState::Count: return "count";
  }
  return "not_draining";
}

DrainState drain_state_from_string(std::string_view name) {
  if (name == "draining") return DrainState::Draining;
  if (name == "drain_complete") return DrainState::DrainComplete;
  if (name == "drain_blocked") return DrainState::DrainBlocked;
  return DrainState::NotDraining;
}

bool PartitionRecord::supersedes(PartitionGeneration other) const noexcept {
  return std::find(superseded_generations.begin(), superseded_generations.end(), other) !=
         superseded_generations.end();
}

Status PartitionRecord::validate() const {
  if (!id.valid()) {
    return failure(ErrorCode::InvalidArgument, "partition identity is not set");
  }
  if (!generation.known()) {
    return failure(ErrorCode::InvalidArgument, "partition generation is not set");
  }
  if (!accelerator.valid()) {
    return failure(ErrorCode::InvalidArgument, "partition does not name an accelerator", id.str());
  }
  if (!accelerator_generation.known()) {
    return failure(ErrorCode::InvalidArgument,
                   "partition does not bind an accelerator generation", id.str());
  }
  if (!profile.valid()) {
    return failure(ErrorCode::InvalidArgument, "partition does not name a profile", id.str());
  }
  if (resources.empty()) {
    return failure(ErrorCode::InvalidArgument, "partition owns no resource", id.str());
  }
  const auto state_index = static_cast<std::size_t>(state);
  if (state_index >= kPartitionStateCount) {
    return failure(ErrorCode::CorruptState, "partition lifecycle value is invalid", id.str());
  }
  for (const PartitionGeneration previous : superseded_generations) {
    if (!previous.known()) {
      return failure(ErrorCode::CorruptState, "superseded generation is invalid", id.str());
    }
    if (previous.value() >= generation.value()) {
      return failure(ErrorCode::CorruptState,
                     "a superseded generation is not older than the current generation",
                     id.str());
    }
  }
  for (std::size_t index = 0; index < superseded_generations.size(); ++index) {
    for (std::size_t other = index + 1; other < superseded_generations.size(); ++other) {
      if (superseded_generations[index] == superseded_generations[other]) {
        return failure(ErrorCode::CorruptState, "duplicate superseded generation", id.str());
      }
    }
  }
  if (held_bucket.has_value()) {
    const auto bucket_index = static_cast<std::size_t>(*held_bucket);
    if (bucket_index >= kCapacityBucketCount || *held_bucket == CapacityBucket::Unavailable ||
        *held_bucket == CapacityBucket::Free) {
      return failure(ErrorCode::CorruptState, "partition names an impossible capacity bucket",
                     id.str());
    }
    if (!holds_capacity(state)) {
      return failure(ErrorCode::CorruptState,
                     "partition holds capacity in a state that cannot own it", id.str());
    }
  } else if (holds_capacity(state) && state != PartitionState::RevalidationRequired) {
    // RevalidationRequired is the one state in which the runtime may hold
    // capacity it cannot attribute: an observed partition whose capacity the
    // ledger could not account for is recorded honestly rather than claiming a
    // bucket it does not hold.
    return failure(ErrorCode::CorruptState,
                   "partition is in a capacity-holding state without naming its bucket", id.str());
  }
  if (drain.state == DrainState::Draining && state != PartitionState::Draining) {
    return failure(ErrorCode::CorruptState,
                   "drain is in progress while the partition is not draining", id.str());
  }
  if (drain.state == DrainState::DrainComplete && drain.completed_at_ms == 0) {
    return failure(ErrorCode::CorruptState,
                   "drain completed without recording when it completed", id.str());
  }
  if (drain.outstanding_assignments > drain.total_assignments) {
    return failure(ErrorCode::CorruptState,
                   "drain outstanding assignments exceed the total", id.str());
  }
  if (drain.state == DrainState::DrainComplete && drain.outstanding_assignments != 0) {
    return failure(ErrorCode::CorruptState,
                   "drain is complete while assignments remain outstanding", id.str());
  }
  if (drain.outstanding_assignments > 0 && state != PartitionState::Draining &&
      !allows_new_assignment(state)) {
    return failure(ErrorCode::CorruptState,
                   "outstanding drain work is recorded on a partition that cannot drain", id.str());
  }
  return success();
}

Status PartitionAssignment::validate() const {
  if (!id.valid()) {
    return failure(ErrorCode::InvalidArgument, "assignment identity is not set");
  }
  if (!partition.valid() || !partition_generation.known()) {
    return failure(ErrorCode::InvalidArgument, "assignment does not bind a partition generation");
  }
  if (!accelerator.valid() || !accelerator_generation.known()) {
    return failure(ErrorCode::InvalidArgument, "assignment does not bind an accelerator generation");
  }
  if (workload_id.empty()) {
    return failure(ErrorCode::InvalidArgument, "assignment does not name a workload");
  }
  return success();
}

}  // namespace apf
