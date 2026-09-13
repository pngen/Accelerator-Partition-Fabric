#pragma once

#include <cstdint>
#include <string>

#define APF_VERSION_MAJOR 1
#define APF_VERSION_MINOR 0
#define APF_VERSION_PATCH 0

// Wire/persistence format version. Independent of the library semantic version.
#define APF_PERSISTENCE_FORMAT_VERSION 1u
#define APF_PROTOCOL_VERSION 1u

namespace apf {

struct Version {
  std::uint32_t major{APF_VERSION_MAJOR};
  std::uint32_t minor{APF_VERSION_MINOR};
  std::uint32_t patch{APF_VERSION_PATCH};
};

/// Semantic version of the runtime library.
Version version() noexcept;

/// "1.0.0"
const char* version_string() noexcept;

/// "Accelerator Partition Fabric 1.0.0 (persistence format 1, protocol 1)"
std::string version_banner();

}  // namespace apf
