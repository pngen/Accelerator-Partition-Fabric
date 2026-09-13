#pragma once

#include "apf/backend_synthetic.hpp"
#include "apf/fabric.hpp"
#include "apf/time.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace apftest {

/// Shared test fixture: one synthetic accelerator governed by one runtime
/// instance, with a deterministic manual clock. Every test drives the same
/// public API a real deployment would use.
struct Fixture {
  apf::ClockPtr clock;
  std::shared_ptr<apf::SyntheticBackend> backend;
  std::unique_ptr<apf::PartitionFabric> fabric;
  std::map<std::string, apf::PartitionProfileId> profiles;
  apf::AcceleratorId accelerator{};
  std::string key{"synthetic-0"};
  apf::ManualClock* manual_clock{nullptr};

  apf::PartitionProfileId profile(const std::string& name) const;
  apf::Result<apf::PartitionPlan> plan(const std::string& profile_name, std::uint32_t count,
                                       bool allow_destructive = false, bool allow_drain = false);
  /// Full authoritative path: plan, reserve, execute, verify.
  apf::Result<apf::MutationOutcome> create(const std::string& profile_name,
                                           std::uint32_t count = 1);
  apf::Status verify_accounting() const;
  std::vector<apf::PartitionId> partitions_of(const apf::AcceleratorId& accelerator) const;
  std::size_t count_state(apf::PartitionState state) const;
  std::uint64_t free_memory() const;
};

struct FixtureOptions {
  std::string key{"synthetic-0"};
  std::uint64_t memory_gib{32};
  bool allow_destructive_reconfiguration{false};
  bool allow_drain{true};
  std::uint64_t evidence_ttl_ms{30'000};
  std::uint64_t max_evidence_age_ms{30'000};
  bool allow_external_adoption{false};
  /// Optional customisation hook applied to the default device model before it
  /// is registered, used by tests that need a specific slice geometry.
  std::function<void(apf::SyntheticDeviceSpec&)> customize;
  /// Additional devices registered on the same backend.
  std::vector<apf::SyntheticDeviceSpec> extra_devices;
};

std::unique_ptr<Fixture> make_fixture(const FixtureOptions& options = {});

}  // namespace apftest
