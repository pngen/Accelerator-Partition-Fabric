#pragma once

#include "apf/backend.hpp"
#include "apf/codec.hpp"
#include "apf/id.hpp"
#include "apf/limits.hpp"
#include "apf/reservation.hpp"
#include "apf/result.hpp"
#include "apf/snapshot.hpp"
#include "apf/transport.hpp"
#include "apf/version.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apf {

/// Wire identity of the framed protocol: 'A' 'P' 'F' '1'.
inline constexpr std::uint32_t kFrameMagic = 0x31465041u;
inline constexpr std::size_t kFrameHeaderSize = 56;

/// Message types. The numeric values are part of the protocol contract.
enum class MessageType : std::uint16_t {
  Invalid = 0,
  Hello = 1,
  HelloAck = 2,
  RegisterWorker = 3,
  RegisterAck = 4,
  EvidencePublish = 5,
  EvidenceAck = 6,
  MutationRequest = 7,
  MutationResult = 8,
  QueryRequest = 9,
  QueryResponse = 10,
  FenceWorker = 11,
  FenceAck = 12,
  Heartbeat = 13,
  HeartbeatAck = 14,
  Shutdown = 15,
  ErrorResponse = 16,
  ReconcileRequest = 17,
  ReconcileResponse = 18,
  AdminRequest = 19,
  AdminResponse = 20,
  CommandRequest = 21,
  CommandResponse = 22,
  Count = 23,
};

const char* to_string(MessageType type) noexcept;

enum class FrameFlag : std::uint32_t {
  None = 0,
  Request = 1u << 0,
  Response = 1u << 1,
  Error = 1u << 2,
  Fenced = 1u << 3,
  Final = 1u << 4,
};

inline std::uint32_t operator|(FrameFlag lhs, FrameFlag rhs) noexcept {
  return static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs);
}

inline std::uint32_t to_flags(FrameFlag flag) noexcept {
  return static_cast<std::uint32_t>(flag);
}

/// A decoded frame. Payload layout is defined per message type.
struct Frame {
  MessageType type{MessageType::Invalid};
  std::uint32_t flags{0};
  std::uint64_t sequence{0};
  CoordinatorEpoch coordinator_epoch{};
  WorkerId worker{};
  WorkerBootId worker_boot{};
  std::vector<std::uint8_t> payload;
};

/// The header as it appears on the wire.
struct FrameHeader {
  std::uint32_t magic{kFrameMagic};
  std::uint16_t version{APF_PROTOCOL_VERSION};
  MessageType type{MessageType::Invalid};
  std::uint32_t flags{0};
  std::uint32_t payload_len{0};
  std::uint32_t reserved{0};
  std::uint64_t sequence{0};
  CoordinatorEpoch coordinator_epoch{};
  WorkerId worker{};
  WorkerBootId worker_boot{};
  std::uint32_t checksum{0};
};

/// Frames the payload with a header and integrity check.
Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame, const Limits& limits);
/// Decodes and validates a complete frame: magic, version, type range, declared
/// length, reserved field, and integrity check are all verified before the
/// payload is parsed.
Result<Frame> decode_frame(const std::uint8_t* data, std::size_t size, const Limits& limits);
/// Validates just the header. Declared payload length is checked against the
/// configured maximum before any buffer is allocated for it.
Result<FrameHeader> decode_frame_header(const std::uint8_t* data, std::size_t size,
                                        const Limits& limits);

/// Bounded framed channel over a TCP socket.
class FramedChannel {
 public:
  FramedChannel(TcpSocket socket, Limits limits);
  ~FramedChannel();

  FramedChannel(const FramedChannel&) = delete;
  FramedChannel& operator=(const FramedChannel&) = delete;

  Status send(const Frame& frame);
  /// Blocks until a complete frame arrives or the peer closes.
  Result<Frame> receive();

  bool valid() const noexcept;
  void close() noexcept;
  std::uint64_t frames_sent() const noexcept;
  std::uint64_t frames_received() const noexcept;
  const Limits& limits() const noexcept { return limits_; }

  /// Sequences outbound frames with a per-channel counter.
  std::uint64_t next_sequence() noexcept;

