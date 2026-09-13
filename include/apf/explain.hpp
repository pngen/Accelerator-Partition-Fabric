#pragma once

#include "apf/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace apf {

/// Machine-readable reasons for material decisions. Every deterministic
/// explanation is built from these; callers never parse prose to understand
/// why a decision was made.
enum class ExplanationCode : std::uint16_t {
  None = 0,
  CandidateEligible,
  CandidateChosen,
  CandidateRejected,
  RejectedDeviceUnknown,
  RejectedStaleEvidence,
  RejectedCapabilityUnknown,
  RejectedCapabilityUnsupported,
  RejectedBackendMismatch,
  RejectedDeviceFamily,
  RejectedLocality,
  RejectedHealth,
  RejectedAcceleratorGeneration,
  RejectedInsufficientFreeCapacity,
  RejectedFragmentedGeometry,
  RejectedMaxPartitionCount,
  RejectedIncompatibleProfileCombination,
  RejectedIsolation,
  RejectedPolicy,
  RejectedDrainRequired,
  RejectedDestructiveReconfiguration,
  RejectedDowntimeExceeded,
  RejectedDeviceBusy,
  RejectedDeviceOffline,
  RejectedRevalidationRequired,
  RejectedDuplicateRequest,
  RejectedLimit,
  RankingFactor,
  TieBreak,
  PlanFeasibleNow,
  PlanFeasibleAfterDrain,
  PlanFeasibleAfterReconfiguration,
  PlanPhysicallyImpossible,
  PlanCapabilityUnsupported,
  PlanInsufficientCapacity,
  PlanFragmented,
  PlanPolicyRejected,
  PlanStaleEvidence,
  PlanRevalidationRequired,
  PlanNoEligibleAccelerator,
  ReserveAccepted,
  ReserveRejected,
  CommitAccepted,
  CommitRejected,
  ReleaseAccepted,
  ReleaseRejected,
  DrainStarted,
  DrainProgressed,
  DrainCompleted,
  DrainBlocked,
  ReconfigurationPlanned,
  ReconfigurationStale,
  ReconfigurationExecuted,
  ReconfigurationVerified,
  AttemptRegistered,
  PhysicalVerificationPassed,
  PhysicalVerificationFailed,
  OutcomeUnknown,
  ReconciliationRequired,
  RevalidationRequired,
  ReconciledMatch,
  ReconciledMissing,
  ReconciledUnexpected,
  ReconciledProfileChanged,
  ReconciledDeviceReset,
  ReconciledCapabilityChanged,
  ReconciledDeviceGone,
  ReconciledDeviceNotPartitionCapable,
  ReconciledGenerationUncorrelatable,
  ReconciledAssignmentOrphaned,
  ExternalAdoption,
  WorkerFenced,
  StaleWorkerRejected,
  StaleEpochRejected,
  StalePlanRejected,
  StaleReservationRejected,
  StalePartitionGenerationRejected,
  CapabilityGenerationChanged,
  PolicyGenerationChanged,
  ReservationRolledBack,
  ReservationExpired,
  AccountingClosed,
  AccountingViolation,
  PersistenceRejected,
  ProtocolRejected,
  Cancelled,
  Shutdown,
  Count,
};

const char* to_string(ExplanationCode code) noexcept;

/// One decisive factor. Magnitude is an integer so that ordering is exact.
struct ExplanationFactor {
  ExplanationCode code{ExplanationCode::None};
  std::string subject;
  std::string detail;
  std::int64_t magnitude{0};
  std::uint32_t sequence{0};
};

/// A deterministic, ordered explanation. Factors are sorted by a total order
/// that does not depend on insertion order.
class Explanation {
 public:
  Explanation() = default;

  void add(ExplanationCode code, std::string subject, std::string detail = {},
           std::int64_t magnitude = 0);
  void add(const ExplanationFactor& factor);
  void append(const Explanation& other);
  void clear() noexcept;

  bool empty() const noexcept { return factors_.empty(); }
  std::size_t size() const noexcept { return factors_.size(); }
  const std::vector<ExplanationFactor>& factors() const noexcept { return factors_; }
  const std::string& summary() const noexcept { return summary_; }
  void set_summary(std::string summary) { summary_ = std::move(summary); }

  bool contains(ExplanationCode code) const noexcept;
  std::size_t count(ExplanationCode code) const noexcept;

  /// Sorts factors into canonical order and trims to the configured bound.
  void canonicalize(std::size_t max_factors);

  std::string format() const;
  /// Stable digest of the canonical factor list, used to prove determinism.
  std::uint64_t digest() const noexcept;

  static Explanation single(ExplanationCode code, std::string subject, std::string detail = {});

 private:
  std::vector<ExplanationFactor> factors_;
  std::string summary_;
};

}  // namespace apf
