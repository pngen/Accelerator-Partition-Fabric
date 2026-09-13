#include "apf/version.hpp"

namespace apf {

Version version() noexcept { return Version{}; }

const char* version_string() noexcept { return "1.0.0"; }

std::string version_banner() {
  return std::string("Accelerator Partition Fabric ") + version_string() +
         " (persistence format " + std::to_string(APF_PERSISTENCE_FORMAT_VERSION) +
         ", protocol " + std::to_string(APF_PROTOCOL_VERSION) + ")";
}

}  // namespace apf
