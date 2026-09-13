#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace apf {

/// Typed failure classification. Callers never need to parse strings to
/// understand machine state; every code names a distinct systems condition.
enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidRequest,
  InvalidArgument,
  NotFound,
  AlreadyExists,
  UnsupportedCapability,
  UnknownCapability,
  StaleGeneration,
  StalePlan,
  StaleReservation,
  StaleWorker,
  StaleCoordinatorEpoch,
  StaleEvidence,
  StalePartitionGeneration,
  InvalidTransition,
  InsufficientCapacity,
  Fragmented,
  IsolationMismatch,
  ReconfigurationRequired,
  DrainRequired,
  PolicyRejected,
  ReservationConflict,
  DeviceUnavailable,
  BackendFailure,
  PersistenceCorruption,
  ProtocolViolation,
  AmbiguousCompletion,
  ReconciliationRequired,
  RevalidationRequired,
  Overflow,
  Underflow,
  LimitExceeded,
  Closed,
  Busy,
  Cancelled,
  CapabilityChanged,
  PolicyChanged,
  GenerationRegression,
  CorruptState,
  Fenced,
  Internal,
};

/// Stable machine-readable name, e.g. "insufficient_capacity".
const char* to_string(ErrorCode code) noexcept;

/// Parse the stable name back to a code. Returns ErrorCode::Internal when the
/// name is not recognised.
ErrorCode error_code_from_string(std::string_view name) noexcept;

struct Error {
  ErrorCode code{ErrorCode::Internal};
  std::string message;
  std::string detail;
};

Error make_error(ErrorCode code, std::string message, std::string detail = {});

std::string to_string(const Error& error);

template <class T>
class Result {
 public:
  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  bool ok() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  const Error& error() const noexcept {
    static const Error kOk{};
    return ok() ? kOk : std::get<1>(storage_);
  }

  T& value() & { return std::get<0>(storage_); }
  const T& value() const& { return std::get<0>(storage_); }
  T&& value() && { return std::get<0>(std::move(storage_)); }

  T* operator->() { return &std::get<0>(storage_); }
  const T* operator->() const { return &std::get<0>(storage_); }
  T& operator*() { return std::get<0>(storage_); }
  const T& operator*() const { return std::get<0>(storage_); }

  T value_or(T fallback) const {
    return ok() ? std::get<0>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, Error> storage_;
};

template <>
class Result<void> {
 public:
  Result() noexcept = default;
  Result(Error error) : error_(std::move(error)) {}

  bool ok() const noexcept { return !error_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  const Error& error() const noexcept {
    static const Error kOk{};
    return error_ ? *error_ : kOk;
  }

 private:
  std::optional<Error> error_;
};

using Status = Result<void>;

inline Status success() noexcept { return Status{}; }

inline Status failure(ErrorCode code, std::string message, std::string detail = {}) {
  return Status{make_error(code, std::move(message), std::move(detail))};
}

}  // namespace apf
