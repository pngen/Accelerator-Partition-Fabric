#include "apf/backend_synthetic.hpp"

#include "apf/codec.hpp"
#include "apf/process.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>

namespace apf {
namespace {

constexpr std::array<char, 8> kSyntheticMagic{{'A', 'P', 'F', 'S', 'Y', 'N', '1', '\0'}};
constexpr std::uint32_t kSyntheticFormatVersion = 1;

/// Profile identity is derived from the backend and the portable profile shape,
/// not from the individual device: two accelerators of the same backend that
/// publish the same shape publish the same profile identity, which is what
/// makes cross-device planning meaningful.
std::uint64_t derive_profile_id(const std::string& name) {
  const std::string material = "apf-synthetic-profile|synthetic|" + name;
  std::uint64_t hash = 1469598103934665603ull;
  for (const char ch : material) {
    hash ^= static_cast<std::uint8_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash == 0 ? 1 : hash;
}

std::string make_native_id(const std::string& key, std::uint64_t index) {
  return key + "#" + std::to_string(index);
}

}  // namespace

SyntheticDeviceSpec make_default_synthetic_device(std::string key, std::uint64_t memory_gib,
                                                  bool full_profile_conflicts) {
  SyntheticDeviceSpec spec;
  spec.key = std::move(key);
  spec.uuid = "SYN-" + spec.key;
  spec.hardware_id = "synthetic-hw-1";
  spec.total_compute_slices = 7;
  spec.total_memory_slices = 8;
  spec.max_partition_count = 7;
  spec.physical_totals = ResourceVector{};
  (void)spec.physical_totals.set(ResourceDimension::ComputeShare, kShareScale);
  (void)spec.physical_totals.set(ResourceDimension::MemoryBytes, gib(memory_gib));
  (void)spec.physical_totals.set(ResourceDimension::MemoryBandwidthShare, kShareScale);
  (void)spec.physical_totals.set(ResourceDimension::ExecutionEngineShare, kShareScale);
  spec.isolation = IsolationSet::none();
  spec.isolation.add(IsolationProperty::LogicalSeparation);
  spec.isolation.add(IsolationProperty::MemoryIsolation);
  spec.isolation.add(IsolationProperty::FaultIsolation);
  spec.isolation.add(IsolationProperty::EngineIsolation);
  spec.isolation.add(IsolationProperty::AddressSpaceIsolation);
  spec.minimum_allocation = ResourceVector{};
  (void)spec.minimum_allocation.set(ResourceDimension::ComputeShare, kShareScale / 7);
  (void)spec.minimum_allocation.set(ResourceDimension::MemoryBytes, gib(memory_gib / 8));

  const std::uint64_t slice_memory = gib(memory_gib / 8);
  const auto add_profile = [&](const std::string& name, std::uint32_t compute_slices,
                               std::uint32_t memory_slices, std::uint64_t compute_ppm,
                               bool full_device) {
    SyntheticProfileSpec profile;
    profile.name = name;
    profile.vendor_native = name;
    profile.compute_slices = compute_slices;
    profile.memory_slices = memory_slices;
    profile.resources = ResourceVector{};
    (void)profile.resources.set(ResourceDimension::ComputeShare, compute_ppm);
    (void)profile.resources.set(ResourceDimension::MemoryBytes,
                                slice_memory * static_cast<std::uint64_t>(memory_slices));
    (void)profile.resources.set(ResourceDimension::MemoryBandwidthShare,
                                compute_ppm == kShareScale ? kShareScale : compute_ppm);
    (void)profile.resources.set(ResourceDimension::ExecutionEngineShare,
                                compute_ppm == kShareScale ? kShareScale : compute_ppm);
    profile.requires_full_device_reconfiguration = full_device;
    profile.live_repartitioning_supported = !full_device;
    profile.engine_grouping = EngineGrouping::Dedicated;
    if (full_profile_conflicts && full_device) {
      profile.incompatible_with = {"synthetic-1g", "synthetic-2g", "synthetic-3g"};
    }
    if (full_profile_conflicts && !full_device) {
      profile.incompatible_with = {"synthetic-7g"};
    }
    spec.profiles.push_back(std::move(profile));
  };
  add_profile("synthetic-1g", 1, 1, kShareScale / 7, false);
  add_profile("synthetic-2g", 2, 2, (kShareScale * 2) / 7, false);
  add_profile("synthetic-3g", 3, 4, (kShareScale * 3) / 7, false);
  add_profile("synthetic-7g", 7, 8, kShareScale, true);
  return spec;
}

struct SyntheticBackend::Impl {
  struct Partition {
    std::string native_id;
    std::string instance_uuid;
    std::string vendor_native;
    PartitionProfileId profile{};
    ResourceVector resources{};
    std::uint32_t compute_slices{0};
    std::uint32_t memory_slices{0};
    std::string isolation_domain;
    IsolationSet isolation{};
    bool live{true};
  };

