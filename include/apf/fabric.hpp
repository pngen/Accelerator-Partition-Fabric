#pragma once

#include "apf/accounting.hpp"
#include "apf/accelerator.hpp"
#include "apf/backend.hpp"
#include "apf/evidence.hpp"
#include "apf/fragmentation.hpp"
#include "apf/id.hpp"
#include "apf/limits.hpp"
#include "apf/partition.hpp"
#include "apf/persistence.hpp"
#include "apf/plan.hpp"
#include "apf/policy.hpp"
#include "apf/reconciliation.hpp"
#include "apf/request.hpp"
#include "apf/reservation.hpp"
#include "apf/snapshot.hpp"
#include "apf/time.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apf {

/// A bounded, ordered observation record.
struct FabricEvent {
  ExplanationCode code{ExplanationCode::None};
  std::string subject;
  std::string detail;
  std::uint64_t at_ms{0};
  std::uint64_t sequence{0};
};

struct FabricOptions {
  Limits limits{};
  ClockPtr clock{};
  CoordinatorEpoch coordinator_epoch{CoordinatorEpoch::first()};
  std::string instance_id{"apf-instance"};
  /// Backend used by execute_local when no explicit backend is supplied.
  std::shared_ptr<AcceleratorBackend> local_backend{};
  /// When true, physical mutation through this instance is only permitted once
  /// a worker incarnation has been bound to the reservation.
  bool require_worker_for_mutation{false};
  /// Retained snapshots. Bounded by Limits::max_snapshot_retention.
  std::size_t snapshot_retention{8};
};

/// Outcome of a physical mutation attempt as observed by the runtime.
struct MutationOutcome {
  PartitionReservationId reservation{};
  PartitionAttemptId attempt{};
  AttemptState state{AttemptState::Registered};
  PartitionPlanId plan{};
  AcceleratorId accelerator{};
  std::vector<PartitionId> partitions;
  std::vector<PartitionGeneration> generations;
  bool outcome_unknown{false};
  bool reconciliation_required{false};
  bool committed{false};
  bool verified_physically{false};
  std::string message;
  Explanation explanation{};

  bool succeeded() const noexcept { return committed && !outcome_unknown; }
};

/// The Accelerator Partition Fabric runtime instance.
///
/// One mutex guards the ledger and registry state. No lock is ever held across
/// a backend call, subprocess execution, socket I/O, filesystem I/O, a callback
/// or any user-supplied adapter: every method that touches the outside world
/// runs in explicit phases (validate under lock, act without the lock, publish
/// under lock).
class PartitionFabric {
 public:
  explicit PartitionFabric(FabricOptions options = {});
  ~PartitionFabric();

  PartitionFabric(const PartitionFabric&) = delete;
  PartitionFabric& operator=(const PartitionFabric&) = delete;

  // --- instance -------------------------------------------------------------
  const Limits& limits() const noexcept;
  const std::string& instance_id() const noexcept;
  CoordinatorEpoch coordinator_epoch() const;
  StateGeneration state_generation() const;
  bool closed() const;
  /// Stops accepting new work, fences new mutations, resolves pending attempts
  /// conservatively and returns accounting to a valid baseline.
  Status close();
  /// Re-opens a closed instance for a new operating session.
  Status reopen();
  /// Verifies that every ledger closes exactly.
  Status verify_accounting() const;

  // --- policy ---------------------------------------------------------------
  Status set_policy(const PlanningPolicy& policy);
  PlanningPolicy policy() const;

  // --- registration ---------------------------------------------------------
  /// Registers a physical accelerator from backend evidence. Returns the
  /// accelerator identity assigned (or the existing one when the stable key is
  /// already known).
  Result<AcceleratorId> register_backend_accelerator(const BackendAccelerator& accelerator);
  /// Registers or updates an explicit accelerator record.
  Status register_accelerator(const AcceleratorRecord& record);
  /// Republishes capability. A materially different publication advances the
  /// capability generation and invalidates dependent plans and reservations.
  Result<CapabilityGeneration> publish_capability(const PartitionCapability& capability);
  Status register_profile(const PartitionProfile& profile);
  Status update_health(const AcceleratorId& accelerator, const HealthEvidence& health);
  Status mark_evidence_stale(const AcceleratorId& accelerator, std::string reason);
  /// Runs discovery through a backend and registers everything it reports.
  Status discover(AcceleratorBackend& backend, std::vector<AcceleratorId>* discovered = nullptr);
  /// Refreshes the observed layout of one accelerator through a backend.
  Result<ReconciliationReport> refresh_layout(AcceleratorBackend& backend,
                                              std::string_view stable_key);

  // --- inspection -----------------------------------------------------------
  std::shared_ptr<const FabricSnapshot> snapshot() const;
  Result<AcceleratorRecord> accelerator(const AcceleratorId& id) const;
  Result<PartitionRecord> partition(const PartitionId& id) const;
  Result<CapacityLedger> ledger(const AcceleratorId& id) const;
  Result<PartitionPlan> plan_by_id(const PartitionPlanId& id) const;
  Result<PartitionReservation> reservation(const PartitionReservationId& id) const;
  Result<PartitionAttempt> attempt(const PartitionAttemptId& id) const;
  /// Backend key that governs an accelerator, as published by discovery.
  Result<std::string> accelerator_key(const AcceleratorId& id) const;
  /// Allocates a fresh worker identity. The coordinator owns worker identity
  /// allocation so that a worker process never has to invent one.
  Result<WorkerId> allocate_worker_id();
  /// Records which worker incarnation published a device's evidence. This is a
  /// live binding: it is never persisted, because a worker incarnation does not
  /// survive a restart.
  Status bind_accelerator_worker(const AcceleratorId& accelerator, const WorkerId& worker,
                                 const WorkerBootId& worker_boot);
  /// The worker incarnation currently responsible for a device.
  Result<WorkerRecord> accelerator_worker(const AcceleratorId& accelerator) const;
  Result<PartitionAssignment> assignment(const PartitionAssignmentId& id) const;
  std::vector<FabricEvent> events(std::size_t max_events = 64) const;
  std::vector<AcceleratorId> accelerator_ids() const;

