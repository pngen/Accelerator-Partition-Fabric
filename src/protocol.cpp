#include "apf/protocol.hpp"

#include "apf/codec.hpp"

#include <cstring>

namespace apf {
namespace {

constexpr std::uint32_t kReservedMustBeZero = 0;

void write_u32_at(std::vector<std::uint8_t>& buffer, std::size_t offset, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    buffer[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  }
}

std::uint32_t read_u32_at(const std::uint8_t* data, std::size_t offset) {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(data[offset + static_cast<std::size_t>(shift / 8)])
             << shift;
  }
  return value;
}

/// The integrity check covers the header (with the checksum field zeroed) and
/// the payload, so a corrupted length, type, epoch or body is detected before
/// anything is parsed.
std::uint32_t frame_checksum(const std::vector<std::uint8_t>& header,
                             const std::vector<std::uint8_t>& payload) {
  std::uint64_t hash = 1469598103934665603ull;
  const auto mix = [&hash](std::uint8_t byte) {
    hash ^= byte;
    hash *= 1099511628211ull;
  };
  for (std::size_t index = 0; index < kFrameHeaderSize - 4; ++index) {
    mix(header[index]);
  }
  for (const std::uint8_t byte : payload) {
    mix(byte);
  }
  return static_cast<std::uint32_t>((hash >> 32) ^ (hash & 0xFFFFFFFFull));
}

void encode_string_list(ByteWriter& writer, const std::vector<std::string>& values,
                        std::size_t max_count) {
  writer.u32(static_cast<std::uint32_t>(values.size()));
  for (const std::string& value : values) {
    writer.text(value);
  }
  (void)max_count;
}

std::vector<std::string> decode_string_list(ByteReader& reader, std::size_t max_count) {
  return reader.list<std::string>(max_count, 4,
                                  [](ByteReader& inner) { return inner.text(256); });
}

}  // namespace

const char* to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Invalid: return "INVALID";
    case MessageType::Hello: return "HELLO";
    case MessageType::HelloAck: return "HELLO_ACK";
    case MessageType::RegisterWorker: return "REGISTER_WORKER";
    case MessageType::RegisterAck: return "REGISTER_ACK";
    case MessageType::EvidencePublish: return "EVIDENCE_PUBLISH";
    case MessageType::EvidenceAck: return "EVIDENCE_ACK";
    case MessageType::MutationRequest: return "MUTATION_REQUEST";
    case MessageType::MutationResult: return "MUTATION_RESULT";
    case MessageType::QueryRequest: return "QUERY_REQUEST";
    case MessageType::QueryResponse: return "QUERY_RESPONSE";
    case MessageType::FenceWorker: return "FENCE_WORKER";
    case MessageType::FenceAck: return "FENCE_ACK";
    case MessageType::Heartbeat: return "HEARTBEAT";
    case MessageType::HeartbeatAck: return "HEARTBEAT_ACK";
    case MessageType::Shutdown: return "SHUTDOWN";
    case MessageType::ErrorResponse: return "ERROR_RESPONSE";
    case MessageType::ReconcileRequest: return "RECONCILE_REQUEST";
    case MessageType::ReconcileResponse: return "RECONCILE_RESPONSE";
    case MessageType::AdminRequest: return "ADMIN_REQUEST";
    case MessageType::AdminResponse: return "ADMIN_RESPONSE";
    case MessageType::Count: return "COUNT";
  }
  return "INVALID";
}

const char* to_string(QueryKind kind) noexcept {
  switch (kind) {
    case QueryKind::Accelerators: return "accelerators";
    case QueryKind::Layout: return "layout";
    case QueryKind::Snapshot: return "snapshot";
    case QueryKind::Attempt: return "attempt";
    case QueryKind::Partitions: return "partitions";
    case QueryKind::Workers: return "workers";
    case QueryKind::Count: return "unknown";
  }
  return "unknown";
}

