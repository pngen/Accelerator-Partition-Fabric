#pragma once

#include "apf/accounting.hpp"
#include "apf/accelerator.hpp"
#include "apf/backend.hpp"
#include "apf/evidence.hpp"
#include "apf/id.hpp"
#include "apf/limits.hpp"
#include "apf/partition.hpp"
#include "apf/policy.hpp"
#include "apf/profile.hpp"
#include "apf/reservation.hpp"
#include "apf/result.hpp"
#include "apf/snapshot.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apf {

/// Explicit little-endian writer with bounded lengths and a sticky error. Once
/// a bound is violated the writer records the failure and every later write is
/// a no-op, so callers check the error once at the end.
class ByteWriter {
 public:
  explicit ByteWriter(const Limits* limits = nullptr, std::size_t reserve = 256);

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);

  /// Length-prefixed text. The declared length is validated against max_len and
  /// against the writer bound before anything is appended.
  void text(std::string_view value, std::size_t max_len);
  void text(std::string_view value) { text(value, limits_ ? limits_->max_metadata_bytes : 4096); }

  void blob(const std::vector<std::uint8_t>& value, std::size_t max_len);

  bool ok() const noexcept { return error_.code == ErrorCode::Ok; }
  const Error& error() const noexcept { return error_; }
  const std::vector<std::uint8_t>& data() const noexcept { return buffer_; }
  std::vector<std::uint8_t> take() noexcept { return std::move(buffer_); }
  std::size_t size() const noexcept { return buffer_.size(); }

  void fail(Error error);
  void fail(ErrorCode code, std::string message, std::string detail = {});

 private:
  void reserve_for(std::size_t extra);

  const Limits* limits_{nullptr};
  std::vector<std::uint8_t> buffer_;
  Error error_{};
};

/// Explicit little-endian reader with a sticky error. Every declared length is
/// validated against the remaining bytes and the configured maximum before any
/// allocation happens.
class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size, const Limits& limits);

  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  std::int64_t i64();
  bool boolean();

  std::string text(std::size_t max_len);
  std::vector<std::uint8_t> blob(std::size_t max_len);

  bool ok() const noexcept { return error_.code == ErrorCode::Ok; }
  const Error& error() const noexcept { return error_; }
  const Limits& limits() const noexcept { return limits_; }
  std::size_t remaining() const noexcept { return size_ - offset_; }
  bool exhausted() const noexcept { return offset_ == size_; }

  void fail(Error error);
  void fail(ErrorCode code, std::string message, std::string detail = {});

  /// Reads a bounded list. max_count is enforced before the loop starts and the
  /// minimum encoded size of an element is used to reject impossible counts
  /// without allocating.
  template <class T, class Decode>
  std::vector<T> list(std::size_t max_count, std::size_t min_element_bytes, Decode&& decode) {
    std::vector<T> out;
    const std::uint32_t declared = u32();
    if (!ok()) {
      return out;
    }
    if (declared > max_count) {
      fail(ErrorCode::LimitExceeded, "declared element count exceeds the configured bound",
           std::to_string(declared) + " > " + std::to_string(max_count));
      return out;
    }
    if (min_element_bytes > 0 && static_cast<std::size_t>(declared) * min_element_bytes > remaining()) {
      fail(ErrorCode::ProtocolViolation, "declared element count cannot fit in the remaining bytes",
           std::to_string(declared));
      return out;
    }
    out.reserve(declared);
    for (std::uint32_t i = 0; i < declared && ok(); ++i) {
      out.push_back(decode(*this));
    }
    return out;
  }

  /// Succeeds only when the reader is healthy and fully consumed.
  Status finish() const;

 private:
  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  std::size_t offset_{0};
  Limits limits_{};
  Error error_{};
};

/// True when the byte string is valid UTF-8 without overlong forms or lone
/// surrogates.
bool is_valid_utf8(std::string_view text) noexcept;

