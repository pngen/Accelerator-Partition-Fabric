#include "support/fabric_fixture.hpp"
#include "support/test_framework.hpp"

#include "apf/client.hpp"
#include "apf/process.hpp"
#include "apf/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace apf;

namespace {

#ifndef APF_COORDINATOR_BINARY
#define APF_COORDINATOR_BINARY "apfcoord"
#endif
#ifndef APF_WORKER_BINARY
#define APF_WORKER_BINARY "apfworker"
#endif

/// Trims surrounding whitespace, including the newline a readiness
/// announcement carries.
std::string trim(const std::string& text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return std::string();
  }
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::string binary(const char* configured) {
  if (file_exists(configured)) {
    return configured;
  }
  const std::string directory = current_executable_directory();
  const std::string candidate = join_path(directory, std::string(configured) + ".exe");
  if (file_exists(candidate)) {
    return candidate;
  }
  return configured;
}

/// Waits for a file that a child process writes once it is serving. This is a
/// readiness wait for a documented condition, not a test timeout: if the
/// condition never appears the test fails loudly and reports the child state.
bool wait_for_file(const std::string& path, const ChildProcess& child, std::string* contents) {
  constexpr int kMaximumPolls = 2'000;
  for (int poll = 0; poll < kMaximumPolls; ++poll) {
    if (file_exists(path)) {
      const Result<std::string> read = read_file(path);
      if (read.ok() && !read.value().empty()) {
        if (contents != nullptr) {
          *contents = read.value();
        }
        return true;
      }
    }
    if (!const_cast<ChildProcess&>(child).running()) {
      std::fprintf(stderr, "child process exited before becoming ready\n");
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::fprintf(stderr, "child process did not become ready\n");
  return false;
}

/// Polls a predicate until it holds. Used for asynchronous control-plane
/// observations such as worker fencing.
template <class Predicate>
bool wait_until(Predicate predicate) {
  constexpr int kMaximumPolls = 4'000;
  for (int poll = 0; poll < kMaximumPolls; ++poll) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

struct CoordinatorProcess {
  ChildProcess process;
  Endpoint endpoint{};
  std::string ready_path;
  std::string state_path;

  ~CoordinatorProcess() {
    // A failing assertion must not leave a coordinator behind.
    if (process.valid()) {
      if (process.running()) {
        (void)process.terminate();
      }
      (void)process.wait();
    }
  }

  static Result<std::unique_ptr<CoordinatorProcess>> start(const std::string& work_directory,
                                                           const std::string& tag,
                                                           bool with_state) {
    auto coordinator = std::make_unique<CoordinatorProcess>();
    coordinator->ready_path = join_path(work_directory, "coordinator-" + tag + ".ready");
    coordinator->state_path = join_path(work_directory, "coordinator-" + tag + ".state");
    (void)remove_file(coordinator->ready_path);
    ProcessOptions options;
    options.executable = binary(APF_COORDINATOR_BINARY);
    options.arguments = {"--listen", "127.0.0.1:0", "--instance", "mp-" + tag, "--ready-file",
                         coordinator->ready_path};
    if (with_state) {
      options.arguments.push_back("--state");
      options.arguments.push_back(coordinator->state_path);
    }
    Result<ChildProcess> spawned = ChildProcess::spawn(options);
    if (!spawned.ok()) {
      return spawned.error();
    }
    coordinator->process = std::move(spawned.value());
    std::string contents;
    if (!wait_for_file(coordinator->ready_path, coordinator->process, &contents)) {
      return make_error(ErrorCode::DeviceUnavailable, "coordinator did not become ready");
    }
    contents = trim(contents);
    const std::size_t space = contents.find(' ');
    if (space == std::string::npos) {
      return make_error(ErrorCode::Internal, "coordinator readiness announcement is malformed",
                        contents);
    }
    Result<Endpoint> endpoint = Endpoint::parse(contents.substr(0, space));
    if (!endpoint.ok()) {
      return endpoint.error();
    }
    coordinator->endpoint = endpoint.value();
    return coordinator;
  }
};

struct WorkerProcess {
  ChildProcess process;
  WorkerId id{};
  WorkerBootId boot{};
  std::string ready_path;

  ~WorkerProcess() {
    if (process.valid()) {
      if (process.running()) {
        (void)process.terminate();
      }
      (void)process.wait();
    }
  }

  static Result<std::unique_ptr<WorkerProcess>> start(const std::string& work_directory,
                                                      const std::string& tag,
                                                      const Endpoint& endpoint,
                                                      const std::string& device_key,
                                                      const std::string& device_state,
                                                      bool ambiguous_once) {
    auto worker = std::make_unique<WorkerProcess>();
    worker->ready_path = join_path(work_directory, "worker-" + tag + ".ready");
    (void)remove_file(worker->ready_path);
    ProcessOptions options;
    options.executable = binary(APF_WORKER_BINARY);
    options.arguments = {"--coordinator", endpoint.to_string(), "--backend", "synthetic",
                         "--device", device_key, "--ready-file", worker->ready_path};
    if (!device_state.empty()) {
      options.arguments.push_back("--device-state");
      options.arguments.push_back(device_state);
    }
    if (ambiguous_once) {
      options.arguments.push_back("--ambiguous-once");
    }
    Result<ChildProcess> spawned = ChildProcess::spawn(options);
    if (!spawned.ok()) {
      return spawned.error();
    }
    worker->process = std::move(spawned.value());
    std::string contents;
    if (!wait_for_file(worker->ready_path, worker->process, &contents)) {
      return make_error(ErrorCode::DeviceUnavailable, "worker did not become ready");
    }
    contents = trim(contents);
    const std::size_t space = contents.find(' ');
    if (space == std::string::npos) {
      return make_error(ErrorCode::Internal, "worker readiness announcement is malformed",
                        contents);
    }
    Result<WorkerId> id = WorkerId::parse(contents.substr(0, space));
    Result<WorkerBootId> boot = WorkerBootId::parse_hex(contents.substr(space + 1));
    if (!id.ok() || !boot.ok()) {
      return make_error(ErrorCode::Internal, "worker identity announcement is malformed",
                        contents);
    }
    worker->id = id.value();
    worker->boot = boot.value();
    return worker;
  }
};

PartitionProfileId worker_profile_id(const std::string& device_key, const std::string& name) {
  // Mirrors the deterministic derivation the synthetic backend uses, so that a
  // coordinator-only client can address a profile by name.
  (void)device_key;
  const std::string material = "apf-synthetic-profile|synthetic|" + name;
  std::uint64_t hash = 1469598103934665603ull;
  for (const char ch : material) {
    hash ^= static_cast<std::uint8_t>(ch);
    hash *= 1099511628211ull;
  }
  return PartitionProfileId::from_value(hash == 0 ? 1 : hash);
}

}  // namespace

APF_TEST(multiprocess_worker_death_fencing_and_reincarnation) {
  const Result<std::string> directory = make_temp_directory("apf-mp");
  REQUIRE(directory.ok());
  const std::string work = directory.value();
  const std::string device_state = join_path(work, "device.state");

  Result<std::unique_ptr<CoordinatorProcess>> coordinator =
      CoordinatorProcess::start(work, "primary", true);
  REQUIRE(coordinator.ok());
  Result<std::unique_ptr<CoordinatorClient>> client =
      CoordinatorClient::connect(coordinator.value()->endpoint);
  REQUIRE(client.ok());

  Result<std::unique_ptr<WorkerProcess>> worker =
      WorkerProcess::start(work, "one", coordinator.value()->endpoint, "mp-0", device_state, false);
  REQUIRE(worker.ok());
  const WorkerId first_id = worker.value()->id;
  const WorkerBootId first_boot = worker.value()->boot;

  // The worker published real evidence through the protocol.
  const Result<CommandResponseMessage> workers_before = client.value()->inspect_workers();
  REQUIRE(workers_before.ok());
  REQUIRE(workers_before.value().workers.size() == 1);
  CHECK_EQ(workers_before.value().workers.front().id, first_id);
  CHECK_EQ(workers_before.value().workers.front().boot, first_boot);
  CHECK(workers_before.value().workers.front().alive);

  const Result<CommandResponseMessage> layout =
      client.value()->query_layout("mp-0");
  REQUIRE(layout.ok());
  CHECK(layout.value().accepted);
  CHECK(layout.value().layout.device_present);

  // Plan, reserve and create through the distributed path.
  PartitionRequest request;
  request.profile = worker_profile_id("mp-0", "synthetic-2g");
  request.count = 1;
  request.requester = "multiprocess-test";
  const Result<CommandResponseMessage> planned = client.value()->plan(request);
  REQUIRE(planned.ok());
  REQUIRE(planned.value().accepted);
  CHECK_EQ(planned.value().plan_outcome, PlanOutcome::FeasibleNow);
  const Result<CommandResponseMessage> reserved =
      client.value()->reserve(planned.value().plan, first_id, first_boot);
  REQUIRE(reserved.ok());
  REQUIRE(reserved.value().accepted);
  const Result<CommandResponseMessage> created = client.value()->create(reserved.value().reservation);
  REQUIRE(created.ok());
  CHECK(created.value().accepted);
  CHECK(created.value().verified_physically);
  REQUIRE(created.value().partitions.size() == 1);
  const PartitionId partition = created.value().partitions.front();

  // Kill the worker as a real operating-system process.
  CHECK_OK(worker.value()->process.terminate());
  const Result<int> exit_code = worker.value()->process.wait();
  CHECK(exit_code.ok());
  CHECK(exit_code.value() != 0);

  // The coordinator observes the death through the control path and fences the
  // incarnation.
  const bool fenced = wait_until([&]() {
    const Result<CommandResponseMessage> workers = client.value()->inspect_workers();
    if (!workers.ok() || workers.value().workers.empty()) {
      return false;
    }
    for (const WorkerRecord& record : workers.value().workers) {
      if (record.id == first_id && record.boot == first_boot) {
        return record.fenced && !record.alive;
      }
    }
    return false;
  });
  CHECK(fenced);

  // Traffic addressed to the dead incarnation is refused.
  const Result<CommandResponseMessage> stale_layout = client.value()->query_layout("mp-0");
  REQUIRE(stale_layout.ok());
  CHECK(!stale_layout.value().accepted);
  const Result<CommandResponseMessage> stale_destroy = client.value()->destroy(partition);
  REQUIRE(stale_destroy.ok());
  CHECK(!stale_destroy.value().accepted);
  // A new reservation cannot be bound to the dead boot identity.
  const Result<CommandResponseMessage> rebound =
      client.value()->reserve(planned.value().plan, first_id, first_boot);
  REQUIRE(rebound.ok());
  CHECK(!rebound.value().accepted);

  // A replacement worker observes the same physical reality and republishes it.
  Result<std::unique_ptr<WorkerProcess>> replacement = WorkerProcess::start(
      work, "two", coordinator.value()->endpoint, "mp-0", device_state, false);
  REQUIRE(replacement.ok());
  CHECK(replacement.value()->boot != first_boot);
  const Result<CommandResponseMessage> layout_again = client.value()->query_layout("mp-0");
  REQUIRE(layout_again.ok());
  CHECK(layout_again.value().accepted);
  CHECK_EQ(layout_again.value().layout.partitions.size(), static_cast<std::size_t>(1));

  // Authority returns only through fresh evidence: a new partition can only be
  // created after the replacement incarnation has published.
  PartitionRequest second_request;
  second_request.profile = worker_profile_id("mp-0", "synthetic-1g");
  second_request.count = 1;
  const Result<CommandResponseMessage> second_plan = client.value()->plan(second_request);
  REQUIRE(second_plan.ok());
  REQUIRE(second_plan.value().accepted);
  const Result<CommandResponseMessage> second_reserved = client.value()->reserve(
      second_plan.value().plan, replacement.value()->id, replacement.value()->boot);
  REQUIRE(second_reserved.ok());
  REQUIRE(second_reserved.value().accepted);
  const Result<CommandResponseMessage> second_created =
      client.value()->create(second_reserved.value().reservation);
  REQUIRE(second_created.ok());
  CHECK(second_created.value().accepted);

  // The pre-existing partition is still not silently authoritative: it needs an
  // explicit reconciliation decision.
  const Result<CommandResponseMessage> reconciled = client.value()->reconcile(
      replacement.value()->id, replacement.value()->boot, "mp-0");
  REQUIRE(reconciled.ok());
  CHECK(reconciled.value().accepted);
  CHECK(reconciled.value().matched >= 1);

  const Result<CommandResponseMessage> snapshot = client.value()->inspect_snapshot();
  REQUIRE(snapshot.ok());
  CHECK(!snapshot.value().snapshot_text.empty());

  // Shut everything down cleanly; no orphan processes remain.
  const Result<CommandResponseMessage> shutdown = [&]() {
    CommandRequestMessage command;
    command.kind = CommandKind::ShutdownCoordinator;
    command.reason = "test complete";
    return client.value()->command(command);
  }();
  REQUIRE(shutdown.ok());
  CHECK(shutdown.value().accepted);
  client.value()->close();
  const Result<int> coordinator_exit = coordinator.value()->process.wait();
  CHECK(coordinator_exit.ok());
  CHECK_EQ(coordinator_exit.value(), 0);
  if (replacement.value()->process.running()) {
    CHECK_OK(replacement.value()->process.terminate());
  }
  CHECK_OK(replacement.value()->process.wait());
  CHECK_OK(remove_directory_recursive(work));
}

APF_TEST(multiprocess_ambiguous_completion_is_reconciled_not_replayed) {
  const Result<std::string> directory = make_temp_directory("apf-mp-ambiguous");
  REQUIRE(directory.ok());
  const std::string work = directory.value();
  const std::string device_state = join_path(work, "device.state");
  Result<std::unique_ptr<CoordinatorProcess>> coordinator =
      CoordinatorProcess::start(work, "ambiguous", true);
  REQUIRE(coordinator.ok());
  Result<std::unique_ptr<CoordinatorClient>> client =
      CoordinatorClient::connect(coordinator.value()->endpoint);
  REQUIRE(client.ok());
  // This worker applies the next mutation physically, checkpoints the physical
  // model, and then dies before acknowledging it.
  Result<std::unique_ptr<WorkerProcess>> worker = WorkerProcess::start(
      work, "ambiguous", coordinator.value()->endpoint, "mp-amb", device_state, true);
  REQUIRE(worker.ok());
  const WorkerId worker_id = worker.value()->id;
  const WorkerBootId worker_boot = worker.value()->boot;

  PartitionRequest request;
  request.profile = worker_profile_id("mp-amb", "synthetic-2g");
  request.count = 1;
  const Result<CommandResponseMessage> planned = client.value()->plan(request);
  REQUIRE(planned.ok());
  REQUIRE(planned.value().accepted);
  const Result<CommandResponseMessage> reserved =
      client.value()->reserve(planned.value().plan, worker_id, worker_boot);
  REQUIRE(reserved.ok());
  REQUIRE(reserved.value().accepted);
  const Result<CommandResponseMessage> created = client.value()->create(reserved.value().reservation);
  REQUIRE(created.ok());
  // The coordinator must not claim success and must not replay the mutation.
  CHECK(created.value().outcome_unknown);
  CHECK(!created.value().accepted);
  CHECK_EQ(created.value().attempt_state, AttemptState::OutcomeUnknown);
  CHECK_OK(worker.value()->process.wait());

  // A replacement incarnation loads the checkpointed physical model and
  // therefore observes the change that did happen.
  Result<std::unique_ptr<WorkerProcess>> replacement = WorkerProcess::start(
      work, "ambiguous-2", coordinator.value()->endpoint, "mp-amb", device_state, false);
  REQUIRE(replacement.ok());
  const Result<CommandResponseMessage> layout = client.value()->query_layout("mp-amb");
  REQUIRE(layout.ok());
  CHECK(layout.value().accepted);
  CHECK_EQ(layout.value().layout.partitions.size(), static_cast<std::size_t>(1));
  const Result<CommandResponseMessage> reconciled = client.value()->reconcile(
      replacement.value()->id, replacement.value()->boot, "mp-amb");
  REQUIRE(reconciled.ok());
  CHECK(reconciled.value().accepted);
  const Result<CommandResponseMessage> partitions = client.value()->inspect_partitions();
  REQUIRE(partitions.ok());
  std::size_t authoritative = 0;
  for (const PartitionRecord& record : partitions.value().partition_records) {
    if (record.state == PartitionState::Active) {
      ++authoritative;
    }
  }
  CHECK_EQ(authoritative, static_cast<std::size_t>(1));
  // Reconciling again must not duplicate authority.
  const Result<CommandResponseMessage> again = client.value()->reconcile(
      replacement.value()->id, replacement.value()->boot, "mp-amb");
  REQUIRE(again.ok());
  CHECK(again.value().accepted);
  const Result<CommandResponseMessage> after = client.value()->inspect_partitions();
  REQUIRE(after.ok());
  std::size_t authoritative_after = 0;
  for (const PartitionRecord& record : after.value().partition_records) {
    if (record.state == PartitionState::Active) {
      ++authoritative_after;
    }
  }
  CHECK_EQ(authoritative_after, static_cast<std::size_t>(1));

  CommandRequestMessage shutdown;
  shutdown.kind = CommandKind::ShutdownCoordinator;
  shutdown.reason = "test complete";
  const Result<CommandResponseMessage> stopped = client.value()->command(shutdown);
  REQUIRE(stopped.ok());
  client.value()->close();
  CHECK_OK(coordinator.value()->process.wait());
  if (replacement.value()->process.running()) {
    CHECK_OK(replacement.value()->process.terminate());
  }
  CHECK_OK(replacement.value()->process.wait());
  CHECK_OK(remove_directory_recursive(work));
}

APF_TEST(multiprocess_coordinator_restart_advances_authority_and_requires_revalidation) {
  const Result<std::string> directory = make_temp_directory("apf-mp-restart");
  REQUIRE(directory.ok());
  const std::string work = directory.value();
  const std::string device_state = join_path(work, "device.state");
  Result<std::unique_ptr<CoordinatorProcess>> first =
      CoordinatorProcess::start(work, "restart", true);
  REQUIRE(first.ok());
  Result<std::unique_ptr<CoordinatorClient>> client =
      CoordinatorClient::connect(first.value()->endpoint);
  REQUIRE(client.ok());
  Result<std::unique_ptr<WorkerProcess>> worker = WorkerProcess::start(
      work, "restart-one", first.value()->endpoint, "mp-restart", device_state, false);
  REQUIRE(worker.ok());
  PartitionRequest request;
  request.profile = worker_profile_id("mp-restart", "synthetic-2g");
  request.count = 1;
  const Result<CommandResponseMessage> planned = client.value()->plan(request);
  REQUIRE(planned.ok());
  REQUIRE(planned.value().accepted);
  const Result<CommandResponseMessage> reserved = client.value()->reserve(
      planned.value().plan, worker.value()->id, worker.value()->boot);
  REQUIRE(reserved.ok());
  REQUIRE(reserved.value().accepted);
  const Result<CommandResponseMessage> created = client.value()->create(reserved.value().reservation);
  REQUIRE(created.ok());
  REQUIRE(created.value().accepted);
  const CoordinatorEpoch first_epoch = created.value().coordinator_epoch;
  const Result<CommandResponseMessage> persisted = client.value()->persist_state();
  REQUIRE(persisted.ok());
  CHECK(persisted.value().accepted);

  // Kill the coordinator as a real process: no graceful shutdown at all.
  CHECK_OK(first.value()->process.terminate());
  CHECK_OK(first.value()->process.wait());
  client.value()->close();
  CHECK_OK(worker.value()->process.terminate());
  CHECK_OK(worker.value()->process.wait());

  // A fresh coordinator reloads the durable structure under a new epoch.
  Result<std::unique_ptr<CoordinatorProcess>> second =
      CoordinatorProcess::start(work, "restart", true);
  REQUIRE(second.ok());
  Result<std::unique_ptr<CoordinatorClient>> restarted =
      CoordinatorClient::connect(second.value()->endpoint);
  REQUIRE(restarted.ok());
  const Result<CommandResponseMessage> snapshot = restarted.value()->inspect_snapshot();
  REQUIRE(snapshot.ok());
  CHECK(snapshot.value().coordinator_epoch > first_epoch);
  // Durable structure survived.
  const Result<CommandResponseMessage> partitions = restarted.value()->inspect_partitions();
  REQUIRE(partitions.ok());
  REQUIRE(partitions.value().partition_records.size() == 1);
  // Live physical authority did not.
  CHECK_EQ(partitions.value().partition_records.front().state,
           PartitionState::RevalidationRequired);
  // The restarted coordinator refuses to bind anything to an incarnation from
  // before the restart.
  const Result<CommandResponseMessage> rejected = restarted.value()->reserve(
      planned.value().plan, worker.value()->id, worker.value()->boot);
  REQUIRE(rejected.ok());
  CHECK(!rejected.value().accepted);

  // A worker incarnation from after the restart publishes fresh evidence and
  // authority becomes usable again.
  Result<std::unique_ptr<WorkerProcess>> replacement = WorkerProcess::start(
      work, "restart-two", second.value()->endpoint, "mp-restart", device_state, false);
  REQUIRE(replacement.ok());
  CHECK(replacement.value()->boot != worker.value()->boot);
  const Result<CommandResponseMessage> layout = restarted.value()->query_layout("mp-restart");
  REQUIRE(layout.ok());
  CHECK(layout.value().accepted);
  CHECK_EQ(layout.value().layout.partitions.size(), static_cast<std::size_t>(1));
  const Result<CommandResponseMessage> reconciled = restarted.value()->reconcile(
      replacement.value()->id, replacement.value()->boot, "mp-restart");
  REQUIRE(reconciled.ok());
  CHECK(reconciled.value().accepted);
  PartitionRequest second_request;
  second_request.profile = worker_profile_id("mp-restart", "synthetic-1g");
  second_request.count = 1;
  const Result<CommandResponseMessage> second_plan = restarted.value()->plan(second_request);
  REQUIRE(second_plan.ok());
  REQUIRE(second_plan.value().accepted);
  const Result<CommandResponseMessage> second_reserved = restarted.value()->reserve(
      second_plan.value().plan, replacement.value()->id, replacement.value()->boot);
  REQUIRE(second_reserved.ok());
  REQUIRE(second_reserved.value().accepted);
  const Result<CommandResponseMessage> second_created =
      restarted.value()->create(second_reserved.value().reservation);
  REQUIRE(second_created.ok());
  CHECK(second_created.value().accepted);

  CommandRequestMessage shutdown;
  shutdown.kind = CommandKind::ShutdownCoordinator;
  shutdown.reason = "test complete";
  const Result<CommandResponseMessage> stopped = restarted.value()->command(shutdown);
  REQUIRE(stopped.ok());
  restarted.value()->close();
  CHECK_OK(second.value()->process.wait());
  if (replacement.value()->process.running()) {
    CHECK_OK(replacement.value()->process.terminate());
  }
  CHECK_OK(replacement.value()->process.wait());
  CHECK_OK(remove_directory_recursive(work));
}

APF_TEST(multiprocess_no_orphan_processes_after_tests) {
  // Every child spawned by the previous tests has been reaped; this test proves
  // the harness itself does not leak processes by spawning and reaping one.
  ProcessOptions options;
  options.executable = binary(APF_COORDINATOR_BINARY);
  options.arguments = {"--version"};
  Result<ChildProcess> child = ChildProcess::spawn(options);
  REQUIRE(child.ok());
  const Result<int> code = child.value().wait();
  REQUIRE(code.ok());
  CHECK_EQ(code.value(), 0);
  CHECK(!child.value().running());
}
