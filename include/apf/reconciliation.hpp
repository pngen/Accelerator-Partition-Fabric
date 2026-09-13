#pragma once

#include "apf/explain.hpp"
#include "apf/id.hpp"
#include "apf/partition.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apf {

/// What reconciliation discovered when durable metadata was compared against
/// observed hardware.
enum class ReconciliationFindingKind : std::uint8_t {
  PartitionMatches = 0,
  PartitionMissing = 1,
  PartitionUnexpected = 2,
  ProfileChanged = 3,
  NativeIdentityChanged = 4,
  DeviceReset = 5,
  CapabilityChanged = 6,
  DeviceGone = 7,
  DeviceNotPartitionCapable = 8,
  GenerationUncorrelatable = 9,
  AssignmentOrphaned = 10,
  LayoutMatches = 11,
  LedgerMismatch = 12,
  LayoutChangedExternally = 13,
  Count = 14,
};

const char* to_string(ReconciliationFindingKind kind) noexcept;

enum class ReconciliationSeverity : std::uint8_t {
  Informational = 0,
  Attention = 1,
  Material = 2,
  Critical = 3,
  Count = 4,
};

const char* to_string(ReconciliationSeverity severity) noexcept;

struct ReconciliationFinding {
  ReconciliationFindingKind kind{ReconciliationFindingKind::PartitionMatches};
  ReconciliationSeverity severity{ReconciliationSeverity::Informational};
  AcceleratorId accelerator{};
  PartitionId partition{};
  PartitionGeneration expected_generation{};
  PartitionState resulting_state{PartitionState::Discovered};
  std::string native_id;
  std::string detail;
};

/// The conservative outcome of a reconciliation pass. Unexplained physical
/// state is recorded, never silently deleted and never silently adopted.
struct ReconciliationReport {
  ReconciliationId id{};
  AcceleratorId accelerator{};
  AcceleratorGeneration accelerator_generation{};
  CapabilityGeneration capability_generation{};
  bool device_present{false};
  bool device_generation_changed{false};
  bool capability_changed{false};
  bool layout_changed{false};
  bool revalidation_required{false};
  bool reconciliation_required{false};
  std::uint32_t matched{0};
  std::uint32_t missing{0};
  std::uint32_t unexpected{0};
  std::uint32_t adopted{0};
  std::uint32_t fenced{0};
  std::vector<ReconciliationFinding> findings;
  Explanation explanation{};

  std::string summary() const;
};

}  // namespace apf
