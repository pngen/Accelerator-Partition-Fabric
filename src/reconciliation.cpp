#include "apf/reconciliation.hpp"

namespace apf {

const char* to_string(ReconciliationFindingKind kind) noexcept {
  switch (kind) {
    case ReconciliationFindingKind::PartitionMatches: return "PARTITION_MATCHES";
    case ReconciliationFindingKind::PartitionMissing: return "PARTITION_MISSING";
    case ReconciliationFindingKind::PartitionUnexpected: return "PARTITION_UNEXPECTED";
    case ReconciliationFindingKind::ProfileChanged: return "PROFILE_CHANGED";
    case ReconciliationFindingKind::NativeIdentityChanged: return "NATIVE_IDENTITY_CHANGED";
    case ReconciliationFindingKind::DeviceReset: return "DEVICE_RESET";
    case ReconciliationFindingKind::CapabilityChanged: return "CAPABILITY_CHANGED";
    case ReconciliationFindingKind::DeviceGone: return "DEVICE_GONE";
    case ReconciliationFindingKind::DeviceNotPartitionCapable: return "DEVICE_NOT_PARTITION_CAPABLE";
    case ReconciliationFindingKind::GenerationUncorrelatable: return "GENERATION_UNCORRELATABLE";
    case ReconciliationFindingKind::AssignmentOrphaned: return "ASSIGNMENT_ORPHANED";
    case ReconciliationFindingKind::LayoutMatches: return "LAYOUT_MATCHES";
    case ReconciliationFindingKind::LedgerMismatch: return "LEDGER_MISMATCH";
    case ReconciliationFindingKind::LayoutChangedExternally: return "LAYOUT_CHANGED_EXTERNALLY";
    case ReconciliationFindingKind::Count: return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* to_string(ReconciliationSeverity severity) noexcept {
  switch (severity) {
    case ReconciliationSeverity::Informational: return "informational";
    case ReconciliationSeverity::Attention: return "attention";
    case ReconciliationSeverity::Material: return "material";
    case ReconciliationSeverity::Critical: return "critical";
    case ReconciliationSeverity::Count: return "unknown";
  }
  return "unknown";
}

std::string ReconciliationReport::summary() const {
  std::string out = "reconciliation ";
  out += id.str();
  out += " accelerator=";
  out += accelerator.valid() ? accelerator.str() : std::string("none");
  out += " device_present=";
  out += device_present ? "true" : "false";
  out += " matched=";
  out += std::to_string(matched);
  out += " missing=";
  out += std::to_string(missing);
  out += " unexpected=";
  out += std::to_string(unexpected);
  out += " adopted=";
  out += std::to_string(adopted);
  out += " fenced=";
  out += std::to_string(fenced);
  if (device_generation_changed) {
    out += " device-generation-changed";
  }
  if (capability_changed) {
    out += " capability-changed";
  }
  if (revalidation_required) {
    out += " REVALIDATION_REQUIRED";
  }
  if (reconciliation_required) {
    out += " RECONCILIATION_REQUIRED";
  }
  return out;
}

}  // namespace apf
