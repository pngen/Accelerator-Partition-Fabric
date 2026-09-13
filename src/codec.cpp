#include "apf/codec.hpp"

#include <cstring>

namespace apf {
namespace {

constexpr std::size_t kMaxTextBytes = 1u << 20;
constexpr std::size_t kMaxBlobBytes = 1u << 20;

bool is_continuation(std::uint8_t byte) noexcept { return (byte & 0xC0u) == 0x80u; }

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  const std::size_t size = text.size();
  while (index < size) {
    const auto byte = static_cast<std::uint8_t>(text[index]);
    if (byte < 0x80u) {
      ++index;
      continue;
    }
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if ((byte & 0xE0u) == 0xC0u) {
      extra = 1;
      code_point = byte & 0x1Fu;
      if (code_point == 0) {
        return false;
      }
    } else if ((byte & 0xF0u) == 0xE0u) {
      extra = 2;
      code_point = byte & 0x0Fu;
    } else if ((byte & 0xF8u) == 0xF0u) {
      extra = 3;
      code_point = byte & 0x07u;
    } else {
      return false;
    }
    if (index + extra >= size) {
      return false;
    }
    for (std::size_t step = 1; step <= extra; ++step) {
      const auto next = static_cast<std::uint8_t>(text[index + step]);
      if (!is_continuation(next)) {
        return false;
      }
      code_point = (code_point << 6) | (next & 0x3Fu);
    }
    if (extra == 1 && code_point < 0x80u) {
      return false;
    }
    if (extra == 2 && code_point < 0x800u) {
      return false;
    }
    if (extra == 3 && code_point < 0x10000u) {
      return false;
    }
    if (code_point > 0x10FFFFu) {
      return false;
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return false;
    }
    index += extra + 1;
  }
  return true;
}

// ---------------------------------------------------------------------------
// ByteWriter
// ---------------------------------------------------------------------------

ByteWriter::ByteWriter(const Limits* limits, std::size_t reserve) : limits_(limits) {
  // A default-constructed Error is not a success value: the sticky error state
  // must be reset explicitly, or every write would be a silent no-op.
  error_ = make_error(ErrorCode::Ok, std::string(), std::string());
  buffer_.reserve(reserve);
}

void ByteWriter::fail(Error error) {
  if (ok()) {
    error_ = std::move(error);
  }
}

void ByteWriter::fail(ErrorCode code, std::string message, std::string detail) {
  fail(make_error(code, std::move(message), std::move(detail)));
}

void ByteWriter::reserve_for(std::size_t extra) {
  const std::size_t bound = limits_ ? limits_->max_frame_bytes : (1u << 20);
  if (extra > bound || buffer_.size() > bound - extra) {
    fail(ErrorCode::LimitExceeded, "encoded message exceeds the configured bound",
         std::to_string(buffer_.size() + extra) + " > " + std::to_string(bound));
    return;
  }
  const std::size_t needed = buffer_.size() + extra;
  if (buffer_.capacity() >= needed) {
    return;
  }
  // Grow geometrically: reserving exactly the needed size on every primitive
  // write would make encoding quadratic in the payload size.
  const std::size_t grown = buffer_.capacity() + buffer_.capacity() / 2 + 64;
  buffer_.reserve(std::min(bound, std::max(needed, grown)));
}

void ByteWriter::u8(std::uint8_t value) {
  if (!ok()) {
    return;
  }
  reserve_for(1);
  if (!ok()) {
    return;
  }
  buffer_.push_back(value);
}

void ByteWriter::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>(value & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void ByteWriter::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void ByteWriter::boolean(bool value) { u8(value ? 1u : 0u); }

void ByteWriter::text(std::string_view value, std::size_t max_len) {
  if (!ok()) {
    return;
  }
  if (value.size() > max_len || value.size() > kMaxTextBytes) {
    fail(ErrorCode::LimitExceeded, "text field exceeds its configured bound",
         std::to_string(value.size()) + " > " + std::to_string(max_len));
    return;
  }
  if (!is_valid_utf8(value)) {
    fail(ErrorCode::InvalidArgument, "text field is not valid UTF-8");
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  if (!ok()) {
    return;
  }
  reserve_for(value.size());
  if (!ok()) {
    return;
  }
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void ByteWriter::blob(const std::vector<std::uint8_t>& value, std::size_t max_len) {
  if (!ok()) {
    return;
  }
  if (value.size() > max_len || value.size() > kMaxBlobBytes) {
    fail(ErrorCode::LimitExceeded, "binary field exceeds its configured bound");
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  if (!ok()) {
    return;
  }
  reserve_for(value.size());
  if (!ok()) {
    return;
  }
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

// ---------------------------------------------------------------------------
// ByteReader
// ---------------------------------------------------------------------------

ByteReader::ByteReader(const std::uint8_t* data, std::size_t size, const Limits& limits)
    : data_(data), size_(size), limits_(limits) {
  error_ = make_error(ErrorCode::Ok, std::string(), std::string());
}

void ByteReader::fail(Error error) {
  if (ok()) {
    error_ = std::move(error);
  }
}

void ByteReader::fail(ErrorCode code, std::string message, std::string detail) {
  fail(make_error(code, std::move(message), std::move(detail)));
}

std::uint8_t ByteReader::u8() {
  if (!ok()) {
    return 0;
  }
  if (offset_ >= size_) {
    fail(ErrorCode::ProtocolViolation, "truncated message: read past the end of the buffer");
    return 0;
  }
  return data_[offset_++];
}

std::uint16_t ByteReader::u16() {
  const std::uint16_t low = u8();
  const std::uint16_t high = u8();
  return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8));
}

std::uint32_t ByteReader::u32() {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(u8()) << shift;
  }
  return value;
}

std::uint64_t ByteReader::u64() {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(u8()) << shift;
  }
  return value;
}

std::int64_t ByteReader::i64() { return static_cast<std::int64_t>(u64()); }

bool ByteReader::boolean() {
  const std::uint8_t raw = u8();
  if (!ok()) {
    return false;
  }
  if (raw > 1) {
    fail(ErrorCode::ProtocolViolation, "boolean field is not 0 or 1", std::to_string(raw));
    return false;
  }
  return raw == 1;
}

std::string ByteReader::text(std::size_t max_len) {
  if (!ok()) {
    return {};
  }
  const std::uint32_t declared = u32();
  if (!ok()) {
    return {};
  }
  if (declared > max_len || declared > kMaxTextBytes) {
    fail(ErrorCode::LimitExceeded, "declared text length exceeds its configured bound",
         std::to_string(declared) + " > " + std::to_string(max_len));
    return {};
  }
  if (declared > remaining()) {
    fail(ErrorCode::ProtocolViolation, "declared text length exceeds the remaining bytes",
         std::to_string(declared));
    return {};
  }
  std::string out(reinterpret_cast<const char*>(data_ + offset_), declared);
  offset_ += declared;
  if (!is_valid_utf8(out)) {
    fail(ErrorCode::InvalidArgument, "text field is not valid UTF-8");
    return {};
  }
  return out;
}

std::vector<std::uint8_t> ByteReader::blob(std::size_t max_len) {
  if (!ok()) {
    return {};
  }
  const std::uint32_t declared = u32();
  if (!ok()) {
    return {};
  }
  if (declared > max_len || declared > kMaxBlobBytes) {
    fail(ErrorCode::LimitExceeded, "declared binary length exceeds its configured bound");
    return {};
  }
  if (declared > remaining()) {
    fail(ErrorCode::ProtocolViolation, "declared binary length exceeds the remaining bytes");
    return {};
  }
  std::vector<std::uint8_t> out(data_ + offset_, data_ + offset_ + declared);
  offset_ += declared;
  return out;
}

Status ByteReader::finish() const {
  if (!ok()) {
    return error_;
  }
  if (offset_ != size_) {
    return failure(ErrorCode::ProtocolViolation, "message carries trailing bytes",
                   std::to_string(size_ - offset_));
  }
  return success();
}

namespace codec {
namespace {

constexpr std::size_t kSmall = 256;
constexpr std::size_t kLarge = 2048;

template <class Id>
void encode_id(ByteWriter& writer, const Id& id) {
  writer.u64(id.value());
}

template <class Id>
Id decode_id(ByteReader& reader) {
  return Id::from_value(reader.u64());
}

void encode_string_list(ByteWriter& writer, const std::vector<PartitionProfileId>& values,
                        std::size_t max_count) {
  writer.u32(static_cast<std::uint32_t>(values.size()));
  for (const PartitionProfileId& value : values) {
    encode_id(writer, value);
  }
  (void)max_count;
}

std::vector<PartitionProfileId> decode_profile_id_list(ByteReader& reader, std::size_t max_count) {
  return reader.list<PartitionProfileId>(max_count, 8, [](ByteReader& inner) {
    return decode_id<PartitionProfileId>(inner);
  });
}

void encode_string(ByteWriter& writer, const std::string& value, std::size_t max_len) {
  writer.text(value, max_len);
}

}  // namespace

void encode(ByteWriter& writer, const ResourceVector& value) {
  writer.u32(value.mask());
  for (const std::uint64_t entry : value.raw()) {
    writer.u64(entry);
  }
}

ResourceVector decode(ByteReader& reader) {
  ResourceVector vector;
  vector.set_mask(reader.u32());
  std::array<std::uint64_t, kResourceDimensionCount>& raw = vector.mutable_raw();
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    raw[index] = reader.u64();
  }
  if (reader.ok()) {
    const Status status = vector.validate_dimensions();
    if (!status.ok()) {
      reader.fail(status.error());
    }
  }
  return vector;
}

void encode(ByteWriter& writer, const Limits& value) {
  const std::uint64_t fields[] = {
      value.max_accelerators,
      value.max_profiles,
      value.max_partitions_per_accelerator,
      value.max_partitions,
      value.max_assignments,
      value.max_reservations,
      value.max_active_reservations,
      value.max_pending_plans,
      value.max_plan_steps,
      value.max_pending_attempts,
      value.max_attempt_history,
      value.max_snapshot_retention,
      value.max_events,
      value.max_explanations,
      value.max_reconciliation_findings,
      value.max_metadata_bytes,
      value.max_native_id_bytes,
      value.max_name_bytes,
      value.max_workers,
      value.max_frame_bytes,
      value.max_message_queue,
      value.max_synthetic_devices,
      value.max_synthetic_partitions,
      value.max_persisted_records,
      value.max_profile_exclusions,
      value.max_retry_state,
  };
  for (const std::uint64_t field : fields) {
    writer.u64(field);
  }
}

Limits decode_limits(ByteReader& reader) {
  Limits limits;
  limits.max_accelerators = static_cast<std::size_t>(reader.u64());
  limits.max_profiles = static_cast<std::size_t>(reader.u64());
  limits.max_partitions_per_accelerator = static_cast<std::size_t>(reader.u64());
  limits.max_partitions = static_cast<std::size_t>(reader.u64());
  limits.max_assignments = static_cast<std::size_t>(reader.u64());
  limits.max_reservations = static_cast<std::size_t>(reader.u64());
  limits.max_active_reservations = static_cast<std::size_t>(reader.u64());
  limits.max_pending_plans = static_cast<std::size_t>(reader.u64());
  limits.max_plan_steps = static_cast<std::size_t>(reader.u64());
  limits.max_pending_attempts = static_cast<std::size_t>(reader.u64());
  limits.max_attempt_history = static_cast<std::size_t>(reader.u64());
  limits.max_snapshot_retention = static_cast<std::size_t>(reader.u64());
  limits.max_events = static_cast<std::size_t>(reader.u64());
  limits.max_explanations = static_cast<std::size_t>(reader.u64());
  limits.max_reconciliation_findings = static_cast<std::size_t>(reader.u64());
  limits.max_metadata_bytes = static_cast<std::size_t>(reader.u64());
  limits.max_native_id_bytes = static_cast<std::size_t>(reader.u64());
  limits.max_name_bytes = static_cast<std::size_t>(reader.u64());
  limits.max_workers = static_cast<std::size_t>(reader.u64());
  limits.max_frame_bytes = static_cast<std::size_t>(reader.u64());
  limits.max_message_queue = static_cast<std::size_t>(reader.u64());
  limits.max_synthetic_devices = static_cast<std::size_t>(reader.u64());
  limits.max_synthetic_partitions = static_cast<std::size_t>(reader.u64());
  limits.max_persisted_records = static_cast<std::size_t>(reader.u64());
  limits.max_profile_exclusions = static_cast<std::size_t>(reader.u64());
  limits.max_retry_state = static_cast<std::size_t>(reader.u64());
  if (reader.ok()) {
    const Status valid = limits.validate();
    if (!valid.ok()) {
      reader.fail(valid.error());
    }
  }
  return limits;
}

void encode(ByteWriter& writer, const EvidenceStamp& value) {
  encode_id(writer, value.generation);
  encode_enum(writer, value.provenance);
  writer.u64(value.observed_at_ms);
  writer.u64(value.ttl_ms);
  encode_string(writer, value.source, kSmall);
  writer.boolean(value.observed);
}

EvidenceStamp decode_evidence(ByteReader& reader) {
  EvidenceStamp stamp;
  stamp.generation = decode_id<EvidenceGeneration>(reader);
  stamp.provenance =
      decode_enum<EvidenceProvenance>(reader, 4, "evidence provenance");
  stamp.observed_at_ms = reader.u64();
  stamp.ttl_ms = reader.u64();
  stamp.source = reader.text(kSmall);
  stamp.observed = reader.boolean();
  return stamp;
}

void encode(ByteWriter& writer, const IsolationSet& value) { writer.u32(value.mask()); }

IsolationSet decode_isolation(ByteReader& reader) {
  IsolationSet set;
  set.set_mask(reader.u32());
  if (reader.ok() && (set.mask() >> kIsolationPropertyCount) != 0) {
    reader.fail(ErrorCode::ProtocolViolation, "isolation mask carries undefined properties");
  }
  return set;
}

void encode(ByteWriter& writer, const ProfileAlignment& value) {
  writer.u32(value.compute_slice_multiple);
  writer.u32(value.memory_slice_multiple);
  writer.boolean(value.requires_contiguous_slices);
  writer.boolean(value.requires_atomic_slice);
}

ProfileAlignment decode_profile_alignment(ByteReader& reader) {
  ProfileAlignment alignment;
  alignment.compute_slice_multiple = reader.u32();
  alignment.memory_slice_multiple = reader.u32();
  alignment.requires_contiguous_slices = reader.boolean();
  alignment.requires_atomic_slice = reader.boolean();
  return alignment;
}

void encode(ByteWriter& writer, const PartitionProfile& value) {
  encode_id(writer, value.id);
  encode_id(writer, value.generation);
  encode_string(writer, value.name, kSmall);
  encode_string(writer, value.vendor_native, kSmall);
  encode_string(writer, value.backend, kSmall);
  encode_enum(writer, value.mechanism);
  encode(writer, value.resources);
  writer.u32(value.compute_slice_count);
  writer.u32(value.memory_slice_count);
  encode_enum(writer, value.engine_grouping);
  writer.u32(value.max_multiplicity);
  encode(writer, value.alignment);
  encode_string_list(writer, value.mutually_exclusive_with, 64);
  writer.boolean(value.requires_full_device_reconfiguration);
  writer.boolean(value.live_repartitioning_supported);
  writer.u32(static_cast<std::uint32_t>(value.backend_requirements.size()));
  for (const std::string& requirement : value.backend_requirements) {
    encode_string(writer, requirement, kSmall);
  }
  encode_string(writer, value.description, kSmall);
}

PartitionProfile decode_profile(ByteReader& reader) {
  PartitionProfile profile;
  profile.id = decode_id<PartitionProfileId>(reader);
  profile.generation = decode_id<PartitionProfileGeneration>(reader);
  profile.name = reader.text(kSmall);
  profile.vendor_native = reader.text(kSmall);
  profile.backend = reader.text(kSmall);
  profile.mechanism =
      decode_enum<PartitionMechanism>(reader, static_cast<std::uint8_t>(PartitionMechanism::Count),
                                      "partition mechanism");
  profile.resources = decode(reader);
  profile.compute_slice_count = reader.u32();
  profile.memory_slice_count = reader.u32();
  profile.engine_grouping =
      decode_enum<EngineGrouping>(reader, static_cast<std::uint8_t>(EngineGrouping::Count),
                                  "engine grouping");
  profile.max_multiplicity = reader.u32();
  profile.alignment = decode_profile_alignment(reader);
  profile.mutually_exclusive_with = decode_profile_id_list(reader, 64);
  profile.requires_full_device_reconfiguration = reader.boolean();
  profile.live_repartitioning_supported = reader.boolean();
  profile.backend_requirements =
      reader.list<std::string>(64, 4, [](ByteReader& inner) { return inner.text(kSmall); });
  profile.description = reader.text(kSmall);
  return profile;
}

void encode(ByteWriter& writer, const ProfileCombinationRule& value) {
  encode_id(writer, value.profile);
  writer.u32(value.max_instances);
  encode_string_list(writer, value.incompatible_with, 64);
  writer.boolean(value.requires_exclusive_device);
}

ProfileCombinationRule decode_combination_rule(ByteReader& reader) {
  ProfileCombinationRule rule;
  rule.profile = decode_id<PartitionProfileId>(reader);
  rule.max_instances = reader.u32();
  rule.incompatible_with = decode_profile_id_list(reader, 64);
  rule.requires_exclusive_device = reader.boolean();
  return rule;
}

void encode(ByteWriter& writer, const PartitionCapability& value) {
  encode_id(writer, value.accelerator);
  encode_id(writer, value.device_generation);
  encode_id(writer, value.generation);
  encode_id(writer, value.topology_generation);
  encode_enum(writer, value.support);
  encode_enum(writer, value.mechanism);
  encode_string(writer, value.mechanism_name, kSmall);
  encode_string_list(writer, value.supported_profiles, 512);
  writer.u32(static_cast<std::uint32_t>(value.combination_rules.size()));
  for (const ProfileCombinationRule& rule : value.combination_rules) {
    encode(writer, rule);
  }
  writer.u32(value.max_partition_count);
  writer.boolean(value.requires_reset_for_reconfiguration);
  writer.boolean(value.live_reconfiguration_supported);
  encode(writer, value.isolation);
  writer.u32(static_cast<std::uint32_t>(value.exposed_dimensions.size()));
  for (const ResourceDimension dimension : value.exposed_dimensions) {
    encode_enum(writer, dimension);
  }
  encode(writer, value.minimum_allocation);
  writer.u32(value.slice_granularity);
  writer.u32(value.total_compute_slices);
  writer.u32(value.total_memory_slices);
  writer.u64(value.drain_estimate_ms);
  writer.u64(value.reconfiguration_estimate_ms);
  encode_string(writer, value.backend_name, kSmall);
  encode_string(writer, value.backend_version, kSmall);
  encode_string(writer, value.driver_version, kSmall);
  encode_string(writer, value.tooling_requirement, kSmall);
  encode(writer, value.evidence);
  encode_string(writer, value.unsupported_reason, kSmall);
}

PartitionCapability decode_capability(ByteReader& reader) {
  PartitionCapability capability;
  capability.accelerator = decode_id<AcceleratorId>(reader);
  capability.device_generation = decode_id<AcceleratorGeneration>(reader);
  capability.generation = decode_id<CapabilityGeneration>(reader);
  capability.topology_generation = decode_id<TopologyGeneration>(reader);
  capability.support =
      decode_enum<PartitionSupportState>(reader, 4, "partition support state");
  capability.mechanism =
      decode_enum<PartitionMechanism>(reader, static_cast<std::uint8_t>(PartitionMechanism::Count),
                                      "partition mechanism");
  capability.mechanism_name = reader.text(kSmall);
  capability.supported_profiles = decode_profile_id_list(reader, 512);
  capability.combination_rules =
      reader.list<ProfileCombinationRule>(512, 4, [](ByteReader& inner) {
        return decode_combination_rule(inner);
      });
  capability.max_partition_count = reader.u32();
  capability.requires_reset_for_reconfiguration = reader.boolean();
  capability.live_reconfiguration_supported = reader.boolean();
  capability.isolation = decode_isolation(reader);
  capability.exposed_dimensions =
      reader.list<ResourceDimension>(kResourceDimensionCount, 1, [](ByteReader& inner) {
        return decode_enum<ResourceDimension>(
            inner, static_cast<std::uint8_t>(ResourceDimension::Count), "resource dimension");
      });
  capability.minimum_allocation = decode(reader);
  capability.slice_granularity = reader.u32();
  capability.total_compute_slices = reader.u32();
  capability.total_memory_slices = reader.u32();
  capability.drain_estimate_ms = reader.u64();
  capability.reconfiguration_estimate_ms = reader.u64();
  capability.backend_name = reader.text(kSmall);
  capability.backend_version = reader.text(kSmall);
  capability.driver_version = reader.text(kSmall);
  capability.tooling_requirement = reader.text(kSmall);
  capability.evidence = decode_evidence(reader);
  capability.unsupported_reason = reader.text(kSmall);
  return capability;
}

void encode(ByteWriter& writer, const DeviceIdentifiers& value) {
  encode_string(writer, value.vendor, kSmall);
  encode_string(writer, value.model, kSmall);
  encode_string(writer, value.uuid, kSmall);
  encode_string(writer, value.pci_bus_id, kSmall);
  encode_string(writer, value.hardware_id, kSmall);
  encode_string(writer, value.firmware_version, kSmall);
  encode_string(writer, value.driver_version, kSmall);
  encode_string(writer, value.serial, kSmall);
  encode_string(writer, value.compute_capability, kSmall);
}

DeviceIdentifiers decode_identifiers(ByteReader& reader) {
  DeviceIdentifiers identifiers;
  identifiers.vendor = reader.text(kSmall);
  identifiers.model = reader.text(kSmall);
  identifiers.uuid = reader.text(kSmall);
  identifiers.pci_bus_id = reader.text(kSmall);
  identifiers.hardware_id = reader.text(kSmall);
  identifiers.firmware_version = reader.text(kSmall);
  identifiers.driver_version = reader.text(kSmall);
  identifiers.serial = reader.text(kSmall);
  identifiers.compute_capability = reader.text(kSmall);
  return identifiers;
}

void encode(ByteWriter& writer, const LocalityDomain& value) {
  encode_string(writer, value.name, kSmall);
  writer.i64(value.numa_node);
}

LocalityDomain decode_locality(ByteReader& reader) {
  LocalityDomain locality;
  locality.name = reader.text(kSmall);
  locality.numa_node = static_cast<std::int32_t>(reader.i64());
  return locality;
}

void encode(ByteWriter& writer, const HealthEvidence& value) {
  encode_enum(writer, value.state);
  encode(writer, value.stamp);
  encode_string(writer, value.source, kSmall);
}

HealthEvidence decode_health(ByteReader& reader) {
  HealthEvidence health;
  health.state = decode_enum<HealthState>(reader, 5, "health state");
  health.stamp = decode_evidence(reader);
  health.source = reader.text(kSmall);
  return health;
}

void encode(ByteWriter& writer, const AcceleratorRecord& value) {
  encode_id(writer, value.id);
  encode_id(writer, value.generation);
  writer.u64(value.boot_id.value());
  encode_string(writer, value.backend, kSmall);
  encode(writer, value.identifiers);
  encode(writer, value.physical_totals);
  encode(writer, value.capability);
  encode_id(writer, value.topology_generation);
  encode(writer, value.locality);
  encode(writer, value.health);
  encode(writer, value.evidence);
  encode_enum(writer, value.provenance);
  encode_string(writer, value.unsupported_reason, kSmall);
  writer.u64(value.registered_at_ms);
  writer.u64(value.updated_at_ms);
}

AcceleratorRecord decode_accelerator(ByteReader& reader) {
  AcceleratorRecord record;
  record.id = decode_id<AcceleratorId>(reader);
  record.generation = decode_id<AcceleratorGeneration>(reader);
  record.boot_id = AcceleratorBootId::from_value(reader.u64());
  record.backend = reader.text(kSmall);
  record.identifiers = decode_identifiers(reader);
  record.physical_totals = decode(reader);
  record.capability = decode_capability(reader);
  record.topology_generation = decode_id<TopologyGeneration>(reader);
  record.locality = decode_locality(reader);
  record.health = decode_health(reader);
  record.evidence = decode_evidence(reader);
  record.provenance = decode_enum<EvidenceProvenance>(reader, 4, "evidence provenance");
  record.unsupported_reason = reader.text(kSmall);
  record.registered_at_ms = reader.u64();
  record.updated_at_ms = reader.u64();
  return record;
}

void encode(ByteWriter& writer, const PartitionNativeIdentity& value) {
  encode_string(writer, value.backend, kSmall);
  encode_string(writer, value.native_id, kSmall);
  encode_string(writer, value.parent_native_id, kSmall);
  encode_string(writer, value.instance_uuid, kSmall);
}

PartitionNativeIdentity decode_native_identity(ByteReader& reader) {
  PartitionNativeIdentity identity;
  identity.backend = reader.text(kSmall);
  identity.native_id = reader.text(kSmall);
  identity.parent_native_id = reader.text(kSmall);
  identity.instance_uuid = reader.text(kSmall);
  return identity;
}

void encode(ByteWriter& writer, const IsolationDomain& value) {
  encode_string(writer, value.name, kSmall);
  encode(writer, value.guarantees);
  writer.boolean(value.backend_defined);
}

IsolationDomain decode_isolation_domain(ByteReader& reader) {
  IsolationDomain domain;
  domain.name = reader.text(kSmall);
  domain.guarantees = decode_isolation(reader);
  domain.backend_defined = reader.boolean();
  return domain;
}

void encode(ByteWriter& writer, const DrainProgress& value) {
  encode_enum(writer, value.state);
  writer.u32(value.outstanding_assignments);
  writer.u32(value.total_assignments);
  writer.u64(value.started_at_ms);
  writer.u64(value.completed_at_ms);
  encode_string(writer, value.blocked_reason, kSmall);
  encode_string(writer, value.blocker, kSmall);
}

DrainProgress decode_drain(ByteReader& reader) {
  DrainProgress drain;
  drain.state = decode_enum<DrainState>(reader, 4, "drain state");
  drain.outstanding_assignments = reader.u32();
  drain.total_assignments = reader.u32();
  drain.started_at_ms = reader.u64();
  drain.completed_at_ms = reader.u64();
  drain.blocked_reason = reader.text(kSmall);
  drain.blocker = reader.text(kSmall);
  return drain;
}

void encode(ByteWriter& writer, const PartitionRecord& value) {
  encode_id(writer, value.id);
  encode_id(writer, value.generation);
  encode_id(writer, value.profile);
  encode_id(writer, value.profile_generation);
  encode_enum(writer, value.mechanism);
  encode_id(writer, value.accelerator);
  encode_id(writer, value.accelerator_generation);
  writer.u64(value.accelerator_boot.value());
  encode(writer, value.resources);
  encode_enum(writer, value.state);
  encode(writer, value.native_identity);
  encode(writer, value.isolation);
  writer.u8(value.held_bucket.has_value()
                ? static_cast<std::uint8_t>(*value.held_bucket)
                : static_cast<std::uint8_t>(0xFFu));
  encode_enum(writer, value.assignment_state);
  encode(writer, value.drain);
  encode_id(writer, value.reservation);
  encode_id(writer, value.last_attempt);
  writer.u32(static_cast<std::uint32_t>(value.superseded_generations.size()));
  for (const PartitionGeneration generation : value.superseded_generations) {
    encode_id(writer, generation);
  }
  encode(writer, value.evidence);
  encode_enum(writer, value.provenance);
  writer.u64(value.created_at_ms);
  writer.u64(value.last_transition_at_ms);
  encode_string(writer, value.last_transition_reason, kSmall);
  writer.boolean(value.externally_observed);
}

PartitionRecord decode_partition(ByteReader& reader) {
  PartitionRecord record;
  record.id = decode_id<PartitionId>(reader);
  record.generation = decode_id<PartitionGeneration>(reader);
  record.profile = decode_id<PartitionProfileId>(reader);
  record.profile_generation = decode_id<PartitionProfileGeneration>(reader);
  record.mechanism =
      decode_enum<PartitionMechanism>(reader, static_cast<std::uint8_t>(PartitionMechanism::Count),
                                      "partition mechanism");
  record.accelerator = decode_id<AcceleratorId>(reader);
  record.accelerator_generation = decode_id<AcceleratorGeneration>(reader);
  record.accelerator_boot = AcceleratorBootId::from_value(reader.u64());
  record.resources = decode(reader);
  record.state = decode_enum<PartitionState>(
      reader, static_cast<std::uint8_t>(PartitionState::Count), "partition state");
  record.native_identity = decode_native_identity(reader);
  record.isolation = decode_isolation_domain(reader);
  {
    const std::uint8_t raw = reader.u8();
    if (reader.ok() && raw != 0xFFu) {
      if (raw >= kCapacityBucketCount) {
        reader.fail(ErrorCode::ProtocolViolation, "invalid capacity bucket value",
                    std::to_string(raw));
      } else {
        record.held_bucket = static_cast<CapacityBucket>(raw);
      }
    }
  }
  record.assignment_state = decode_enum<AssignmentState>(reader, 4, "assignment state");
  record.drain = decode_drain(reader);
  record.reservation = decode_id<PartitionReservationId>(reader);
  record.last_attempt = decode_id<PartitionAttemptId>(reader);
  record.superseded_generations =
      reader.list<PartitionGeneration>(64, 8, [](ByteReader& inner) {
        return decode_id<PartitionGeneration>(inner);
      });
  record.evidence = decode_evidence(reader);
  record.provenance = decode_enum<EvidenceProvenance>(reader, 4, "evidence provenance");
  record.created_at_ms = reader.u64();
  record.last_transition_at_ms = reader.u64();
  record.last_transition_reason = reader.text(kSmall);
  record.externally_observed = reader.boolean();
  return record;
}

void encode(ByteWriter& writer, const IsolationRequirement& value) {
  encode(writer, value.required);
  encode_id(writer, value.policy);
  encode_id(writer, value.policy_generation);
}

IsolationRequirement decode_isolation_requirement(ByteReader& reader) {
  IsolationRequirement requirement;
  requirement.required = decode_isolation(reader);
  requirement.policy = decode_id<IsolationPolicyId>(reader);
  requirement.policy_generation = decode_id<IsolationPolicyGeneration>(reader);
  return requirement;
}

void encode(ByteWriter& writer, const PartitionAssignment& value) {
  encode_id(writer, value.id);
  encode_id(writer, value.partition);
  encode_id(writer, value.partition_generation);
  encode_id(writer, value.accelerator);
  encode_id(writer, value.accelerator_generation);
  encode_string(writer, value.workload_id, kSmall);
  encode_string(writer, value.tenant_id, kSmall);
  encode(writer, value.isolation);
  encode_id(writer, value.policy_generation);
  encode_id(writer, value.coordinator_epoch);
  encode_id(writer, value.evidence_generation);
  writer.boolean(value.exclusive);
  writer.u64(value.bound_at_ms);
  writer.u64(value.last_validated_at_ms);
}

PartitionAssignment decode_assignment(ByteReader& reader) {
  PartitionAssignment assignment;
  assignment.id = decode_id<PartitionAssignmentId>(reader);
  assignment.partition = decode_id<PartitionId>(reader);
  assignment.partition_generation = decode_id<PartitionGeneration>(reader);
  assignment.accelerator = decode_id<AcceleratorId>(reader);
  assignment.accelerator_generation = decode_id<AcceleratorGeneration>(reader);
  assignment.workload_id = reader.text(kSmall);
  assignment.tenant_id = reader.text(kSmall);
  assignment.isolation = decode_isolation_requirement(reader);
  assignment.policy_generation = decode_id<PolicyGeneration>(reader);
  assignment.coordinator_epoch = decode_id<CoordinatorEpoch>(reader);
  assignment.evidence_generation = decode_id<EvidenceGeneration>(reader);
  assignment.exclusive = reader.boolean();
  assignment.bound_at_ms = reader.u64();
  assignment.last_validated_at_ms = reader.u64();
  return assignment;
}

void encode(ByteWriter& writer, const CapacityLedger& value) {
  encode(writer, value.total());
  for (std::size_t index = 0; index < kCapacityBucketCount; ++index) {
    encode(writer, value.buckets()[index]);
  }
}

CapacityLedger decode_ledger(ByteReader& reader) {
  CapacityLedger ledger;
  const ResourceVector total = decode(reader);
  std::array<ResourceVector, kCapacityBucketCount> buckets{};
  for (std::size_t index = 0; index < kCapacityBucketCount; ++index) {
    buckets[index] = decode(reader);
  }
  // Re-establish the physical totals, then restore the encoded buckets. The
  // ledger is only usable when the invariant closes exactly and every bucket
  // carries the device's dimension set.
  const Status initialized = ledger.initialize_totals(total);
  if (!initialized.ok()) {
    reader.fail(initialized.error());
    return ledger;
  }
  for (std::size_t index = 0; index < kCapacityBucketCount; ++index) {
    if (!buckets[index].same_dimensions(total)) {
      reader.fail(ErrorCode::CorruptState,
                  "ledger bucket does not carry the device dimension set");
      return ledger;
    }
    ledger.mutable_buckets()[index] = buckets[index];
  }
  const Status validation = ledger.validate();
  if (!validation.ok()) {
    reader.fail(validation.error());
  }
  return ledger;
}

void encode(ByteWriter& writer, const PlanningPolicy& value) {
  encode_id(writer, value.generation);
  encode_string(writer, value.name, kSmall);
  writer.i64(value.weights.fragmentation);
  writer.i64(value.weights.capacity_waste);
  writer.i64(value.weights.destructive_reconfiguration);
  writer.i64(value.weights.drain);
  writer.i64(value.weights.mutation_count);
  writer.i64(value.weights.health);
  writer.i64(value.weights.locality);
  writer.i64(value.weights.future_optionality);
  writer.i64(value.weights.contiguity_preservation);
  writer.i64(value.weights.reconfiguration_cost);
  writer.i64(value.weights.live_reconfiguration_bonus);
  writer.boolean(value.allow_destructive_reconfiguration);
  writer.boolean(value.allow_drain);
  writer.u64(value.max_reconfiguration_downtime_ms);
  writer.boolean(value.require_fresh_evidence);
  writer.u64(value.max_evidence_age_ms);
  writer.boolean(value.require_healthy_device);
  writer.boolean(value.allow_degraded_device);
  writer.boolean(value.allow_external_adoption);
  writer.boolean(value.exclusive_by_default);
  writer.u32(value.max_partition_mutations_per_plan);
  writer.boolean(value.prefer_live_reconfiguration);
  encode(writer, value.minimum_isolation);
  writer.u32(static_cast<std::uint32_t>(value.protected_dimensions.size()));
  for (const ResourceDimension dimension : value.protected_dimensions) {
    encode_enum(writer, dimension);
  }
  writer.u32(value.max_instances_per_profile);
}

PlanningPolicy decode_policy(ByteReader& reader) {
  PlanningPolicy policy;
  policy.generation = decode_id<PolicyGeneration>(reader);
  policy.name = reader.text(kSmall);
  policy.weights.fragmentation = reader.i64();
  policy.weights.capacity_waste = reader.i64();
  policy.weights.destructive_reconfiguration = reader.i64();
  policy.weights.drain = reader.i64();
  policy.weights.mutation_count = reader.i64();
  policy.weights.health = reader.i64();
  policy.weights.locality = reader.i64();
  policy.weights.future_optionality = reader.i64();
  policy.weights.contiguity_preservation = reader.i64();
  policy.weights.reconfiguration_cost = reader.i64();
  policy.weights.live_reconfiguration_bonus = reader.i64();
  policy.allow_destructive_reconfiguration = reader.boolean();
  policy.allow_drain = reader.boolean();
  policy.max_reconfiguration_downtime_ms = reader.u64();
  policy.require_fresh_evidence = reader.boolean();
  policy.max_evidence_age_ms = reader.u64();
  policy.require_healthy_device = reader.boolean();
  policy.allow_degraded_device = reader.boolean();
  policy.allow_external_adoption = reader.boolean();
  policy.exclusive_by_default = reader.boolean();
  policy.max_partition_mutations_per_plan = reader.u32();
  policy.prefer_live_reconfiguration = reader.boolean();
  policy.minimum_isolation = decode_isolation_requirement(reader);
  policy.protected_dimensions =
      reader.list<ResourceDimension>(kResourceDimensionCount, 1, [](ByteReader& inner) {
        return decode_enum<ResourceDimension>(
            inner, static_cast<std::uint8_t>(ResourceDimension::Count), "resource dimension");
      });
  policy.max_instances_per_profile = reader.u32();
  return policy;
}

void encode(ByteWriter& writer, const PartitionReservation& value) {
  encode_id(writer, value.id);
  encode_id(writer, value.plan);
  encode_id(writer, value.plan_generation);
  encode_id(writer, value.accelerator);
  encode_id(writer, value.accelerator_generation);
  writer.u64(value.accelerator_boot.value());
  encode_id(writer, value.capability_generation);
  encode_id(writer, value.topology_generation);
  encode_id(writer, value.policy_generation);
  encode_id(writer, value.coordinator_epoch);
  encode_id(writer, value.worker);
  writer.u64(value.worker_boot.value());
  encode_enum(writer, value.lifecycle);
  writer.u32(static_cast<std::uint32_t>(value.partitions.size()));
  for (const PartitionId partition : value.partitions) {
    encode_id(writer, partition);
  }
  writer.u32(static_cast<std::uint32_t>(value.partition_generations.size()));
  for (const PartitionGeneration generation : value.partition_generations) {
    encode_id(writer, generation);
  }
  encode(writer, value.resources);
  encode_id(writer, value.attempt);
  writer.u32(value.planned_mutations);
  writer.boolean(value.destructive);
  writer.u64(value.created_at_ms);
  writer.u64(value.updated_at_ms);
  writer.u64(value.expires_at_ms);
  writer.u32(value.rollback_count);
  encode_string(writer, value.reason, kSmall);
}

PartitionReservation decode_reservation(ByteReader& reader) {
  PartitionReservation reservation;
  reservation.id = decode_id<PartitionReservationId>(reader);
  reservation.plan = decode_id<PartitionPlanId>(reader);
  reservation.plan_generation = decode_id<PartitionPlanGeneration>(reader);
  reservation.accelerator = decode_id<AcceleratorId>(reader);
  reservation.accelerator_generation = decode_id<AcceleratorGeneration>(reader);
  reservation.accelerator_boot = AcceleratorBootId::from_value(reader.u64());
  reservation.capability_generation = decode_id<CapabilityGeneration>(reader);
  reservation.topology_generation = decode_id<TopologyGeneration>(reader);
  reservation.policy_generation = decode_id<PolicyGeneration>(reader);
  reservation.coordinator_epoch = decode_id<CoordinatorEpoch>(reader);
  reservation.worker = decode_id<WorkerId>(reader);
  reservation.worker_boot = WorkerBootId::from_value(reader.u64());
  reservation.lifecycle =
      decode_enum<ReservationLifecycle>(reader, 8, "reservation lifecycle");
  reservation.partitions = reader.list<PartitionId>(256, 8, [](ByteReader& inner) {
    return decode_id<PartitionId>(inner);
  });
  reservation.partition_generations =
      reader.list<PartitionGeneration>(256, 8, [](ByteReader& inner) {
        return decode_id<PartitionGeneration>(inner);
      });
  reservation.resources = decode(reader);
  reservation.attempt = decode_id<PartitionAttemptId>(reader);
  reservation.planned_mutations = reader.u32();
  reservation.destructive = reader.boolean();
  reservation.created_at_ms = reader.u64();
  reservation.updated_at_ms = reader.u64();
  reservation.expires_at_ms = reader.u64();
  reservation.rollback_count = reader.u32();
  reservation.reason = reader.text(kSmall);
  return reservation;
}

void encode(ByteWriter& writer, const PlannedPartition& value) {
  encode_id(writer, value.profile);
  encode_id(writer, value.profile_generation);
  encode(writer, value.resources);
  writer.u32(value.ordinal);
  encode_string(writer, value.vendor_native_profile, kSmall);
}

PlannedPartition decode_planned_partition(ByteReader& reader) {
  PlannedPartition planned;
  planned.profile = decode_id<PartitionProfileId>(reader);
  planned.profile_generation = decode_id<PartitionProfileGeneration>(reader);
  planned.resources = decode(reader);
  planned.ordinal = reader.u32();
  planned.vendor_native_profile = reader.text(kSmall);
  return planned;
}

void encode(ByteWriter& writer, const DeviceSelector& value) {
  encode_string(writer, value.backend, kSmall);
  encode_string(writer, value.vendor, kSmall);
  encode_string(writer, value.device_family, kSmall);
  encode_string(writer, value.locality_domain, kSmall);
  writer.u32(static_cast<std::uint32_t>(value.allowed.size()));
  for (const AcceleratorId id : value.allowed) {
    writer.u64(id.value());
  }
  writer.u32(static_cast<std::uint32_t>(value.excluded.size()));
  for (const AcceleratorId id : value.excluded) {
    writer.u64(id.value());
  }
}

DeviceSelector decode_device_selector(ByteReader& reader) {
  DeviceSelector selector;
  selector.backend = reader.text(kSmall);
  selector.vendor = reader.text(kSmall);
  selector.device_family = reader.text(kSmall);
  selector.locality_domain = reader.text(kSmall);
  selector.allowed = reader.list<AcceleratorId>(64, 8, [](ByteReader& inner) {
    return AcceleratorId::from_value(inner.u64());
  });
  selector.excluded = reader.list<AcceleratorId>(64, 8, [](ByteReader& inner) {
    return AcceleratorId::from_value(inner.u64());
  });
  return selector;
}

void encode(ByteWriter& writer, const PartitionRequest& value) {
  encode_id(writer, value.profile);
  writer.u32(value.count);
  encode(writer, value.minimum_resources);
  encode(writer, value.isolation);
  writer.boolean(value.exclusive);
  writer.boolean(value.allow_drain);
  writer.boolean(value.allow_destructive_reconfiguration);
  writer.boolean(value.allow_degraded_device);
  writer.boolean(value.allow_shared_device);
  encode(writer, value.selector);
  encode_string(writer, value.policy_class, kSmall);
  writer.u64(value.max_evidence_age_ms);
  encode_string(writer, value.requester, kSmall);
  writer.boolean(value.all_or_nothing);
  encode_string(writer, value.request_tag, kSmall);
}

PartitionRequest decode_request(ByteReader& reader) {
  PartitionRequest request;
  request.profile = decode_id<PartitionProfileId>(reader);
  request.count = reader.u32();
  request.minimum_resources = decode(reader);
  request.isolation = decode_isolation_requirement(reader);
  request.exclusive = reader.boolean();
  request.allow_drain = reader.boolean();
  request.allow_destructive_reconfiguration = reader.boolean();
  request.allow_degraded_device = reader.boolean();
  request.allow_shared_device = reader.boolean();
  request.selector = decode_device_selector(reader);
  request.policy_class = reader.text(kSmall);
  request.max_evidence_age_ms = reader.u64();
  request.requester = reader.text(kSmall);
  request.all_or_nothing = reader.boolean();
  request.request_tag = reader.text(kSmall);
  return request;
}

void encode(ByteWriter& writer, const PlanStep& value) {
  writer.u32(value.index);
  encode_enum(writer, value.kind);
  encode_id(writer, value.accelerator);
  encode_id(writer, value.accelerator_generation);
  writer.u64(value.accelerator_boot.value());
  encode_id(writer, value.partition);
  encode_id(writer, value.profile);
  writer.u32(value.planned_index);
  writer.boolean(value.destructive);
  writer.boolean(value.requires_reset);
  writer.u64(value.estimated_duration_ms);
  encode_string(writer, value.detail, kSmall);
}

PlanStep decode_plan_step(ByteReader& reader) {
  PlanStep step;
  step.index = reader.u32();
  step.kind = decode_enum<PlanStepKind>(
      reader, static_cast<std::uint8_t>(PlanStepKind::Count), "plan step kind");
  step.accelerator = decode_id<AcceleratorId>(reader);
  step.accelerator_generation = decode_id<AcceleratorGeneration>(reader);
  step.accelerator_boot = AcceleratorBootId::from_value(reader.u64());
  step.partition = decode_id<PartitionId>(reader);
  step.profile = decode_id<PartitionProfileId>(reader);
  step.planned_index = reader.u32();
  step.destructive = reader.boolean();
  step.requires_reset = reader.boolean();
  step.estimated_duration_ms = reader.u64();
  step.detail = reader.text(kSmall);
  return step;
}

void encode(ByteWriter& writer, const PlanScoreTerm& value) {
  encode_enum(writer, value.code);
  encode_string(writer, value.subject, kSmall);
  writer.i64(value.weight);
  writer.i64(value.raw);
  writer.i64(value.contribution);
}

PlanScoreTerm decode_plan_score_term(ByteReader& reader) {
  PlanScoreTerm term;
  term.code = decode_enum<ExplanationCode>(
      reader, static_cast<std::uint8_t>(ExplanationCode::Count), "explanation code");
  term.subject = reader.text(kSmall);
  term.weight = reader.i64();
  term.raw = reader.i64();
  term.contribution = reader.i64();
  return term;
}

void encode(ByteWriter& writer, const CandidateEvaluation& value) {
  encode_id(writer, value.accelerator);
  encode_id(writer, value.accelerator_generation);
  writer.boolean(value.eligible);
  encode_enum(writer, value.outcome);
  writer.i64(value.score.total);
  writer.u32(static_cast<std::uint32_t>(value.score.terms.size()));
  for (const PlanScoreTerm& term : value.score.terms) {
    encode(writer, term);
  }
  writer.u32(static_cast<std::uint32_t>(value.explanation.factors().size()));
  for (const ExplanationFactor& factor : value.explanation.factors()) {
    encode_enum(writer, factor.code);
    encode_string(writer, factor.subject, kSmall);
    encode_string(writer, factor.detail, kLarge);
    writer.i64(factor.magnitude);
  }
}

CandidateEvaluation decode_candidate(ByteReader& reader) {
  CandidateEvaluation candidate;
  candidate.accelerator = decode_id<AcceleratorId>(reader);
  candidate.accelerator_generation = decode_id<AcceleratorGeneration>(reader);
  candidate.eligible = reader.boolean();
  candidate.outcome = decode_enum<PlanOutcome>(
      reader, static_cast<std::uint8_t>(PlanOutcome::Count), "plan outcome");
  candidate.score.total = reader.i64();
  candidate.score.terms = reader.list<PlanScoreTerm>(64, 8, [](ByteReader& inner) {
    return decode_plan_score_term(inner);
  });
  const std::size_t factor_count = reader.u32();
  if (reader.ok() && factor_count > 128) {
    reader.fail(ErrorCode::LimitExceeded, "explanation factor count exceeds the bound");
    return candidate;
  }
  for (std::size_t index = 0; index < factor_count && reader.ok(); ++index) {
    ExplanationFactor factor;
    factor.code = decode_enum<ExplanationCode>(
        reader, static_cast<std::uint8_t>(ExplanationCode::Count), "explanation code");
    factor.subject = reader.text(kSmall);
    factor.detail = reader.text(kLarge);
    factor.magnitude = reader.i64();
    candidate.explanation.add(factor);
  }
  return candidate;
}

void encode(ByteWriter& writer, const PartitionAttempt& value) {
  encode_id(writer, value.id);
  encode_enum(writer, value.kind);
  encode_id(writer, value.reservation);
  encode_id(writer, value.accelerator);
  encode_id(writer, value.accelerator_generation);
  writer.u64(value.accelerator_boot.value());
  encode_id(writer, value.capability_generation);
  encode_id(writer, value.coordinator_epoch);
  encode_id(writer, value.worker);
  writer.u64(value.worker_boot.value());
  writer.u32(static_cast<std::uint32_t>(value.partitions.size()));
  for (const PartitionId partition : value.partitions) {
    encode_id(writer, partition);
  }
  writer.u32(static_cast<std::uint32_t>(value.desired.size()));
  for (const PlannedPartition& planned : value.desired) {
    encode(writer, planned);
  }
  encode_enum(writer, value.state);
  writer.u64(value.token);
  writer.u64(value.registered_at_ms);
  writer.u64(value.dispatched_at_ms);
  writer.u64(value.settled_at_ms);
  encode_string(writer, value.detail, kSmall);
  encode_string(writer, value.observed_native_layout_digest, kSmall);
}

PartitionAttempt decode_attempt(ByteReader& reader) {
  PartitionAttempt attempt;
  attempt.id = decode_id<PartitionAttemptId>(reader);
  attempt.kind =
      decode_enum<AttemptKind>(reader, static_cast<std::uint8_t>(AttemptKind::Count), "attempt kind");
  attempt.reservation = decode_id<PartitionReservationId>(reader);
  attempt.accelerator = decode_id<AcceleratorId>(reader);
  attempt.accelerator_generation = decode_id<AcceleratorGeneration>(reader);
  attempt.accelerator_boot = AcceleratorBootId::from_value(reader.u64());
  attempt.capability_generation = decode_id<CapabilityGeneration>(reader);
  attempt.coordinator_epoch = decode_id<CoordinatorEpoch>(reader);
  attempt.worker = decode_id<WorkerId>(reader);
  attempt.worker_boot = WorkerBootId::from_value(reader.u64());
  attempt.partitions = reader.list<PartitionId>(256, 8, [](ByteReader& inner) {
    return decode_id<PartitionId>(inner);
  });
  attempt.desired = reader.list<PlannedPartition>(256, 4, [](ByteReader& inner) {
    return decode_planned_partition(inner);
  });
  attempt.state = decode_enum<AttemptState>(reader, 6, "attempt state");
  attempt.token = reader.u64();
  attempt.registered_at_ms = reader.u64();
  attempt.dispatched_at_ms = reader.u64();
  attempt.settled_at_ms = reader.u64();
  attempt.detail = reader.text(kSmall);
  attempt.observed_native_layout_digest = reader.text(kSmall);
  return attempt;
}

void encode(ByteWriter& writer, const WorkerRecord& value) {
  encode_id(writer, value.id);
  writer.u64(value.boot.value());
  encode_string(writer, value.endpoint, kSmall);
  encode_string(writer, value.backend, kSmall);
  writer.boolean(value.alive);
  writer.boolean(value.fenced);
  writer.u32(value.device_count);
  writer.u64(value.registered_at_ms);
  writer.u64(value.last_seen_ms);
  writer.u64(value.fenced_at_ms);
  encode_string(writer, value.fence_reason, kSmall);
  encode_id(writer, value.coordinator_epoch);
}

WorkerRecord decode_worker(ByteReader& reader) {
  WorkerRecord worker;
  worker.id = decode_id<WorkerId>(reader);
  worker.boot = WorkerBootId::from_value(reader.u64());
  worker.endpoint = reader.text(kSmall);
  worker.backend = reader.text(kSmall);
  worker.alive = reader.boolean();
  worker.fenced = reader.boolean();
  worker.device_count = reader.u32();
  worker.registered_at_ms = reader.u64();
  worker.last_seen_ms = reader.u64();
  worker.fenced_at_ms = reader.u64();
  worker.fence_reason = reader.text(kSmall);
  worker.coordinator_epoch = decode_id<CoordinatorEpoch>(reader);
  return worker;
}

void encode(ByteWriter& writer, const BackendNativePartition& value) {
  encode_string(writer, value.native_id, kSmall);
  encode_string(writer, value.instance_uuid, kSmall);
  encode_string(writer, value.vendor_native_profile, kSmall);
  encode_id(writer, value.profile);
  encode(writer, value.resources);
  encode_string(writer, value.isolation_domain, kSmall);
  encode(writer, value.isolation);
  writer.boolean(value.live);
}

BackendNativePartition decode_native_partition(ByteReader& reader) {
  BackendNativePartition partition;
  partition.native_id = reader.text(kSmall);
  partition.instance_uuid = reader.text(kSmall);
  partition.vendor_native_profile = reader.text(kSmall);
  partition.profile = decode_id<PartitionProfileId>(reader);
  partition.resources = decode(reader);
  partition.isolation_domain = reader.text(kSmall);
  partition.isolation = decode_isolation(reader);
  partition.live = reader.boolean();
  return partition;
}

void encode(ByteWriter& writer, const BackendLayout& value) {
  encode_string(writer, value.stable_key, kSmall);
  writer.boolean(value.device_present);
  writer.u64(value.boot_id.value());
  encode(writer, value.physical_totals);
  writer.u32(static_cast<std::uint32_t>(value.partitions.size()));
  for (const BackendNativePartition& partition : value.partitions) {
    encode(writer, partition);
  }
  encode(writer, value.evidence);
  encode_string(writer, value.detail, kSmall);
}

BackendLayout decode_layout(ByteReader& reader) {
  BackendLayout layout;
  layout.stable_key = reader.text(kSmall);
  layout.device_present = reader.boolean();
  layout.boot_id = AcceleratorBootId::from_value(reader.u64());
  layout.physical_totals = decode(reader);
  layout.partitions = reader.list<BackendNativePartition>(512, 4, [](ByteReader& inner) {
    return decode_native_partition(inner);
  });
  layout.evidence = decode_evidence(reader);
  layout.detail = reader.text(kSmall);
  return layout;
}

void encode(ByteWriter& writer, const BackendAccelerator& value) {
  encode_string(writer, value.stable_key, kSmall);
  encode_string(writer, value.backend, kSmall);
  encode(writer, value.identifiers);
  encode(writer, value.physical_totals);
  encode(writer, value.capability);
  writer.u32(static_cast<std::uint32_t>(value.profiles.size()));
  for (const PartitionProfile& profile : value.profiles) {
    encode(writer, profile);
  }
  writer.u64(value.boot_id.value());
  encode(writer, value.locality);
  encode(writer, value.evidence);
  encode_enum(writer, value.provenance);
  encode_string(writer, value.unsupported_reason, kSmall);
}

BackendAccelerator decode_backend_accelerator(ByteReader& reader) {
  BackendAccelerator accelerator;
  accelerator.stable_key = reader.text(kSmall);
  accelerator.backend = reader.text(kSmall);
  accelerator.identifiers = decode_identifiers(reader);
  accelerator.physical_totals = decode(reader);
  accelerator.capability = decode_capability(reader);
  accelerator.profiles = reader.list<PartitionProfile>(512, 8, [](ByteReader& inner) {
    return decode_profile(inner);
  });
  accelerator.boot_id = AcceleratorBootId::from_value(reader.u64());
  accelerator.locality = decode_locality(reader);
  accelerator.evidence = decode_evidence(reader);
  accelerator.provenance = decode_enum<EvidenceProvenance>(reader, 4, "evidence provenance");
  accelerator.unsupported_reason = reader.text(kSmall);
  return accelerator;
}

void encode(ByteWriter& writer, const NativePartitionSpec& value) {
  encode_string(writer, value.vendor_native_profile, kSmall);
  encode_id(writer, value.profile);
  encode(writer, value.resources);
  encode_string(writer, value.existing_native_id, kSmall);
  encode_id(writer, value.logical_partition);
}

NativePartitionSpec decode_native_spec(ByteReader& reader) {
  NativePartitionSpec spec;
  spec.vendor_native_profile = reader.text(kSmall);
  spec.profile = decode_id<PartitionProfileId>(reader);
  spec.resources = decode(reader);
  spec.existing_native_id = reader.text(kSmall);
  spec.logical_partition = decode_id<PartitionId>(reader);
  return spec;
}

void encode(ByteWriter& writer, const PartitionMutationRequest& value) {
  encode_string(writer, value.stable_key, kSmall);
  writer.u64(value.expected_boot.value());
  encode_id(writer, value.expected_accelerator_generation);
  encode_id(writer, value.expected_capability_generation);
  encode_id(writer, value.coordinator_epoch);
  encode_id(writer, value.worker);
  writer.u64(value.worker_boot.value());
  encode_id(writer, value.attempt);
  writer.u64(value.idempotency_token);
  writer.boolean(value.destructive);
  writer.boolean(value.full_device_reconfiguration);
  writer.u32(static_cast<std::uint32_t>(value.desired.size()));
  for (const NativePartitionSpec& spec : value.desired) {
    encode(writer, spec);
  }
  writer.u32(static_cast<std::uint32_t>(value.remove_native_ids.size()));
  for (const std::string& native_id : value.remove_native_ids) {
    encode_string(writer, native_id, kSmall);
  }
}

PartitionMutationRequest decode_mutation_request(ByteReader& reader) {
  PartitionMutationRequest request;
  request.stable_key = reader.text(kSmall);
  request.expected_boot = AcceleratorBootId::from_value(reader.u64());
  request.expected_accelerator_generation = decode_id<AcceleratorGeneration>(reader);
  request.expected_capability_generation = decode_id<CapabilityGeneration>(reader);
  request.coordinator_epoch = decode_id<CoordinatorEpoch>(reader);
  request.worker = decode_id<WorkerId>(reader);
  request.worker_boot = WorkerBootId::from_value(reader.u64());
  request.attempt = decode_id<PartitionAttemptId>(reader);
  request.idempotency_token = reader.u64();
  request.destructive = reader.boolean();
  request.full_device_reconfiguration = reader.boolean();
  request.desired = reader.list<NativePartitionSpec>(256, 4, [](ByteReader& inner) {
    return decode_native_spec(inner);
  });
  request.remove_native_ids = reader.list<std::string>(256, 4, [](ByteReader& inner) {
    return inner.text(kSmall);
  });
  return request;
}

void encode(ByteWriter& writer, const BackendMutationResult& value) {
  writer.boolean(value.accepted);
  writer.boolean(value.ambiguous);
  writer.u32(static_cast<std::uint32_t>(value.created_native_ids.size()));
  for (const std::string& native_id : value.created_native_ids) {
    encode_string(writer, native_id, kSmall);
  }
  encode_string(writer, value.detail, kSmall);
  encode(writer, value.observed_layout);
}

BackendMutationResult decode_mutation_result(ByteReader& reader) {
  BackendMutationResult result;
  result.accepted = reader.boolean();
  result.ambiguous = reader.boolean();
  result.created_native_ids = reader.list<std::string>(256, 4, [](ByteReader& inner) {
    return inner.text(kSmall);
  });
  result.detail = reader.text(kSmall);
  result.observed_layout = decode_layout(reader);
  return result;
}

}  // namespace codec
}  // namespace apf
