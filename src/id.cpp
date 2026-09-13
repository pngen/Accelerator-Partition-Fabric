#include "apf/id.hpp"

// The strong identity and generation templates are header-only. This
// translation unit exists to anchor the identity allocator's out-of-line
// behaviour and to keep a single definition point for future identity types.

namespace apf {
namespace {

[[maybe_unused]] constexpr bool identity_templates_are_instantiable() {
  return sizeof(AcceleratorId) > 0 && sizeof(PartitionGeneration) > 0 &&
         sizeof(WorkerBootId) > 0;
}

}  // namespace
}  // namespace apf
