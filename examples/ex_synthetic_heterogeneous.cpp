// Heterogeneous profiles on one device, planned deterministically.
//
// Shows that the planner distinguishes outcomes that look similar to a human:
// feasible now, feasible only after a destructive reconfiguration, physically
// impossible, and blocked by the device's partition count.

#include "apf/backend_synthetic.hpp"
#include "apf/fabric.hpp"

#include <cstdio>

int main() {
  apf::FabricOptions options;
  options.instance_id = "example-heterogeneous";
  auto backend = std::make_shared<apf::SyntheticBackend>(apf::make_system_clock());
  if (!backend->add_device(apf::make_default_synthetic_device("sim-0", 32, true)).ok()) {
    return 1;
  }
  options.local_backend = backend;
  apf::PartitionFabric fabric(options);
  apf::PlanningPolicy policy = apf::make_default_policy(apf::PolicyGeneration::first());
  policy.allow_destructive_reconfiguration = true;
  if (!fabric.set_policy(policy).ok()) {
    return 1;
  }
  std::vector<apf::AcceleratorId> discovered;
  if (!fabric.discover(*backend, &discovered).ok() || discovered.empty()) {
    return 1;
  }
  const std::shared_ptr<const apf::FabricSnapshot> snapshot = fabric.snapshot();
  const std::vector<std::pair<std::string, std::uint32_t>> requests = {
      {"synthetic-1g", 7}, {"synthetic-1g", 8}, {"synthetic-3g", 2}, {"synthetic-3g", 3},
      {"synthetic-7g", 1}, {"synthetic-7g", 2}};
  for (const auto& entry : requests) {
    apf::PartitionProfileId profile{};
    for (const apf::PartitionProfile& candidate : snapshot->profiles) {
      if (candidate.name == entry.first) {
        profile = candidate.id;
      }
    }
    apf::PartitionRequest request;
    request.profile = profile;
    request.count = entry.second;
    request.allow_destructive_reconfiguration = true;
    const apf::Result<apf::PartitionPlan> plan = fabric.plan(request);
    if (!plan.ok()) {
      std::fprintf(stderr, "error: %s\n", apf::to_string(plan.error()).c_str());
      return 1;
    }
    std::printf("%-14s x%u -> %s\n", entry.first.c_str(), entry.second,
                apf::to_string(plan.value().outcome));
  }
  return 0;
}
