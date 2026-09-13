#include "apf/accounting.hpp"

#include <string>

namespace apf {
namespace {

std::string describe_shortfall(const ResourceVector& from, const ResourceVector& amount) {
  std::string detail;
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const auto dimension = static_cast<ResourceDimension>(index);
    if (!amount.has(dimension)) {
      continue;
    }
    const std::uint64_t needed = amount.get(dimension);
    const std::uint64_t available = from.get(dimension);
    if (needed <= available) {
      continue;
    }
    if (!detail.empty()) {
      detail += ", ";
    }
    detail += std::string(to_string(dimension)) + " needs " + std::to_string(needed) + " has " +
              std::to_string(available);
  }
  return detail;
}

Status move_capacity(ResourceVector& from, ResourceVector& to, const ResourceVector& amount) {
  if (!amount.is_subset_of(from)) {
    return failure(ErrorCode::InsufficientCapacity,
                   "capacity is not available in the source bucket",
                   describe_shortfall(from, amount));
  }
  ResourceVector updated_from = from;
  ResourceVector updated_to = to;
  Status status = updated_from.sub_assign(amount);
  if (!status.ok()) {
    return status;
  }
  status = updated_to.add_assign(amount);
  if (!status.ok()) {
    return status;
  }
  from = updated_from;
  to = updated_to;
  return success();
}

}  // namespace

const char* to_string(CapacityBucket bucket) noexcept {
  switch (bucket) {
    case CapacityBucket::Unavailable: return "unavailable";
    case CapacityBucket::Free: return "free";
    case CapacityBucket::Reserved: return "reserved";
    case CapacityBucket::Active: return "active";
    case CapacityBucket::Draining: return "draining";
    case CapacityBucket::ReconfigurationHeld: return "reconfiguration_held";
    case CapacityBucket::Count: return "count";
  }
  return "unknown";
}

const ResourceVector& CapacityLedger::bucket(CapacityBucket bucket) const noexcept {
  static const ResourceVector kEmpty{};
  const auto index = static_cast<std::size_t>(bucket);
  if (index >= kCapacityBucketCount) {
    return kEmpty;
  }
  return buckets_[index];
}

Status CapacityLedger::initialize_totals(const ResourceVector& totals) {
  const Status dimensions = totals.validate_dimensions();
  if (!dimensions.ok()) {
    return dimensions;
  }
  if (totals.empty()) {
    return failure(ErrorCode::InvalidArgument, "physical totals are empty");
  }
  total_ = totals;
  for (ResourceVector& vector : buckets_) {
    vector = ResourceVector{};
  }
  // Every bucket carries the device's full dimension set, with zeros where the
  // bucket is empty. Dimension-incomplete buckets would make capacity
  // impossible to move between buckets, and would let a dimension silently
  // disappear from the accounting identity.
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const auto dimension = static_cast<ResourceDimension>(index);
    if (!totals.has(dimension)) {
      continue;
    }
    for (std::size_t bucket = 0; bucket < kCapacityBucketCount; ++bucket) {
      const Status status = buckets_[bucket].set(dimension, 0);
      if (!status.ok()) {
        return status;
      }
    }
    const Status status = buckets_[static_cast<std::size_t>(CapacityBucket::Free)].set(
        dimension, totals.get(dimension));
    if (!status.ok()) {
      return status;
    }
  }
  return validate();
}

Status CapacityLedger::reserve(const ResourceVector& amount) {
  if (amount.empty()) {
    return failure(ErrorCode::InvalidArgument, "reservation amount is empty");
  }
  return move_capacity(buckets_[static_cast<std::size_t>(CapacityBucket::Free)],
                       buckets_[static_cast<std::size_t>(CapacityBucket::Reserved)], amount);
}

Status CapacityLedger::reserve_from_reconfiguration_held(const ResourceVector& amount) {
  return move_capacity(
      buckets_[static_cast<std::size_t>(CapacityBucket::ReconfigurationHeld)],
      buckets_[static_cast<std::size_t>(CapacityBucket::Reserved)], amount);
}

Status CapacityLedger::commit_reserved(const ResourceVector& amount, bool draining) {
  return move_capacity(buckets_[static_cast<std::size_t>(CapacityBucket::Reserved)],
                       buckets_[static_cast<std::size_t>(draining
                                                             ? CapacityBucket::Draining
                                                             : CapacityBucket::Active)],
                       amount);
}

