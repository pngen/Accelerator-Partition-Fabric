#include "support/fabric_fixture.hpp"
#include "support/test_framework.hpp"

#include "apf/process.hpp"

#include <algorithm>
#include <set>
#include <string>

using namespace apf;
using apftest::Fixture;
using apftest::make_fixture;

namespace {

/// A device whose compute slice geometry is the binding constraint while its
/// memory geometry is generous: this is what makes profile-shape fragmentation
/// observable rather than merely asserted.
apf::SyntheticDeviceSpec shape_limited_device(const std::string& key) {
  apf::SyntheticDeviceSpec spec;
  spec.key = key;
  spec.uuid = "SYN-" + key;
  spec.total_compute_slices = 1;
  spec.total_memory_slices = 8;
  spec.max_partition_count = 4;
  spec.physical_totals = *make_resource_vector({{ResourceDimension::ComputeShare, kShareScale},
                                                {ResourceDimension::MemoryBytes, gib(32)}});
  spec.isolation.add(IsolationProperty::LogicalSeparation);
  spec.isolation.add(IsolationProperty::MemoryIsolation);
  const auto add = [&](const std::string& name, std::uint32_t compute_slices,
                       std::uint32_t memory_slices, std::uint64_t ppm) {
    apf::SyntheticProfileSpec profile;
    profile.name = name;
    profile.vendor_native = name;
    profile.compute_slices = compute_slices;
    profile.memory_slices = memory_slices;
    profile.resources = *make_resource_vector(
        {{ResourceDimension::ComputeShare, ppm},
         {ResourceDimension::MemoryBytes, gib(4) * static_cast<std::uint64_t>(memory_slices)}});
    spec.profiles.push_back(std::move(profile));
  };
  add("shape-small", 1, 1, kShareScale / 2);
  add("shape-large", 1, 4, kShareScale / 2);
  return spec;
}

std::size_t count_state(const Fixture& fixture, PartitionState state) {
  std::size_t total = 0;
  for (const PartitionRecord& record : fixture.fabric->snapshot()->partitions) {
    if (record.state == state) {
      ++total;
    }
  }
  return total;
}

}  // namespace

// ---------------------------------------------------------------------------
// Planning and fragmentation
// ---------------------------------------------------------------------------

APF_TEST(planning_is_feasible_and_deterministic) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<PartitionPlan> first = fixture->plan("synthetic-2g", 2);
  REQUIRE(first.ok());
  CHECK_EQ(first.value().outcome, PlanOutcome::FeasibleNow);
  CHECK(first.value().feasible());
  CHECK_EQ(first.value().planned_partitions.size(), static_cast<std::size_t>(2));
  const Result<PartitionPlan> second = fixture->plan("synthetic-2g", 2);
  REQUIRE(second.ok());
  // An identical request against identical state returns the same plan.
  CHECK_EQ(first.value().id, second.value().id);
  CHECK_EQ(plan_digest(first.value()), plan_digest(second.value()));
}

APF_TEST(planning_reports_max_partition_count) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-1g", 8);
  REQUIRE(plan.ok());
CHECK_EQ(plan.value().outcome, PlanOutcome::MaxPartitionCountReached);
  CHECK(!plan.value().feasible());
}

APF_TEST(planning_reports_physically_impossible) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-3g", 3);
  REQUIRE(plan.ok());
CHECK_EQ(plan.value().outcome, PlanOutcome::PhysicallyImpossible);
}

APF_TEST(planning_reports_profile_shape_fragmentation) {
  apftest::FixtureOptions options;
  options.customize = [](SyntheticDeviceSpec& spec) { spec = shape_limited_device(spec.key); };
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture(options);
  REQUIRE(fixture != nullptr);
  // Occupy the only compute slice with the small profile.
  const Result<MutationOutcome> created = fixture->create("shape-small", 1);
  REQUIRE(created.ok());
  CHECK(created.value().committed);
  const Result<FragmentationReport> report =
      fixture->fabric->analyze_fragmentation(fixture->accelerator,
                                             [&]() {
                                               PartitionRequest request;
                                               request.profile = fixture->profile("shape-large");
                                               request.count = 1;
                                               return request;
                                             }());
  REQUIRE(report.ok());
  // Aggregate compute share and memory are both still sufficient, but no
  // compute slice remains: the failure is a shape failure, not a capacity one.
  CHECK(report.value().aggregate_capacity_sufficient);
  CHECK_EQ(report.value().classification, FragmentationClass::ProfileShape);
  CHECK_EQ(report.value().max_instances_now, 0u);
  CHECK(!report.value().feasible_now);
}

