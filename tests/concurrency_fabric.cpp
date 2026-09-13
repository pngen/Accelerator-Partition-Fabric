#include "support/fabric_fixture.hpp"
#include "support/test_framework.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace apf;

namespace {

/// Runs the body on several threads and joins all of them. No timeouts: the
/// test completes when the threads complete.
template <class Body>
void run_threads(std::size_t count, Body body) {
  std::vector<std::thread> threads;
  threads.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    threads.emplace_back([index, &body]() { body(index); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

}  // namespace

APF_TEST(concurrency_only_one_reservation_wins_the_last_slot) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  // Six single-slice partitions consume six of the seven compute slices, so
  // exactly one racing request can win the remaining slice.
  const Result<MutationOutcome> preload = fixture->create("synthetic-1g", 6);
  REQUIRE(preload.ok());
  CHECK(preload.value().committed);

  std::vector<PartitionReservationId> winners;
  std::vector<ErrorCode> losers;
  std::mutex mutex;
  run_threads(4, [&](std::size_t) {
    const Result<PartitionPlan> raced = fixture->plan("synthetic-1g", 1);
    if (!raced.ok()) {
      return;
    }
    const Result<PartitionReservation> reservation =
        fixture->fabric->reserve(raced.value().id);
    std::lock_guard<std::mutex> lock(mutex);
    if (reservation.ok()) {
      winners.push_back(reservation.value().id);
    } else {
      losers.push_back(reservation.error().code);
    }
  });
  CHECK_EQ(winners.size(), static_cast<std::size_t>(1));
  CHECK_EQ(losers.size(), static_cast<std::size_t>(3));
  for (const ErrorCode code : losers) {
    CHECK(code == ErrorCode::InsufficientCapacity || code == ErrorCode::Fragmented ||
          code == ErrorCode::PolicyRejected || code == ErrorCode::StalePlan);
  }
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(concurrency_release_is_exactly_once) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  REQUIRE(reservation.ok());
  std::atomic<int> successes{0};
  std::atomic<int> failures{0};
  run_threads(4, [&](std::size_t) {
    const Status status =
        fixture->fabric->release_reservation(reservation.value().id, "raced release");
    if (status.ok()) {
      ++successes;
    } else {
      ++failures;
    }
  });
  CHECK_EQ(successes.load(), 1);
  CHECK_EQ(failures.load(), 3);
  CHECK_OK(fixture->fabric->verify_accounting());
  CHECK_EQ(fixture->free_memory(), gib(32));
}

APF_TEST(concurrency_drain_races_admission) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-3g", 1);
  REQUIRE(created.ok());
  const PartitionId partition = created.value().partitions.front();
  std::vector<AdmissionDecision> decisions;
  std::mutex mutex;
  std::atomic<bool> drained{false};
  run_threads(5, [&](std::size_t index) {
    if (index == 0) {
      CHECK_OK(fixture->fabric->begin_drain(partition, "raced drain"));
      drained.store(true);
      return;
    }
    WorkloadRequirement requirement;
    requirement.workload_id = "raced-workload-" + std::to_string(index);
    requirement.required_profile = fixture->profile("synthetic-3g");
    const Result<AdmissionDecision> decision = fixture->fabric->admit(requirement);
    std::lock_guard<std::mutex> lock(mutex);
    if (decision.ok()) {
      decisions.push_back(decision.value());
    }
  });
  const Result<PartitionRecord> record = fixture->fabric->partition(partition);
  REQUIRE(record.ok());
  CHECK_EQ(record.value().state, PartitionState::Draining);
  // Every admission that succeeded must have been committed before the drain
  // started; nothing may slip past the drain gate afterwards.
  for (const AdmissionDecision& decision : decisions) {
    if (decision.admitted()) {
      CHECK(decision.assignment.valid());
    } else {
      CHECK(decision.outcome == AdmissionOutcome::RejectedDraining ||
            decision.outcome == AdmissionOutcome::RejectedStaleAuthority);
    }
  }
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(concurrency_capability_publication_races_planning) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  std::atomic<int> stale_failures{0};
  std::atomic<int> successful_reservations{0};
  run_threads(4, [&](std::size_t index) {
    if (index == 3) {
      // Republish capability repeatedly while the others plan and reserve.
      for (int repeat = 0; repeat < 8; ++repeat) {
        std::vector<AcceleratorId> discovered;
        (void)fixture->fabric->discover(*fixture->backend, &discovered);
      }
      return;
    }
    for (int repeat = 0; repeat < 12; ++repeat) {
      const Result<PartitionPlan> plan = fixture->plan("synthetic-1g", 1);
      if (!plan.ok() || !plan.value().feasible()) {
        continue;
      }
      const Result<PartitionReservation> reservation =
          fixture->fabric->reserve(plan.value().id);
      if (reservation.ok()) {
        ++successful_reservations;
        (void)fixture->fabric->release_reservation(reservation.value().id, "raced");
      } else if (reservation.error().code == ErrorCode::StalePlan ||
                 reservation.error().code == ErrorCode::CapabilityChanged ||
                 reservation.error().code == ErrorCode::StaleEvidence ||
                 reservation.error().code == ErrorCode::StaleReservation) {
        ++stale_failures;
      }
    }
  });
  CHECK_OK(fixture->fabric->verify_accounting());
  CHECK(successful_reservations.load() > 0 || stale_failures.load() > 0);
  // Accounting must be back at the baseline because every reservation was
  // either released or refused before capacity was held.
  CHECK_EQ(fixture->free_memory(), gib(32));
}

APF_TEST(concurrency_snapshots_race_mutation) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  std::atomic<bool> stop{false};
  std::atomic<int> inconsistent{0};
  std::atomic<int> snapshots{0};
  run_threads(5, [&](std::size_t index) {
    if (index == 4) {
      for (int repeat = 0; repeat < 20; ++repeat) {
        const Result<MutationOutcome> outcome = fixture->create("synthetic-1g", 1);
        (void)outcome;
        for (const PartitionId& id : fixture->partitions_of(fixture->accelerator)) {
          (void)fixture->fabric->begin_drain(id, "raced snapshot drain");
        }
      }
      stop.store(true);
      return;
    }
    while (!stop.load()) {
      const std::shared_ptr<const FabricSnapshot> snapshot = fixture->fabric->snapshot();
      ++snapshots;
      for (const AcceleratorView& view : snapshot->accelerators) {
        const Status closed = view.ledger.validate();
        if (!closed.ok()) {
          ++inconsistent;
        }
      }
      // Rendering under concurrent mutation must be well defined.
      const std::string rendered = snapshot->render();
      if (rendered.empty()) {
        ++inconsistent;
      }
    }
  });
  CHECK_EQ(inconsistent.load(), 0);
  CHECK(snapshots.load() > 0);
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(concurrency_epoch_advance_fences_older_reservations) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  std::vector<PartitionReservationId> reservations;
  std::mutex mutex;
  run_threads(4, [&](std::size_t index) {
    if (index == 3) {
      for (int repeat = 0; repeat < 4; ++repeat) {
        (void)fixture->fabric->advance_coordinator_epoch();
      }
      return;
    }
    for (int repeat = 0; repeat < 6; ++repeat) {
      const Result<PartitionPlan> plan = fixture->plan("synthetic-1g", 1);
      if (!plan.ok() || !plan.value().feasible()) {
        continue;
      }
      const Result<PartitionReservation> reservation =
          fixture->fabric->reserve(plan.value().id);
      if (reservation.ok()) {
        std::lock_guard<std::mutex> lock(mutex);
        reservations.push_back(reservation.value().id);
      }
    }
  });
  CHECK_OK(fixture->fabric->verify_accounting());
  const CoordinatorEpoch epoch = fixture->fabric->coordinator_epoch();
  for (const PartitionReservationId& id : reservations) {
    const Result<PartitionReservation> reservation = fixture->fabric->reservation(id);
    REQUIRE(reservation.ok());
    // Any reservation that held capacity under a superseded epoch is fenced.
    if (reservation.value().coordinator_epoch != epoch) {
      CHECK(reservation.value().lifecycle == ReservationLifecycle::Fenced ||
            reservation.value().lifecycle == ReservationLifecycle::Released ||
            reservation.value().lifecycle == ReservationLifecycle::Committed);
    }
  }
}