  struct Device {
    SyntheticDeviceSpec spec;
    AcceleratorBootId boot{};
    std::vector<Partition> partitions;
    std::uint64_t next_partition_index{1};
    std::uint64_t applied_tokens_capacity{0};
    std::map<std::uint64_t, BackendMutationResult> applied_tokens;
    std::uint64_t mutation_count{0};
    bool present{true};
  };

  mutable std::mutex mutex;
  ClockPtr clock;
  std::map<std::string, Device> devices;
  SyntheticFaults faults;

  std::uint64_t now() const { return clock ? clock->now_ms() : system_now_ms(); }

  Device* find(const std::string& key) {
    const auto it = devices.find(key);
    return it == devices.end() ? nullptr : &it->second;
  }

  const SyntheticProfileSpec* find_profile(const Device& device, const std::string& vendor_native) {
    for (const SyntheticProfileSpec& profile : device.spec.profiles) {
      if (profile.name == vendor_native || profile.vendor_native == vendor_native) {
        return &profile;
      }
    }
    return nullptr;
  }

  /// Builds the observed layout. The caller must already hold the backend lock:
  /// every mutation path publishes its observed result through this function
  /// without re-entering the lock.
  BackendLayout build_layout(Device& device) {
    if (faults.silent_device_reset) {
      faults.silent_device_reset = false;
      device.boot = AcceleratorBootId::derive(device.boot.value() ^ 0xDEADBEEFull);
      device.partitions.clear();
    }
    BackendLayout layout;
    layout.stable_key = device.spec.key;
    layout.device_present = device.present && !faults.device_disappears;
    layout.boot_id = device.boot;
    layout.physical_totals = device.spec.physical_totals;
    layout.evidence.observed = true;
    layout.evidence.generation = EvidenceGeneration::first();
    layout.evidence.provenance = device.spec.provenance;
    layout.evidence.observed_at_ms = now();
    layout.evidence.ttl_ms = device.spec.evidence_ttl_ms;
    layout.evidence.source = "synthetic";
    layout.detail = "synthetic observed layout";
    if (!layout.device_present) {
      return layout;
    }
    for (const Partition& partition : device.partitions) {
      if (!partition.live) {
        continue;
      }
      BackendNativePartition native;
      native.native_id = partition.native_id;
      native.instance_uuid = partition.instance_uuid;
      native.vendor_native_profile = partition.vendor_native;
      native.profile = partition.profile;
      native.resources = partition.resources;
      native.isolation_domain = partition.isolation_domain;
      native.isolation = partition.isolation;
      native.live = partition.live;
      layout.partitions.push_back(std::move(native));
    }
    if (faults.malformed_metadata && !layout.partitions.empty()) {
      BackendNativePartition malformed = layout.partitions.front();
      malformed.native_id.clear();
      layout.partitions.push_back(std::move(malformed));
    }
    if (faults.duplicate_native_ids && !layout.partitions.empty()) {
      layout.partitions.push_back(layout.partitions.front());
    }
    return layout;
  }

  ResourceVector used_capacity(const Device& device) const {
    ResourceVector total;
    for (const Partition& partition : device.partitions) {
      if (total.empty()) {
        total = partition.resources;
        continue;
      }
      if (!total.add_assign(partition.resources).ok()) {
        return ResourceVector{};
      }
    }
    return total;
  }
};

SyntheticBackend::SyntheticBackend(ClockPtr clock) : impl_(std::make_unique<Impl>()) {
  impl_->clock = clock ? clock : make_system_clock();
}

SyntheticBackend::~SyntheticBackend() = default;

Status SyntheticBackend::add_device(const SyntheticDeviceSpec& spec) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (spec.key.empty() || spec.key.size() > 256) {
    return failure(ErrorCode::InvalidArgument, "synthetic device key is not usable");
  }
  if (impl_->devices.size() >= 64 && impl_->devices.find(spec.key) == impl_->devices.end()) {
    return failure(ErrorCode::LimitExceeded, "synthetic device bound reached");
  }
  Impl::Device device;
  device.spec = spec;
  device.boot = AcceleratorBootId::derive(spec.boot_entropy ^ 0xC0FFEEull);
  device.present = spec.device_present;
  impl_->devices[spec.key] = std::move(device);
  return success();
}

Status SyntheticBackend::remove_device(std::string_view key) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->devices.find(std::string(key));
  if (it == impl_->devices.end()) {
    return failure(ErrorCode::NotFound, "synthetic device is not registered", std::string(key));
  }
  impl_->devices.erase(it);
  return success();
}

