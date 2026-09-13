#pragma once

#include "apf/id.hpp"
#include "apf/resource.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace apf {

/// Capacity buckets. The exact invariant maintained for every governed
/// dimension is:
///
///   physical total = unavailable + free + reserved + active + draining
///                    + reconfiguration_held
///
/// adapted per device model. Dimensions are never summed across incompatible
/// resource classes.
enum class CapacityBucket : std::uint8_t {
  /// Reserved by the device/driver and never partitionable (for example ECC
  /// reserved memory or fixed firmware overhead).
  Unavailable = 0,
  /// Not currently assigned to any partition.
  Free = 1,
  /// Held by an accepted reservation whose physical mutation has not committed.
  Reserved = 2,
  /// Owned by an authoritative active partition.
  Active = 3,
  /// Owned by a draining partition: still honoured, no longer admittable.
  Draining = 4,
  /// Held across a destructive reconfiguration transition.
  ReconfigurationHeld = 5,
  Count = 6,
};

inline constexpr std::size_t kCapacityBucketCount = static_cast<std::size_t>(CapacityBucket::Count);

const char* to_string(CapacityBucket bucket) noexcept;

/// Exact, closed capacity accounting for one physical accelerator.
/// Every mutation is applied to a copy and swapped in only when the whole
/// transaction succeeded, so accounting always returns to a valid state.
class CapacityLedger {
 public:
  CapacityLedger() = default;

  const ResourceVector& total() const noexcept { return total_; }
  const ResourceVector& bucket(CapacityBucket bucket) const noexcept;
  std::array<ResourceVector, kCapacityBucketCount>& mutable_buckets() noexcept { return buckets_; }
  const std::array<ResourceVector, kCapacityBucketCount>& buckets() const noexcept {
    return buckets_;
  }

  /// Establishes physical totals. All dimensions become present; free is set to
  /// total (minus anything already recorded unavailable) and all other buckets
  /// are zeroed. Returns Overflow when a total would exceed a share scale.
  Status initialize_totals(const ResourceVector& totals);

  /// Reserves amount from free into reserved. Fails with InsufficientCapacity
  /// when free is short in any dimension, and with InvalidArgument when the
  /// amount carries a dimension the device does not publish.
  Status reserve(const ResourceVector& amount);
  /// reconfiguration_held -> reserved (or back) during a destructive transition.
  Status reserve_from_reconfiguration_held(const ResourceVector& amount);
  /// Consumes reserved capacity into an active/draining partition.
  Status commit_reserved(const ResourceVector& amount, bool draining);
  /// Releases reserved capacity back to free exactly once.
  Status release_reserved(const ResourceVector& amount);
  /// Active/draining -> free when a partition is destroyed.
  Status release_committed(const ResourceVector& amount, bool was_draining);
  /// Active -> draining.
  Status begin_drain(const ResourceVector& amount);
  /// Draining -> active (drain cancelled).
  Status cancel_drain(const ResourceVector& amount);
  /// free -> reconfiguration_held before a destructive layout change.
  Status hold_for_reconfiguration(const ResourceVector& amount);
  /// reconfiguration_held -> free after a verified reconfiguration.
  Status release_reconfiguration_hold(const ResourceVector& amount);
  /// Moves capacity between two non-free buckets. Used for exact lifecycle
  /// transitions where the partition record names the bucket that holds it.
  Status move_between(CapacityBucket from, CapacityBucket to, const ResourceVector& amount);

  /// Marks capacity as device-unavailable, taking it from free.
  Status mark_unavailable(const ResourceVector& amount);

  /// True when every dimension closes exactly.
  Status validate() const noexcept;

  std::uint64_t free_of(ResourceDimension dimension) const noexcept {
    return bucket(CapacityBucket::Free).get(dimension);
  }

  std::string format() const;

 private:
  std::array<ResourceVector, kCapacityBucketCount> buckets_{};
  ResourceVector total_;
};

/// Ledger plus the identity it belongs to.
struct LedgerSnapshot {
  AcceleratorId accelerator{};
  AcceleratorGeneration generation{};
  CapacityLedger ledger{};
};

}  // namespace apf
