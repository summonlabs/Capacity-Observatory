# Changelog

All notable changes to Capacity Observatory are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026

Initial release.

### Added

- Exact, checked, dimension-aware capacity accounting over space, rack units, power,
  cooling, weight, and serviceability, in canonical integer units, with explicit
  unknown propagation and no missing-to-zero conversion.
- Evidence model with authority contracts: an evidence record occupies the slot
  `(authority, scope, dimension, capacity class)`, and an authority may only assert
  the classes its registered contract grants.
- Acceptance window with epochs, per-slot generation watermarks, revision, validity
  windows, content digests, duplicate/idempotent handling, conflict detection, and a
  bounded refusal audit trail.
- Explainable ledger with declared classes, derived classes, four closure identities,
  signed unexplained residuals, over-commitment detection, per-line freshness, and
  the evidence ids behind every number.
- Fragmentation, stranding, and binding-constraint analysis with a proven invariant
  between realizable placement and the aggregate bound.
- Durable, versioned, integrity-checked evidence store: CRC-32C framed log, magic and
  version header, exact commit point, conservative torn-tail recovery, interior
  corruption rejection, atomic snapshot and epoch publication, kernel-enforced
  single-writer lock, durable mutation identity, and monotonic session epochs.
- Ingest pipeline with durability-first ordering, a bounded queue, and explicit
  window access policies for single-threaded and concurrently read windows.
- Snapshot assembly and deterministic JSON and text rendering with provenance,
  refusal summaries, and no non-deterministic output.
- Command line interface: `ingest`, `explain`, `closure`, `fragmentation`,
  `verify`, `snapshot`, `recover`, `version`, `help` with deterministic exit codes.
- CMake package with install rules, exported targets, and a package config file, plus
  an independent out-of-tree `find_package` consumer.
- Test suites: foundations, JSON, ledger closure property tests against an independent
  reference model, fragmentation against a brute-force reference, adversarial input
  handling, persistence and failure injection, real multiprocess and restart proofs,
  concurrency hazard proofs, and end-to-end CLI tests.
- Benchmarks that report measured work labelled REAL, SYNTHETIC, or UNSUPPORTED.

### Notes

- Plant, DCIM, telemetry, and economic evidence is modelled as SYNTHETIC: no real
  facility hardware or plant data is attached to this repository.
- No electrical, cooling, multi-node, or hardware-in-the-loop proof is claimed.
