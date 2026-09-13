#include "apf/backend_nvml.hpp"

#include "apf/time.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#define APF_NVML_CALL __stdcall
#else
#include <dlfcn.h>
#define APF_NVML_CALL
#endif

namespace apf {
namespace {

// Minimal NVML surface declared locally so that the runtime never depends on a
// vendor SDK at build time and never links a vendor library directly.
using NvmlDevice = void*;
using NvmlReturn = int;

constexpr NvmlReturn kNvmlSuccess = 0;
constexpr NvmlReturn kNvmlErrorNotSupported = 3;
constexpr NvmlReturn kNvmlErrorNotFound = 6;
constexpr NvmlReturn kNvmlErrorInsufficientSize = 7;
constexpr NvmlReturn kNvmlErrorDriverNotLoaded = 9;

constexpr unsigned kNvmlMigDisabled = 0;
constexpr unsigned kNvmlMigEnabled = 1;

struct NvmlMemory {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
};

struct NvmlPciInfo {
  char bus_id_legacy[16];
  unsigned int domain;
  unsigned int bus;
  unsigned int device;
  unsigned int pci_device_id;
  unsigned int pci_sub_system_id;
  char bus_id[32];
};

using InitFn = NvmlReturn(APF_NVML_CALL*)();
using ShutdownFn = NvmlReturn(APF_NVML_CALL*)();
using ErrorStringFn = const char*(APF_NVML_CALL*)(NvmlReturn);
using DeviceGetCountFn = NvmlReturn(APF_NVML_CALL*)(unsigned int*);
using DeviceGetHandleFn = NvmlReturn(APF_NVML_CALL*)(unsigned int, NvmlDevice*);
using DeviceGetNameFn = NvmlReturn(APF_NVML_CALL*)(NvmlDevice, char*, unsigned int);
using DeviceGetUuidFn = NvmlReturn(APF_NVML_CALL*)(NvmlDevice, char*, unsigned int);
using DeviceGetMemoryInfoFn = NvmlReturn(APF_NVML_CALL*)(NvmlDevice, NvmlMemory*);
using DeviceGetCudaComputeCapabilityFn = NvmlReturn(APF_NVML_CALL*)(NvmlDevice, int*, int*);
using DeviceGetMigModeFn = NvmlReturn(APF_NVML_CALL*)(NvmlDevice, unsigned int*, unsigned int*);
using DeviceGetMaxMigDeviceCountFn = NvmlReturn(APF_NVML_CALL*)(NvmlDevice, unsigned int*);
using DeviceGetMigDeviceHandleByIndexFn = NvmlReturn(APF_NVML_CALL*)(NvmlDevice, unsigned int,
                                                                     NvmlDevice*);
using SystemGetDriverVersionFn = NvmlReturn(APF_NVML_CALL*)(char*, unsigned int);
using DeviceGetVbiosVersionFn = NvmlReturn(APF_NVML_CALL*)(NvmlDevice, char*, unsigned int);
using DeviceGetPciInfoFn = NvmlReturn(APF_NVML_CALL*)(NvmlDevice, NvmlPciInfo*);
using DeviceGetSerialFn = NvmlReturn(APF_NVML_CALL*)(NvmlDevice, char*, unsigned int);

std::uint64_t digest_text(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const char ch : text) {
    hash ^= static_cast<std::uint8_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string sanitize(const char* text) {
  if (text == nullptr) {
    return std::string();
  }
  std::string out;
  for (std::size_t index = 0; index < 256 && text[index] != '\0'; ++index) {
    const char ch = text[index];
    const bool printable = ch >= 0x20 && ch != 0x7F;
    if (printable) {
      out.push_back(ch);
    }
  }
  return out;
}

}  // namespace

struct NvmlBackend::Impl {
#if defined(_WIN32)
  HMODULE library{nullptr};
#else
  void* library{nullptr};
#endif
  bool initialized{false};
  std::string library_path;
  std::string probe;
  std::vector<std::string> notes;

