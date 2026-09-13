#include "apf/request.hpp"

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

bool DeviceSelector::admits(const AcceleratorId& id) const noexcept {
  if (!allowed.empty() &&
      std::find(allowed.begin(), allowed.end(), id) == allowed.end()) {
    return false;
  }
  if (std::find(excluded.begin(), excluded.end(), id) != excluded.end()) {
    return false;
  }
  return true;
}

Status PartitionRequest::validate(const Limits& limits) const {
  if (!profile.valid()) {
    return failure(ErrorCode::InvalidRequest, "partition request does not name a profile");
  }
  if (count == 0) {
    return failure(ErrorCode::InvalidRequest, "partition request asks for zero partitions");
  }
  if (count > limits.max_partitions_per_accelerator) {
    return failure(ErrorCode::LimitExceeded,
                   "partition request exceeds the per-accelerator partition bound",
                   std::to_string(count));
  }
  if (count > limits.max_partitions) {
    return failure(ErrorCode::LimitExceeded, "partition request exceeds the global partition bound");
  }
  Status text = check_text(requester, limits.max_name_bytes, "requester string");
  if (!text.ok()) {
    return text;
  }
  text = check_text(request_tag, limits.max_metadata_bytes, "request tag");
  if (!text.ok()) {
    return text;
  }
  text = check_text(policy_class, limits.max_name_bytes, "policy class");
  if (!text.ok()) {
    return text;
  }
  if (request_tag.size() > limits.max_metadata_bytes) {
    return failure(ErrorCode::LimitExceeded, "request tag is too long");
  }
  if (policy_class.size() > limits.max_name_bytes) {
    return failure(ErrorCode::LimitExceeded, "policy class string is too long");
  }
  if (selector.backend.size() > limits.max_metadata_bytes ||
      selector.vendor.size() > limits.max_metadata_bytes ||
      selector.device_family.size() > limits.max_metadata_bytes ||
      selector.locality_domain.size() > limits.max_metadata_bytes) {
    return failure(ErrorCode::LimitExceeded, "device selector string is too long");
  }
  if (exclusive && !all_or_nothing) {
    return failure(ErrorCode::InvalidRequest,
                   "an exclusive request must be all-or-nothing");
  }
  return success();
}

Status WorkloadRequirement::validate(const Limits& limits) const {
  if (workload_id.empty()) {
    return failure(ErrorCode::InvalidRequest, "workload requirement does not name a workload");
  }
  Status text = check_text(workload_id, limits.max_name_bytes, "workload identity");
  if (!text.ok()) {
    return text;
  }
  text = check_text(tenant_id, limits.max_name_bytes, "tenant identity");
  if (!text.ok()) {
    return text;
  }
  if (requested_partition_count == 0) {
    return failure(ErrorCode::InvalidRequest, "workload requires zero partitions");
  }
  if (minimum_resources.empty() && !required_profile.valid()) {
    return failure(ErrorCode::InvalidRequest,
                   "workload requirement names neither a profile nor a resource minimum");
  }
  return success();
}

const char* to_string(AdmissionOutcome outcome) noexcept {
  switch (outcome) {
    case AdmissionOutcome::Admitted: return "ADMITTED";
    case AdmissionOutcome::RejectedNoCandidatePartition: return "REJECTED_NO_CANDIDATE_PARTITION";
    case AdmissionOutcome::RejectedInsufficientResources: return "REJECTED_INSUFFICIENT_RESOURCES";
    case AdmissionOutcome::RejectedIsolation: return "REJECTED_ISOLATION";
    case AdmissionOutcome::RejectedStaleAuthority: return "REJECTED_STALE_AUTHORITY";
    case AdmissionOutcome::RejectedPolicy: return "REJECTED_POLICY";
    case AdmissionOutcome::RejectedProfile: return "REJECTED_PROFILE";
    case AdmissionOutcome::RejectedDraining: return "REJECTED_DRAINING";
    case AdmissionOutcome::RejectedCapability: return "REJECTED_CAPABILITY";
    case AdmissionOutcome::RejectedExclusiveConflict: return "REJECTED_EXCLUSIVE_CONFLICT";
    case AdmissionOutcome::RejectedRevalidationRequired: return "REJECTED_REVALIDATION_REQUIRED";
    case AdmissionOutcome::RejectedDeviceUnavailable: return "REJECTED_DEVICE_UNAVAILABLE";
    case AdmissionOutcome::RejectedLimit: return "REJECTED_LIMIT";
    case AdmissionOutcome::RejectedInvalidRequest: return "REJECTED_INVALID_REQUEST";
    case AdmissionOutcome::Count: return "REJECTED_UNKNOWN";
  }
  return "REJECTED_UNKNOWN";
}

bool admission_admitted(AdmissionOutcome outcome) noexcept {
  return outcome == AdmissionOutcome::Admitted;
}

}  // namespace apf