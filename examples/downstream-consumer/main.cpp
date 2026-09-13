// Independent downstream consumer.
//
// Built from a clean install prefix with nothing but
// find_package(AcceleratorPartitionFabric CONFIG REQUIRED) and the exported
// namespaced target. It exercises the public API surface a real embedder uses:
// evidence registration, capability publication, planning, fragmentation
// analysis, reservation, verified mutation, admission and durable state.

#include <apf/backend_synthetic.hpp>
#include <apf/fabric.hpp>
#include <apf/persistence.hpp>
#include <apf/process.hpp>
#include <apf/version.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

apf::PartitionProfileId profile_named(const apf::FabricSnapshot& snapshot,
                                      const std::string& name) {
  for (const apf::PartitionProfile& profile : snapshot.profiles) {
    if (profile.name == name) {
      return profile.id;
    }
  }
  return apf::PartitionProfileId{};
}

}  // namespace

int main() {
  std::printf("consumer built against %s\n", apf::version_banner().c_str());

  apf::FabricOptions options;
  options.instance_id = "downstream-consumer";
  auto backend = std::make_shared<apf::SyntheticBackend>(apf::make_system_clock());
  if (!backend->add_device(apf::make_default_synthetic_device("consumer-0", 32, true)).ok()) {
    std::fprintf(stderr, "device registration failed\n");
    return 1;
  }
  options.local_backend = backend;
  apf::PartitionFabric fabric(options);

  std::vector<apf::AcceleratorId> discovered;
  if (!fabric.discover(*backend, &discovered).ok() || discovered.empty()) {
    std::fprintf(stderr, "discovery failed\n");
    return 1;
  }
  const apf::AcceleratorId accelerator = discovered.front();

  apf::PartitionRequest request;
  request.profile = profile_named(*fabric.snapshot(), "synthetic-2g");
  request.count = 1;
  request.requester = "downstream-consumer";
  const apf::Result<apf::PartitionPlan> plan = fabric.plan(request);
  if (!plan.ok() || !plan.value().feasible()) {
    std::fprintf(stderr, "planning failed\n");
    return 1;
  }
  const apf::Result<apf::FragmentationReport> fragmentation =
      fabric.analyze_fragmentation(accelerator, request);
  if (!fragmentation.ok()) {
    std::fprintf(stderr, "fragmentation analysis failed\n");
    return 1;
  }
  const apf::Result<apf::PartitionReservation> reservation = fabric.reserve(plan.value().id);
  if (!reservation.ok()) {
    std::fprintf(stderr, "reservation failed\n");
    return 1;
  }
  const apf::Result<apf::MutationOutcome> outcome = fabric.execute(reservation.value().id);
  if (!outcome.ok() || !outcome.value().committed) {
    std::fprintf(stderr, "mutation was not verified and committed\n");
    return 1;
  }
  apf::WorkloadRequirement requirement;
  requirement.workload_id = "consumer-workload";
  requirement.required_profile = profile_named(*fabric.snapshot(), "synthetic-2g");
  const apf::Result<apf::AdmissionDecision> admitted = fabric.admit(requirement);
  if (!admitted.ok() || !admitted.value().admitted()) {
    std::fprintf(stderr, "admission failed\n");
    return 1;
  }
  const apf::Status accounting = fabric.verify_accounting();
  if (!accounting.ok()) {
    std::fprintf(stderr, "accounting violated: %s\n", apf::to_string(accounting.error()).c_str());
    return 1;
  }
  const std::string directory = apf::temp_directory();
  apf::PersistenceStore store;
  const std::string state_path = apf::join_path(directory, "apf-consumer.state");
  if (!store.set_path(state_path).ok()) {
    return 1;
  }
  if (!fabric.save(store).ok()) {
    std::fprintf(stderr, "durable save failed\n");
    return 1;
  }
  apf::PartitionFabric reloaded(options);
  if (!reloaded.load(store).ok()) {
    std::fprintf(stderr, "durable load failed\n");
    return 1;
  }
  std::printf("plan=%s fragmentation=%s partitions=%zu reloaded=%zu\n",
              apf::to_string(plan.value().outcome),
              apf::to_string(fragmentation.value().classification),
              fabric.snapshot()->partitions.size(), reloaded.snapshot()->partitions.size());
  (void)apf::remove_file(state_path);
  std::printf("downstream consumer completed successfully\n");
  return 0;
}
