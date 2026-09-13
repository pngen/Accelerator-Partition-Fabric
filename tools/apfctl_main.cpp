// apfctl: inspection and administrative control tool.
//
// Read-only commands are separated from administrative mutations: nothing in
// this tool mutates state unless the operator both selects a mutating command
// and passes --admin.

#include "apf/backend_nvml.hpp"
#include "apf/backend_synthetic.hpp"
#include "apf/client.hpp"
#include "apf/fabric.hpp"
#include "apf/persistence.hpp"
#include "apf/process.hpp"
#include "apf/version.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using apf::Endpoint;

struct Options {
  std::string coordinator;
  std::string state_path;
  bool admin{false};
  bool verbose{false};
  std::string command;
  std::vector<std::string> positional;
  std::map<std::string, std::string> flags;
};

void usage() {
  std::printf(
      "Accelerator Partition Fabric inspection tool\n"
      "\n"
      "  apfctl [options] <command> [arguments]\n"
      "\n"
      "Options:\n"
      "  --coordinator <host:port>   inspect or administer a running coordinator\n"
      "  --state <file>              inspect a durable state file (read-only)\n"
      "  --admin                     permit administrative mutation commands\n"
      "  --verbose                   print explanations in full\n"
      "\n"
      "Read-only commands:\n"
      "  version | discover [--synthetic] [--device <key>] | probe | snapshot\n"
      "  accelerators | partitions | workers | reservations | attempts | assignments\n"
      "  fragmentation --profile <id> [--count N] [--accelerator N]\n"
      "  plan --profile <id> [--count N] [--allow-drain] [--allow-destructive]\n"
      "  layout --key <backend-key> --worker <id> --boot <hex>\n"
      "\n"
      "Administrative commands (require --admin):\n"
      "  reserve --plan N --worker N --boot HEX | create --reservation N\n"
      "  drain --partition N [--reason TEXT] | complete-drain --partition N\n"
      "  cancel-drain --partition N | destroy --partition N\n"
      "  release --reservation N | reconcile --key K --worker N --boot HEX\n"
      "  fence --worker N --boot HEX | advance-epoch | persist | shutdown\n");
}

apf::Result<Options> parse(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      usage();
      std::exit(0);
    }
    if (argument == "--admin") {
      options.admin = true;
      continue;
    }
    if (argument == "--verbose") {
      options.verbose = true;
      continue;
    }
    if (argument == "--synthetic") {
      options.flags["synthetic"] = "1";
      continue;
    }
    if (argument.rfind("--", 0) == 0) {
      const std::string name = argument.substr(2);
      if (name == "synthetic" || name == "nvml" || name == "allow-drain" ||
          name == "allow-destructive" || name == "allow-degraded") {
        options.flags[name] = "1";
        continue;
      }
      if (index + 1 >= argc) {
        return apf::make_error(apf::ErrorCode::InvalidArgument, "option requires a value",
                               argument);
      }
      options.flags[name] = argv[++index];
      continue;
    }
    if (options.command.empty()) {
      options.command = argument;
      continue;
    }
    options.positional.push_back(argument);
  }
  options.coordinator = options.flags.count("coordinator") != 0 ? options.flags["coordinator"] : "";
  options.state_path = options.flags.count("state") != 0 ? options.flags["state"] : "";
  if (options.command.empty()) {
    return apf::make_error(apf::ErrorCode::InvalidArgument, "no command was given");
  }
  return options;
}

std::string flag(const Options& options, const std::string& name,
                 const std::string& fallback = {}) {
  const auto it = options.flags.find(name);
  return it == options.flags.end() ? fallback : it->second;
}

/// Parses an identity that may be written in decimal or as a 16-digit hex boot
/// identity, matching what the inspection commands print.
apf::Result<apf::WorkerBootId> boot_identity(const Options& options) {
  const std::string text = flag(options, "boot");
  if (text.size() == 16) {
    return apf::WorkerBootId::parse_hex(text);
  }
  const apf::Result<apf::WorkerId> decimal = apf::WorkerId::parse(text);
  if (!decimal.ok()) {
    return decimal.error();
  }
  return apf::WorkerBootId::from_value(decimal.value().value());
}

std::uint64_t number(const Options& options, const std::string& name, std::uint64_t fallback) {
  const std::string text = flag(options, name);
  if (text.empty()) {
    return fallback;
  }
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return fallback;
    }
    value = value * 10 + static_cast<std::uint64_t>(ch - '0');
  }
  return value;
}

