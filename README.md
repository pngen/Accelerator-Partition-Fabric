# Accelerator Partition Fabric

A vendor-neutral C++20 runtime for governing **accelerator partition lifecycle,
capacity, placement, admission, isolation, fragmentation, reconfiguration and
generation-bound authority** across heterogeneous AI infrastructure.

The runtime treats accelerator partitioning as governed infrastructure rather
than static configuration. It decides *which logical accelerator partitions may
exist now, how much resource capacity each partition owns, which workloads may be
admitted to them, how the physical device may be repartitioned safely, and which
partition state remains authoritative under current device, worker, coordinator,
policy, capability, topology, health and generation evidence.*

## The systems question

> Given a physical accelerator and its current partitioning capabilities, which
> logical accelerator partitions may exist now, how much resource capacity each
> partition owns, which workloads may be admitted to them, how may the physical
> device be repartitioned safely, and which partition state remains
> authoritative under current device, worker, coordinator, policy, capability,
> topology, health and generation evidence?

The runtime makes one distinction mechanically enforceable throughout:

**A partition configuration was once observed or requested.**

is not

**This exact physical accelerator generation currently authorizes this exact
partition generation under current capability, policy, topology, isolation,
resource, worker and coordinator evidence.**

A physical accelerator being visible does not make an arbitrary partition
configuration legal. A partition profile being theoretically supported does not
make it currently realizable. Free capacity does not mean it can be assembled
into the requested shape. A partition record surviving restart does not make the
physical partition still present. A successful driver call does not make stale
control-plane authority current.

## Boundary

**Accelerator Partition Fabric owns:** physical accelerator identity relevant to
partition governance; partition-capable accelerator registration; partition
capability publication; partition profiles and resource geometry; partition
identity and generation; physical-device generation; device incarnation
evidence where discoverable; partition lifecycle; partition planning; partition
creation, destruction and reconfiguration authority; resource accounting;
capacity feasibility; internal accelerator fragmentation; admission against
partition resource requirements; deterministic partition placement; isolation
metadata and policy; reconfiguration impact analysis; drain semantics before
destructive repartitioning; partition reservation; workload-to-partition
assignment authority at the partition boundary; partition evidence freshness;
stale partition rejection; runtime snapshots; persistence of durable structural
state; conservative recovery; coordinator/worker authority in distributed
deployments; deterministic explanations for every material decision.

**Accelerator Partition Fabric does not own:** generic cluster scheduling; rack
or cluster topology governance; full GPU fleet management; generic device-health
monitoring; model routing; inference scheduling; agent scheduling; CUDA kernel
scheduling; general accelerator memory allocation inside a partition; GPU Memory
Service semantics; generic NUMA or PCIe placement; generic resource brokering
across unrelated resource classes; VM or container orchestration; device-driver
implementation; vendor-specific hardware configuration outside the narrow
backend adapter; general accelerator virtualization beyond the partition
boundary; cross-node capacity planning; fabric/network routing; workload
execution itself.

Adjacent systems integrate through the explicit interfaces in
`include/apf/`: a backend adapter (`AcceleratorBackend`) for vendor operations,
an evidence interface (`BackendAccelerator`, `BackendLayout`) for physical
truth, admission requests for schedulers, and the framed protocol for
distributed deployments.

## Architecture

| Layer | Headers | Responsibility |
| --- | --- | --- |
| Value types | `id.hpp`, `resource.hpp`, `isolation.hpp`, `evidence.hpp`, `limits.hpp`, `result.hpp` | Strong identities, generations, sparse resource vectors, typed errors |
| Model | `profile.hpp`, `capability.hpp`, `accelerator.hpp`, `partition.hpp`, `lifecycle.hpp`, `policy.hpp` | What a device is, what it can do, and what a partition is |
| Decisions | `request.hpp`, `plan.hpp`, `fragmentation.hpp`, `accounting.hpp`, `explain.hpp` | Requests, plans, fragmentation reports, exact accounting, explanations |
| Authority | `reservation.hpp`, `reconciliation.hpp`, `snapshot.hpp` | Reservations, attempts, reconciliation findings, immutable snapshots |
| Runtime | `fabric.hpp` | `PartitionFabric`: the single governed entry point |
| Hardware | `backend.hpp`, `backend_synthetic.hpp`, `backend_nvml.hpp` | The narrow vendor boundary and its two implementations |
| Durable state | `persistence.hpp`, `codec.hpp` | Versioned, integrity-checked, atomically replaced state |
| Distributed | `transport.hpp`, `protocol.hpp`, `coordinator.hpp`, `worker.hpp`, `client.hpp` | Framed TCP control plane, coordinator, worker incarnations, client |
| Platform | `process.hpp` | Process and filesystem primitives used by tools and tests |

