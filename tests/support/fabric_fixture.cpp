#include "support/fabric_fixture.hpp"

#include <algorithm>

namespace apftest {

apf::PartitionProfileId Fixture::profile(const std::string& name) const {
  const auto it = profiles.find(name);
  if (it == profiles.end()) {
    return apf::PartitionProfileId{};
  }
  return it->second;
}

apf::Result<apf::PartitionPlan> Fixture::plan(const std::string& profile_name, std::uint32_t count,
                                              bool allow_destructive, bool allow_drain) {
  apf::PartitionRequest request;
  request.profile = profile(profile_name);
  request.count = count;
  request.allow_destructive_reconfiguration = allow_destructive;
  request.allow_drain = allow_drain;
  request.requester = "test";
  return fabric->plan(request);
}

apf::Result<apf::MutationOutcome> Fixture::create(const std::string& profile_name,
                                                  std::uint32_t count) {
  apf::Result<apf::PartitionPlan> planned = plan(profile_name, count);
  if (!planned.ok()) {
    return planned.error();
  }
  if (!planned.value().feasible()) {
    return apf::make_error(apf::ErrorCode::PolicyRejected,
                           std::string("plan is not feasible: ") +
                               apf::to_string(planned.value().outcome));
  }
  apf::Result<apf::PartitionReservation> reservation = fabric->reserve(planned.value().id);
  if (!reservation.ok()) {
    return reservation.error();
  }
  return fabric->execute(reservation.value().id);
}

apf::Status Fixture::verify_accounting() const { return fabric->verify_accounting(); }

std::vector<apf::PartitionId> Fixture::partitions_of(const apf::AcceleratorId& id) const {
  return fabric->snapshot()->partitions_of(id);
}

std::size_t Fixture::count_state(apf::PartitionState state) const {
  std::size_t total = 0;
  for (const apf::PartitionRecord& record : fabric->snapshot()->partitions) {
    if (record.state == state) {
      ++total;
    }
  }
  return total;
}

std::uint64_t Fixture::free_memory() const {
  const apf::Result<apf::CapacityLedger> ledger = fabric->ledger(accelerator);
  if (!ledger.ok()) {
    return 0;
  }
  return ledger.value().bucket(apf::CapacityBucket::Free)
      .get(apf::ResourceDimension::MemoryBytes);
}

std::unique_ptr<Fixture> make_fixture(const FixtureOptions& options) {
  auto fixture = std::make_unique<Fixture>();
  auto manual = std::make_shared<apf::ManualClock>(1'000'000);
  fixture->manual_clock = manual.get();
  fixture->clock = manual;

  apf::SyntheticDeviceSpec spec =
      apf::make_default_synthetic_device(options.key, options.memory_gib, true);
  spec.evidence_ttl_ms = options.evidence_ttl_ms;
  if (options.customize) {
    options.customize(spec);
  }
  auto backend = std::make_shared<apf::SyntheticBackend>(fixture->clock);
  const apf::Status added = backend->add_device(spec);
  if (!added.ok()) {
    return nullptr;
  }
  for (const apf::SyntheticDeviceSpec& extra : options.extra_devices) {
    const apf::Status extra_added = backend->add_device(extra);
    if (!extra_added.ok()) {
      return nullptr;
    }
  }
  fixture->backend = backend;
  fixture->key = options.key;

  apf::FabricOptions fabric_options;
  fabric_options.clock = fixture->clock;
  fabric_options.instance_id = "apf-test";
  fabric_options.local_backend = backend;
  fixture->fabric = std::make_unique<apf::PartitionFabric>(fabric_options);
  apf::PolicyGeneration generation = apf::PolicyGeneration::first();
  apf::PlanningPolicy policy = apf::make_default_policy(generation);
  policy.allow_destructive_reconfiguration = options.allow_destructive_reconfiguration;
  policy.allow_drain = options.allow_drain;
  policy.max_evidence_age_ms = options.max_evidence_age_ms;
  policy.allow_external_adoption = options.allow_external_adoption;
  const apf::Status applied = fixture->fabric->set_policy(policy);
  if (!applied.ok()) {
    return nullptr;
  }
  std::vector<apf::AcceleratorId> discovered;
  const apf::Status discovery = fixture->fabric->discover(*backend, &discovered);
  if (!discovery.ok() || discovered.empty()) {
    return nullptr;
  }
  fixture->accelerator = discovered.front();
  // Profile identities are device-specific: only the profiles the primary
  // accelerator actually publishes may be used to address it.
  const std::shared_ptr<const apf::FabricSnapshot> snapshot = fixture->fabric->snapshot();
  const apf::AcceleratorView* view = snapshot->find_accelerator(fixture->accelerator);
  if (view == nullptr) {
    return nullptr;
  }
  for (const apf::PartitionProfileId id : view->accelerator.capability.supported_profiles) {
    const apf::PartitionProfile* profile = snapshot->find_profile(id);
    if (profile != nullptr) {
      fixture->profiles[profile->name] = profile->id;
    }
  }
  return fixture;
}

}  // namespace apftest
