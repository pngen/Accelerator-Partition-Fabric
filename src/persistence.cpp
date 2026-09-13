#include "apf/persistence.hpp"

#include "apf/codec.hpp"
#include "apf/process.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

namespace apf {
namespace {

constexpr std::array<char, 8> kMagic{{'A', 'P', 'F', 'S', 'T', 'A', 'T', 'E'}};
constexpr std::uint32_t kEndianMarker = 0x01020304u;
constexpr std::size_t kHeaderSize = 8 + 4 + 4 + 8 + 8 + 8;
constexpr std::uint64_t kMaxPayloadBytes = 64ull * 1024ull * 1024ull;

void write_u32(std::vector<std::uint8_t>& buffer, std::size_t offset, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    buffer[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  }
}

void write_u64(std::vector<std::uint8_t>& buffer, std::size_t offset, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    buffer[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  }
}

std::uint32_t read_u32(const std::uint8_t* data, std::size_t offset) {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(data[offset + static_cast<std::size_t>(shift / 8)])
             << shift;
  }
  return value;
}

std::uint64_t read_u64(const std::uint8_t* data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(data[offset + static_cast<std::size_t>(shift / 8)])
             << shift;
  }
  return value;
}

Status reject(ErrorCode code, std::string message, std::string detail = {}) {
  return failure(code, std::move(message), std::move(detail));
}

