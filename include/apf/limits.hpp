#pragma once

#include "apf/result.hpp"

#include <cstddef>

namespace apf {

/// Hard resource discipline. No user-supplied declared size ever flows
/// directly into an allocation without being bounded here first.
struct Limits {
  std::size_t max_accelerators{256};
  std::size_t max_profiles{512};
  std::size_t max_partitions_per_accelerator{128};
  std::size_t max_partitions{4096};
  std::size_t max_assignments{16384};
  std::size_t max_reservations{4096};
  std::size_t max_active_reservations{256};
  std::size_t max_pending_plans{1024};
  std::size_t max_plan_steps{512};
  std::size_t max_pending_attempts{64};
  std::size_t max_attempt_history{4096};
  std::size_t max_snapshot_retention{64};
  std::size_t max_events{8192};
  std::size_t max_explanations{128};
  std::size_t max_reconciliation_findings{1024};
  std::size_t max_metadata_bytes{4096};
  std::size_t max_native_id_bytes{256};
  std::size_t max_name_bytes{256};
  std::size_t max_workers{64};
  std::size_t max_frame_bytes{1u << 20};
  std::size_t max_message_queue{4096};
  std::size_t max_synthetic_devices{64};
  std::size_t max_synthetic_partitions{512};
  std::size_t max_persisted_records{65536};
  std::size_t max_profile_exclusions{64};
  std::size_t max_retry_state{16};

  /// Rejects any configuration that is structurally unusable.
  Status validate() const;
};

/// Process-wide defaults used when a runtime instance is created without an
/// explicit Limits value.
const Limits& default_limits() noexcept;

}  // namespace apf
