// Reconciliation after the physical world changes behind the runtime's back.
//
// Shows the three conservative outcomes: a partition that disappeared, a
// partition that appeared without authorisation, and a device that reset.

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

}  // namespace

int main() {
  apf::FabricOptions options;
  options.instance_id = "example-reconcile";
  auto backend = std::make_shared<apf::SyntheticBackend>(apf::make_system_clock());
  if (!backend->add_device(apf::make_default_synthetic_device("sim-0", 32, true)).ok()) {
    return 1;
  }
  options.local_backend = backend;
  apf::PartitionFabric fabric(options);
  std::vector<apf::AcceleratorId> discovered;
  if (!fabric.discover(*backend, &discovered).ok() || discovered.empty()) {
    return 1;
  }
  const apf::AcceleratorId accelerator = discovered.front();
  apf::PartitionRequest request;
  request.profile = profile_named(*fabric.snapshot(), "synthetic-2g");
  request.count = 1;
  const apf::Result<apf::PartitionPlan> plan = fabric.plan(request);
  if (!plan.ok()) {
    return 1;
  }
  const apf::Result<apf::PartitionReservation> reservation = fabric.reserve(plan.value().id);
  if (!reservation.ok()) {
    return 1;
  }
  const apf::Result<apf::MutationOutcome> created = fabric.execute(reservation.value().id);
  if (!created.ok() || !created.value().committed) {
    std::fprintf(stderr, "creation failed\n");
    return 1;
  }
  apf::PartitionId partition = created.value().partitions.front();
  const apf::Result<apf::PartitionRecord> record = fabric.partition(partition);
  if (!record.ok()) {
    return 1;
  }
  // 1. The partition disappears without the runtime being told.
  if (!backend->external_destroy("sim-0", record.value().native_identity.native_id).ok()) {
    return 1;
  }
  apf::Result<apf::BackendLayout> layout = backend->query_layout("sim-0");
  if (!layout.ok()) {
    return 1;
  }
  apf::Result<apf::ReconciliationReport> report = fabric.reconcile(accelerator, layout.value());
  if (!report.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(report.error()).c_str());
    return 1;
  }
  std::printf("missing case: %s\n", report.value().summary().c_str());
  // 2. A partition appears that no authorised mutation created.
  std::string native_id;
  if (!backend->external_create("sim-0", "synthetic-1g", &native_id).ok()) {
    return 1;
  }
  layout = backend->query_layout("sim-0");
  if (!layout.ok()) {
    return 1;
  }
  report = fabric.reconcile(accelerator, layout.value());
  if (!report.ok()) {
    return 1;
  }
  std::printf("unexpected case: %s\n", report.value().summary().c_str());
  for (const apf::ReconciliationFinding& finding : report.value().findings) {
    std::printf("  finding %s severity=%s: %s\n", apf::to_string(finding.kind),
                apf::to_string(finding.severity), finding.detail.c_str());
  }
  // 3. The device resets: durable structure survives, authority does not.
  if (!backend->reset_device("sim-0").ok()) {
    return 1;
  }
  layout = backend->query_layout("sim-0");
  if (!layout.ok()) {
    return 1;
  }
  report = fabric.reconcile(accelerator, layout.value());
  if (!report.ok()) {
    return 1;
  }
  std::printf("reset case: device_generation_changed=%d revalidation_required=%d\n",
              report.value().device_generation_changed ? 1 : 0,
              report.value().revalidation_required ? 1 : 0);
  const apf::Status closed = fabric.verify_accounting();
  std::printf("accounting: %s\n", closed.ok() ? "closed" : "VIOLATION");
  return closed.ok() ? 0 : 1;
}
