// Reservation, physical creation, verification and release.
//
// Shows that capacity is held before the mutation, that authority is published
// only after the physical result has been observed, and that accounting returns
// to its baseline afterwards.

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
  options.instance_id = "example-reserve";
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
    std::fprintf(stderr, "error: %s\n", apf::to_string(plan.error()).c_str());
    return 1;
  }
  const apf::Result<apf::PartitionReservation> reservation = fabric.reserve(plan.value().id);
  if (!reservation.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(reservation.error()).c_str());
    return 1;
  }
  const apf::Result<apf::CapacityLedger> held = fabric.ledger(accelerator);
  if (held.ok()) {
    std::printf("reserved: %s\n",
                held.value().bucket(apf::CapacityBucket::Reserved).format().c_str());
  }
  const apf::Result<apf::MutationOutcome> outcome = fabric.execute(reservation.value().id);
  if (!outcome.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(outcome.error()).c_str());
    return 1;
  }
  std::printf("committed=%d verified_physically=%d outcome_unknown=%d\n",
              outcome.value().committed ? 1 : 0, outcome.value().verified_physically ? 1 : 0,
              outcome.value().outcome_unknown ? 1 : 0);
  for (const apf::PartitionId& partition : outcome.value().partitions) {
    const apf::Result<apf::PartitionRecord> record = fabric.partition(partition);
    if (record.ok()) {
      std::printf("partition %s state=%s native=%s\n", record.value().id.str().c_str(),
                  apf::to_string(record.value().state),
                  record.value().native_identity.native_id.c_str());
    }
  }
  const apf::Status closed = fabric.verify_accounting();
  std::printf("accounting: %s\n", closed.ok() ? "closed" : "VIOLATION");
  return closed.ok() ? 0 : 1;
}