namespace codec {

// --- primitive model encoders shared by persistence and the wire protocol ----
void encode(ByteWriter& writer, const ResourceVector& value);
ResourceVector decode(ByteReader& reader);

void encode(ByteWriter& writer, const Limits& value);
Limits decode_limits(ByteReader& reader);

void encode(ByteWriter& writer, const EvidenceStamp& value);
EvidenceStamp decode_evidence(ByteReader& reader);

void encode(ByteWriter& writer, const IsolationSet& value);
IsolationSet decode_isolation(ByteReader& reader);

void encode(ByteWriter& writer, const ProfileAlignment& value);
ProfileAlignment decode_profile_alignment(ByteReader& reader);

void encode(ByteWriter& writer, const PartitionProfile& value);
PartitionProfile decode_profile(ByteReader& reader);

void encode(ByteWriter& writer, const ProfileCombinationRule& value);
ProfileCombinationRule decode_combination_rule(ByteReader& reader);

void encode(ByteWriter& writer, const PartitionCapability& value);
PartitionCapability decode_capability(ByteReader& reader);

void encode(ByteWriter& writer, const DeviceIdentifiers& value);
DeviceIdentifiers decode_identifiers(ByteReader& reader);

void encode(ByteWriter& writer, const LocalityDomain& value);
LocalityDomain decode_locality(ByteReader& reader);

void encode(ByteWriter& writer, const HealthEvidence& value);
HealthEvidence decode_health(ByteReader& reader);

void encode(ByteWriter& writer, const AcceleratorRecord& value);
AcceleratorRecord decode_accelerator(ByteReader& reader);

void encode(ByteWriter& writer, const PartitionNativeIdentity& value);
PartitionNativeIdentity decode_native_identity(ByteReader& reader);

void encode(ByteWriter& writer, const IsolationDomain& value);
IsolationDomain decode_isolation_domain(ByteReader& reader);

void encode(ByteWriter& writer, const DrainProgress& value);
DrainProgress decode_drain(ByteReader& reader);

void encode(ByteWriter& writer, const PartitionRecord& value);
PartitionRecord decode_partition(ByteReader& reader);

void encode(ByteWriter& writer, const IsolationRequirement& value);
IsolationRequirement decode_isolation_requirement(ByteReader& reader);

void encode(ByteWriter& writer, const PartitionAssignment& value);
PartitionAssignment decode_assignment(ByteReader& reader);

void encode(ByteWriter& writer, const CapacityLedger& value);
CapacityLedger decode_ledger(ByteReader& reader);

void encode(ByteWriter& writer, const PlanningPolicy& value);
PlanningPolicy decode_policy(ByteReader& reader);

void encode(ByteWriter& writer, const PartitionReservation& value);
PartitionReservation decode_reservation(ByteReader& reader);

void encode(ByteWriter& writer, const PlannedPartition& value);
PlannedPartition decode_planned_partition(ByteReader& reader);

void encode(ByteWriter& writer, const DeviceSelector& value);
DeviceSelector decode_device_selector(ByteReader& reader);

void encode(ByteWriter& writer, const PartitionRequest& value);
PartitionRequest decode_request(ByteReader& reader);

void encode(ByteWriter& writer, const PlanStep& value);
PlanStep decode_plan_step(ByteReader& reader);

void encode(ByteWriter& writer, const PlanScoreTerm& value);
PlanScoreTerm decode_plan_score_term(ByteReader& reader);

void encode(ByteWriter& writer, const CandidateEvaluation& value);
CandidateEvaluation decode_candidate(ByteReader& reader);

void encode(ByteWriter& writer, const PartitionAttempt& value);
PartitionAttempt decode_attempt(ByteReader& reader);

void encode(ByteWriter& writer, const WorkerRecord& value);
WorkerRecord decode_worker(ByteReader& reader);

void encode(ByteWriter& writer, const BackendNativePartition& value);
BackendNativePartition decode_native_partition(ByteReader& reader);

void encode(ByteWriter& writer, const BackendLayout& value);
BackendLayout decode_layout(ByteReader& reader);

void encode(ByteWriter& writer, const BackendAccelerator& value);
BackendAccelerator decode_backend_accelerator(ByteReader& reader);

void encode(ByteWriter& writer, const NativePartitionSpec& value);
NativePartitionSpec decode_native_spec(ByteReader& reader);

void encode(ByteWriter& writer, const PartitionMutationRequest& value);
PartitionMutationRequest decode_mutation_request(ByteReader& reader);

void encode(ByteWriter& writer, const BackendMutationResult& value);
BackendMutationResult decode_mutation_result(ByteReader& reader);

/// Enum helpers with explicit range checks: an out-of-range enum value on the
/// wire or on disk is a protocol/persistence violation, never silently coerced.
template <class E>
void encode_enum(ByteWriter& writer, E value) {
  writer.u8(static_cast<std::uint8_t>(value));
}

template <class E>
E decode_enum(ByteReader& reader, std::uint8_t max_exclusive, const char* what) {
  const std::uint8_t raw = reader.u8();
  if (reader.ok() && raw >= max_exclusive) {
    reader.fail(ErrorCode::ProtocolViolation, std::string("invalid ") + what + " value",
                std::to_string(raw));
    return static_cast<E>(0);
  }
  return static_cast<E>(raw);
}

}  // namespace codec
}  // namespace apf
