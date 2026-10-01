#pragma once
// Capacity Observatory - evidence records and the adjacent authority registry.
//
// The observatory never mutates adjacent authority state. It consumes immutable
// evidence records that name the authority that asserted them, the scope and
// dimension they describe, and the capacity class they assert. An authority may
// only assert the classes its contract grants: visibility of another runtime's
// evidence never transfers its authority.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/strong.hpp"
#include "capacity_observatory/time.hpp"

namespace co {

// ---------------------------------------------------------------------------
// Scope paths: site / hall / zone / row / enclosure, outermost first.
// ---------------------------------------------------------------------------
enum class ScopeKind : std::uint8_t { Site = 0, Hall = 1, Zone = 2, Row = 3, Enclosure = 4 };

[[nodiscard]] std::string_view scope_kind_text(ScopeKind kind) noexcept;
[[nodiscard]] Result<ScopeKind> scope_kind_from_text(std::string_view text);

struct ScopeSegment {
  ScopeKind kind{ScopeKind::Site};
  std::string name;

  [[nodiscard]] static Result<ScopeSegment> parse(std::string_view text);
  [[nodiscard]] std::string text() const;

  friend bool operator==(const ScopeSegment& a, const ScopeSegment& b) noexcept {
    return a.kind == b.kind && a.name == b.name;
  }
  friend bool operator<(const ScopeSegment& a, const ScopeSegment& b) noexcept {
    return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
  }
};

class ScopePath {
 public:
  ScopePath() = default;
  explicit ScopePath(std::vector<ScopeSegment> segments) : segments_(std::move(segments)) {}

  // Parses "site=dc1/hall=h1/zone=z1". Segments must appear in outermost-first
  // order and may not repeat a kind.
  [[nodiscard]] static Result<ScopePath> parse(std::string_view text);
  [[nodiscard]] static Result<ScopePath> of(std::initializer_list<ScopeSegment> segments);

  [[nodiscard]] const std::vector<ScopeSegment>& segments() const noexcept { return segments_; }
  [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }
  [[nodiscard]] std::size_t depth() const noexcept { return segments_.size(); }
  [[nodiscard]] const ScopeSegment& back() const noexcept { return segments_.back(); }

  // True when 'this' is an ancestor of, or equal to, 'other'.
  [[nodiscard]] bool is_prefix_of(const ScopePath& other) const noexcept;
  [[nodiscard]] ScopePath parent() const;
  [[nodiscard]] std::string text() const;

  friend bool operator==(const ScopePath& a, const ScopePath& b) noexcept { return a.segments_ == b.segments_; }
  friend bool operator<(const ScopePath& a, const ScopePath& b) noexcept { return a.segments_ < b.segments_; }

 private:
  std::vector<ScopeSegment> segments_;
};

// ---------------------------------------------------------------------------
// Capacity classes. This single enumeration is the ledger's vocabulary; the
// specification's planned/reserved/installed/observed/available lifecycle
// distinctions are preserved as the Planned/Reserved/Installed/Observed/
// Available members rather than as a second redundant field.
// ---------------------------------------------------------------------------
enum class CapacityAssertion : std::uint8_t {
  Nameplate = 0,             // commissioned/bounded ceiling for the scope
  Governed = 1,              // policy declared allocatable ceiling
  Planned = 2,               // planned, not yet installed
  Installed = 3,             // physically installed
  Observed = 4,              // measured by plant telemetry (synthetic source unless real plant exists)
  Reserved = 5,              // reserved but not committed
  Committed = 6,             // committed to a workload
  Available = 7,             // declared available by an authority
  Stranded = 8,              // declared structurally unusable
  Disputed = 9,              // declared as under dispute between authorities
  ExcludedPolicy = 10,
  ExcludedMaintenance = 11,
  ExcludedFailure = 12,
  OperationalReserve = 13
};

inline constexpr std::size_t kAssertionCount = 14;

[[nodiscard]] std::string_view assertion_text(CapacityAssertion assertion) noexcept;
[[nodiscard]] Result<CapacityAssertion> assertion_from_text(std::string_view text);
[[nodiscard]] constexpr std::size_t assertion_index(CapacityAssertion assertion) noexcept {
  return static_cast<std::size_t>(assertion);
}

// ---------------------------------------------------------------------------
// Authority contracts
// ---------------------------------------------------------------------------
enum class AuthorityRole : std::uint8_t {
  CommittedCapacity = 0,   // owns committed capacity and entitlements
  Reservations = 1,        // owns reservations and admission
  InstalledInventory = 2,  // owns the installed physical inventory
  Policy = 3,              // owns nameplate, exclusions, and operational reserve
  PlantTelemetry = 4,      // plant/DCIM telemetry (synthetic unless real plant exists)
  Economics = 5,           // economic evidence; carries no capacity authority here
  Arbitration = 6          // resolves disputed capacity
};

[[nodiscard]] std::string_view authority_role_text(AuthorityRole role) noexcept;

class AssertionSet {
 public:
  constexpr AssertionSet() noexcept = default;
  [[nodiscard]] static constexpr AssertionSet of(CapacityAssertion assertion) noexcept {
    AssertionSet set;
    set.mask_ = static_cast<std::uint16_t>(1u << assertion_index(assertion));
    return set;
  }
  [[nodiscard]] static constexpr AssertionSet all() noexcept {
    AssertionSet set;
    set.mask_ = 0x3FFF;
    return set;
  }
  constexpr AssertionSet& add(CapacityAssertion assertion) noexcept {
    mask_ = static_cast<std::uint16_t>(mask_ | (1u << assertion_index(assertion)));
    return *this;
  }
  [[nodiscard]] constexpr bool contains(CapacityAssertion assertion) const noexcept {
    return (mask_ & static_cast<std::uint16_t>(1u << assertion_index(assertion))) != 0;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return mask_ == 0; }
  [[nodiscard]] constexpr std::uint16_t mask() const noexcept { return mask_; }
  [[nodiscard]] std::string text() const;

