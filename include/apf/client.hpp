#pragma once

#include "apf/limits.hpp"
#include "apf/protocol.hpp"
#include "apf/result.hpp"
#include "apf/transport.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace apf {

/// Programmatic control and observation client for a running coordinator.
///
/// One connection carries one request/response exchange at a time; the client
/// serialises its own commands. Read-only commands are separated from mutating
/// commands so that an inspection tool cannot change state by accident, and a
/// mutating command must be explicitly confirmed.
class CoordinatorClient {
 public:
  static Result<std::unique_ptr<CoordinatorClient>> connect(const Endpoint& endpoint,
                                                            Limits limits = {});

  CoordinatorClient(const CoordinatorClient&) = delete;
  CoordinatorClient& operator=(const CoordinatorClient&) = delete;
  ~CoordinatorClient();

  bool connected() const;
  void close();
  const Endpoint& endpoint() const noexcept { return endpoint_; }

  /// Sends one command and waits for its response.
  Result<CommandResponseMessage> command(const CommandRequestMessage& request);

  // Read-only helpers: these never mutate runtime state.
  Result<CommandResponseMessage> inspect_snapshot();
  Result<CommandResponseMessage> inspect_accelerators();
  Result<CommandResponseMessage> inspect_workers();
  Result<CommandResponseMessage> inspect_partitions();
  Result<CommandResponseMessage> query_layout(std::string_view stable_key);

  // Administrative helpers: each one is explicitly confirmed.
  Result<CommandResponseMessage> plan(const PartitionRequest& request);
  Result<CommandResponseMessage> reserve(const PartitionPlanId& plan, const WorkerId& worker,
                                         const WorkerBootId& worker_boot);
  Result<CommandResponseMessage> create(const PartitionReservationId& reservation);
  Result<CommandResponseMessage> destroy(const PartitionId& partition);
  Result<CommandResponseMessage> drain(const PartitionId& partition, std::string reason);
  Result<CommandResponseMessage> complete_drain(const PartitionId& partition);
  Result<CommandResponseMessage> cancel_drain(const PartitionId& partition);
  Result<CommandResponseMessage> release_reservation(const PartitionReservationId& reservation,
                                                     std::string reason);
  Result<CommandResponseMessage> reconcile(const WorkerId& worker, const WorkerBootId& worker_boot,
                                           std::string_view stable_key);
  Result<CommandResponseMessage> fence_worker(const WorkerId& worker,
                                              const WorkerBootId& worker_boot,
                                              std::string reason);
  Result<CommandResponseMessage> advance_epoch();
  Result<CommandResponseMessage> persist_state();

 private:
  CoordinatorClient();

  Endpoint endpoint_{};
  Limits limits_{};
  std::unique_ptr<FramedChannel> channel_;
  std::mutex mutex_;
  std::uint64_t sequence_{0};
};

}  // namespace apf