  InitFn init{nullptr};
  ShutdownFn shutdown{nullptr};
  ErrorStringFn error_string{nullptr};
  DeviceGetCountFn device_count{nullptr};
  DeviceGetHandleFn device_handle{nullptr};
  DeviceGetNameFn device_name{nullptr};
  DeviceGetUuidFn device_uuid{nullptr};
  DeviceGetMemoryInfoFn memory_info{nullptr};
  DeviceGetCudaComputeCapabilityFn compute_capability{nullptr};
  DeviceGetMigModeFn mig_mode{nullptr};
  DeviceGetMaxMigDeviceCountFn max_mig_devices{nullptr};
  DeviceGetMigDeviceHandleByIndexFn mig_device_handle{nullptr};
  SystemGetDriverVersionFn driver_version{nullptr};
  DeviceGetVbiosVersionFn vbios_version{nullptr};
  DeviceGetPciInfoFn pci_info{nullptr};
  DeviceGetSerialFn serial{nullptr};

  std::mutex mutex;
  std::map<std::string, unsigned int> key_index;

  std::string last_error(NvmlReturn code) const {
    if (error_string != nullptr) {
      return sanitize(error_string(code));
    }
    return std::to_string(code);
  }

  template <class Fn>
  bool resolve(Fn& target, const char* name) {
#if defined(_WIN32)
    target = reinterpret_cast<Fn>(GetProcAddress(library, name));
#else
    target = reinterpret_cast<Fn>(dlsym(library, name));
#endif
    return target != nullptr;
  }

