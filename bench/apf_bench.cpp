// Benchmarks for the operations that actually matter in this runtime.
//
// Every benchmark measures completed work: each timed iteration completes the
// whole operation before the clock is read. Sizes are large enough to expose
// accidental quadratic behaviour, and the reported numbers are the ones
// observed on this machine, not estimates.

#include "apf/backend_synthetic.hpp"
#include "apf/fabric.hpp"
#include "apf/persistence.hpp"
#include "apf/process.hpp"
#include "apf/protocol.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(const Clock::time_point& start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void report(const char* name, std::size_t operations, double seconds) {
  const double per_operation = operations == 0 ? 0.0 : seconds / static_cast<double>(operations);
  std::printf("%-46s %8zu ops %9.4f s %11.3f us/op\n", name, operations, seconds,
              per_operation * 1e6);
}

std::unique_ptr<apf::PartitionFabric> make_fabric(std::size_t devices,
                                                  std::shared_ptr<apf::SyntheticBackend>* out) {
  apf::FabricOptions options;
  options.instance_id = "bench";
  auto backend = std::make_shared<apf::SyntheticBackend>(apf::make_system_clock());
  for (std::size_t index = 0; index < devices; ++index) {
    (void)backend->add_device(
        apf::make_default_synthetic_device("bench-" + std::to_string(index), 32, true));
  }
  options.local_backend = backend;
  auto fabric = std::make_unique<apf::PartitionFabric>(options);
  std::vector<apf::AcceleratorId> discovered;
  (void)fabric->discover(*backend, &discovered);
  *out = backend;
  return fabric;
}

apf::PartitionProfileId profile_named(const apf::FabricSnapshot& snapshot,
                                      const std::string& name) {
  for (const apf::PartitionProfile& profile : snapshot.profiles) {
    if (profile.name == name) {
      return profile.id;
    }
  }
  return apf::PartitionProfileId{};
}

void fill_partitions(apf::PartitionFabric& fabric, const std::string& profile_name,
                     std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    apf::PartitionRequest request;
    request.profile = profile_named(*fabric.snapshot(), profile_name);
    request.count = 1;
    const apf::Result<apf::PartitionPlan> plan = fabric.plan(request);
    if (!plan.ok() || !plan.value().feasible()) {
      continue;
    }
    const apf::Result<apf::PartitionReservation> reservation = fabric.reserve(plan.value().id);
    if (!reservation.ok()) {
      continue;
    }
    (void)fabric.execute(reservation.value().id);
  }
}

void bench_planning() {
  const std::size_t device_counts[] = {1, 16, 64};
  for (const std::size_t devices : device_counts) {
    std::shared_ptr<apf::SyntheticBackend> backend;
    std::unique_ptr<apf::PartitionFabric> fabric = make_fabric(devices, &backend);
    apf::PartitionRequest request;
    request.profile = profile_named(*fabric->snapshot(), "synthetic-2g");
    request.count = 1;
    const std::size_t iterations = 2000;
    const Clock::time_point start = Clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
      (void)fabric->plan(request);
    }
    report(("plan (devices=" + std::to_string(devices) + ")").c_str(), iterations,
           seconds_since(start));
  }
}

void bench_fragmentation() {
  std::shared_ptr<apf::SyntheticBackend> backend;
  std::unique_ptr<apf::PartitionFabric> fabric = make_fabric(1, &backend);
  apf::PartitionRequest request;
  request.profile = profile_named(*fabric->snapshot(), "synthetic-2g");
  request.count = 1;
  const std::vector<apf::AcceleratorId> ids = fabric->accelerator_ids();
  const std::size_t iterations = 5000;
  const Clock::time_point start = Clock::now();
  for (std::size_t index = 0; index < iterations; ++index) {
    (void)fabric->analyze_fragmentation(ids.front(), request);
  }
  report("fragmentation analysis", iterations, seconds_since(start));
}

