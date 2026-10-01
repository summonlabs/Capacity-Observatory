#pragma once
// Capacity Observatory - evidence acceptance, watermarks, and freshness.
//
// Acceptance is a pure decision: an evidence record either advances the current
// record of its slot, or it is refused with a stable reason. Refusals never
// change derived state; they are retained, bounded, for explanation.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/strong.hpp"
#include "capacity_observatory/time.hpp"

namespace co {

// The slot a piece of evidence occupies: authority, scope, dimension, assertion.
struct EvidenceSlotKey {
  AuthorityId authority;
  ScopePath scope;
  Dimension dimension{Dimension::Power};
  CapacityAssertion assertion{CapacityAssertion::Installed};

  [[nodiscard]] std::string text() const;

  friend bool operator==(const EvidenceSlotKey& a, const EvidenceSlotKey& b) noexcept {
    return a.authority == b.authority && a.scope == b.scope && a.dimension == b.dimension &&
           a.assertion == b.assertion;
  }
  friend bool operator<(const EvidenceSlotKey& a, const EvidenceSlotKey& b) noexcept;
};

enum class AcceptanceOutcome : std::uint8_t {
  Accepted = 0,          // becomes the current record of its slot
  Duplicate = 1,         // identical generation and digest: idempotent, no state change
  Conflicted = 2,        // same generation, different content: refused, both retained
  StaleGeneration = 3,   // older generation in the same epoch
  StaleEpoch = 4,        // older epoch
  Refused = 5,           // outside the authority's contract, or otherwise not acceptable
  Invalid = 6            // structurally invalid
};

[[nodiscard]] std::string_view acceptance_outcome_text(AcceptanceOutcome outcome) noexcept;

struct AcceptanceDecision {
  AcceptanceOutcome outcome{AcceptanceOutcome::Invalid};
  ReasonCode reason{ReasonCode::InternalError};
  std::string explanation;
  Freshness freshness{Freshness::Unknown};
  bool affects_derived_state{false};
  bool generation_gap{false};
  bool epoch_advanced{false};

  [[nodiscard]] bool accepted() const noexcept { return outcome == AcceptanceOutcome::Accepted; }
};

// Bounded audit trail of everything that was not accepted into derived state.
struct RefusedEvidence {
  EvidenceRecord record;
  AcceptanceDecision decision;
};

struct Watermark {
  Epoch epoch;
  Generation generation;
  Digest digest;
  bool present{false};
};

// Thread confinement: an EvidenceWindow owns mutable maps and is deliberately
// NOT internally synchronized. It must be confined to one thread at a time.
// IngestPipeline provides that confinement by doing all window mutation on its
// single worker thread; callers that need concurrent readers must place the
// window inside a Synchronized<EvidenceWindow> and take a read guard around
// snapshot composition and a write guard around consideration.
class EvidenceWindow {
 public:
  // Refusals retained per window; further refusals are counted but not stored.
  static constexpr std::size_t kDefaultRefusalCapacity = 4096;

  EvidenceWindow(const AuthorityRegistry& registry, std::shared_ptr<Clock> clock);
  EvidenceWindow(const AuthorityRegistry& registry, std::shared_ptr<Clock> clock, std::size_t refusal_capacity);

  // Evaluates and, when accepted, applies the record. Applying a newer record
  // replaces the previous current record of the same slot.
  [[nodiscard]] AcceptanceDecision consider(const EvidenceRecord& record);
  // Re-evaluates freshness of already accepted records at the current instant
  // without mutating watermarks. Used by snapshots so that freshness is always
  // evaluated against the snapshot instant rather than an ingest instant.
  [[nodiscard]] Freshness freshness_of(const EvidenceRecord& record) const;

  [[nodiscard]] const AuthorityRegistry& registry() const noexcept { return *registry_; }
  [[nodiscard]] const std::shared_ptr<Clock>& clock() const noexcept { return clock_; }

  // Current records, ordered by slot key for deterministic iteration.
  [[nodiscard]] const std::map<EvidenceSlotKey, EvidenceRecord>& current() const noexcept { return current_; }
  [[nodiscard]] const std::vector<RefusedEvidence>& refusals() const noexcept { return refusals_; }
  [[nodiscard]] std::size_t dropped_refusals() const noexcept { return dropped_refusals_; }
  [[nodiscard]] std::size_t accepted_count() const noexcept { return accepted_count_; }

  [[nodiscard]] const Watermark* watermark_for(const EvidenceSlotKey& key) const noexcept;

  // Marks every current record as recovered: used after a durable reopen. The
  // records stay current for explanation but are never promoted to fresh.
  void mark_all_recovered(Timestamp recovery_instant);
  void clear();

  [[nodiscard]] std::size_t size() const noexcept { return current_.size(); }

 private:
  const AuthorityRegistry* registry_;
  std::shared_ptr<Clock> clock_;
  std::map<EvidenceSlotKey, EvidenceRecord> current_;
  std::map<EvidenceSlotKey, Watermark> watermarks_;
  std::vector<RefusedEvidence> refusals_;
  std::size_t refusal_capacity_;
  std::size_t dropped_refusals_{0};
  std::size_t accepted_count_{0};
};

}  // namespace co
