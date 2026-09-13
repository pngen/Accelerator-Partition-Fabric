#pragma once

#include "apf/fabric.hpp"
#include "apf/persistence.hpp"
#include "apf/protocol.hpp"
#include "apf/result.hpp"
#include "apf/transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace apf {

struct CoordinatorOptions {
  /// Listening endpoint. Port 0 lets the operating system choose, and the
  /// actually bound endpoint is reported after start().
  Endpoint listen{};
  FabricOptions fabric{};
  Limits limits{};
  /// Optional durable state path. When set, structure is reloaded on start and
  /// saved after every material change.
  std::string state_path;
  bool persist{false};
  /// When true, mutations may only be dispatched to a registered worker.
  bool require_worker_for_mutation{true};
};

/// Distributed coordinator. Owns logical authority: capability publications,
/// plans, reservations, lifecycle, accounting and generation binding. Physical
/// mutation is delegated to worker incarnations over a framed TCP protocol.
class PartitionCoordinator {
 public:
  static Result<std::unique_ptr<PartitionCoordinator>> create(CoordinatorOptions options);
  ~PartitionCoordinator();

  PartitionCoordinator(const PartitionCoordinator&) = delete;
  PartitionCoordinator& operator=(const PartitionCoordinator&) = delete;

  /// Binds the listener, loads durable state when configured, advances the
  /// coordinator epoch and starts serving.
  Status start();
  /// Stops accepting work, fences every live worker, joins all serving threads,
  /// resolves pending attempts conservatively and closes the transport.
  Status stop();
  bool running() const;
  bool closed() const;

  const Endpoint& endpoint() const noexcept;
  CoordinatorEpoch epoch() const;
  PartitionFabric& fabric();
  const PartitionFabric& fabric() const;

  std::vector<WorkerRecord> workers() const;
  Result<WorkerRecord> worker(const WorkerId& id) const;

  /// Dispatches a mutation to the worker bound to the reservation and waits for
  /// the verified result. Returns AmbiguousCompletion when the worker dies
  /// after the mutation request was sent.
  Result<MutationOutcome> dispatch_mutation(const PartitionReservationId& reservation,
                                            AttemptKind kind);
  /// Sends a fencing notice to a live worker and fences it locally.
  Status fence_worker(const WorkerId& worker, const WorkerBootId& boot, std::string reason);

  /// Serves connections in the calling thread until shutdown is requested.
  Status serve_forever();
  /// Requests a clean shutdown from another thread or from a signal handler.
  void request_shutdown(std::string reason);
  bool shutdown_requested() const;

  /// Persists durable state when persistence is configured.
  Status persist();

  /// Asks one worker incarnation for the current physical layout of a device.
  Result<BackendLayout> query_worker_layout(const WorkerId& worker,
                                            const WorkerBootId& worker_boot,
                                            std::string_view stable_key);
  /// Publishes device and layout evidence from a worker into logical state.
  Status publish_evidence(const WorkerId& worker, const WorkerBootId& worker_boot,
                          const std::vector<BackendAccelerator>& devices,
                          const std::vector<BackendLayout>& layouts);
  /// Runs reconciliation for a device using a worker's observation. This is the
  /// only way a coordinator that owns no hardware can learn physical truth.
  Result<ReconciliationReport> reconcile_via_worker(const WorkerId& worker,
                                                    const WorkerBootId& worker_boot,
                                                    std::string_view stable_key);
  /// Full authoritative path for the distributed deployment: plan, reserve
  /// against a worker incarnation, dispatch the physical mutation, verify the
  /// observed result and publish authority.
  Result<MutationOutcome> create_partitions(const PartitionRequest& request, const WorkerId& worker,
                                            const WorkerBootId& worker_boot,
                                            std::string_view stable_key);
  /// Destroys one partition through its owning worker.
  Result<MutationOutcome> destroy_partition(const PartitionId& partition);
  bool is_worker_live(const WorkerId& worker, const WorkerBootId& worker_boot) const;
  const Limits& limits() const noexcept;

 private:
  PartitionCoordinator();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace apf
