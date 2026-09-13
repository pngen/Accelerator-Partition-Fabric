#pragma once

#include "apf/capability.hpp"
#include "apf/evidence.hpp"
#include "apf/id.hpp"
#include "apf/limits.hpp"
#include "apf/resource.hpp"

#include <cstdint>
#include <string>

namespace apf {

/// Stable identity attributes of a physical accelerator. Fields the backend
/// cannot discover remain empty rather than being guessed.
struct DeviceIdentifiers {
  std::string vendor;
  std::string model;
  std::string uuid;
  std::string pci_bus_id;
  std::string hardware_id;
  std::string firmware_version;
  std::string driver_version;
  std::string serial;
  std::string compute_capability;
};

/// Locality attributes supplied by the deployment. The fabric does not compute
/// topology; it consumes a locality domain so that caller-provided locality
/// preferences can be honoured deterministically.
struct LocalityDomain {
  std::string name;
  std::int32_t numa_node{-1};

  bool empty() const noexcept { return name.empty() && numa_node < 0; }
};

/// The physical accelerator, represented separately from its logical
/// partitions.
struct AcceleratorRecord {
  AcceleratorId id{};
  AcceleratorGeneration generation{};
  AcceleratorBootId boot_id{};
  /// Backend family that governs this device ("synthetic", "nvidia-nvml").
  std::string backend;
  DeviceIdentifiers identifiers;
  ResourceVector physical_totals;
  PartitionCapability capability{};
  TopologyGeneration topology_generation{};
  LocalityDomain locality;
  HealthEvidence health{};
  EvidenceStamp evidence{};
  EvidenceProvenance provenance{EvidenceProvenance::Unknown};
  /// Populated when the device is known not to support partitioning.
  std::string unsupported_reason;
  std::uint64_t registered_at_ms{0};
  std::uint64_t updated_at_ms{0};

  Status validate(const Limits& limits) const;

  /// True when the device may be planned against at the given instant.
  bool is_governable_at(std::uint64_t now_ms, bool require_fresh) const noexcept;
};

}  // namespace apf
