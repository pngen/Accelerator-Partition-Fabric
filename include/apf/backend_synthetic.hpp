#pragma once

#include "apf/backend.hpp"
#include "apf/time.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace apf {

/// Deterministic accelerator model used to prove partition semantics that the
/// local hardware cannot: multi-partition geometry, legal and illegal profile
/// combinations, fragmentation, destructive reconfiguration, device reset,
/// capability change, external mutation, ambiguous completion and fault
/// injection.
///
/// The synthetic backend obeys exactly the same public contracts as a real
/// backend: it is registered, discovered, planned against, mutated, verified
/// and reconciled through the same interfaces. Tests never reach around the
/// production API.
struct SyntheticProfileSpec {
  std::string name;
  std::string vendor_native;
  ResourceVector resources;
  std::uint32_t compute_slices{1};
  std::uint32_t memory_slices{1};
  EngineGrouping engine_grouping{EngineGrouping::Dedicated};
  std::uint32_t max_multiplicity{0};
  bool requires_full_device_reconfiguration{false};
  bool live_repartitioning_supported{true};
  std::vector<std::string> incompatible_with;
  bool requires_contiguous_slices{false};
  bool requires_atomic_slice{false};
};

/// Static description of one synthetic accelerator.
struct SyntheticDeviceSpec {
  std::string key;
  std::string vendor{"synthetic"};
  std::string model{"APF Synthetic Accelerator"};
  std::string backend{"synthetic"};
  std::string uuid;
  std::string driver_version{"synthetic-driver-1.0"};
  std::string firmware_version{"synthetic-fw-1.0"};
  std::string hardware_id;
  std::string locality_domain{"locality-0"};
  std::int32_t numa_node{-1};
  ResourceVector physical_totals;
  std::vector<SyntheticProfileSpec> profiles;
  std::uint32_t max_partition_count{8};
  PartitionMechanism mechanism{PartitionMechanism::Synthetic};
  std::string mechanism_name{"synthetic-slice"};
  bool partitioning_supported{true};
  std::string unsupported_reason;
  bool requires_reset_for_reconfiguration{true};
  bool live_reconfiguration_supported{false};
  IsolationSet isolation{};
  ResourceVector minimum_allocation;
  std::uint32_t slice_granularity{1};
  EvidenceProvenance provenance{EvidenceProvenance::Synthetic};
  HealthState health{HealthState::Healthy};
  std::uint64_t evidence_ttl_ms{30'000};
  /// Backend-published disruption estimate for a destructive layout change.
  std::uint64_t reconfiguration_downtime_ms{2'500};
  std::uint64_t drain_estimate_ms{500};
  /// Slice geometry: the device is modelled as a row of compute slices and a
  /// row of memory slices, which is what makes profile-shape fragmentation real.
  std::uint32_t total_compute_slices{7};
  std::uint32_t total_memory_slices{8};
  /// Monotonic incarnation counter; a reset advances the device boot identity.
  std::uint64_t boot_entropy{0x1000};
  bool device_present{true};
};

/// Fault injection hooks. Every hook models a failure mode a real vendor stack
/// can exhibit.
struct SyntheticFaults {
  bool fail_discovery{false};
  bool fail_capability_query{false};
  /// The backend positively refuses the mutation before applying anything.
  bool fail_create{false};
  bool fail_destroy{false};
  bool fail_reconfigure{false};
  /// The call does not complete, so whether anything was applied is unknown.
  bool error_create{false};
  bool error_destroy{false};
  bool error_reconfigure{false};
  /// The mutation is applied physically but the acknowledgement is lost.
  bool ambiguous_create{false};
  bool ambiguous_destroy{false};
  bool ambiguous_reconfigure{false};
  /// The backend reports a result that does not match what it actually did.
  bool mismatch_create_result{false};
  /// Partitions vanish right after a successful creation.
  bool partition_disappears_after_create{false};
  bool device_disappears{false};
  /// The device resets, advancing the boot identity, without the runtime being
  /// told; the next query discovers it.
  bool silent_device_reset{false};
  /// Externally applied layout change performed behind the runtime's back.
  bool external_mutation_pending{false};
  /// Malformed metadata returned from the backend.
  bool malformed_metadata{false};
  /// Duplicate native ids returned by the backend.
  bool duplicate_native_ids{false};
};

/// Convenience device model used by tests, examples and the worker tool: a
/// documented profile ladder over a fixed slice geometry, which is what makes
/// profile-shape fragmentation, slice exhaustion and destructive
/// reconfiguration real rather than simulated-by-agreement.
SyntheticDeviceSpec make_default_synthetic_device(std::string key,
                                                  std::uint64_t memory_gib = 32,
                                                  bool full_profile_conflicts = true);

/// Deterministic synthetic backend. All state transitions are exact integer
/// operations; time comes from an injected clock.
class SyntheticBackend final : public AcceleratorBackend {
 public:
  explicit SyntheticBackend(ClockPtr clock = nullptr);
  ~SyntheticBackend() override;

  std::string name() const override { return "synthetic"; }

  /// Registers or replaces a device model. Takes effect on the next discovery.
  Status add_device(const SyntheticDeviceSpec& spec);
  /// Removes a device model. Existing partitions of that device disappear.
  Status remove_device(std::string_view key);

  /// Fault injection control.
  void set_faults(const SyntheticFaults& faults);
  SyntheticFaults faults() const;
  void set_fault(std::string_view name, bool enabled);
  /// Applies a capability change (new profile set / max partition count) and
  /// advances the capability generation.
  Status mutate_capability(std::string_view key, std::vector<SyntheticProfileSpec> profiles,
                           std::uint32_t max_partition_count, bool partitioning_supported,
                           std::string unsupported_reason);
  /// Performs an unannounced device reset: boot identity advances and every
  /// physical partition disappears.
  Status reset_device(std::string_view key);
  /// Simulates an external actor changing the physical layout directly.
  Status external_create(std::string_view key, std::string_view vendor_native_profile,
                         std::string* out_native_id);
  Status external_destroy(std::string_view key, std::string_view native_id);
  /// Marks the device absent from discovery.
  Status set_device_present(std::string_view key, bool present);

  Result<std::vector<BackendAccelerator>> discover() override;
  Result<BackendLayout> query_layout(std::string_view stable_key) override;
  Result<BackendMutationResult> create_partitions(
      const PartitionMutationRequest& request) override;
  Result<BackendMutationResult> destroy_partitions(
      const PartitionMutationRequest& request) override;
  Result<BackendMutationResult> reconfigure_layout(
      const PartitionMutationRequest& request) override;
  Result<bool> validate_partition(std::string_view stable_key,
                                  std::string_view native_id) override;
  std::uint64_t estimate_reconfiguration_downtime_ms(
      std::string_view stable_key, const std::vector<NativePartitionSpec>& desired) override;

  /// Serialises the full synthetic device state so that a replacement worker
  /// process observes the same physical reality (used by the multiprocess
  /// proofs). The format is versioned and integrity checked.
  Result<std::string> export_state() const;
  Status import_state(std::string_view bytes);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace apf