Governance logic is portable C++20 with no vendor dependency. Vendor operations
live behind `AcceleratorBackend`, which exposes discover, capability query,
layout query, create, destroy, reconfigure, validate and downtime estimation -
and nothing else. There is no path from the public API to arbitrary vendor
command execution.

## The accelerator and capability model

A `AcceleratorRecord` describes one physical accelerator: stable identity,
vendor, backend, model, hardware/firmware/driver identifiers where reliably
discoverable, physical resource totals, partition support state, partition
mechanism, supported profiles, maximum concurrent partitions, isolation
capabilities, reconfiguration capabilities, whether a destructive reset is
required, locality, health evidence consumed from an external provider,
capability generation, device generation and evidence provenance.

A `PartitionCapability` is a generation-bound publication of what the device can
do *right now*: support state (`Unknown`, `Supported`, `Unsupported`,
`Unavailable`), mechanism, supported profiles, profile combination rules, maximum
partition count, reset and live-reconfiguration semantics, isolation properties,
exposed resource dimensions, minimum allocation, slice geometry, backend and
driver requirements, and the evidence stamp that backs the claim. UNKNOWN never
becomes SUPPORTED: absence of evidence is not capability, and a capability
publication that materially changes advances the generation and invalidates every
plan and reservation that depended on it.

Mechanisms are never treated as equivalent. MIG, SR-IOV-like accelerator
partitioning, mediated devices and vendor slices have different authority,
isolation and reconfiguration semantics; `PartitionMechanism` names them
individually and the generic layer refuses to claim a physical mechanism from
synthetic evidence.

## Profile geometry and resource dimensions

`PartitionProfile` describes a partition shape explicitly: required compute and
memory slice counts, engine grouping, alignment and contiguity requirements,
resource vector, maximum multiplicity, mutual exclusions, whether a full-device
reconfiguration is required, whether live repartitioning is supported, and
backend requirements. A vendor-native identifier (for example a MIG profile
string) is preserved as opaque metadata and never interpreted by generic logic.

Capacity dimensions are sparse and per-dimension: compute share, memory bytes,
memory bandwidth share, execution-engine share, copy-engine share, media-engine
share, cache share and DMA share. A dimension is only ever published by a backend
that can make a meaningful claim about it; absent dimensions stay absent rather
than defaulting to zero, and incompatible dimensions are never summed together.
Shares are parts per million integers, so every decision is exact and
reproducible.

## Authority: identities, generations and evidence

`include/apf/id.hpp` defines strongly typed identities and generations as
distinct C++ types, so mixing them is a compile error:

`AcceleratorId`, `AcceleratorGeneration`, `AcceleratorBootId`, `PartitionId`,
`PartitionGeneration`, `PartitionProfileId`, `PartitionProfileGeneration`,
`PartitionPlanId`, `PartitionPlanGeneration`, `PartitionReservationId`,
`PartitionAssignmentId`, `PartitionAttemptId`, `CapabilityGeneration`,
`TopologyGeneration`, `PolicyGeneration`, `IsolationPolicyId`,
`IsolationPolicyGeneration`, `WorkerId`, `WorkerBootId`, `CoordinatorEpoch`,
`EvidenceGeneration`, `SnapshotGeneration`, `StateGeneration`.

