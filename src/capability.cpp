#include "apf/capability.hpp"

#include "apf/codec.hpp"

#include <algorithm>

namespace apf {

const char* to_string(PartitionSupportState state) noexcept {
  switch (state) {
    case PartitionSupportState::Unknown: return "unknown";
    case PartitionSupportState::Supported: return "supported";
    case PartitionSupportState::Unsupported: return "unsupported";
    case PartitionSupportState::Unavailable: return "unavailable";
  }
  return "unknown";
}

PartitionSupportState partition_support_state_from_string(std::string_view name) {
  if (name == "supported") return PartitionSupportState::Supported;
  if (name == "unsupported") return PartitionSupportState::Unsupported;
  if (name == "unavailable") return PartitionSupportState::Unavailable;
  return PartitionSupportState::Unknown;
}

Status PartitionCapability::validate(const Limits& limits, bool require_accelerator) const {
  if (require_accelerator && !accelerator.valid()) {
    return failure(ErrorCode::InvalidArgument, "capability does not name an accelerator");
  }
  if (!generation.known()) {
    return failure(ErrorCode::InvalidArgument, "capability generation is not set");
  }
  if (require_accelerator && !device_generation.known()) {
    return failure(ErrorCode::InvalidArgument, "capability device generation is not set");
  }
  if (mechanism_name.size() > limits.max_metadata_bytes ||
      backend_name.size() > limits.max_metadata_bytes ||
      backend_version.size() > limits.max_metadata_bytes ||
      driver_version.size() > limits.max_metadata_bytes ||
      tooling_requirement.size() > limits.max_metadata_bytes ||
      unsupported_reason.size() > limits.max_metadata_bytes) {
    return failure(ErrorCode::LimitExceeded, "capability metadata string is too long");
  }
  if (supported_profiles.size() > limits.max_profiles) {
    return failure(ErrorCode::LimitExceeded, "capability publishes too many profiles");
  }
  if (combination_rules.size() > limits.max_profiles) {
    return failure(ErrorCode::LimitExceeded, "capability publishes too many combination rules");
  }
  for (std::size_t index = 0; index < supported_profiles.size(); ++index) {
    if (!supported_profiles[index].valid()) {
      return failure(ErrorCode::InvalidArgument, "capability publishes an invalid profile identity");
    }
    for (std::size_t other = index + 1; other < supported_profiles.size(); ++other) {
      if (supported_profiles[index] == supported_profiles[other]) {
        return failure(ErrorCode::AlreadyExists, "capability publishes a duplicate profile");
      }
    }
  }
  if (support == PartitionSupportState::Supported) {
    if (supported_profiles.empty()) {
      return failure(ErrorCode::InvalidArgument,
                     "capability claims support but publishes no profiles");
    }
    if (max_partition_count == 0) {
      return failure(ErrorCode::InvalidArgument,
                     "capability claims support but publishes a zero partition count");
    }
    if (mechanism == PartitionMechanism::None) {
      return failure(ErrorCode::InvalidArgument,
                     "capability claims support but names no partition mechanism");
    }
    if (exposed_dimensions.empty()) {
      return failure(ErrorCode::InvalidArgument,
                     "capability claims support but exposes no resource dimension");
    }
  }
  if (support == PartitionSupportState::Unsupported && unsupported_reason.empty()) {
    return failure(ErrorCode::InvalidArgument,
                   "an unsupported capability must state why the device cannot be partitioned");
  }
  if (support == PartitionSupportState::Supported && mechanism == PartitionMechanism::Synthetic &&
      evidence.provenance != EvidenceProvenance::Synthetic) {
    return failure(ErrorCode::InvalidArgument,
                   "synthetic mechanism must carry synthetic provenance");
  }
  if (support == PartitionSupportState::Supported && mechanism_is_physical(mechanism) &&
      evidence.provenance == EvidenceProvenance::Synthetic) {
    return failure(ErrorCode::InvalidArgument,
                   "a physical partition mechanism may not be claimed from synthetic evidence");
  }
  return success();
}

bool PartitionCapability::supports_profile(PartitionProfileId profile) const noexcept {
  return std::find(supported_profiles.begin(), supported_profiles.end(), profile) !=
         supported_profiles.end();
}

const ProfileCombinationRule* PartitionCapability::find_rule(
    PartitionProfileId profile) const noexcept {
  for (const ProfileCombinationRule& rule : combination_rules) {
    if (rule.profile == profile) {
      return &rule;
    }
  }
  return nullptr;
}

bool PartitionCapability::same_capability_as(const PartitionCapability& other) const noexcept {
  return capability_digest(*this) == capability_digest(other);
}

std::uint64_t capability_digest(const PartitionCapability& capability) noexcept {
  Limits limits;
  ByteWriter writer(&limits, 512);
  codec::encode(writer, capability);
  if (!writer.ok()) {
    return 0;
  }
  // FNV-1a over the canonical encoding defined in codec.cpp.
  std::uint64_t hash = 1469598103934665603ull;
  for (const std::uint8_t byte : writer.data()) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace apf