const char* to_string(AdminAction action) noexcept {
  switch (action) {
    case AdminAction::DrainPartition: return "drain_partition";
    case AdminAction::DrainAccelerator: return "drain_accelerator";
    case AdminAction::CompleteDrain: return "complete_drain";
    case AdminAction::CancelDrain: return "cancel_drain";
    case AdminAction::DestroyPartition: return "destroy_partition";
    case AdminAction::ReleaseReservation: return "release_reservation";
    case AdminAction::FenceWorker: return "fence_worker";
    case AdminAction::ReconcileAccelerator: return "reconcile_accelerator";
    case AdminAction::AdvanceEpoch: return "advance_epoch";
    case AdminAction::SaveState: return "save_state";
    case AdminAction::Shutdown: return "shutdown";
    case AdminAction::Count: return "unknown";
  }
  return "unknown";
}

Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame, const Limits& limits) {
  if (frame.type == MessageType::Invalid || frame.type >= MessageType::Count) {
    return make_error(ErrorCode::ProtocolViolation, "cannot encode an invalid message type");
  }
  if (frame.payload.size() > limits.max_frame_bytes) {
    return make_error(ErrorCode::LimitExceeded, "frame payload exceeds the configured bound",
                      std::to_string(frame.payload.size()));
  }
  std::vector<std::uint8_t> header;
  header.reserve(kFrameHeaderSize);
  const auto push_u16 = [&header](std::uint16_t value) {
    header.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    header.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  };
  const auto push_u32 = [&header](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      header.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  };
  const auto push_u64 = [&header](std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      header.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  };
  push_u32(kFrameMagic);
  push_u16(APF_PROTOCOL_VERSION);
  push_u16(static_cast<std::uint16_t>(frame.type));
  push_u32(frame.flags);
  push_u32(static_cast<std::uint32_t>(frame.payload.size()));
  push_u32(kReservedMustBeZero);
  push_u64(frame.sequence);
  push_u64(frame.coordinator_epoch.value());
  push_u64(frame.worker.value());
  push_u64(frame.worker_boot.value());
  push_u32(0);
  const std::uint32_t checksum = frame_checksum(header, frame.payload);
  write_u32_at(header, kFrameHeaderSize - 4, checksum);

  std::vector<std::uint8_t> out;
  out.reserve(kFrameHeaderSize + frame.payload.size());
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return out;
}

Result<FrameHeader> decode_frame_header(const std::uint8_t* data, std::size_t size,
                                        const Limits& limits) {
  if (data == nullptr) {
    return make_error(ErrorCode::ProtocolViolation, "frame buffer is null");
  }
  if (size < kFrameHeaderSize) {
    return make_error(ErrorCode::ProtocolViolation, "truncated frame header",
                      std::to_string(size));
  }
  FrameHeader header;
  header.magic = read_u32_at(data, 0);
  if (header.magic != kFrameMagic) {
    return make_error(ErrorCode::ProtocolViolation, "invalid frame magic",
                      std::to_string(header.magic));
  }
  header.version = static_cast<std::uint16_t>(data[4] | (static_cast<std::uint16_t>(data[5]) << 8));
  if (header.version != APF_PROTOCOL_VERSION) {
    return make_error(ErrorCode::ProtocolViolation, "unsupported protocol version",
                      std::to_string(header.version));
  }
  const std::uint16_t type = static_cast<std::uint16_t>(data[6] |
                                                        (static_cast<std::uint16_t>(data[7]) << 8));
  if (type == 0 || type >= static_cast<std::uint16_t>(MessageType::Count)) {
    return make_error(ErrorCode::ProtocolViolation, "unknown message type", std::to_string(type));
  }
  header.type = static_cast<MessageType>(type);
  header.flags = read_u32_at(data, 8);
  header.payload_len = read_u32_at(data, 12);
  header.reserved = read_u32_at(data, 16);
  if (header.reserved != kReservedMustBeZero) {
    return make_error(ErrorCode::ProtocolViolation, "reserved header field is not zero");
  }
  if (header.payload_len > limits.max_frame_bytes) {
    return make_error(ErrorCode::LimitExceeded, "declared payload length exceeds the bound",
                      std::to_string(header.payload_len));
  }
  const auto read_u64 = [data](std::size_t offset) {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(data[offset + static_cast<std::size_t>(shift / 8)])
               << shift;
    }
    return value;
  };
  header.sequence = read_u64(20);
  header.coordinator_epoch = CoordinatorEpoch::from_value(read_u64(28));
  header.worker = WorkerId::from_value(read_u64(36));
  header.worker_boot = WorkerBootId::from_value(read_u64(44));
  header.checksum = read_u32_at(data, 52);
  return header;
}

