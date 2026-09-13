#include "fabric_internal.hpp"

#include <algorithm>

namespace apf {

AdmissionProbe evaluate_admission(const FabricState& state, const WorkloadRequirement& requirement,
                                  std::uint64_t now_ms) {
  AdmissionProbe probe;
  if (!requirement.validate(state.limits).ok()) {
    probe.outcome = AdmissionOutcome::RejectedInvalidRequest;
    probe.explanation.add(ExplanationCode::CandidateRejected, "requirement",
                          to_string(requirement.validate(state.limits).error()));
    probe.explanation.set_summary("workload requirement is not a valid admission request");
    return probe;
  }

  const std::uint64_t freshness_window = requirement.required_freshness_ms != 0
                                             ? requirement.required_freshness_ms
                                             : state.policy.max_evidence_age_ms;

  AdmissionOutcome deferred = AdmissionOutcome::RejectedNoCandidatePartition;
  bool saw_draining = false;
  bool saw_revalidation = false;
  bool saw_stale = false;
  bool saw_isolation = false;
  bool saw_resources = false;
  bool saw_profile = false;
  bool saw_exclusive = false;
  bool saw_capability = false;
  bool saw_device = false;

  for (const auto& entry : state.partitions) {
    const PartitionRecord& record = entry.second;
    const AcceleratorRecord* accelerator = state.find_accelerator(record.accelerator);
    if (accelerator == nullptr) {
      saw_device = true;
      continue;
    }
    if (record.state == PartitionState::Draining ||
        record.drain.state == DrainState::Draining) {
      saw_draining = true;
      continue;
    }
    if (record.state == PartitionState::RevalidationRequired ||
        record.state == PartitionState::Offline || record.state == PartitionState::Degraded) {
      saw_revalidation = true;
      continue;
    }
    if (record.state != PartitionState::Active) {
      continue;
    }
    ++probe.candidates_considered;

    if (!requirement.backend.empty() && requirement.backend != accelerator->backend) {
      saw_capability = true;
      continue;
    }
    if (!requirement.device_family.empty() &&
        requirement.device_family != accelerator->identifiers.model) {
      saw_capability = true;
      continue;
    }
    if (!requirement.locality_domain.empty() &&
        requirement.locality_domain != accelerator->locality.name) {
      saw_capability = true;
      continue;
    }
    if (requirement.required_profile.valid() && record.profile != requirement.required_profile) {
      saw_profile = true;
      continue;
    }
    if (!requirement.minimum_resources.empty() &&
        !requirement.minimum_resources.is_subset_of(record.resources)) {
      saw_resources = true;
      continue;
    }
    if (!requirement.isolation.required.is_subset_of(record.isolation.guarantees)) {
      saw_isolation = true;
      continue;
    }
    if (!accelerator->capability.declares_supported()) {
      saw_capability = true;
      continue;
    }
    if (requirement.exclusive) {
      bool occupied = false;
      for (const auto& assignment_entry : state.assignments) {
        if (assignment_entry.second.partition == record.id) {
          occupied = true;
          break;
        }
      }
      if (occupied) {
        saw_exclusive = true;
        continue;
      }
    }
    const bool fresh = now_ms <= record.evidence.observed_at_ms ||
                       (now_ms - record.evidence.observed_at_ms) <=
                           (freshness_window == 0 ? record.evidence.ttl_ms : freshness_window);
    if (!record.evidence.is_observed() || !fresh) {
      saw_stale = true;
      continue;
    }

    probe.outcome = AdmissionOutcome::Admitted;
    probe.partition = record.id;
    probe.profile = record.profile;
    probe.resources = record.resources;
    probe.explanation.add(ExplanationCode::CandidateChosen, record.id.str(),
                          "partition authority is current and satisfies the requirement");
    probe.explanation.set_summary("workload may be admitted to partition " + record.id.str());
    probe.explanation.canonicalize(state.limits.max_explanations);
    return probe;
  }

  if (saw_draining) {
    deferred = AdmissionOutcome::RejectedDraining;
  } else if (saw_revalidation) {
    deferred = AdmissionOutcome::RejectedRevalidationRequired;
  } else if (saw_stale) {
    deferred = AdmissionOutcome::RejectedStaleAuthority;
  } else if (saw_isolation) {
    deferred = AdmissionOutcome::RejectedIsolation;
  } else if (saw_resources) {
    deferred = AdmissionOutcome::RejectedInsufficientResources;
  } else if (saw_exclusive) {
    deferred = AdmissionOutcome::RejectedExclusiveConflict;
  } else if (saw_profile) {
    deferred = AdmissionOutcome::RejectedProfile;
  } else if (saw_capability) {
    deferred = AdmissionOutcome::RejectedCapability;
  } else if (saw_device) {
    deferred = AdmissionOutcome::RejectedDeviceUnavailable;
  }
  probe.outcome = deferred;
  probe.explanation.add(ExplanationCode::CandidateRejected, "requirement",
                        to_string(probe.outcome));
  probe.explanation.set_summary(std::string("admission rejected: ") + to_string(probe.outcome));
  probe.explanation.canonicalize(state.limits.max_explanations);
  return probe;
}

}  // namespace apf
