#include "support/fabric_fixture.hpp"
#include "support/test_framework.hpp"

#include "apf/codec.hpp"
#include "apf/persistence.hpp"
#include "apf/process.hpp"
#include "apf/protocol.hpp"

#include <algorithm>

using namespace apf;

// ---------------------------------------------------------------------------
// Strong identities and generations
// ---------------------------------------------------------------------------

APF_TEST(ids_are_strongly_typed_and_validated) {
  const AcceleratorId id = AcceleratorId::from_value(42);
  CHECK(id.valid());
  CHECK_EQ(id.value(), 42u);
  CHECK_EQ(id.str(), std::string("42"));
  CHECK(!AcceleratorId{}.valid());
  const Result<AcceleratorId> parsed = AcceleratorId::parse("42");
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value(), id);
  CHECK_ERR(AcceleratorId::parse(""), ErrorCode::InvalidArgument);
  CHECK_ERR(AcceleratorId::parse("42x"), ErrorCode::InvalidArgument);
  CHECK_ERR(AcceleratorId::parse("-1"), ErrorCode::InvalidArgument);
  CHECK_ERR(AcceleratorId::parse("99999999999999999999999999"), ErrorCode::InvalidArgument);
  const WorkerBootId boot = WorkerBootId::derive(7);
  CHECK(boot.valid());
  const Result<WorkerBootId> reparsed = WorkerBootId::parse_hex(boot.hex());
  CHECK(reparsed.ok());
  CHECK_EQ(reparsed.value(), boot);
  CHECK_ERR(WorkerBootId::parse_hex("zzzz"), ErrorCode::InvalidArgument);
  CHECK(WorkerBootId::derive(1) != WorkerBootId::derive(2));
}

APF_TEST(generations_are_monotonic_and_checked) {
  const PartitionGeneration first = PartitionGeneration::first();
  CHECK(first.known());
  CHECK(!PartitionGeneration::unknown().known());
  const Result<PartitionGeneration> second = first.next();
  CHECK(second.ok());
  CHECK_EQ(second.value().value(), 2u);
  const PartitionGeneration exhausted = PartitionGeneration::from_value(UINT64_MAX);
  CHECK_ERR(exhausted.next(), ErrorCode::Overflow);
  IdAllocator allocator;
  const Result<PartitionId> a = allocator.allocate<PartitionId>();
  const Result<PartitionId> b = allocator.allocate<PartitionId>();
  CHECK(a.ok());
  CHECK(b.ok());
  CHECK(a.value() != b.value());
  CHECK_OK(allocator.restore_floor(allocator.raw()));
  CHECK_ERR(allocator.restore_floor(1), ErrorCode::GenerationRegression);
}

// ---------------------------------------------------------------------------
// Partition lifecycle
// ---------------------------------------------------------------------------