 private:
  TcpSocket socket_;
  Limits limits_{};
  std::uint64_t send_sequence_{0};
  std::uint64_t recv_sequence_{0};
  std::uint64_t frames_sent_{0};
  std::uint64_t frames_received_{0};
};

// ---------------------------------------------------------------------------
// Message payloads
// ---------------------------------------------------------------------------

/// Requests sent to a coordinator use the sender's view of the coordinator
/// epoch; the coordinator rejects any value it has already superseded.
struct HelloMessage {
  std::uint16_t protocol_version{APF_PROTOCOL_VERSION};
  CoordinatorEpoch known_epoch{};
  WorkerId worker{};
  WorkerBootId worker_boot{};
  std::string backend;
  std::string agent_version;
  std::string instance;
};

struct HelloAckMessage {
  bool accepted{false};
  CoordinatorEpoch epoch{};
  WorkerId assigned_worker{};
  std::uint64_t session_nonce{0};
  Limits limits{};
  std::string reason;
};

struct RegisterWorkerMessage {
  WorkerRecord worker{};
  std::vector<BackendAccelerator> devices{};
  std::uint64_t started_at_ms{0};
};

struct RegisterAckMessage {
  bool accepted{false};
  CoordinatorEpoch epoch{};
  std::vector<AcceleratorId> registered_accelerators;
  std::vector<std::string> rejected_devices;
  std::string reason;
};

struct EvidencePublishMessage {
  std::vector<BackendAccelerator> devices{};
  std::vector<BackendLayout> layouts{};
  std::uint64_t published_at_ms{0};
  bool revalidate{false};
};

struct EvidenceAckMessage {
  bool accepted{false};
  std::vector<AcceleratorId> accelerators{};
  std::vector<PartitionId> revalidated_partitions{};
  std::string reason;
};

struct MutationRequestMessage {
  PartitionAttemptId attempt{};
  PartitionReservationId reservation{};
  AttemptKind kind{AttemptKind::CreatePartition};
  PartitionMutationRequest request{};
};

struct MutationResultMessage {
  PartitionAttemptId attempt{};
  std::uint64_t idempotency_token{0};
  BackendMutationResult result{};
};

enum class QueryKind : std::uint8_t {
  Accelerators = 0,
  Layout = 1,
  Snapshot = 2,
  Attempt = 3,
  Partitions = 4,
  Workers = 5,
  Count = 6,
};

const char* to_string(QueryKind kind) noexcept;

struct QueryRequestMessage {
  QueryKind kind{QueryKind::Accelerators};
  std::string key;
  PartitionAttemptId attempt{};
  PartitionId partition{};
  bool include_snapshot{false};
};

struct QueryResponseMessage {
  QueryKind kind{QueryKind::Accelerators};
  bool found{false};
  std::string detail;
  std::vector<BackendAccelerator> devices{};
  BackendLayout layout{};
  PartitionAttempt attempt{};
  std::vector<PartitionRecord> partitions{};
  std::vector<WorkerRecord> workers{};
  std::string snapshot_text;
};

struct FenceMessage {
  WorkerId worker{};
  WorkerBootId worker_boot{};
  CoordinatorEpoch epoch{};
  std::string reason;
};

struct FenceAckMessage {
  bool acknowledged{false};
  std::string reason;
};

struct HeartbeatMessage {
  std::uint64_t sent_at_ms{0};
  std::uint64_t applied_attempts{0};
};

struct HeartbeatAckMessage {
  CoordinatorEpoch epoch{};
  std::uint64_t received_at_ms{0};
  std::uint64_t allowed_attempts{0};
};

struct ShutdownMessage {
  std::string reason;
  bool fence{false};
};

struct ErrorResponseMessage {
  ErrorCode code{ErrorCode::Internal};
  std::string message;
  std::string detail;
  std::uint64_t sequence{0};
};

struct ReconcileRequestMessage {
  std::string stable_key;
  bool allow_adoption{false};
};

struct ReconcileResponseMessage {
  bool accepted{false};
  std::string summary;
  std::uint32_t matched{0};
  std::uint32_t missing{0};
  std::uint32_t unexpected{0};
  std::uint32_t adopted{0};
  bool revalidation_required{false};
  std::string reason;
};