void bench_reservation_commit() {
  std::shared_ptr<apf::SyntheticBackend> backend;
  std::unique_ptr<apf::PartitionFabric> fabric = make_fabric(1, &backend);
  apf::PartitionRequest request;
  request.profile = profile_named(*fabric->snapshot(), "synthetic-1g");
  request.count = 1;
  const std::size_t iterations = 1000;
  const Clock::time_point start = Clock::now();
  std::size_t completed = 0;
  for (std::size_t index = 0; index < iterations; ++index) {
    const apf::Result<apf::PartitionPlan> plan = fabric->plan(request);
    if (!plan.ok() || !plan.value().feasible()) {
      continue;
    }
    const apf::Result<apf::PartitionReservation> reservation =
        fabric->reserve(plan.value().id);
    if (!reservation.ok()) {
      continue;
    }
    const apf::Result<apf::MutationOutcome> outcome =
        fabric->execute(reservation.value().id);
    if (outcome.ok() && outcome.value().committed) {
      ++completed;
    }
  }
  std::printf("%-46s %8zu attempts, %zu committed\n",
              "reserve + execute + verified commit", iterations, completed);
  report("reserve + execute + verified commit", iterations, seconds_since(start));
}

void bench_snapshot() {
  std::shared_ptr<apf::SyntheticBackend> backend;
  std::unique_ptr<apf::PartitionFabric> fabric = make_fabric(8, &backend);
  fill_partitions(*fabric, "synthetic-1g", 8);
  const std::size_t iterations = 2000;
  const Clock::time_point start = Clock::now();
  for (std::size_t index = 0; index < iterations; ++index) {
    (void)fabric->snapshot();
  }
  report("snapshot (8 devices)", iterations, seconds_since(start));
}

void bench_admission() {
  std::shared_ptr<apf::SyntheticBackend> backend;
  std::unique_ptr<apf::PartitionFabric> fabric = make_fabric(1, &backend);
  fill_partitions(*fabric, "synthetic-7g", 1);
  const std::size_t iterations = 5000;
  const Clock::time_point start = Clock::now();
  std::size_t decisions = 0;
  for (std::size_t index = 0; index < iterations; ++index) {
    apf::WorkloadRequirement requirement;
    requirement.workload_id = "bench-workload-" + std::to_string(index);
    const apf::Result<apf::AdmissionProbe> probe = fabric->probe_admission(requirement);
    if (probe.ok()) {
      ++decisions;
    }
  }
  report("admission evaluation", decisions, seconds_since(start));
}

void bench_reconciliation() {
  std::shared_ptr<apf::SyntheticBackend> backend;
  std::unique_ptr<apf::PartitionFabric> fabric = make_fabric(1, &backend);
  fill_partitions(*fabric, "synthetic-1g", 4);
  const std::vector<apf::AcceleratorId> ids = fabric->accelerator_ids();
  const std::size_t iterations = 1000;
  const Clock::time_point start = Clock::now();
  std::size_t completed = 0;
  for (std::size_t index = 0; index < iterations; ++index) {
    apf::Result<apf::BackendLayout> layout = backend->query_layout("bench-0");
    if (!layout.ok()) {
      continue;
    }
    apf::Result<apf::ReconciliationReport> report =
        fabric->reconcile(ids.front(), layout.value());
    if (report.ok()) {
      ++completed;
    }
  }
  report("reconciliation (4 partitions)", completed, seconds_since(start));
}

void bench_explanation() {
  std::shared_ptr<apf::SyntheticBackend> backend;
  std::unique_ptr<apf::PartitionFabric> fabric = make_fabric(1, &backend);
  apf::PartitionRequest request;
  request.profile = profile_named(*fabric->snapshot(), "synthetic-2g");
  request.count = 1;
  const std::size_t iterations = 5000;
  const Clock::time_point start = Clock::now();
  std::size_t completed = 0;
  for (std::size_t index = 0; index < iterations; ++index) {
    const apf::Result<apf::PartitionPlan> plan = fabric->plan(request);
    if (plan.ok() && !plan.value().explanation.empty()) {
      ++completed;
    }
  }
  report("deterministic explanation", completed, seconds_since(start));
}