APF_TEST(lifecycle_transitions_are_explicit) {
  CHECK_OK(validate_transition(PartitionState::Reserved, PartitionState::Creating));
  CHECK_OK(validate_transition(PartitionState::Creating, PartitionState::Active));
  CHECK_OK(validate_transition(PartitionState::Active, PartitionState::Draining));
  CHECK_OK(validate_transition(PartitionState::Draining, PartitionState::ReconfigurationRequired));
  CHECK_OK(validate_transition(PartitionState::ReconfigurationRequired, PartitionState::Active));
  CHECK_OK(validate_transition(PartitionState::Active, PartitionState::Destroying));
  CHECK_OK(validate_transition(PartitionState::Destroying, PartitionState::Retired));
  CHECK_ERR(validate_transition(PartitionState::Retired, PartitionState::Active),
            ErrorCode::InvalidTransition);
  CHECK_ERR(validate_transition(PartitionState::Retired, PartitionState::Creating),
            ErrorCode::InvalidTransition);
  CHECK_ERR(validate_transition(PartitionState::Failed, PartitionState::Active),
            ErrorCode::InvalidTransition);
  CHECK_ERR(validate_transition(PartitionState::Unpartitioned, PartitionState::Active),
            ErrorCode::InvalidTransition);
  CHECK_ERR(validate_transition(PartitionState::Active, PartitionState::Reserved),
            ErrorCode::InvalidTransition);
  CHECK_OK(validate_transition(PartitionState::Active, PartitionState::Active));
  CHECK_ERR(validate_transition(PartitionState::Creating, PartitionState::Creating),
            ErrorCode::InvalidTransition);
  CHECK(is_terminal_state(PartitionState::Retired));
  CHECK(is_terminal_state(PartitionState::Failed));
  CHECK(!is_terminal_state(PartitionState::Active));
  CHECK(allows_new_assignment(PartitionState::Active));
  CHECK(!allows_new_assignment(PartitionState::Draining));
  const Result<PartitionState> parsed = partition_state_from_string("REVALIDATION_REQUIRED");
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value(), PartitionState::RevalidationRequired);
  CHECK_ERR(partition_state_from_string("NOPE"), ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Exact accounting
// ---------------------------------------------------------------------------

APF_TEST(accounting_closes_and_rejects_impossible_moves) {
  CapacityLedger ledger;
  const Result<ResourceVector> totals = make_resource_vector(
      {{ResourceDimension::ComputeShare, kShareScale},
       {ResourceDimension::MemoryBytes, gib(32)}});
  REQUIRE(totals.ok());
  CHECK_OK(ledger.initialize_totals(totals.value()));
  CHECK_OK(ledger.validate());
  const ResourceVector quarter = *make_resource_vector({{ResourceDimension::ComputeShare, 250'000},
                                                        {ResourceDimension::MemoryBytes, gib(8)}});
  CHECK_OK(ledger.reserve(quarter));
  CHECK_OK(ledger.validate());
  CHECK_EQ(ledger.bucket(CapacityBucket::Free).get(ResourceDimension::MemoryBytes), gib(24));
  CHECK_OK(ledger.commit_reserved(quarter, false));
  CHECK_OK(ledger.validate());
  CHECK_ERR(ledger.release_reserved(quarter), ErrorCode::InsufficientCapacity);
  CHECK_OK(ledger.release_committed(quarter, false));
  CHECK_OK(ledger.validate());
  CHECK_EQ(ledger.bucket(CapacityBucket::Free).get(ResourceDimension::MemoryBytes), gib(32));
  // Over-reservation is rejected before any mutation happens.
  const ResourceVector too_much = *make_resource_vector(
      {{ResourceDimension::ComputeShare, kShareScale}, {ResourceDimension::MemoryBytes, gib(64)}});
  CHECK_ERR(ledger.reserve(too_much), ErrorCode::InsufficientCapacity);
  CHECK_OK(ledger.validate());
  // A dimension the device does not publish is rejected, not silently ignored.
  const ResourceVector foreign = *make_resource_vector({{ResourceDimension::MemoryBytes, 1},
                                                        {ResourceDimension::MediaEngineShare, 1}});
  CHECK_ERR(ledger.reserve(foreign), ErrorCode::InsufficientCapacity);
  CHECK_OK(ledger.validate());
  // Shares cannot exceed the whole device.
  ResourceVector invalid;
  CHECK_ERR(invalid.set(ResourceDimension::ComputeShare, kShareScale + 1),
            ErrorCode::InvalidArgument);
}

APF_TEST(accounting_bucket_moves_preserve_the_invariant) {
  CapacityLedger ledger;
  const ResourceVector totals =
      *make_resource_vector({{ResourceDimension::ComputeShare, kShareScale},
                             {ResourceDimension::MemoryBytes, gib(8)}});
  CHECK_OK(ledger.initialize_totals(totals));
  const ResourceVector unit = *make_resource_vector({{ResourceDimension::ComputeShare, 100'000},
                                                      {ResourceDimension::MemoryBytes, gib(1)}});
  CHECK_OK(ledger.reserve(unit));
  CHECK_OK(ledger.commit_reserved(unit, false));
  CHECK_OK(ledger.begin_drain(unit));
  CHECK_OK(ledger.validate());
  CHECK_OK(ledger.move_between(CapacityBucket::Draining, CapacityBucket::ReconfigurationHeld, unit));
  CHECK_OK(ledger.validate());
  CHECK_OK(ledger.release_reconfiguration_hold(unit));
  CHECK_OK(ledger.validate());
  CHECK_EQ(ledger.bucket(CapacityBucket::Free).get(ResourceDimension::MemoryBytes), gib(8));
  CHECK_ERR(ledger.move_between(CapacityBucket::Draining, CapacityBucket::Free, unit),
            ErrorCode::InsufficientCapacity);
  CHECK_OK(ledger.validate());
  CHECK_OK(ledger.mark_unavailable(unit));
  CHECK_OK(ledger.validate());
  CHECK_EQ(ledger.bucket(CapacityBucket::Unavailable).get(ResourceDimension::MemoryBytes), gib(1));
}

// ---------------------------------------------------------------------------
// Profiles and capability
// ---------------------------------------------------------------------------

APF_TEST(profile_validation_rejects_impossible_shapes) {
  Limits limits;
  PartitionProfile profile;
  CHECK_ERR(profile.validate(limits), ErrorCode::InvalidArgument);
  profile.id = PartitionProfileId::from_value(1);
  profile.generation = PartitionProfileGeneration::first();
  profile.name = "test-1g";
  profile.backend = "synthetic";
  profile.mechanism = PartitionMechanism::Synthetic;
  profile.resources = *make_resource_vector({{ResourceDimension::ComputeShare, 100'000},
                                              {ResourceDimension::MemoryBytes, gib(1)}});
  profile.compute_slice_count = 1;
  profile.memory_slice_count = 1;
  CHECK_OK(profile.validate(limits));
  profile.mutually_exclusive_with = {profile.id};
  CHECK_ERR(profile.validate(limits), ErrorCode::InvalidArgument);
  profile.mutually_exclusive_with.clear();
  profile.alignment.compute_slice_multiple = 0;
  CHECK_ERR(profile.validate(limits), ErrorCode::InvalidArgument);
}

APF_TEST(capability_never_upgrades_absence_of_evidence) {
  Limits limits;
  PartitionCapability capability;
  capability.accelerator = AcceleratorId::from_value(1);
  capability.device_generation = AcceleratorGeneration::first();
  capability.generation = CapabilityGeneration::first();
  capability.support = PartitionSupportState::Supported;
  capability.mechanism = PartitionMechanism::Synthetic;
  capability.evidence.provenance = EvidenceProvenance::Synthetic;
  capability.evidence.observed = true;
  capability.evidence.generation = EvidenceGeneration::first();
  capability.evidence.ttl_ms = 1000;
  capability.max_partition_count = 2;
  capability.exposed_dimensions = {ResourceDimension::MemoryBytes};
  // Supported without any profile is not a usable capability.
  CHECK_ERR(capability.validate(limits), ErrorCode::InvalidArgument);
  capability.supported_profiles = {PartitionProfileId::from_value(9)};
  CHECK_OK(capability.validate(limits));
  // An unsupported determination must say why.
  capability.support = PartitionSupportState::Unsupported;
  capability.unsupported_reason.clear();
  CHECK_ERR(capability.validate(limits), ErrorCode::InvalidArgument);
  capability.unsupported_reason = "no partition mechanism";
  CHECK_OK(capability.validate(limits));
  // A physical mechanism may never be claimed from synthetic evidence.
  capability.support = PartitionSupportState::Supported;
  capability.mechanism = PartitionMechanism::Mig;
  capability.unsupported_reason.clear();
  CHECK_ERR(capability.validate(limits), ErrorCode::InvalidArgument);
}

APF_TEST(capability_digest_is_stable_and_sensitive) {
  PartitionCapability capability;
  capability.accelerator = AcceleratorId::from_value(1);
  capability.device_generation = AcceleratorGeneration::first();
  capability.generation = CapabilityGeneration::first();
  capability.support = PartitionSupportState::Unsupported;
  capability.unsupported_reason = "no mechanism";
  const std::uint64_t first = capability_digest(capability);
  CHECK_EQ(capability_digest(capability), first);
  capability.unsupported_reason = "another reason";
  CHECK(capability_digest(capability) != first);
}

// ---------------------------------------------------------------------------
// Explanations and snapshots
// ---------------------------------------------------------------------------

APF_TEST(explanations_are_canonical_and_deterministic) {
  Explanation left;
  left.add(ExplanationCode::RejectedIsolation, "b", "second");
  left.add(ExplanationCode::CandidateChosen, "a", "first");
  left.canonicalize(64);
  Explanation right;
  right.add(ExplanationCode::CandidateChosen, "a", "first");
  right.add(ExplanationCode::RejectedIsolation, "b", "second");
  right.canonicalize(64);
  CHECK_EQ(left.digest(), right.digest());
  CHECK_EQ(left.format(), right.format());
  CHECK(left.contains(ExplanationCode::CandidateChosen));
  CHECK_EQ(left.count(ExplanationCode::RejectedIsolation), static_cast<std::size_t>(1));
  Explanation truncated;
  for (int index = 0; index < 10; ++index) {
    truncated.add(ExplanationCode::RankingFactor, "subject" + std::to_string(index), "detail");
  }
  truncated.canonicalize(4);
  CHECK_EQ(truncated.size(), static_cast<std::size_t>(4));
}

APF_TEST(snapshot_rendering_is_deterministic) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const std::shared_ptr<const FabricSnapshot> first = fixture->fabric->snapshot();
  const std::shared_ptr<const FabricSnapshot> second = fixture->fabric->snapshot();
  CHECK_EQ(snapshot_digest(*first), snapshot_digest(*second));
  CHECK_EQ(first->render_logical(), second->render_logical());
  CHECK(first->find_accelerator(fixture->accelerator) != nullptr);
}

// ---------------------------------------------------------------------------
// Codecs
// ---------------------------------------------------------------------------

APF_TEST(codec_round_trips_and_rejects_malformed_input) {
  Limits limits;
  PartitionProfile profile;
  profile.id = PartitionProfileId::from_value(3);
  profile.generation = PartitionProfileGeneration::first();
  profile.name = "codec-profile";
  profile.vendor_native = "1g.4gb";
  profile.backend = "synthetic";
  profile.mechanism = PartitionMechanism::Synthetic;
  profile.resources = *make_resource_vector({{ResourceDimension::ComputeShare, 100'000},
                                              {ResourceDimension::MemoryBytes, gib(4)}});
  profile.compute_slice_count = 1;
  profile.memory_slice_count = 1;
  profile.description = "round trip";
  ByteWriter writer(&limits);
  codec::encode(writer, profile);
  REQUIRE(writer.ok());
  ByteReader reader(writer.data().data(), writer.data().size(), limits);
  const PartitionProfile decoded = codec::decode_profile(reader);
  CHECK_OK(reader.finish());
  CHECK_EQ(decoded.id, profile.id);
  CHECK_EQ(decoded.name, profile.name);
  CHECK_EQ(decoded.vendor_native, profile.vendor_native);
  CHECK_EQ(decoded.compute_slice_count, profile.compute_slice_count);
  CHECK(decoded.resources == profile.resources);

  // Truncation at every boundary must be rejected, never partially accepted.
  for (std::size_t cut = 0; cut < writer.data().size(); cut += 3) {
    ByteReader partial(writer.data().data(), cut, limits);
    (void)codec::decode_profile(partial);
    CHECK(!partial.finish().ok());
  }
  // Trailing bytes are a protocol violation.
  std::vector<std::uint8_t> extended = writer.data();
  extended.push_back(0);
  ByteReader trailing(extended.data(), extended.size(), limits);
  (void)codec::decode_profile(trailing);
  CHECK_ERR(trailing.finish(), ErrorCode::ProtocolViolation);
  // Invalid UTF-8 in a text field is rejected.
  ByteWriter bad(&limits);
  bad.u32(2);
  bad.u8(0xFF);
  bad.u8(0xFF);
  ByteReader bad_reader(bad.data().data(), bad.data().size(), limits);
  (void)bad_reader.text(64);
  CHECK(!bad_reader.ok());
  CHECK_EQ(bad_reader.error().code, ErrorCode::InvalidArgument);
  // A declared text length beyond the bound is rejected before allocation.
  ByteWriter oversized(&limits);
  oversized.u32(1u << 20);
  ByteReader oversized_reader(oversized.data().data(), oversized.data().size(), limits);
  (void)oversized_reader.text(64);
  CHECK_EQ(oversized_reader.error().code, ErrorCode::LimitExceeded);
}

APF_TEST(frames_are_integrity_checked) {
  Limits limits;
  Frame frame;
  frame.type = MessageType::Heartbeat;
  frame.flags = to_flags(FrameFlag::Request);
  frame.sequence = 7;
  frame.coordinator_epoch = CoordinatorEpoch::from_value(5);
  frame.worker = WorkerId::from_value(9);
  frame.worker_boot = WorkerBootId::derive(11);
  HeartbeatMessage heartbeat;
  heartbeat.sent_at_ms = 1234;
  heartbeat.applied_attempts = 3;
  frame.payload = pack_payload(heartbeat, limits);
  const Result<std::vector<std::uint8_t>> encoded = encode_frame(frame, limits);
  REQUIRE(encoded.ok());
  const Result<Frame> decoded = decode_frame(encoded.value().data(), encoded.value().size(), limits);
  REQUIRE(decoded.ok());
  CHECK_EQ(decoded.value().sequence, frame.sequence);
  CHECK_EQ(decoded.value().coordinator_epoch.value(), 5u);
  const Result<HeartbeatMessage> payload = unpack_payload<HeartbeatMessage>(
      decoded.value().payload.data(), decoded.value().payload.size(), limits);
  REQUIRE(payload.ok());
  CHECK_EQ(payload.value().applied_attempts, 3u);

  // A single flipped bit anywhere in the header or payload must be rejected.
  for (std::size_t index = 0; index < encoded.value().size(); index += 7) {
    std::vector<std::uint8_t> corrupted = encoded.value();
    corrupted[index] = static_cast<std::uint8_t>(corrupted[index] ^ 0x40u);
    const Result<Frame> rejected = decode_frame(corrupted.data(), corrupted.size(), limits);
    CHECK(!rejected.ok());
  }
  const Result<Frame> truncated =
      decode_frame(encoded.value().data(), encoded.value().size() - 1, limits);
  CHECK(!truncated.ok());
  std::vector<std::uint8_t> bad_magic = encoded.value();
  bad_magic[0] = 0;
  CHECK(!decode_frame(bad_magic.data(), bad_magic.size(), limits).ok());
  std::vector<std::uint8_t> bad_version = encoded.value();
  bad_version[4] = 99;
  CHECK(!decode_frame(bad_version.data(), bad_version.size(), limits).ok());
  std::vector<std::uint8_t> bad_type = encoded.value();
  bad_type[6] = 0xEE;
  bad_type[7] = 0xEE;
  CHECK(!decode_frame(bad_type.data(), bad_type.size(), limits).ok());
}

APF_TEST(persistence_container_round_trips) {
  const std::unique_ptr<apftest::Fixture> fixture = apftest::make_fixture();
  REQUIRE(fixture != nullptr);
  const Result<MutationOutcome> created = fixture->create("synthetic-2g", 1);
  REQUIRE(created.ok());
  const Result<DurableState> durable = fixture->fabric->export_durable_state();
  REQUIRE(durable.ok());
  Limits limits;
  const Result<std::vector<std::uint8_t>> encoded = PersistenceStore::encode(durable.value(), limits);
  REQUIRE(encoded.ok());
  const Result<DurableState> decoded =
      PersistenceStore::decode(encoded.value().data(), encoded.value().size(), limits);
  REQUIRE(decoded.ok());
  CHECK_EQ(decoded.value().partitions.size(), durable.value().partitions.size());
  CHECK_EQ(decoded.value().accelerators.size(), durable.value().accelerators.size());
  CHECK_EQ(decoded.value().next_identity, durable.value().next_identity);
}

APF_TEST(atomic_file_replacement_works_and_rejects_unsafe_paths) {
  const Result<std::string> directory = make_temp_directory("apf-test");
  REQUIRE(directory.ok());
  const std::string path = join_path(directory.value(), "state.bin");
  CHECK_OK(write_file_atomic(path, "payload-one"));
  const Result<std::string> first = read_file(path);
  REQUIRE(first.ok());
  CHECK_EQ(first.value(), std::string("payload-one"));
  CHECK_OK(write_file_atomic(path, "payload-two"));
  const Result<std::string> second = read_file(path);
  REQUIRE(second.ok());
  CHECK_EQ(second.value(), std::string("payload-two"));
  CHECK(!file_exists(path + ".tmp"));
  CHECK_ERR(write_file_atomic("../escape.bin", "x"), ErrorCode::InvalidArgument);
  const std::string traversal = std::string("..") + static_cast<char>(92) + "escape.bin";
  CHECK_ERR(remove_file(traversal), ErrorCode::InvalidArgument);
  CHECK_OK(remove_directory_recursive(directory.value()));
  CHECK(!file_exists(directory.value()));
}

APF_TEST(limits_validation_rejects_unusable_configurations) {
  Limits limits;
  CHECK_OK(limits.validate());
  Limits broken = limits;
  broken.max_frame_bytes = 8;
  CHECK_ERR(broken.validate(), ErrorCode::InvalidArgument);
  broken = limits;
  broken.max_native_id_bytes = limits.max_metadata_bytes + 1;
  CHECK_ERR(broken.validate(), ErrorCode::InvalidArgument);
  broken = limits;
  broken.max_pending_attempts = limits.max_attempt_history + 1;
  CHECK_ERR(broken.validate(), ErrorCode::InvalidArgument);
}