APF_TEST(planning_reports_trapped_capacity_and_requires_policy_for_destruction) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 3);
  REQUIRE(created.ok());
  const Result<PartitionPlan> refused = fixture->plan("synthetic-3g", 1);
  REQUIRE(refused.ok());
  CHECK_EQ(refused.value().outcome, PlanOutcome::PolicyRejected);
  // Destructive reconfiguration must be permitted by policy, not merely
  // requested by the caller.
  apftest::FixtureOptions permissive;
  permissive.allow_destructive_reconfiguration = true;
  const std::unique_ptr<Fixture> destructive = apftest::make_fixture(permissive);
  REQUIRE(destructive != nullptr);
  REQUIRE(destructive->create("synthetic-2g", 3).ok());
  const Result<PartitionPlan> allowed = destructive->plan("synthetic-3g", 1, true);
  REQUIRE(allowed.ok());
  CHECK_EQ(allowed.value().outcome, PlanOutcome::FeasibleAfterReconfiguration);
  CHECK(allowed.value().feasible());
}

APF_TEST(planning_prefers_the_deterministic_tie_break) {
  apftest::FixtureOptions options;
  SyntheticDeviceSpec second = make_default_synthetic_device("synthetic-1", 32, true);
  options.extra_devices.push_back(second);
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture(options);
  REQUIRE(fixture != nullptr);
  const std::vector<AcceleratorId> ids = fixture->fabric->accelerator_ids();
  REQUIRE(ids.size() == 2);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  CHECK_EQ(plan.value().outcome, PlanOutcome::FeasibleNow);
  // Both devices are identical, so the lowest accelerator identity must win.
  CHECK_EQ(plan.value().binding.accelerator, ids.front());
  bool saw_tie_break = false;
  for (const ExplanationFactor& factor : plan.value().explanation.factors()) {
    if (factor.code == ExplanationCode::TieBreak) {
      saw_tie_break = true;
    }
  }
  CHECK(saw_tie_break);
}

APF_TEST(plans_are_invalidated_by_capability_and_policy_changes) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  CHECK(plan.value().feasible());
  // A capability republication that materially changes the device invalidates
  // the plan, which must then be refused rather than executed.
  apf::SyntheticProfileSpec only_small;
  only_small.name = "synthetic-1g";
  only_small.vendor_native = "synthetic-1g";
  only_small.compute_slices = 1;
  only_small.memory_slices = 1;
  only_small.resources = *make_resource_vector({{ResourceDimension::ComputeShare, kShareScale / 7},
                                                 {ResourceDimension::MemoryBytes, gib(4)}});
  CHECK_OK(fixture->backend->mutate_capability(fixture->key, {only_small}, 7, true, ""));
  std::vector<AcceleratorId> discovered;
  CHECK_OK(fixture->fabric->discover(*fixture->backend, &discovered));
  // The refusal names the actual cause: the capability changed.
  const apf::Result<PartitionReservation> stale = fixture->fabric->reserve(plan.value().id);
  CHECK(!stale.ok());
  CHECK(stale.error().code == ErrorCode::CapabilityChanged ||
        stale.error().code == ErrorCode::StalePlan);

  const Result<PartitionPlan> fresh = fixture->plan("synthetic-1g", 1);
  REQUIRE(fresh.ok());
  CHECK(fresh.value().feasible());
  PlanningPolicy policy = fixture->fabric->policy();
  policy.max_evidence_age_ms = 5'000;
  CHECK_OK(fixture->fabric->set_policy(policy));
  CHECK_ERR(fixture->fabric->reserve(fresh.value().id), ErrorCode::PolicyChanged);
}

// ---------------------------------------------------------------------------
// Reservation, execution and verification
// ---------------------------------------------------------------------------