APF_TEST(concurrency_create_races_destroy) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  const PartitionId partition = created.value().partitions.front();
  CHECK_OK(fixture->fabric->begin_drain(partition, "prepare for raced destruction"));
  CHECK_OK(fixture->fabric->complete_drain(partition));
  std::atomic<int> committed{0};
  std::atomic<int> refused{0};
  run_threads(4, [&](std::size_t) {
    const Result<MutationOutcome> outcome = fixture->fabric->destroy_partition(partition);
    if (outcome.ok() && outcome.value().committed) {
      ++committed;
    } else {
      ++refused;
    }
  });
  CHECK_EQ(committed.load(), 1);
  CHECK_EQ(refused.load(), 3);
  const Result<PartitionRecord> record = fixture->fabric->partition(partition);
  REQUIRE(record.ok());
  CHECK_EQ(record.value().state, PartitionState::Retired);
  CHECK_OK(fixture->fabric->verify_accounting());
  CHECK_EQ(fixture->free_memory(), gib(32));
}

APF_TEST(concurrency_shutdown_is_idempotent_and_releases_pending_capacity) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 2);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  REQUIRE(reservation.ok());
  const Result<MutationOutcome> created = fixture->create("synthetic-1g", 1);
  REQUIRE(created.ok());
  std::atomic<int> closed_ok{0};
  run_threads(3, [&](std::size_t) {
    if (fixture->fabric->close().ok()) {
      ++closed_ok;
    }
  });
  CHECK_EQ(closed_ok.load(), 3);
  CHECK(fixture->fabric->closed());
  // Committed partitions keep their capacity; the uncommitted reservation has
  // released exactly once.
  CHECK_OK(fixture->fabric->verify_accounting());
  const Result<PartitionReservation> after = fixture->fabric->reservation(reservation.value().id);
  REQUIRE(after.ok());
  CHECK_EQ(after.value().lifecycle, ReservationLifecycle::Released);
  CHECK_OK(fixture->fabric->reopen());
  CHECK(!fixture->fabric->closed());
  CHECK_OK(fixture->fabric->verify_accounting());
}
