#pragma once

#include "apf/id.hpp"
#include "apf/limits.hpp"
#include "apf/resource.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apf {

/// The physical partitioning mechanism. These are deliberately not treated as
/// equivalent: MIG, SR-IOV-like accelerator partitioning, mediated devices and
/// vendor slices have different authority, isolation and reconfiguration
/// semantics. The generic layer governs authority and lifecycle; the backend
/// adapter translates the actual mechanism.
enum class PartitionMechanism : std::uint8_t {
  None = 0,
  Mig = 1,
  SrIov = 2,
  MediatedDevice = 3,
  VendorSlice = 4,
  /// Deterministic simulator mechanism used by the synthetic backend.
  Synthetic = 5,
  Count = 6,
};

const char* to_string(PartitionMechanism mechanism) noexcept;
Result<PartitionMechanism> partition_mechanism_from_string(std::string_view name);
bool mechanism_is_physical(PartitionMechanism mechanism) noexcept;

/// How execution engines are grouped for a profile.
enum class EngineGrouping : std::uint8_t {
  None = 0,
  Shared = 1,
  Dedicated = 2,
  Count = 3,
};

const char* to_string(EngineGrouping grouping) noexcept;
Result<EngineGrouping> engine_grouping_from_string(std::string_view name);

/// Geometry alignment constraints. Slice-based mechanisms constrain which
/// combinations of profiles can coexist even when aggregate capacity suffices.
struct ProfileAlignment {
  std::uint32_t compute_slice_multiple{1};
  std::uint32_t memory_slice_multiple{1};
  bool requires_contiguous_slices{false};
  /// The slice cannot be subdivided any further; this is what makes small
  /// stranded remainders permanent.
  bool requires_atomic_slice{false};
};

/// An explicit partition shape.
struct PartitionProfile {
  PartitionProfileId id{};
  PartitionProfileGeneration generation{};
  std::string name;
  /// Opaque vendor-native identity (for example a MIG profile string). It is
  /// preserved as backend metadata and never interpreted by generic logic.
  std::string vendor_native;
  /// Backend family that can realise this profile.
  std::string backend;
  PartitionMechanism mechanism{PartitionMechanism::None};
  ResourceVector resources;
  std::uint32_t compute_slice_count{0};
  std::uint32_t memory_slice_count{0};
  EngineGrouping engine_grouping{EngineGrouping::None};
  /// 0 means the backend publishes no profile-level multiplicity cap.
  std::uint32_t max_multiplicity{0};
  ProfileAlignment alignment{};
  std::vector<PartitionProfileId> mutually_exclusive_with;
  bool requires_full_device_reconfiguration{false};
  bool live_repartitioning_supported{false};
  std::vector<std::string> backend_requirements;
  std::string description;

  Status validate(const Limits& limits) const;

  /// True when both profiles may not be resident on one device simultaneously.
  bool conflicts_with(const PartitionProfile& other) const noexcept;
  /// True when this profile is at least as large as the other in every shared
  /// dimension, i.e. a request satisfied by *other* could also be satisfied by
  /// *this*.
  bool dominates(const PartitionProfile& other) const noexcept;
};

/// A constraint on how many instances of a profile may coexist and what else
/// may be resident at the same time.
struct ProfileCombinationRule {
  PartitionProfileId profile{};
  /// 0 means the device-wide max_partition_count governs.
  std::uint32_t max_instances{0};
  std::vector<PartitionProfileId> incompatible_with;
  bool requires_exclusive_device{false};
};

}  // namespace apf