int report(const apf::Error& error) {
  std::fprintf(stderr, "error: %s\n", apf::to_string(error).c_str());
  return 1;
}

void print_capability(const apf::AcceleratorRecord& record) {
  std::printf("  capability support=%s mechanism=%s generation=%llu provenance=%s\n",
              apf::to_string(record.capability.support),
              apf::to_string(record.capability.mechanism),
              static_cast<unsigned long long>(record.capability.generation.value()),
              apf::to_string(record.provenance));
  if (!record.capability.unsupported_reason.empty()) {
    std::printf("  unsupported_reason: %s\n", record.capability.unsupported_reason.c_str());
  }
}

int discover_local(const Options& options) {
  const std::string key = flag(options, "device", "synthetic-0");
  const std::uint64_t memory_gib = number(options, "memory-gib", 32);
  apf::FabricOptions fabric_options;
  fabric_options.instance_id = "apfctl-discover";
  fabric_options.limits = apf::default_limits();
  apf::PartitionFabric fabric(fabric_options);

  bool did_any = false;
  if (flag(options, "synthetic") == "1" || flag(options, "nvml").empty()) {
    apf::SyntheticBackend backend(apf::make_system_clock());
    const apf::Status added =
        backend.add_device(apf::make_default_synthetic_device(key, memory_gib, true));
    if (!added.ok()) {
      return report(added.error());
    }
    std::vector<apf::AcceleratorId> discovered;
    const apf::Status status = fabric.discover(backend, &discovered);
    if (!status.ok()) {
      return report(status.error());
    }
    if (discovered.empty()) {
      for (const apf::FabricEvent& event : fabric.events(16)) {
        std::printf("  rejected: %s %s %s\n", apf::to_string(event.code), event.subject.c_str(),
                    event.detail.c_str());
      }
    }
    std::printf("SYNTHETIC discovery (%zu accelerator(s)):\n", discovered.size());
    for (const apf::AcceleratorId& id : discovered) {
      const apf::Result<apf::AcceleratorRecord> record = fabric.accelerator(id);
      if (!record.ok()) {
        continue;
      }
      std::printf("  accelerator %s model=%s backend=%s uuid=%s\n", id.str().c_str(),
                  record.value().identifiers.model.c_str(), record.value().backend.c_str(),
                  record.value().identifiers.uuid.c_str());
      std::printf("  totals=%s\n", record.value().physical_totals.format().c_str());
      print_capability(record.value());
    }
    const std::shared_ptr<const apf::FabricSnapshot> snapshot = fabric.snapshot();
    for (const apf::PartitionProfile& profile : snapshot->profiles) {
      std::printf("  profile %s name=%s vendor_native=%s resources=%s slices=%u/%u\n",
                  profile.id.str().c_str(), profile.name.c_str(),
                  profile.vendor_native.c_str(), profile.resources.format().c_str(),
                  profile.compute_slice_count, profile.memory_slice_count);
    }
    did_any = true;
  }
  if (flag(options, "nvml") == "1" || flag(options, "synthetic").empty()) {
    const apf::Result<std::unique_ptr<apf::NvmlBackend>> backend = apf::NvmlBackend::create();
    if (!backend.ok()) {
      std::printf("REAL discovery: unavailable (%s)\n",
                  apf::to_string(backend.error()).c_str());
    } else {
      std::vector<apf::AcceleratorId> discovered;
      const apf::Status status = fabric.discover(*backend.value(), &discovered);
      if (!status.ok()) {
        std::printf("REAL discovery failed: %s\n", apf::to_string(status.error()).c_str());
      } else {
        if (discovered.empty()) {
          for (const apf::FabricEvent& event : fabric.events(16)) {
            std::printf("  rejected: %s %s %s\n", apf::to_string(event.code),
                        event.subject.c_str(), event.detail.c_str());
          }
        }
        std::printf("REAL discovery (%zu accelerator(s)):\n", discovered.size());
        for (const apf::AcceleratorId& id : discovered) {
          const apf::Result<apf::AcceleratorRecord> record = fabric.accelerator(id);
          if (!record.ok()) {
            continue;
          }
          std::printf("  accelerator %s model=%s uuid=%s pci=%s compute=%s\n",
                      id.str().c_str(), record.value().identifiers.model.c_str(),
                      record.value().identifiers.uuid.c_str(),
                      record.value().identifiers.pci_bus_id.c_str(),
                      record.value().identifiers.compute_capability.c_str());
          std::printf("  totals=%s driver=%s\n", record.value().physical_totals.format().c_str(),
                      record.value().identifiers.driver_version.c_str());
          print_capability(record.value());
        }
      }
    }
    did_any = true;
  }
  if (!did_any) {
    std::fprintf(stderr, "nothing to discover\n");
    return 1;
  }
  return 0;
}