Result<Frame> decode_frame(const std::uint8_t* data, std::size_t size, const Limits& limits) {
  Result<FrameHeader> header = decode_frame_header(data, size, limits);
  if (!header.ok()) {
    return header.error();
  }
  const std::size_t expected = kFrameHeaderSize + header.value().payload_len;
  if (size != expected) {
    return make_error(ErrorCode::ProtocolViolation, "frame length does not match the declared size",
                      std::to_string(size) + " != " + std::to_string(expected));
  }
  std::vector<std::uint8_t> header_bytes(data, data + kFrameHeaderSize);
  std::vector<std::uint8_t> payload(data + kFrameHeaderSize, data + size);
  write_u32_at(header_bytes, kFrameHeaderSize - 4, 0);
  const std::uint32_t expected_checksum = frame_checksum(header_bytes, payload);
  if (expected_checksum != header.value().checksum) {
    return make_error(ErrorCode::ProtocolViolation, "frame integrity check failed");
  }
  Frame frame;
  frame.type = header.value().type;
  frame.flags = header.value().flags;
  frame.sequence = header.value().sequence;
  frame.coordinator_epoch = header.value().coordinator_epoch;
  frame.worker = header.value().worker;
  frame.worker_boot = header.value().worker_boot;
  frame.payload = std::move(payload);
  return frame;
}

FramedChannel::FramedChannel(TcpSocket socket, Limits limits)
    : socket_(std::move(socket)), limits_(limits) {}

FramedChannel::~FramedChannel() { close(); }

bool FramedChannel::valid() const noexcept { return socket_.valid(); }

void FramedChannel::close() noexcept {
  socket_.shutdown_both();
  socket_.close();
}

std::uint64_t FramedChannel::next_sequence() noexcept { return send_sequence_++; }

std::uint64_t FramedChannel::frames_sent() const noexcept { return frames_sent_; }

std::uint64_t FramedChannel::frames_received() const noexcept { return frames_received_; }

Status FramedChannel::send(const Frame& frame) {
  Result<std::vector<std::uint8_t>> encoded = encode_frame(frame, limits_);
  if (!encoded.ok()) {
    return encoded.error();
  }
  const Status status = socket_.send_all(encoded.value().data(), encoded.value().size());
  if (status.ok()) {
    ++frames_sent_;
  }
  return status;
}

Result<Frame> FramedChannel::receive() {
  std::vector<std::uint8_t> header(kFrameHeaderSize, 0);
  const Status status = socket_.recv_exact(header.data(), header.size());
  if (!status.ok()) {
    return status.error();
  }
  Result<FrameHeader> decoded = decode_frame_header(header.data(), header.size(), limits_);
  if (!decoded.ok()) {
    return decoded.error();
  }
  std::vector<std::uint8_t> buffer = std::move(header);
  if (decoded.value().payload_len > 0) {
    buffer.resize(kFrameHeaderSize + decoded.value().payload_len);
    const Status payload_status =
        socket_.recv_exact(buffer.data() + kFrameHeaderSize, decoded.value().payload_len);
    if (!payload_status.ok()) {
      return payload_status.error();
    }
  }
  Result<Frame> frame = decode_frame(buffer.data(), buffer.size(), limits_);
  if (frame.ok()) {
    ++frames_received_;
  }
  return frame;
}

// ---------------------------------------------------------------------------
// Message codecs
// ---------------------------------------------------------------------------

void encode_payload(ByteWriter& writer, const HelloMessage& message) {
  writer.u16(message.protocol_version);
  writer.u64(message.known_epoch.value());
  writer.u64(message.worker.value());
  writer.u64(message.worker_boot.value());
  writer.text(message.backend);
  writer.text(message.agent_version);
  writer.text(message.instance);
}

void decode_payload(ByteReader& reader, HelloMessage& message) {
  message.protocol_version = reader.u16();
  message.known_epoch = CoordinatorEpoch::from_value(reader.u64());
  message.worker = WorkerId::from_value(reader.u64());
  message.worker_boot = WorkerBootId::from_value(reader.u64());
  message.backend = reader.text(256);
  message.agent_version = reader.text(64);
  message.instance = reader.text(256);
}

void encode_payload(ByteWriter& writer, const HelloAckMessage& message) {
  writer.boolean(message.accepted);
  writer.u64(message.epoch.value());
  writer.u64(message.assigned_worker.value());
  writer.u64(message.session_nonce);
  codec::encode(writer, message.limits);
  writer.text(message.reason);
}

