#pragma once

#include "apf/backend.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace apf {

/// Real-hardware discovery through the NVIDIA Management Library.
///
/// This backend exists to answer one question truthfully on real hardware:
/// which physical accelerator is present, and can it actually be partitioned?
/// It publishes REAL physical totals and identities, and it publishes
/// UNSUPPORTED partition support when the device genuinely has no partition
/// mechanism. It never fabricates MIG capability, and it exposes no path that
/// creates or destroys partitions: physical mutation through this backend is
/// positively UNSUPPORTED on devices without a partition mechanism.
class NvmlBackend final : public AcceleratorBackend {
 public:
  /// Loads the NVML shared library dynamically.
  /// Returns UnsupportedCapability when the library cannot be loaded, which is
  /// a truthful "no real accelerator stack is reachable" answer.
  static Result<std::unique_ptr<NvmlBackend>> create();

  ~NvmlBackend() override;

  std::string name() const override { return "nvidia-nvml"; }

  /// True when the NVML library was loaded successfully.
  bool loaded() const noexcept;

  /// Version string reported by the loaded library.
  std::string library_version() const;

  /// Diagnostics for the inspection tool: one line per probed NVML entry point.
  std::string probe_report() const;

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

 private:
  NvmlBackend();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace apf
