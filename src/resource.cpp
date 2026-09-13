#include "apf/resource.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace apf {
namespace {

constexpr std::array<const char*, kResourceDimensionCount> kDimensionNames{{
    "compute_share",
    "memory_bytes",
    "memory_bandwidth_share",
    "execution_engine_share",
    "copy_engine_share",
    "media_engine_share",
    "cache_share",
    "dma_share",
}};

constexpr std::array<const char*, kResourceDimensionCount> kUnitNames{{
    "ppm",
    "bytes",
    "ppm",
    "ppm",
    "ppm",
    "ppm",
    "ppm",
    "ppm",
}};

bool is_share_index(std::size_t index) noexcept { return index != 1; }

std::string format_share(std::uint64_t value) {
  const std::uint64_t whole = value / 10'000ull;
  const std::uint64_t fraction = value % 10'000ull;
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%llu.%04llu %%",
                static_cast<unsigned long long>(whole),
                static_cast<unsigned long long>(fraction));
  return std::string(buffer);
}

std::string format_bytes(std::uint64_t value) {
  char buffer[64];
  if (value >= (1ull << 30)) {
    const std::uint64_t whole = value >> 30;
    const std::uint64_t fraction = ((value & ((1ull << 30) - 1)) * 100ull) >> 30;
    std::snprintf(buffer, sizeof(buffer), "%llu.%02llu GiB",
                  static_cast<unsigned long long>(whole),
                  static_cast<unsigned long long>(fraction));
    return std::string(buffer);
  }
  if (value >= (1ull << 20)) {
    const std::uint64_t whole = value >> 20;
    const std::uint64_t fraction = ((value & ((1ull << 20) - 1)) * 100ull) >> 20;
    std::snprintf(buffer, sizeof(buffer), "%llu.%02llu MiB",
                  static_cast<unsigned long long>(whole),
                  static_cast<unsigned long long>(fraction));
    return std::string(buffer);
  }
  std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(value));
  return std::string(buffer);
}

std::string format_dimension_value(std::size_t index, std::uint64_t value) {
  if (is_share_index(index)) {
    return format_share(value);
  }
  return format_bytes(value);
}

std::string number_to_string(std::uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return std::string(buffer);
}

}  // namespace

const char* to_string(ResourceDimension dimension) noexcept {
  const auto index = static_cast<std::size_t>(dimension);
  if (index >= kResourceDimensionCount) {
    return "unknown";
  }
  return kDimensionNames[index];
}

Result<ResourceDimension> resource_dimension_from_string(std::string_view name) {
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    if (name == kDimensionNames[index]) {
      return static_cast<ResourceDimension>(index);
    }
  }
  return make_error(ErrorCode::InvalidArgument, "unknown resource dimension",
                    std::string(name));
}

bool is_share_dimension(ResourceDimension dimension) noexcept {
  const auto index = static_cast<std::size_t>(dimension);
  if (index >= kResourceDimensionCount) {
    return false;
  }
  return is_share_index(index);
}

const char* unit_name(ResourceDimension dimension) noexcept {
  const auto index = static_cast<std::size_t>(dimension);
  if (index >= kResourceDimensionCount) {
    return "unknown";
  }
  return kUnitNames[index];
}

Status ResourceVector::set(ResourceDimension dimension, std::uint64_t value) {
  const auto index = static_cast<std::size_t>(dimension);
  if (index >= kResourceDimensionCount) {
    return failure(ErrorCode::InvalidArgument, "resource dimension out of range",
                   number_to_string(index));
  }
  if (is_share_dimension(dimension) && value > kShareScale) {
    return failure(ErrorCode::InvalidArgument,
                   "share exceeds the whole device and is not representable",
                   std::string(to_string(dimension)) + "=" + number_to_string(value));
  }
  values_[index] = value;
  mask_ |= (1u << static_cast<std::uint32_t>(index));
  return success();
}

void ResourceVector::clear(ResourceDimension dimension) noexcept {
  const auto index = static_cast<std::size_t>(dimension);
  if (index >= kResourceDimensionCount) {
    return;
  }
  values_[index] = 0;
  mask_ &= ~(1u << static_cast<std::uint32_t>(index));
}

std::size_t ResourceVector::dimension_count() const noexcept {
  std::size_t count = 0;
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    if ((mask_ & (1u << static_cast<std::uint32_t>(index))) != 0) {
      ++count;
    }
  }
  return count;
}

bool ResourceVector::is_subset_of(const ResourceVector& other) const noexcept {
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(index);
    if ((mask_ & bit) == 0) {
      continue;
    }
    if ((other.mask_ & bit) == 0) {
      return false;
    }
    if (values_[index] > other.values_[index]) {
      return false;
    }
  }
  return true;
}

Status ResourceVector::add(ResourceDimension dimension, std::uint64_t amount) {
  const auto index = static_cast<std::size_t>(dimension);
  if (index >= kResourceDimensionCount) {
    return failure(ErrorCode::InvalidArgument, "resource dimension out of range");
  }
  const std::uint32_t bit = 1u << static_cast<std::uint32_t>(index);
  if ((mask_ & bit) == 0) {
    return failure(ErrorCode::InvalidArgument, "dimension is not present in the vector",
                   to_string(dimension));
  }
  if (values_[index] > UINT64_MAX - amount) {
    return failure(ErrorCode::Overflow, "resource addition overflows",
                   to_string(dimension));
  }
  const std::uint64_t sum = values_[index] + amount;
  if (is_share_dimension(dimension) && sum > kShareScale) {
    return failure(ErrorCode::Overflow, "share addition exceeds the whole device",
                   to_string(dimension));
  }
  values_[index] = sum;
  return success();
}