void decode_payload(ByteReader& reader, HelloAckMessage& message) {
  message.accepted = reader.boolean();
  message.epoch = CoordinatorEpoch::from_value(reader.u64());
  message.assigned_worker = WorkerId::from_value(reader.u64());
  message.session_nonce = reader.u64();
  message.limits = codec::decode_limits(reader);
  message.reason = reader.text(256);
}

void encode_payload(ByteWriter& writer, const RegisterWorkerMessage& message) {
  codec::encode(writer, message.worker);
  writer.u32(static_cast<std::uint32_t>(message.devices.size()));
  for (const BackendAccelerator& device : message.devices) {
    codec::encode(writer, device);
  }
  writer.u64(message.started_at_ms);
}

void decode_payload(ByteReader& reader, RegisterWorkerMessage& message) {
  message.worker = codec::decode_worker(reader);
  message.devices = reader.list<BackendAccelerator>(64, 8, [](ByteReader& inner) {
    return codec::decode_backend_accelerator(inner);
  });
  message.started_at_ms = reader.u64();
}

void encode_payload(ByteWriter& writer, const RegisterAckMessage& message) {
  writer.boolean(message.accepted);
  writer.u64(message.epoch.value());
  writer.u32(static_cast<std::uint32_t>(message.registered_accelerators.size()));
  for (const AcceleratorId id : message.registered_accelerators) {
    writer.u64(id.value());
  }
  encode_string_list(writer, message.rejected_devices, 64);
  writer.text(message.reason);
}

void decode_payload(ByteReader& reader, RegisterAckMessage& message) {
  message.accepted = reader.boolean();
  message.epoch = CoordinatorEpoch::from_value(reader.u64());
  message.registered_accelerators = reader.list<AcceleratorId>(64, 8, [](ByteReader& inner) {
    return AcceleratorId::from_value(inner.u64());
  });
  message.rejected_devices = decode_string_list(reader, 64);
  message.reason = reader.text(256);
}

void encode_payload(ByteWriter& writer, const EvidencePublishMessage& message) {
  writer.u32(static_cast<std::uint32_t>(message.devices.size()));
  for (const BackendAccelerator& device : message.devices) {
    codec::encode(writer, device);
  }
  writer.u32(static_cast<std::uint32_t>(message.layouts.size()));
  for (const BackendLayout& layout : message.layouts) {
    codec::encode(writer, layout);
  }
  writer.u64(message.published_at_ms);
  writer.boolean(message.revalidate);
}

void decode_payload(ByteReader& reader, EvidencePublishMessage& message) {
  message.devices = reader.list<BackendAccelerator>(64, 8, [](ByteReader& inner) {
    return codec::decode_backend_accelerator(inner);
  });
  message.layouts = reader.list<BackendLayout>(64, 8, [](ByteReader& inner) {
    return codec::decode_layout(inner);
  });
  message.published_at_ms = reader.u64();
  message.revalidate = reader.boolean();
}

void encode_payload(ByteWriter& writer, const EvidenceAckMessage& message) {
  writer.boolean(message.accepted);
  writer.u32(static_cast<std::uint32_t>(message.accelerators.size()));
  for (const AcceleratorId id : message.accelerators) {
    writer.u64(id.value());
  }
  writer.u32(static_cast<std::uint32_t>(message.revalidated_partitions.size()));
  for (const PartitionId id : message.revalidated_partitions) {
    writer.u64(id.value());
  }
  writer.text(message.reason);
}

void decode_payload(ByteReader& reader, EvidenceAckMessage& message) {
  message.accepted = reader.boolean();
  message.accelerators = reader.list<AcceleratorId>(64, 8, [](ByteReader& inner) {
    return AcceleratorId::from_value(inner.u64());
  });
  message.revalidated_partitions = reader.list<PartitionId>(256, 8, [](ByteReader& inner) {
    return PartitionId::from_value(inner.u64());
  });
  message.reason = reader.text(256);
}

void encode_payload(ByteWriter& writer, const MutationRequestMessage& message) {
  writer.u64(message.attempt.value());
  writer.u64(message.reservation.value());
  codec::encode_enum(writer, message.kind);
  codec::encode(writer, message.request);
}