int probe_nvml() {
  const apf::Result<std::unique_ptr<apf::NvmlBackend>> backend = apf::NvmlBackend::create();
  if (!backend.ok()) {
    std::printf("NVML is not reachable: %s\n", apf::to_string(backend.error()).c_str());
    return 0;
  }
  const apf::Result<std::vector<apf::BackendAccelerator>> devices = backend.value()->discover();
  if (!devices.ok()) {
    return report(devices.error());
  }
  std::printf("%s\n", backend.value()->probe_report().c_str());
  std::printf("devices=%zu\n", devices.value().size());
  for (const apf::BackendAccelerator& device : devices.value()) {
    std::printf("  %s model=%s mig_capability=%s\n", device.stable_key.c_str(),
                device.identifiers.model.c_str(), apf::to_string(device.capability.support));
    if (!device.capability.unsupported_reason.empty()) {
      std::printf("    reason: %s\n", device.capability.unsupported_reason.c_str());
    }
  }
  return 0;
}

apf::Result<std::unique_ptr<apf::PartitionFabric>> load_local(const Options& options) {
  apf::FabricOptions fabric_options;
  fabric_options.instance_id = "apfctl-inspect";
  fabric_options.limits = apf::default_limits();
  auto fabric = std::make_unique<apf::PartitionFabric>(fabric_options);
  if (!options.state_path.empty()) {
    apf::PersistenceStore store;
    const apf::Status configured = store.set_path(options.state_path);
    if (!configured.ok()) {
      return configured.error();
    }
    const apf::Status loaded = fabric->load(store);
    if (!loaded.ok()) {
      return loaded.error();
    }
  }
  return fabric;
}

int render_snapshot_text(const std::string& text, const Options& options) {
  if (options.verbose) {
    std::printf("%s", text.c_str());
    return 0;
  }
  std::printf("%s", text.c_str());
  return 0;
}

int run_remote(const Options& options, const std::string& command);

}  // namespace

