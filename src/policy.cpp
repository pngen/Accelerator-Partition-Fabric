#include "apf/policy.hpp"

#include <algorithm>

namespace apf {

Status PlanningPolicy::validate() const {
  if (name.empty()) {
    return failure(ErrorCode::InvalidArgument, "policy must be named");
  }
  if (max_partition_mutations_per_plan == 0) {
    return failure(ErrorCode::InvalidArgument,
                   "policy must allow at least one partition mutation per plan");
  }
  if (require_fresh_evidence && max_evidence_age_ms == 0) {
    return failure(ErrorCode::InvalidArgument,
                   "policy requires fresh evidence but sets no freshness window");
  }
  if (weights.destructive_reconfiguration < 0) {
    return failure(ErrorCode::InvalidArgument,
                   "destructive reconfiguration weight must not be negative");
  }
  if (weights.fragmentation < 0 || weights.capacity_waste < 0 || weights.mutation_count < 0 ||
      weights.health < 0 || weights.future_optionality < 0) {
    return failure(ErrorCode::InvalidArgument, "policy weights must not be negative");
  }
  if (protected_dimensions.size() > kResourceDimensionCount) {
    return failure(ErrorCode::LimitExceeded, "policy protects too many resource dimensions");
  }
  for (std::size_t index = 0; index < protected_dimensions.size(); ++index) {
    for (std::size_t other = index + 1; other < protected_dimensions.size(); ++other) {
      if (protected_dimensions[index] == protected_dimensions[other]) {
        return failure(ErrorCode::AlreadyExists,
                       "policy lists a protected dimension more than once");
      }
    }
  }
  if (allow_degraded_device && require_healthy_device) {
    return failure(ErrorCode::InvalidArgument,
                   "policy both requires a healthy device and allows a degraded one");
  }
  return success();
}

PlanningPolicy make_default_policy(PolicyGeneration generation) {
  PlanningPolicy policy;
  policy.generation = generation;
  policy.name = "default";
  policy.weights = PlanningWeights{};
  policy.allow_destructive_reconfiguration = false;
  policy.allow_drain = true;
  policy.max_reconfiguration_downtime_ms = 0;
  policy.require_fresh_evidence = true;
  policy.max_evidence_age_ms = 30'000;
  policy.require_healthy_device = false;
  policy.allow_degraded_device = false;
  policy.allow_external_adoption = false;
  policy.exclusive_by_default = false;
  policy.max_partition_mutations_per_plan = 4;
  policy.prefer_live_reconfiguration = true;
  return policy;
}

}  // namespace apf