void decode_payload(ByteReader& reader, MutationRequestMessage& message) {
  message.attempt = PartitionAttemptId::from_value(reader.u64());
  message.reservation = PartitionReservationId::from_value(reader.u64());
  message.kind = codec::decode_enum<AttemptKind>(
      reader, static_cast<std::uint8_t>(AttemptKind::Count), "attempt kind");
  message.request = codec::decode_mutation_request(reader);
}

void encode_payload(ByteWriter& writer, const MutationResultMessage& message) {
  writer.u64(message.attempt.value());
  writer.u64(message.idempotency_token);
  codec::encode(writer, message.result);
}

void decode_payload(ByteReader& reader, MutationResultMessage& message) {
  message.attempt = PartitionAttemptId::from_value(reader.u64());
  message.idempotency_token = reader.u64();
  message.result = codec::decode_mutation_result(reader);
}

void encode_payload(ByteWriter& writer, const QueryRequestMessage& message) {
  codec::encode_enum(writer, message.kind);
  writer.text(message.key);
  writer.u64(message.attempt.value());
  writer.u64(message.partition.value());
  writer.boolean(message.include_snapshot);
}

void decode_payload(ByteReader& reader, QueryRequestMessage& message) {
  message.kind = codec::decode_enum<QueryKind>(
      reader, static_cast<std::uint8_t>(QueryKind::Count), "query kind");
  message.key = reader.text(256);
  message.attempt = PartitionAttemptId::from_value(reader.u64());
  message.partition = PartitionId::from_value(reader.u64());
  message.include_snapshot = reader.boolean();
}

void encode_payload(ByteWriter& writer, const QueryResponseMessage& message) {
  codec::encode_enum(writer, message.kind);
  writer.boolean(message.found);
  writer.text(message.detail);
  writer.u32(static_cast<std::uint32_t>(message.devices.size()));
  for (const BackendAccelerator& device : message.devices) {
    codec::encode(writer, device);
  }
  codec::encode(writer, message.layout);
  codec::encode(writer, message.attempt);
  writer.u32(static_cast<std::uint32_t>(message.partitions.size()));
  for (const PartitionRecord& record : message.partitions) {
    codec::encode(writer, record);
  }
  writer.u32(static_cast<std::uint32_t>(message.workers.size()));
  for (const WorkerRecord& worker : message.workers) {
    codec::encode(writer, worker);
  }
  writer.text(message.snapshot_text, 1u << 18);
}

void decode_payload(ByteReader& reader, QueryResponseMessage& message) {
  message.kind = codec::decode_enum<QueryKind>(
      reader, static_cast<std::uint8_t>(QueryKind::Count), "query kind");
  message.found = reader.boolean();
  message.detail = reader.text(512);
  message.devices = reader.list<BackendAccelerator>(64, 8, [](ByteReader& inner) {
    return codec::decode_backend_accelerator(inner);
  });
  message.layout = codec::decode_layout(reader);
  message.attempt = codec::decode_attempt(reader);
  message.partitions = reader.list<PartitionRecord>(256, 8, [](ByteReader& inner) {
    return codec::decode_partition(inner);
  });
  message.workers = reader.list<WorkerRecord>(64, 8, [](ByteReader& inner) {
    return codec::decode_worker(inner);
  });
  message.snapshot_text = reader.text(1u << 18);
}

void encode_payload(ByteWriter& writer, const FenceMessage& message) {
  writer.u64(message.worker.value());
  writer.u64(message.worker_boot.value());
  writer.u64(message.epoch.value());
  writer.text(message.reason);
}

void decode_payload(ByteReader& reader, FenceMessage& message) {
  message.worker = WorkerId::from_value(reader.u64());
  message.worker_boot = WorkerBootId::from_value(reader.u64());
  message.epoch = CoordinatorEpoch::from_value(reader.u64());
  message.reason = reader.text(256);
}

void encode_payload(ByteWriter& writer, const FenceAckMessage& message) {
  writer.boolean(message.acknowledged);
  writer.text(message.reason);
}

void decode_payload(ByteReader& reader, FenceAckMessage& message) {
  message.acknowledged = reader.boolean();
  message.reason = reader.text(256);
}

void encode_payload(ByteWriter& writer, const HeartbeatMessage& message) {
  writer.u64(message.sent_at_ms);
  writer.u64(message.applied_attempts);
}

void decode_payload(ByteReader& reader, HeartbeatMessage& message) {
  message.sent_at_ms = reader.u64();
  message.applied_attempts = reader.u64();
}