Each one corresponds to an actual stale-state, lifecycle, compatibility or
authority boundary. Generation zero means "no generation has ever been
established", which is materially different from generation one, and generation
increments are overflow-checked because a wrapped generation would resurrect
fenced authority.

Every hardware-facing statement carries an `EvidenceStamp`: generation,
provenance, observation time, time-to-live, source and freshness. A plan binds
the accelerator generation, accelerator boot identity, capability generation,
topology generation, policy generation, coordinator epoch, state generation,
evidence generation and the exact partition layout it was derived from. Any
relevant change makes the plan stale, and a stale destructive plan is never
executed.

## REAL, SYNTHETIC and UNSUPPORTED

Every hardware-facing capability and proof is classified:

* **REAL** - observed from real hardware through a real backend. On the
  validation machine this is physical accelerator identity, driver/firmware
  version, PCI bus identity, compute capability and physical memory totals from
  NVML.
* **UNSUPPORTED** - the backend positively determined that the capability does
  not exist. On the validation machine this is physical partition creation: the
  installed GeForce RTX 5090 does not expose a partition mechanism, so the NVML
  backend publishes `Unsupported` with the reason reported by the driver.
* **SYNTHETIC** - produced by the deterministic simulator. All multi-partition
  geometry, fragmentation, destructive reconfiguration and process-death
  evidence in this repository is SYNTHETIC and is labelled as such everywhere it
  appears.

No vendor family name is used to infer capability, and no synthetic result is
ever called physical NVIDIA MIG validation.

## Partition lifecycle

Lifecycle is explicit; booleans are never used to express it:

`DISCOVERED`, `UNPARTITIONED`, `PLAN_PENDING`, `RESERVED`, `CREATING`,
`ACTIVE`, `DRAINING`, `RECONFIGURATION_REQUIRED`, `DESTROYING`,
`REVALIDATION_REQUIRED`, `DEGRADED`, `OFFLINE`, `RETIRED`, `FAILED`.

Every transition has explicit preconditions in a fixed transition table. Illegal
transitions fail deterministically with `invalid_transition`; repeated
idempotent transitions are safe for stable states and deliberately rejected for
transient ones. `RETIRED` is terminal: no transition out of it exists, so a
stale request cannot resurrect retired authority. Each partition record names the
exact ledger bucket that holds its capacity, which makes the accounting identity
auditable per partition instead of inferred.

## Exact capacity accounting

For every governed dimension the ledger maintains

```
physical total = unavailable + free + reserved + active + draining + reconfiguration-held
```

Every mutation is applied to a copy and swapped in only when the whole
transaction succeeded, so accounting always returns to a valid state. Overflow
and underflow are rejected and checked; double reservation, double release,
release from a superseded partition generation, commit using a stale reservation
and requests larger than physical or profile-supported limits are all rejected
before any resource mutation happens. The invariant is verified after every
lifecycle transition and exposed through `verify_accounting()`.

## Fragmentation

Fragmentation is a primary responsibility, not "free bytes remain". The analysis
distinguishes profile-shape fragmentation, slice non-contiguity, engine-group
fragmentation, memory-segment constraints, maximum-partition-count exhaustion,
incompatible profile combinations, stranded capacity, capacity trapped behind
active partitions, and geometry that only a destructive reconfiguration could
recover. It reports the free geometry, stranded capacity, capacity trapped behind
active partitions, capacity recoverable by reconfiguration, the partitions that
would have to drain, and the backend-published disruption estimates (a zero
estimate means the backend published none; it is never replaced by a fabricated
number).

A request resolves to a materially distinct outcome rather than one generic
failure: `FEASIBLE_NOW`, `FEASIBLE_AFTER_DRAIN`, `FEASIBLE_AFTER_RECONFIGURATION`,
`PHYSICALLY_IMPOSSIBLE`, `CAPABILITY_UNSUPPORTED`, `CAPABILITY_UNKNOWN`,
`INSUFFICIENT_CAPACITY`, `FRAGMENTED`, `MAX_PARTITION_COUNT_REACHED`,
`ISOLATION_UNSATISFIED`, `POLICY_REJECTED`, `STALE_EVIDENCE`,
`REVALIDATION_REQUIRED`, `NO_ELIGIBLE_ACCELERATOR`, `LIMIT_EXCEEDED`,
`DUPLICATE_REQUEST`, `INVALID_REQUEST`.

