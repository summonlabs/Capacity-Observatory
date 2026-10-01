#pragma once
// Capacity Observatory - derived snapshot assembly and rendering.
//
// A snapshot is the observable answer to the core question: which classes hold
// how much capacity, which authority evidence explains every residual, and how
// fresh and how authoritative that evidence is. Rendering is deterministic:
// identical inputs produce byte identical output.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/fragmentation.hpp"
#include "capacity_observatory/json.hpp"
#include "capacity_observatory/ledger.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/store.hpp"
#include "capacity_observatory/time.hpp"
#include "capacity_observatory/topology.hpp"
#include "capacity_observatory/watermark.hpp"

namespace co {

struct SnapshotProvenance {
  Timestamp generated_at;
  Epoch session_epoch;
  std::string store_directory;
  bool store_attached{false};
  bool recovered_only{false};
  std::size_t evidence_records{0};
  std::size_t accepted_total{0};
  std::size_t refused_records{0};
  std::size_t dropped_refusals{0};
  std::size_t durable_mutations{0};
  std::size_t synthetic_sources{0};
  std::string plant_evidence_note;
};

struct Snapshot {
  ScopePath scope;
  Timestamp generated_at;
  Ledger ledger;
  std::optional<FragmentationReport> fragmentation;
  SnapshotProvenance provenance;
  std::vector<std::string> refused_summary;
  std::vector<std::string> explanations;
};

// Builds the snapshot. 'topology' and 'profile' are optional: when both are
// present the fragmentation section is included, otherwise it is absent rather
// than reported as zero.
[[nodiscard]] Result<Snapshot> build_snapshot(const EvidenceWindow& window,
                                             const ScopePath& scope,
                                             DimensionSet dimensions,
                                             const Topology* topology,
                                             const RackProfile* profile,
                                             const Store* store);

[[nodiscard]] json::Value snapshot_to_json(const Snapshot& snapshot);

// Deterministic human readable rendering used by the CLI's default output.
[[nodiscard]] std::string render_snapshot_text(const Snapshot& snapshot);

// Renders the refusal audit trail, newest last, bounded by 'limit'.
[[nodiscard]] std::vector<std::string> summarize_refusals(const EvidenceWindow& window, std::size_t limit);

}  // namespace co