APF_TEST(reservation_commit_and_release_keep_accounting_exact) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const std::uint64_t free_before = fixture->free_memory();
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 2);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  REQUIRE(reservation.ok());
  CHECK_EQ(reservation.value().lifecycle, ReservationLifecycle::Active);
  CHECK_EQ(fixture->free_memory(), free_before - gib(16));
  CHECK_OK(fixture->fabric->verify_accounting());
  CHECK_EQ(count_state(*fixture, PartitionState::Reserved), static_cast<std::size_t>(2));
  const Result<MutationOutcome> outcome = fixture->fabric->execute(reservation.value().id);
  REQUIRE(outcome.ok());
  CHECK(outcome.value().committed);
  CHECK(outcome.value().verified_physically);
  CHECK(!outcome.value().outcome_unknown);
  CHECK_EQ(count_state(*fixture, PartitionState::Active), static_cast<std::size_t>(2));
  CHECK_OK(fixture->fabric->verify_accounting());
  CHECK_ERR(fixture->fabric->release_reservation(reservation.value().id, "too late"),
            ErrorCode::StaleReservation);
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(release_is_exactly_once_and_returns_capacity) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const std::uint64_t free_before = fixture->free_memory();
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  REQUIRE(reservation.ok());
  CHECK_OK(fixture->fabric->release_reservation(reservation.value().id, "cancelled"));
  CHECK_EQ(fixture->free_memory(), free_before);
  CHECK_ERR(fixture->fabric->release_reservation(reservation.value().id, "again"),
            ErrorCode::StaleReservation);
  CHECK_EQ(fixture->free_memory(), free_before);
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(each_logical_partition_claims_one_physical_partition) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  // Three partitions of the same profile are physically indistinguishable, so
  // binding by shape alone would let two logical partitions claim one physical
  // partition identity.
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 3);
  REQUIRE(created.ok());
  CHECK(created.value().committed);
  std::set<std::string> native_ids;
  for (const PartitionId& id : created.value().partitions) {
    const Result<PartitionRecord> record = fixture->fabric->partition(id);
    REQUIRE(record.ok());
    CHECK_EQ(record.value().state, PartitionState::Active);
    CHECK(!record.value().native_identity.native_id.empty());
    CHECK(native_ids.insert(record.value().native_identity.native_id).second);
  }
  CHECK_EQ(native_ids.size(), static_cast<std::size_t>(3));
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(execution_requires_verified_physical_state) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  fixture->backend->set_fault("mismatch_create_result", true);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  REQUIRE(reservation.ok());
  const Result<MutationOutcome> outcome = fixture->fabric->execute(reservation.value().id);
  REQUIRE(outcome.ok());
  // The backend claimed a partition that does not exist; authority is not
  // published and the partition requires reconciliation.
  CHECK(!outcome.value().committed);
  CHECK(outcome.value().outcome_unknown);
  CHECK(outcome.value().reconciliation_required);
  CHECK_EQ(count_state(*fixture, PartitionState::Active), static_cast<std::size_t>(0));
  CHECK_EQ(count_state(*fixture, PartitionState::RevalidationRequired), static_cast<std::size_t>(1));
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(ambiguous_acknowledgement_is_resolved_by_observation) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  // The mutation is applied and the acknowledgement is reported as ambiguous.
  // Because freshly observed state proves the result, the runtime is allowed to
  // commit: an observation outranks an acknowledgement.
  fixture->backend->set_fault("ambiguous_create", true);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  REQUIRE(reservation.ok());
  const Result<MutationOutcome> outcome = fixture->fabric->execute(reservation.value().id);
  REQUIRE(outcome.ok());
  CHECK(outcome.value().committed);
  CHECK(outcome.value().verified_physically);
  CHECK(!outcome.value().outcome_unknown);
  CHECK_EQ(count_state(*fixture, PartitionState::Active), static_cast<std::size_t>(1));
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(ambiguous_completion_is_classified_and_reconciled_exactly_once) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  // The mutation is applied, the acknowledgement is lost, and the resulting
  // partition is not observable. Nothing can prove what happened, so the
  // runtime records an explicit unknown outcome and refuses to replay.
  fixture->backend->set_fault("partition_disappears_after_create", true);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  REQUIRE(reservation.ok());
  const Result<MutationOutcome> outcome = fixture->fabric->execute(reservation.value().id);
  REQUIRE(outcome.ok());
  CHECK(!outcome.value().committed);
  CHECK(outcome.value().outcome_unknown);
  CHECK(outcome.value().reconciliation_required);
  const PartitionAttemptId attempt = outcome.value().attempt;
  CHECK_EQ(count_state(*fixture, PartitionState::Active), static_cast<std::size_t>(0));
  CHECK_EQ(count_state(*fixture, PartitionState::RevalidationRequired), static_cast<std::size_t>(1));
  // Reconciliation decides the case exactly once, from observed reality.
  const Result<BackendLayout> layout = fixture->backend->query_layout(fixture->key);
  REQUIRE(layout.ok());
  CHECK_EQ(layout.value().partitions.size(), static_cast<std::size_t>(0));
  const Result<ReconciliationReport> report =
      fixture->fabric->reconcile(fixture->accelerator, layout.value());
  REQUIRE(report.ok());
  const Result<PartitionAttempt> settled = fixture->fabric->attempt(attempt);
  REQUIRE(settled.ok());
  CHECK_EQ(settled.value().state, AttemptState::Reconciled);
  CHECK_EQ(count_state(*fixture, PartitionState::Active), static_cast<std::size_t>(0));
  CHECK_OK(fixture->fabric->verify_accounting());
  // The capacity the failed attempt held has returned to the free bucket.
  CHECK_EQ(fixture->free_memory(), gib(32));
  // Reconciling again changes nothing.
  const Result<ReconciliationReport> again =
      fixture->fabric->reconcile(fixture->accelerator, layout.value());
  REQUIRE(again.ok());
  CHECK_EQ(count_state(*fixture, PartitionState::Active), static_cast<std::size_t>(0));
  CHECK_EQ(fixture->free_memory(), gib(32));
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(stale_device_generation_fences_execution) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  REQUIRE(reservation.ok());
  // The device resets behind the runtime's back.
  std::vector<AcceleratorId> discovered;
  CHECK_OK(fixture->backend->reset_device(fixture->key));
  CHECK_OK(fixture->fabric->discover(*fixture->backend, &discovered));
  const Result<MutationOutcome> outcome = fixture->fabric->execute(reservation.value().id);
  CHECK(!outcome.ok());
  // The device incarnation changed, so the reservation loses authority: the
  // refusal names one of the generation-bound causes.
  CHECK(outcome.error().code == ErrorCode::StalePlan ||
        outcome.error().code == ErrorCode::StaleGeneration ||
        outcome.error().code == ErrorCode::StaleReservation ||
        outcome.error().code == ErrorCode::StaleEvidence ||
        outcome.error().code == ErrorCode::Fenced);
  const Result<PartitionReservation> after = fixture->fabric->reservation(reservation.value().id);
  REQUIRE(after.ok());
  CHECK(after.value().lifecycle == ReservationLifecycle::Fenced ||
        after.value().lifecycle == ReservationLifecycle::Released);
  CHECK_OK(fixture->fabric->verify_accounting());
}

