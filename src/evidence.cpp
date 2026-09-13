#include "apf/evidence.hpp"

namespace apf {

const char* to_string(EvidenceProvenance provenance) noexcept {
  switch (provenance) {
    case EvidenceProvenance::Unknown: return "unknown";
    case EvidenceProvenance::Real: return "real";
    case EvidenceProvenance::Synthetic: return "synthetic";
    case EvidenceProvenance::Unsupported: return "unsupported";
  }
  return "unknown";
}

EvidenceProvenance evidence_provenance_from_string(std::string_view name) {
  if (name == "real") {
    return EvidenceProvenance::Real;
  }
  if (name == "synthetic") {
    return EvidenceProvenance::Synthetic;
  }
  if (name == "unsupported") {
    return EvidenceProvenance::Unsupported;
  }
  return EvidenceProvenance::Unknown;
}

const char* to_string(EvidenceFreshness freshness) noexcept {
  switch (freshness) {
    case EvidenceFreshness::Unknown: return "unknown";
    case EvidenceFreshness::Fresh: return "fresh";
    case EvidenceFreshness::Stale: return "stale";
    case EvidenceFreshness::Expired: return "expired";
  }
  return "unknown";
}

const char* to_string(HealthState state) noexcept {
  switch (state) {
    case HealthState::Unknown: return "unknown";
    case HealthState::Healthy: return "healthy";
    case HealthState::Degraded: return "degraded";
    case HealthState::Unhealthy: return "unhealthy";
    case HealthState::Recovering: return "recovering";
  }
  return "unknown";
}

HealthState health_state_from_string(std::string_view name) {
  if (name == "healthy") {
    return HealthState::Healthy;
  }
  if (name == "degraded") {
    return HealthState::Degraded;
  }
  if (name == "unhealthy") {
    return HealthState::Unhealthy;
  }
  if (name == "recovering") {
    return HealthState::Recovering;
  }
  return HealthState::Unknown;
}

}  // namespace apf
