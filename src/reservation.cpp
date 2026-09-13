#include "apf/reservation.hpp"

namespace apf {

const char* to_string(ReservationLifecycle value) noexcept {
  switch (value) {
    case ReservationLifecycle::Pending: return "PENDING";
    case ReservationLifecycle::Active: return "ACTIVE";
    case ReservationLifecycle::Committed: return "COMMITTED";
    case ReservationLifecycle::Released: return "RELEASED";
    case ReservationLifecycle::RolledBack: return "ROLLED_BACK";
    case ReservationLifecycle::Expired: return "EXPIRED";
    case ReservationLifecycle::Fenced: return "FENCED";
    case ReservationLifecycle::Failed: return "FAILED";
    case ReservationLifecycle::Count: return "UNKNOWN";
  }
  return "UNKNOWN";
}

ReservationLifecycle reservation_lifecycle_from_string(std::string_view name) {
  if (name == "PENDING") return ReservationLifecycle::Pending;
  if (name == "ACTIVE") return ReservationLifecycle::Active;
  if (name == "COMMITTED") return ReservationLifecycle::Committed;
  if (name == "RELEASED") return ReservationLifecycle::Released;
  if (name == "ROLLED_BACK") return ReservationLifecycle::RolledBack;
  if (name == "EXPIRED") return ReservationLifecycle::Expired;
  if (name == "FENCED") return ReservationLifecycle::Fenced;
  if (name == "FAILED") return ReservationLifecycle::Failed;
  return ReservationLifecycle::Count;
}

bool reservation_holds_capacity(ReservationLifecycle value) noexcept {
  return value == ReservationLifecycle::Pending || value == ReservationLifecycle::Active;
}

bool reservation_is_terminal(ReservationLifecycle value) noexcept {
  switch (value) {
    case ReservationLifecycle::Committed:
    case ReservationLifecycle::Released:
    case ReservationLifecycle::RolledBack:
    case ReservationLifecycle::Expired:
    case ReservationLifecycle::Fenced:
    case ReservationLifecycle::Failed:
      return true;
    default:
      return false;
  }
}

Status PartitionReservation::validate() const {
  if (!id.valid()) {
    return failure(ErrorCode::InvalidArgument, "reservation identity is not set");
  }
  if (!plan.valid() || !plan_generation.known()) {
    return failure(ErrorCode::InvalidArgument, "reservation does not bind a plan generation");
  }
  if (!accelerator.valid() || !accelerator_generation.known()) {
    return failure(ErrorCode::InvalidArgument,
                   "reservation does not bind an accelerator generation");
  }
  if (!capability_generation.known()) {
    return failure(ErrorCode::InvalidArgument, "reservation does not bind a capability generation");
  }
  if (!policy_generation.known()) {
    return failure(ErrorCode::InvalidArgument, "reservation does not bind a policy generation");
  }
  if (partitions.size() != partition_generations.size()) {
    return failure(ErrorCode::CorruptState,
                   "reservation partition identities and generations disagree");
  }
  // A destructive reservation holds only the net additional capacity it needs,
  // which may legitimately be nothing at all.
  if (resources.empty() && reservation_holds_capacity(lifecycle) && !destructive) {
    return failure(ErrorCode::InvalidArgument, "reservation holds no resource");
  }
  if (static_cast<std::size_t>(lifecycle) >= static_cast<std::size_t>(ReservationLifecycle::Count)) {
    return failure(ErrorCode::CorruptState, "reservation lifecycle value is invalid");
  }
  return success();
}

const char* to_string(AttemptKind kind) noexcept {
  switch (kind) {
    case AttemptKind::CreatePartition: return "CREATE_PARTITION";
    case AttemptKind::DestroyPartition: return "DESTROY_PARTITION";
    case AttemptKind::ReconfigureLayout: return "RECONFIGURE_LAYOUT";
    case AttemptKind::Count: return "UNKNOWN";
  }
  return "UNKNOWN";
}

AttemptKind attempt_kind_from_string(std::string_view name) {
  if (name == "CREATE_PARTITION") return AttemptKind::CreatePartition;
  if (name == "DESTROY_PARTITION") return AttemptKind::DestroyPartition;
  if (name == "RECONFIGURE_LAYOUT") return AttemptKind::ReconfigureLayout;
  return AttemptKind::Count;
}

const char* to_string(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::Registered: return "REGISTERED";
    case AttemptState::Dispatched: return "DISPATCHED";
    case AttemptState::Succeeded: return "SUCCEEDED";
    case AttemptState::Failed: return "FAILED";
    case AttemptState::OutcomeUnknown: return "OUTCOME_UNKNOWN";
    case AttemptState::Reconciled: return "RECONCILED";
    case AttemptState::Count: return "UNKNOWN";
  }
  return "UNKNOWN";
}

AttemptState attempt_state_from_string(std::string_view name) {
  if (name == "REGISTERED") return AttemptState::Registered;
  if (name == "DISPATCHED") return AttemptState::Dispatched;
  if (name == "SUCCEEDED") return AttemptState::Succeeded;
  if (name == "FAILED") return AttemptState::Failed;
  if (name == "OUTCOME_UNKNOWN") return AttemptState::OutcomeUnknown;
  if (name == "RECONCILED") return AttemptState::Reconciled;
  return AttemptState::Count;
}

bool attempt_settled(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::Succeeded:
    case AttemptState::Failed:
    case AttemptState::Reconciled:
      return true;
    default:
      return false;
  }
}

Status PartitionAttempt::validate() const {
  if (!id.valid()) {
    return failure(ErrorCode::InvalidArgument, "attempt identity is not set");
  }
  if (!reservation.valid()) {
    return failure(ErrorCode::InvalidArgument, "attempt does not bind a reservation");
  }
  if (static_cast<std::size_t>(kind) >= static_cast<std::size_t>(AttemptKind::Count)) {
    return failure(ErrorCode::CorruptState, "attempt kind is invalid");
  }
  if (static_cast<std::size_t>(state) >= static_cast<std::size_t>(AttemptState::Count)) {
    return failure(ErrorCode::CorruptState, "attempt state is invalid");
  }
  if (state == AttemptState::Dispatched && dispatched_at_ms == 0) {
    return failure(ErrorCode::CorruptState, "attempt is dispatched but carries no dispatch time");
  }
  if (attempt_settled(state) && settled_at_ms == 0) {
    return failure(ErrorCode::CorruptState, "attempt is settled but carries no settle time");
  }
  return success();
}

}  // namespace apf