// ---------------------------------------------------------------------------
// Admission and drain
// ---------------------------------------------------------------------------

APF_TEST(admission_binds_authority_and_respects_exclusivity) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  WorkloadRequirement requirement;
  requirement.workload_id = "workload-a";
  requirement.tenant_id = "tenant-a";
  requirement.required_profile = fixture->profile("synthetic-2g");
  requirement.minimum_resources = *make_resource_vector({{ResourceDimension::MemoryBytes, gib(8)}});
  const Result<AdmissionDecision> admitted = fixture->fabric->admit(requirement);
  REQUIRE(admitted.ok());
  CHECK_EQ(admitted.value().outcome, AdmissionOutcome::Admitted);
  CHECK(admitted.value().assignment.valid());
  WorkloadRequirement exclusive = requirement;
  exclusive.workload_id = "workload-b";
  exclusive.exclusive = true;
  const Result<AdmissionDecision> conflict = fixture->fabric->admit(exclusive);
  REQUIRE(conflict.ok());
  CHECK_EQ(conflict.value().outcome, AdmissionOutcome::RejectedExclusiveConflict);
  // A requirement the partition cannot satisfy is rejected explicitly.
  WorkloadRequirement too_large = requirement;
  too_large.workload_id = "workload-c";
  too_large.minimum_resources = *make_resource_vector({{ResourceDimension::MemoryBytes, gib(16)}});
  const Result<AdmissionDecision> rejected = fixture->fabric->admit(too_large);
  REQUIRE(rejected.ok());
  CHECK_EQ(rejected.value().outcome, AdmissionOutcome::RejectedInsufficientResources);
  CHECK_OK(fixture->fabric->unbind_assignment(admitted.value().assignment, "test complete"));
}

