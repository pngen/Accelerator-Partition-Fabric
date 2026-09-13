#include "apf/snapshot.hpp"

#include <algorithm>

namespace apf {
namespace {

template <class T, class Key>
const T* find_by_id(const std::vector<T>& values, const Key& key, Key T::*member) {
  for (const T& value : values) {
    if (value.*member == key) {
      return &value;
    }
  }
  return nullptr;
}

}  // namespace

const AcceleratorView* FabricSnapshot::find_accelerator(const AcceleratorId& id) const noexcept {
  for (const AcceleratorView& view : accelerators) {
    if (view.accelerator.id == id) {
      return &view;
    }
  }
  return nullptr;
}

const PartitionRecord* FabricSnapshot::find_partition(const PartitionId& id) const noexcept {
  return find_by_id(partitions, id, &PartitionRecord::id);
}

const PartitionProfile* FabricSnapshot::find_profile(const PartitionProfileId& id) const noexcept {
  return find_by_id(profiles, id, &PartitionProfile::id);
}

const PartitionReservation* FabricSnapshot::find_reservation(
    const PartitionReservationId& id) const noexcept {
  return find_by_id(reservations, id, &PartitionReservation::id);
}

const PartitionAttempt* FabricSnapshot::find_attempt(const PartitionAttemptId& id) const noexcept {
  return find_by_id(attempts, id, &PartitionAttempt::id);
}

const WorkerRecord* FabricSnapshot::find_worker(const WorkerId& id) const noexcept {
  return find_by_id(workers, id, &WorkerRecord::id);
}

std::vector<PartitionId> FabricSnapshot::partitions_of(const AcceleratorId& accelerator) const {
  std::vector<PartitionId> out;
  for (const PartitionRecord& record : partitions) {
    if (record.accelerator == accelerator) {
      out.push_back(record.id);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<PartitionAssignment> FabricSnapshot::assignments_of(
    const PartitionId& partition) const {
  std::vector<PartitionAssignment> out;
  for (const PartitionAssignment& assignment : assignments) {
    if (assignment.partition == partition) {
      out.push_back(assignment);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const PartitionAssignment& lhs, const PartitionAssignment& rhs) {
              return lhs.id < rhs.id;
            });
  return out;
}

std::string FabricSnapshot::render() const {
  std::string out;
  out += "snapshot generation=";
  out += std::to_string(generation.value());
  out += " state_generation=";
  out += std::to_string(state_generation.value());
  out += " coordinator_epoch=";
  out += std::to_string(coordinator_epoch.value());
  out += " created_at_ms=";
  out += std::to_string(created_at_ms);
  out += " closed=";
  out += closed ? "true" : "false";
  out += " instance=";
  out += instance_id;
  out += "\n";
  out += render_logical();
  return out;
}

std::string FabricSnapshot::render_logical() const {
  std::string out;
  out += "policy generation=";
  out += std::to_string(policy.generation.value());
  out += " name=";
  out += policy.name;
  out += " destructive_reconfiguration=";
  out += policy.allow_destructive_reconfiguration ? "allowed" : "forbidden";
  out += " drain=";
  out += policy.allow_drain ? "allowed" : "forbidden";
  out += "\n";

  out += "accelerators=";
  out += std::to_string(accelerators.size());
  out += "\n";
  for (const AcceleratorView& view : accelerators) {
    const AcceleratorRecord& record = view.accelerator;
    out += "  accelerator ";
    out += record.id.str();
    out += " gen=";
    out += std::to_string(record.generation.value());
    out += " boot=";
    out += record.boot_id.valid() ? record.boot_id.hex() : std::string("unknown");
    out += " backend=";
    out += record.backend;
    out += " model=";
    out += record.identifiers.model;
    out += " uuid=";
    out += record.identifiers.uuid;
    out += " provenance=";
    out += to_string(record.provenance);
    out += " freshness=";
    out += to_string(view.freshness);
    out += " partition_support=";
    out += to_string(record.capability.support);
    out += " mechanism=";
    out += to_string(record.capability.mechanism);
    out += " capability_gen=";
    out += std::to_string(record.capability.generation.value());
    out += " max_partitions=";
    out += std::to_string(record.capability.max_partition_count);
    out += " active=";
    out += std::to_string(view.active_partitions);
    out += " draining=";
    out += std::to_string(view.draining_partitions);
    out += " stale=";
    out += std::to_string(view.stale_partitions);
    out += "\n";
    if (!record.unsupported_reason.empty()) {
      out += "    unsupported_reason=";
      out += record.unsupported_reason;
      out += "\n";
    }
    out += "    totals=";
    out += record.physical_totals.format();
    out += "\n";
    out += "    free=";
    out += view.ledger.bucket(CapacityBucket::Free).format();
    out += "\n";
    out += "    reserved=";
    out += view.ledger.bucket(CapacityBucket::Reserved).format();
    out += "\n";
    out += "    active=";
    out += view.ledger.bucket(CapacityBucket::Active).format();
    out += "\n";
    out += "    draining=";
    out += view.ledger.bucket(CapacityBucket::Draining).format();
    out += "\n";
    out += "    reconfiguration_held=";
    out += view.ledger.bucket(CapacityBucket::ReconfigurationHeld).format();
    out += "\n";
  }

  out += "profiles=";
  out += std::to_string(profiles.size());
  out += "\n";
  for (const PartitionProfile& profile : profiles) {
    out += "  profile ";
    out += profile.id.str();
    out += " name=";
    out += profile.name;
    out += " backend=";
    out += profile.backend;
    out += " vendor_native=";
    out += profile.vendor_native;
    out += " resources=";
    out += profile.resources.format();
    out += " compute_slices=";
    out += std::to_string(profile.compute_slice_count);
    out += " memory_slices=";
    out += std::to_string(profile.memory_slice_count);
    out += " destructive_reconfiguration=";
    out += profile.requires_full_device_reconfiguration ? "required" : "not-required";
    out += "\n";
  }

  out += "partitions=";
  out += std::to_string(partitions.size());
  out += "\n";
  for (const PartitionRecord& record : partitions) {
    out += "  partition ";
    out += record.id.str();
    out += " gen=";
    out += std::to_string(record.generation.value());
    out += " state=";
    out += to_string(record.state);
    out += " profile=";
    out += record.profile.str();
    out += " accelerator=";
    out += record.accelerator.str();
    out += " accel_gen=";
    out += std::to_string(record.accelerator_generation.value());
    out += " accel_boot=";
    out += record.accelerator_boot.valid() ? record.accelerator_boot.hex() : std::string("unknown");
    out += " native_id=";
    out += record.native_identity.native_id;
    out += " assignment=";
    out += to_string(record.assignment_state);
    out += " drain=";
    out += to_string(record.drain.state);
    out += " provenance=";
    out += to_string(record.provenance);
    out += " resources=";
    out += record.resources.format();
    if (record.externally_observed) {
      out += " externally_observed";
    }
    out += "\n";
  }

  out += "reservations=";
  out += std::to_string(reservations.size());
  out += "\n";
  for (const PartitionReservation& reservation : reservations) {
    out += "  reservation ";
    out += reservation.id.str();
    out += " lifecycle=";
    out += to_string(reservation.lifecycle);
    out += " plan=";
    out += reservation.plan.str();
    out += " accelerator=";
    out += reservation.accelerator.str();
    out += " worker=";
    out += reservation.worker.valid() ? reservation.worker.str() : std::string("none");
    out += " resources=";
    out += reservation.resources.format();
    out += "\n";
  }

  out += "attempts=";
  out += std::to_string(attempts.size());
  out += "\n";
  for (const PartitionAttempt& attempt : attempts) {
    out += "  attempt ";
    out += attempt.id.str();
    out += " kind=";
    out += to_string(attempt.kind);
    out += " state=";
    out += to_string(attempt.state);
    out += " reservation=";
    out += attempt.reservation.str();
    out += " worker_boot=";
    out += attempt.worker_boot.valid() ? attempt.worker_boot.hex() : std::string("unknown");
    out += "\n";
  }

  out += "assignments=";
  out += std::to_string(assignments.size());
  out += "\n";
  for (const PartitionAssignment& assignment : assignments) {
    out += "  assignment ";
    out += assignment.id.str();
    out += " partition=";
    out += assignment.partition.str();
    out += " partition_gen=";
    out += std::to_string(assignment.partition_generation.value());
    out += " workload=";
    out += assignment.workload_id;
    out += " tenant=";
    out += assignment.tenant_id;
    out += " isolation=";
    out += assignment.isolation.required.format();
    out += "\n";
  }

  out += "workers=";
  out += std::to_string(workers.size());
  out += "\n";
  for (const WorkerRecord& worker : workers) {
    out += "  worker ";
    out += worker.id.valid() ? worker.id.str() : std::string("unassigned");
    out += " boot=";
    out += worker.boot.valid() ? worker.boot.hex() : std::string("unknown");
    out += " endpoint=";
    out += worker.endpoint;
    out += " backend=";
    out += worker.backend;
    out += " alive=";
    out += worker.alive ? "true" : "false";
    out += " fenced=";
    out += worker.fenced ? "true" : "false";
    out += " devices=";
    out += std::to_string(worker.device_count);
    if (!worker.fence_reason.empty()) {
      out += " fence_reason=";
      out += worker.fence_reason;
    }
    out += "\n";
  }
  return out;
}

}  // namespace apf
