#pragma once

#include "apf/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace apf {

/// Portable capacity dimensions. A dimension is only ever published by a
/// backend that can make a meaningful claim about it; absent dimensions stay
/// absent instead of being defaulted to zero.
enum class ResourceDimension : std::uint8_t {
  /// Fraction of the physical device's compute partitioning, in parts per million.
  ComputeShare = 0,
  /// Device memory capacity in bytes.
  MemoryBytes = 1,
  /// Memory bandwidth share in parts per million, only where the backend can
  /// substantiate the claim for the partition mechanism in use.
  MemoryBandwidthShare = 2,
  /// Execution-engine (streaming multiprocessor / compute unit) share in ppm.
  ExecutionEngineShare = 3,
  /// Copy/DMA engine share in ppm, where engines are separately partitionable.
  CopyEngineShare = 4,
  /// Media/codec engine share in ppm, only where real hardware exposes it.
  MediaEngineShare = 5,
  /// Cache or L2 slice ownership share in ppm, only where authoritatively exposed.
  CacheShare = 6,
  /// DMA/IOMMU isolation resource share in ppm, only where the backend exposes it.
  DmaShare = 7,
  Count = 8,
};

inline constexpr std::size_t kResourceDimensionCount =
    static_cast<std::size_t>(ResourceDimension::Count);

/// Parts-per-million denominator for every share dimension.
inline constexpr std::uint64_t kShareScale = 1'000'000ull;

const char* to_string(ResourceDimension dimension) noexcept;
Result<ResourceDimension> resource_dimension_from_string(std::string_view name);

/// True when the dimension is a ppm share rather than a byte/count quantity.
bool is_share_dimension(ResourceDimension dimension) noexcept;

/// Unit suffix used by the inspection tooling.
const char* unit_name(ResourceDimension dimension) noexcept;

/// A sparse vector of capacity values over the portable dimensions. Values are
/// never added across incompatible dimensions: every arithmetic operation is
/// per-dimension and checked.
class ResourceVector {
 public:
  constexpr ResourceVector() noexcept = default;

  static constexpr ResourceVector zeros() noexcept { return ResourceVector{}; }

  constexpr bool has(ResourceDimension dimension) const noexcept {
    return (mask_ & bit(dimension)) != 0;
  }
  constexpr std::uint64_t get(ResourceDimension dimension) const noexcept {
    return has(dimension) ? values_[index(dimension)] : 0;
  }
  constexpr std::optional<std::uint64_t> try_get(ResourceDimension dimension) const noexcept {
    if (!has(dimension)) {
      return std::nullopt;
    }
    return values_[index(dimension)];
  }

  /// Sets a dimension, making it present. A share value above the ppm scale is
  /// rejected: capability that claims more than the whole device is not
  /// representable.
  Status set(ResourceDimension dimension, std::uint64_t value);
  void clear(ResourceDimension dimension) noexcept;

  /// Applies a default of zero for a dimension that is not present.
  void set_if_absent(ResourceDimension dimension, std::uint64_t value) {
    if (!has(dimension)) {
      (void)set(dimension, value);
    }
  }

  std::uint32_t mask() const noexcept { return mask_; }
  void set_mask(std::uint32_t mask) noexcept { mask_ = mask; }
  const std::array<std::uint64_t, kResourceDimensionCount>& raw() const noexcept {
    return values_;
  }
  /// Mutable access used by the codec, which must fill the vector before it can
  /// validate it.
  std::array<std::uint64_t, kResourceDimensionCount>& mutable_raw() noexcept {
    return values_;
  }

  bool empty() const noexcept { return mask_ == 0; }
  std::size_t dimension_count() const noexcept;

  /// Every present dimension of *this is present in *other and no larger.
  bool is_subset_of(const ResourceVector& other) const noexcept;
  /// Present dimension sets are identical.
  bool same_dimensions(const ResourceVector& other) const noexcept {
    return mask_ == other.mask_;
  }

  Status add(ResourceDimension dimension, std::uint64_t amount);
  Status sub(ResourceDimension dimension, std::uint64_t amount);
  Status add_assign(const ResourceVector& other);
  Status sub_assign(const ResourceVector& other);

  /// Element-wise minimum over the union of present dimensions.
  ResourceVector min_with(const ResourceVector& other) const noexcept;
  /// Element-wise difference, saturating at zero, over this vector's dimensions.
  ResourceVector saturating_sub(const ResourceVector& other) const noexcept;

  std::uint64_t magnitude(ResourceDimension weighting) const noexcept;

  /// Validates every present dimension: no bits outside the defined dimension
  /// range, and every share within the whole-device scale. Used by the codec,
  /// which must reject out-of-range values arriving from disk or from the wire.
  Status validate_dimensions() const;

  friend bool operator==(const ResourceVector& lhs, const ResourceVector& rhs) noexcept;
  /// Deterministic total order: present-dimension mask first, then values.
  friend bool operator<(const ResourceVector& lhs, const ResourceVector& rhs) noexcept;

  std::string format() const;

 private:
  static constexpr std::size_t index(ResourceDimension dimension) noexcept {
    return static_cast<std::size_t>(dimension);
  }
  static constexpr std::uint32_t bit(ResourceDimension dimension) noexcept {
    return 1u << static_cast<std::uint32_t>(dimension);
  }

  std::array<std::uint64_t, kResourceDimensionCount> values_{};
  std::uint32_t mask_{0};
};

/// Builds a vector from a list of (dimension, value) pairs, rejecting
/// duplicates and out-of-range shares.
Result<ResourceVector> make_resource_vector(
    std::initializer_list<std::pair<ResourceDimension, std::uint64_t>> values);

/// "6.00 GiB", "50.00 %", "1024 MiB/s"
std::string format_quantity(ResourceDimension dimension, std::uint64_t value);

std::uint64_t mib(std::uint64_t count) noexcept;
std::uint64_t gib(std::uint64_t count) noexcept;

}  // namespace apf