void SyntheticBackend::set_faults(const SyntheticFaults& faults) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->faults = faults;
}

SyntheticFaults SyntheticBackend::faults() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->faults;
}

void SyntheticBackend::set_fault(std::string_view name, bool enabled) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  SyntheticFaults& faults = impl_->faults;
  const std::string key(name);
  if (key == "fail_discovery") faults.fail_discovery = enabled;
  else if (key == "fail_capability_query") faults.fail_capability_query = enabled;
  else if (key == "fail_create") faults.fail_create = enabled;
  else if (key == "error_create") faults.error_create = enabled;
  else if (key == "error_destroy") faults.error_destroy = enabled;
  else if (key == "error_reconfigure") faults.error_reconfigure = enabled;
  else if (key == "fail_destroy") faults.fail_destroy = enabled;
  else if (key == "fail_reconfigure") faults.fail_reconfigure = enabled;
  else if (key == "ambiguous_create") faults.ambiguous_create = enabled;
  else if (key == "ambiguous_destroy") faults.ambiguous_destroy = enabled;
  else if (key == "ambiguous_reconfigure") faults.ambiguous_reconfigure = enabled;
  else if (key == "mismatch_create_result") faults.mismatch_create_result = enabled;
  else if (key == "partition_disappears_after_create") {
    faults.partition_disappears_after_create = enabled;
  } else if (key == "device_disappears") faults.device_disappears = enabled;
  else if (key == "silent_device_reset") faults.silent_device_reset = enabled;
  else if (key == "external_mutation_pending") faults.external_mutation_pending = enabled;
  else if (key == "malformed_metadata") faults.malformed_metadata = enabled;
  else if (key == "duplicate_native_ids") faults.duplicate_native_ids = enabled;
}

Status SyntheticBackend::mutate_capability(std::string_view key,
                                           std::vector<SyntheticProfileSpec> profiles,
                                           std::uint32_t max_partition_count,
                                           bool partitioning_supported,
                                           std::string unsupported_reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::Device* device = impl_->find(std::string(key));
  if (device == nullptr) {
    return failure(ErrorCode::NotFound, "synthetic device is not registered", std::string(key));
  }
  device->spec.profiles = std::move(profiles);
  device->spec.max_partition_count = max_partition_count;
  device->spec.partitioning_supported = partitioning_supported;
  device->spec.unsupported_reason = std::move(unsupported_reason);
  return success();
}

Status SyntheticBackend::reset_device(std::string_view key) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::Device* device = impl_->find(std::string(key));
  if (device == nullptr) {
    return failure(ErrorCode::NotFound, "synthetic device is not registered", std::string(key));
  }
  device->boot = AcceleratorBootId::derive(device->boot.value() ^ 0x5A5A5A5Aull);
  device->partitions.clear();
  device->applied_tokens.clear();
  return success();
}

Status SyntheticBackend::set_device_present(std::string_view key, bool present) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::Device* device = impl_->find(std::string(key));
  if (device == nullptr) {
    return failure(ErrorCode::NotFound, "synthetic device is not registered", std::string(key));
  }
  device->present = present;
  return success();
}

Status SyntheticBackend::external_create(std::string_view key, std::string_view vendor_native,
                                         std::string* out_native_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::Device* device = impl_->find(std::string(key));
  if (device == nullptr) {
    return failure(ErrorCode::NotFound, "synthetic device is not registered", std::string(key));
  }
  const SyntheticProfileSpec* profile = impl_->find_profile(*device, std::string(vendor_native));
  if (profile == nullptr) {
    return failure(ErrorCode::NotFound, "synthetic profile is not published",
                   std::string(vendor_native));
  }
  Impl::Partition partition;
  partition.native_id = make_native_id(device->spec.key, device->next_partition_index++);
  partition.instance_uuid = partition.native_id + "-uuid";
  partition.vendor_native = profile->vendor_native;
  partition.profile = PartitionProfileId::from_value(
      derive_profile_id(profile->name));
  partition.resources = profile->resources;
  partition.compute_slices = profile->compute_slices;
  partition.memory_slices = profile->memory_slices;
  partition.isolation_domain = device->spec.key + "-external";
  partition.isolation = device->spec.isolation;
  device->partitions.push_back(partition);
  if (out_native_id != nullptr) {
    *out_native_id = partition.native_id;
  }
  return success();
}

