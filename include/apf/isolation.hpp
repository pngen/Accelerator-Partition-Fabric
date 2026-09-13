#pragma once

#include "apf/id.hpp"
#include "apf/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace apf {

/// Isolation properties that a partition mechanism may or may not actually
/// provide. Nothing here is inferred from a vendor family name: a backend must
/// publish a property explicitly before policy may rely on it.
enum class IsolationProperty : std::uint8_t {
  /// Scheduling separation only: the runtime treats the partitions as distinct
  /// scheduling units. This is the weakest claim and is not a security claim.
  LogicalSeparation = 0,
  MemoryIsolation = 1,
  FaultIsolation = 2,
  PerformanceIsolation = 3,
  EngineIsolation = 4,
  AddressSpaceIsolation = 5,
  DmaIsolation = 6,
  TenantIsolation = 7,
  Count = 8,
};

inline constexpr std::size_t kIsolationPropertyCount =
    static_cast<std::size_t>(IsolationProperty::Count);

const char* to_string(IsolationProperty property) noexcept;
Result<IsolationProperty> isolation_property_from_string(std::string_view name);
bool is_security_relevant(IsolationProperty property) noexcept;

/// A set of isolation properties with conservative semantics: an absent
/// property is not claimed, and an unknown backend publishes an empty set.
class IsolationSet {
 public:
  constexpr IsolationSet() noexcept = default;

  static IsolationSet none() noexcept { return IsolationSet{}; }
  static IsolationSet all() noexcept;

  constexpr bool has(IsolationProperty property) const noexcept {
    return (mask_ & bit(property)) != 0;
  }
  constexpr bool empty() const noexcept { return mask_ == 0; }
  constexpr std::uint32_t mask() const noexcept { return mask_; }
  constexpr void set_mask(std::uint32_t mask) noexcept { mask_ = mask; }

  void add(IsolationProperty property) noexcept { mask_ |= bit(property); }
  void remove(IsolationProperty property) noexcept { mask_ &= ~bit(property); }

  /// Every property of *this is present in *other.
  constexpr bool is_subset_of(const IsolationSet& other) const noexcept {
    return (mask_ & ~other.mask_) == 0;
  }
  constexpr IsolationSet operator|(const IsolationSet& other) const noexcept {
    IsolationSet out;
    out.mask_ = mask_ | other.mask_;
    return out;
  }
  constexpr IsolationSet operator&(const IsolationSet& other) const noexcept {
    IsolationSet out;
    out.mask_ = mask_ & other.mask_;
    return out;
  }
  friend constexpr bool operator==(const IsolationSet& lhs, const IsolationSet& rhs) noexcept {
    return lhs.mask_ == rhs.mask_;
  }
  friend constexpr auto operator<=>(const IsolationSet& lhs, const IsolationSet& rhs) noexcept {
    return lhs.mask_ <=> rhs.mask_;
  }

  std::size_t count() const noexcept;
  std::string format() const;

 private:
  static constexpr std::uint32_t bit(IsolationProperty property) noexcept {
    return 1u << static_cast<std::uint32_t>(property);
  }
  std::uint32_t mask_{0};
};

/// What a policy demands before a partition may host a workload.
struct IsolationRequirement {
  IsolationSet required{};
  IsolationPolicyId policy{};
  IsolationPolicyGeneration policy_generation{};

  bool satisfied_by(const IsolationSet& available) const noexcept {
    return required.is_subset_of(available);
  }
};

/// The isolation domain a partition actually belongs to, as reported by the
/// backend. An empty domain name means the backend did not publish one.
struct IsolationDomain {
  std::string name;
  IsolationSet guarantees{};
  bool backend_defined{false};

  bool empty() const noexcept { return name.empty() && guarantees.empty(); }
};

}  // namespace apf
