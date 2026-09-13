#pragma once

#include "apf/accelerator.hpp"
#include "apf/accounting.hpp"
#include "apf/id.hpp"
#include "apf/partition.hpp"
#include "apf/policy.hpp"
#include "apf/profile.hpp"
#include "apf/reservation.hpp"
#include "apf/result.hpp"
#include "apf/snapshot.hpp"
#include "apf/version.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace apf {

/// Durable structural state. Dynamic hardware evidence is persisted only so
/// that it can be classified on reload; it never becomes current again merely
/// because it survived a restart.
struct DurableState {
  std::uint32_t format_version{APF_PERSISTENCE_FORMAT_VERSION};
  std::string instance_id;
  StateGeneration state_generation{};
  CoordinatorEpoch coordinator_epoch{};
  PolicyGeneration policy_generation{};
  std::uint64_t next_identity{1};
  std::uint64_t saved_at_ms{0};
  PlanningPolicy policy{};
  std::vector<PartitionProfile> profiles;
  std::vector<AcceleratorRecord> accelerators;
  /// accelerator id -> backend stable key
  std::vector<std::pair<AcceleratorId, std::string>> accelerator_keys;
  std::vector<std::pair<AcceleratorId, CapacityLedger>> ledgers;
  std::vector<PartitionRecord> partitions;
  std::vector<PartitionReservation> reservations;
  std::vector<PartitionAttempt> attempts;
  std::vector<PartitionAssignment> assignments;
  std::vector<WorkerRecord> workers;
};

/// Versioned, integrity-checked, atomically-replaced durable store.
class PersistenceStore {
 public:
  PersistenceStore() = default;

  /// Configures the authoritative path. Path handling is validated: empty
  /// paths, paths whose parent is a non-directory, and path traversal outside
  /// the configured root are rejected.
  Status set_path(std::string path);

  const std::string& path() const noexcept { return path_; }

  /// Encodes the state into the versioned binary container.
  static Result<std::vector<std::uint8_t>> encode(const DurableState& state,
                                                  const Limits& limits);

  /// Decodes and fully validates a container. Corrupt, truncated, oversized,
  /// duplicate, impossible, or internally inconsistent state is rejected
  /// without partially applying anything.
  static Result<DurableState> decode(const std::uint8_t* bytes, std::size_t size,
                                     const Limits& limits);

  /// write temporary -> flush -> verify -> replace authoritative file.
  Status save(const DurableState& state, const Limits& limits);

  /// Loads the authoritative file. Returns NotFound when the file does not
  /// exist yet, which is not an error condition.
  Result<DurableState> load(const Limits& limits) const;

  /// Removes the authoritative file and any stale temporary sibling.
  Status remove();

 private:
  std::string path_;
};

/// Computes the canonical integrity hash used by the persistence container and
/// by the wire protocol. FNV-1a 64 over the byte range.
std::uint64_t integrity_hash(const std::uint8_t* data, std::size_t size) noexcept;
std::uint64_t integrity_hash(std::string_view data) noexcept;

}  // namespace apf
