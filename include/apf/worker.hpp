#pragma once

#include "apf/backend.hpp"
#include "apf/fabric.hpp"
#include "apf/limits.hpp"
#include "apf/protocol.hpp"
#include "apf/result.hpp"
#include "apf/transport.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace apf {

struct WorkerOptions {
  Endpoint coordinator{};
  /// Stable worker identity. When invalid, the coordinator assigns one.
  WorkerId worker_id{};
  /// Physical incarnation identity. A replacement process must present a fresh
  /// boot identity; the old one loses authority permanently.
  WorkerBootId worker_boot{};
  std::string backend_name{"synthetic"};
  std::shared_ptr<AcceleratorBackend> backend;
  Limits limits{};
  std::string description;
  /// Optional path used to persist the physical device model so that a
  /// replacement process observes the same physical reality after the original
  /// worker is killed.
  std::string device_state_path;
  /// Fault injection: the worker applies the next mutation physically and then
  /// exits before acknowledging, which is what an ambiguous completion looks
  /// like from the coordinator.
  bool ambiguous_next_mutation{false};
  /// Fault injection: the worker exits immediately after connecting.
  bool die_after_register{false};
  /// Save the physical device state after this many mutations.
  std::uint32_t checkpoint_every{1};
};

/// Worker incarnation. Owns the hardware backend and executes physical
/// mutation on behalf of the coordinator, reporting observed physical state
/// rather than assumed results.
class PartitionWorker {
 public:
  static Result<std::unique_ptr<PartitionWorker>> create(WorkerOptions options,
                                                         ClockPtr clock = nullptr);
  ~PartitionWorker();

  PartitionWorker(const PartitionWorker&) = delete;
  PartitionWorker& operator=(const PartitionWorker&) = delete;

  /// Connects, performs the hello handshake, registers and publishes evidence.
  Status start();
  /// Stops applying new mutations and closes the connection.
  Status stop();
  /// Serves coordinator messages in the calling thread until shutdown.
  Status serve_forever();

  const WorkerId& id() const noexcept;
  const WorkerBootId& boot() const noexcept;
  bool connected() const noexcept;
  CoordinatorEpoch epoch() const noexcept;
  AcceleratorBackend& backend();
  /// Re-publishes device evidence to the coordinator.
  Status republish_evidence();
  /// Saves the physical device model when the backend supports it.
  Status checkpoint_device_state();
  bool shutdown_requested() const;

 private:
  PartitionWorker();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace apf