## Planning

Planning is deterministic and evaluates hard constraints before any ranking: an
ineligible accelerator cannot win through score, a stale accelerator cannot win
through lower fragmentation, and a device whose destructive reconfiguration is
forbidden cannot win because it has more capacity. Candidate filtering covers
caller device selectors, backend/vendor/model/locality constraints, evidence
presence and freshness, health and readiness, capability support state, profile
support, backend agreement, isolation requirements and policy admission of drain
or destructive transitions.

Only eligible candidates are scored, using integer weights from the planning
policy: capacity waste, stranded remainder, destructive reconfiguration, drain,
mutation count, health, locality preference (consolidation onto a device already
hosting the same profile when the caller gives no locality constraint),
future optionality and reconfiguration cost, with a bonus for live
reconfiguration. Ties are broken by the lowest accelerator identity, and the
tie-break is recorded in the explanation. Every plan carries a canonical
explanation naming the decisive factors rather than an opaque scalar.

The shipped default policy is documented in `policy.hpp` and
`make_default_policy`: fresh evidence required, drain allowed, destructive
reconfiguration forbidden, external adoption forbidden, at most four partition
mutations per plan.

## Admission

Admission is the partition-side gate for one workload against one partition, not
a scheduler. A `WorkloadRequirement` expresses required profile, minimum memory
and compute share, isolation requirement, backend and device-family compatibility,
exclusivity, locality, reconfiguration and drain tolerance, freshness requirement
and policy class. Outcomes are explicit: `ADMITTED`,
`REJECTED_NO_CANDIDATE_PARTITION`, `REJECTED_INSUFFICIENT_RESOURCES`,
`REJECTED_ISOLATION`, `REJECTED_STALE_AUTHORITY`, `REJECTED_POLICY`,
`REJECTED_PROFILE`, `REJECTED_DRAINING`, `REJECTED_CAPABILITY`,
`REJECTED_EXCLUSIVE_CONFLICT`, `REJECTED_REVALIDATION_REQUIRED`,
`REJECTED_DEVICE_UNAVAILABLE`, `REJECTED_LIMIT`, `REJECTED_INVALID_REQUEST`.

A workload is admitted only to a partition whose authority is still valid: the
decision revalidates partition state, device generation, capability, isolation,
resources, exclusivity and assignment bounds immediately before binding, and a
change in between yields `REJECTED_STALE_AUTHORITY` rather than a stale binding.
`probe_admission()` evaluates without binding anything.

## Isolation

Isolation claims are conservative. The model distinguishes logical scheduling
separation, memory isolation, fault isolation, performance isolation, engine
isolation, address-space isolation, DMA isolation and tenant isolation. Only what
the backend and hardware actually provide is claimed: an accelerator partition
being represented as independent does not prove strong security isolation. Policy
can require isolation properties and rejects partitions that cannot demonstrate
them.

## Reservation and verified physical mutation

Capacity and partition claims use explicit reservation semantics with the
lifecycle `PENDING`, `ACTIVE`, `COMMITTED`, `RELEASED`, `ROLLED_BACK`,
`EXPIRED`, `FENCED`, `FAILED`. A reservation has stable identity and binds the
accelerator generation, accelerator boot identity, plan generation, capability
generation, topology generation, policy generation, coordinator epoch and - when
physical mutation is delegated - the worker identity and worker boot identity.
Capacity is held exactly once between reserve and release or commit; a release or
fence while a physical attempt may be in flight is refused with
`reconciliation_required` instead of handing out capacity a real partition may
already own, and a multi-partition reservation is all-or-nothing.

Creating, destroying or reconfiguring hardware partitions is an external side
effect and is treated as one:

