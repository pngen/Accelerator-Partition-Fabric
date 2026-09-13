// Basic partition planning against a synthetic accelerator.
//
// Shows the smallest complete decision path: register evidence, plan, and read
// back the deterministic explanation for the outcome.

#include "apf/backend_synthetic.hpp"
#include "apf/fabric.hpp"

#include <cstdio>

namespace {

int fail(const apf::Error& error) {
  std::fprintf(stderr, "error: %s\n", apf::to_string(error).c_str());
  return 1;
}

}  // namespace

int main() {
  apf::FabricOptions options;
  options.instance_id = "example-plan";
  auto backend = std::make_shared<apf::SyntheticBackend>(apf::make_system_clock());
  if (!backend->add_device(apf::make_default_synthetic_device("sim-0", 32, true)).ok()) {
    return 1;
  }
  options.local_backend = backend;
  apf::PartitionFabric fabric(options);
  std::vector<apf::AcceleratorId> discovered;
  if (!fabric.discover(*backend, &discovered).ok() || discovered.empty()) {
    std::fprintf(stderr, "discovery failed\n");
    return 1;
  }
  const std::shared_ptr<const apf::FabricSnapshot> snapshot = fabric.snapshot();
  const apf::PartitionProfile* profile = nullptr;
  for (const apf::PartitionProfile& candidate : snapshot->profiles) {
    if (candidate.name == "synthetic-2g") {
      profile = &candidate;
    }
  }
  if (profile == nullptr) {
    std::fprintf(stderr, "profile not published\n");
    return 1;
  }
  apf::PartitionRequest request;
  request.profile = profile->id;
  request.count = 2;
  request.requester = "example-plan";
  const apf::Result<apf::PartitionPlan> plan = fabric.plan(request);
  if (!plan.ok()) {
    return fail(plan.error());
  }
  std::printf("outcome: %s\n", apf::to_string(plan.value().outcome));
  std::printf("chosen accelerator: %s\n", plan.value().binding.accelerator.str().c_str());
  for (const apf::PlanStep& step : plan.value().steps) {
    std::printf("  step %s\n", step.describe().c_str());
  }
  std::printf("%s", plan.value().explanation.format().c_str());
  return plan.value().feasible() ? 0 : 1;
}
