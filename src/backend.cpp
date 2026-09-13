#include "apf/backend.hpp"

#include <algorithm>
#include <set>

namespace apf {

AcceleratorBackend::~AcceleratorBackend() = default;

Status validate_backend_accelerator(const BackendAccelerator& accelerator, const Limits& limits) {
  if (accelerator.stable_key.empty()) {
    return failure(ErrorCode::InvalidArgument, "backend reported an accelerator without a key");
  }
  if (accelerator.stable_key.size() > limits.max_metadata_bytes) {
    return failure(ErrorCode::LimitExceeded, "backend accelerator key is too long",
                   accelerator.stable_key);
  }
  if (accelerator.backend.empty() || accelerator.backend.size() > limits.max_metadata_bytes) {
    return failure(ErrorCode::InvalidArgument, "backend reported an accelerator without a backend");
  }
  if (accelerator.physical_totals.empty()) {
    return failure(ErrorCode::InvalidArgument,
                   "backend reported an accelerator without physical capacity",
                   accelerator.stable_key);
  }
  const Status dimensions = accelerator.physical_totals.validate_dimensions();
  if (!dimensions.ok()) {
    return dimensions;
  }
  if (accelerator.identifiers.model.size() > limits.max_metadata_bytes ||
      accelerator.identifiers.uuid.size() > limits.max_metadata_bytes ||
      accelerator.identifiers.vendor.size() > limits.max_metadata_bytes) {
    return failure(ErrorCode::LimitExceeded, "backend reported oversized device identifiers",
                   accelerator.stable_key);
  }
  if (accelerator.identifiers.uuid.empty() && accelerator.identifiers.pci_bus_id.empty() &&
      accelerator.stable_key.empty()) {
    return failure(ErrorCode::InvalidArgument,
                   "backend reported an accelerator with no stable identity",
                   accelerator.stable_key);
  }
  if (accelerator.provenance == EvidenceProvenance::Unknown) {
    return failure(ErrorCode::InvalidArgument,
                   "backend reported an accelerator without evidence provenance",
                   accelerator.stable_key);
  }
  if (accelerator.capability.generation.known()) {
    // The runtime has not assigned an accelerator identity yet; every other
    // capability field is validated here so that malformed backend output can
    // never reach authoritative state.
    const Status capability_status = accelerator.capability.validate(limits, false);
    if (!capability_status.ok()) {
      return capability_status;
    }
  } else if (accelerator.capability.support != PartitionSupportState::Unknown) {
    return failure(ErrorCode::InvalidArgument,
                   "backend published a capability determination without a generation",
                   accelerator.stable_key);
  }
  if (accelerator.capability.declares_supported() &&
      accelerator.provenance == EvidenceProvenance::Unsupported) {
    return failure(ErrorCode::InvalidArgument,
                   "backend claims partition support while labelling the evidence unsupported",
                   accelerator.stable_key);
  }
  if (accelerator.capability.declares_supported() && !accelerator.boot_id.valid()) {
    return failure(ErrorCode::InvalidArgument,
                   "backend claims partition support without a device incarnation identity",
                   accelerator.stable_key);
  }
  return success();
}

Status validate_backend_layout(const BackendLayout& layout, const Limits& limits) {
  if (layout.stable_key.empty()) {
    return failure(ErrorCode::InvalidArgument, "backend reported a layout without a device key");
  }
  if (layout.device_present) {
    if (layout.physical_totals.empty()) {
      return failure(ErrorCode::InvalidArgument,
                     "backend reported a present device without physical capacity",
                     layout.stable_key);
    }
    const Status dimensions = layout.physical_totals.validate_dimensions();
    if (!dimensions.ok()) {
      return dimensions;
    }
  }
  if (layout.partitions.size() > limits.max_synthetic_partitions &&
      layout.partitions.size() > limits.max_partitions) {
    return failure(ErrorCode::LimitExceeded, "backend reported too many partitions",
                   layout.stable_key);
  }
  std::set<std::string> native_ids;
  for (std::size_t index = 0; index < layout.partitions.size(); ++index) {
    const BackendNativePartition& partition = layout.partitions[index];
    if (partition.native_id.empty()) {
      return failure(ErrorCode::InvalidArgument, "backend reported a partition without a native id",
                     layout.stable_key);
    }
    if (partition.native_id.size() > limits.max_native_id_bytes) {
      return failure(ErrorCode::LimitExceeded, "native partition identity is too long",
                     layout.stable_key);
    }
    if (!native_ids.insert(partition.native_id).second) {
      return failure(ErrorCode::InvalidArgument,
                     "backend reported the same native partition identity twice",
                     partition.native_id);
    }
    if (partition.resources.empty()) {
      return failure(ErrorCode::InvalidArgument,
                     "backend reported a partition without resource ownership",
                     partition.native_id);
    }
    const Status dimensions = partition.resources.validate_dimensions();
    if (!dimensions.ok()) {
      return dimensions;
    }
  }
  return success();
}

}  // namespace apf