  friend constexpr bool operator==(AssertionSet a, AssertionSet b) noexcept { return a.mask_ == b.mask_; }

 private:
  std::uint16_t mask_{0};
};

struct AuthorityContract {
  AuthorityId id;
  AuthorityRole role{AuthorityRole::Economics};
  DimensionSet dimensions = DimensionSet::all();
  AssertionSet assertions;
  // Zero means no staleness budget is declared for this authority.
  Duration staleness_budget{};
  // True only when the evidence source is modelled rather than measured.
  bool synthetic_source{false};

  [[nodiscard]] bool accepts(Dimension dimension, CapacityAssertion assertion) const noexcept {
    return dimensions.contains(dimension) && assertions.contains(assertion);
  }
};

class AuthorityRegistry {
 public:
  // The registry used by the CLI and by the tests: dccp, asi, dfi, policy,
  // plant (synthetic), economics (no capacity assertions), arbiter.
  [[nodiscard]] static AuthorityRegistry standard();

  [[nodiscard]] Result<void> add(AuthorityContract contract);
  [[nodiscard]] const AuthorityContract* find(const AuthorityId& id) const noexcept;
  [[nodiscard]] const std::vector<AuthorityContract>& contracts() const noexcept { return contracts_; }
  [[nodiscard]] std::size_t size() const noexcept { return contracts_.size(); }

 private:
  std::vector<AuthorityContract> contracts_;
};

// ---------------------------------------------------------------------------
// Evidence records
// ---------------------------------------------------------------------------
enum class Provenance : std::uint8_t {
  Observed = 0,   // received directly from the authority in this process lifetime
  Recovered = 1,  // restored from durable storage; recovery is not observation
  Synthetic = 2   // modelled source, labelled as such end to end
};

[[nodiscard]] std::string_view provenance_text(Provenance provenance) noexcept;

struct ValidityWindow {
  std::optional<Timestamp> from;
  std::optional<Timestamp> until;

  // A closed window is inclusive of 'until'.
  [[nodiscard]] bool contains(Timestamp instant) const noexcept;
  [[nodiscard]] bool is_open_ended() const noexcept { return !from.has_value() && !until.has_value(); }
};

class EvidenceRecord {
 public:
  // Identity of the slot this evidence occupies. Refused and superseded records
  // are retained for explanation but only the current record of a slot drives
  // derived state.
  AuthorityId authority;
  AuthorityRole role{AuthorityRole::Economics};
  ScopePath scope;
  Dimension dimension{Dimension::Power};
  CapacityAssertion assertion{CapacityAssertion::Installed};
  Unit unit = Unit::canonical(Dimension::Power);
  std::int64_t declared_amount{0};   // magnitude as declared by the authority
  Amount amount;                     // exact canonical amount
  Generation generation;
  Epoch epoch;
  Revision revision;
  Provenance provenance{Provenance::Observed};
  ValidityWindow validity;
  bool has_observed_at{false};
  Timestamp observed_at{};
  // Free-form, deterministic provenance note, e.g. "dccp-2026-01.jsonl:7".
  std::string source;

  [[nodiscard]] static Result<EvidenceRecord> make(AuthorityId authority,
                                                   AuthorityRole role,
                                                   ScopePath scope,
                                                   Dimension dimension,
                                                   CapacityAssertion assertion,
                                                   Unit unit,
                                                   std::int64_t declared_amount,
                                                   Generation generation,
                                                   Epoch epoch,
                                                   Revision revision);

  // Structural validation: identifier shapes, unit/dimension agreement, exact
  // conversion, non-negative magnitude, observed instant presence.
  [[nodiscard]] Result<void> validate() const;

  // Deterministic slot identity, e.g. "dccp|site=dc1/hall=h1|power|installed".
  [[nodiscard]] std::string slot_text() const;
  // Slot identity plus epoch and generation, e.g. "...|e1|g41".
  [[nodiscard]] std::string id_text() const;

  // SHA-256 over every field that changes the meaning of the record.
  [[nodiscard]] Digest content_digest() const;
};

}  // namespace co