1. All authority is revalidated: plan binding, plan expiry, accelerator and boot
   generation, capability generation, policy generation, freshness and drain
   obligations.
2. The pending attempt is **registered before dispatch**, so a fast completion
   cannot race state registration.
3. The mutation is dispatched through the narrow backend interface.
4. Physical state is **rediscovered or queried** and the observed result is
   compared with the requested geometry.
5. Only when the observation proves the requested geometry exists is
   authoritative logical state published, bound to fresh evidence.

A backend return value is never treated as proof that the desired physical state
now exists.

## Ambiguous completion

Physical partition mutation is not reducible to an ordinary RPC. The backend
outcome contract makes this explicit:

* a returned result with `accepted == false` means the backend positively
  determined that nothing was applied;
* a result with `accepted == true` and `ambiguous == false` means the backend
  applied the change and observed the result;
* an error return means the call did not complete and **whether anything was
  applied is unknown**.

An unknown outcome is recorded as `OUTCOME_UNKNOWN` with
`reconciliation_required`, the affected partitions move to
`REVALIDATION_REQUIRED`, and the runtime does not replay a destructive or
non-idempotent mutation. Reconciliation inspects current physical state and then
commits, repairs or rolls back exactly once: the attempt's own logical partition
identity adopts the physical partition it produced, or releases the capacity if
the intended result is provably absent. This behaviour is exercised in-process
and across real processes, including a worker that applies a mutation, checkpoints
the physical model and then dies before acknowledging it.

## Reconfiguration

Reconfiguration is a first-class transition with its own plan: current
configuration, desired configuration, partitions that must be drained,
partitions that may remain, workloads displaced, physical reset requirements,
device downtime, temporary loss of capacity, transition ordering, rollback
feasibility and post-change verification. The plan binds the exact state it was
derived from; if any relevant state changes before execution the plan is stale and
is refused with `stale_plan` or `stale_generation`.

Execution rebuilds the layout under a registered attempt, verifies the observed
geometry exactly, replaces the whole layout in one transactional accounting step,
advances and fences every superseded partition generation, removes assignments
that referred to those generations, publishes new generations bound to fresh
evidence, and closes accounting under the new layout.

## Drain

Drain is explicit and owned end to end by the partition side: `begin_drain`
rejects new assignments, tracks outstanding authoritative assignments, and
`complete_drain` succeeds only when none remain; otherwise it reports
`drain_blocked` with a stable blocker identifier. Destructive mutation requires a
completed, empty drain or a partition that provably holds nothing. This runtime
owns the partition-side drain gate; it does not claim to migrate workloads, and
the drain blocker states that plainly.

## Persistence and recovery

Durable state is a versioned container: magic, format version, byte-order marker,
declared payload length, header integrity hash and payload integrity hash over a
bounded, canonically ordered payload. Saving is
*write temporary -> flush -> verify -> replace authoritative file*. Loading
rejects truncated state, corrupt magic, unsupported versions, corrupt
checksums, oversized counts and lengths, duplicate identities, broken parent
relationships, impossible resource totals, invalid lifecycle or enum values,
generation regressions and invalid metadata - without partially applying
anything.

Persistence is not physical truth. Recovered dynamic hardware state never becomes
current again: affected partitions enter `REVALIDATION_REQUIRED`, in-flight
attempts are classified `OUTCOME_UNKNOWN`, live reservations are fenced, worker
incarnations are fenced, and the runtime requires fresh physical publication or
reconciliation before authority returns. Purely durable structure - which
accelerators exist, which profiles exist, which partitions were known - survives.

## State reconciliation

Reconciliation compares durable state with observed hardware and handles a
persisted partition that still exists exactly as expected, one that disappeared,
a partition that exists but was never known, a profile that changed externally, a
device reset, a capability change, a driver or backend change, a partition
generation that can no longer be correlated, a device that is no longer partition
capable, and assignments that refer to partitions that no longer exist.

