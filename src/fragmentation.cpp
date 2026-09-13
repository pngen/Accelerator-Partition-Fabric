#include "apf/fragmentation.hpp"

namespace apf {

const char* to_string(FragmentationClass value) noexcept {
  switch (value) {
    case FragmentationClass::None: return "NONE";
    case FragmentationClass::ProfileShape: return "PROFILE_SHAPE";
    case FragmentationClass::SliceNotContiguous: return "SLICE_NOT_CONTIGUOUS";
    case FragmentationClass::EngineGroup: return "ENGINE_GROUP";
    case FragmentationClass::MemorySegment: return "MEMORY_SEGMENT";
    case FragmentationClass::MaxPartitionCount: return "MAX_PARTITION_COUNT";
    case FragmentationClass::IncompatibleCombination: return "INCOMPATIBLE_COMBINATION";
    case FragmentationClass::StrandedCapacity: return "STRANDED_CAPACITY";
    case FragmentationClass::TrappedBehindActive: return "TRAPPED_BEHIND_ACTIVE";
    case FragmentationClass::ReconfigurationRequired: return "RECONFIGURATION_REQUIRED";
    case FragmentationClass::ExceedsDeviceCapacity: return "EXCEEDS_DEVICE_CAPACITY";
    case FragmentationClass::Count: return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::string FragmentationReport::summary() const {
  std::string out = "fragmentation accelerator=";
  out += accelerator.valid() ? accelerator.str() : std::string("none");
  out += " profile=";
  out += profile.str();
  out += " requested=";
  out += std::to_string(requested_count);
  out += " classification=";
  out += to_string(classification);
  out += " feasible_now=";
  out += feasible_now ? "true" : "false";
  if (feasible_after_drain) {
    out += " feasible_after_drain=true";
  }
  if (feasible_after_reconfiguration) {
    out += " feasible_after_reconfiguration=true";
  }
  out += " max_instances_now=";
  out += std::to_string(max_instances_now);
  out += " free=";
  out += free_capacity.format();
  if (!stranded_capacity.empty()) {
    out += " stranded=";
    out += stranded_capacity.format();
  }
  if (!capacity_trapped_behind_active.empty()) {
    out += " trapped=";
    out += capacity_trapped_behind_active.format();
  }
  if (!blockers.empty()) {
    out += " blockers=[";
    for (std::size_t index = 0; index < blockers.size(); ++index) {
      if (index != 0) {
        out += ",";
      }
      out += blockers[index];
    }
    out += "]";
  }
  return out;
}

}  // namespace apf
