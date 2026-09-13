#pragma once

#include "apf/accounting.hpp"
#include "apf/explain.hpp"
#include "apf/id.hpp"
#include "apf/profile.hpp"
#include "apf/resource.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apf {

/// Why a request could not be satisfied by the currently free geometry.
/// Materially different failure modes are never collapsed into one generic
/// failure.
enum class FragmentationClass : std::uint8_t {
  None = 0,
  /// Aggregate free capacity is sufficient but the requested profile shape
  /// (slice counts / alignment) cannot be assembled from what remains.
  ProfileShape = 1,
  /// Remaining slices exist but are not in a legal contiguous arrangement.
  SliceNotContiguous = 2,
  /// Engine groups cannot be split further to satisfy the request.
  EngineGroup = 3,
  /// Memory-segment constraints block the request.
  MemorySegment = 4,
  /// The device-wide maximum partition count is already reached.
  MaxPartitionCount = 5,
  /// Coexisting profiles are mutually exclusive.
  IncompatibleCombination = 6,
  /// Capacity that can never be allocated because no legal profile fits it.
  StrandedCapacity = 7,
  /// Capacity exists but is trapped behind active partitions and only becomes
  /// available after drain and/or destructive reconfiguration.
  TrappedBehindActive = 8,
  /// Free geometry is not representable without a destructive reconfiguration.
  ReconfigurationRequired = 9,
  /// The request demands more of a dimension than the physical device has at
  /// all, which is a different statement from "the free geometry cannot form
  /// it".
  ExceedsDeviceCapacity = 10,
  Count = 11,
};

const char* to_string(FragmentationClass value) noexcept;

/// Deterministic fragmentation analysis for one accelerator and one request.
struct FragmentationReport {
  AcceleratorId accelerator{};
  AcceleratorGeneration accelerator_generation{};
  CapabilityGeneration capability_generation{};
  PartitionProfileId profile{};
  std::uint32_t requested_count{1};

  FragmentationClass classification{FragmentationClass::None};
  bool aggregate_capacity_sufficient{false};
  bool profile_supported{false};
  bool feasible_now{false};
  bool feasible_after_drain{false};
  bool feasible_after_reconfiguration{false};

  std::uint32_t max_instances_now{0};
  std::uint32_t max_instances_after_drain{0};
  std::uint32_t partition_slots_free{0};
  ResourceVector free_capacity{};
  ResourceVector stranded_capacity{};
  ResourceVector capacity_trapped_behind_active{};
  ResourceVector capacity_recoverable_by_reconfiguration{};

  std::uint32_t blocking_active_partitions{0};
  bool reconfiguration_required{false};
  bool reconfiguration_can_help{false};
  bool destructive_reconfiguration_required{false};
  /// Backend-provided estimates. Zero means the backend published no estimate;
  /// it is never a fabricated number.
  std::uint64_t estimated_drain_ms{0};
  std::uint64_t estimated_reconfiguration_downtime_ms{0};
  std::uint32_t estimated_partition_mutations{0};

  std::vector<PartitionId> partitions_to_drain;
  std::vector<std::string> blockers;
  Explanation explanation{};

  std::string summary() const;
};

}  // namespace apf