Status CapacityLedger::release_reserved(const ResourceVector& amount) {
  return move_capacity(buckets_[static_cast<std::size_t>(CapacityBucket::Reserved)],
                       buckets_[static_cast<std::size_t>(CapacityBucket::Free)], amount);
}

Status CapacityLedger::release_committed(const ResourceVector& amount, bool was_draining) {
  return move_capacity(buckets_[static_cast<std::size_t>(was_draining
                                                             ? CapacityBucket::Draining
                                                             : CapacityBucket::Active)],
                       buckets_[static_cast<std::size_t>(CapacityBucket::Free)], amount);
}

Status CapacityLedger::begin_drain(const ResourceVector& amount) {
  return move_capacity(buckets_[static_cast<std::size_t>(CapacityBucket::Active)],
                       buckets_[static_cast<std::size_t>(CapacityBucket::Draining)], amount);
}

Status CapacityLedger::cancel_drain(const ResourceVector& amount) {
  return move_capacity(buckets_[static_cast<std::size_t>(CapacityBucket::Draining)],
                       buckets_[static_cast<std::size_t>(CapacityBucket::Active)], amount);
}

Status CapacityLedger::hold_for_reconfiguration(const ResourceVector& amount) {
  return move_capacity(buckets_[static_cast<std::size_t>(CapacityBucket::Free)],
                       buckets_[static_cast<std::size_t>(CapacityBucket::ReconfigurationHeld)],
                       amount);
}

Status CapacityLedger::release_reconfiguration_hold(const ResourceVector& amount) {
  return move_capacity(
      buckets_[static_cast<std::size_t>(CapacityBucket::ReconfigurationHeld)],
      buckets_[static_cast<std::size_t>(CapacityBucket::Free)], amount);
}

Status CapacityLedger::move_between(CapacityBucket from, CapacityBucket to, const ResourceVector& amount) {
  const auto from_index = static_cast<std::size_t>(from);
  const auto to_index = static_cast<std::size_t>(to);
  if (from_index >= kCapacityBucketCount || to_index >= kCapacityBucketCount) {
    return failure(ErrorCode::InvalidArgument, "capacity bucket out of range");
  }
  if (from_index == to_index) {
    return success();
  }
  return move_capacity(buckets_[from_index], buckets_[to_index], amount);
}

Status CapacityLedger::mark_unavailable(const ResourceVector& amount) {
  return move_capacity(buckets_[static_cast<std::size_t>(CapacityBucket::Free)],
                       buckets_[static_cast<std::size_t>(CapacityBucket::Unavailable)], amount);
}

Status CapacityLedger::validate() const noexcept {
  if (total_.empty()) {
    return failure(ErrorCode::CorruptState, "ledger has no physical totals");
  }
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const auto dimension = static_cast<ResourceDimension>(index);
    std::uint64_t sum = 0;
    bool present_in_bucket = false;
    for (std::size_t bucket_index = 0; bucket_index < kCapacityBucketCount; ++bucket_index) {
      const ResourceVector& vector = buckets_[bucket_index];
      if (!vector.has(dimension)) {
        continue;
      }
      present_in_bucket = true;
      const std::uint64_t value = vector.get(dimension);
      if (sum > UINT64_MAX - value) {
        return failure(ErrorCode::Overflow, "ledger bucket sum overflows",
                       to_string(dimension));
      }
      sum += value;
    }
    if (!present_in_bucket) {
      continue;
    }
    if (!total_.has(dimension)) {
      return failure(ErrorCode::CorruptState,
                     "a bucket holds a dimension the device does not publish",
                     to_string(dimension));
    }
    if (sum != total_.get(dimension)) {
      return failure(ErrorCode::CorruptState, "capacity does not close for a dimension",
                     std::string(to_string(dimension)) + ": buckets=" + std::to_string(sum) +
                         " total=" + std::to_string(total_.get(dimension)));
    }
  }
  return success();
}

std::string CapacityLedger::format() const {
  std::string out = "total=" + total_.format();
  for (std::size_t index = 0; index < kCapacityBucketCount; ++index) {
    const ResourceVector& vector = buckets_[index];
    if (vector.empty()) {
      continue;
    }
    out += " ";
    out += to_string(static_cast<CapacityBucket>(index));
    out += "=";
    out += vector.format();
  }
  return out;
}

}  // namespace apf
