#pragma once

#include "apf/evidence.hpp"
#include "apf/id.hpp"
#include "apf/isolation.hpp"
#include "apf/limits.hpp"
#include "apf/profile.hpp"
#include "apf/resource.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apf {

/// Positive capability determination. UNKNOWN never becomes SUPPORTED: absence
/// of evidence is not capability.
enum class PartitionSupportState : std::uint8_t {
  Unknown = 0,
  Supported = 1,
  /// The backend positively determined that the device cannot be partitioned.
  Unsupported = 2,
  /// Capability could not be queried at all (device gone, driver missing).
  Unavailable = 3,
};

const char* to_string(PartitionSupportState state) noexcept;
PartitionSupportState partition_support_state_from_string(std::string_view name);

/// Generation-bound publication of what the device can do right now. A change
/// to any field forces revalidation of plans that depended on it.
struct PartitionCapability {
  AcceleratorId accelerator{};
  AcceleratorGeneration device_generation{};
  CapabilityGeneration generation{};
  TopologyGeneration topology_generation{};
  PartitionSupportState support{PartitionSupportState::Unknown};
  PartitionMechanism mechanism{PartitionMechanism::None};
  std::string mechanism_name;
  std::vector<PartitionProfileId> supported_profiles;
  std::vector<ProfileCombinationRule> combination_rules;
  /// 0 means "not published". A device with no partitions publishes at least 1.
  std::uint32_t max_partition_count{0};
  bool requires_reset_for_reconfiguration{false};
  bool live_reconfiguration_supported{false};
  IsolationSet isolation{};
  std::vector<ResourceDimension> exposed_dimensions;
  ResourceVector minimum_allocation;
  std::uint32_t slice_granularity{0};
  /// Device-level slice geometry, published only by backends whose partition
  /// mechanism actually exposes it. Zero means "not published", and the generic
  /// layer then refuses to reason about slice shape rather than guessing.
  std::uint32_t total_compute_slices{0};
  std::uint32_t total_memory_slices{0};
  /// Disruption estimates published by the backend. Zero means the backend
  /// published no estimate; it is never replaced by a fabricated number.
  std::uint64_t drain_estimate_ms{0};
  std::uint64_t reconfiguration_estimate_ms{0};
  std::string backend_name;
  std::string backend_version;
  std::string driver_version;
  std::string tooling_requirement;
  EvidenceStamp evidence{};
  std::string unsupported_reason;

  /// Validates the capability. A backend publishes capability before the runtime
  /// has assigned an accelerator identity, so the identity check is optional and
  /// is performed again once the identity exists.
  Status validate(const Limits& limits, bool require_accelerator = true) const;

  bool declares_supported() const noexcept {
    return support == PartitionSupportState::Supported;
  }
  bool supports_profile(PartitionProfileId profile) const noexcept;
  const ProfileCombinationRule* find_rule(PartitionProfileId profile) const noexcept;

  /// Structural equality over the capability-bearing fields. Used to decide
  /// whether a republication actually changed anything.
  bool same_capability_as(const PartitionCapability& other) const noexcept;
};

/// Digest over externally visible capability content.
std::uint64_t capability_digest(const PartitionCapability& capability) noexcept;

}  // namespace apf
