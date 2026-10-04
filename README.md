# Capacity Observatory

Facility-capacity observability for facility deployments.

Capacity Observatory answers one question, and answers it with evidence:

> **Where is facility capacity committed, reserved, available, stranded, fragmented, disputed, or constrained,
> and which authoritative evidence explains every residual?**

It is a read-only observatory. It composes the evidence published by adjacent authorities into an exact,
explainable accounting of capacity, reports what it cannot explain instead of hiding it, and never mutates
another runtime's state.

---

## Systems boundary and non-ownership

Capacity Observatory **owns** the explanation and inspection of capacity state and capacity deltas: the derived
capacity classes, the closure identities and their residuals, the fragmentation and stranding analysis, the
evidence attribution behind every number, and the durable record of the evidence it was given.

Capacity Observatory **does not own and never mutates**:

| Not owned | Owner |
|---|---|
| Canonical capacity | the capacity authority (DCCP) |
| Reservations | the reservation authority (ASI) |
| Placement | the placement authority |
| Admission | the admission authority |
| Entitlements | the entitlement authority |
| Reconciliation mutations | the reconciling authority |

This is enforced structurally, not by convention. Evidence carries the authority that asserted it and the
capacity class it asserts; each authority is registered with an explicit contract naming the dimensions and
classes it is authoritative for. Evidence that is visible but out of contract is refused with
`EvidenceRefused` — visibility is not authority, and the observatory never infers one runtime's authority from
the presence of another runtime's data.

## Architecture

```
                 +--------------------------- adjacent authorities ---------------------------+
                 |        DCCP        ASI        DFI        policy       plant     arbiter  |
                 |     committed   reserved  installed   nameplate   observed   disputed  |
                 +------------------------------------+-----------------------------------+
                                                      |  immutable evidence records
                                                      v
  +---------------------------- Capacity Observatory --------------------------------------------------+
  |  AuthorityRegistry  ->  EvidenceWindow (acceptance, watermarks, epochs, generations, freshness)   |
  |            |                                                                                      |
  |            +--> Store (durable, framed, integrity checked, single writer, idempotent)             |
  |            |                                                                                      |
  |            +--> Ledger (declared classes -> derived classes -> closure checks -> residuals)        |
  |            |                                                                                      |
  |            +--> Topology + Fragmentation (realizable vs ideal, stranding, binding constraints)     |
  |            |                                                                                      |
  |            +--> Snapshot (deterministic JSON and text, with provenance and refusal audit)          |
  +---------------------------------------------------------------------------------------------------+
                                                      |
                                                      v
                                          CLI / library consumers
```

Layout:

| Path | Contents |
|---|---|
| `include/capacity_observatory/` | public headers: strong types, dimensions, evidence, acceptance, ledger, fragmentation, store, ingest, snapshot, JSON, concurrency |
| `src/` | implementation |
| `tests/` | unit, integration, end-to-end, property, seeded randomized, adversarial, failure-injection, real multiprocess, restart/reopen suites |
| `bench/` | measured benchmarks with REAL / SYNTHETIC / UNSUPPORTED labelling |
| `examples/consumer/` | an independent out-of-tree `find_package` consumer |
| `scripts/` | install/downstream validation and fresh-clone release validation |
| `docs/` | [architecture](docs/ARCHITECTURE.md), [persistence](docs/PERSISTENCE.md), [concurrency](docs/CONCURRENCY.md) |

## State, evidence, and authority model

**Evidence slot.** A record occupies the slot `(authority, scope, dimension, capacity class)`. The scope path
is a validated outermost-first path (`site=dc1/hall=h1/zone=z1/row=r1/enclosure=e1`). The dimension is one of
`space`, `rack-units`, `power`, `cooling`, `weight`, `serviceability`. The capacity class is one of
`nameplate`, `governed`, `planned`, `installed`, `observed`, `reserved`, `committed`, `available`,
`stranded`, `disputed`, `excluded-policy`, `excluded-maintenance`, `excluded-failure`, `operational-reserve`.

The specification's planned / reserved / installed / observed / available lifecycle distinctions are preserved
as class members rather than as a second redundant field, so there is exactly one vocabulary to reason about.

**Exact accounting.** Every magnitude is an exact signed integer in the canonical unit of its dimension
(space in mm², rack units in U, power in mW, cooling in mWth, weight in g, serviceability in positions).
A declared unit is an exact rational multiple of the canonical unit. Conversion is exact or refused with
`InexactScale`; it is never rounded. Every add, subtract, multiply, and divide is overflow checked and returns
`Overflow` instead of wrapping.