void encode_payload(ByteWriter& writer, const HeartbeatAckMessage& message) {
  writer.u64(message.epoch.value());
  writer.u64(message.received_at_ms);
  writer.u64(message.allowed_attempts);
}

void decode_payload(ByteReader& reader, HeartbeatAckMessage& message) {
  message.epoch = CoordinatorEpoch::from_value(reader.u64());
  message.received_at_ms = reader.u64();
  message.allowed_attempts = reader.u64();
}

void encode_payload(ByteWriter& writer, const ShutdownMessage& message) {
  writer.text(message.reason);
  writer.boolean(message.fence);
}

void decode_payload(ByteReader& reader, ShutdownMessage& message) {
  message.reason = reader.text(256);
  message.fence = reader.boolean();
}

void encode_payload(ByteWriter& writer, const ErrorResponseMessage& message) {
  writer.u16(static_cast<std::uint16_t>(message.code));
  writer.text(message.message);
  writer.text(message.detail);
  writer.u64(message.sequence);
}

void decode_payload(ByteReader& reader, ErrorResponseMessage& message) {
  const std::uint16_t code = reader.u16();
  if (reader.ok() && code > static_cast<std::uint16_t>(ErrorCode::Internal)) {
    reader.fail(ErrorCode::ProtocolViolation, "invalid error code on the wire",
                std::to_string(code));
  }
  message.code = static_cast<ErrorCode>(code);
  message.message = reader.text(256);
  message.detail = reader.text(512);
  message.sequence = reader.u64();
}

void encode_payload(ByteWriter& writer, const ReconcileRequestMessage& message) {
  writer.text(message.stable_key);
  writer.boolean(message.allow_adoption);
}

void decode_payload(ByteReader& reader, ReconcileRequestMessage& message) {
  message.stable_key = reader.text(256);
  message.allow_adoption = reader.boolean();
}

void encode_payload(ByteWriter& writer, const ReconcileResponseMessage& message) {
  writer.boolean(message.accepted);
  writer.text(message.summary, 2048);
  writer.u32(message.matched);
  writer.u32(message.missing);
  writer.u32(message.unexpected);
  writer.u32(message.adopted);
  writer.boolean(message.revalidation_required);
  writer.text(message.reason);
}

void decode_payload(ByteReader& reader, ReconcileResponseMessage& message) {
  message.accepted = reader.boolean();
  message.summary = reader.text(2048);
  message.matched = reader.u32();
  message.missing = reader.u32();
  message.unexpected = reader.u32();
  message.adopted = reader.u32();
  message.revalidation_required = reader.boolean();
  message.reason = reader.text(256);
}

void encode_payload(ByteWriter& writer, const AdminRequestMessage& message) {
  codec::encode_enum(writer, message.action);
  writer.u64(message.partition.value());
  writer.u64(message.accelerator.value());
  writer.u64(message.reservation.value());
  writer.u64(message.worker.value());
  writer.u64(message.worker_boot.value());
  writer.text(message.reason);
  writer.boolean(message.confirmed);
}

void decode_payload(ByteReader& reader, AdminRequestMessage& message) {
  message.action = codec::decode_enum<AdminAction>(
      reader, static_cast<std::uint8_t>(AdminAction::Count), "admin action");
  message.partition = PartitionId::from_value(reader.u64());
  message.accelerator = AcceleratorId::from_value(reader.u64());
  message.reservation = PartitionReservationId::from_value(reader.u64());
  message.worker = WorkerId::from_value(reader.u64());
  message.worker_boot = WorkerBootId::from_value(reader.u64());
  message.reason = reader.text(256);
  message.confirmed = reader.boolean();
}

