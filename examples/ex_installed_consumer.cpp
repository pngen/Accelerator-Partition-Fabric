// Independent consumer of the installed CMake package.
//
// This file is built by the repository for convenience and, verbatim, by the
// downstream consumer project under examples/downstream-consumer that links
// only against the installed package.

#include <apf/backend_synthetic.hpp>
#include <apf/fabric.hpp>
#include <apf/version.hpp>

#include <cstdio>

int main() {
  std::printf("Accelerator Partition Fabric %s\n", apf::version_string());
  apf::FabricOptions options;
  options.instance_id = "installed-consumer";
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
  apf::PartitionProfileId profile{};
  for (const apf::PartitionProfile& candidate : fabric.snapshot()->profiles) {
    if (candidate.name == "synthetic-2g") {
      profile = candidate.id;
    }
  }
  apf::PartitionRequest request;
  request.profile = profile;
  request.count = 1;
  const apf::Result<apf::PartitionPlan> plan = fabric.plan(request);
  if (!plan.ok()) {
    std::fprintf(stderr, "planning failed: %s\n", apf::to_string(plan.error()).c_str());
    return 1;
  }
  std::printf("plan outcome: %s\n", apf::to_string(plan.value().outcome));
  return plan.value().feasible() ? 0 : 1;
}

