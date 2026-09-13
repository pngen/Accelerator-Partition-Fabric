// Fragmentation: aggregate capacity is not the same as assemblable capacity.
//
// The simulated device exposes compute slices and memory slices whose geometry
// does not line up, so a request can fail as FRAGMENTED even though the free
// capacity numbers look sufficient.

#include "apf/backend_synthetic.hpp"
#include "apf/fabric.hpp"

#include <cstdio>

namespace {

apf::SyntheticDeviceSpec shape_limited_device(const std::string& key) {
  apf::SyntheticDeviceSpec spec;
  spec.key = key;
  spec.uuid = "SYN-" + key;
  spec.total_compute_slices = 1;
  spec.total_memory_slices = 8;
  spec.max_partition_count = 4;
  spec.physical_totals = *apf::make_resource_vector(
      {{apf::ResourceDimension::ComputeShare, apf::kShareScale},
       {apf::ResourceDimension::MemoryBytes, apf::gib(32)}});
  spec.isolation.add(apf::IsolationProperty::LogicalSeparation);
  spec.isolation.add(apf::IsolationProperty::MemoryIsolation);
  const auto add = [&](const std::string& name, std::uint32_t compute_slices,
                       std::uint32_t memory_slices) {
    apf::SyntheticProfileSpec profile;
    profile.name = name;
    profile.vendor_native = name;
    profile.compute_slices = compute_slices;
    profile.memory_slices = memory_slices;
    profile.resources = *apf::make_resource_vector(
        {{apf::ResourceDimension::ComputeShare, apf::kShareScale / 2},
         {apf::ResourceDimension::MemoryBytes,
          apf::gib(4) * static_cast<std::uint64_t>(memory_slices)}});
    spec.profiles.push_back(std::move(profile));
  };
  add("small", 1, 1);
  add("large", 1, 4);
  return spec;
}

}  // namespace

int main() {
  apf::FabricOptions options;
  options.instance_id = "example-fragmentation";
  auto backend = std::make_shared<apf::SyntheticBackend>(apf::make_system_clock());
  if (!backend->add_device(shape_limited_device("shape-0")).ok()) {
    return 1;
  }
  options.local_backend = backend;
  apf::PartitionFabric fabric(options);
  std::vector<apf::AcceleratorId> discovered;
  if (!fabric.discover(*backend, &discovered).ok() || discovered.empty()) {
    return 1;
  }
  const apf::AcceleratorId accelerator = discovered.front();
  apf::PartitionProfileId small{};
  apf::PartitionProfileId large{};
  for (const apf::PartitionProfile& profile : fabric.snapshot()->profiles) {
    if (profile.name == "small") {
      small = profile.id;
    }
    if (profile.name == "large") {
      large = profile.id;
    }
  }
  apf::PartitionRequest allocate_small;
  allocate_small.profile = small;
  allocate_small.count = 1;
  const apf::Result<apf::PartitionPlan> small_plan = fabric.plan(allocate_small);
  if (!small_plan.ok()) {
    return 1;
  }
  const apf::Result<apf::PartitionReservation> reservation =
      fabric.reserve(small_plan.value().id);
  if (!reservation.ok()) {
    return 1;
  }
  if (!fabric.execute(reservation.value().id).ok()) {
    return 1;
  }
  apf::PartitionRequest allocate_large;
  allocate_large.profile = large;
  allocate_large.count = 1;
  const apf::Result<apf::FragmentationReport> report =
      fabric.analyze_fragmentation(accelerator, allocate_large);
  if (!report.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(report.error()).c_str());
    return 1;
  }
  std::printf("%s\n", report.value().summary().c_str());
  std::printf("aggregate capacity sufficient: %d\n",
              report.value().aggregate_capacity_sufficient ? 1 : 0);
  std::printf("%s", report.value().explanation.format().c_str());
  const apf::Result<apf::PartitionPlan> rejected = fabric.plan(allocate_large);
  if (rejected.ok()) {
    std::printf("plan outcome: %s\n", apf::to_string(rejected.value().outcome));
  }
  return 0;
}