/// Structural validation that must hold before any of the state is applied.
Status validate_decoded(const DurableState& state, const Limits& limits) {
  if (state.format_version != APF_PERSISTENCE_FORMAT_VERSION) {
    return reject(ErrorCode::PersistenceCorruption, "unsupported durable format version",
                  std::to_string(state.format_version));
  }
  if (state.profiles.size() > limits.max_persisted_records ||
      state.accelerators.size() > limits.max_accelerators ||
      state.partitions.size() > limits.max_partitions ||
      state.reservations.size() > limits.max_reservations ||
      state.attempts.size() > limits.max_attempt_history ||
      state.assignments.size() > limits.max_assignments ||
      state.workers.size() > limits.max_workers) {
    return reject(ErrorCode::LimitExceeded, "durable state exceeds configured bounds");
  }
  std::set<std::uint64_t> profile_ids;
  for (const PartitionProfile& profile : state.profiles) {
    const Status valid = profile.validate(limits);
    if (!valid.ok()) {
      return reject(ErrorCode::PersistenceCorruption, to_string(valid.error()));
    }
    if (!profile_ids.insert(profile.id.value()).second) {
      return reject(ErrorCode::PersistenceCorruption, "duplicate profile identity",
                    profile.id.str());
    }
  }
  std::set<std::uint64_t> accelerator_ids;
  for (const AcceleratorRecord& record : state.accelerators) {
    const Status valid = record.validate(limits);
    if (!valid.ok()) {
      return reject(ErrorCode::PersistenceCorruption, to_string(valid.error()));
    }
    if (!accelerator_ids.insert(record.id.value()).second) {
      return reject(ErrorCode::PersistenceCorruption, "duplicate accelerator identity",
                    record.id.str());
    }
  }
  std::set<std::uint64_t> ledger_ids;
  for (const auto& entry : state.ledgers) {
    if (accelerator_ids.count(entry.first.value()) == 0) {
      return reject(ErrorCode::PersistenceCorruption,
                    "durable ledger references an unknown accelerator", entry.first.str());
    }
    if (!ledger_ids.insert(entry.first.value()).second) {
      return reject(ErrorCode::PersistenceCorruption, "duplicate ledger for an accelerator",
                    entry.first.str());
    }
    const Status valid = entry.second.validate();
    if (!valid.ok()) {
      return reject(ErrorCode::PersistenceCorruption, to_string(valid.error()),
                    entry.first.str());
    }
  }
  for (const AcceleratorRecord& record : state.accelerators) {
    if (ledger_ids.count(record.id.value()) == 0) {
      return reject(ErrorCode::PersistenceCorruption, "accelerator has no durable ledger",
                    record.id.str());
    }
  }
  std::set<std::uint64_t> partition_ids;
  for (const PartitionRecord& record : state.partitions) {
    const Status valid = record.validate();
    if (!valid.ok()) {
      return reject(ErrorCode::PersistenceCorruption, to_string(valid.error()));
    }
    if (!partition_ids.insert(record.id.value()).second) {
      return reject(ErrorCode::PersistenceCorruption, "duplicate partition identity",
                    record.id.str());
    }
    if (accelerator_ids.count(record.accelerator.value()) == 0) {
      return reject(ErrorCode::PersistenceCorruption,
                    "partition references an unknown accelerator", record.id.str());
    }
    const auto ledger = std::find_if(state.ledgers.begin(), state.ledgers.end(),
                                     [&record](const auto& entry) {
                                       return entry.first == record.accelerator;
                                     });
    if (ledger == state.ledgers.end()) {
      return reject(ErrorCode::PersistenceCorruption,
                    "partition references an accelerator without a ledger", record.id.str());
    }
    const auto accelerator = std::find_if(state.accelerators.begin(), state.accelerators.end(),
                                          [&record](const AcceleratorRecord& candidate) {
                                            return candidate.id == record.accelerator;
                                          });
    if (accelerator != state.accelerators.end() &&
        ledger->second.total() != accelerator->physical_totals) {
      return reject(ErrorCode::PersistenceCorruption,
                    "durable ledger total disagrees with the accelerator capacity",
                    record.id.str());
    }
  }
  std::set<std::uint64_t> reservation_ids;
  for (const PartitionReservation& reservation : state.reservations) {
    const Status valid = reservation.validate();
    if (!valid.ok()) {
      return reject(ErrorCode::PersistenceCorruption, to_string(valid.error()));
    }
    if (!reservation_ids.insert(reservation.id.value()).second) {
      return reject(ErrorCode::PersistenceCorruption, "duplicate reservation identity",
                    reservation.id.str());
    }
  }
  std::set<std::uint64_t> attempt_ids;
  for (const PartitionAttempt& attempt : state.attempts) {
    const Status valid = attempt.validate();
    if (!valid.ok()) {
      return reject(ErrorCode::PersistenceCorruption, to_string(valid.error()));
    }
    if (!attempt_ids.insert(attempt.id.value()).second) {
      return reject(ErrorCode::PersistenceCorruption, "duplicate attempt identity",
                    attempt.id.str());
    }
  }
  std::set<std::uint64_t> assignment_ids;
  for (const PartitionAssignment& assignment : state.assignments) {
    const Status valid = assignment.validate();
    if (!valid.ok()) {
      return reject(ErrorCode::PersistenceCorruption, to_string(valid.error()));
    }
    if (!assignment_ids.insert(assignment.id.value()).second) {
      return reject(ErrorCode::PersistenceCorruption, "duplicate assignment identity",
                    assignment.id.str());
    }
    if (partition_ids.count(assignment.partition.value()) == 0) {
      return reject(ErrorCode::PersistenceCorruption,
                    "assignment references an unknown partition", assignment.id.str());
    }
  }
  std::set<std::uint64_t> worker_ids;
  for (const WorkerRecord& worker : state.workers) {
    if (!worker.id.valid() || !worker.boot.valid()) {
      return reject(ErrorCode::PersistenceCorruption, "durable worker identity is incomplete");
    }
    if (!worker_ids.insert(worker.id.value()).second) {
      return reject(ErrorCode::PersistenceCorruption, "duplicate worker identity",
                    worker.id.str());
    }
  }
  const Status policy_valid = state.policy.validate();
  if (!policy_valid.ok()) {
    return reject(ErrorCode::PersistenceCorruption, to_string(policy_valid.error()));
  }
  if (state.next_identity == 0) {
    return reject(ErrorCode::PersistenceCorruption, "durable identity counter is not usable");
  }
  return success();
}

}  // namespace

