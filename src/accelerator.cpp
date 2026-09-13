#include "apf/accelerator.hpp"

namespace apf {

Status AcceleratorRecord::validate(const Limits& limits) const {
  if (!id.valid()) {
    return failure(ErrorCode::InvalidArgument, "accelerator identity is not set");
  }
  if (!generation.known()) {
    return failure(ErrorCode::InvalidArgument, "accelerator generation is not set");
  }
  if (backend.empty()) {
    return failure(ErrorCode::InvalidArgument, "accelerator does not name a backend");
  }
  if (backend.size() > limits.max_metadata_bytes) {
    return failure(ErrorCode::LimitExceeded, "backend name is too long", backend);
  }
  if (physical_totals.empty()) {
    return failure(ErrorCode::InvalidArgument, "accelerator does not publish physical capacity",
                   id.str());
  }
  if (identifiers.vendor.size() > limits.max_metadata_bytes ||
      identifiers.model.size() > limits.max_metadata_bytes ||
      identifiers.uuid.size() > limits.max_metadata_bytes ||
      identifiers.pci_bus_id.size() > limits.max_metadata_bytes ||
      identifiers.hardware_id.size() > limits.max_metadata_bytes ||
      identifiers.firmware_version.size() > limits.max_metadata_bytes ||
      identifiers.driver_version.size() > limits.max_metadata_bytes ||
      identifiers.serial.size() > limits.max_metadata_bytes ||
      identifiers.compute_capability.size() > limits.max_metadata_bytes ||
      locality.name.size() > limits.max_metadata_bytes) {
    return failure(ErrorCode::LimitExceeded, "accelerator metadata string is too long", id.str());
  }
  if (unsupported_reason.size() > limits.max_metadata_bytes) {
    return failure(ErrorCode::LimitExceeded, "unsupported reason is too long", id.str());
  }
  if (capability.accelerator.valid() && capability.accelerator != id) {
    return failure(ErrorCode::InvalidArgument,
                   "capability refers to a different accelerator than the record",
                   id.str());
  }
  if (capability.generation.known()) {
    const Status capability_status = capability.validate(limits);
    if (!capability_status.ok()) {
      return capability_status;
    }
    if (capability.device_generation != generation) {
      return failure(ErrorCode::StaleGeneration,
                     "capability was published against a different device generation",
                     id.str());
    }
  }
  return success();
}

bool AcceleratorRecord::is_governable_at(std::uint64_t now_ms, bool require_fresh) const noexcept {
  if (!id.valid() || !generation.known() || physical_totals.empty()) {
    return false;
  }
  if (!evidence.is_observed()) {
    return false;
  }
  if (require_fresh && !evidence.is_fresh_at(now_ms)) {
    return false;
  }
  if (health.stamp.is_observed() && health.usable_at(now_ms)) {
    if (health.state == HealthState::Unhealthy) {
      return false;
    }
  }
  return true;
}

}  // namespace apf