const char* to_string(CommandKind kind) noexcept {
  switch (kind) {
    case CommandKind::InspectSnapshot: return "inspect_snapshot";
    case CommandKind::InspectAccelerators: return "inspect_accelerators";
    case CommandKind::InspectWorkers: return "inspect_workers";
    case CommandKind::InspectPartitions: return "inspect_partitions";
    case CommandKind::QueryLayout: return "query_layout";
    case CommandKind::PublishEvidence: return "publish_evidence";
    case CommandKind::Plan: return "plan";
    case CommandKind::PlanReconfiguration: return "plan_reconfiguration";
    case CommandKind::Reserve: return "reserve";
    case CommandKind::Create: return "create";
    case CommandKind::Destroy: return "destroy";
    case CommandKind::Drain: return "drain";
    case CommandKind::CompleteDrain: return "complete_drain";
    case CommandKind::CancelDrain: return "cancel_drain";
    case CommandKind::ReleaseReservation: return "release_reservation";
    case CommandKind::Reconcile: return "reconcile";
    case CommandKind::FenceWorker: return "fence_worker";
    case CommandKind::AdvanceEpoch: return "advance_epoch";
    case CommandKind::PersistState: return "persist_state";
    case CommandKind::ShutdownCoordinator: return "shutdown_coordinator";
    case CommandKind::Count: return "count";
  }
  return "count";
}

bool command_is_read_only(CommandKind kind) noexcept {
  switch (kind) {
    case CommandKind::InspectSnapshot:
    case CommandKind::InspectAccelerators:
    case CommandKind::InspectWorkers:
    case CommandKind::InspectPartitions:
    case CommandKind::QueryLayout:
      return true;
    default:
      return false;
  }
}

}  // namespace apf

namespace {
// (helpers below are part of the same translation unit)
}

