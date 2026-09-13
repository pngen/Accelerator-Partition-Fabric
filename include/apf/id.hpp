#pragma once

#include "apf/result.hpp"

#include <charconv>
#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

namespace apf {

/// Strongly typed identity. Two identities with different tags are distinct
/// types and cannot be compared, assigned, or passed interchangeably.
template <class Tag, class Repr = std::uint64_t>
class StrongId {
 public:
  using rep_type = Repr;
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Repr value) noexcept : value_(value) {}

  static constexpr StrongId from_value(Repr value) noexcept { return StrongId(value); }
  static constexpr StrongId invalid() noexcept { return StrongId(Repr{0}); }

  constexpr bool valid() const noexcept { return value_ != Repr{0}; }
  constexpr Repr value() const noexcept { return value_; }
  constexpr void reset() noexcept { value_ = Repr{0}; }

  std::string str() const { return std::to_string(value_); }

  static Result<StrongId> parse(std::string_view text) {
    if (text.empty()) {
      return make_error(ErrorCode::InvalidArgument, "empty identity text");
    }
    if constexpr (std::is_signed_v<Repr>) {
      if (text.front() == '-') {
        return make_error(ErrorCode::InvalidArgument, "negative identity is not legal",
                          std::string(text));
      }
    }
    Repr out{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const std::from_chars_result res = std::from_chars(begin, end, out);
    if (res.ec != std::errc{} || res.ptr != end) {
      return make_error(ErrorCode::InvalidArgument, "identity is not a canonical decimal integer",
                        std::string(text));
    }
    return StrongId(out);
  }

  friend constexpr bool operator==(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr auto operator<=>(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  Repr value_{};
};

/// Physical-incarnation identity: a boot/reset marker that changes whenever the
/// underlying device or process is reincarnated. Never reused across boots.
template <class Tag>
class BootId {
 public:
  using tag_type = Tag;

  constexpr BootId() noexcept = default;
  constexpr explicit BootId(std::uint64_t value) noexcept : value_(value) {}

  static constexpr BootId from_value(std::uint64_t value) noexcept { return BootId(value); }
  static constexpr BootId unknown() noexcept { return BootId(0); }

  /// Deterministic derivation from caller-supplied entropy (for example a
  /// process start tick plus a counter). Distinct entropy yields distinct
  /// identities with overwhelming probability.
  static BootId derive(std::uint64_t entropy) noexcept {
    std::uint64_t mixed = entropy + 0x9E3779B97F4A7C15ull;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
    mixed = mixed ^ (mixed >> 31);
    if (mixed == 0) {
      mixed = 0xA5A5A5A5A5A5A5A5ull;
    }
    return BootId(mixed);
  }

  constexpr bool valid() const noexcept { return value_ != 0; }
  constexpr std::uint64_t value() const noexcept { return value_; }

  std::string hex() const {
    static const char* digits = "0123456789abcdef";
    std::string out(16, '0');
    std::uint64_t remaining = value_;
    for (int i = 15; i >= 0; --i) {
      out[static_cast<std::size_t>(i)] = digits[remaining & 0xF];
      remaining >>= 4;
    }
    return out;
  }

  static Result<BootId> parse_hex(std::string_view text) {
    if (text.size() != 16) {
      return make_error(ErrorCode::InvalidArgument, "boot identity must be 16 hex digits",
                        std::string(text));
    }
    std::uint64_t out = 0;
    for (char ch : text) {
      out <<= 4;
      if (ch >= '0' && ch <= '9') {
        out |= static_cast<std::uint64_t>(ch - '0');
      } else if (ch >= 'a' && ch <= 'f') {
        out |= static_cast<std::uint64_t>(ch - 'a' + 10);
      } else if (ch >= 'A' && ch <= 'F') {
        out |= static_cast<std::uint64_t>(ch - 'A' + 10);
      } else {
        return make_error(ErrorCode::InvalidArgument, "boot identity has a non-hex digit",
                          std::string(text));
      }
    }
    return BootId(out);
  }

  friend constexpr bool operator==(BootId lhs, BootId rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr auto operator<=>(BootId lhs, BootId rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  std::uint64_t value_{0};
};

/// Monotonic generation counter. Zero means "no generation has ever been
/// established", which is materially different from generation 1.
template <class Tag>
class Generation {
 public:
  using tag_type = Tag;
  using rep_type = std::uint64_t;

  constexpr Generation() noexcept = default;
  constexpr explicit Generation(std::uint64_t value) noexcept : value_(value) {}

  static constexpr Generation from_value(std::uint64_t value) noexcept { return Generation(value); }
  static constexpr Generation unknown() noexcept { return Generation(0); }
  static constexpr Generation first() noexcept { return Generation(1); }

  constexpr bool known() const noexcept { return value_ != 0; }
  constexpr std::uint64_t value() const noexcept { return value_; }

  /// Checked successor. Overflow is rejected rather than wrapping, because a
  /// wrapped generation would resurrect fenced authority.
  Result<Generation> next() const {
    if (value_ == UINT64_MAX) {
      return make_error(ErrorCode::Overflow, "generation counter exhausted",
                        std::to_string(value_));
    }
    return Generation(value_ + 1);
  }

  friend constexpr bool operator==(Generation lhs, Generation rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr auto operator<=>(Generation lhs, Generation rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  std::uint64_t value_{0};
};

#define APF_DECLARE_ID(name)                       \
  struct name##Tag {};                             \
  using name = StrongId<name##Tag>

#define APF_DECLARE_BOOT_ID(name)                  \
  struct name##Tag {};                             \
  using name = BootId<name##Tag>

#define APF_DECLARE_GENERATION(name)               \
  struct name##Tag {};                             \
  using name = Generation<name##Tag>

// --- Logical identity -------------------------------------------------------
APF_DECLARE_ID(AcceleratorId);
APF_DECLARE_ID(PartitionId);
APF_DECLARE_ID(PartitionProfileId);
APF_DECLARE_ID(PartitionPlanId);
APF_DECLARE_ID(PartitionReservationId);
APF_DECLARE_ID(PartitionAssignmentId);
APF_DECLARE_ID(PartitionAttemptId);
APF_DECLARE_ID(IsolationPolicyId);
APF_DECLARE_ID(WorkerId);
APF_DECLARE_ID(ReconciliationId);

// --- Physical incarnation ---------------------------------------------------
APF_DECLARE_BOOT_ID(AcceleratorBootId);
APF_DECLARE_BOOT_ID(WorkerBootId);

// --- Generations ------------------------------------------------------------
APF_DECLARE_GENERATION(AcceleratorGeneration);
APF_DECLARE_GENERATION(PartitionGeneration);
APF_DECLARE_GENERATION(PartitionProfileGeneration);
APF_DECLARE_GENERATION(PartitionPlanGeneration);
APF_DECLARE_GENERATION(CapabilityGeneration);
APF_DECLARE_GENERATION(TopologyGeneration);
APF_DECLARE_GENERATION(PolicyGeneration);
APF_DECLARE_GENERATION(IsolationPolicyGeneration);
APF_DECLARE_GENERATION(CoordinatorEpoch);
APF_DECLARE_GENERATION(EvidenceGeneration);
APF_DECLARE_GENERATION(SnapshotGeneration);
APF_DECLARE_GENERATION(StateGeneration);

#undef APF_DECLARE_ID
#undef APF_DECLARE_BOOT_ID
#undef APF_DECLARE_GENERATION

template <class Id>
struct StrongIdHash {
  std::size_t operator()(const Id& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

/// Monotonic identity allocator. Zero is never handed out.
class IdAllocator {
 public:
  IdAllocator() = default;

  template <class Id>
  Result<Id> allocate() {
    if (next_ == UINT64_MAX) {
      return make_error(ErrorCode::Overflow, "identity space exhausted");
    }
    return Id::from_value(next_++);
  }

  template <class Id>
  Id peek_next() const noexcept {
    return Id::from_value(next_);
  }

  /// Restores the allocator after a durable reload. The counter only ever
  /// moves forward; a lower persisted value is a generation regression.
  Status restore_floor(std::uint64_t persisted_next) {
    if (persisted_next < next_) {
      return make_error(ErrorCode::GenerationRegression,
                        "persisted identity counter is behind the live allocator",
                        std::to_string(persisted_next) + " < " + std::to_string(next_));
    }
    next_ = persisted_next;
    return success();
  }

  std::uint64_t raw() const noexcept { return next_; }

 private:
  std::uint64_t next_{1};
};

}  // namespace apf