Status SyntheticBackend::external_destroy(std::string_view key, std::string_view native_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::Device* device = impl_->find(std::string(key));
  if (device == nullptr) {
    return failure(ErrorCode::NotFound, "synthetic device is not registered", std::string(key));
  }
  const auto it = std::remove_if(device->partitions.begin(), device->partitions.end(),
                                 [native_id](const Impl::Partition& partition) {
                                   return partition.native_id == native_id;
                                 });
  if (it == device->partitions.end()) {
    return failure(ErrorCode::NotFound, "synthetic partition is not present",
                   std::string(native_id));
  }
  device->partitions.erase(it, device->partitions.end());
  return success();
}

Result<std::vector<BackendAccelerator>> SyntheticBackend::discover() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->faults.fail_discovery) {
    return make_error(ErrorCode::BackendFailure, "synthetic discovery fault injected");
  }
  std::vector<BackendAccelerator> out;
  const std::uint64_t now = impl_->now();
  for (auto& entry : impl_->devices) {
    Impl::Device& device = entry.second;
    if (!device.present || impl_->faults.device_disappears) {
      continue;
    }
    BackendAccelerator accelerator;
    accelerator.stable_key = device.spec.key;
    accelerator.backend = device.spec.backend;
    accelerator.identifiers.vendor = device.spec.vendor;
    accelerator.identifiers.model = device.spec.model;
    accelerator.identifiers.uuid = device.spec.uuid;
    accelerator.identifiers.driver_version = device.spec.driver_version;
    accelerator.identifiers.firmware_version = device.spec.firmware_version;
    accelerator.identifiers.hardware_id = device.spec.hardware_id;
    accelerator.physical_totals = device.spec.physical_totals;
    accelerator.boot_id = device.boot;
    accelerator.locality.name = device.spec.locality_domain;
    accelerator.locality.numa_node = device.spec.numa_node;
    accelerator.provenance = device.spec.provenance;
    accelerator.unsupported_reason =
        device.spec.partitioning_supported ? std::string() : device.spec.unsupported_reason;

    PartitionCapability& capability = accelerator.capability;
    capability.generation = CapabilityGeneration::first();
    capability.support = device.spec.partitioning_supported
                             ? PartitionSupportState::Supported
                             : (device.spec.unsupported_reason.empty()
                                    ? PartitionSupportState::Unknown
                                    : PartitionSupportState::Unsupported);
    capability.mechanism = device.spec.mechanism;
    capability.mechanism_name = device.spec.mechanism_name;
    capability.max_partition_count = device.spec.max_partition_count;
    capability.requires_reset_for_reconfiguration =
        device.spec.requires_reset_for_reconfiguration;
    capability.live_reconfiguration_supported = device.spec.live_reconfiguration_supported;
    capability.isolation = device.spec.isolation;
    capability.minimum_allocation = device.spec.minimum_allocation;
    capability.slice_granularity = device.spec.slice_granularity;
    capability.total_compute_slices = device.spec.total_compute_slices;
    capability.total_memory_slices = device.spec.total_memory_slices;
    capability.drain_estimate_ms = device.spec.drain_estimate_ms;
    capability.reconfiguration_estimate_ms = device.spec.reconfiguration_downtime_ms;
    capability.backend_name = device.spec.backend;
    capability.backend_version = "1.0.0";
    capability.driver_version = device.spec.driver_version;
    capability.tooling_requirement = "none";
    capability.unsupported_reason = accelerator.unsupported_reason;
    capability.evidence.observed = true;
    capability.evidence.generation = EvidenceGeneration::first();
    capability.evidence.provenance = device.spec.provenance;
    capability.evidence.observed_at_ms = now;
    capability.evidence.ttl_ms = device.spec.evidence_ttl_ms;
    capability.evidence.source = "synthetic";
    for (std::size_t index = 0; index < kResourceDimensionCount; ++index) {
      const auto dimension = static_cast<ResourceDimension>(index);
      if (device.spec.physical_totals.has(dimension)) {
        capability.exposed_dimensions.push_back(dimension);
      }
    }
    for (const SyntheticProfileSpec& profile : device.spec.profiles) {
      PartitionProfile record;
      record.id = PartitionProfileId::from_value(derive_profile_id(profile.name));
      record.generation = PartitionProfileGeneration::first();
      record.name = profile.name;
      record.vendor_native = profile.vendor_native;
      record.backend = device.spec.backend;
      record.mechanism = device.spec.mechanism;
      record.resources = profile.resources;
      record.compute_slice_count = profile.compute_slices;
      record.memory_slice_count = profile.memory_slices;
      record.engine_grouping = profile.engine_grouping;
      record.max_multiplicity = profile.max_multiplicity;
      record.alignment.requires_contiguous_slices = profile.requires_contiguous_slices;
      record.alignment.requires_atomic_slice = profile.requires_atomic_slice;
      record.requires_full_device_reconfiguration = profile.requires_full_device_reconfiguration;
      record.live_repartitioning_supported = profile.live_repartitioning_supported;
      record.description = "synthetic profile " + profile.name;
      for (const std::string& excluded : profile.incompatible_with) {
        record.mutually_exclusive_with.push_back(
            PartitionProfileId::from_value(derive_profile_id(excluded)));
      }
      capability.supported_profiles.push_back(record.id);
      accelerator.profiles.push_back(std::move(record));
    }
    accelerator.evidence = capability.evidence;
    out.push_back(std::move(accelerator));
  }
  return out;
}

