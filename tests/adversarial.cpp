#include "support/fabric_fixture.hpp"
#include "support/test_framework.hpp"

#include "apf/client.hpp"
#include "apf/coordinator.hpp"
#include "apf/process.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

using namespace apf;

namespace {

struct LiveCoordinator {
  std::unique_ptr<PartitionCoordinator> coordinator;
  Endpoint endpoint{};
};

std::unique_ptr<LiveCoordinator> start_coordinator() {
  CoordinatorOptions options;
  options.listen.host = "127.0.0.1";
  options.listen.port = 0;
  options.fabric.instance_id = "adversarial";
  Result<std::unique_ptr<PartitionCoordinator>> created = PartitionCoordinator::create(options);
  if (!created.ok()) {
    return nullptr;
  }
  auto live = std::make_unique<LiveCoordinator>();
  live->coordinator = std::move(created.value());
  if (!live->coordinator->start().ok()) {
    return nullptr;
  }
  live->endpoint = live->coordinator->endpoint();
  return live;
}

/// Sends raw bytes to the coordinator and reports whether the connection was
/// closed without the coordinator accepting them.
bool raw_bytes_rejected(const Endpoint& endpoint, const std::vector<std::uint8_t>& bytes,
                        bool half_close = false) {
  Result<TcpSocket> socket = connect_to(endpoint);
  if (!socket.ok()) {
    return false;
  }
  (void)socket.value().send_all(bytes.data(), bytes.size());
  if (half_close) {
    // The attacker sends a partial frame and then stops sending, which is what
    // a truncated header looks like from the receiving side.
    socket.value().shutdown_both();
  }
  std::uint8_t buffer[64];
  Result<std::size_t> received = socket.value().recv_some(buffer, sizeof(buffer));
  // Rejection means either an error response or a closed connection; what must
  // never happen is a successful mutation.
  return !received.ok() || received.value() == 0 || buffer[0] != 0;
}

std::vector<std::uint8_t> make_header(std::uint32_t magic, std::uint16_t version,
                                      std::uint16_t type, std::uint32_t payload_length) {
  std::vector<std::uint8_t> out(56, 0);
  const auto put32 = [&out](std::size_t offset, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      out[offset + static_cast<std::size_t>(shift / 8)] =
          static_cast<std::uint8_t>((value >> shift) & 0xFFu);
    }
  };
  const auto put16 = [&out](std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  };
  put32(0, magic);
  put16(4, version);
  put16(6, type);
  put32(12, payload_length);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// State machine and authority attacks
// ---------------------------------------------------------------------------

APF_TEST(adversarial_illegal_transitions_are_refused) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  const PartitionId partition = created.value().partitions.front();
  // Retiring a partition and then trying to use its identity again must fail.
  CHECK_OK(fixture->fabric->begin_drain(partition, "attack"));
  CHECK_OK(fixture->fabric->complete_drain(partition));
  const Result<MutationOutcome> destroyed = fixture->fabric->destroy_partition(partition);
  REQUIRE(destroyed.ok());
  CHECK_ERR(fixture->fabric->begin_drain(partition, "resurrect"), ErrorCode::InvalidTransition);
  CHECK_ERR(fixture->fabric->destroy_partition(partition),
            ErrorCode::StalePartitionGeneration);
  CHECK_ERR(fixture->fabric->cancel_drain(partition), ErrorCode::InvalidTransition);
  // A release on a retired identity must not touch capacity.
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(adversarial_stale_release_and_double_commit_are_rejected) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  REQUIRE(reservation.ok());
  const Result<MutationOutcome> first = fixture->fabric->execute(reservation.value().id);
  REQUIRE(first.ok());
  CHECK(first.value().committed);
  // Re-executing a committed reservation must not create a second partition.
  const Result<MutationOutcome> second = fixture->fabric->execute(reservation.value().id);
  CHECK(!second.ok());
  CHECK_EQ(second.error().code, ErrorCode::StaleReservation);
  CHECK_EQ(fixture->count_state(PartitionState::Active), static_cast<std::size_t>(1));
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(adversarial_stale_async_completion_never_publishes_authority) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  REQUIRE(reservation.ok());
  const Result<PartitionAttempt> attempt = fixture->fabric->register_attempt(
      reservation.value().id, AttemptKind::CreatePartition, WorkerId::from_value(1),
      WorkerBootId::derive(11));
  REQUIRE(attempt.ok());
  // The worker incarnation dies: its attempt becomes an explicit unknown.
  CHECK_OK(fixture->fabric->mark_attempt_outcome_unknown(attempt.value().id, "worker died"));
  // A late completion now arrives. It must not publish authority.
  BackendMutationResult late;
  late.accepted = true;
  late.detail = "late acknowledgement";
  const Result<MutationOutcome> applied = fixture->fabric->apply_mutation_result(
      attempt.value().id, late, nullptr);
  REQUIRE(applied.ok());
  CHECK(!applied.value().committed);
  CHECK(applied.value().outcome_unknown);
  CHECK_EQ(fixture->count_state(PartitionState::Active), static_cast<std::size_t>(0));
  CHECK_EQ(fixture->count_state(PartitionState::RevalidationRequired), static_cast<std::size_t>(1));
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(adversarial_evidence_and_worker_authority_are_fenced) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  WorkerRecord worker;
  worker.id = WorkerId::from_value(7);
  worker.boot = WorkerBootId::derive(77);
  worker.backend = "synthetic";
  worker.alive = true;
  worker.device_count = 1;
  CHECK_OK(fixture->fabric->note_worker(worker));
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  const Result<PartitionReservation> bound = fixture->fabric->reserve_for_worker(
      plan.value().id, worker.id, worker.boot, fixture->fabric->coordinator_epoch());
  REQUIRE(bound.ok());
  CHECK_OK(fixture->fabric->fence_worker(worker.id, worker.boot, "test fence"));
  const Result<PartitionReservation> after = fixture->fabric->reservation(bound.value().id);
  REQUIRE(after.ok());
  CHECK_EQ(after.value().lifecycle, ReservationLifecycle::Fenced);
  // A replacement incarnation under the same worker identity fences the old one.
  WorkerRecord replacement = worker;
  replacement.boot = WorkerBootId::derive(78);
  CHECK_OK(fixture->fabric->note_worker(replacement));
  CHECK_ERR(fixture->fabric->reserve_for_worker(plan.value().id, worker.id, worker.boot,
                                                fixture->fabric->coordinator_epoch()),
            ErrorCode::StaleWorker);
  CHECK_OK(fixture->fabric->note_worker_seen(worker.id, replacement.boot, 1'500'000));
  CHECK_ERR(fixture->fabric->note_worker_seen(worker.id, worker.boot, 1'500'000),
            ErrorCode::StaleWorker);
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(adversarial_evidence_freshness_is_enforced) {
  apftest::FixtureOptions options;
  options.evidence_ttl_ms = 1'000;
  options.max_evidence_age_ms = 1'000;
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture(options);
  REQUIRE(fixture != nullptr);
  const Result<PartitionPlan> plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(plan.ok());
  REQUIRE(plan.value().feasible());
  fixture->manual_clock->advance(5'000);
  const Result<PartitionReservation> reservation = fixture->fabric->reserve(plan.value().id);
  CHECK(!reservation.ok());
  CHECK(reservation.error().code == ErrorCode::StaleEvidence ||
        reservation.error().code == ErrorCode::StalePlan);
  CHECK_EQ(fixture->count_state(PartitionState::Active), static_cast<std::size_t>(0));
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(adversarial_limits_are_enforced_on_growth) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  // Requesting more partitions than any bound allows is refused outright.
  PartitionRequest request;
  request.profile = fixture->profile("synthetic-1g");
  request.count = static_cast<std::uint32_t>(fixture->fabric->limits().max_partitions + 1);
  const Result<PartitionPlan> plan = fixture->fabric->plan(request);
  REQUIRE(plan.ok());
  CHECK_EQ(plan.value().outcome, PlanOutcome::LimitExceeded);
  // A profile name carrying invalid UTF-8 never reaches authoritative state.
  PartitionProfile profile;
  profile.id = PartitionProfileId::from_value(4242);
  profile.generation = PartitionProfileGeneration::first();
  profile.name = std::string("bad\xFF\xFEname");
  profile.backend = "synthetic";
  profile.mechanism = PartitionMechanism::Synthetic;
  profile.resources = *make_resource_vector({{ResourceDimension::MemoryBytes, gib(1)}});
  profile.compute_slice_count = 1;
  CHECK_ERR(fixture->fabric->register_profile(profile), ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Backend attacks
// ---------------------------------------------------------------------------

APF_TEST(adversarial_backend_faults_are_classified) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  // A positively refused creation returns the reservation to a usable state.
  fixture->backend->set_fault("fail_create", true);
  const Result<PartitionPlan> first_plan = fixture->plan("synthetic-2g", 1);
  REQUIRE(first_plan.ok());
  const Result<PartitionReservation> first_reservation =
      fixture->fabric->reserve(first_plan.value().id);
  REQUIRE(first_reservation.ok());
  const Result<MutationOutcome> failed = fixture->fabric->execute(first_reservation.value().id);
  REQUIRE(failed.ok());
  CHECK(!failed.value().committed);
  CHECK(!failed.value().outcome_unknown);
  CHECK_EQ(fixture->count_state(PartitionState::Reserved), static_cast<std::size_t>(1));
  CHECK_OK(fixture->fabric->verify_accounting());
  fixture->backend->set_fault("fail_create", false);
  const Result<MutationOutcome> retried = fixture->fabric->execute(first_reservation.value().id);
  REQUIRE(retried.ok());
  CHECK(retried.value().committed);

  // A call that does not complete leaves the outcome unknown, and the runtime
  // records that instead of assuming either result.
  fixture->backend->set_fault("error_create", true);
  const Result<PartitionPlan> unknown_plan = fixture->plan("synthetic-1g", 1);
  REQUIRE(unknown_plan.ok());
  const Result<PartitionReservation> unknown_reservation =
      fixture->fabric->reserve(unknown_plan.value().id);
  REQUIRE(unknown_reservation.ok());
  const Result<MutationOutcome> unknown =
      fixture->fabric->execute(unknown_reservation.value().id);
  REQUIRE(unknown.ok());
  CHECK(!unknown.value().committed);
  CHECK(unknown.value().outcome_unknown);
  CHECK(unknown.value().reconciliation_required);
  fixture->backend->set_fault("error_create", false);
  const Result<BackendLayout> observed = fixture->backend->query_layout(fixture->key);
  REQUIRE(observed.ok());
  CHECK_OK(fixture->fabric->reconcile(fixture->accelerator, observed.value()));
  CHECK_OK(fixture->fabric->verify_accounting());

  // A discovery failure is reported, never silently ignored.
  fixture->backend->set_fault("fail_discovery", true);
  std::vector<AcceleratorId> discovered;
  CHECK_ERR(fixture->fabric->discover(*fixture->backend, &discovered), ErrorCode::BackendFailure);
  fixture->backend->set_fault("fail_discovery", false);

  // A layout that reports the same native identity twice is rejected rather
  // than double counting capacity.
  fixture->backend->set_fault("duplicate_native_ids", true);
  const Result<BackendLayout> duplicated = fixture->backend->query_layout(fixture->key);
  REQUIRE(duplicated.ok());
  const Result<ReconciliationReport> rejected =
      fixture->fabric->reconcile(fixture->accelerator, duplicated.value());
  CHECK(!rejected.ok());
  fixture->backend->set_fault("duplicate_native_ids", false);

  // Malformed metadata is rejected by layout validation.
  fixture->backend->set_fault("malformed_metadata", true);
  const Result<BackendLayout> malformed = fixture->backend->query_layout(fixture->key);
  REQUIRE(malformed.ok());
  const Result<ReconciliationReport> malformed_report =
      fixture->fabric->reconcile(fixture->accelerator, malformed.value());
  CHECK(!malformed_report.ok());
  fixture->backend->set_fault("malformed_metadata", false);
  CHECK_OK(fixture->fabric->verify_accounting());
}

APF_TEST(adversarial_device_disappearance_offlines_partitions) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  CHECK_OK(fixture->backend->set_device_present(fixture->key, false));
  const Result<BackendLayout> layout = fixture->backend->query_layout(fixture->key);
  REQUIRE(layout.ok());
  CHECK(!layout.value().device_present);
  const Result<ReconciliationReport> report =
      fixture->fabric->reconcile(fixture->accelerator, layout.value());
  REQUIRE(report.ok());
  CHECK(!report.value().device_present);
  CHECK(report.value().revalidation_required);
  CHECK_EQ(fixture->count_state(PartitionState::Offline), static_cast<std::size_t>(1));
  CHECK_EQ(fixture->count_state(PartitionState::Active), static_cast<std::size_t>(0));
  CHECK_OK(fixture->fabric->verify_accounting());
}

// ---------------------------------------------------------------------------
// Protocol attacks against a live coordinator
// ---------------------------------------------------------------------------

APF_TEST(adversarial_protocol_corruption_is_rejected_before_mutation) {
  const std::unique_ptr<LiveCoordinator> live = start_coordinator();
  REQUIRE(live != nullptr);
  const Endpoint endpoint = live->endpoint;
  // Invalid magic.
  CHECK(raw_bytes_rejected(endpoint, make_header(0xDEADBEEF, APF_PROTOCOL_VERSION, 1, 0)));
  // Unsupported protocol version.
  CHECK(raw_bytes_rejected(endpoint, make_header(kFrameMagic, 99, 1, 0)));
  // Unknown message type.
  CHECK(raw_bytes_rejected(endpoint, make_header(kFrameMagic, APF_PROTOCOL_VERSION, 0xEEEE, 0)));
  // Declared payload length beyond the bound.
  std::vector<std::uint8_t> oversized =
      make_header(kFrameMagic, APF_PROTOCOL_VERSION, 1, 0x00FFFFFF);
  CHECK(raw_bytes_rejected(endpoint, oversized));
  // Truncated header.
  CHECK(raw_bytes_rejected(endpoint, std::vector<std::uint8_t>(10, 0), true));
  // A structurally valid frame with a corrupted integrity check.
  Frame frame;
  frame.type = MessageType::Hello;
  frame.payload = pack_payload(HelloMessage{}, Limits{});
  const Result<std::vector<std::uint8_t>> encoded = encode_frame(frame, Limits{});
  REQUIRE(encoded.ok());
  std::vector<std::uint8_t> corrupted = encoded.value();
  corrupted[20] = static_cast<std::uint8_t>(corrupted[20] ^ 0xFFu);
  CHECK(raw_bytes_rejected(endpoint, corrupted));

  // None of that may have changed any authoritative state.
  const std::shared_ptr<const FabricSnapshot> snapshot = live->coordinator->fabric().snapshot();
  CHECK_EQ(snapshot->partitions.size(), static_cast<std::size_t>(0));
  CHECK_EQ(snapshot->workers.size(), static_cast<std::size_t>(0));
  CHECK_OK(live->coordinator->fabric().verify_accounting());
  CHECK_OK(live->coordinator->stop());
}

APF_TEST(adversarial_protocol_payloads_are_validated) {
  const std::unique_ptr<LiveCoordinator> live = start_coordinator();
  REQUIRE(live != nullptr);
  const Result<std::unique_ptr<CoordinatorClient>> client =
      CoordinatorClient::connect(live->endpoint);
  REQUIRE(client.ok());
  // A command whose payload is truncated by the transport is rejected.
  Result<TcpSocket> socket = connect_to(live->endpoint);
  REQUIRE(socket.ok());
  FramedChannel channel(std::move(socket.value()), Limits{});
  Frame truncated;
  truncated.type = MessageType::CommandRequest;
  truncated.payload = {1, 2, 3};
  CHECK_OK(channel.send(truncated));
  const Result<Frame> reply = channel.receive();
  REQUIRE(reply.ok());
  CHECK_EQ(reply.value().type, MessageType::ErrorResponse);
  const Result<ErrorResponseMessage> error = unpack_payload<ErrorResponseMessage>(
      reply.value().payload.data(), reply.value().payload.size(), Limits{});
  REQUIRE(error.ok());
  CHECK_EQ(error.value().code, ErrorCode::ProtocolViolation);

  // A read-only client may not mutate state.
  CommandRequestMessage command;
  command.kind = CommandKind::Create;
  command.confirmed = false;
  Frame raw;
  raw.type = MessageType::CommandRequest;
  raw.payload = pack_payload(command, Limits{});
  Result<TcpSocket> second = connect_to(live->endpoint);
  REQUIRE(second.ok());
  FramedChannel second_channel(std::move(second.value()), Limits{});
  CHECK_OK(second_channel.send(raw));
  const Result<Frame> refusal = second_channel.receive();
  REQUIRE(refusal.ok());
  const Result<CommandResponseMessage> refusal_payload = unpack_payload<CommandResponseMessage>(
      refusal.value().payload.data(), refusal.value().payload.size(), Limits{});
  REQUIRE(refusal_payload.ok());
  CHECK(!refusal_payload.value().accepted);
  CHECK_EQ(refusal_payload.value().code, ErrorCode::PolicyRejected);
  CHECK_OK(live->coordinator->stop());
}

APF_TEST(adversarial_client_rejects_unknown_and_stale_authority) {
  const std::unique_ptr<LiveCoordinator> live = start_coordinator();
  REQUIRE(live != nullptr);
  const Result<std::unique_ptr<CoordinatorClient>> client =
      CoordinatorClient::connect(live->endpoint);
  REQUIRE(client.ok());
  // Inspecting an unknown device is an explicit not-found answer.
  const Result<CommandResponseMessage> layout = client.value()->query_layout("no-such-device");
  REQUIRE(layout.ok());
  CHECK(!layout.value().accepted);
  // Fencing an unknown worker is refused rather than silently accepted.
  const Result<CommandResponseMessage> fenced = client.value()->fence_worker(
      WorkerId::from_value(999), WorkerBootId::derive(1), "unknown worker");
  REQUIRE(fenced.ok());
  CHECK(!fenced.value().accepted);
  // Advancing the epoch fences nothing that does not exist, and stays coherent.
  const Result<CommandResponseMessage> advanced = client.value()->advance_epoch();
  REQUIRE(advanced.ok());
  CHECK(advanced.value().accepted);
  const Result<CommandResponseMessage> snapshot = client.value()->inspect_snapshot();
  REQUIRE(snapshot.ok());
  CHECK_EQ(snapshot.value().coordinator_epoch.value(), advanced.value().coordinator_epoch.value());
  CHECK_OK(live->coordinator->fabric().verify_accounting());
  CHECK_OK(live->coordinator->stop());
}

APF_TEST(adversarial_repeated_start_stop_is_clean) {
  for (int repeat = 0; repeat < 3; ++repeat) {
    const std::unique_ptr<LiveCoordinator> live = start_coordinator();
    REQUIRE(live != nullptr);
    const Result<std::unique_ptr<CoordinatorClient>> client =
        CoordinatorClient::connect(live->endpoint);
    REQUIRE(client.ok());
    const Result<CommandResponseMessage> snapshot = client.value()->inspect_snapshot();
    REQUIRE(snapshot.ok());
    client.value()->close();
    CHECK_OK(live->coordinator->stop());
    CHECK(live->coordinator->closed());
  }
}
