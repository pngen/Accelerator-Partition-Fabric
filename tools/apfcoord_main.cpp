// apfcoord: the distributed coordinator process.
//
// Owns logical authority: capability publication, planning, reservations,
// lifecycle, accounting and generation binding. Physical mutation is delegated
// to worker incarnations over the framed protocol.

#include "apf/coordinator.hpp"
#include "apf/process.hpp"
#include "apf/version.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void usage() {
  std::printf(
      "Accelerator Partition Fabric coordinator\n"
      "\n"
      "  apfcoord --listen <host:port> [options]\n"
      "\n"
      "Options:\n"
      "  --listen <host:port>   listening endpoint (port 0 chooses a free port)\n"
      "  --state <file>         durable state file (implies --persist)\n"
      "  --ready-file <file>    write the bound endpoint and epoch once serving\n"
      "  --instance <name>      instance identity reported in snapshots\n"
      "  --max-workers N        worker bound\n"
      "\n"
      "Prints APF-COORDINATOR-READY <endpoint> <epoch> once it is serving, then\n"
      "serves the framed protocol until a shutdown command arrives.\n");
}

}  // namespace

int main(int argc, char** argv) {
  apf::CoordinatorOptions options;
  std::string endpoint_text = "127.0.0.1:0";
  std::string ready_file;
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
    if (index + 1 >= argc) {
      std::fprintf(stderr, "error: option %s requires a value\n", argument.c_str());
      return 2;
    }
    const std::string value = argv[++index];
    if (argument == "--listen") {
      endpoint_text = value;
    } else if (argument == "--state") {
      options.state_path = value;
      options.persist = true;
    } else if (argument == "--ready-file") {
      ready_file = value;
    } else if (argument == "--instance") {
      options.fabric.instance_id = value;
    } else if (argument == "--max-workers") {
      options.limits.max_workers = static_cast<std::size_t>(std::stoull(value));
    } else {
      std::fprintf(stderr, "error: unknown option %s\n", argument.c_str());
      return 2;
    }
  }
  const apf::Result<apf::Endpoint> endpoint = apf::Endpoint::parse(endpoint_text);
  if (!endpoint.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(endpoint.error()).c_str());
    return 2;
  }
  options.listen = endpoint.value();
  if (options.fabric.instance_id.empty()) {
    options.fabric.instance_id = "apf-coordinator";
  }
  if (!options.persist && !options.state_path.empty()) {
    options.persist = true;
  }
  const apf::Result<std::unique_ptr<apf::PartitionCoordinator>> coordinator =
      apf::PartitionCoordinator::create(options);
  if (!coordinator.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(coordinator.error()).c_str());
    return 1;
  }
  const apf::Status started = coordinator.value()->start();
  if (!started.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(started.error()).c_str());
    return 1;
  }
  std::printf("APF-COORDINATOR-READY %s %llu\n",
              coordinator.value()->endpoint().to_string().c_str(),
              static_cast<unsigned long long>(coordinator.value()->epoch().value()));
  std::fflush(stdout);
  if (!ready_file.empty()) {
    const std::string announcement = coordinator.value()->endpoint().to_string() + " " +
                                     std::to_string(coordinator.value()->epoch().value()) + "\n";
    const apf::Status written = apf::write_file_atomic(ready_file, announcement);
    if (!written.ok()) {
      std::fprintf(stderr, "error: %s\n", apf::to_string(written.error()).c_str());
      return 1;
    }
  }
  const apf::Status served = coordinator.value()->serve_forever();
  if (!served.ok()) {
    std::fprintf(stderr, "error: %s\n", apf::to_string(served.error()).c_str());
    return 1;
  }
  std::printf("APF-COORDINATOR-STOPPED %s\n", coordinator.value()->endpoint().to_string().c_str());
  return 0;
}