int main(int argc, char** argv) {
  const apf::Result<Options> parsed = parse(argc, argv);
  if (!parsed.ok()) {
    std::fprintf(stderr, "error: %s\n\n", apf::to_string(parsed.error()).c_str());
    usage();
    return 2;
  }
  const Options& options = parsed.value();
  if (options.command == "version") {
    std::printf("%s\n", apf::version_banner().c_str());
    return 0;
  }
  if (options.command == "probe") {
    return probe_nvml();
  }
  if (options.command == "discover") {
    return discover_local(options);
  }
  if (!options.coordinator.empty()) {
    return run_remote(options, options.command);
  }
  if (options.command == "snapshot" || options.command == "accelerators" ||
      options.command == "partitions" || options.command == "workers" ||
      options.command == "reservations" || options.command == "attempts" ||
      options.command == "assignments") {
    apf::Result<std::unique_ptr<apf::PartitionFabric>> fabric = load_local(options);
    if (!fabric.ok()) {
      return report(fabric.error());
    }
    const std::shared_ptr<const apf::FabricSnapshot> snapshot = fabric.value()->snapshot();
    if (options.command == "snapshot") {
      std::printf("%s", snapshot->render().c_str());
      const apf::Status accounting = fabric.value()->verify_accounting();
      std::printf("accounting=%s\n", accounting.ok() ? "closed" : "VIOLATION");
      return accounting.ok() ? 0 : 1;
    }
    if (options.command == "accelerators") {
      for (const apf::AcceleratorView& view : snapshot->accelerators) {
        std::printf("accelerator %s model=%s backend=%s provenance=%s freshness=%s support=%s\n",
                    view.accelerator.id.str().c_str(), view.accelerator.identifiers.model.c_str(),
                    view.accelerator.backend.c_str(),
                    apf::to_string(view.accelerator.provenance), apf::to_string(view.freshness),
                    apf::to_string(view.accelerator.capability.support));
      }
      return 0;
    }
    if (options.command == "partitions") {
      for (const apf::PartitionRecord& record : snapshot->partitions) {
        std::printf("partition %s gen=%llu state=%s profile=%s accelerator=%s resources=%s\n",
                    record.id.str().c_str(),
                    static_cast<unsigned long long>(record.generation.value()),
                    apf::to_string(record.state), record.profile.str().c_str(),
                    record.accelerator.str().c_str(), record.resources.format().c_str());
      }
      return 0;
    }
    if (options.command == "workers") {
      for (const apf::WorkerRecord& record : snapshot->workers) {
        std::printf("worker %s boot=%s alive=%d fenced=%d backend=%s\n",
                    record.id.str().c_str(), record.boot.hex().c_str(),
                    record.alive ? 1 : 0, record.fenced ? 1 : 0, record.backend.c_str());
      }
      return 0;
    }
    if (options.command == "reservations") {
      for (const apf::PartitionReservation& record : snapshot->reservations) {
        std::printf("reservation %s lifecycle=%s plan=%s resources=%s\n",
                    record.id.str().c_str(), apf::to_string(record.lifecycle),
                    record.plan.str().c_str(), record.resources.format().c_str());
      }
      return 0;
    }
    if (options.command == "attempts") {
      for (const apf::PartitionAttempt& record : snapshot->attempts) {
        std::printf("attempt %s kind=%s state=%s detail=%s\n", record.id.str().c_str(),
                    apf::to_string(record.kind), apf::to_string(record.state),
                    record.detail.c_str());
      }
      return 0;
    }
    for (const apf::PartitionAssignment& record : snapshot->assignments) {
      std::printf("assignment %s partition=%s gen=%llu workload=%s tenant=%s\n",
                  record.id.str().c_str(), record.partition.str().c_str(),
                  static_cast<unsigned long long>(record.partition_generation.value()),
                  record.workload_id.c_str(), record.tenant_id.c_str());
    }
    return 0;
  }
  if (options.command == "fragmentation" || options.command == "plan") {
    apf::Result<std::unique_ptr<apf::PartitionFabric>> fabric = load_local(options);
    if (!fabric.ok()) {
      return report(fabric.error());
    }
    apf::PartitionRequest request;
    const std::string profile = flag(options, "profile");
    if (profile.empty()) {
      std::fprintf(stderr, "error: --profile is required\n");
      return 2;
    }
    const apf::Result<apf::PartitionProfileId> profile_id = apf::PartitionProfileId::parse(profile);
    if (!profile_id.ok()) {
      const std::shared_ptr<const apf::FabricSnapshot> snapshot = fabric.value()->snapshot();
      bool matched = false;
      for (const apf::PartitionProfile& candidate : snapshot->profiles) {
        if (candidate.name == profile || candidate.vendor_native == profile) {
          request.profile = candidate.id;
          matched = true;
          break;
        }
      }
      if (!matched) {
        std::fprintf(stderr, "error: unknown profile %s\n", profile.c_str());
        return 2;
      }
    } else {
      request.profile = profile_id.value();
    }
    request.count = static_cast<std::uint32_t>(number(options, "count", 1));
    request.allow_drain = flag(options, "allow-drain") == "1";
    request.allow_destructive_reconfiguration = flag(options, "allow-destructive") == "1";
    request.allow_degraded_device = flag(options, "allow-degraded") == "1";
    request.requester = "apfctl";
    const std::vector<apf::AcceleratorId> ids = fabric.value()->accelerator_ids();
    if (ids.empty()) {
      std::printf("no accelerators are registered\n");
      return 0;
    }
    for (const apf::AcceleratorId& id : ids) {
      const apf::Result<apf::FragmentationReport> analysis =
          fabric.value()->analyze_fragmentation(id, request);
      if (!analysis.ok()) {
        return report(analysis.error());
      }
      std::printf("%s\n", analysis.value().summary().c_str());
      if (options.verbose) {
        std::printf("%s", analysis.value().explanation.format().c_str());
      }
    }
    if (options.command == "plan") {
      const apf::Result<apf::PartitionPlan> plan = fabric.value()->plan(request);
      if (!plan.ok()) {
        return report(plan.error());
      }
      std::printf("plan outcome=%s\n", apf::to_string(plan.value().outcome));
      for (const apf::PlanStep& step : plan.value().steps) {
        std::printf("  step %s\n", step.describe().c_str());
      }
      std::printf("%s", plan.value().explanation.format().c_str());
    }
    return 0;
  }
  std::fprintf(stderr, "error: unknown or unusable command '%s'\n\n", options.command.c_str());
  usage();
  return 2;
}