  ~Impl() {
    if (initialized && shutdown != nullptr) {
      (void)shutdown();
    }
#if defined(_WIN32)
    if (library != nullptr) {
      FreeLibrary(library);
    }
#else
    if (library != nullptr) {
      dlclose(library);
    }
#endif
  }
};

NvmlBackend::NvmlBackend() : impl_(std::make_unique<Impl>()) {}

NvmlBackend::~NvmlBackend() = default;

bool NvmlBackend::loaded() const noexcept { return impl_->initialized; }

std::string NvmlBackend::library_version() const {
  if (impl_->driver_version == nullptr) {
    return std::string();
  }
  std::array<char, 96> buffer{};
  if (impl_->driver_version(buffer.data(), static_cast<unsigned int>(buffer.size())) !=
      kNvmlSuccess) {
    return std::string();
  }
  return sanitize(buffer.data());
}

std::string NvmlBackend::probe_report() const { return impl_->probe; }

Result<std::unique_ptr<NvmlBackend>> NvmlBackend::create() {
  auto backend = std::unique_ptr<NvmlBackend>(new NvmlBackend());
  Impl& impl = *backend->impl_;
#if defined(_WIN32)
  impl.library_path = "nvml.dll";
  impl.library = LoadLibraryW(L"nvml.dll");
  if (impl.library == nullptr) {
    impl.library_path = "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll";
    impl.library = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
  }
#else
  impl.library_path = "libnvidia-ml.so.1";
  impl.library = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
  if (impl.library == nullptr) {
    return make_error(ErrorCode::UnsupportedCapability,
                      "no NVIDIA management library is reachable on this host");
  }
  const bool have_init = impl.resolve(impl.init, "nvmlInit_v2") ||
                         impl.resolve(impl.init, "nvmlInit");
  (void)impl.resolve(impl.shutdown, "nvmlShutdown");
  (void)impl.resolve(impl.error_string, "nvmlErrorString");
  const bool have_count = impl.resolve(impl.device_count, "nvmlDeviceGetCount_v2") ||
                          impl.resolve(impl.device_count, "nvmlDeviceGetCount");
  const bool have_handle = impl.resolve(impl.device_handle, "nvmlDeviceGetHandleByIndex_v2") ||
                           impl.resolve(impl.device_handle, "nvmlDeviceGetHandleByIndex");
  (void)impl.resolve(impl.device_name, "nvmlDeviceGetName");
  (void)impl.resolve(impl.device_uuid, "nvmlDeviceGetUUID");
  (void)impl.resolve(impl.memory_info, "nvmlDeviceGetMemoryInfo");
  (void)impl.resolve(impl.compute_capability, "nvmlDeviceGetCudaComputeCapability");
  (void)impl.resolve(impl.mig_mode, "nvmlDeviceGetMigMode");
  (void)impl.resolve(impl.max_mig_devices, "nvmlDeviceGetMaxMigDeviceCount");
  (void)impl.resolve(impl.mig_device_handle, "nvmlDeviceGetMigDeviceHandleByIndex");
  (void)impl.resolve(impl.driver_version, "nvmlSystemGetDriverVersion");
  (void)impl.resolve(impl.vbios_version, "nvmlDeviceGetVbiosVersion");
  (void)impl.resolve(impl.pci_info, "nvmlDeviceGetPciInfo_v3");
  if (impl.pci_info == nullptr) {
    (void)impl.resolve(impl.pci_info, "nvmlDeviceGetPciInfo");
  }
  (void)impl.resolve(impl.serial, "nvmlDeviceGetSerial");
  if (!have_init || !have_count || !have_handle) {
    return make_error(ErrorCode::UnsupportedCapability,
                      "the NVIDIA management library is present but lacks the required entry "
                      "points");
  }
  const NvmlReturn initialized = impl.init();
  if (initialized != kNvmlSuccess) {
    return make_error(ErrorCode::UnsupportedCapability,
                      "the NVIDIA management library could not be initialised",
                      impl.last_error(initialized));
  }
  impl.initialized = true;
  impl.probe += "library=" + impl.library_path + "\n";
  impl.probe += "nvmlInit=ok\n";
  if (impl.driver_version != nullptr) {
    impl.probe += "driver=" + backend->library_version() + "\n";
  }
  impl.probe += std::string("nvmlDeviceGetMigMode=") +
                (impl.mig_mode != nullptr ? "resolved" : "unavailable") + "\n";
  impl.probe += std::string("nvmlDeviceGetMaxMigDeviceCount=") +
                (impl.max_mig_devices != nullptr ? "resolved" : "unavailable") + "\n";
  impl.probe += std::string("nvmlDeviceGetMigDeviceHandleByIndex=") +
                (impl.mig_device_handle != nullptr ? "resolved" : "unavailable") + "\n";
  return backend;
}

Result<std::vector<BackendAccelerator>> NvmlBackend::discover() {
  Impl& impl = *impl_;
  if (!impl.initialized) {
    return make_error(ErrorCode::UnsupportedCapability, "NVML backend is not initialised");
  }
  unsigned int count = 0;
  const NvmlReturn counted = impl.device_count(&count);
  if (counted != kNvmlSuccess) {
    return make_error(ErrorCode::BackendFailure, "nvmlDeviceGetCount failed",
                      impl.last_error(counted));
  }
  const std::uint64_t now = system_now_ms();
  std::vector<BackendAccelerator> out;
  std::string probe;
  for (unsigned int index = 0; index < count; ++index) {
    NvmlDevice handle = nullptr;
    const NvmlReturn fetched = impl.device_handle(index, &handle);
    if (fetched != kNvmlSuccess || handle == nullptr) {
      probe += "device " + std::to_string(index) + ": handle query failed\n";
      continue;
    }
    BackendAccelerator accelerator;
    accelerator.backend = "nvidia-nvml";
    accelerator.provenance = EvidenceProvenance::Real;
    std::array<char, 96> buffer{};
    if (impl.device_name != nullptr &&
        impl.device_name(handle, buffer.data(), static_cast<unsigned int>(buffer.size())) ==
            kNvmlSuccess) {
      accelerator.identifiers.model = sanitize(buffer.data());
    }
    buffer.fill('\0');
    if (impl.device_uuid != nullptr &&
        impl.device_uuid(handle, buffer.data(), static_cast<unsigned int>(buffer.size())) ==
            kNvmlSuccess) {
      accelerator.identifiers.uuid = sanitize(buffer.data());
    }
    buffer.fill('\0');
    if (impl.vbios_version != nullptr &&
        impl.vbios_version(handle, buffer.data(), static_cast<unsigned int>(buffer.size())) ==
            kNvmlSuccess) {
      accelerator.identifiers.firmware_version = sanitize(buffer.data());
    }
    buffer.fill('\0');
    if (impl.serial != nullptr &&
        impl.serial(handle, buffer.data(), static_cast<unsigned int>(buffer.size())) ==
            kNvmlSuccess) {
      accelerator.identifiers.serial = sanitize(buffer.data());
    }
    accelerator.identifiers.vendor = "NVIDIA";
    accelerator.identifiers.driver_version = library_version();
    NvmlPciInfo pci{};
    if (impl.pci_info != nullptr && impl.pci_info(handle, &pci) == kNvmlSuccess) {
      char bus[32];
      std::snprintf(bus, sizeof(bus), "%04x:%02x:%02x.0", pci.domain, pci.bus, pci.device);
      accelerator.identifiers.pci_bus_id = bus;
    }
    int major = 0;
    int minor = 0;
    if (impl.compute_capability != nullptr &&
        impl.compute_capability(handle, &major, &minor) == kNvmlSuccess) {
      accelerator.identifiers.compute_capability =
          std::to_string(major) + "." + std::to_string(minor);
    }
    NvmlMemory memory{};
    bool have_memory = false;
    if (impl.memory_info != nullptr && impl.memory_info(handle, &memory) == kNvmlSuccess) {
      have_memory = true;
      const Status set = accelerator.physical_totals.set(ResourceDimension::MemoryBytes,
                                                         memory.total);
      if (!set.ok()) {
        probe += "device " + std::to_string(index) + ": impossible memory total\n";
        continue;
      }
    }
    if (!have_memory) {
      probe += "device " + std::to_string(index) + ": memory query failed; device skipped\n";
      continue;
    }
    (void)accelerator.physical_totals.set(ResourceDimension::ComputeShare, kShareScale);

    accelerator.stable_key = accelerator.identifiers.uuid.empty()
                                 ? ("nvml-index-" + std::to_string(index))
                                 : accelerator.identifiers.uuid;
    accelerator.locality = LocalityDomain{};
    accelerator.locality.name = accelerator.identifiers.pci_bus_id;

    PartitionCapability& capability = accelerator.capability;
    capability.generation = CapabilityGeneration::first();
    capability.backend_name = "nvidia-nvml";
    capability.backend_version = library_version();
    capability.driver_version = accelerator.identifiers.driver_version;
    capability.exposed_dimensions = {ResourceDimension::ComputeShare,
                                     ResourceDimension::MemoryBytes};
    capability.evidence.observed = true;
    capability.evidence.generation = EvidenceGeneration::first();
    capability.evidence.provenance = EvidenceProvenance::Real;
    capability.evidence.observed_at_ms = now;
    capability.evidence.ttl_ms = 30'000;
    capability.evidence.source = "nvml";
    capability.minimum_allocation = accelerator.physical_totals;

    unsigned int mig_current = 0;
    unsigned int mig_pending = 0;
    bool mig_queryable = false;
    if (impl.mig_mode != nullptr) {
      const NvmlReturn queried = impl.mig_mode(handle, &mig_current, &mig_pending);
      mig_queryable = queried == kNvmlSuccess;
      probe += "device " + std::to_string(index) + " nvmlDeviceGetMigMode=" +
               (mig_queryable ? "supported" : impl.last_error(queried)) + "\n";
    } else {
      probe += "device " + std::to_string(index) + " nvmlDeviceGetMigMode=unavailable\n";
    }
    unsigned int max_mig = 0;
    if (mig_queryable && impl.max_mig_devices != nullptr) {
      if (impl.max_mig_devices(handle, &max_mig) == kNvmlSuccess) {
        probe += "device " + std::to_string(index) +
                 " nvmlDeviceGetMaxMigDeviceCount=" + std::to_string(max_mig) + "\n";
      } else {
        max_mig = 0;
      }
    }

    if (!mig_queryable) {
      // The device positively reported that the MIG query is not supported, so
      // physical partitioning is UNSUPPORTED rather than unknown.
      capability.support = PartitionSupportState::Unsupported;
      capability.mechanism = PartitionMechanism::None;
      capability.max_partition_count = 0;
      capability.unsupported_reason =
          "nvmlDeviceGetMigMode reports no partition mechanism on this device";
      accelerator.unsupported_reason = capability.unsupported_reason;
      capability.total_compute_slices = 0;
      capability.total_memory_slices = 0;
    } else {
      // MIG is a real mechanism on this device, but this backend never mutates
      // partitions. It publishes the geometry it can actually observe.
      capability.support = PartitionSupportState::Unknown;
      capability.mechanism = PartitionMechanism::Mig;
      capability.mechanism_name = "mig";
      capability.max_partition_count = max_mig;
      capability.requires_reset_for_reconfiguration = true;
      capability.live_reconfiguration_supported = false;
      capability.tooling_requirement =
          "partition creation is UNSUPPORTED through this backend; use an administrative MIG tool "
          "outside the fabric";
      capability.unsupported_reason =
          "the device exposes a partition mechanism but this backend publishes no profile geometry "
          "and performs no partition mutation";
      accelerator.unsupported_reason = capability.unsupported_reason;
    }

    std::string material = accelerator.identifiers.uuid + "|" +
                           accelerator.identifiers.driver_version + "|" +
                           accelerator.identifiers.firmware_version + "|" +
                           std::to_string(mig_current) + "|" + std::to_string(mig_pending);
    accelerator.boot_id = AcceleratorBootId::derive(digest_text(material));
    accelerator.evidence = capability.evidence;
    {
      std::lock_guard<std::mutex> lock(impl.mutex);
      impl.key_index[accelerator.stable_key] = index;
    }
    out.push_back(std::move(accelerator));
  }
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    impl.probe += probe;
  }
  return out;
}

Result<BackendLayout> NvmlBackend::query_layout(std::string_view stable_key) {
  Result<std::vector<BackendAccelerator>> devices = discover();
  if (!devices.ok()) {
    return devices.error();
  }
  for (const BackendAccelerator& device : devices.value()) {
    if (device.stable_key != stable_key) {
      continue;
    }
    BackendLayout layout;
    layout.stable_key = device.stable_key;
    layout.device_present = true;
    layout.boot_id = device.boot_id;
    layout.physical_totals = device.physical_totals;
    layout.evidence = device.evidence;
    if (device.capability.mechanism == PartitionMechanism::Mig &&
        impl_->mig_device_handle != nullptr && impl_->max_mig_devices != nullptr) {
      unsigned int index = 0;
      NvmlDevice parent = nullptr;
      const auto found = impl_->key_index.find(device.stable_key);
      if (found != impl_->key_index.end() &&
          impl_->device_handle(found->second, &parent) == kNvmlSuccess) {
        unsigned int max_mig = 0;
        if (impl_->max_mig_devices(parent, &max_mig) == kNvmlSuccess) {
          for (; index < max_mig; ++index) {
            NvmlDevice mig = nullptr;
            if (impl_->mig_device_handle(parent, index, &mig) != kNvmlSuccess) {
              continue;
            }
            BackendNativePartition native;
            std::array<char, 96> uuid{};
            if (impl_->device_uuid != nullptr &&
                impl_->device_uuid(mig, uuid.data(), static_cast<unsigned int>(uuid.size())) ==
                    kNvmlSuccess) {
              native.native_id = sanitize(uuid.data());
              native.instance_uuid = native.native_id;
            } else {
              native.native_id = device.stable_key + "/mig/" + std::to_string(index);
            }
            NvmlMemory memory{};
            if (impl_->memory_info != nullptr &&
                impl_->memory_info(mig, &memory) == kNvmlSuccess) {
              (void)native.resources.set(ResourceDimension::MemoryBytes, memory.total);
            }
            if (native.resources.empty()) {
              continue;
            }
            native.live = true;
            layout.partitions.push_back(std::move(native));
          }
        }
      }
      layout.detail = layout.partitions.empty()
                          ? "MIG is exposed by the device but no configured instance was "
                            "observed"
                          : "observed MIG instances reported by NVML";
    } else {
      layout.detail = "device exposes no partition mechanism; no physical partitions exist";
    }
    return layout;
  }
  return make_error(ErrorCode::NotFound, "accelerator key is not known to this backend",
                    std::string(stable_key));
}

Result<BackendMutationResult> NvmlBackend::create_partitions(
    const PartitionMutationRequest& request) {
  return make_error(ErrorCode::UnsupportedCapability,
                    "physical partition creation is UNSUPPORTED through the NVML backend",
                    request.stable_key);
}

Result<BackendMutationResult> NvmlBackend::destroy_partitions(
    const PartitionMutationRequest& request) {
  return make_error(ErrorCode::UnsupportedCapability,
                    "physical partition destruction is UNSUPPORTED through the NVML backend",
                    request.stable_key);
}

Result<BackendMutationResult> NvmlBackend::reconfigure_layout(
    const PartitionMutationRequest& request) {
  return make_error(ErrorCode::UnsupportedCapability,
                    "physical device reconfiguration is UNSUPPORTED through the NVML backend",
                    request.stable_key);
}

Result<bool> NvmlBackend::validate_partition(std::string_view stable_key,
                                             std::string_view native_id) {
  Result<BackendLayout> layout = query_layout(stable_key);
  if (!layout.ok()) {
    return layout.error();
  }
  for (const BackendNativePartition& partition : layout.value().partitions) {
    if (partition.native_id == native_id) {
      return true;
    }
  }
  return false;
}

}  // namespace apf