Outcomes are conservative: a disappeared partition releases its capacity and is
retired with a finding; an unexplained physical partition is recorded, its
capacity is held conservatively, and it is *not* adopted as authoritative unless
policy explicitly allows external adoption; a device reset advances the device
generation, resets capability and forces revalidation. Unexplained physical state
is never silently deleted.

## Distributed deployment

`PartitionCoordinator` owns logical authority; `PartitionWorker` incarnations own
hardware and execute physical mutation. They communicate over real TCP sockets
with a bounded, versioned, integrity-checked framing protocol (magic `APF1`,
protocol version 1, fixed 56-byte header carrying type, flags, sequence,
coordinator epoch, worker identity, worker boot identity and a frame checksum over
header and payload). Declared lengths are validated against the configured bound
and the remaining bytes before anything is allocated; the message type is
validated before the payload is parsed.

Worker registration, capability publication, device and partition-state
publication, mutation request and acknowledgement, query and reconciliation
requests, fencing messages and state inspection are all part of the protocol.
A worker process that dies loses authority permanently for that boot identity:
the coordinator observes the connection loss through the control path, fences the
boot identity, marks its in-flight attempts `OUTCOME_UNKNOWN` and refuses further
traffic from that incarnation. A replacement worker receives a fresh boot identity
and must republish physical evidence before authority is usable again. A
coordinator restart advances the coordinator epoch, rejects old-epoch traffic and
requires revalidation rather than restoring live evidence.

## Hardware backends

**Synthetic backend** (`backend_synthetic.hpp`) is a deterministic accelerator
model with configurable physical capacity, profile ladders, slice geometry, legal
and illegal profile combinations, alignment and atomicity constraints, destructive
reconfiguration requirements, device reset, capability change, external mutation,
ambiguous completion, idempotency tokens and fault injection. It obeys exactly the
same public contracts as a real backend: it is registered, discovered, planned
against, mutated, verified and reconciled through the same interfaces, and tests
never reach around the production API.

**NVIDIA NVML backend** (`backend_nvml.hpp`) loads the NVIDIA management library
dynamically at run time - no vendor SDK is required to build the runtime - and
performs *read-only* real-hardware discovery: device identity, UUID, PCI bus
identity, compute capability, driver and VBIOS versions, physical memory totals
and partition-mechanism capability. It publishes REAL evidence for what it
observes and UNSUPPORTED for physical partition creation, which it never
performs. The MIG enumeration path is present but has never executed on the
validation hardware; nothing here should be read as validated MIG support.

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler, and (optionally) Ninja.
There are no third-party library dependencies; the core links only the platform
thread and socket libraries.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Options:

| Option | Default | Effect |
| --- | --- | --- |
| `APF_BUILD_TESTS` | ON | Build the test suites |
| `APF_BUILD_EXAMPLES` | ON | Build the examples |
| `APF_BUILD_TOOLS` | ON | Build `apfctl`, `apfcoord`, `apfworker` |
| `APF_BUILD_BENCHMARKS` | ON | Build `apf_bench` |
| `APF_ENABLE_WERROR` | ON | Treat first-party warnings as errors (`/W4 /WX` on MSVC) |
| `APF_ENABLE_ASAN` | OFF | Build with AddressSanitizer |
| `APF_BUILD_SHARED` | OFF | Build the runtime as a shared library |

## Tests

```sh
ctest --test-dir build --output-on-failure
# or run the suites directly
./build/tests/apf_tests
./build/tests/apf_mp_tests
```

Test commands run plainly. No timeouts are configured, and no test is hidden
behind forced termination: intentional process kill is used only where process
death is the scenario under test.

* `apf_tests` - 61 unit, property, concurrency and adversarial tests in one
  deterministic binary: identities and generations, lifecycle, accounting,
  profiles and capability, planning and fragmentation, reservation, verified
  execution, admission and drain, reconfiguration, reconciliation, persistence
  codecs, protocol codecs, snapshots, explanations, seeded property sequences,
  real multithreaded mutation, state-machine attacks, persistence corruption and
  truncation, protocol corruption against a live coordinator, and backend fault
  classification.
