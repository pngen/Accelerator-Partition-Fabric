// Drain, then destructive reconfiguration under policy control.
//
// Shows that an occupied partition cannot be repartitioned until its drain gate
// completes, that the plan binds the state it was derived from, and that old
// partition generations are fenced when the new layout is published.

#include "apf/backend_synthetic.hpp"
#include "apf/fabric.hpp"

#include <cstdio>

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

int create_one(apf::PartitionFabric& fabric, const apf::FabricSnapshot& snapshot,
               const std::string& name) {
  apf::PartitionRequest request;
  request.profile = profile_named(snapshot, name);
  request.count = 1;
  const apf::Result<apf::PartitionPlan> plan = fabric.plan(request);
  if (!plan.ok()) {
    return 1;
  }
  const apf::Result<apf::PartitionReservation> reservation = fabric.reserve(plan.value().id);
  if (!reservation.ok()) {
    return 1;
  }
  const apf::Result<apf::MutationOutcome> outcome = fabric.execute(reservation.value().id);
  return outcome.ok() && outcome.value().committed ? 0 : 1;
}

}  // namespace

int main() {
  apf::FabricOptions options;
  options.instance_id = "example-reconfigure";
  auto backend = std::make_shared<apf::SyntheticBackend>(apf::make_system_clock());
  if (!backend->add_device(apf::make_default_synthetic_device("sim-0", 32, true)).ok()) {
    return 1;
  }
  options.local_backend = backend;
  apf::PartitionFabric fabric(options);
  apf::PlanningPolicy policy = apf::make_default_policy(apf::PolicyGeneration::first());
  policy.allow_destructive_reconfiguration = true;
  policy.allow_drain = true;
  if (!fabric.set_policy(policy).ok()) {
    return 1;
  }
  std::vector<apf::AcceleratorId> discovered;
  if (!fabric.discover(*backend, &discovered).ok() || discovered.empty()) {
    return 1;
  }
  const apf::AcceleratorId accelerator = discovered.front();
  if (create_one(fabric, *fabric.snapshot(), "synthetic-2g") != 0) {
    std::fprintf(stderr, "initial creation failed\n");
    return 1;
  }
  apf::PartitionId occupied{};
  for (const apf::PartitionRecord& record : fabric.snapshot()->partitions) {
    if (record.state == apf::PartitionState::Active) {
      occupied = record.id;
    }
  }
  // A workload occupies the partition, so destructive mutation must be refused.
  apf::WorkloadRequirement requirement;
  requirement.workload_id = "example-workload";
  requirement.required_profile = profile_named(*fabric.snapshot(), "synthetic-2g");
  const apf::Result<apf::AdmissionDecision> admitted = fabric.admit(requirement);
  if (!admitted.ok() || !admitted.value().admitted()) {
    std::fprintf(stderr, "admission failed\n");
    return 1;
  }
  const apf::Result<apf::MutationOutcome> refused = fabric.destroy_partition(occupied);
  std::printf("destroy while occupied: %s\n",
              refused.ok() ? "accepted" : apf::to_string(refused.error()).c_str());
  if (!fabric.begin_drain(occupied, "example drain").ok()) {
    return 1;
  }
  std::printf("drain incomplete while work is outstanding: %s\n",
              apf::to_string(fabric.complete_drain(occupied).error()).c_str());
  if (!fabric.unbind_assignment(admitted.value().assignment, "work released").ok()) {
    return 1;
  }
  if (!fabric.complete_drain(occupied).ok()) {
    return 1;
  }
  // Now the layout can be rebuilt.
  const apf::PartitionProfile* target =
      fabric.snapshot()->find_profile(profile_named(*fabric.snapshot(), "synthetic-3g"));
  if (target == nullptr) {
    return 1;
  }
  std::vector<apf::PlannedPartition> desired;
  apf::PlannedPartition planned;
  planned.profile = target->id;
  planned.profile_generation = target->generation;
  planned.resources = target->resources;
  planned.vendor_native_profile = target->vendor_native;
  desired.push_back(planned);
  apf::PartitionRequest context;
  context.allow_destructive_reconfiguration = true;
  context.profile = target->id;
  context.count = 1;
  const apf::Result<apf::PartitionPlan> plan =
      fabric.plan_reconfiguration(accelerator, desired, context);
  if (!plan.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(plan.error()).c_str());
    return 1;
  }
  const apf::Result<apf::MutationOutcome> outcome =
      fabric.execute_reconfiguration(plan.value().id);
  if (!outcome.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(outcome.error()).c_str());
    return 1;
  }
  std::printf("reconfiguration committed=%d\n", outcome.value().committed ? 1 : 0);
  for (const apf::PartitionRecord& record : fabric.snapshot()->partitions) {
    std::printf("partition %s state=%s generation=%llu\n", record.id.str().c_str(),
                apf::to_string(record.state),
                static_cast<unsigned long long>(record.generation.value()));
  }
  const apf::Status closed = fabric.verify_accounting();
  std::printf("accounting: %s\n", closed.ok() ? "closed" : "VIOLATION");
  return closed.ok() ? 0 : 1;
}
