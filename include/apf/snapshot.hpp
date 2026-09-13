#pragma once

#include "apf/accounting.hpp"
#include "apf/accelerator.hpp"
#include "apf/evidence.hpp"
#include "apf/fragmentation.hpp"
#include "apf/id.hpp"
#include "apf/partition.hpp"
#include "apf/policy.hpp"
#include "apf/profile.hpp"
#include "apf/reservation.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apf {

/// A worker (or, in a single-process deployment, the in-process executor) that
/// owns physical mutation authority.
struct WorkerRecord {
  WorkerId id{};
  WorkerBootId boot{};
  std::string endpoint;
  std::string backend;
  bool alive{false};
  bool fenced{false};
  std::uint32_t device_count{0};
  std::uint64_t registered_at_ms{0};
  std::uint64_t last_seen_ms{0};
  std::uint64_t fenced_at_ms{0};
  std::string fence_reason;
  CoordinatorEpoch coordinator_epoch{};
};

/// Per-accelerator view with its ledger and partitions.
struct AcceleratorView {
  AcceleratorRecord accelerator{};
  CapacityLedger ledger{};
  EvidenceFreshness freshness{EvidenceFreshness::Unknown};
  std::vector<PartitionId> partitions;
  std::uint32_t active_partitions{0};
  std::uint32_t draining_partitions{0};
  std::uint32_t stale_partitions{0};
};

/// Immutable snapshot. Readers never hold an internal lock while inspecting a
/// snapshot: the snapshot owns its data.
class FabricSnapshot {
 public:
  FabricSnapshot() = default;

  SnapshotGeneration generation{};
  StateGeneration state_generation{};
  CoordinatorEpoch coordinator_epoch{};
  std::uint64_t created_at_ms{0};
  bool closed{false};
  std::string instance_id;

  std::vector<AcceleratorView> accelerators;
  std::vector<PartitionProfile> profiles;
  std::vector<PartitionRecord> partitions;
  std::vector<PartitionReservation> reservations;
  std::vector<PartitionAttempt> attempts;
  std::vector<PartitionAssignment> assignments;
  std::vector<WorkerRecord> workers;
  PlanningPolicy policy{};

  const AcceleratorView* find_accelerator(const AcceleratorId& id) const noexcept;
  const PartitionRecord* find_partition(const PartitionId& id) const noexcept;
  const PartitionProfile* find_profile(const PartitionProfileId& id) const noexcept;
  const PartitionReservation* find_reservation(const PartitionReservationId& id) const noexcept;
  const PartitionAttempt* find_attempt(const PartitionAttemptId& id) const noexcept;
  const WorkerRecord* find_worker(const WorkerId& id) const noexcept;

  std::vector<PartitionId> partitions_of(const AcceleratorId& accelerator) const;
  std::vector<PartitionAssignment> assignments_of(const PartitionId& partition) const;

  /// Canonical textual rendering used by the inspection tool and by
  /// determinism tests. Ordering is canonical, never map-iteration order.
  std::string render() const;
  /// The same rendering without the volatile header (snapshot generation and
  /// capture time). Two snapshots of the same logical state render identically.
  std::string render_logical() const;
};

/// Digest of a rendered snapshot. Two snapshots of identical logical state have
/// identical digests, which is what makes determinism provable rather than
/// asserted.
std::uint64_t snapshot_digest(const FabricSnapshot& snapshot) noexcept;

}  // namespace apf
