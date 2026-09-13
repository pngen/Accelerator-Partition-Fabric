#include "apf/profile.hpp"

#include "apf/codec.hpp"

#include <algorithm>



namespace apf {

namespace {

/// Text that reaches authoritative state must be well-formed: an invalid UTF-8
/// sequence is rejected rather than stored and later mangled by persistence or
/// the wire protocol.
Status check_text(const std::string& value, std::size_t max_bytes, const char* what) {
  if (value.empty()) {
    return success();
  }
  if (value.size() > max_bytes) {
    return failure(ErrorCode::LimitExceeded, std::string(what) + " is too long", value);
  }
  if (!is_valid_utf8(value)) {
    return failure(ErrorCode::InvalidArgument, std::string(what) + " is not valid UTF-8", value);
  }
  return success();
}

}  // namespace

const char* to_string(PartitionMechanism mechanism) noexcept {
  switch (mechanism) {
    case PartitionMechanism::None: return "none";
    case PartitionMechanism::Mig: return "mig";
    case PartitionMechanism::SrIov: return "sriov";
    case PartitionMechanism::MediatedDevice: return "mediated_device";
    case PartitionMechanism::VendorSlice: return "vendor_slice";
    case PartitionMechanism::Synthetic: return "synthetic";
    case PartitionMechanism::Count: return "count";
  }
  return "none";
}

Result<PartitionMechanism> partition_mechanism_from_string(std::string_view name) {
  if (name == "none") return PartitionMechanism::None;
  if (name == "mig") return PartitionMechanism::Mig;
  if (name == "sriov") return PartitionMechanism::SrIov;
  if (name == "mediated_device") return PartitionMechanism::MediatedDevice;
  if (name == "vendor_slice") return PartitionMechanism::VendorSlice;
  if (name == "synthetic") return PartitionMechanism::Synthetic;
  return make_error(ErrorCode::InvalidArgument, "unknown partition mechanism", std::string(name));
}

bool mechanism_is_physical(PartitionMechanism mechanism) noexcept {
  switch (mechanism) {
    case PartitionMechanism::Mig:
    case PartitionMechanism::SrIov:
    case PartitionMechanism::MediatedDevice:
    case PartitionMechanism::VendorSlice:
      return true;
    default:
      return false;
  }
}

const char* to_string(EngineGrouping grouping) noexcept {
  switch (grouping) {
    case EngineGrouping::None: return "none";
    case EngineGrouping::Shared: return "shared";
    case EngineGrouping::Dedicated: return "dedicated";
    case EngineGrouping::Count: return "count";
  }
  return "none";
}

Result<EngineGrouping> engine_grouping_from_string(std::string_view name) {
  if (name == "none") return EngineGrouping::None;
  if (name == "shared") return EngineGrouping::Shared;
  if (name == "dedicated") return EngineGrouping::Dedicated;
  return make_error(ErrorCode::InvalidArgument, "unknown engine grouping", std::string(name));
}

Status PartitionProfile::validate(const Limits& limits) const {
  if (!id.valid()) {
    return failure(ErrorCode::InvalidArgument, "profile identity is not set");
  }
  if (!generation.known()) {
    return failure(ErrorCode::InvalidArgument, "profile generation is not set");
  }
  if (name.empty() || name.size() > limits.max_name_bytes) {
    return failure(ErrorCode::InvalidArgument, "profile name is empty or too long", name);
  }
  Status text = check_text(name, limits.max_name_bytes, "profile name");
  if (!text.ok()) {
    return text;
  }
  text = check_text(vendor_native, limits.max_metadata_bytes, "vendor-native profile identity");
  if (!text.ok()) {
    return text;
  }
  text = check_text(backend, limits.max_metadata_bytes, "profile backend");
  if (!text.ok()) {
    return text;
  }
  text = check_text(description, limits.max_metadata_bytes, "profile description");
  if (!text.ok()) {
    return text;
  }
  if (backend.empty()) {
    return failure(ErrorCode::InvalidArgument, "profile does not name a backend", name);
  }
  if (vendor_native.size() > limits.max_metadata_bytes) {
    return failure(ErrorCode::LimitExceeded, "vendor-native profile identity is too long", name);
  }
  if (mechanism == PartitionMechanism::None || mechanism == PartitionMechanism::Count) {
    return failure(ErrorCode::InvalidArgument, "profile does not name a partition mechanism", name);
  }
  if (resources.empty()) {
    return failure(ErrorCode::InvalidArgument, "profile does not describe any resource", name);
  }
  const Status vector_status = resources.validate_dimensions();
  if (!vector_status.ok()) {
    return vector_status;
  }
  if (compute_slice_count == 0 && memory_slice_count == 0) {
    return failure(ErrorCode::InvalidArgument,
                   "profile describes neither compute nor memory slices", name);
  }
  if (alignment.compute_slice_multiple == 0 || alignment.memory_slice_multiple == 0) {
    return failure(ErrorCode::InvalidArgument, "profile alignment multiple must be non-zero", name);
  }
  if (mutually_exclusive_with.size() > limits.max_profile_exclusions) {
    return failure(ErrorCode::LimitExceeded, "profile exclusion list is too long", name);
  }
  for (std::size_t index = 0; index < mutually_exclusive_with.size(); ++index) {
    if (mutually_exclusive_with[index] == id) {
      return failure(ErrorCode::InvalidArgument, "profile excludes itself", name);
    }
    for (std::size_t other = index + 1; other < mutually_exclusive_with.size(); ++other) {
      if (mutually_exclusive_with[index] == mutually_exclusive_with[other]) {
        return failure(ErrorCode::AlreadyExists, "duplicate profile exclusion entry", name);
      }
    }
  }
  for (const std::string& requirement : backend_requirements) {
    if (requirement.size() > limits.max_metadata_bytes) {
      return failure(ErrorCode::LimitExceeded, "backend requirement string is too long", name);
    }
  }
  return success();
}

bool PartitionProfile::conflicts_with(const PartitionProfile& other) const noexcept {
  if (other.id == id) {
    return false;
  }
  const bool forward =
      std::find(mutually_exclusive_with.begin(), mutually_exclusive_with.end(), other.id) !=
      mutually_exclusive_with.end();
  const bool backward = std::find(other.mutually_exclusive_with.begin(),
                                  other.mutually_exclusive_with.end(),
                                  id) != other.mutually_exclusive_with.end();
  return forward || backward;
}

bool PartitionProfile::dominates(const PartitionProfile& other) const noexcept {
  if (other.resources.empty() || resources.empty()) {
    return false;
  }
  return other.resources.is_subset_of(resources);
}

}  // namespace apf