#pragma once

#include "apf/accelerator.hpp"
#include "apf/partition.hpp"
#include "apf/profile.hpp"
#include "apf/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace apf {

/// What discovery reports about one physical accelerator.
struct BackendAccelerator {
  /// Stable key used by every subsequent backend call. For real hardware this
  /// is the device UUID; for the synthetic backend it is the configured key.
  std::string stable_key;
  std::string backend;
  DeviceIdentifiers identifiers;
  ResourceVector physical_totals;
  PartitionCapability capability{};
  /// Profile definitions published by the backend. Capability references these
  /// by identity, so a discovered device is self-describing without the caller
  /// having to reconstruct vendor profile geometry.
  std::vector<PartitionProfile> profiles{};
  AcceleratorBootId boot_id{};
  LocalityDomain locality;
  EvidenceStamp evidence{};
  EvidenceProvenance provenance{EvidenceProvenance::Unknown};
  std::string unsupported_reason;
};

/// A partition as the hardware actually reports it.
struct BackendNativePartition {
  std::string native_id;
  std::string instance_uuid;
  std::string vendor_native_profile;
  PartitionProfileId profile{};
  ResourceVector resources;
  std::string isolation_domain;
  IsolationSet isolation{};
  bool live{true};
};

/// The observed physical layout of one accelerator.
struct BackendLayout {
  std::string stable_key;
  bool device_present{false};
  AcceleratorBootId boot_id{};
  ResourceVector physical_totals;
  std::vector<BackendNativePartition> partitions;
  EvidenceStamp evidence{};
  std::string detail;
};

/// One partition the caller wants to exist after a mutation.
struct NativePartitionSpec {
  std::string vendor_native_profile;
  PartitionProfileId profile{};
  ResourceVector resources;
  /// Native identity to destroy/replace, when the operation targets an
  /// existing partition.
  std::string existing_native_id;
  PartitionId logical_partition{};
};

/// A physical mutation request. Every request carries the exact authority the
/// caller believes it holds so that a backend can refuse to act on stale
/// assumptions.
struct PartitionMutationRequest {
  std::string stable_key;
  AcceleratorBootId expected_boot{};
  AcceleratorGeneration expected_accelerator_generation{};
  CapabilityGeneration expected_capability_generation{};
  CoordinatorEpoch coordinator_epoch{};
  WorkerId worker{};
  WorkerBootId worker_boot{};
  PartitionAttemptId attempt{};
  /// Exactly-once token. A backend that already applied a mutation with this
  /// token must report the previous result rather than applying it twice.
  std::uint64_t idempotency_token{0};
  bool destructive{false};
  /// True when the caller requires the whole layout to be rebuilt.
  bool full_device_reconfiguration{false};
  std::vector<NativePartitionSpec> desired;
  std::vector<std::string> remove_native_ids;
};

struct BackendMutationResult {
  bool accepted{false};
  /// True when the backend applied the change but cannot prove it, or when the
  /// caller lost the acknowledgement. Callers must reconcile, never replay
  /// blindly.
  bool ambiguous{false};
  /// Native ids created by this mutation, in the order of the request.
  std::vector<std::string> created_native_ids;
  std::string detail;
  /// Fresh layout observed after the mutation, when the backend could read it.
  BackendLayout observed_layout;
};

/// The narrow backend boundary. Vendor operations are reachable only through
/// these calls; the public API never exposes arbitrary vendor command
/// execution.
///
/// Outcome contract for every mutating call, which the runtime relies on to
/// decide whether it may replay anything:
///
///   * a returned BackendMutationResult with accepted == false means the
///     backend positively determined that nothing was applied;
///   * accepted == true with ambiguous == false means the backend applied the
///     change and observed the result;
///   * an Error return means the call did not complete and *whether anything
///     was applied is unknown*.
///
/// The runtime never replays a mutation whose outcome is unknown; it reconciles
/// against freshly observed physical state instead.
class AcceleratorBackend {
 public:
  AcceleratorBackend() = default;
  AcceleratorBackend(const AcceleratorBackend&) = delete;
  AcceleratorBackend& operator=(const AcceleratorBackend&) = delete;
  virtual ~AcceleratorBackend();

  /// Backend family name, stable and machine readable.
  virtual std::string name() const = 0;

  /// Enumerates the physical accelerators this backend governs.
  virtual Result<std::vector<BackendAccelerator>> discover() = 0;

  /// Reads the current physical layout of one accelerator.
  virtual Result<BackendLayout> query_layout(std::string_view stable_key) = 0;

  /// Creates partitions. Must be idempotent with respect to idempotency_token.
  virtual Result<BackendMutationResult> create_partitions(
      const PartitionMutationRequest& request) = 0;

  /// Destroys partitions.
  virtual Result<BackendMutationResult> destroy_partitions(
      const PartitionMutationRequest& request) = 0;

  /// Performs a device-wide layout change.
  virtual Result<BackendMutationResult> reconfigure_layout(
      const PartitionMutationRequest& request) = 0;

  /// Verifies that a specific native partition still exists.
  virtual Result<bool> validate_partition(std::string_view stable_key,
                                          std::string_view native_id) = 0;

  /// Estimated disruptive cost of a layout change, when the backend can
  /// publish one. Zero means "not published".
  virtual std::uint64_t estimate_reconfiguration_downtime_ms(
      std::string_view stable_key, const std::vector<NativePartitionSpec>& desired) {
    (void)stable_key;
    (void)desired;
    return 0;
  }
};

/// Validates a discovery result before it is allowed to mutate runtime state:
/// missing identities, impossible totals, out-of-range shares and malformed
/// metadata are rejected here rather than downstream.
Status validate_backend_accelerator(const BackendAccelerator& accelerator, const Limits& limits);

/// Validates an observed layout the same way.
Status validate_backend_layout(const BackendLayout& layout, const Limits& limits);

}  // namespace apf
