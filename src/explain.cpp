#include "apf/explain.hpp"

#include <algorithm>
#include <cstdio>

namespace apf {

const char* to_string(ExplanationCode code) noexcept {
  switch (code) {
    case ExplanationCode::None: return "none";
    case ExplanationCode::CandidateEligible: return "candidate_eligible";
    case ExplanationCode::CandidateChosen: return "candidate_chosen";
    case ExplanationCode::CandidateRejected: return "candidate_rejected";
    case ExplanationCode::RejectedDeviceUnknown: return "rejected_device_unknown";
    case ExplanationCode::RejectedStaleEvidence: return "rejected_stale_evidence";
    case ExplanationCode::RejectedCapabilityUnknown: return "rejected_capability_unknown";
    case ExplanationCode::RejectedCapabilityUnsupported: return "rejected_capability_unsupported";
    case ExplanationCode::RejectedBackendMismatch: return "rejected_backend_mismatch";
    case ExplanationCode::RejectedDeviceFamily: return "rejected_device_family";
    case ExplanationCode::RejectedLocality: return "rejected_locality";
    case ExplanationCode::RejectedHealth: return "rejected_health";
    case ExplanationCode::RejectedAcceleratorGeneration: return "rejected_accelerator_generation";
    case ExplanationCode::RejectedInsufficientFreeCapacity: return "rejected_insufficient_free_capacity";
    case ExplanationCode::RejectedFragmentedGeometry: return "rejected_fragmented_geometry";
    case ExplanationCode::RejectedMaxPartitionCount: return "rejected_max_partition_count";
    case ExplanationCode::RejectedIncompatibleProfileCombination: return "rejected_incompatible_profile_combination";
    case ExplanationCode::RejectedIsolation: return "rejected_isolation";
    case ExplanationCode::RejectedPolicy: return "rejected_policy";
    case ExplanationCode::RejectedDrainRequired: return "rejected_drain_required";
    case ExplanationCode::RejectedDestructiveReconfiguration: return "rejected_destructive_reconfiguration";
    case ExplanationCode::RejectedDowntimeExceeded: return "rejected_downtime_exceeded";
    case ExplanationCode::RejectedDeviceBusy: return "rejected_device_busy";
    case ExplanationCode::RejectedDeviceOffline: return "rejected_device_offline";
    case ExplanationCode::RejectedRevalidationRequired: return "rejected_revalidation_required";
    case ExplanationCode::RejectedDuplicateRequest: return "rejected_duplicate_request";
    case ExplanationCode::RejectedLimit: return "rejected_limit";
    case ExplanationCode::RankingFactor: return "ranking_factor";
    case ExplanationCode::TieBreak: return "tie_break";
    case ExplanationCode::PlanFeasibleNow: return "plan_feasible_now";
    case ExplanationCode::PlanFeasibleAfterDrain: return "plan_feasible_after_drain";
    case ExplanationCode::PlanFeasibleAfterReconfiguration: return "plan_feasible_after_reconfiguration";
    case ExplanationCode::PlanPhysicallyImpossible: return "plan_physically_impossible";
    case ExplanationCode::PlanCapabilityUnsupported: return "plan_capability_unsupported";
    case ExplanationCode::PlanInsufficientCapacity: return "plan_insufficient_capacity";
    case ExplanationCode::PlanFragmented: return "plan_fragmented";
    case ExplanationCode::PlanPolicyRejected: return "plan_policy_rejected";
    case ExplanationCode::PlanStaleEvidence: return "plan_stale_evidence";
    case ExplanationCode::PlanRevalidationRequired: return "plan_revalidation_required";
    case ExplanationCode::PlanNoEligibleAccelerator: return "plan_no_eligible_accelerator";
    case ExplanationCode::ReserveAccepted: return "reserve_accepted";
    case ExplanationCode::ReserveRejected: return "reserve_rejected";
    case ExplanationCode::CommitAccepted: return "commit_accepted";
    case ExplanationCode::CommitRejected: return "commit_rejected";
    case ExplanationCode::ReleaseAccepted: return "release_accepted";
    case ExplanationCode::ReleaseRejected: return "release_rejected";
    case ExplanationCode::DrainStarted: return "drain_started";
    case ExplanationCode::DrainProgressed: return "drain_progressed";
    case ExplanationCode::DrainCompleted: return "drain_completed";
    case ExplanationCode::DrainBlocked: return "drain_blocked";
    case ExplanationCode::ReconfigurationPlanned: return "reconfiguration_planned";
    case ExplanationCode::ReconfigurationStale: return "reconfiguration_stale";
    case ExplanationCode::ReconfigurationExecuted: return "reconfiguration_executed";
    case ExplanationCode::ReconfigurationVerified: return "reconfiguration_verified";
    case ExplanationCode::AttemptRegistered: return "attempt_registered";
    case ExplanationCode::PhysicalVerificationPassed: return "physical_verification_passed";
    case ExplanationCode::PhysicalVerificationFailed: return "physical_verification_failed";
    case ExplanationCode::OutcomeUnknown: return "outcome_unknown";
    case ExplanationCode::ReconciliationRequired: return "reconciliation_required";
    case ExplanationCode::RevalidationRequired: return "revalidation_required";
    case ExplanationCode::ReconciledMatch: return "reconciled_match";
    case ExplanationCode::ReconciledMissing: return "reconciled_missing";
    case ExplanationCode::ReconciledUnexpected: return "reconciled_unexpected";
    case ExplanationCode::ReconciledProfileChanged: return "reconciled_profile_changed";
    case ExplanationCode::ReconciledDeviceReset: return "reconciled_device_reset";
    case ExplanationCode::ReconciledCapabilityChanged: return "reconciled_capability_changed";
    case ExplanationCode::ReconciledDeviceGone: return "reconciled_device_gone";
    case ExplanationCode::ReconciledDeviceNotPartitionCapable: return "reconciled_device_not_partition_capable";
    case ExplanationCode::ReconciledGenerationUncorrelatable: return "reconciled_generation_uncorrelatable";
    case ExplanationCode::ReconciledAssignmentOrphaned: return "reconciled_assignment_orphaned";
    case ExplanationCode::ExternalAdoption: return "external_adoption";
    case ExplanationCode::WorkerFenced: return "worker_fenced";
    case ExplanationCode::StaleWorkerRejected: return "stale_worker_rejected";
    case ExplanationCode::StaleEpochRejected: return "stale_epoch_rejected";
    case ExplanationCode::StalePlanRejected: return "stale_plan_rejected";
    case ExplanationCode::StaleReservationRejected: return "stale_reservation_rejected";
    case ExplanationCode::StalePartitionGenerationRejected: return "stale_partition_generation_rejected";
    case ExplanationCode::CapabilityGenerationChanged: return "capability_generation_changed";
    case ExplanationCode::PolicyGenerationChanged: return "policy_generation_changed";
    case ExplanationCode::ReservationRolledBack: return "reservation_rolled_back";
    case ExplanationCode::ReservationExpired: return "reservation_expired";
    case ExplanationCode::AccountingClosed: return "accounting_closed";
    case ExplanationCode::AccountingViolation: return "accounting_violation";
    case ExplanationCode::PersistenceRejected: return "persistence_rejected";
    case ExplanationCode::ProtocolRejected: return "protocol_rejected";
    case ExplanationCode::Cancelled: return "cancelled";
    case ExplanationCode::Shutdown: return "shutdown";
    case ExplanationCode::Count: return "count";
  }
  return "none";
}

void Explanation::add(ExplanationCode code, std::string subject, std::string detail,
                      std::int64_t magnitude) {
  ExplanationFactor factor;
  factor.code = code;
  factor.subject = std::move(subject);
  factor.detail = std::move(detail);
  factor.magnitude = magnitude;
  factor.sequence = static_cast<std::uint32_t>(factors_.size());
  factors_.push_back(std::move(factor));
}

void Explanation::add(const ExplanationFactor& factor) {
  ExplanationFactor copy = factor;
  copy.sequence = static_cast<std::uint32_t>(factors_.size());
  factors_.push_back(std::move(copy));
}

void Explanation::append(const Explanation& other) {
  for (const ExplanationFactor& factor : other.factors_) {
    add(factor);
  }
  if (summary_.empty()) {
    summary_ = other.summary_;
  }
}

void Explanation::clear() noexcept {
  factors_.clear();
  summary_.clear();
}

bool Explanation::contains(ExplanationCode code) const noexcept { return count(code) > 0; }

std::size_t Explanation::count(ExplanationCode code) const noexcept {
  std::size_t total = 0;
  for (const ExplanationFactor& factor : factors_) {
    if (factor.code == code) {
      ++total;
    }
  }
  return total;
}

void Explanation::canonicalize(std::size_t max_factors) {
  std::stable_sort(factors_.begin(), factors_.end(),
                   [](const ExplanationFactor& lhs, const ExplanationFactor& rhs) {
                     if (lhs.code != rhs.code) {
                       return lhs.code < rhs.code;
                     }
                     if (lhs.subject != rhs.subject) {
                       return lhs.subject < rhs.subject;
                     }
                     if (lhs.detail != rhs.detail) {
                       return lhs.detail < rhs.detail;
                     }
                     return lhs.magnitude < rhs.magnitude;
                   });
  if (max_factors > 0 && factors_.size() > max_factors) {
    factors_.resize(max_factors);
  }
  for (std::size_t index = 0; index < factors_.size(); ++index) {
    factors_[index].sequence = static_cast<std::uint32_t>(index);
  }
}

std::string Explanation::format() const {
  std::string out;
  if (!summary_.empty()) {
    out += summary_;
    out += "\n";
  }
  for (const ExplanationFactor& factor : factors_) {
    out += "  - ";
    out += to_string(factor.code);
    if (!factor.subject.empty()) {
      out += " [";
      out += factor.subject;
      out += "]";
    }
    if (factor.magnitude != 0) {
      out += " magnitude=";
      out += std::to_string(factor.magnitude);
    }
    if (!factor.detail.empty()) {
      out += ": ";
      out += factor.detail;
    }
    out += "\n";
  }
  return out;
}

std::uint64_t Explanation::digest() const noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  const auto mix = [&hash](std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
      hash ^= (value >> (byte * 8)) & 0xFFu;
      hash *= 1099511628211ull;
    }
  };
  mix(factors_.size());
  for (const ExplanationFactor& factor : factors_) {
    mix(static_cast<std::uint64_t>(factor.code));
    mix(static_cast<std::uint64_t>(factor.magnitude));
    for (const char ch : factor.subject) {
      hash ^= static_cast<std::uint8_t>(ch);
      hash *= 1099511628211ull;
    }
    hash ^= 0xFFu;
    hash *= 1099511628211ull;
    for (const char ch : factor.detail) {
      hash ^= static_cast<std::uint8_t>(ch);
      hash *= 1099511628211ull;
    }
    hash ^= 0xFEu;
    hash *= 1099511628211ull;
  }
  return hash;
}

Explanation Explanation::single(ExplanationCode code, std::string subject, std::string detail) {
  Explanation explanation;
  explanation.add(code, std::move(subject), std::move(detail));
  return explanation;
}

}  // namespace apf