APF_TEST(drain_gates_destructive_mutation) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  const PartitionId partition = created.value().partitions.front();
  WorkloadRequirement requirement;
  requirement.workload_id = "workload-drain";
  requirement.required_profile = fixture->profile("synthetic-2g");
  const Result<AdmissionDecision> admitted = fixture->fabric->admit(requirement);
  REQUIRE(admitted.ok());
  CHECK_EQ(admitted.value().outcome, AdmissionOutcome::Admitted);
  // An occupied partition cannot be destroyed before it is drained.
  const Result<MutationOutcome> refused = fixture->fabric->destroy_partition(partition);
  CHECK(!refused.ok());
  CHECK_EQ(refused.error().code, ErrorCode::DrainRequired);
  CHECK_OK(fixture->fabric->begin_drain(partition, "test drain"));
  CHECK_ERR(fixture->fabric->complete_drain(partition), ErrorCode::DrainRequired);
  // New admission is rejected while draining.
  WorkloadRequirement second;
  second.workload_id = "workload-late";
  second.required_profile = fixture->profile("synthetic-2g");
  const Result<AdmissionDecision> after = fixture->fabric->admit(second);
  REQUIRE(after.ok());
  CHECK_EQ(after.value().outcome, AdmissionOutcome::RejectedDraining);
  CHECK_OK(fixture->fabric->unbind_assignment(admitted.value().assignment, "work released"));
  CHECK_OK(fixture->fabric->complete_drain(partition));
  const Result<MutationOutcome> destroyed = fixture->fabric->destroy_partition(partition);
  REQUIRE(destroyed.ok());
  CHECK(destroyed.value().committed);
  CHECK_OK(fixture->fabric->verify_accounting());
  CHECK_EQ(count_state(*fixture, PartitionState::Retired), static_cast<std::size_t>(1));
}

// ---------------------------------------------------------------------------
// Destructive reconfiguration
// ---------------------------------------------------------------------------

APF_TEST(reconfiguration_replaces_the_layout_and_fences_old_generations) {
  apftest::FixtureOptions options;
  options.allow_destructive_reconfiguration = true;
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture(options);
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 2);
  REQUIRE(created.ok());
  std::vector<std::pair<PartitionId, PartitionGeneration>> before;
  for (const PartitionRecord& record : fixture->fabric->snapshot()->partitions) {
    if (record.state == PartitionState::Active) {
      before.emplace_back(record.id, record.generation);
    }
  }
  REQUIRE(before.size() == 2);
  std::vector<PlannedPartition> desired;
  const PartitionProfile* profile = fixture->fabric->snapshot()->find_profile(
      fixture->profile("synthetic-3g"));
  REQUIRE(profile != nullptr);
  for (std::uint32_t ordinal = 0; ordinal < 2; ++ordinal) {
    PlannedPartition planned;
    planned.profile = profile->id;
    planned.profile_generation = profile->generation;
    planned.resources = profile->resources;
    planned.ordinal = ordinal;
    planned.vendor_native_profile = profile->vendor_native;
    desired.push_back(std::move(planned));
  }
  PartitionRequest context;
  context.allow_destructive_reconfiguration = true;
  context.profile = profile->id;
  context.count = 2;
  // A reconfiguration plan may not be reserved through the create path.
  const Result<PartitionPlan> plan =
      fixture->fabric->plan_reconfiguration(fixture->accelerator, desired, context);
  REQUIRE(plan.ok());
  CHECK_EQ(plan.value().outcome, PlanOutcome::FeasibleAfterReconfiguration);
  CHECK_ERR(fixture->fabric->reserve(plan.value().id), ErrorCode::ReconfigurationRequired);
  const Result<MutationOutcome> outcome =
      fixture->fabric->execute_reconfiguration(plan.value().id);
  REQUIRE(outcome.ok());
  if (!outcome.value().committed || outcome.value().partitions.size() != 2) {
  }
  CHECK(outcome.value().committed);
  CHECK_EQ(outcome.value().partitions.size(), static_cast<std::size_t>(2));
  CHECK_EQ(count_state(*fixture, PartitionState::Active), static_cast<std::size_t>(2));
  CHECK_OK(fixture->fabric->verify_accounting());
  // Every previous generation is fenced.
  for (const auto& entry : before) {
    const Result<PartitionRecord> record = fixture->fabric->partition(entry.first);
    REQUIRE(record.ok());
    CHECK_EQ(record.value().state, PartitionState::Retired);
    CHECK(record.value().supersedes(entry.second));
  }
  // The plan is now terminal: a stale destructive plan can never be re-executed.
  const Result<MutationOutcome> replay =
      fixture->fabric->execute_reconfiguration(plan.value().id);
  CHECK(!replay.ok());
}