Result<BackendLayout> SyntheticBackend::query_layout(std::string_view stable_key) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::Device* device = impl_->find(std::string(stable_key));
  if (device == nullptr) {
    return make_error(ErrorCode::NotFound, "synthetic device is not registered",
                      std::string(stable_key));
  }
  if (impl_->faults.fail_capability_query) {
    return make_error(ErrorCode::BackendFailure, "synthetic capability query fault injected");
  }
  return impl_->build_layout(*device);
}

Result<BackendMutationResult> SyntheticBackend::create_partitions(
    const PartitionMutationRequest& request) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->faults.fail_create) {
    // A positively refused mutation: the backend knows nothing was applied.
    BackendMutationResult refused;
    refused.accepted = false;
    refused.detail = "synthetic create fault injected: refused before applying";
    return refused;
  }
  if (impl_->faults.error_create) {
    // A call that did not complete: whether anything was applied is unknown.
    return make_error(ErrorCode::BackendFailure,
                      "synthetic create call failed without a definite outcome");
  }
  Impl::Device* device = impl_->find(request.stable_key);
  if (device == nullptr) {
    return make_error(ErrorCode::NotFound, "synthetic device is not registered",
                      request.stable_key);
  }
  if (!device->present || impl_->faults.device_disappears) {
    return make_error(ErrorCode::DeviceUnavailable, "synthetic device is not present",
                      request.stable_key);
  }
  if (request.expected_boot.valid() && request.expected_boot != device->boot) {
    return make_error(ErrorCode::StaleGeneration,
                      "synthetic device incarnation does not match the request",
                      request.expected_boot.hex());
  }
  const auto applied = device->applied_tokens.find(request.idempotency_token);
  if (applied != device->applied_tokens.end() && request.idempotency_token != 0) {
    BackendMutationResult repeated = applied->second;
    repeated.detail += " (idempotent replay)";
    return repeated;
  }
  if (request.desired.empty()) {
    return make_error(ErrorCode::InvalidRequest, "synthetic create received no partitions");
  }
  if (device->partitions.size() + request.desired.size() > device->spec.max_partition_count) {
    return make_error(ErrorCode::InsufficientCapacity,
                      "synthetic device cannot hold that many partitions",
                      std::to_string(device->spec.max_partition_count));
  }
  ResourceVector used = impl_->used_capacity(*device);
  std::uint32_t compute_slices = 0;
  std::uint32_t memory_slices = 0;
  for (const Impl::Partition& partition : device->partitions) {
    compute_slices += partition.compute_slices;
    memory_slices += partition.memory_slices;
  }
  for (const NativePartitionSpec& spec : request.desired) {
    const SyntheticProfileSpec* profile = impl_->find_profile(*device, spec.vendor_native_profile);
    if (profile == nullptr) {
      return make_error(ErrorCode::UnsupportedCapability,
                        "synthetic device does not publish the requested profile",
                        spec.vendor_native_profile);
    }
    compute_slices += profile->compute_slices;
    memory_slices += profile->memory_slices;
  }
  if (compute_slices > device->spec.total_compute_slices ||
      memory_slices > device->spec.total_memory_slices) {
    return make_error(ErrorCode::Fragmented,
                      "synthetic slice geometry cannot assemble the requested layout");
  }
  for (const NativePartitionSpec& spec : request.desired) {
    const SyntheticProfileSpec* profile = impl_->find_profile(*device, spec.vendor_native_profile);
    if (used.empty()) {
      used = profile->resources;
      continue;
    }
    const Status added = used.add_assign(profile->resources);
    if (!added.ok()) {
      return make_error(ErrorCode::InsufficientCapacity,
                        "synthetic capacity overflow while assembling the layout");
    }
  }
  if (!used.is_subset_of(device->spec.physical_totals)) {
    return make_error(ErrorCode::InsufficientCapacity,
                      "synthetic device does not have enough capacity: needed " + used.format());
  }

  BackendMutationResult result;
  result.accepted = true;
  for (const NativePartitionSpec& spec : request.desired) {
    const SyntheticProfileSpec* profile = impl_->find_profile(*device, spec.vendor_native_profile);
    Impl::Partition partition;
    partition.native_id = make_native_id(device->spec.key, device->next_partition_index++);
    partition.instance_uuid = partition.native_id + "-uuid";
    partition.vendor_native = profile->vendor_native;
    partition.profile = PartitionProfileId::from_value(
        derive_profile_id(profile->name));
    partition.resources = profile->resources;
    partition.compute_slices = profile->compute_slices;
    partition.memory_slices = profile->memory_slices;
    partition.isolation_domain = device->spec.key + "-domain-" +
                                 std::to_string(device->next_partition_index);
    partition.isolation = device->spec.isolation;
    result.created_native_ids.push_back(partition.native_id);
    device->partitions.push_back(std::move(partition));
  }
  device->mutation_count += 1;
  if (impl_->faults.partition_disappears_after_create) {
    device->partitions.clear();
    result.ambiguous = true;
    result.detail = "synthetic partitions were removed immediately after creation";
  } else if (impl_->faults.ambiguous_create) {
    result.ambiguous = true;
    result.detail = "synthetic create applied but the acknowledgement is ambiguous";
  } else {
    result.detail = "synthetic partitions created";
  }
  if (impl_->faults.mismatch_create_result) {
    result.created_native_ids = {"phantom-partition-that-does-not-exist"};
    result.detail = "synthetic backend reported a result that does not match what it did";
  }
  result.observed_layout = impl_->build_layout(*device);
  device->applied_tokens[request.idempotency_token] = result;
  while (device->applied_tokens.size() > 256) {
    device->applied_tokens.erase(device->applied_tokens.begin());
  }
  return result;
}