void bench_persistence() {
  std::shared_ptr<apf::SyntheticBackend> backend;
  std::unique_ptr<apf::PartitionFabric> fabric = make_fabric(4, &backend);
  fill_partitions(*fabric, "synthetic-1g", 16);
  apf::Result<apf::DurableState> durable = fabric->export_durable_state();
  if (!durable.ok()) {
    return;
  }
  apf::Limits limits;
  const std::size_t iterations = 500;
  const Clock::time_point encode_start = Clock::now();
  std::vector<std::uint8_t> encoded;
  for (std::size_t index = 0; index < iterations; ++index) {
    apf::Result<std::vector<std::uint8_t>> bytes =
        apf::PersistenceStore::encode(durable.value(), limits);
    if (bytes.ok()) {
      encoded = bytes.value();
    }
  }
  report("durable encode", iterations, seconds_since(encode_start));
  const Clock::time_point decode_start = Clock::now();
  for (std::size_t index = 0; index < iterations; ++index) {
    (void)apf::PersistenceStore::decode(encoded.data(), encoded.size(), limits);
  }
  report("durable decode + validate", iterations, seconds_since(decode_start));

  apf::PersistenceStore store;
  const std::string path = apf::join_path(apf::temp_directory(), "apf-bench.state");
  if (!store.set_path(path).ok()) {
    return;
  }
  const std::size_t save_iterations = 100;
  const Clock::time_point save_start = Clock::now();
  for (std::size_t index = 0; index < save_iterations; ++index) {
    (void)fabric->save(store);
  }
  report("durable save (atomic replace)", save_iterations, seconds_since(save_start));
  const std::size_t load_iterations = 100;
  const Clock::time_point load_start = Clock::now();
  for (std::size_t index = 0; index < load_iterations; ++index) {
    apf::PartitionFabric reloaded(apf::FabricOptions{});
    (void)reloaded.load(store);
  }
  report("durable load + conservative import", load_iterations, seconds_since(load_start));
  (void)apf::remove_file(path);
}

void bench_query_payload() {
  apf::Limits limits;
  apf::QueryResponseMessage response;
  response.kind = apf::QueryKind::Layout;
  response.found = true;
  response.snapshot_text = std::string(4096, 'x');
  const std::size_t iterations = 5000;
  const Clock::time_point start = Clock::now();
  std::size_t completed = 0;
  for (std::size_t index = 0; index < iterations; ++index) {
    const std::vector<std::uint8_t> payload = apf::pack_payload(response, limits);
    apf::Result<apf::QueryResponseMessage> decoded = apf::unpack_payload<apf::QueryResponseMessage>(
        payload.data(), payload.size(), limits);
    if (decoded.ok()) {
      ++completed;
    }
  }
  report("query payload round trip (4 KiB body)", completed, seconds_since(start));
}

std::string format_capacity_sweep() {
  // A scaling probe for candidate filtering: planning cost against the number
  // of published profiles on one device.
  std::string summary;
  for (const std::size_t profiles : {4u, 16u, 64u}) {
    apf::SyntheticDeviceSpec spec = apf::make_default_synthetic_device("scale-0", 32, true);
    for (std::size_t index = 4; index < profiles; ++index) {
      apf::SyntheticProfileSpec profile;
      profile.name = "scale-" + std::to_string(index);
      profile.vendor_native = "scale-" + std::to_string(index);
      profile.compute_slices = 1;
      profile.memory_slices = 1;
      profile.resources = *apf::make_resource_vector(
          {{apf::ResourceDimension::ComputeShare, apf::kShareScale / 7},
           {apf::ResourceDimension::MemoryBytes, apf::gib(4)}});
      spec.profiles.push_back(std::move(profile));
    }
    auto backend = std::make_shared<apf::SyntheticBackend>(apf::make_system_clock());
    (void)backend->add_device(spec);
    apf::FabricOptions options;
    options.local_backend = backend;
    apf::PartitionFabric fabric(options);
    std::vector<apf::AcceleratorId> discovered;
    (void)fabric.discover(*backend, &discovered);
    apf::PartitionRequest request;
    request.profile = apf::PartitionProfileId::from_value(1);
    const std::size_t iterations = 2000;
    const Clock::time_point start = Clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
      (void)fabric.plan(request);
    }
    summary += "profiles=" + std::to_string(profiles) + ":" +
               std::to_string(seconds_since(start) / static_cast<double>(iterations) * 1e6) +
               "us ";
  }
  return summary;
}

}  // namespace

int main() {
  std::printf("%s benchmarks\n\n", apf::version_banner().c_str());
  bench_planning();
  bench_fragmentation();
  bench_reservation_commit();
  bench_snapshot();
  bench_admission();
  bench_reconciliation();
  bench_explanation();
  bench_persistence();
  bench_query_payload();
  std::printf("\nplanning cost against published profile count:\n  %s\n",
              format_capacity_sweep().c_str());
  return 0;
}
