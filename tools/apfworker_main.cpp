// apfworker: a physical-mutation worker process.
//
// Owns a hardware backend and executes physical partition mutation on behalf of
// a coordinator, reporting observed physical state rather than assumed results.
// A replacement process always presents a fresh boot identity.

#include "apf/backend_nvml.hpp"
#include "apf/backend_synthetic.hpp"
#include "apf/process.hpp"
#include "apf/version.hpp"
#include "apf/worker.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void usage() {
  std::printf(
      "Accelerator Partition Fabric worker\n"
      "\n"
      "  apfworker --coordinator <host:port> [options]\n"
      "\n"
      "Options:\n"
      "  --coordinator <host:port>  coordinator endpoint (required)\n"
      "  --backend synthetic|nvml   hardware backend (default synthetic)\n"
      "  --device <key>             synthetic device key (default synthetic-0)\n"
      "  --memory-gib N             synthetic device memory (default 32)\n"
      "  --device-state <file>      persist the synthetic physical model so a\n"
      "                             replacement process observes reality\n"
      "  --worker-id N              stable worker identity\n"
      "  --boot-hex HEX             explicit boot identity (16 hex digits)\n"
      "  --ambiguous-once           apply the next mutation, then die before\n"
      "                             acknowledging it (fault injection)\n"
      "  --die-after-register       exit immediately after registering\n"
      "  --description TEXT         endpoint description reported to the coordinator\n"
      "  --ready-file <file>        write the assigned identity once registered\n"
      "\n"
      "Prints APF-WORKER-READY <id> <boot> once registered, then serves until the\n"
      "coordinator shuts down or the process is killed.\n");
}

}  // namespace

int main(int argc, char** argv) {
  apf::WorkerOptions options;
  std::string endpoint_text;
  std::string backend_name = "synthetic";
  std::string device_key = "synthetic-0";
  std::string ready_file;
  std::uint64_t memory_gib = 32;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    }
    if (argument == "--version") {
      std::printf("%s\n", apf::version_banner().c_str());
      return 0;
    }
    if (argument == "--ambiguous-once") {
      options.ambiguous_next_mutation = true;
      continue;
    }
    if (argument == "--die-after-register") {
      options.die_after_register = true;
      continue;
    }
    if (index + 1 >= argc) {
      std::fprintf(stderr, "error: option %s requires a value\n", argument.c_str());
      return 2;
    }
    const std::string value = argv[++index];
    if (argument == "--coordinator") {
      endpoint_text = value;
    } else if (argument == "--backend") {
      backend_name = value;
    } else if (argument == "--device") {
      device_key = value;
    } else if (argument == "--memory-gib") {
      memory_gib = std::strtoull(value.c_str(), nullptr, 10);
    } else if (argument == "--device-state") {
      options.device_state_path = value;
    } else if (argument == "--ready-file") {
      ready_file = value;
    } else if (argument == "--worker-id") {
      options.worker_id = apf::WorkerId::from_value(std::strtoull(value.c_str(), nullptr, 10));
    } else if (argument == "--boot-hex") {
      const apf::Result<apf::WorkerBootId> boot = apf::WorkerBootId::parse_hex(value);
      if (!boot.ok()) {
        std::fprintf(stderr, "error: %s\n", apf::to_string(boot.error()).c_str());
        return 2;
      }
      options.worker_boot = boot.value();
    } else if (argument == "--description") {
      options.description = value;
    } else {
      std::fprintf(stderr, "error: unknown option %s\n", argument.c_str());
      return 2;
    }
  }
  if (endpoint_text.empty()) {
    std::fprintf(stderr, "error: --coordinator is required\n");
    return 2;
  }
  const apf::Result<apf::Endpoint> endpoint = apf::Endpoint::parse(endpoint_text);
  if (!endpoint.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(endpoint.error()).c_str());
    return 2;
  }
  options.coordinator = endpoint.value();
  if (backend_name == "nvml") {
    apf::Result<std::unique_ptr<apf::NvmlBackend>> backend = apf::NvmlBackend::create();
    if (!backend.ok()) {
      std::fprintf(stderr, "error: %s\n", apf::to_string(backend.error()).c_str());
      return 3;
    }
    options.backend_name = "nvidia-nvml";
    options.backend = std::shared_ptr<apf::AcceleratorBackend>(backend.value().release());
  } else if (backend_name == "synthetic") {
    auto backend = std::make_shared<apf::SyntheticBackend>(apf::make_system_clock());
    const apf::Status added =
        backend->add_device(apf::make_default_synthetic_device(device_key, memory_gib, true));
    if (!added.ok()) {
      std::fprintf(stderr, "error: %s\n", apf::to_string(added.error()).c_str());
      return 3;
    }
    options.backend_name = "synthetic";
    options.backend = backend;
  } else {
    std::fprintf(stderr, "error: unknown backend %s\n", backend_name.c_str());
    return 2;
  }
  if (options.description.empty()) {
    options.description = device_key + "@" + backend_name;
  }
  const apf::Result<std::unique_ptr<apf::PartitionWorker>> worker =
      apf::PartitionWorker::create(options);
  if (!worker.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(worker.error()).c_str());
    return 1;
  }
  const apf::Status started = worker.value()->start();
  if (!started.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(started.error()).c_str());
    return 1;
  }
  std::printf("APF-WORKER-READY %s %s\n", worker.value()->id().str().c_str(),
              worker.value()->boot().hex().c_str());
  std::fflush(stdout);
  if (!ready_file.empty()) {
    const std::string announcement =
        worker.value()->id().str() + " " + worker.value()->boot().hex() + "\n";
    const apf::Status written = apf::write_file_atomic(ready_file, announcement);
    if (!written.ok()) {
      std::fprintf(stderr, "error: %s\n", apf::to_string(written.error()).c_str());
      return 1;
    }
  }
  const apf::Status served = worker.value()->serve_forever();
  if (!served.ok() && served.error().code != apf::ErrorCode::Closed) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(served.error()).c_str());
    return 1;
  }
  std::printf("APF-WORKER-STOPPED %s\n", worker.value()->boot().hex().c_str());
  return 0;
}