Result<BackendMutationResult> SyntheticBackend::destroy_partitions(
    const PartitionMutationRequest& request) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->faults.fail_destroy) {
    BackendMutationResult refused;
    refused.accepted = false;
    refused.detail = "synthetic destroy fault injected: refused before applying";
    return refused;
  }
  if (impl_->faults.error_destroy) {
    return make_error(ErrorCode::BackendFailure,
                      "synthetic destroy call failed without a definite outcome");
  }
  Impl::Device* device = impl_->find(request.stable_key);
  if (device == nullptr) {
    return make_error(ErrorCode::NotFound, "synthetic device is not registered",
                      request.stable_key);
  }
  if (request.expected_boot.valid() && request.expected_boot != device->boot) {
    return make_error(ErrorCode::StaleGeneration,
                      "synthetic device incarnation does not match the request");
  }
  const auto applied = device->applied_tokens.find(request.idempotency_token);
  if (applied != device->applied_tokens.end() && request.idempotency_token != 0) {
    BackendMutationResult repeated = applied->second;
    repeated.detail += " (idempotent replay)";
    return repeated;
  }
  if (request.remove_native_ids.empty()) {
    return make_error(ErrorCode::InvalidRequest, "synthetic destroy received no partitions");
  }
  for (const std::string& native_id : request.remove_native_ids) {
    const auto it = std::remove_if(device->partitions.begin(), device->partitions.end(),
                                   [&native_id](const Impl::Partition& partition) {
                                     return partition.native_id == native_id;
                                   });
    device->partitions.erase(it, device->partitions.end());
  }
  device->mutation_count += 1;
  BackendMutationResult result;
  result.accepted = true;
  result.detail = "synthetic partitions destroyed";
  if (impl_->faults.ambiguous_destroy) {
    result.ambiguous = true;
    result.detail = "synthetic destroy applied but the acknowledgement is ambiguous";
  }
  result.observed_layout = impl_->build_layout(*device);
  device->applied_tokens[request.idempotency_token] = result;
  return result;
}