APF_TEST(stale_destructive_reconfiguration_is_refused) {
  apftest::FixtureOptions options;
  options.allow_destructive_reconfiguration = true;
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture(options);
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  const PartitionProfile* profile =
      fixture->fabric->snapshot()->find_profile(fixture->profile("synthetic-3g"));
  REQUIRE(profile != nullptr);
  std::vector<PlannedPartition> desired;
  PlannedPartition planned;
  planned.profile = profile->id;
  planned.profile_generation = profile->generation;
  planned.resources = profile->resources;
  planned.vendor_native_profile = profile->vendor_native;
  desired.push_back(planned);
  PartitionRequest context;
  context.allow_destructive_reconfiguration = true;
  context.profile = profile->id;
  context.count = 1;
  const Result<PartitionPlan> plan =
      fixture->fabric->plan_reconfiguration(fixture->accelerator, desired, context);
  REQUIRE(plan.ok());
  REQUIRE(plan.value().feasible());
  // The layout changes after the plan was derived.
  const Result<MutationOutcome> extra = fixture->create("synthetic-1g", 1);
  REQUIRE(extra.ok());
  const Result<MutationOutcome> refused =
      fixture->fabric->execute_reconfiguration(plan.value().id);
  CHECK(!refused.ok());
  CHECK(refused.error().code == ErrorCode::StalePlan ||
        refused.error().code == ErrorCode::StaleGeneration);
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

APF_TEST(reconciliation_reports_missing_unexpected_and_reset) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  const PartitionId partition = created.value().partitions.front();
  const Result<PartitionRecord> record = fixture->fabric->partition(partition);
  REQUIRE(record.ok());
  const std::string native_id = record.value().native_identity.native_id;
  REQUIRE(!native_id.empty());
  // The partition disappears externally.
  CHECK_OK(fixture->backend->external_destroy(fixture->key, native_id));
  const Result<BackendLayout> layout = fixture->backend->query_layout(fixture->key);
  REQUIRE(layout.ok());
  const Result<ReconciliationReport> report =
      fixture->fabric->reconcile(fixture->accelerator, layout.value());
  REQUIRE(report.ok());
  CHECK_EQ(report.value().missing, 1u);
  const Result<PartitionRecord> after = fixture->fabric->partition(partition);
  REQUIRE(after.ok());
  CHECK_EQ(after.value().state, PartitionState::Retired);
  CHECK_OK(fixture->fabric->verify_accounting());
  // An unexplained physical partition is recorded but not adopted.
  std::string created_native;
  CHECK_OK(fixture->backend->external_create(fixture->key, "synthetic-1g", &created_native));
  const Result<BackendLayout> second_layout = fixture->backend->query_layout(fixture->key);
  REQUIRE(second_layout.ok());
  const Result<ReconciliationReport> second =
      fixture->fabric->reconcile(fixture->accelerator, second_layout.value());
  REQUIRE(second.ok());
  CHECK_EQ(second.value().unexpected, 1u);
  CHECK_EQ(second.value().adopted, 0u);
  CHECK(second.value().revalidation_required);
  CHECK_EQ(count_state(*fixture, PartitionState::Active), static_cast<std::size_t>(0));
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(reconciliation_detects_device_reset_and_requires_revalidation) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  CHECK_OK(fixture->backend->reset_device(fixture->key));
  const Result<BackendLayout> layout = fixture->backend->query_layout(fixture->key);
  REQUIRE(layout.ok());
  const Result<ReconciliationReport> report =
      fixture->fabric->reconcile(fixture->accelerator, layout.value());
  REQUIRE(report.ok());
  CHECK(report.value().device_generation_changed);
  CHECK_EQ(count_state(*fixture, PartitionState::Active), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Persistence and restart behaviour
// ---------------------------------------------------------------------------

APF_TEST(durable_state_round_trips_through_the_store_and_requires_revalidation) {
  const Result<std::string> directory = make_temp_directory("apf-persist");
  REQUIRE(directory.ok());
  const std::string path = join_path(directory.value(), "fabric.state");
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  PersistenceStore store;
  CHECK_OK(store.set_path(path));
  CHECK_OK(fixture->fabric->save(store));

  // A fresh instance loads the durable structure but not live authority.
  apf::FabricOptions options;
  options.instance_id = "restarted";
  PartitionFabric restarted(options);
  const Status loaded = restarted.load(store);
  CHECK_OK(loaded);
  const std::shared_ptr<const FabricSnapshot> snapshot = restarted.snapshot();
  CHECK_EQ(snapshot->accelerators.size(), static_cast<std::size_t>(1));
  CHECK_EQ(snapshot->partitions.size(), static_cast<std::size_t>(1));
  CHECK_OK(restarted.verify_accounting());
  std::size_t revalidating = 0;
  for (const PartitionRecord& record : snapshot->partitions) {
    if (record.state == PartitionState::RevalidationRequired) {
      ++revalidating;
    }
  }
  CHECK_EQ(revalidating, static_cast<std::size_t>(1));
  // Persisted dynamic evidence never becomes current again.
  const AcceleratorRecord& accelerator = snapshot->accelerators.front().accelerator;
  CHECK(!accelerator.evidence.is_fresh_at(fixture->clock->now_ms()));
  // The restarted instance cannot authorise a mutation without fresh evidence.
  const Result<PartitionPlan> plan = restarted.plan([&]() {
    PartitionRequest request;
    request.profile = snapshot->profiles.front().id;
    request.count = 1;
    return request;
  }());
  REQUIRE(plan.ok());
  CHECK(!plan.value().feasible());
  CHECK_OK(remove_directory_recursive(directory.value()));
}

APF_TEST(persistence_rejects_corrupt_state_without_partial_application) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  const Result<DurableState> durable = fixture->fabric->export_durable_state();
  REQUIRE(durable.ok());
  Limits limits;
  const Result<std::vector<std::uint8_t>> encoded =
      PersistenceStore::encode(durable.value(), limits);
  REQUIRE(encoded.ok());
  // Every truncation boundary is rejected.
  for (std::size_t cut = 0; cut < encoded.value().size(); cut += 5) {
    std::vector<std::uint8_t> truncated(encoded.value().begin(),
                                        encoded.value().begin() + static_cast<std::ptrdiff_t>(cut));
    CHECK(!PersistenceStore::decode(truncated.data(), truncated.size(), limits).ok());
  }
  // Corrupt magic, version, byte-order marker and payload hash.
  std::vector<std::uint8_t> corrupted = encoded.value();
  corrupted[0] = 'X';
  CHECK(!PersistenceStore::decode(corrupted.data(), corrupted.size(), limits).ok());
  corrupted = encoded.value();
  corrupted[8] = 9;
  CHECK(!PersistenceStore::decode(corrupted.data(), corrupted.size(), limits).ok());
  corrupted = encoded.value();
  corrupted[12] = 9;
  CHECK(!PersistenceStore::decode(corrupted.data(), corrupted.size(), limits).ok());
  corrupted = encoded.value();
  corrupted[24] = static_cast<std::uint8_t>(corrupted[24] ^ 0xFFu);
  CHECK(!PersistenceStore::decode(corrupted.data(), corrupted.size(), limits).ok());
  corrupted = encoded.value();
  corrupted.back() = static_cast<std::uint8_t>(corrupted.back() ^ 0xFFu);
  CHECK(!PersistenceStore::decode(corrupted.data(), corrupted.size(), limits).ok());
  // A declared payload length beyond the container bound is rejected.
  corrupted = encoded.value();
  const std::uint64_t huge = 1ull << 40;
  for (int index = 0; index < 8; ++index) {
    corrupted[16 + static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((huge >> (index * 8)) & 0xFFu);
  }
  CHECK(!PersistenceStore::decode(corrupted.data(), corrupted.size(), limits).ok());
}