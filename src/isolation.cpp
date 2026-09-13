#include "apf/isolation.hpp"

#include <array>

namespace apf {
namespace {

constexpr std::array<const char*, kIsolationPropertyCount> kIsolationNames{{
    "logical_separation",
    "memory_isolation",
    "fault_isolation",
    "performance_isolation",
    "engine_isolation",
    "address_space_isolation",
    "dma_isolation",
    "tenant_isolation",
}};

}  // namespace

const char* to_string(IsolationProperty property) noexcept {
  const auto index = static_cast<std::size_t>(property);
  if (index >= kIsolationPropertyCount) {
    return "unknown";
  }
  return kIsolationNames[index];
}

Result<IsolationProperty> isolation_property_from_string(std::string_view name) {
  for (std::size_t index = 0; index < kIsolationPropertyCount; ++index) {
    if (name == kIsolationNames[index]) {
      return static_cast<IsolationProperty>(index);
    }
  }
  return make_error(ErrorCode::InvalidArgument, "unknown isolation property",
                    std::string(name));
}

bool is_security_relevant(IsolationProperty property) noexcept {
  switch (property) {
    case IsolationProperty::MemoryIsolation:
    case IsolationProperty::FaultIsolation:
    case IsolationProperty::AddressSpaceIsolation:
    case IsolationProperty::DmaIsolation:
    case IsolationProperty::TenantIsolation:
      return true;
    case IsolationProperty::LogicalSeparation:
    case IsolationProperty::PerformanceIsolation:
    case IsolationProperty::EngineIsolation:
    case IsolationProperty::Count:
      return false;
  }
  return false;
}

IsolationSet IsolationSet::all() noexcept {
  IsolationSet set;
  set.mask_ = (1u << static_cast<std::uint32_t>(IsolationProperty::Count)) - 1u;
  return set;
}

std::size_t IsolationSet::count() const noexcept {
  std::size_t total = 0;
  for (std::size_t index = 0; index < kIsolationPropertyCount; ++index) {
    if ((mask_ & (1u << static_cast<std::uint32_t>(index))) != 0) {
      ++total;
    }
  }
  return total;
}

std::string IsolationSet::format() const {
  if (mask_ == 0) {
    return "none";
  }
  std::string out;
  for (std::size_t index = 0; index < kIsolationPropertyCount; ++index) {
    if ((mask_ & (1u << static_cast<std::uint32_t>(index))) == 0) {
      continue;
    }
    if (!out.empty()) {
      out += "|";
    }
    out += kIsolationNames[index];
  }
  return out;
}

}  // namespace apf