Result<BackendMutationResult> SyntheticBackend::reconfigure_layout(
    const PartitionMutationRequest& request) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->faults.fail_reconfigure) {
    BackendMutationResult refused;
    refused.accepted = false;
    refused.detail = "synthetic reconfigure fault injected: refused before applying";
    return refused;
  }
  if (impl_->faults.error_reconfigure) {
    return make_error(ErrorCode::BackendFailure,
                      "synthetic reconfigure call failed without a definite outcome");
  }
  Impl::Device* device = impl_->find(request.stable_key);
  if (device == nullptr) {
    return make_error(ErrorCode::NotFound, "synthetic device is not registered",
                      request.stable_key);
  }
  if (request.expected_boot.valid() && request.expected_boot != device->boot) {
    return make_error(ErrorCode::StaleGeneration,
                      "synthetic device incarnation does not match the request");
  }
  const auto applied = device->applied_tokens.find(request.idempotency_token);
  if (applied != device->applied_tokens.end() && request.idempotency_token != 0) {
    BackendMutationResult repeated = applied->second;
    repeated.detail += " (idempotent replay)";
    return repeated;
  }
  if (request.desired.empty()) {
    return make_error(ErrorCode::InvalidRequest, "synthetic reconfiguration target is empty");
  }
  if (request.desired.size() > device->spec.max_partition_count) {
    return make_error(ErrorCode::InsufficientCapacity,
                      "synthetic device cannot hold that many partitions");
  }
  std::uint32_t compute_slices = 0;
  std::uint32_t memory_slices = 0;
  ResourceVector target;
  for (const NativePartitionSpec& spec : request.desired) {
    const SyntheticProfileSpec* profile = impl_->find_profile(*device, spec.vendor_native_profile);
    if (profile == nullptr) {
      return make_error(ErrorCode::UnsupportedCapability,
                        "synthetic device does not publish the requested profile",
                        spec.vendor_native_profile);
    }
    compute_slices += profile->compute_slices;
    memory_slices += profile->memory_slices;
    if (target.empty()) {
      target = profile->resources;
    } else if (!target.add_assign(profile->resources).ok()) {
      return make_error(ErrorCode::InsufficientCapacity, "synthetic target layout overflows");
    }
  }
  if (compute_slices > device->spec.total_compute_slices ||
      memory_slices > device->spec.total_memory_slices) {
    return make_error(ErrorCode::Fragmented,
                      "synthetic slice geometry cannot assemble the target layout");
  }
  if (!target.is_subset_of(device->spec.physical_totals)) {
    return make_error(ErrorCode::InsufficientCapacity,
                      "synthetic target layout exceeds physical capacity");
  }
  device->partitions.clear();
  BackendMutationResult result;
  result.accepted = true;
  for (const NativePartitionSpec& spec : request.desired) {
    const SyntheticProfileSpec* profile = impl_->find_profile(*device, spec.vendor_native_profile);
    Impl::Partition partition;
    partition.native_id = make_native_id(device->spec.key, device->next_partition_index++);
    partition.instance_uuid = partition.native_id + "-uuid";
    partition.vendor_native = profile->vendor_native;
    partition.profile = PartitionProfileId::from_value(
        derive_profile_id(profile->name));
    partition.resources = profile->resources;
    partition.compute_slices = profile->compute_slices;
    partition.memory_slices = profile->memory_slices;
    partition.isolation_domain = device->spec.key + "-domain-" +
                                 std::to_string(device->next_partition_index);
    partition.isolation = device->spec.isolation;
    result.created_native_ids.push_back(partition.native_id);
    device->partitions.push_back(std::move(partition));
  }
  device->mutation_count += 1;
  result.detail = "synthetic layout reconfigured";
  if (impl_->faults.ambiguous_reconfigure) {
    result.ambiguous = true;
    result.detail = "synthetic reconfiguration applied but the acknowledgement is ambiguous";
  }
  if (impl_->faults.mismatch_create_result) {
    device->partitions.clear();
    result.detail = "synthetic backend performed a layout different from the request";
  }
  result.observed_layout = impl_->build_layout(*device);
  device->applied_tokens[request.idempotency_token] = result;
  return result;
}

Result<bool> SyntheticBackend::validate_partition(std::string_view stable_key,
                                                  std::string_view native_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::Device* device = impl_->find(std::string(stable_key));
  if (device == nullptr) {
    return make_error(ErrorCode::NotFound, "synthetic device is not registered");
  }
  for (const Impl::Partition& partition : device->partitions) {
    if (partition.native_id == native_id) {
      return true;
    }
  }
  return false;
}

std::uint64_t SyntheticBackend::estimate_reconfiguration_downtime_ms(
    std::string_view stable_key, const std::vector<NativePartitionSpec>& desired) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::Device* device = impl_->find(std::string(stable_key));
  (void)desired;
  if (device == nullptr) {
    return 0;
  }
  return device->spec.reconfiguration_downtime_ms;
}

