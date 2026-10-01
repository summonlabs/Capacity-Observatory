# Capacity Observatory - Architecture

This document describes the system that exists in this repository, for an
engineer who has to integrate with it or audit it. Every statement is grounded
in the headers under `include/capacity_observatory/` and the implementation
under `src/`; file names are cited inline. Where something is measured,
modelled, or simply not proven on this machine, it says so.

## 1. What the system is, and the question it answers

Capacity Observatory is a facility capacity observability runtime. It consumes
immutable evidence records asserted by adjacent authorities, decides which of
them may be accepted, and explains what capacity exists per scope and dimension,
with every unexplained residual reported rather than clamped.

The core question, stated in `include/capacity_observatory/snapshot.hpp`, is:
which classes hold how much capacity, which authority evidence explains every
residual, and how fresh and how authoritative that evidence is.

The observable answers are a `co::Ledger` (per-dimension lines with declared
classes, derived quantities, and four closure checks), a `co::Snapshot` (the
ledger plus optional fragmentation analysis plus provenance), deterministic JSON
rendering (`co::snapshot_to_json`), and a human-readable rendering
(`co::render_snapshot_text`). Both renderings are deterministic: identical
inputs produce byte-identical output (`src/snapshot.cpp`).

## 2. Systems boundary and explicit non-ownership

The observatory does not own capacity. It owns the explanation of capacity.

