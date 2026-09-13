#include "apf/limits.hpp"

namespace apf {

Status Limits::validate() const {
  if (max_accelerators == 0 || max_profiles == 0 || max_partitions == 0) {
    return failure(ErrorCode::InvalidArgument, "limits must allow at least one entity");
  }
  if (max_partitions_per_accelerator > max_partitions) {
    return failure(ErrorCode::InvalidArgument,
                   "per-accelerator partition bound exceeds the global bound");
  }
  if (max_frame_bytes < 64 || max_frame_bytes > (64u << 20)) {
    return failure(ErrorCode::InvalidArgument, "frame bound is outside the supported range",
                   std::to_string(max_frame_bytes));
  }
  if (max_metadata_bytes == 0 || max_metadata_bytes > (1u << 20)) {
    return failure(ErrorCode::InvalidArgument, "metadata bound is outside the supported range",
                   std::to_string(max_metadata_bytes));
  }
  if (max_native_id_bytes == 0 || max_native_id_bytes > max_metadata_bytes) {
    return failure(ErrorCode::InvalidArgument, "native identity bound is not usable",
                   std::to_string(max_native_id_bytes));
  }
  if (max_name_bytes == 0 || max_name_bytes > max_metadata_bytes) {
    return failure(ErrorCode::InvalidArgument, "name bound is not usable");
  }
  if (max_pending_attempts == 0 || max_pending_attempts > max_attempt_history) {
    return failure(ErrorCode::InvalidArgument, "pending attempt bound exceeds attempt history");
  }
  if (max_plan_steps == 0 || max_explanations == 0 || max_events == 0) {
    return failure(ErrorCode::InvalidArgument, "explanatory bounds must be non-zero");
  }
  if (max_synthetic_devices == 0 || max_synthetic_partitions == 0) {
    return failure(ErrorCode::InvalidArgument, "synthetic bounds must be non-zero");
  }
  if (max_workers == 0 || max_message_queue == 0) {
    return failure(ErrorCode::InvalidArgument, "distributed bounds must be non-zero");
  }
  if (max_persisted_records < 16) {
    return failure(ErrorCode::InvalidArgument, "persistence record bound is too small");
  }
  return success();
}

const Limits& default_limits() noexcept {
  static const Limits limits{};
  return limits;
}

}  // namespace apf