**Unknown is not zero.** An amount that is not known is `std::nullopt`, and unknown propagates through every
identity. A derived value is either known or explicitly unknown; a missing record never becomes a zero
contribution, and a partial line can never be mistaken for a complete one.

**Epoch, generation, revision, freshness.** Each authority has an epoch (reset) and a per-slot monotonic
generation. An older epoch is refused with `StaleEpoch`; an older generation within an epoch is refused with
`StaleGeneration`; the same generation with a different digest is `ConflictingEvidence`; the same generation
with the identical digest is an idempotent `DuplicateEvidence`. A skipped generation is accepted and reported
as `GenerationGap`. Freshness is evaluated against an injected clock and the authority's declared staleness
budget; evidence recovered from durable storage is `Recovered` and is never promoted to fresh by recovery
alone. Refusals are retained in a bounded audit trail and never change derived state.

## The ledger and its closure identities

For each scope and dimension, the declared classes are aggregated and the derived classes computed with exact
checked arithmetic:

```
exclusion_total = policy + maintenance + failure + operational_reserve
serviceable     = installed - failure - maintenance
free_usable     = serviceable - committed - reserved - stranded - disputed
governed        = declared governed ceiling, or nameplate - (policy + maintenance + failure + reserve)
```

Four closure checks are evaluated, each reporting a signed residual and an explanation:

| Check | Identity | Residual means |
|---|---|---|
| governance | `nameplate = policy + maintenance + failure + reserve + governed` | the policy authority's declared governed ceiling disagrees with its own nameplate evidence |
| installed | `installed = failure + maintenance + stranded + disputed + committed + reserved + free_usable` | the physical partition does not close (reported so the identity stays auditable) |
| available | `declared available = free_usable` | the declared available figure is not explained by the other evidence in scope |
| allocation | `committed + reserved <= governed` | allocations exceed the governed ceiling (over-commitment) |

Residuals are computed, never clamped, and may be negative. A line is `complete` only when every identity
evaluated and every residual is exactly zero; otherwise it is `residual`, `partial`, or `indeterminate`, and
the number of unknown classes is reported.

## Fragmentation, stranding, and binding constraints

A placement profile demands a rack height in U plus a per-rack budget of any dimensions it names. A rack must
sit in one contiguous free run inside one enclosure.

* **realizable** — the exact maximum number of racks that actually fit, summed over enclosures.
* **ideal** — the aggregate bound that ignores enclosure boundaries and gaps: total free capacity divided by
  per-rack demand.
* **fragmented** — `ideal - realizable`, reported per dimension as well as overall. The invariants
  `realizable <= ideal` and `fragmented = ideal - realizable` are asserted at run time; a violation is an
  `InvariantViolation`, because it would mean the analysis is wrong, not the facility.
* **stranded** — the free capacity of an enclosure that can host none of the profile, and of enclosures that
  are structurally unusable. Stranded capacity is never counted as ideal.
* **binding** — which constraint limited each enclosure (`rack-units-contiguity`, a named dimension, or
  `structurally-unusable`).

## Persistence and recovery

A store directory holds `evidence.log` (append-only, framed), `snapshot.json` (atomically published, disposable),
`epoch` (atomically published writer-session epoch), and `writer.lock` (kernel-enforced single-writer lock).

* Frames are `[u32 payload length][u32 CRC-32C][canonical JSON payload]` after a magic/version header with its
  own integrity check.
* **The commit point is a successful flush of a complete frame.** Before that call returns the record is either
  fully durable or not durable at all.
* A **torn tail** (a partial or CRC-failing final frame with nothing valid after it) is truncated conservatively
  on reopen and reported. **Interior corruption** (damage with valid frames after it) rejects the whole log with
  `InteriorCorruption`; later records are never silently dropped.
* Every writable open publishes a higher **session epoch**; the epoch is recorded in every frame.
* **Idempotency identity lives in the same durable commit as the mutation.** Replaying the same mutation with the
  same content is answered with `IdempotentReplay` and appends nothing; reusing it with different content is
  `IdempotencyConflict`.
* Ingest is **durability first**: derived state is only ever built from evidence that already survived a durable
  commit.

Details, including the exact recovery algorithm and what is *not* claimed, are in
[docs/PERSISTENCE.md](docs/PERSISTENCE.md).