/// Administrative mutation requests, kept separate from read-only queries so
/// that an inspection client can never mutate state by accident.
enum class AdminAction : std::uint8_t {
  DrainPartition = 0,
  DrainAccelerator = 1,
  CompleteDrain = 2,
  CancelDrain = 3,
  DestroyPartition = 4,
  ReleaseReservation = 5,
  FenceWorker = 6,
  ReconcileAccelerator = 7,
  AdvanceEpoch = 8,
  SaveState = 9,
  Shutdown = 10,
  Count = 11,
};

const char* to_string(AdminAction action) noexcept;

struct AdminRequestMessage {
  AdminAction action{AdminAction::DrainPartition};
  PartitionId partition{};
  AcceleratorId accelerator{};
  PartitionReservationId reservation{};
  WorkerId worker{};
  WorkerBootId worker_boot{};
  std::string reason;
  bool confirmed{false};
};

struct AdminResponseMessage {
  bool accepted{false};
  ErrorCode code{ErrorCode::Ok};
  std::string message;
  std::string detail;
};

/// Typed control commands. This is the coordinator's administrative surface:
/// read-only commands are separate from mutating ones so that a pure
/// inspection client cannot change state.
enum class CommandKind : std::uint8_t {
  /// Read-only.
  InspectSnapshot = 0,
  InspectAccelerators = 1,
  InspectWorkers = 2,
  InspectPartitions = 3,
  QueryLayout = 4,
  /// Mutating.
  PublishEvidence = 10,
  Plan = 11,
  PlanReconfiguration = 12,
  Reserve = 13,
  Create = 14,
  Destroy = 15,
  Drain = 16,
  CompleteDrain = 17,
  CancelDrain = 18,
  ReleaseReservation = 19,
  Reconcile = 20,
  FenceWorker = 21,
  AdvanceEpoch = 22,
  PersistState = 23,
  /// Asks the coordinator to shut down cleanly after acknowledging.
  ShutdownCoordinator = 24,
  Count = 25,
};

const char* to_string(CommandKind kind) noexcept;
/// Read-only commands never mutate state; the coordinator refuses them from a
/// client that has not been granted administrative capability, and refuses
/// mutating commands that were not explicitly confirmed.
bool command_is_read_only(CommandKind kind) noexcept;

struct CommandRequestMessage {
  CommandKind kind{CommandKind::InspectSnapshot};
  PartitionRequest request{};
  PartitionReservationId reservation{};
  PartitionId partition{};
  AcceleratorId accelerator{};
  PartitionPlanId plan{};
  WorkerId worker{};
  WorkerBootId worker_boot{};
  std::string stable_key;
  std::string reason;
  bool confirmed{false};
};

struct CommandResponseMessage {
  bool accepted{false};
  ErrorCode code{ErrorCode::Ok};
  std::string message;
  std::string detail;

  PartitionPlanId plan{};
  PartitionPlanGeneration plan_generation{};
  PlanOutcome plan_outcome{PlanOutcome::InvalidRequest};
  std::vector<PlanStep> plan_steps{};
  std::vector<PlannedPartition> planned_partitions{};
  std::vector<CandidateEvaluation> candidates{};

  PartitionReservationId reservation{};
  ReservationLifecycle reservation_lifecycle{ReservationLifecycle::Released};

  PartitionAttemptId attempt{};
  AttemptState attempt_state{AttemptState::Registered};
  bool outcome_unknown{false};
  bool verified_physically{false};
  std::vector<PartitionId> partitions{};
  std::vector<PartitionGeneration> generations{};

  std::uint32_t matched{0};
  std::uint32_t missing{0};
  std::uint32_t unexpected{0};
  std::uint32_t adopted{0};
  bool revalidation_required{false};

  AcceleratorId accelerator{};
  CoordinatorEpoch coordinator_epoch{};
  BackendLayout layout{};
  std::vector<BackendAccelerator> devices{};
  std::vector<WorkerRecord> workers{};
  std::vector<PartitionRecord> partition_records{};
  std::string snapshot_text;
};