namespace apf {

void encode_payload(ByteWriter& writer, const CommandRequestMessage& message) {
  codec::encode_enum(writer, message.kind);
  codec::encode(writer, message.request);
  writer.u64(message.reservation.value());
  writer.u64(message.partition.value());
  writer.u64(message.accelerator.value());
  writer.u64(message.plan.value());
  writer.u64(message.worker.value());
  writer.u64(message.worker_boot.value());
  writer.text(message.stable_key);
  writer.text(message.reason);
  writer.boolean(message.confirmed);
}

void decode_payload(ByteReader& reader, CommandRequestMessage& message) {
  message.kind = codec::decode_enum<CommandKind>(
      reader, static_cast<std::uint8_t>(CommandKind::Count), "command kind");
  message.request = codec::decode_request(reader);
  message.reservation = PartitionReservationId::from_value(reader.u64());
  message.partition = PartitionId::from_value(reader.u64());
  message.accelerator = AcceleratorId::from_value(reader.u64());
  message.plan = PartitionPlanId::from_value(reader.u64());
  message.worker = WorkerId::from_value(reader.u64());
  message.worker_boot = WorkerBootId::from_value(reader.u64());
  message.stable_key = reader.text(256);
  message.reason = reader.text(512);
  message.confirmed = reader.boolean();
}

void encode_payload(ByteWriter& writer, const CommandResponseMessage& message) {
  writer.boolean(message.accepted);
  writer.u16(static_cast<std::uint16_t>(message.code));
  writer.text(message.message);
  writer.text(message.detail, 4096);
  writer.u64(message.plan.value());
  writer.u64(message.plan_generation.value());
  codec::encode_enum(writer, message.plan_outcome);
  writer.u32(static_cast<std::uint32_t>(message.plan_steps.size()));
  for (const PlanStep& step : message.plan_steps) {
    codec::encode(writer, step);
  }
  writer.u32(static_cast<std::uint32_t>(message.planned_partitions.size()));
  for (const PlannedPartition& planned : message.planned_partitions) {
    codec::encode(writer, planned);
  }
  writer.u32(static_cast<std::uint32_t>(message.candidates.size()));
  for (const CandidateEvaluation& candidate : message.candidates) {
    codec::encode(writer, candidate);
  }
  writer.u64(message.reservation.value());
  codec::encode_enum(writer, message.reservation_lifecycle);
  writer.u64(message.attempt.value());
  codec::encode_enum(writer, message.attempt_state);
  writer.boolean(message.outcome_unknown);
  writer.boolean(message.verified_physically);
  writer.u32(static_cast<std::uint32_t>(message.partitions.size()));
  for (const PartitionId id : message.partitions) {
    writer.u64(id.value());
  }
  writer.u32(static_cast<std::uint32_t>(message.generations.size()));
  for (const PartitionGeneration generation : message.generations) {
    writer.u64(generation.value());
  }
  writer.u32(message.matched);
  writer.u32(message.missing);
  writer.u32(message.unexpected);
  writer.u32(message.adopted);
  writer.boolean(message.revalidation_required);
  writer.u64(message.accelerator.value());
  writer.u64(message.coordinator_epoch.value());
  codec::encode(writer, message.layout);
  writer.u32(static_cast<std::uint32_t>(message.devices.size()));
  for (const BackendAccelerator& device : message.devices) {
    codec::encode(writer, device);
  }
  writer.u32(static_cast<std::uint32_t>(message.workers.size()));
  for (const WorkerRecord& worker : message.workers) {
    codec::encode(writer, worker);
  }
  writer.u32(static_cast<std::uint32_t>(message.partition_records.size()));
  for (const PartitionRecord& record : message.partition_records) {
    codec::encode(writer, record);
  }
  writer.text(message.snapshot_text, 1u << 18);
}

void decode_payload(ByteReader& reader, CommandResponseMessage& message) {
  message.accepted = reader.boolean();
  const std::uint16_t code = reader.u16();
  if (reader.ok() && code > static_cast<std::uint16_t>(ErrorCode::Internal)) {
    reader.fail(ErrorCode::ProtocolViolation, "invalid error code on the wire",
                std::to_string(code));
  }
  message.code = static_cast<ErrorCode>(code);
  message.message = reader.text(256);
  message.detail = reader.text(4096);
  message.plan = PartitionPlanId::from_value(reader.u64());
  message.plan_generation = PartitionPlanGeneration::from_value(reader.u64());
  message.plan_outcome = codec::decode_enum<PlanOutcome>(
      reader, static_cast<std::uint8_t>(PlanOutcome::Count), "plan outcome");
  message.plan_steps = reader.list<PlanStep>(512, 8, [](ByteReader& inner) {
    return codec::decode_plan_step(inner);
  });
  message.planned_partitions = reader.list<PlannedPartition>(512, 8, [](ByteReader& inner) {
    return codec::decode_planned_partition(inner);
  });
  message.candidates = reader.list<CandidateEvaluation>(64, 8, [](ByteReader& inner) {
    return codec::decode_candidate(inner);
  });
  message.reservation = PartitionReservationId::from_value(reader.u64());
  message.reservation_lifecycle = codec::decode_enum<ReservationLifecycle>(
      reader, static_cast<std::uint8_t>(ReservationLifecycle::Count), "reservation lifecycle");
  message.attempt = PartitionAttemptId::from_value(reader.u64());
  message.attempt_state = codec::decode_enum<AttemptState>(
      reader, static_cast<std::uint8_t>(AttemptState::Count), "attempt state");
  message.outcome_unknown = reader.boolean();
  message.verified_physically = reader.boolean();
  message.partitions = reader.list<PartitionId>(512, 8, [](ByteReader& inner) {
    return PartitionId::from_value(inner.u64());
  });
  message.generations = reader.list<PartitionGeneration>(512, 8, [](ByteReader& inner) {
    return PartitionGeneration::from_value(inner.u64());
  });
  message.matched = reader.u32();
  message.missing = reader.u32();
  message.unexpected = reader.u32();
  message.adopted = reader.u32();
  message.revalidation_required = reader.boolean();
  message.accelerator = AcceleratorId::from_value(reader.u64());
  message.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
  message.layout = codec::decode_layout(reader);
  message.devices = reader.list<BackendAccelerator>(64, 8, [](ByteReader& inner) {
    return codec::decode_backend_accelerator(inner);
  });
  message.workers = reader.list<WorkerRecord>(64, 8, [](ByteReader& inner) {
    return codec::decode_worker(inner);
  });
  message.partition_records = reader.list<PartitionRecord>(512, 8, [](ByteReader& inner) {
    return codec::decode_partition(inner);
  });
  message.snapshot_text = reader.text(1u << 18);
}

void encode_payload(ByteWriter& writer, const AdminResponseMessage& message) {
  writer.boolean(message.accepted);
  writer.u16(static_cast<std::uint16_t>(message.code));
  writer.text(message.message);
  writer.text(message.detail);
}

void decode_payload(ByteReader& reader, AdminResponseMessage& message) {
  message.accepted = reader.boolean();
  const std::uint16_t code = reader.u16();
  if (reader.ok() && code > static_cast<std::uint16_t>(ErrorCode::Internal)) {
    reader.fail(ErrorCode::ProtocolViolation, "invalid error code on the wire",
                std::to_string(code));
  }
  message.code = static_cast<ErrorCode>(code);
  message.message = reader.text(256);
  message.detail = reader.text(512);
}

}  // namespace apf
