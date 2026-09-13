#include "support/fabric_fixture.hpp"
#include "support/test_framework.hpp"

#include "apf/process.hpp"

#include <algorithm>
#include <set>

using namespace apf;
using apftest::Fixture;
using apftest::make_fixture;

namespace {

/// One randomised operation. The generator only produces operations that are
/// meaningful for the current state; the invariant checks after every step are
/// what make the sequence a proof rather than a smoke test.
struct Model {
  std::unique_ptr<Fixture> fixture;
  std::vector<PartitionId> known;
  std::set<std::uint64_t> retired;
  std::vector<PartitionReservationId> live_reservations;
  std::uint64_t operations{0};
};

Status check_invariants(Model& model, const char* where) {
  const Status accounting = model.fixture->fabric->verify_accounting();
  if (!accounting.ok()) {
    return failure(ErrorCode::CorruptState, std::string("accounting violated at ") + where,
                   to_string(accounting.error()));
  }
  const std::shared_ptr<const FabricSnapshot> snapshot = model.fixture->fabric->snapshot();
  for (const PartitionRecord& record : snapshot->partitions) {
    if (record.state == PartitionState::Retired) {
      model.retired.insert(record.id.value());
    }
    if (model.retired.count(record.id.value()) != 0 && record.state != PartitionState::Retired) {
      return failure(ErrorCode::CorruptState,
                     "a retired partition generation was resurrected: " + record.id.str());
    }
    if (record.state == PartitionState::Active) {
      const AcceleratorView* view = snapshot->find_accelerator(record.accelerator);
      if (view == nullptr) {
        return failure(ErrorCode::CorruptState, "active partition has no accelerator");
      }
      if (!record.evidence.is_observed()) {
        return failure(ErrorCode::CorruptState,
                       "active partition has no observed evidence: " + record.id.str());
      }
      if (view->accelerator.generation != record.accelerator_generation) {
        return failure(ErrorCode::CorruptState,
                       "active partition is bound to a superseded device generation: " +
                           record.id.str());
      }
      const Status valid = record.validate();
      if (!valid.ok()) {
        return failure(valid.error().code, to_string(valid.error()), record.id.str());
      }
    }
  }
  // Accounting must close with the sum of held buckets matching the totals.
  for (const AcceleratorView& view : snapshot->accelerators) {
    const Status closed = view.ledger.validate();
    if (!closed.ok()) {
      return failure(ErrorCode::CorruptState, to_string(closed.error()));
    }
  }
  return success();
}

Status run_sequence(std::uint64_t seed, std::uint32_t steps, std::string* trace) {
  apftest::Rng rng(seed);
  Model model;
  apftest::FixtureOptions options;
  options.allow_destructive_reconfiguration = true;
  options.key = "prop-0";
  // A second device makes device selection part of the randomised state.
  options.extra_devices.push_back(make_default_synthetic_device("prop-1", 32, true));
  model.fixture = apftest::make_fixture(options);
  if (model.fixture == nullptr) {
    return failure(ErrorCode::Internal, "fixture construction failed");
  }
  const std::vector<std::string> profile_names = {"synthetic-1g", "synthetic-2g", "synthetic-3g",
                                                  "synthetic-7g"};
  for (std::uint32_t step = 0; step < steps; ++step) {
    const std::uint32_t choice = rng.below(100);
    *trace = "seed=" + std::to_string(seed) + " step=" + std::to_string(step) +
             " choice=" + std::to_string(choice);
    if (choice < 22) {
      const std::string name = profile_names[rng.below(4)];
      const std::uint32_t count = 1 + rng.below(3);
      const Result<PartitionPlan> plan = model.fixture->plan(name, count, true, true);
      if (!plan.ok()) {
        continue;
      }
      const Result<PartitionReservation> reservation =
          model.fixture->fabric->reserve(plan.value().id);
      if (!reservation.ok()) {
        continue;
      }
      model.live_reservations.push_back(reservation.value().id);
      const Result<MutationOutcome> outcome =
          model.fixture->fabric->execute(reservation.value().id);
      if (outcome.ok()) {
        for (const PartitionId id : outcome.value().partitions) {
          model.known.push_back(id);
        }
      }
    } else if (choice < 30) {
      if (!model.live_reservations.empty()) {
        const PartitionReservationId id =
            model.live_reservations[rng.below(static_cast<std::uint32_t>(
                model.live_reservations.size()))];
        (void)model.fixture->fabric->release_reservation(id, "property release");
      }
    } else if (choice < 40) {
      if (!model.known.empty()) {
        const PartitionId id =
            model.known[rng.below(static_cast<std::uint32_t>(model.known.size()))];
        (void)model.fixture->fabric->begin_drain(id, "property drain");
      }
    } else if (choice < 50) {
      if (!model.known.empty()) {
        const PartitionId id =
            model.known[rng.below(static_cast<std::uint32_t>(model.known.size()))];
        (void)model.fixture->fabric->complete_drain(id);
      }
    } else if (choice < 60) {
      if (!model.known.empty()) {
        const PartitionId id =
            model.known[rng.below(static_cast<std::uint32_t>(model.known.size()))];
        const Result<MutationOutcome> outcome = model.fixture->fabric->destroy_partition(id);
        if (outcome.ok() && outcome.value().committed) {
          model.retired.insert(id.value());
        }
      }
    } else if (choice < 68) {
      const std::string name = profile_names[rng.below(4)];
      const std::uint32_t count = 1 + rng.below(2);
      (void)model.fixture->plan(name, count, true, true);
    } else if (choice < 74) {
      const PartitionProfile* profile = model.fixture->fabric->snapshot()->find_profile(
          model.fixture->profile("synthetic-2g"));
      if (profile != nullptr) {
        std::vector<PlannedPartition> desired;
        const std::uint32_t count = 1 + rng.below(2);
        for (std::uint32_t ordinal = 0; ordinal < count; ++ordinal) {
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
        context.count = count;
        const Result<PartitionPlan> plan = model.fixture->fabric->plan_reconfiguration(
            model.fixture->accelerator, desired, context);
        if (plan.ok() && plan.value().feasible()) {
          (void)model.fixture->fabric->execute_reconfiguration(plan.value().id);
        }
      }
    } else if (choice < 80) {
      // External mutation behind the runtime's back.
      const std::string key = rng.below(2) == 0 ? "prop-0" : "prop-1";
      std::string native_id;
      (void)model.fixture->backend->external_create(key, "synthetic-1g", &native_id);
      const Result<BackendLayout> layout = model.fixture->backend->query_layout(key);
      if (layout.ok()) {
        const Result<AcceleratorId> accelerator =
            model.fixture->fabric->accelerator_for_key(key);
        if (accelerator.ok()) {
          (void)model.fixture->fabric->reconcile(accelerator.value(), layout.value());
        }
      }
    } else if (choice < 84) {
      const std::string key = rng.below(2) == 0 ? "prop-0" : "prop-1";
      (void)model.fixture->backend->reset_device(key);
      std::vector<AcceleratorId> discovered;
      (void)model.fixture->fabric->discover(*model.fixture->backend, &discovered);
    } else if (choice < 88) {
      apf::Result<DurableState> durable = model.fixture->fabric->export_durable_state();
      if (durable.ok()) {
        Limits limits;
        const Result<std::vector<std::uint8_t>> encoded =
            PersistenceStore::encode(durable.value(), limits);
        if (!encoded.ok()) {
          return failure(encoded.error().code, to_string(encoded.error()));
        }
        const Result<DurableState> decoded = PersistenceStore::decode(
            encoded.value().data(), encoded.value().size(), limits);
        if (!decoded.ok()) {
          return failure(decoded.error().code, to_string(decoded.error()));
        }
      }
    } else if (choice < 92) {
      (void)model.fixture->fabric->advance_coordinator_epoch();
    } else if (choice < 96) {
      WorkloadRequirement requirement;
      requirement.workload_id = "prop-workload-" + std::to_string(step);
      const Result<AdmissionDecision> decision = model.fixture->fabric->admit(requirement);
      if (decision.ok() && decision.value().admitted()) {
        (void)model.fixture->fabric->unbind_assignment(decision.value().assignment, "property");
      }
    } else {
      (void)model.fixture->fabric->snapshot();
    }
    const Status invariants = check_invariants(model, trace->c_str());
    if (!invariants.ok()) {
      *trace += " | " + to_string(invariants.error());
      return invariants;
    }
    model.operations += 1;
  }
  return success();
}

std::string render_fixture(const Fixture& fixture) {
  // The logical rendering excludes the volatile snapshot header, so two
  // instances in the same logical state render identically.
  return fixture.fabric->snapshot()->render_logical();
}

}  // namespace

APF_TEST(property_randomised_sequences_preserve_every_invariant) {
  const std::uint64_t seeds[] = {1, 0xC0FFEE, 0x5EED5EED, 42, 7777};
  for (const std::uint64_t seed : seeds) {
    std::string trace;
    const Status status = run_sequence(seed, 320, &trace);
    if (!status.ok()) {
      std::fprintf(stderr, "property failure: %s\n  %s\n", to_string(status.error()).c_str(),
                   trace.c_str());
    }
    CHECK(status.ok());
  }
}

APF_TEST(property_identical_sequences_produce_identical_decisions) {
  std::string first_trace;
  std::string second_trace;
  const Status first = run_sequence(0xA11CE, 200, &first_trace);
  const Status second = run_sequence(0xA11CE, 200, &second_trace);
  CHECK(first.ok());
  CHECK(second.ok());
  CHECK_EQ(first_trace, second_trace);

  apftest::FixtureOptions options;
  const std::unique_ptr<Fixture> left = apftest::make_fixture(options);
  const std::unique_ptr<Fixture> right = apftest::make_fixture(options);
  REQUIRE(left != nullptr);
  REQUIRE(right != nullptr);
  for (int step = 0; step < 4; ++step) {
    const std::string name = step % 2 == 0 ? "synthetic-2g" : "synthetic-1g";
    const Result<MutationOutcome> a = left->create(name, 1);
    const Result<MutationOutcome> b = right->create(name, 1);
    CHECK_EQ(a.ok(), b.ok());
    if (!a.ok()) {
      CHECK_EQ(a.error().code, b.error().code);
    }
  }
  CHECK_EQ(render_fixture(*left), render_fixture(*right));
  CHECK_EQ(snapshot_digest(*left->fabric->snapshot()), snapshot_digest(*right->fabric->snapshot()));
}

APF_TEST(property_failed_transactions_leave_state_unchanged) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-7g", 1);
  REQUIRE(created.ok());
  const std::string before = render_fixture(*fixture);
  // Every rejected request must leave the registry and the ledger untouched.
  const Result<PartitionPlan> impossible = fixture->plan("synthetic-2g", 1, true, true);
  REQUIRE(impossible.ok());
  CHECK(!impossible.value().feasible());
  CHECK_ERR(fixture->fabric->reserve(impossible.value().id), ErrorCode::PolicyRejected);
  CHECK_EQ(render_fixture(*fixture), before);
  const Result<PartitionPlan> oversized = fixture->plan("synthetic-3g", 5, true, true);
  REQUIRE(oversized.ok());
  CHECK(!oversized.value().feasible());
  CHECK(!fixture->fabric->reserve(oversized.value().id).ok());
  CHECK_EQ(render_fixture(*fixture), before);
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(property_capacity_never_goes_negative_under_pressure) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  std::size_t created = 0;
  for (int attempt = 0; attempt < 24; ++attempt) {
    const Result<MutationOutcome> outcome = fixture->create("synthetic-1g", 1);
    if (outcome.ok() && outcome.value().committed) {
      ++created;
    }
    CHECK_OK(fixture->fabric->verify_accounting());
    const Result<CapacityLedger> ledger = fixture->fabric->ledger(fixture->accelerator);
    REQUIRE(ledger.ok());
    for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
      const auto dimension = static_cast<ResourceDimension>(index);
      std::uint64_t sum = 0;
      for (std::size_t bucket = 0; bucket < kCapacityBucketCount; ++bucket) {
        sum += ledger.value().bucket(static_cast<CapacityBucket>(bucket)).get(dimension);
      }
      CHECK(sum == ledger.value().total().get(dimension));
    }
  }
  // The device has seven compute slices and eight memory slices, so at most
  // seven single-slice partitions can ever be created.
  CHECK_EQ(created, static_cast<std::size_t>(7));
}