## Concurrency

* There is **no read→write upgrade API**. A caller either takes the write lock directly or releases the read
  guard first, so upgrade self-deadlock is not expressible.
* Callbacks are **never invoked while a lock is held**: publishers copy the subscriber list under the lock and
  deliver after releasing it.
* The lock order is documented (`control < state < store < publication`) and audited at run time in builds that
  set `CO_ENABLE_LOCK_AUDIT=ON`.
* Background workers are joined only from outside every state lock; shutdown is a state machine.
* The ingest queue is bounded and **refuses** (`QueueFull`) instead of blocking a producer or growing without
  bound.

See [docs/CONCURRENCY.md](docs/CONCURRENCY.md) for the hazard-by-hazard audit.

## Build

Requirements: CMake ≥ 3.25, a C++20 compiler (validated with MSVC 19.44; GCC and Clang are supported by the
build files but not validated on this machine), Ninja recommended.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful options: `CO_BUILD_TESTS`, `CO_BUILD_BENCHMARKS`, `CO_BUILD_CLI`, `CO_WARNINGS_AS_ERRORS` (default ON),
`CO_ENABLE_ASAN`, `CO_ENABLE_ANALYZE`, `CO_ENABLE_LOCK_AUDIT`.

## Install and downstream use

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=C:/prefix
cmake --build build
cmake --install build
```

A consumer project then writes only:

```cmake
find_package(CapacityObservatory 1.0 REQUIRED CONFIG)
target_link_libraries(my-target PRIVATE CapacityObservatory::capacity_observatory)
```

[examples/consumer/](examples/consumer/) is a complete independent project that is **not** part of this build
tree; [scripts/validate-install.ps1](scripts/validate-install.ps1) installs to a clean prefix, builds that
consumer out of tree against the installed package, and runs it.

## Library use

```cpp
#include <capacity_observatory/ledger.hpp>
#include <capacity_observatory/watermark.hpp>

using namespace co;

const std::shared_ptr<Clock> clock = manual_clock(Timestamp(1'700'000'000'000'000'000LL));
const AuthorityRegistry registry = AuthorityRegistry::standard();
EvidenceWindow window(registry, clock);

const Result<EvidenceRecord> record = EvidenceRecord::make(
    AuthorityId("dccp"), AuthorityRole::CommittedCapacity, ScopePath::parse("site=dc1/hall=h1").value(),
    Dimension::Power, CapacityAssertion::Committed, Unit::parse_for(Dimension::Power, "kW").value(), 900,
    Generation(33), Epoch(1), Revision(0));
const AcceptanceDecision decision = window.consider(record.value());