* `apf_mp_tests` - multiprocess proofs over real sockets and real processes:
  worker death, boot fencing, reincarnation, ambiguous completion reconciled
  exactly once, coordinator death and restart with epoch advance and conservative
  revalidation, and orphan-process checks.

## Examples

```sh
./build/examples/ex_plan_basic              # evidence, planning, explanation
./build/examples/ex_reserve_commit          # reserve, verified create, accounting
./build/examples/ex_fragmentation_rejection # shape fragmentation vs capacity
./build/examples/ex_drain_reconfigure       # drain gate, destructive layout change
./build/examples/ex_synthetic_heterogeneous # deterministic outcome differences
./build/examples/ex_reconcile_after_change  # missing, unexpected, reset
./build/examples/ex_installed_consumer      # embedding the runtime in-process
```

## Command line tools

`apfctl` is the inspection and administrative tool. Read-only commands and
administrative mutations are explicitly separated: nothing mutates state unless a
mutating command is selected *and* `--admin` is passed.

```sh
apfctl version
apfctl probe                       # what the real accelerator stack exposes
apfctl discover                    # SYNTHETIC and REAL discovery side by side
apfctl --state fabric.state snapshot
apfctl --coordinator 127.0.0.1:9000 accelerators
apfctl --coordinator 127.0.0.1:9000 --admin drain --partition 3
```

Read-only commands: `version`, `probe`, `discover`, `snapshot`,
`accelerators`, `partitions`, `workers`, `reservations`, `attempts`,
`assignments`, `fragmentation`, `plan`, `layout`.
Administrative commands (require `--admin`): `reserve`, `create`, `destroy`,
`drain`, `complete-drain`, `cancel-drain`, `release`, `reconcile`, `fence`,
`advance-epoch`, `persist`, `shutdown`.

`apfcoord` runs the coordinator; `apfworker` runs a worker incarnation:

```sh
apfcoord --listen 127.0.0.1:9000 --state fabric.state
apfworker --coordinator 127.0.0.1:9000 --backend synthetic --device sim-0
apfworker --coordinator 127.0.0.1:9000 --backend nvml     # real discovery only
```

## Installing and consuming the CMake package

```sh
cmake --install build --prefix /opt/apf
```

An independent consumer uses only the installed package:

```cmake
find_package(AcceleratorPartitionFabric CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE SummonSoftwareLabs::AcceleratorPartitionFabric)
```

`examples/downstream-consumer` is a complete standalone project that builds
against an install prefix and exercises registration, planning, fragmentation,
reservation, verified mutation, admission and durable state:

```sh
cmake -S examples/downstream-consumer -B consumer-build -G Ninja \
      -DCMAKE_PREFIX_PATH=/opt/apf
cmake --build consumer-build
./consumer-build/consumer
```

## Benchmarks

`apf_bench` measures completed work; each timed iteration finishes the whole
operation before the clock is read, and no benchmark reports enqueue latency as
throughput. Numbers below were observed on the validation machine (Windows 11,
MSVC 19.44, Release, single socket):

| Benchmark | Completed work | Measured |
| --- | --- | --- |
| plan, 1 device | 2000 plans | 1.9 us/plan |
| plan, 16 devices | 2000 plans | 15.6 us/plan |
| plan, 64 devices | 2000 plans | 74.8 us/plan |
| fragmentation analysis | 5000 analyses | 1.9 us/analysis |
| reserve + execute + verified commit | 1000 attempts | 16.9 us/attempt |
| snapshot, 8 devices | 2000 snapshots | 9.8 us/snapshot |
| admission evaluation | 5000 evaluations | 0.29 us/evaluation |
| reconciliation, 4 partitions | 1000 passes | 2.8 us/pass |
| deterministic explanation | 5000 explanations | 1.6 us/explanation |
| durable encode | 500 encodes | 41.0 us/encode |
| durable decode + validate | 500 decodes | 25.0 us/decode |
| durable save (atomic replace) | 100 saves | 1039 us/save |
| durable load + conservative import | 100 loads | 186 us/load |
| query payload round trip (4 KiB) | 5000 round trips | 3.8 us/round trip |