  // --- fragmentation --------------------------------------------------------
  Result<FragmentationReport> analyze_fragmentation(const AcceleratorId& accelerator,
                                                    const PartitionRequest& request) const;

  // --- planning -------------------------------------------------------------
  Result<PartitionPlan> plan(const PartitionRequest& request);
  Result<PartitionPlan> plan_on(const AcceleratorId& accelerator,
                                const PartitionRequest& request);
  /// Plans a destructive layout change from the current configuration to the
  /// desired set of partitions. The plan binds the exact state it was derived
  /// from; any relevant change makes it stale.
  Result<PartitionPlan> plan_reconfiguration(const AcceleratorId& accelerator,
                                             const std::vector<PlannedPartition>& desired,
                                             const PartitionRequest& context);
  Status discard_plan(const PartitionPlanId& plan);

  // --- reservation ----------------------------------------------------------
  Result<PartitionReservation> reserve(const PartitionPlanId& plan);
  Result<PartitionReservation> reserve_for_worker(const PartitionPlanId& plan,
                                                  const WorkerId& worker,
                                                  const WorkerBootId& worker_boot,
                                                  const CoordinatorEpoch& epoch);
  Status release_reservation(const PartitionReservationId& reservation, std::string reason);
  Status fence_reservation(const PartitionReservationId& reservation, std::string reason);

  // --- mutation -------------------------------------------------------------
  /// Registers an attempt before dispatch. Fast completion can never race
  /// state registration because the record exists first.
  Result<PartitionAttempt> register_attempt(const PartitionReservationId& reservation,
                                            AttemptKind kind, const WorkerId& worker = {},
                                            const WorkerBootId& worker_boot = {});
  /// Registers, dispatches and verifies a local mutation through a backend.
  Result<MutationOutcome> execute(const PartitionReservationId& reservation,
                                  AcceleratorBackend* backend = nullptr);
  /// Applies the outcome reported by a worker for a previously registered
  /// attempt, verifying against observed physical state.
  Result<MutationOutcome> apply_mutation_result(const PartitionAttemptId& attempt,
                                                const BackendMutationResult& result,
                                                const BackendLayout* observed);
  /// Marks an attempt whose acknowledgement was lost.
  Status mark_attempt_outcome_unknown(const PartitionAttemptId& attempt, std::string reason);
  /// Executes a planned destructive reconfiguration.
  Result<MutationOutcome> execute_reconfiguration(const PartitionPlanId& plan,
                                                  AcceleratorBackend* backend = nullptr);
  /// Destroys one partition through a backend.
  Result<MutationOutcome> destroy_partition(const PartitionId& partition,
                                            AcceleratorBackend* backend = nullptr);

  // --- drain ----------------------------------------------------------------
  Status begin_drain(const PartitionId& partition, std::string reason);
  Status begin_drain_accelerator(const AcceleratorId& accelerator, std::string reason);
  Status complete_drain(const PartitionId& partition);
  Status cancel_drain(const PartitionId& partition);
  Result<DrainProgress> drain_progress(const PartitionId& partition) const;

  // --- admission ------------------------------------------------------------
  Result<AdmissionProbe> probe_admission(const WorkloadRequirement& requirement) const;
  Result<AdmissionDecision> admit(const WorkloadRequirement& requirement);
  Status bind_assignment(const PartitionAssignment& assignment);
  Status unbind_assignment(const PartitionAssignmentId& assignment, std::string reason);
  std::vector<PartitionAssignment> assignments_of(const PartitionId& partition) const;

  // --- generation-bound authority ------------------------------------------
  Status fence_worker(const WorkerId& worker, const WorkerBootId& worker_boot, std::string reason);
  Status note_worker(const WorkerRecord& worker);
  Status note_worker_seen(const WorkerId& worker, const WorkerBootId& worker_boot,
                          std::uint64_t at_ms);
  Result<CoordinatorEpoch> advance_coordinator_epoch();
  Status install_coordinator_epoch(const CoordinatorEpoch& epoch);

  // --- reconciliation -------------------------------------------------------
  Result<ReconciliationReport> reconcile(const AcceleratorId& accelerator,
                                         const BackendLayout& observed);
  Result<ReconciliationReport> reconcile_with_backend(AcceleratorBackend& backend,
                                                      std::string_view stable_key);
  /// Reconciliation against an observation obtained elsewhere (for example from
  /// a worker process) addressed by the backend key.
  Result<ReconciliationReport> reconcile_with_backend_key(const BackendLayout& observed);
  /// Resolves a backend key to the accelerator identity that governs it.
  Result<AcceleratorId> accelerator_for_key(std::string_view stable_key) const;

  /// Authority envelope for destroying one partition through a delegated
  /// worker: validates the drain gate and registers the destruction plan and
  /// reservation that the attempt will be bound to.
  Result<PartitionReservation> create_destruction_reservation(const PartitionId& partition);

  // --- persistence ----------------------------------------------------------
  Result<DurableState> export_durable_state() const;
  /// Imports durable state. Live physical authority is never restored as
  /// current: affected partitions land in RevalidationRequired and in-flight
  /// attempts in ReconciliationRequired.
  Result<ReconciliationReport> import_durable_state(const DurableState& state);
  Status save(PersistenceStore& store);
  Status load(PersistenceStore& store);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace apf