LedgerRequest request;
request.scope = ScopePath::parse("site=dc1/hall=h1").value();
request.dimensions = DimensionSet::of(Dimension::Power);
const Ledger ledger = compose_ledger(window, request).value();
const LedgerLine* line = ledger.find(Dimension::Power);
```

Every fallible call returns `co::Result<T>` carrying a stable `co::ReasonCode`; nothing throws for a domain
outcome. `co::ReasonCode` values are part of the public contract and are classified as refused, indeterminate,
or failed so callers can react without string matching.

## Command line

```
capacity-observatory ingest        --store DIR --evidence FILE [--json] [--now NANOS]
capacity-observatory explain       (--store DIR | --evidence FILE) [--scope PATH] [--dimension NAME] [--json]
capacity-observatory closure       (--store DIR | --evidence FILE) [--scope PATH] [--json]
capacity-observatory fragmentation  --topology FILE --profile FILE [--scope PATH] [--json]
capacity-observatory verify        --store DIR [--json]
capacity-observatory snapshot      --store DIR [--json] [--out FILE]
capacity-observatory recover       --store DIR [--json]
capacity-observatory version | help
```

Exit codes are deterministic: `0` success, `1` usage error, `2` a reported refusal or indeterminate result,
`3` integrity or I/O failure. Output for identical inputs is byte identical.

## Validation

Everything below was executed on this machine (Windows 11, MSVC 19.44.35222, CMake 4.3.2, Ninja, 16 hardware threads) against the
released source state. `ctest --test-dir build --output-on-failure` runs the whole matrix; Release, Debug, and an
AddressSanitizer configuration all pass with zero test failures.

| Suite | Cases | Assertions | What it proves |
|---|---:|---:|---|
| `test_foundations` | 16 | 162 | reason-code stability, strong types, checked arithmetic at the integer boundaries, exact-or-refused unit conversion, CRC-32C and SHA-256 against published vectors, freshness states |
| `test_json` | 25 | 489 | strict parsing of every documented rejection with its exact reason code, surrogate pairs, UTF-8 validation, byte-order-mark tolerance, canonical and pretty writers, INT64 boundaries, megabyte documents, deep-nesting bombs |
| `test_concurrency` | 10 | 325 | no torn reads under 4 readers and 200 writes, callbacks delivered with no lock held, lock-order auditor semantics with the auditor compiled in, bounded queue refusal, worker restart, shutdown that drains every queued item, concurrent submit-and-snapshot agreement, stale-authority races, concurrent publication |
| `test_adversarial` | 13 | 280 | dimension/unit confusion, stale epoch and generation, conflicting vs duplicate digests, authorities asserting outside their contract, unknown authority, validity windows, stale freshness that is never promoted, malformed scopes and identifiers, bounded refusal audit, duplicate and reordered ingestion |
| `test_property_conservation` | 15 | 29,380 | 450 seeded randomized ledgers compared field by field against an independent arbitrary-precision reference model, conservation closure, unknown-propagation (no missing-to-zero), boundary sums that must refuse with Overflow, and identical results across shuffled and duplicated ingest orders |
| `test_property_fragmentation` | 11 | 20,944 | 370 randomized and gapped topologies compared against exhaustive enumeration of every rack distribution, plus exact-fit, one-unit-short, gap, unusable, unknown-dimension, unknown-contiguity, empty-topology, and degenerate-profile cases |
| `test_store_persistence` | 17 | 465 | framing byte by byte, header and frame integrity, epoch publication and integrity, atomic snapshot publication, bounded sizes, idempotency, and durability of committed records |
| `test_store_failure_injection` | 15 | 7,185 | torn tails at every position recovered with an exact discarded-byte count, interior corruption rejected at any distance including beyond any search window, header preservation when the first frame is damaged, the sequence ceiling, and configuration mismatches refused with actionable reasons |
| `test_store_multiprocess` | 6 | 165 | real child processes: cross-process lock exclusion, kernel release of the lock when a holder is terminated, committed records surviving an abrupt kill, and a killed partial write recovered by the next process |
| `test_restart_reopen` | 6 | 242 | restart inside one process and across processes: every record marked recovered and never fresh, watermarks rebuilt from durable state so stale replays are still refused, byte-identical derived numbers before and after, and self-consistent recovery |
| `test_cli_e2e` | 9 | 402 | the real installed-style binary run as a subprocess: exit codes 0/1/2/3, JSON output re-parsed by the library's own parser, a store written by one process and explained by another, a second concurrent writer refused, byte-identical repeated output, and integrity failures reported as such |

Totals: **143 cases, 60,039 assertions**, plus a self-checking benchmark (176 internal checks) and a static-analysis
build (`-DCO_ENABLE_ANALYZE=ON`) that reports zero warnings.

Additional proofs run outside the unit suites:

* `scripts/validate-install.ps1` builds Release, runs `ctest`, installs into a clean prefix, builds the independent
  out-of-tree consumer at [examples/consumer/](examples/consumer/) against the installed package with
  `find_package(CapacityObservatory CONFIG)`, runs it, and then drives the **installed** `capacity-observatory` binary
  through a full ingest → verify → explain cycle in two separate processes, asserting the ledger is complete, the
  derived numbers are exact, and the two process outputs are byte-identical.
* `scripts/validate-release.ps1` clones the remote into a temporary directory and repeats the install/downstream
  validation from the released state.

## Benchmarks

`bench/` measures completed work with `std::chrono::steady_clock` and labels every row **REAL**, **SYNTHETIC**, or
**UNSUPPORTED**. Every synthetic input derives from a printed seed, every row prints its exact input size, and every
row self-checks that the work it timed actually completed.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCO_BUILD_BENCHMARKS=ON
cmake --build build
./build/bench/capacity-observatory-bench --quick     # or omit --quick for the larger default sizes
```

Representative Release measurements from this machine (16 hardware threads, MSVC 19.44, seed `0x5eed0000c0ffee01`,
quick sizes: 500 enclosures, 19,524 fixture records, 1,000 durable appends):