Planning cost against the number of published profiles on one device was flat
(about 32 us/plan at 4, 16 and 64 profiles), and planning cost scales linearly
with the number of devices, which is the expected shape for deterministic
candidate filtering. A quadratic encoding defect was found by these measurements
(durable encode was 674 us/encode) and fixed by growing the encoder buffer
geometrically.

## Validation evidence

All results below were produced by the repository as committed, on Windows 11
(10.0.26200), MSVC 19.44.35222, CMake 4.3.2, Ninja 1.13.2.

| Claim | Evidence |
| --- | --- |
| Release test suite | `apf_tests`: 61 passed, 0 failed |
| Multiprocess suite | `apf_mp_tests`: 4 passed, 0 failed, no orphan processes |
| Debug test suite | `build-debug`: 61 passed, 0 failed; multiprocess 4 passed, 0 failed |
| Strict warnings | `/W4 /WX` (and `-Wall -Wextra -Wpedantic ... -Werror` elsewhere) on every first-party target, zero warnings |
| AddressSanitizer | `-DAPF_ENABLE_ASAN=ON` (`/fsanitize=address /Oy-`): 61 passed, 0 failed; multiprocess 4 passed, 0 failed; no sanitizer report |
| CMake package | `cmake --install` to a prefix outside the source tree, then `find_package` from an independent project |
| Downstream consumer | Built from the install prefix only, runs to completion |
| Real accelerator discovery | NVIDIA GeForce RTX 5090, UUID `GPU-d1056bb6-4fec-2891-83f2-3a24fc70276b`, driver 616.92, compute capability 12.0, 32607 MiB |
| Physical partition capability | **UNSUPPORTED**: `nvmlDeviceGetMigMode` reports Not Supported on this device |
| Synthetic partition semantics | Multi-partition geometry, fragmentation, destructive reconfiguration, ambiguous completion and process death proven through the synthetic backend and real processes |

## Sanitizer note

The MSVC AddressSanitizer runtime is loaded dynamically, so the instrumented test
binaries need `clang_rt.asan_dynamic-x86_64.dll` on `PATH`. The validation run
added the toolchain directory that ships it
(`VC\\Tools\\MSVC\\<version>\\bin\\Hostx64\\x64`) to `PATH` before running
`build-asan/tests/apf_tests` and `build-asan/tests/apf_mp_tests`. No sanitizer
finding was reported in either suite, including the multiprocess proofs.

## Limitations

These are genuine, current limitations of the repository as committed:

* **No physical accelerator partitioning is validated.** The only real
  accelerator available exposes no partition mechanism, so every partition
  lifecycle proof in this repository is SYNTHETIC. The NVML backend performs
  read-only discovery and never mutates hardware.
* **The MIG enumeration path of the NVML backend has never executed.** It is
  guarded by a successful MIG capability query, which cannot happen on the
  validation hardware.
* **Only Windows x64 with MSVC 19.44 was validated.** The generic governance
  layer, the socket layer and the process layer contain POSIX code paths, but
  they have not been compiled or run here, so no other platform is claimed.
* **No CUDA integration.** The repository contains no CUDA code and no CUDA
  build option, so no CUDA memory, kernel, or parity claim is made. Partition
  functionality is not provable by a CUDA kernel, and this runtime does not
  pretend otherwise.
* **A peer that opens a connection and sends a partial frame occupies one session
  slot until it disconnects.** There is no receive timeout by design; the number
  of sessions and frames is bounded by `Limits`, and the coordinator refuses new
  sessions beyond the bound.
* **The distributed protocol has no authentication or encryption.** It is a
  control plane for a trusted network segment; binding it to a hostile network is
  out of scope for this release.
* **Reconfiguration downtime and drain estimates are backend-published or zero.**
  The runtime never invents a number, so planning against downtime is only as good
  as the backend's estimate.
* **Single coordinator per deployment.** There is no coordinator election or
  quorum; a restart advances the epoch and requires revalidation rather than
  providing availability.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