Result<std::string> SyntheticBackend::export_state() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Limits limits;
  ByteWriter writer(&limits, 1024);
  writer.u32(kSyntheticFormatVersion);
  writer.u32(static_cast<std::uint32_t>(impl_->devices.size()));
  for (const auto& entry : impl_->devices) {
    const Impl::Device& device = entry.second;
    writer.text(device.spec.key);
    writer.u64(device.boot.value());
    writer.boolean(device.present);
    writer.u64(device.next_partition_index);
    writer.u32(static_cast<std::uint32_t>(device.partitions.size()));
    for (const Impl::Partition& partition : device.partitions) {
      writer.text(partition.native_id);
      writer.text(partition.instance_uuid);
      writer.text(partition.vendor_native);
      writer.u64(partition.profile.value());
      codec::encode(writer, partition.resources);
      writer.u32(partition.compute_slices);
      writer.u32(partition.memory_slices);
      writer.text(partition.isolation_domain);
      writer.u32(partition.isolation.mask());
      writer.boolean(partition.live);
    }
  }
  if (!writer.ok()) {
    return writer.error();
  }
  std::string out(reinterpret_cast<const char*>(writer.data().data()), writer.data().size());
  const std::string header(reinterpret_cast<const char*>(kSyntheticMagic.data()),
                           kSyntheticMagic.size());
  return header + out;
}

Status SyntheticBackend::import_state(std::string_view bytes) {
  if (bytes.size() < kSyntheticMagic.size() + 8) {
    return failure(ErrorCode::InvalidArgument, "synthetic state payload is truncated");
  }
  if (std::memcmp(bytes.data(), kSyntheticMagic.data(), kSyntheticMagic.size()) != 0) {
    return failure(ErrorCode::InvalidArgument, "synthetic state payload has invalid magic");
  }
  Limits limits;
  ByteReader reader(reinterpret_cast<const std::uint8_t*>(bytes.data()) + kSyntheticMagic.size(),
                    bytes.size() - kSyntheticMagic.size(), limits);
  const std::uint32_t version = reader.u32();
  if (version != kSyntheticFormatVersion) {
    return failure(ErrorCode::InvalidArgument, "synthetic state payload has an unsupported version",
                   std::to_string(version));
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::uint32_t device_count = reader.u32();
  if (device_count > 64) {
    return failure(ErrorCode::LimitExceeded, "synthetic state payload declares too many devices");
  }
  for (std::uint32_t index = 0; index < device_count && reader.ok(); ++index) {
    const std::string key = reader.text(256);
    const std::uint64_t boot = reader.u64();
    const bool present = reader.boolean();
    const std::uint64_t next_index = reader.u64();
    const std::uint32_t partition_count = reader.u32();
    if (partition_count > 512) {
      return failure(ErrorCode::LimitExceeded, "synthetic state declares too many partitions");
    }
    Impl::Device* device = impl_->find(key);
    if (device == nullptr) {
      // A device the operator no longer configures is skipped rather than
      // resurrected from a stale file.
      for (std::uint32_t skip = 0; skip < partition_count && reader.ok(); ++skip) {
        (void)reader.text(256);
        (void)reader.text(256);
        (void)reader.text(256);
        (void)reader.u64();
        (void)codec::decode(reader);
        (void)reader.u32();
        (void)reader.u32();
        (void)reader.text(256);
        (void)reader.u32();
        (void)reader.boolean();
      }
      continue;
    }
    device->boot = AcceleratorBootId::from_value(boot);
    device->present = present;
    device->next_partition_index = next_index == 0 ? 1 : next_index;
    device->partitions.clear();
    for (std::uint32_t p = 0; p < partition_count && reader.ok(); ++p) {
      Impl::Partition partition;
      partition.native_id = reader.text(256);
      partition.instance_uuid = reader.text(256);
      partition.vendor_native = reader.text(256);
      partition.profile = PartitionProfileId::from_value(reader.u64());
      partition.resources = codec::decode(reader);
      partition.compute_slices = reader.u32();
      partition.memory_slices = reader.u32();
      partition.isolation_domain = reader.text(256);
      partition.isolation.set_mask(reader.u32());
      partition.live = reader.boolean();
      device->partitions.push_back(std::move(partition));
    }
  }
  const Status finished = reader.finish();
  if (!finished.ok()) {
    return failure(ErrorCode::InvalidArgument, "synthetic state payload is malformed",
                   to_string(finished.error()));
  }
  return success();
}

}  // namespace apf