| Row | Label | Work | Measured |
|---|---|---|---|
| evidence ingest + acceptance | REAL (synthetic data) | 19,524 records through the real pipeline and acceptance window, each with an exact unit conversion | 0.346 s, 56,375 records/s |
| ledger composition, per scope | REAL | `compose_ledger` scanning a 19,524-record window | 1.83 ms/ledger, 10.7 M records scanned/s |
| ledger composition, roll-up | REAL | the same window aggregated across 65 scopes | 4.90 ms/ledger |
| ledger with deliberately inconsistent evidence | REAL | a residual of −1,000,000 governance and +1,000,000 available, reported and never clamped | 5.40 ms/ledger |
| fragmentation analysis | REAL | 500 enclosures (30 structurally unusable), realizable 1,033 vs ideal 1,410, fragmented 377 | 0.44 ms/analysis |
| durable append | REAL | 1,000 records committed and flushed one at a time (`FlushFileBuffers` per frame) | 1.86 s, 537 records/s, 364 kB/s, 677.6 bytes/record |
| store reopen and recovery | REAL | 1,001 framed records re-read, CRC-checked, and re-parsed, plus the next session epoch publication | 24.9 ms, 40,206 frames/s |
| read-only inspection scan | REAL | the same log scanned without taking the writer lock | 16.7 ms, 59,842 frames/s |
| snapshot JSON serialization | REAL | a 363,394-byte canonical document over 500 enclosures | 8.0 ms/document |
| snapshot JSON parsing | REAL | the library's own strict parser over those bytes | 2.5 ms/document, 7.3 MB/s |
| fixture generation | SYNTHETIC | the seeded generator itself, not library work | 0.023 s |
| network fabric ingest | UNSUPPORTED | no network transport exists in this tree | not attempted |
| multi-node cluster throughput | UNSUPPORTED | single-process library, no clustering or replication | not attempted |
| BMS/DCIM protocol ingest | UNSUPPORTED | no BMS, DCIM, Modbus, SNMP, or Redfish implementation exists | not attempted |
| electrical power measurement | UNSUPPORTED | power is an exact integer capacity dimension asserted by an authority, not a measurement taken here | not attempted |

These are measurements, not promises: they are Debug-gated nowhere and Release-gated nowhere, they are simply what this
machine did on this input, and each row prints the input size it used so the numbers can be reproduced or challenged.

## REAL, SYNTHETIC, and UNSUPPORTED

Honesty about the provenance of every claim is part of the contract:

* **REAL** — the runtime, the checked arithmetic, the closure identities, the fragmentation analysis, the
  durable store with its framing/recovery/locking, packaging, installation, and the out-of-tree consumer. All of
  it runs on this machine and is covered by the test suites listed below.
* **SYNTHETIC** — plant, DCIM, BMS, telemetry, and economic evidence. No real facility hardware or plant data is
  attached to this repository, so the `plant` and `economics` authorities are registered as synthetic sources
  and the snapshot provenance says so. Their *handling* is real; their *values* are supplied by the caller.
* **UNSUPPORTED** — there is no electrical, cooling, multi-node, or hardware-in-the-loop proof here, and none is
  claimed. Benchmarks mark anything they cannot measure as UNSUPPORTED rather than printing a number.

## Limitations

* Capacity Observatory is not a source of truth. If an authority publishes wrong evidence, the observatory
  explains it faithfully and reports the residual; it does not correct it.
* Roll-up lines aggregate evidence from several scope depths into one line when a parent scope is queried. The
  line reports the contributing scopes and the number of depths so a roll-up is never mistaken for a single
  authority figure.
* The four closure identities are the ones defined above. They are deliberately conservative: an identity that
  cannot be evaluated is reported as indeterminate, not assumed to hold.
* The POSIX code paths (flock, fsync, pread, rename) are written and compiled only on non-Windows hosts; they are
  **not validated on this machine**.
* Beyond the operating system's own flush guarantees, no power-loss atomicity is claimed. Snapshot publication
  and the log record that names the published digest are two steps, not one transaction.
* Fragmentation uses one uniform rack profile per analysis. Mixed-profile packing is not modelled.
* JSON nesting is accepted up to 64 levels and refused beyond it, whatever `max_depth` a caller asks for. The
  ceiling is measured rather than assumed: an instrumented build overflowed its stack on a 200-level document.
* A writable open of a store whose durable sequence counter has reached the signed 64-bit ceiling is refused with
  `EpochExhausted`; the data stays readable through a read-only open.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