namespace {

int run_remote(const Options& options, const std::string& command) {
  const apf::Result<Endpoint> endpoint = Endpoint::parse(options.coordinator);
  if (!endpoint.ok()) {
    return report(endpoint.error());
  }
  const apf::Result<std::unique_ptr<apf::CoordinatorClient>> client =
      apf::CoordinatorClient::connect(endpoint.value());
  if (!client.ok()) {
    return report(client.error());
  }
  const bool mutating = command == "reserve" || command == "create" || command == "destroy" ||
                        command == "drain" || command == "complete-drain" ||
                        command == "cancel-drain" || command == "release" ||
                        command == "reconcile" || command == "fence" ||
                        command == "advance-epoch" || command == "persist" ||
                        command == "shutdown";
  if (mutating && !options.admin) {
    std::fprintf(stderr,
                 "error: '%s' mutates runtime state; re-run with --admin to confirm\n",
                 command.c_str());
    return 3;
  }
  if (command == "snapshot") {
    const apf::Result<apf::CommandResponseMessage> response = client.value()->inspect_snapshot();
    if (!response.ok()) {
      return report(response.error());
    }
    std::printf("%s", response.value().snapshot_text.c_str());
    return response.value().accepted ? 0 : 1;
  }
  if (command == "accelerators") {
    const apf::Result<apf::CommandResponseMessage> response =
        client.value()->inspect_accelerators();
    if (!response.ok()) {
      return report(response.error());
    }
    for (const apf::BackendAccelerator& device : response.value().devices) {
      std::printf("accelerator key=%s model=%s provenance=%s support=%s\n",
                  device.stable_key.c_str(), device.identifiers.model.c_str(),
                  apf::to_string(device.provenance), apf::to_string(device.capability.support));
      if (!device.unsupported_reason.empty()) {
        std::printf("  unsupported_reason: %s\n", device.unsupported_reason.c_str());
      }
      for (const apf::PartitionProfile& profile : device.profiles) {
        std::printf("  profile id=%s name=%s vendor_native=%s resources=%s slices=%u/%u\n",
                    profile.id.str().c_str(), profile.name.c_str(), profile.vendor_native.c_str(),
                    profile.resources.format().c_str(), profile.compute_slice_count,
                    profile.memory_slice_count);
      }
    }
    return 0;
  }
  if (command == "partitions") {
    const apf::Result<apf::CommandResponseMessage> response =
        client.value()->inspect_partitions();
    if (!response.ok()) {
      return report(response.error());
    }
    for (const apf::PartitionRecord& record : response.value().partition_records) {
      std::printf("partition %s gen=%llu state=%s profile=%s native=%s\n",
                  record.id.str().c_str(),
                  static_cast<unsigned long long>(record.generation.value()),
                  apf::to_string(record.state), record.profile.str().c_str(),
                  record.native_identity.native_id.c_str());
    }
    return 0;
  }
  if (command == "workers") {
    const apf::Result<apf::CommandResponseMessage> response = client.value()->inspect_workers();
    if (!response.ok()) {
      return report(response.error());
    }
    for (const apf::WorkerRecord& record : response.value().workers) {
      std::printf("worker %s boot=%s alive=%d fenced=%d devices=%u reason=%s\n",
                  record.id.str().c_str(), record.boot.hex().c_str(), record.alive ? 1 : 0,
                  record.fenced ? 1 : 0, record.device_count, record.fence_reason.c_str());
    }
    return 0;
  }
  if (command == "layout") {
    const apf::Result<apf::CommandResponseMessage> response = client.value()->query_layout(
        flag(options, "key"));
    if (!response.ok()) {
      return report(response.error());
    }
    std::printf("device_present=%d partitions=%zu detail=%s\n",
                response.value().layout.device_present ? 1 : 0,
                response.value().layout.partitions.size(), response.value().layout.detail.c_str());
    for (const apf::BackendNativePartition& partition : response.value().layout.partitions) {
      std::printf("  native_id=%s profile=%s resources=%s\n", partition.native_id.c_str(),
                  partition.vendor_native_profile.c_str(), partition.resources.format().c_str());
    }
    return 0;
  }
  if (command == "plan") {
    apf::PartitionRequest request;
    request.count = static_cast<std::uint32_t>(number(options, "count", 1));
    request.allow_drain = flag(options, "allow-drain") == "1";
    request.allow_destructive_reconfiguration = flag(options, "allow-destructive") == "1";
    request.requester = "apfctl";
    const std::string profile = flag(options, "profile");
    const apf::Result<apf::PartitionProfileId> profile_id = apf::PartitionProfileId::parse(profile);
    if (!profile_id.ok()) {
      std::fprintf(stderr, "error: --profile must be a numeric profile identity\n");
      return 2;
    }
    request.profile = profile_id.value();
    const apf::Result<apf::CommandResponseMessage> response = client.value()->plan(request);
    if (!response.ok()) {
      return report(response.error());
    }
    std::printf("outcome=%s message=%s\n", apf::to_string(response.value().plan_outcome),
                response.value().message.c_str());
    for (const apf::PlanStep& step : response.value().plan_steps) {
      std::printf("  step %s\n", step.describe().c_str());
    }
    if (options.verbose) {
      std::printf("%s", response.value().detail.c_str());
    }
    return 0;
  }
  apf::Result<apf::CommandResponseMessage> response =
      apf::make_error(apf::ErrorCode::InvalidRequest, "unsupported command");
  if (command == "reserve") {
    const apf::Result<apf::WorkerBootId> boot = boot_identity(options);
    if (!boot.ok()) {
      return report(boot.error());
    }
    response = client.value()->reserve(apf::PartitionPlanId::from_value(number(options, "plan", 0)),
                                       apf::WorkerId::from_value(number(options, "worker", 0)),
                                       boot.value());
  } else if (command == "create") {
    response = client.value()->create(
        apf::PartitionReservationId::from_value(number(options, "reservation", 0)));
  } else if (command == "destroy") {
    response = client.value()->destroy(apf::PartitionId::from_value(number(options, "partition", 0)));
  } else if (command == "drain") {
    response = client.value()->drain(apf::PartitionId::from_value(number(options, "partition", 0)),
                                     flag(options, "reason", "operator requested drain"));
  } else if (command == "complete-drain") {
    response = client.value()->complete_drain(
        apf::PartitionId::from_value(number(options, "partition", 0)));
  } else if (command == "cancel-drain") {
    response = client.value()->cancel_drain(
        apf::PartitionId::from_value(number(options, "partition", 0)));
  } else if (command == "release") {
    response = client.value()->release_reservation(
        apf::PartitionReservationId::from_value(number(options, "reservation", 0)),
        flag(options, "reason", "operator released the reservation"));
  } else if (command == "reconcile") {
    const apf::Result<apf::WorkerBootId> boot = boot_identity(options);
    if (!boot.ok()) {
      return report(boot.error());
    }
    response = client.value()->reconcile(
        apf::WorkerId::from_value(number(options, "worker", 0)), boot.value(),
        flag(options, "key"));
  } else if (command == "fence") {
    const apf::Result<apf::WorkerBootId> boot = boot_identity(options);
    if (!boot.ok()) {
      return report(boot.error());
    }
    response = client.value()->fence_worker(
        apf::WorkerId::from_value(number(options, "worker", 0)), boot.value(),
        flag(options, "reason", "operator fenced the worker"));
  } else if (command == "advance-epoch") {
    response = client.value()->advance_epoch();
  } else if (command == "persist") {
    response = client.value()->persist_state();
  } else if (command == "shutdown") {
    apf::CommandRequestMessage request;
    request.kind = apf::CommandKind::ShutdownCoordinator;
    request.reason = flag(options, "reason", "operator requested shutdown");
    response = client.value()->command(request);
  } else {
    std::fprintf(stderr, "error: unknown command '%s'\n", command.c_str());
    return 2;
  }
  if (!response.ok()) {
    return report(response.error());
  }
  std::printf("accepted=%d %s\n", response.value().accepted ? 1 : 0,
              response.value().message.c_str());
  if (!response.value().detail.empty() && options.verbose) {
    std::printf("%s\n", response.value().detail.c_str());
  }
  if (response.value().outcome_unknown) {
    std::printf("note: the physical outcome is unknown and requires reconciliation\n");
  }
  return response.value().accepted ? 0 : 1;
}

}  // namespace