Status ResourceVector::sub(ResourceDimension dimension, std::uint64_t amount) {
  const auto index = static_cast<std::size_t>(dimension);
  if (index >= kResourceDimensionCount) {
    return failure(ErrorCode::InvalidArgument, "resource dimension out of range");
  }
  const std::uint32_t bit = 1u << static_cast<std::uint32_t>(index);
  if ((mask_ & bit) == 0) {
    return failure(ErrorCode::InvalidArgument, "dimension is not present in the vector",
                   to_string(dimension));
  }
  if (values_[index] < amount) {
    return failure(ErrorCode::Underflow, "resource subtraction underflows",
                   std::string(to_string(dimension)) + "=" + number_to_string(values_[index]) +
                       " - " + number_to_string(amount));
  }
  values_[index] -= amount;
  return success();
}

Status ResourceVector::add_assign(const ResourceVector& other) {
  if ((other.mask_ & ~mask_) != 0) {
    return failure(ErrorCode::InvalidArgument,
                   "cannot add vectors with incompatible dimension sets");
  }
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(index);
    if ((other.mask_ & bit) == 0) {
      continue;
    }
    const Status status = add(static_cast<ResourceDimension>(index), other.values_[index]);
    if (!status.ok()) {
      return status;
    }
  }
  return success();
}

Status ResourceVector::sub_assign(const ResourceVector& other) {
  if ((other.mask_ & ~mask_) != 0) {
    return failure(ErrorCode::InvalidArgument,
                   "cannot subtract vectors with incompatible dimension sets");
  }
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(index);
    if ((other.mask_ & bit) == 0) {
      continue;
    }
    const Status status = sub(static_cast<ResourceDimension>(index), other.values_[index]);
    if (!status.ok()) {
      return status;
    }
  }
  return success();
}

ResourceVector ResourceVector::min_with(const ResourceVector& other) const noexcept {
  ResourceVector out;
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(index);
    if ((mask_ & bit) == 0) {
      continue;
    }
    const std::uint64_t other_value =
        (other.mask_ & bit) != 0 ? other.values_[index] : values_[index];
    out.values_[index] = std::min(values_[index], other_value);
    out.mask_ |= bit;
  }
  return out;
}

ResourceVector ResourceVector::saturating_sub(const ResourceVector& other) const noexcept {
  ResourceVector out;
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(index);
    if ((mask_ & bit) == 0) {
      continue;
    }
    const std::uint64_t other_value = (other.mask_ & bit) != 0 ? other.values_[index] : 0;
    out.values_[index] = values_[index] > other_value ? values_[index] - other_value : 0;
    out.mask_ |= bit;
  }
  return out;
}

Status ResourceVector::validate_dimensions() const {
  const std::uint32_t valid_mask =
      (kResourceDimensionCount >= 32) ? 0xFFFFFFFFu : ((1u << kResourceDimensionCount) - 1u);
  if ((mask_ & ~valid_mask) != 0) {
    return failure(ErrorCode::InvalidArgument, "resource vector carries undefined dimensions",
                   number_to_string(mask_));
  }
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    if ((mask_ & (1u << static_cast<std::uint32_t>(index))) == 0) {
      continue;
    }
    if (is_share_index(index) && values_[index] > kShareScale) {
      return failure(ErrorCode::InvalidArgument, "share dimension exceeds the whole device",
                     std::string(kDimensionNames[index]) + "=" + number_to_string(values_[index]));
    }
  }
  return success();
}

std::uint64_t ResourceVector::magnitude(ResourceDimension weighting) const noexcept {
  return get(weighting);
}

bool operator==(const ResourceVector& lhs, const ResourceVector& rhs) noexcept {
  if (lhs.mask_ != rhs.mask_) {
    return false;
  }
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    if ((lhs.mask_ & (1u << static_cast<std::uint32_t>(index))) == 0) {
      continue;
    }
    if (lhs.values_[index] != rhs.values_[index]) {
      return false;
    }
  }
  return true;
}

bool operator<(const ResourceVector& lhs, const ResourceVector& rhs) noexcept {
  if (lhs.mask_ != rhs.mask_) {
    return lhs.mask_ < rhs.mask_;
  }
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(index);
    if ((lhs.mask_ & bit) == 0) {
      continue;
    }
    if (lhs.values_[index] != rhs.values_[index]) {
      return lhs.values_[index] < rhs.values_[index];
    }
  }
  return false;
}

std::string ResourceVector::format() const {
  if (mask_ == 0) {
    return "{}";
  }
  std::string out = "{";
  bool first = true;
  for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(index);
    if ((mask_ & bit) == 0) {
      continue;
    }
    if (!first) {
      out += ", ";
    }
    first = false;
    out += kDimensionNames[index];
    out += "=";
    out += format_dimension_value(index, values_[index]);
  }
  out += "}";
  return out;
}

Result<ResourceVector> make_resource_vector(
    std::initializer_list<std::pair<ResourceDimension, std::uint64_t>> values) {
  ResourceVector vector;
  for (const auto& entry : values) {
    if (vector.has(entry.first)) {
      return make_error(ErrorCode::AlreadyExists, "duplicate resource dimension",
                        to_string(entry.first));
    }
    const Status status = vector.set(entry.first, entry.second);
    if (!status.ok()) {
      return status.error();
    }
  }
  return vector;
}

std::string format_quantity(ResourceDimension dimension, std::uint64_t value) {
  return format_dimension_value(static_cast<std::size_t>(dimension), value);
}

std::uint64_t mib(std::uint64_t count) noexcept { return count << 20; }

std::uint64_t gib(std::uint64_t count) noexcept { return count << 30; }

}  // namespace apf
