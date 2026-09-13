#pragma once

#include "apf/fabric.hpp"

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace apf {

/// Registry state guarded by PartitionFabric::Impl::mutex.
///
/// Ordered maps are used deliberately: iteration order is then deterministic,
/// which is what allows snapshot rendering, candidate ordering and explanation
/// ordering to be reproducible rather than dependent on hash seeding.
struct FabricState {
  Limits limits{};
  ClockPtr clock;
  CoordinatorEpoch coordinator_epoch{CoordinatorEpoch::first()};
  StateGeneration state_generation{StateGeneration::first()};
  SnapshotGeneration snapshot_generation{SnapshotGeneration::first()};
  PolicyGeneration policy_generation{PolicyGeneration::first()};
  PlanningPolicy policy{};
  IdAllocator ids{};

  std::map<std::uint64_t, AcceleratorRecord> accelerators;
  std::map<std::uint64_t, CapacityLedger> ledgers;
  /// accelerator id -> backend stable key
  std::map<std::uint64_t, std::string> stable_keys;
  /// backend stable key -> accelerator id
  std::map<std::string, AcceleratorId> key_index;
  std::map<std::uint64_t, PartitionProfile> profiles;
  std::map<std::uint64_t, PartitionRecord> partitions;
  std::map<std::uint64_t, PartitionPlan> plans;
  std::map<std::uint64_t, PartitionReservation> reservations;
  std::map<std::uint64_t, PartitionAttempt> attempts;
  std::map<std::uint64_t, PartitionAssignment> assignments;
  std::map<std::uint64_t, WorkerRecord> workers;
  /// accelerator id -> the worker incarnation that published its evidence.
  /// Live binding only: a worker incarnation never survives a restart.
  std::map<std::uint64_t, WorkerRecord> accelerator_owner;

  EvidenceGeneration evidence_generation{EvidenceGeneration::first()};
  std::deque<FabricEvent> events;
  std::uint64_t event_sequence{0};
  std::string instance_id{"apf-instance"};
  std::uint64_t opened_at_ms{0};
  bool closed{false};
  std::uint64_t closed_at_ms{0};

  AcceleratorRecord* find_accelerator(const AcceleratorId& id) {
    const auto it = accelerators.find(id.value());
    return it == accelerators.end() ? nullptr : &it->second;
  }
  const AcceleratorRecord* find_accelerator(const AcceleratorId& id) const {
    const auto it = accelerators.find(id.value());
    return it == accelerators.end() ? nullptr : &it->second;
  }
  CapacityLedger* find_ledger(const AcceleratorId& id) {
    const auto it = ledgers.find(id.value());
    return it == ledgers.end() ? nullptr : &it->second;
  }
  const CapacityLedger* find_ledger(const AcceleratorId& id) const {
    const auto it = ledgers.find(id.value());
    return it == ledgers.end() ? nullptr : &it->second;
  }
  PartitionProfile* find_profile(const PartitionProfileId& id) {
    const auto it = profiles.find(id.value());
    return it == profiles.end() ? nullptr : &it->second;
  }
  const PartitionProfile* find_profile(const PartitionProfileId& id) const {
    const auto it = profiles.find(id.value());
    return it == profiles.end() ? nullptr : &it->second;
  }
  PartitionRecord* find_partition(const PartitionId& id) {
    const auto it = partitions.find(id.value());
    return it == partitions.end() ? nullptr : &it->second;
  }
  const PartitionRecord* find_partition(const PartitionId& id) const {
    const auto it = partitions.find(id.value());
    return it == partitions.end() ? nullptr : &it->second;
  }
  PartitionReservation* find_reservation(const PartitionReservationId& id) {
    const auto it = reservations.find(id.value());
    return it == reservations.end() ? nullptr : &it->second;
  }
  const PartitionReservation* find_reservation(const PartitionReservationId& id) const {
    const auto it = reservations.find(id.value());
    return it == reservations.end() ? nullptr : &it->second;
  }
  PartitionAttempt* find_attempt(const PartitionAttemptId& id) {
    const auto it = attempts.find(id.value());
    return it == attempts.end() ? nullptr : &it->second;
  }
  const PartitionAttempt* find_attempt(const PartitionAttemptId& id) const {
    const auto it = attempts.find(id.value());
    return it == attempts.end() ? nullptr : &it->second;
  }
  WorkerRecord* find_worker(const WorkerId& id) {
    const auto it = workers.find(id.value());
    return it == workers.end() ? nullptr : &it->second;
  }
  std::vector<const PartitionRecord*> partitions_of(const AcceleratorId& id) const {
    std::vector<const PartitionRecord*> out;
    for (const auto& entry : partitions) {
      if (entry.second.accelerator == id) {
        out.push_back(&entry.second);
      }
    }
    return out;
  }
  std::size_t count_partitions_in_states(const AcceleratorId& id,
                                         std::initializer_list<PartitionState> states) const {
    std::size_t total = 0;
    for (const auto& entry : partitions) {
      if (entry.second.accelerator != id) {
        continue;
      }
      for (const PartitionState state : states) {
        if (entry.second.state == state) {
          ++total;
          break;
        }
      }
    }
    return total;
  }
};

/// Records a bounded, ordered event.
void record_event(FabricState& state, ExplanationCode code, std::string subject,
                  std::string detail = {});
/// Advances the state generation. Called after every material mutation.
void bump_state_generation(FabricState& state);

/// The ledger bucket that is authoritative for a partition state. An absent
/// value means the state holds no capacity.
std::optional<CapacityBucket> bucket_for_state(PartitionState state);

/// Applies the capacity consequence of a lifecycle change. Capacity always
/// moves between exactly two buckets, and a transition that would need capacity
/// the partition never reserved is rejected instead of inventing it.
Status apply_capacity_transition(CapacityLedger& ledger, PartitionRecord& record,
                                 PartitionState target_state);

/// Validates a lifecycle transition and applies its capacity consequence.
Status transition_partition(FabricState& state, PartitionRecord& record, PartitionState to,
                            std::string reason);

/// True when the plan binding still matches the live state and has not expired.
Status validate_plan_binding(const FabricState& state, const PartitionPlan& plan,
                             std::uint64_t now_ms, bool require_fresh_evidence);

/// Builds the immutable snapshot for the current state.
std::shared_ptr<const FabricSnapshot> build_snapshot(const FabricState& state,
                                                     std::uint64_t now_ms);

// --- analysis engines (pure functions over a consistent state) --------------

/// Deterministic fragmentation analysis for one accelerator and one request.
FragmentationReport analyze_fragmentation_impl(const FabricState& state,
                                               const AcceleratorRecord& accelerator,
                                               const CapacityLedger& ledger,
                                               const PartitionRequest& request,
                                               std::uint64_t now_ms);

/// Full planning pass. Returns the materialised plan without mutating state
/// other than allocating a plan identity.
PartitionPlan build_plan(FabricState& state, const PartitionRequest& request,
                         std::uint64_t now_ms);

/// Reconfiguration plan from the current configuration to a desired set.
PartitionPlan build_reconfiguration_plan(FabricState& state, const AcceleratorId& accelerator,
                                         const std::vector<PlannedPartition>& desired,
                                         const PartitionRequest& context, std::uint64_t now_ms);

/// Admission evaluation. Pure: it never binds authority.
AdmissionProbe evaluate_admission(const FabricState& state, const WorkloadRequirement& requirement,
                                  std::uint64_t now_ms);

// --- mutation plumbing shared by the local and the distributed paths --------

/// Registers a physical mutation attempt. The record exists before anything is
/// dispatched, so a fast completion cannot race state registration.
Result<PartitionAttempt> register_attempt_locked(FabricState& state,
                                                 PartitionReservation& reservation,
                                                 AttemptKind kind, const WorkerId& worker,
                                                 const WorkerBootId& worker_boot,
                                                 std::uint64_t now_ms);

/// Applies an observed mutation outcome. Authority is published only when the
/// physical result has been verified against fresh evidence.
MutationOutcome apply_mutation_result_locked(FabricState& state,
                                             const PartitionAttemptId& attempt_id,
                                             const Result<BackendMutationResult>& mutation,
                                             const BackendLayout* observed, std::uint64_t now_ms);

/// Records the current generation as superseded and advances the record to the
/// next generation, so that a superseded generation is always strictly older
/// than the current one and can never be resurrected.
void supersede_partition_generation(FabricState& state, PartitionRecord& record);

/// Fresh, generation-bound evidence for a partition that was just observed.
void stamp_partition_evidence(FabricState& state, PartitionRecord& record,
                              const AcceleratorRecord& accelerator, std::uint64_t now_ms,
                              EvidenceProvenance provenance, const std::string& source);

}  // namespace apf