void encode_payload(ByteWriter& writer, const CommandRequestMessage& message);
void decode_payload(ByteReader& reader, CommandRequestMessage& message);
void encode_payload(ByteWriter& writer, const CommandResponseMessage& message);
void decode_payload(ByteReader& reader, CommandResponseMessage& message);

// ---------------------------------------------------------------------------
// Payload codecs. Each message has an explicit encode_payload/decode_payload
// pair; pack/unpack add the framing and validate the result.
// ---------------------------------------------------------------------------

void encode_payload(ByteWriter& writer, const HelloMessage& message);
void decode_payload(ByteReader& reader, HelloMessage& message);
void encode_payload(ByteWriter& writer, const HelloAckMessage& message);
void decode_payload(ByteReader& reader, HelloAckMessage& message);
void encode_payload(ByteWriter& writer, const RegisterWorkerMessage& message);
void decode_payload(ByteReader& reader, RegisterWorkerMessage& message);
void encode_payload(ByteWriter& writer, const RegisterAckMessage& message);
void decode_payload(ByteReader& reader, RegisterAckMessage& message);
void encode_payload(ByteWriter& writer, const EvidencePublishMessage& message);
void decode_payload(ByteReader& reader, EvidencePublishMessage& message);
void encode_payload(ByteWriter& writer, const EvidenceAckMessage& message);
void decode_payload(ByteReader& reader, EvidenceAckMessage& message);
void encode_payload(ByteWriter& writer, const MutationRequestMessage& message);
void decode_payload(ByteReader& reader, MutationRequestMessage& message);
void encode_payload(ByteWriter& writer, const MutationResultMessage& message);
void decode_payload(ByteReader& reader, MutationResultMessage& message);
void encode_payload(ByteWriter& writer, const QueryRequestMessage& message);
void decode_payload(ByteReader& reader, QueryRequestMessage& message);
void encode_payload(ByteWriter& writer, const QueryResponseMessage& message);
void decode_payload(ByteReader& reader, QueryResponseMessage& message);
void encode_payload(ByteWriter& writer, const FenceMessage& message);
void decode_payload(ByteReader& reader, FenceMessage& message);
void encode_payload(ByteWriter& writer, const FenceAckMessage& message);
void decode_payload(ByteReader& reader, FenceAckMessage& message);
void encode_payload(ByteWriter& writer, const HeartbeatMessage& message);
void decode_payload(ByteReader& reader, HeartbeatMessage& message);
void encode_payload(ByteWriter& writer, const HeartbeatAckMessage& message);
void decode_payload(ByteReader& reader, HeartbeatAckMessage& message);
void encode_payload(ByteWriter& writer, const ShutdownMessage& message);
void decode_payload(ByteReader& reader, ShutdownMessage& message);
void encode_payload(ByteWriter& writer, const ErrorResponseMessage& message);
void decode_payload(ByteReader& reader, ErrorResponseMessage& message);
void encode_payload(ByteWriter& writer, const ReconcileRequestMessage& message);
void decode_payload(ByteReader& reader, ReconcileRequestMessage& message);
void encode_payload(ByteWriter& writer, const ReconcileResponseMessage& message);
void decode_payload(ByteReader& reader, ReconcileResponseMessage& message);
void encode_payload(ByteWriter& writer, const AdminRequestMessage& message);
void decode_payload(ByteReader& reader, AdminRequestMessage& message);
void encode_payload(ByteWriter& writer, const AdminResponseMessage& message);
void decode_payload(ByteReader& reader, AdminResponseMessage& message);

/// Serialises a message payload with the bounds from a Limits value.
template <class Message>
std::vector<std::uint8_t> pack_payload(const Message& message, const Limits& limits) {
  ByteWriter writer(&limits);
  encode_payload(writer, message);
  return writer.take();
}

/// Parses a message payload, rejecting trailing bytes and every malformed
/// encoding the reader detects.
template <class Message>
Result<Message> unpack_payload(const std::uint8_t* data, std::size_t size, const Limits& limits) {
  ByteReader reader(data, size, limits);
  Message message{};
  decode_payload(reader, message);
  const Status finished = reader.finish();
  if (!finished.ok()) {
    return finished.error();
  }
  return message;
}

}  // namespace apf
