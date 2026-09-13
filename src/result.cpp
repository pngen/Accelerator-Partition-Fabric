#include "apf/result.hpp"

namespace apf {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidRequest: return "invalid_request";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::AlreadyExists: return "already_exists";
    case ErrorCode::UnsupportedCapability: return "unsupported_capability";
    case ErrorCode::UnknownCapability: return "unknown_capability";
    case ErrorCode::StaleGeneration: return "stale_generation";
    case ErrorCode::StalePlan: return "stale_plan";
    case ErrorCode::StaleReservation: return "stale_reservation";
    case ErrorCode::StaleWorker: return "stale_worker";
    case ErrorCode::StaleCoordinatorEpoch: return "stale_coordinator_epoch";
    case ErrorCode::StaleEvidence: return "stale_evidence";
    case ErrorCode::StalePartitionGeneration: return "stale_partition_generation";
    case ErrorCode::InvalidTransition: return "invalid_transition";
    case ErrorCode::InsufficientCapacity: return "insufficient_capacity";
    case ErrorCode::Fragmented: return "fragmented";
    case ErrorCode::IsolationMismatch: return "isolation_mismatch";
    case ErrorCode::ReconfigurationRequired: return "reconfiguration_required";
    case ErrorCode::DrainRequired: return "drain_required";
    case ErrorCode::PolicyRejected: return "policy_rejected";
    case ErrorCode::ReservationConflict: return "reservation_conflict";
    case ErrorCode::DeviceUnavailable: return "device_unavailable";
    case ErrorCode::BackendFailure: return "backend_failure";
    case ErrorCode::PersistenceCorruption: return "persistence_corruption";
    case ErrorCode::ProtocolViolation: return "protocol_violation";
    case ErrorCode::AmbiguousCompletion: return "ambiguous_completion";
    case ErrorCode::ReconciliationRequired: return "reconciliation_required";
    case ErrorCode::RevalidationRequired: return "revalidation_required";
    case ErrorCode::Overflow: return "overflow";
    case ErrorCode::Underflow: return "underflow";
    case ErrorCode::LimitExceeded: return "limit_exceeded";
    case ErrorCode::Closed: return "closed";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::CapabilityChanged: return "capability_changed";
    case ErrorCode::PolicyChanged: return "policy_changed";
    case ErrorCode::GenerationRegression: return "generation_regression";
    case ErrorCode::CorruptState: return "corrupt_state";
    case ErrorCode::Fenced: return "fenced";
    case ErrorCode::Internal: return "internal";
  }
  return "internal";
}

ErrorCode error_code_from_string(std::string_view name) noexcept {
  for (std::uint16_t raw = 0; raw <= static_cast<std::uint16_t>(ErrorCode::Internal); ++raw) {
    const auto code = static_cast<ErrorCode>(raw);
    if (name == to_string(code)) {
      return code;
    }
  }
  return ErrorCode::Internal;
}

Error make_error(ErrorCode code, std::string message, std::string detail) {
  Error error;
  error.code = code;
  error.message = std::move(message);
  error.detail = std::move(detail);
  return error;
}

std::string to_string(const Error& error) {
  std::string out = to_string(error.code);
  if (!error.message.empty()) {
    out += ": ";
    out += error.message;
  }
  if (!error.detail.empty()) {
    out += " [";
    out += error.detail;
    out += "]";
  }
  return out;
}

}  // namespace apf