It owns no canonical capacity, no reservations, no placement, no admission, no
entitlements, and no reconciliation mutations. The rendered boundary string in
`src/snapshot.cpp` states this in the output itself, and the same boundary is
restated in `include/capacity_observatory/evidence.hpp` ("The observatory never
mutates adjacent authority state"), in
`include/capacity_observatory/topology.hpp` ("Topology is inspection input: it
describes what placement room exists, not what may be admitted. The observatory
never places or admits anything"), and in
`include/capacity_observatory/fragmentation.hpp` ("Free capacity that is spread
across enclosures or interrupted by gaps cannot host it").

What the system does own:

* acceptance state for evidence: which record currently occupies a slot, and the
  watermark of that slot (`co::EvidenceWindow` in `src/watermark.cpp`);
* a durable, integrity-checked evidence log and its recovery rules
  (`co::Store` in `src/store_file.cpp`);
* derived state: the ledger, the fragmentation analysis, and the snapshot;
* a bounded audit trail of refused evidence.

What it does not do:

* it never writes to another runtime's state; the only mutable bytes it produces
  are its own store directory;
* it never places, admits, reserves, or commits anything;
* it never resolves a dispute; it can only report that an authority asserted
  `CapacityAssertion::Disputed`;
* it never invoices, prices, or trades; the `AuthorityRole::Economics` contract
  in `co::AuthorityRegistry::standard()` carries an empty assertion set, so an
  economic record asserting any capacity class is refused with
  `ReasonCode::EvidenceRefused`.

## 3. Authority model

Authority is declared, not inferred. `co::AuthorityContract` (in
`include/capacity_observatory/evidence.hpp`) binds an identifier to:

* a role (`co::AuthorityRole`),
* the dimensions it may speak about (`co::DimensionSet`),
* the capacity classes it may assert (`co::AssertionSet`),
* a staleness budget (`co::Duration`; zero means no budget is declared),
* whether the source is modelled rather than measured (`c synthetic_source`).

`co::AuthorityRegistry::standard()` (`src/evidence.cpp`) defines the
deployment's registry. It is the registry the tests use; a deployment supplies
its own with the same shape:

| authority | role | assertions granted | staleness budget | synthetic |
|---|---|---|---|---|
| `dccp` | CommittedCapacity | committed | 900 s | no |
| `asi` | Reservations | reserved | 300 s | no |
| `dfi` | InstalledInventory | installed, planned, available, stranded | 3600 s | no |
| `policy` | Policy | nameplate, governed, excluded-policy, excluded-maintenance, excluded-failure, operational-reserve | 86400 s | no |
| `plant` | PlantTelemetry | observed | 60 s | yes |
| `economics` | Economics | (none) | 3600 s | yes |
| `arbiter` | Arbitration | disputed | 86400 s | no |

All seven contracts are granted every dimension in the standard registry.

### The AssertionSet contract makes "visibility is not authority" structural

`co::EvidenceWindow::consider` (`src/watermark.cpp`) checks the registry
before it looks at any watermark, generation, or epoch:

1. the record must pass `co::EvidenceRecord::validate()`;
2. the authority must exist in the registry, otherwise
   `ReasonCode::UnknownAuthority`;
3. `contract.accepts(dimension, assertion)` must be true, otherwise
   `ReasonCode::EvidenceRefused` with an explanation of what the contract does
   grant;
4. only then are validity window, epoch, and generation considered.

Consequences that follow from that ordering:

* Seeing another runtime's evidence never transfers its authority. A record whose
  `(authority, dimension, assertion)` triple is outside the contract is refused
  even when the record is perfectly formed, in date, and newer than the current
  one, and it never reaches derived state.
* `EvidenceRecord::role` is descriptive. `validate()` does not consult the
  registry, and `consider()` authorises against the registry contract, not
  against the role carried in the record.
* A capacity class no authority is granted can never appear in a ledger. In the
  standard registry every one of the fourteen assertions is granted to exactly
  one authority except `observed` (plant) and `disputed` (arbiter), which are
  single-authority as well.

## 4. Evidence model

An `co::EvidenceRecord` is immutable input. Its fields are:

* slot identity: `authority`, `scope`, `dimension`, `assertion`. The slot
  text is `authority|scope|power|installed` style
  (`co::EvidenceRecord::slot_text`); `id_text` appends `|e<epoch>|g<generation>`.
* magnitude: `unit` (an exact rational multiple of the canonical unit),
  `declared_amount` as the authority wrote it, and `amount`, the exact
  canonical amount produced by the conversion. A record is only constructible
  through `EvidenceRecord::make`, which refuses a unit whose dimension differs
  from the record dimension and refuses an inexact conversion.
* ordering: `generation`, `epoch`, `revision` (all strong types; see
  section 8).
* provenance: `co::Provenance` = Observed, Recovered, or Synthetic.
* time: an optional observation instant (`has_observed_at`, `observed_at`)
  and a validity window (`from`, `until`; closed, inclusive of `until`).
* `source`: a deterministic provenance note such as `dccp-2026-01.jsonl:7`.

Scope paths (`co::ScopePath`) are `site=dc1/hall=h1/zone=z1` style: kinds
appear outermost first, each kind at most once, each name a validated identifier.
`ScopePath::is_prefix_of` defines exactly which evidence a request aggregates
(section 6).

### Slot, watermark, and content identity

The unit of competition is the slot: `co::EvidenceSlotKey`, ordered by
(authority, scope, dimension, assertion). Each slot has at most one current
record and one `co::Watermark` = {epoch, generation, digest, present}.
Accepted records replace the previous current record of their slot; refused and
superseded records are retained only for explanation.

`co::EvidenceRecord::content_digest()` is SHA-256 over every field that changes
the meaning of the record. The digest is computed with an unambiguous encoding
(`DigestBuilder` in `src/evidence.cpp`): the domain preamble `co.evidence.v1`,
then for each of twenty-one named fields the name, 0x1F, the decimal length of
the value, 0x1E, the value bytes, 0x1D. Length-prefixing is what makes
`authority="ab", scope="c"` distinguishable from `authority="a",
scope="bc"`. The digest is the tie-breaker for equal generations: same
generation and same digest is a duplicate; same generation and a different digest
is a conflict.

### Freshness

`co::evaluate_freshness` (`src/core.cpp`) is evaluated against the
registry's staleness budget and the evaluation instant, never against a hidden
clock read:

* no observation instant: Unknown;
* observation instant after the evaluation instant: Future (or Recovered when the
  record was recovered), with `skew_detected` set and the skew reported;
* recovered provenance: Recovered, always, regardless of age; recovery is not
  observation and never promotes a record to Fresh;
* age beyond the declared budget: Stale;
* otherwise Fresh. A zero budget means "no staleness budget is declared" and
  yields Fresh for a non-recovered record.

`co::EvidenceWindow::freshness_of` re-evaluates an already accepted record at
the current instant without mutating watermarks, so a snapshot's freshness is
the freshness at the snapshot instant.

## 5. Acceptance rules

`co::EvidenceWindow::consider` returns an `co::AcceptanceDecision` with one
of seven outcomes. In evaluation order:

| step | condition | outcome | reason code | affects derived state |
|---|---|---|---|---|
| 1 | `validate()` fails | Invalid | the validation reason | no |
| 2 | authority not in registry | Refused | UnknownAuthority | no |
| 3 | contract does not grant (dimension, assertion) | Refused | EvidenceRefused | no |
| 4 | evaluation instant before `validity.from` | Refused | EvidenceNotYetValid | no |
| 5 | evaluation instant after `validity.until` | Refused | EvidenceExpired | no |
| 6 | record epoch < watermark epoch | StaleEpoch | StaleEpoch | no |
| 7 | same epoch, generation < watermark generation | StaleGeneration | StaleGeneration | no |
| 8 | same epoch and generation, identical digest | Duplicate | DuplicateEvidence | no |
| 9 | same epoch and generation, different digest | Conflicted | ConflictingEvidence | no |
| 10 | otherwise | Accepted | Ok, or GenerationGap | yes |

Details that matter to a caller:

* Refusals are retained in a bounded audit trail
  (`EvidenceWindow::refusals()`). The default capacity is
  `kDefaultRefusalCapacity = 4096`; beyond it, refusals are counted in
  `dropped_refusals()` and not stored. A Duplicate is not retained (it is a
  no-op, not evidence of a problem).
* An accepted record always advances the watermark to
  `(record.epoch, record.generation, digest)`. A newer epoch resets the
  generation comparison entirely, which is reported as `epoch_advanced`.
* An accepted record with `generation > previous + 1` in the same epoch is
  still accepted, and flagged: `generation_gap = true` and the decision reason
  is `ReasonCode::GenerationGap`. A gap is reported, not silently tolerated.
* Refusals never change derived state. `affects_derived_state` is false for
  every non-accepted outcome.
* `co::EvidenceWindow::mark_all_recovered` rewrites provenance to Recovered
  after a durable reopen; a recovered record stays current for explanation but is
  never promoted to fresh.
* `co::IngestPipeline::ingest` (`src/ingest.cpp`) makes the durable commit
  first and only then calls `consider`. A crash between the two can lose a
  derivation but can never fabricate one. A store-attached pipeline answers a
  replayed mutation identity with `IngestDisposition::Duplicate` and does not
  re-enter the window; a caller that attaches a store must therefore recover the
  window from that same store (`co::Store::load_evidence`) before ingesting.

## 6. Ledger identities and the four closure checks

`co::compose_ledger` (`src/ledger.cpp`) aggregates every current record whose
scope is at or below the request scope (`request.scope.is_prefix_of(record.scope)`)
and whose dimension is requested. For each dimension it produces one
`co::LedgerLine`.

Declared classes: `declared[a] = sum of record.amount over matching records
with assertion a`, computed with `checked_add`; an overflow fails the whole
composition with `ReasonCode::Overflow`. Each line also carries
`evidence_count`, the sorted `evidence_ids`, the `contributing_scopes`, and
the worst freshness among its records (Fresh < Future < Recovered < Stale <
Unknown).

Derived quantities (`Amount` is signed; all arithmetic is checked):

* `exclusion_total = excluded-policy + excluded-maintenance + excluded-failure + operational-reserve`
* `serviceable = installed - excluded-failure - excluded-maintenance`
* `free_usable = serviceable - committed - reserved - stranded - disputed`
* `governed_derived = nameplate - (excluded-policy + excluded-maintenance + excluded-failure + operational-reserve)`
* `governed = declared governed` when a `governed` record exists in scope,
  otherwise `governed_derived` (and `governed_derived` is set true).

Any unknown input makes its derived value unknown; there is no
missing-to-zero conversion anywhere in `src/ledger.cpp`.

### The four closure checks

Each line carries `std::array<ClosureCheck, 4> closures` named, in order,
`governance`, `installed`, `available`, `allocation`. Each check has a
`residual` (declared minus derived; negative is meaningful), a `holds` flag,
an `indeterminate` flag, a reason code, and a human-readable explanation.

1. Governance (`closures[0]`). Identity:
   `nameplate = excluded-policy + excluded-maintenance + excluded-failure + operational-reserve + governed`.
   `residual = nameplate - (excluded-policy + excluded-maintenance + excluded-failure + operational-reserve + governed)`.
   `holds = residual == 0`. Failure reason `ResidualGovernance`. Indeterminate
   when nameplate or the
   governed partition is unknown (`UnknownNameplate`).
2. Installed partition (`closures[1]`). Identity:
   `installed = excluded-failure + excluded-maintenance + stranded + disputed + committed + reserved + free_usable`.
   `residual = installed - that sum`, `holds = residual == 0`, failure
   reason `ResidualUnallocated`. Indeterminate when installed or the partition
   is unknown (`UnknownInstalled`). A negative `free_usable` here means the
   allocations exceed installed capacity, and the check explanation says so.
3. Available (`closures[2]`). `residual = declared available - free_usable`,
   `holds = residual == 0`, failure reason `AvailableMismatch`.
   Indeterminate when either side is unknown (`UnknownExclusions`).
4. Allocation (`closures[3]`). This is an inequality, not an identity:
   `residual = (committed + reserved) - governed`, `holds = residual <= 0`,
   failure reason `Overcommitted`. A negative residual is headroom under the
   governed ceiling. Indeterminate when allocated or governed is unknown
   (`UndeterminedCapacity`).

Nothing is clamped. A residual is reported with its sign. When derived free
capacity is negative the line is additionally marked `overcommitted` and the
recorded `overcommitment` becomes the magnitude of that negative free capacity,
unless closure 4 has already recorded a larger positive excess (the implemented
condition is `overcommitment < free_usable`, where `free_usable` is
negative). Over-subscription therefore has two independent detectors: the
allocation closure against the governed ceiling, and a negative derived free
capacity against installed capacity.

### Line state and completeness

`co::LineState` is chosen in this exact order:

* no matching evidence at all: `Indeterminate` (the line is emitted with
  declared classes all unknown, worst freshness Unknown, and all four closures
  named and marked indeterminate with `UndeterminedCapacity`);
* otherwise, if both `installed` and `committed` are unknown:
  `Indeterminate`;
* otherwise, if some closure neither holds nor is indeterminate and its residual
  is non-zero: `Residual`;
* otherwise, if any of the nine identity classes is unknown, or any closure is
  indeterminate: `Partial`;
* otherwise: `Complete`.

Because closure 4 is an inequality, its "not holding" case is over-commitment
only; a negative allocation residual is headroom and leaves the line
`Complete`. A line is therefore `Residual` exactly when one of the three
partition identities fails to close, or when the allocation identity is
exceeded.

The nine identity classes are installed, committed, reserved, stranded,
disputed, excluded-failure, excluded-maintenance, available, and nameplate;
`LedgerLine::unknown_classes` counts how many of them are unknown.

Two ledger-level predicates summarise the same facts and can be read instead of
walking every line:

* `co::Ledger::has_residuals()` is true when `unexplained_governance`,
  `unexplained_installed`, or `unexplained_available` (the residuals of
  closures 1 to 3, which are zero when the identity holds) is non-zero;
* `co::Ledger::has_overcommitment()` is true when any line is overcommitted.

Both agree with a line-state scan: a line that reports `Complete` contributes
neither a residual nor an over-commitment, and a line that reports `Residual`
contributes one of the two. A consumer that needs the exact figure should read
`unexplained_governance`, `unexplained_installed`, `unexplained_available`,
and `overcommitment` for the line it is explaining.

The ledger detects *inconsistent* declarations, not *duplicated* ones. Hall-level
policy evidence that repeats its enclosures' nameplate adds to the roll-up
without violating any identity; that is visible in `contributing_scopes` and in
the "aggregated evidence from N scope depths" explanation, not in a residual.
Re-declaring a subtractive class at a coarser scope (maintenance already
excluded by the enclosures) does produce a residual, and a benchmark row in
`bench/bench_main.cpp` asserts the exact values of both affected residuals.

## 7. Fragmentation, stranding, and binding constraints

`co::analyze_fragmentation` (`src/fragmentation.cpp`) answers a different
question from the ledger: given a rack profile and the free budgets of the
enclosures in scope, how many racks of that profile can actually be placed, and
how much of the difference between the aggregate total and that number is
explained by enclosure and gap boundaries.

Inputs:

* `co::Topology`: a list of `co::EnclosureBudget`, each with free capacity per
  dimension (`nullopt` = unknown), the longest contiguous run of free rack units
  (`free_ru_contiguous`), the total free rack units
  (`free_ru_total`), and a `structurally_usable` flag with a reason.
* `co::RackProfile`: a name, a rack height in U, and a per-rack demand vector.
  A dimension the profile does not demand must be left unknown; a present zero is
  a positive requirement of zero and is refused by `RackProfile::make`
  (`ZeroProfile`), as is a profile that demands nothing at all.

Definitions, exactly as implemented:

* Per enclosure, if it is structurally usable and declares contiguous free rack
  units: `slot_limit = floor(free_ru_contiguous / profile.rack_units)` and
  `racks = min(slot_limit, floor(free[d] / demand[d]) for every demanded
  dimension d whose free amount is declared)`. The binding constraint is the
  first limit that lowered `racks` (dimension order is the canonical dimension
  order); the default binding is `rack-units-contiguity`.
* An enclosure that is structurally unusable hosts nothing, with binding
  `structurally-unusable`.
* An enclosure that does not declare contiguous free rack units, or that fails to
  declare a demanded dimension, is **indeterminate**: placement cannot be
  decided, and the enclosure is excluded from both the realizable and the ideal
  totals rather than being counted as zero.
* `realizable_racks = sum of racks over evaluable enclosures`.
* `ideal_racks = min(floor(sum of contiguous RU over evaluable usable
  enclosures / rack height), floor(sum of free[d] / demand[d]))`, where the
  per-dimension term exists only for demanded dimensions with a known total. The
  ideal ignores enclosure boundaries and gaps; structurally unusable capacity is
  excluded from it.
* `fragmented_racks = ideal_racks - realizable_racks` (never negative; a
  violation raises `ReasonCode::InvariantViolation`, because
  `sum(floor(x_i/d)) <= floor(sum(x_i)/d)` always holds).
* `stranded[d]` is the free capacity of an enclosure that can host none of
  the profile (structurally unusable, or zero realizable racks). Stranded
  capacity is reported separately and is never folded into the ideal or the
  fragmentation figure.
* `headroom[d]` is what remains free in an enclosure after its realizable
  racks are placed: for an undemanded dimension the whole free amount, for a
  demanded dimension `free[d] - demand[d] * racks`, and for an enclosure that
  hosts nothing the whole free amount.
* `constrained_dimensions` collects rack-units (when an evaluable, usable
  enclosure realizes zero racks), every dimension that bound an enclosure
  (`ReasonCode::BindingConstraint`), and every dimension that lowered the
  aggregate ideal.
* `indeterminate` and `indeterminate_enclosures` report how many enclosures
  could not be evaluated; when set, realizable and ideal are lower bounds over
  the evaluable set.

Every enclosure keeps its own `EnclosureCapacity` record with its binding
constraint, its stranded and headroom vectors, and an explanation, so a
disagreement can be traced to one enclosure.

## 8. Strong types and the exact-integer unit system

### Strong types

`include/capacity_observatory/strong.hpp` defines `Generation`, `Epoch`,
`Revision`, `Attempt`, and `SequenceNumber` as `StrongNumber<Tag, Rep>`:
distinct tag types over an integer, so interchanging an epoch and a generation is
a compile error. `co::next_number` is the checked successor; it refuses with
`ReasonCode::EpochExhausted` instead of wrapping, so a saturated counter can
never masquerade as a fresh one. `co::StrongId<Tag>` gives the same treatment to
site, hall, zone, row, enclosure, authority, evidence, mutation, and subject
identifiers, with `StrongId::parse` validating the text. `co::Digest` is a
32-byte SHA-256 value with hex parsing and rendering. `co::Timestamp` and
`co::Duration` are int64 nanoseconds since the Unix epoch and a signed
nanosecond span, with checked `elapsed` and `advanced`.

### Dimensions and canonical units

`co::Dimension` has six members, each with exactly one canonical unit
(`src/dimension.cpp`):

| dimension | canonical unit |
|---|---|
| space | mm2 (square millimetre) |
| rack-units | U (rack unit) |
| power | mW (milliwatt) |
| cooling | mWth (milliwatt thermal) |
| weight | g (gram) |
| serviceability | service (one serviceable position) |

`co::Amount` is a signed int64 count of canonical units. It is signed because
unexplained residuals and headroom deltas are legitimately negative; evidence
magnitudes themselves are non-negative and `validate()` refuses a negative one
with `ReasonCode::NegativeAmount`.

### Units are exact rationals, and conversion is refused rather than rounded

`co::Unit` is a dimension plus a reduced positive rational
(numerator/denominator). `Unit::make` divides both by their gcd and refuses a
non-positive factor; `Unit::parse` accepts only the declared table:

| dimension | units (exact multiple of the canonical unit) |
|---|---|
| space | mm2 1, m2 1000000, ft2 9290304/100 |
| rack-units | U 1 |
| power | mW 1, W 1000, kW 1000000, MW 1000000000 |
| cooling | mWth 1, Wth 1000, kWth 1000000, MWth 1000000000 |
| weight | g 1, kg 1000, t 1000000, lb 45359237/100000 |
| serviceability | service 1 |

`Unit::to_canonical` calls `co::scale_exact` (`src/dimension.cpp`), which

* reduces the scale factors by their gcd first,
* refuses with `ReasonCode::InexactScale` when the value is not an exact
  integer multiple, and
* divides before multiplying so that a representable quotient is not lost to an
  intermediate overflow, and refuses with `ReasonCode::Overflow` when the exact
  result is not representable.

Conversion is refused rather than rounded because a rounded magnitude would
silently become wrong capacity accounting: a 1 lb record would become 454 g and
the error would then propagate into every identity, residual, and fragmentation
figure computed from it. A refusal is a stable, explainable outcome
(`inexact-scale`) that a caller can fix by declaring in a unit that is exact or
by supplying a magnitude that is; a rounding error is invisible. The same rule is
why `Unit::symbol()` returns a synthesised `mm2*3/7`-style text for a unit
that is exact but not in the table.

### Checked arithmetic

`checked_add`, `checked_sub`, `checked_mul`, `checked_div`,
`checked_negate`, `floor_div`, and `checked_sum` detect overflow,
underflow, division by zero, and the unrepresentable negation of the minimum
value. `floor_div` is correct for negative dividends (a plain `/` is not).
`checked_sum` reports how many inputs were unknown rather than treating them as
zero. `co::AmountVector` (one optional amount per dimension) accumulates
all-or-nothing: a failed `accumulate` leaves the vector unchanged, and unknown
components stay unknown.

### Outcome types

Domain outcomes are values, not exceptions: `co::Status` carries a stable
`co::ReasonCode` and a human-readable detail, `co::Result<T>` is value or
status, and `co::reason_class` classifies every code as Ok, Refused,
Indeterminate, or Failed so a caller can decide whether to retry, refuse, or
fail without string matching. `co::fatal_status` aborts only for invariant
violations and for test or CLI paths where a failure means the harness itself is
broken.

## 9. The unknown-versus-zero rule

Unknown is `std::nullopt` and is never zero.

* An unknown amount is never written as zero anywhere in the ledger: `sum_of`
  and `subtract_all` (`src/ledger.cpp`) return unknown when any input is
  unknown.
* An unknown input makes its closure check `indeterminate`, which makes the line
  `Partial` (or `Indeterminate` when nothing can be derived), and counts
  toward `unknown_classes`.
* The reason codes say which figure is unknown rather than zero:
  `UnknownNameplate`, `UnknownInstalled`, `UnknownCommitted`,
  `UnknownReserved`, `UnknownExclusions`, `AmountUnknown`,
  `UndeterminedCapacity`.
* JSON rendering emits an explicit null for an unknown amount
  (`optional_amount_json` in `src/snapshot.cpp`), and the snapshot omits the
  fragmentation section entirely when topology or profile is absent, instead of
  reporting zero racks.
* A dimension that a rack profile does not demand must be left unknown; a
  present zero would be a positive requirement of zero, and is refused.
* An enclosure that does not declare free capacity in a demanded dimension is
  indeterminate, not empty.
* A declared zero is different from unknown, and is a known value: a class
  asserted as `0` is present with a zero amount and participates in the
  identities (the property suite in `tests/test_property_conservation.cpp`
  exercises exactly this distinction).
* Freshness follows the same rule: no observation instant yields
  `Freshness::Unknown`, never Fresh.

## 10. Real, synthetic, and unproven claims

The system labels what it knows and refuses to imply more.

| claim | status |
|---|---|
| ledger composition, closure checks, acceptance rules, unit arithmetic, hashing, JSON, persistence | REAL: implemented and measured on this machine (see `bench/bench_main.cpp`) |
| installed, committed, reserved, policy, and disputed evidence | REAL only when the named authority actually published it; the observatory neither checks nor can check that |
| plant, DCIM, telemetry, and economic evidence | SYNTHETIC unless a deployment attaches real plant data; the standard registry marks `plant` and `economics` with `synthetic_source = true`, and every snapshot carries `SnapshotProvenance::plant_evidence_note` saying so |
| recovered evidence | REAL persistence, but recovery is not observation: it never becomes Fresh by being recovered (`Freshness::Recovered`) |
| power and cooling figures | exact integer capacity assertions (mW, mWth), not electrical measurements taken by this process |
| network transport, multi-node coordination, BMS/DCIM protocols | UNSUPPORTED: no such code exists in this tree |
| POSIX durability behaviour | compiled (`#else` branches in `src/platform.cpp`) but not validated on this machine, which is Windows/MSVC |

## 11. Source map

| file | responsibility |
|---|---|
| `include/capacity_observatory/reason.hpp` | the stable reason-code vocabulary and its classification |
| `include/capacity_observatory/result.hpp` | Status and Result |
| `include/capacity_observatory/strong.hpp` `src/core.cpp` | strong numbers, identifiers, digests, time, freshness |
| `include/capacity_observatory/dimension.hpp` `src/dimension.cpp` | dimensions, amounts, units, checked arithmetic |
| `include/capacity_observatory/hash.hpp` `src/hash.cpp` | SHA-256 and CRC-32C |
| `include/capacity_observatory/evidence.hpp` `src/evidence.cpp` | scope paths, assertions, authority registry, evidence records |
| `include/capacity_observatory/watermark.hpp` `src/watermark.cpp` | slots, acceptance, watermarks, refusal audit |
| `include/capacity_observatory/ledger.hpp` `src/ledger.cpp` | declared classes, derived quantities, closure identities |
| `include/capacity_observatory/topology.hpp` `src/topology.cpp` | enclosure budgets and narrowing |
| `include/capacity_observatory/fragmentation.hpp` `src/fragmentation.cpp` | placement reality, stranding, binding constraints |
| `include/capacity_observatory/io.hpp` `src/platform.cpp` | files, durability points, atomic publication, single-writer lock |
| `include/capacity_observatory/store.hpp` `src/store_file.cpp` | framed evidence log, recovery, epochs, snapshots |
| `include/capacity_observatory/ingest.hpp` `src/ingest.cpp` | the only path into derived state, durable-first |
| `include/capacity_observatory/snapshot.hpp` `src/snapshot.cpp` | snapshot assembly and deterministic rendering |
| `include/capacity_observatory/concurrency.hpp` `src/concurrency.cpp` | synchronization, queues, workers, publication (see CONCURRENCY.md) |
| `include/capacity_observatory/json.hpp` `src/json.cpp` | strict parser and deterministic writer |

See `docs/PERSISTENCE.md` for the on-disk format and recovery algorithm, and
`docs/CONCURRENCY.md` for the ownership rules and the hazard audit.
