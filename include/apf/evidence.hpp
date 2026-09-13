#pragma once

#include "apf/id.hpp"

#include <cstdint>
#include <string>

namespace apf {

/// Where a claim came from. Every hardware-facing statement in the runtime
/// carries one of these, and no code path may silently upgrade one to another.
enum class EvidenceProvenance : std::uint8_t {
  Unknown = 0,
  /// Observed from real hardware through a real backend.
  Real = 1,
  /// Produced by the deterministic synthetic backend.
  Synthetic = 2,
  /// The backend positively determined that the capability does not exist.
  Unsupported = 3,
};

const char* to_string(EvidenceProvenance provenance) noexcept;
EvidenceProvenance evidence_provenance_from_string(std::string_view name);

enum class EvidenceFreshness : std::uint8_t {
  Unknown = 0,
  Fresh = 1,
  Stale = 2,
  Expired = 3,
};

const char* to_string(EvidenceFreshness freshness) noexcept;

/// Generation-bound evidence stamp. A capability, layout, or health claim is
/// only usable while it is both current and unexpired.
struct EvidenceStamp {
  EvidenceGeneration generation{};
  EvidenceProvenance provenance{EvidenceProvenance::Unknown};
  std::uint64_t observed_at_ms{0};
  std::uint64_t ttl_ms{0};
  std::string source;
  bool observed{false};

  static EvidenceStamp unknown() noexcept { return EvidenceStamp{}; }

  bool is_observed() const noexcept { return observed && generation.known(); }

  EvidenceFreshness freshness_at(std::uint64_t now_ms) const noexcept {
    if (!is_observed()) {
      return EvidenceFreshness::Unknown;
    }
    if (ttl_ms == 0) {
      return EvidenceFreshness::Unknown;
    }
    if (now_ms <= observed_at_ms) {
      return EvidenceFreshness::Fresh;
    }
    const std::uint64_t age = now_ms - observed_at_ms;
    if (age <= ttl_ms) {
      return EvidenceFreshness::Fresh;
    }
    if (ttl_ms <= UINT64_MAX / 4 && age <= ttl_ms * 4) {
      return EvidenceFreshness::Stale;
    }
    return EvidenceFreshness::Expired;
  }

  bool is_fresh_at(std::uint64_t now_ms) const noexcept {
    return freshness_at(now_ms) == EvidenceFreshness::Fresh;
  }
};

/// Health evidence consumed from an external health provider. The fabric does
/// not monitor device health; it consumes the observation and governs
/// partition authority with it.
enum class HealthState : std::uint8_t {
  Unknown = 0,
  Healthy = 1,
  Degraded = 2,
  Unhealthy = 3,
  Recovering = 4,
};

const char* to_string(HealthState state) noexcept;
HealthState health_state_from_string(std::string_view name);

struct HealthEvidence {
  HealthState state{HealthState::Unknown};
  EvidenceStamp stamp{};
  std::string source;

  bool usable_at(std::uint64_t now_ms) const noexcept { return stamp.is_fresh_at(now_ms); }
};

}  // namespace apf