std::uint64_t integrity_hash(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

std::uint64_t integrity_hash(std::string_view data) noexcept {
  return integrity_hash(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

Status PersistenceStore::set_path(std::string path) {
  if (path.empty()) {
    return failure(ErrorCode::InvalidArgument, "persistence path is empty");
  }
  if (!is_path_safe(path)) {
    return failure(ErrorCode::InvalidArgument, "persistence path is not safe", path);
  }
  path_ = std::move(path);
  return success();
}

Result<std::vector<std::uint8_t>> PersistenceStore::encode(const DurableState& state,
                                                           const Limits& limits) {
  const Status valid = validate_decoded(state, limits);
  if (!valid.ok()) {
    return valid.error();
  }
  ByteWriter writer(&limits, 4096);
  writer.u32(state.format_version);
  writer.u64(state.state_generation.value());
  writer.u64(state.coordinator_epoch.value());
  writer.u64(state.policy_generation.value());
  writer.u64(state.next_identity);
  writer.u64(state.saved_at_ms);
  writer.text(state.instance_id);
  codec::encode(writer, state.policy);
  writer.u32(static_cast<std::uint32_t>(state.profiles.size()));
  for (const PartitionProfile& profile : state.profiles) {
    codec::encode(writer, profile);
  }
  writer.u32(static_cast<std::uint32_t>(state.accelerators.size()));
  for (const AcceleratorRecord& record : state.accelerators) {
    codec::encode(writer, record);
  }
  writer.u32(static_cast<std::uint32_t>(state.accelerator_keys.size()));
  for (const auto& entry : state.accelerator_keys) {
    writer.u64(entry.first.value());
    writer.text(entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(state.ledgers.size()));
  for (const auto& entry : state.ledgers) {
    writer.u64(entry.first.value());
    codec::encode(writer, entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(state.partitions.size()));
  for (const PartitionRecord& record : state.partitions) {
    codec::encode(writer, record);
  }
  writer.u32(static_cast<std::uint32_t>(state.reservations.size()));
  for (const PartitionReservation& reservation : state.reservations) {
    codec::encode(writer, reservation);
  }
  writer.u32(static_cast<std::uint32_t>(state.attempts.size()));
  for (const PartitionAttempt& attempt : state.attempts) {
    codec::encode(writer, attempt);
  }
  writer.u32(static_cast<std::uint32_t>(state.assignments.size()));
  for (const PartitionAssignment& assignment : state.assignments) {
    codec::encode(writer, assignment);
  }
  writer.u32(static_cast<std::uint32_t>(state.workers.size()));
  for (const WorkerRecord& worker : state.workers) {
    codec::encode(writer, worker);
  }
  if (!writer.ok()) {
    return writer.error();
  }
  const std::vector<std::uint8_t>& payload = writer.data();
  if (payload.size() > kMaxPayloadBytes) {
    return make_error(ErrorCode::LimitExceeded, "durable payload exceeds the container limit");
  }

  std::vector<std::uint8_t> out(kHeaderSize + payload.size(), 0);
  std::memcpy(out.data(), kMagic.data(), kMagic.size());
  write_u32(out, 8, state.format_version);
  write_u32(out, 12, kEndianMarker);
  write_u64(out, 16, static_cast<std::uint64_t>(payload.size()));
  write_u64(out, 24, integrity_hash(payload.data(), payload.size()));
  write_u64(out, 32, integrity_hash(out.data(), 32));
  std::memcpy(out.data() + kHeaderSize, payload.data(), payload.size());
  return out;
}

Result<DurableState> PersistenceStore::decode(const std::uint8_t* bytes, std::size_t size,
                                              const Limits& limits) {
  if (bytes == nullptr) {
    return make_error(ErrorCode::PersistenceCorruption, "durable buffer is null");
  }
  if (size < kHeaderSize) {
    return make_error(ErrorCode::PersistenceCorruption, "durable state is truncated",
                      std::to_string(size));
  }
  if (std::memcmp(bytes, kMagic.data(), kMagic.size()) != 0) {
    return make_error(ErrorCode::PersistenceCorruption, "durable state has invalid magic");
  }
  const std::uint32_t version = read_u32(bytes, 8);
  if (version != APF_PERSISTENCE_FORMAT_VERSION) {
    return make_error(ErrorCode::PersistenceCorruption, "unsupported durable format version",
                      std::to_string(version));
  }
  if (read_u32(bytes, 12) != kEndianMarker) {
    return make_error(ErrorCode::PersistenceCorruption, "durable state byte order is not "
                                                        "supported");
  }
  const std::uint64_t payload_length = read_u64(bytes, 16);
  if (payload_length > kMaxPayloadBytes || payload_length > size - kHeaderSize) {
    return make_error(ErrorCode::PersistenceCorruption,
                      "declared durable payload length is not usable",
                      std::to_string(payload_length));
  }
  if (size != kHeaderSize + payload_length) {
    return make_error(ErrorCode::PersistenceCorruption, "durable state size does not match its "
                                                        "header");
  }
  if (read_u64(bytes, 32) != integrity_hash(bytes, 32)) {
    return make_error(ErrorCode::PersistenceCorruption, "durable header integrity check failed");
  }
  if (read_u64(bytes, 24) != integrity_hash(bytes + kHeaderSize, static_cast<std::size_t>(
                                                                       payload_length))) {
    return make_error(ErrorCode::PersistenceCorruption, "durable payload integrity check failed");
  }

  ByteReader reader(bytes + kHeaderSize, static_cast<std::size_t>(payload_length), limits);
  DurableState state;
  state.format_version = reader.u32();
  state.state_generation = StateGeneration::from_value(reader.u64());
  state.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
  state.policy_generation = PolicyGeneration::from_value(reader.u64());
  state.next_identity = reader.u64();
  state.saved_at_ms = reader.u64();
  state.instance_id = reader.text(limits.max_name_bytes);
  state.policy = codec::decode_policy(reader);
  const std::size_t record_bound = limits.max_persisted_records;
  state.profiles = reader.list<PartitionProfile>(record_bound, 8, [](ByteReader& inner) {
    return codec::decode_profile(inner);
  });
  state.accelerators = reader.list<AcceleratorRecord>(record_bound, 8, [](ByteReader& inner) {
    return codec::decode_accelerator(inner);
  });
  state.accelerator_keys =
      reader.list<std::pair<AcceleratorId, std::string>>(record_bound, 8, [](ByteReader& inner) {
        const AcceleratorId id = AcceleratorId::from_value(inner.u64());
        return std::make_pair(id, inner.text(256));
      });
  state.ledgers = reader.list<std::pair<AcceleratorId, CapacityLedger>>(
      record_bound, 8, [](ByteReader& inner) {
        const AcceleratorId id = AcceleratorId::from_value(inner.u64());
        return std::make_pair(id, codec::decode_ledger(inner));
      });
  state.partitions = reader.list<PartitionRecord>(record_bound, 8, [](ByteReader& inner) {
    return codec::decode_partition(inner);
  });
  state.reservations = reader.list<PartitionReservation>(record_bound, 8, [](ByteReader& inner) {
    return codec::decode_reservation(inner);
  });
  state.attempts = reader.list<PartitionAttempt>(record_bound, 8, [](ByteReader& inner) {
    return codec::decode_attempt(inner);
  });
  state.assignments = reader.list<PartitionAssignment>(record_bound, 8, [](ByteReader& inner) {
    return codec::decode_assignment(inner);
  });
  state.workers = reader.list<WorkerRecord>(record_bound, 8, [](ByteReader& inner) {
    return codec::decode_worker(inner);
  });
  const Status finished = reader.finish();
  if (!finished.ok()) {
    return make_error(ErrorCode::PersistenceCorruption, to_string(finished.error()));
  }
  const Status valid = validate_decoded(state, limits);
  if (!valid.ok()) {
    return make_error(ErrorCode::PersistenceCorruption, to_string(valid.error()));
  }
  return state;
}

Status PersistenceStore::save(const DurableState& state, const Limits& limits) {
  if (path_.empty()) {
    return failure(ErrorCode::InvalidArgument, "persistence path has not been configured");
  }
  Result<std::vector<std::uint8_t>> encoded = encode(state, limits);
  if (!encoded.ok()) {
    return encoded.error();
  }
  const std::string payload(reinterpret_cast<const char*>(encoded.value().data()),
                            encoded.value().size());
  return write_file_atomic(path_, payload);
}

Result<DurableState> PersistenceStore::load(const Limits& limits) const {
  if (path_.empty()) {
    return make_error(ErrorCode::InvalidArgument, "persistence path has not been configured");
  }
  Result<std::string> contents = read_file(path_);
  if (!contents.ok()) {
    return contents.error();
  }
  return decode(reinterpret_cast<const std::uint8_t*>(contents.value().data()),
                contents.value().size(), limits);
}

Status PersistenceStore::remove() {
  if (path_.empty()) {
    return failure(ErrorCode::InvalidArgument, "persistence path has not been configured");
  }
  Status status = remove_file(path_);
  const Status temporary = remove_file(path_ + ".tmp");
  if (!temporary.ok() && temporary.error().code != ErrorCode::NotFound) {
    return temporary;
  }
  if (!status.ok() && status.error().code != ErrorCode::NotFound) {
    return status;
  }
  status = success();
  return status;
}

}  // namespace apf
