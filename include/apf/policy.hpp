#pragma once

#include "apf/id.hpp"
#include "apf/isolation.hpp"
#include "apf/limits.hpp"
#include "apf/resource.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apf {

/// Integer weights for the deterministic ranking stage. Weights are applied
/// only after every hard constraint has been evaluated, so an ineligible
/// device can never win by scoring well.
struct PlanningWeights {
  std::int64_t fragmentation{100};
  std::int64_t capacity_waste{10};
  std::int64_t destructive_reconfiguration{1000};
  std::int64_t drain{200};
  std::int64_t mutation_count{50};
  std::int64_t health{500};
  std::int64_t locality{25};
  std::int64_t future_optionality{75};
  std::int64_t contiguity_preservation{40};
  std::int64_t reconfiguration_cost{100};
  std::int64_t live_reconfiguration_bonus{-50};
};

/// Governance policy. The tradeoff between fragmentation, disruption and
/// optionality is policy-defined rather than hardcoded. The shipped default is
/// deterministic and documented in the README.
struct PlanningPolicy {
  PolicyGeneration generation{};
  std::string name{"default"};
  PlanningWeights weights{};
  bool allow_destructive_reconfiguration{false};
  bool allow_drain{true};
  /// 0 means unbounded; only enforced against a supplied downtime estimate.
  std::uint64_t max_reconfiguration_downtime_ms{0};
  bool require_fresh_evidence{true};
  std::uint64_t max_evidence_age_ms{30'000};
  bool require_healthy_device{false};
  bool allow_degraded_device{false};
  /// Reconciliation policy: whether externally observed partitions may ever be
  /// adopted as authoritative without going through the mutation path.
  bool allow_external_adoption{false};
  bool exclusive_by_default{false};
  /// Maximum number of physical mutations a single plan may schedule.
  std::uint32_t max_partition_mutations_per_plan{4};
  bool prefer_live_reconfiguration{true};
  IsolationRequirement minimum_isolation{};
  std::vector<ResourceDimension> protected_dimensions;
  std::uint32_t max_instances_per_profile{0};

  Status validate() const;
};

/// The shipped default policy. Deterministic and documented.
PlanningPolicy make_default_policy(PolicyGeneration generation);

}  // namespace apf
