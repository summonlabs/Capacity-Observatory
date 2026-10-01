// Capacity Observatory - adversarial suite.
//
// Every case drives the real public API with a hostile input and asserts the
// exact ReasonCode, that derived state did not move, and - where the contract
// promises it - that the refusal is visible in the bounded audit trail.
#include "co_test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/ledger.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/time.hpp"
#include "capacity_observatory/watermark.hpp"

using namespace co;

namespace {

constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();
constexpr std::int64_t kNowNanos = 1700000000000000000LL;
constexpr std::int64_t kSecond = 1000000000LL;

std::array<CapacityAssertion, kAssertionCount> all_assertions() {
  std::array<CapacityAssertion, kAssertionCount> all{};
  for (std::size_t i = 0; i < kAssertionCount; ++i) {
    all[i] = static_cast<CapacityAssertion>(i);
  }
  return all;
}

ScopePath scope_of(std::initializer_list<ScopeSegment> segments) {
  const Result<ScopePath> parsed = ScopePath::of(segments);
  if (!parsed.ok()) {
    CO_FAIL("test scope construction failed: " + parsed.status().render());
  }
  return parsed.value();
}

ScopePath site_scope() { return scope_of({{ScopeKind::Site, "dc1"}}); }
ScopePath hall_scope() { return scope_of({{ScopeKind::Site, "dc1"}, {ScopeKind::Hall, "hall-a"}}); }

std::string amount_or_unknown(const std::optional<Amount>& value) {
  return value.has_value() ? std::to_string(value.value().canonical()) : std::string("unknown");
}

// Canonical text of every derived number of a ledger, used to prove that a
// refused, duplicated, or stale record did not move anything.
std::string dump(const Ledger& ledger) {
  std::string out;
  for (const LedgerLine& line : ledger.lines) {
    out += std::string(dimension_text(line.dimension));
    for (const CapacityAssertion assertion : all_assertions()) {
      out += "|";
      out += assertion_text(assertion);
      out += "=";
      out += amount_or_unknown(line.declared.get(assertion));
    }
    out += "|exclusion_total=" + amount_or_unknown(line.exclusion_total);
    out += "|serviceable=" + amount_or_unknown(line.serviceable);
    out += "|free_usable=" + amount_or_unknown(line.free_usable);
    out += "|governed=" + amount_or_unknown(line.governed);
    out += "|overcommitment=" + amount_or_unknown(line.overcommitment);
    out += std::string("|overcommitted=") + (line.overcommitted ? "1" : "0");
    out += "|state=" + std::string(line_state_text(line.state));
    out += "|unknown_classes=" + std::to_string(line.unknown_classes);
    out += "|evidence_count=" + std::to_string(line.evidence_count);
    out += "|freshness=" + std::string(freshness_text(line.worst_freshness));
    out += "\n";
  }
  return out;
}

LedgerRequest request_for(const ScopePath& scope) {
  LedgerRequest request;
  request.scope = scope;
  request.dimensions = DimensionSet::all();
  return request;
}

EvidenceRecord record_from(AuthorityId authority, AuthorityRole role, const ScopePath& scope, Dimension dimension,
                           CapacityAssertion assertion, const Unit& unit, std::int64_t declared,
                           std::uint64_t epoch, std::uint64_t generation) {
  const Result<EvidenceRecord> made = EvidenceRecord::make(authority, role, scope, dimension, assertion, unit,
                                                           declared, Generation(generation), Epoch(epoch), Revision(1));
  if (!made.ok()) {
    CO_FAIL("EvidenceRecord::make failed while building a test record: " + made.status().render());
  }
  EvidenceRecord record = made.value();
  record.has_observed_at = true;
  record.observed_at = Timestamp(kNowNanos);
  record.source = "test-adversarial";
  return record;
}

EvidenceRecord canonical_record(AuthorityId authority, AuthorityRole role, const ScopePath& scope,
                                Dimension dimension, CapacityAssertion assertion, std::int64_t amount,
                                std::uint64_t epoch, std::uint64_t generation) {
  return record_from(std::move(authority), role, scope, dimension, assertion, Unit::canonical(dimension), amount,
                     epoch, generation);
}

// The standard committed-capacity record: dccp owns committed capacity.
EvidenceRecord committed_record(const ScopePath& scope, std::int64_t amount, std::uint64_t epoch,
                                std::uint64_t generation) {
  return canonical_record(AuthorityId("dccp"), AuthorityRole::CommittedCapacity, scope, Dimension::Power,
                          CapacityAssertion::Committed, amount, epoch, generation);
}

void require_refused(const AcceptanceDecision& decision, ReasonCode reason, AcceptanceOutcome outcome,
                     const std::string& context) {
  co::test::note_assertion();
  if (decision.accepted()) {
    CO_FAIL(context + ": expected a refusal but the record was accepted");
  }
  if (decision.reason != reason) {
    CO_FAIL(context + ": expected " + std::string(reason_text(reason)) + " but observed " +
            std::string(reason_text(decision.reason)) + " (" + decision.explanation + ")");
  }
  if (decision.outcome != outcome) {
    CO_FAIL(context + ": expected outcome " + std::string(acceptance_outcome_text(outcome)) + " but observed " +
            std::string(acceptance_outcome_text(decision.outcome)));
  }
  if (decision.affects_derived_state) {
    CO_FAIL(context + ": a refusal must not affect derived state");
  }
}

// A window that never reads a wall clock, so freshness is deterministic.
struct Window {
  AuthorityRegistry registry{AuthorityRegistry::standard()};
  std::shared_ptr<Clock> clock{manual_clock(Timestamp(kNowNanos))};
  EvidenceWindow window{registry, clock};
};

}  // namespace

// ---------------------------------------------------------------------------
// Units, dimensions, and magnitudes
// ---------------------------------------------------------------------------
CO_TEST(unit_and_dimension_confusion_is_refused) {
  const ScopePath site = site_scope();

  // kg denotes weight; asking for it as a power unit is a dimension mismatch.
  CO_REQUIRE_ERR(Unit::parse_for(Dimension::Power, "kg"), ReasonCode::DimensionMismatch);
  CO_REQUIRE_ERR(Unit::parse_for(Dimension::Power, "furlong"), ReasonCode::UnknownUnit);
  CO_REQUIRE_ERR(Unit::parse("kg "), ReasonCode::UnknownUnit);
  CO_REQUIRE_OK(Unit::parse_for(Dimension::Weight, "kg"), kilograms);
  CO_REQUIRE_EQ(kilograms.to_canonical(2).value().canonical(), static_cast<std::int64_t>(2000));

  // ft2 is 9290304/100 mm2: exact for some magnitudes and refused for others,
  // never rounded.
  CO_REQUIRE_OK(Unit::parse_for(Dimension::Space, "ft2"), square_feet);
  CO_REQUIRE_ERR(square_feet.to_canonical(1), ReasonCode::InexactScale);
  CO_REQUIRE_ERR(square_feet.to_canonical(99), ReasonCode::InexactScale);
  CO_REQUIRE_OK(square_feet.to_canonical(100), exact_square_feet);
  CO_REQUIRE_EQ(exact_square_feet.canonical(), static_cast<std::int64_t>(9290304));

  const Unit kilograms_unit = Unit::parse("kg").value();
  const Unit space_unit = Unit::canonical(Dimension::Space);
  const Unit watts = Unit::parse("W").value();

  CO_REQUIRE_ERR(EvidenceRecord::make(AuthorityId("dfi"), AuthorityRole::InstalledInventory, site, Dimension::Power,
                                      CapacityAssertion::Installed, kilograms_unit, 5, Generation(1), Epoch(1),
                                      Revision(1)),
                 ReasonCode::DimensionMismatch);
  CO_REQUIRE_ERR(EvidenceRecord::make(AuthorityId("dfi"), AuthorityRole::InstalledInventory, site, Dimension::Power,
                                      CapacityAssertion::Installed, space_unit, 5, Generation(1), Epoch(1),
                                      Revision(1)),
                 ReasonCode::DimensionMismatch);
  CO_REQUIRE_ERR(EvidenceRecord::make(AuthorityId("dfi"), AuthorityRole::InstalledInventory, site, Dimension::Space,
                                      CapacityAssertion::Installed, square_feet, 1, Generation(1), Epoch(1),
                                      Revision(1)),
                 ReasonCode::InexactScale);

  // A magnitude that only exceeds the range after the unit scale is applied.
  CO_REQUIRE_ERR(EvidenceRecord::make(AuthorityId("dfi"), AuthorityRole::InstalledInventory, site, Dimension::Power,
                                      CapacityAssertion::Installed, Unit::parse("MW").value(), kMax, Generation(1),
                                      Epoch(1), Revision(1)),
                 ReasonCode::Overflow);
  CO_REQUIRE_OK(EvidenceRecord::make(AuthorityId("dfi"), AuthorityRole::InstalledInventory, site, Dimension::Power,
                                     CapacityAssertion::Installed, watts, 3, Generation(1), Epoch(1), Revision(1)),
                scaled);
  CO_REQUIRE_EQ(scaled.amount.canonical(), static_cast<std::int64_t>(3000));

  // A hand-built record whose unit belongs to another dimension must fail
  // structural validation rather than being accepted on a numeric coincidence.
  EvidenceRecord confused;
  confused.authority = AuthorityId("dfi");
  confused.role = AuthorityRole::InstalledInventory;
  confused.scope = site;
  confused.dimension = Dimension::Power;
  confused.assertion = CapacityAssertion::Installed;
  confused.unit = space_unit;
  confused.declared_amount = 5;
  confused.amount = Amount(5);
  confused.generation = Generation(1);
  confused.epoch = Epoch(1);
  CO_REQUIRE_ERR(confused.validate(), ReasonCode::DimensionMismatch);

  Window fixture;
  const AcceptanceDecision decision = fixture.window.consider(confused);
  require_refused(decision, ReasonCode::DimensionMismatch, AcceptanceOutcome::Invalid, "dimension confusion");
  CO_REQUIRE_EQ(fixture.window.size(), static_cast<std::size_t>(0));
  CO_REQUIRE_EQ(fixture.window.accepted_count(), static_cast<std::size_t>(0));
  CO_REQUIRE_EQ(fixture.window.refusals().size(), static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(fixture.window.refusals().front().decision.reason, ReasonCode::DimensionMismatch);
}

CO_TEST(negative_magnitudes_are_refused) {
  const ScopePath site = site_scope();
  const EvidenceRecord negative = canonical_record(AuthorityId("dfi"), AuthorityRole::InstalledInventory, site,
                                                   Dimension::Power, CapacityAssertion::Installed, -5, 1, 1);
  CO_REQUIRE_ERR(negative.validate(), ReasonCode::NegativeAmount);

  Window fixture;
  const AcceptanceDecision decision = fixture.window.consider(negative);
  require_refused(decision, ReasonCode::NegativeAmount, AcceptanceOutcome::Invalid, "negative magnitude");
  CO_REQUIRE_EQ(fixture.window.refusals().size(), static_cast<std::size_t>(1));

  CO_REQUIRE_ERR(non_negative(Amount(-1), "capacity"), ReasonCode::NegativeAmount);
  CO_REQUIRE_OK(non_negative(Amount(0), "capacity"), zero_ok);
  CO_REQUIRE_EQ(zero_ok.canonical(), static_cast<std::int64_t>(0));

  // A record whose declared amount disagrees with its canonical amount is
  // malformed even when both are positive.
  EvidenceRecord inconsistent = canonical_record(AuthorityId("dfi"), AuthorityRole::InstalledInventory, site,
                                                 Dimension::Power, CapacityAssertion::Installed, 10, 1, 1);
  inconsistent.amount = Amount(11);
  CO_REQUIRE_ERR(inconsistent.validate(), ReasonCode::MalformedEvidence);
}

// ---------------------------------------------------------------------------
// Epochs, generations, and digests
// ---------------------------------------------------------------------------
CO_TEST(stale_epoch_and_generation_are_refused_without_state_change) {
  const ScopePath site = site_scope();
  Window fixture;
  const LedgerRequest request = request_for(site);

  const EvidenceRecord current = committed_record(site, 500, 2, 10);
  const AcceptanceDecision accepted = fixture.window.consider(current);
  co::test::note_assertion();
  if (!accepted.accepted()) {
    CO_FAIL("the baseline record was not accepted: " + accepted.explanation);
  }
  const Result<Ledger> before = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(before, before_value);
  const std::string before_dump = dump(before_value);

  // Same epoch, older generation.
  const EvidenceRecord older_generation = committed_record(site, 999, 2, 9);
  require_refused(fixture.window.consider(older_generation), ReasonCode::StaleGeneration,
                  AcceptanceOutcome::StaleGeneration, "stale generation");

  // Older epoch, newer generation: the epoch watermark dominates.
  const EvidenceRecord older_epoch = committed_record(site, 999, 1, 11);
  require_refused(fixture.window.consider(older_epoch), ReasonCode::StaleEpoch, AcceptanceOutcome::StaleEpoch,
                  "stale epoch");

  CO_REQUIRE_EQ(fixture.window.size(), static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(fixture.window.accepted_count(), static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(fixture.window.refusals().size(), static_cast<std::size_t>(2));
  CO_REQUIRE_EQ(fixture.window.refusals()[0].decision.reason, ReasonCode::StaleGeneration);
  CO_REQUIRE_EQ(fixture.window.refusals()[1].decision.reason, ReasonCode::StaleEpoch);
  CO_REQUIRE_EQ(fixture.window.refusals()[0].record.amount.canonical(), static_cast<std::int64_t>(999));

  const Result<Ledger> after = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(after, after_value);
  CO_REQUIRE_EQ(dump(after_value), before_dump);

  // A superseding epoch resets the generation watermark and is accepted.
  const EvidenceRecord new_epoch = committed_record(site, 700, 3, 1);
  const AcceptanceDecision advanced = fixture.window.consider(new_epoch);
  co::test::note_assertion();
  if (!advanced.accepted() || !advanced.epoch_advanced) {
    CO_FAIL("a newer epoch must be accepted and reported as an epoch advance");
  }
  const Result<Ledger> final_ledger = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(final_ledger, final_value);
  const LedgerLine* line = final_value.find(Dimension::Power);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Committed).value().canonical(),
                static_cast<std::int64_t>(700));
}

CO_TEST(conflicting_and_duplicate_generations_are_distinguished) {
  const ScopePath site = site_scope();
  Window fixture;
  const LedgerRequest request = request_for(site);

  const EvidenceRecord original = committed_record(site, 100, 1, 5);
  const AcceptanceDecision accepted = fixture.window.consider(original);
  co::test::note_assertion();
  if (!accepted.accepted()) {
    CO_FAIL("the baseline record was not accepted: " + accepted.explanation);
  }
  const Result<Ledger> before = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(before, before_value);
  const std::string before_dump = dump(before_value);

  // Identical digest: idempotent, not an error, not a refusal.
  const AcceptanceDecision duplicate = fixture.window.consider(original);
  CO_REQUIRE(duplicate.outcome == AcceptanceOutcome::Duplicate);
  CO_REQUIRE_EQ(duplicate.reason, ReasonCode::DuplicateEvidence);
  CO_REQUIRE(!duplicate.affects_derived_state);
  CO_REQUIRE_EQ(fixture.window.refusals().size(), static_cast<std::size_t>(0));
  CO_REQUIRE_EQ(fixture.window.accepted_count(), static_cast<std::size_t>(1));

  // Same epoch and generation, different content: a conflict, never a silent
  // overwrite of the accepted truth.
  const EvidenceRecord conflict = committed_record(site, 101, 1, 5);
  CO_REQUIRE(conflict.content_digest() != original.content_digest());
  require_refused(fixture.window.consider(conflict), ReasonCode::ConflictingEvidence,
                  AcceptanceOutcome::Conflicted, "conflicting generation");
  CO_REQUIRE_EQ(fixture.window.refusals().size(), static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(fixture.window.refusals().front().record.amount.canonical(), static_cast<std::int64_t>(101));

  const Result<Ledger> after = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(after, after_value);
  CO_REQUIRE_EQ(dump(after_value), before_dump);
  const LedgerLine* line = after_value.find(Dimension::Power);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Committed).value().canonical(),
                static_cast<std::int64_t>(100));
}

// ---------------------------------------------------------------------------
// Authority contracts
// ---------------------------------------------------------------------------
CO_TEST(an_authority_cannot_assert_outside_its_contract) {
  const ScopePath site = site_scope();
  Window fixture;
  const LedgerRequest request = request_for(site);

  // dccp owns committed capacity and is visible first.
  const EvidenceRecord committed = committed_record(site, 400, 1, 1);
  const AcceptanceDecision accepted = fixture.window.consider(committed);
  co::test::note_assertion();
  if (!accepted.accepted()) {
    CO_FAIL("the baseline record was not accepted: " + accepted.explanation);
  }
  const Result<Ledger> before = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(before, before_value);
  const std::string before_dump = dump(before_value);

  // The economics authority holds no capacity assertions at all.
  const EvidenceRecord economic_claim = canonical_record(AuthorityId("economics"), AuthorityRole::Economics, site,
                                                        Dimension::Power, CapacityAssertion::Installed, 12345, 1, 1);
  require_refused(fixture.window.consider(economic_claim), ReasonCode::EvidenceRefused,
                  AcceptanceOutcome::Refused, "economics asserting installed");

  // dccp may not speak for reservations; asi may not speak for installed.
  const EvidenceRecord dccp_reservation = canonical_record(AuthorityId("dccp"), AuthorityRole::CommittedCapacity,
                                                           site, Dimension::Power, CapacityAssertion::Reserved, 7, 1, 1);
  require_refused(fixture.window.consider(dccp_reservation), ReasonCode::EvidenceRefused,
                  AcceptanceOutcome::Refused, "dccp asserting reserved");
  const EvidenceRecord asi_installed = canonical_record(AuthorityId("asi"), AuthorityRole::Reservations, site,
                                                        Dimension::Power, CapacityAssertion::Installed, 7, 1, 1);
  require_refused(fixture.window.consider(asi_installed), ReasonCode::EvidenceRefused,
                  AcceptanceOutcome::Refused, "asi asserting installed");
  // policy owns the exclusions but not the committed inventory.
  const EvidenceRecord policy_committed = canonical_record(AuthorityId("policy"), AuthorityRole::Policy, site,
                                                           Dimension::Power, CapacityAssertion::Committed, 7, 1, 1);
  require_refused(fixture.window.consider(policy_committed), ReasonCode::EvidenceRefused,
                  AcceptanceOutcome::Refused, "policy asserting committed");

  // Visibility never transfers authority: the refused records are retained for
  // explanation, the slot map is untouched, and nothing moved.
  CO_REQUIRE_EQ(fixture.window.size(), static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(fixture.window.accepted_count(), static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(fixture.window.refusals().size(), static_cast<std::size_t>(4));
  for (const RefusedEvidence& refused : fixture.window.refusals()) {
    CO_REQUIRE(!refused.decision.affects_derived_state);
    CO_REQUIRE(!refused.decision.accepted());
    CO_REQUIRE(!refused.decision.explanation.empty());
  }
  EvidenceSlotKey economic_slot;
  economic_slot.authority = AuthorityId("economics");
  economic_slot.scope = site;
  economic_slot.dimension = Dimension::Power;
  economic_slot.assertion = CapacityAssertion::Installed;
  CO_REQUIRE(fixture.window.watermark_for(economic_slot) == nullptr);

  const Result<Ledger> after = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(after, after_value);
  CO_REQUIRE_EQ(dump(after_value), before_dump);
  const LedgerLine* line = after_value.find(Dimension::Power);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE(!line->declared.get(CapacityAssertion::Installed).has_value());
  CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Committed).value().canonical(),
                static_cast<std::int64_t>(400));
}

CO_TEST(an_unknown_authority_is_refused) {
  const ScopePath site = site_scope();
  Window fixture;
  const EvidenceRecord rogue = canonical_record(AuthorityId("rogue"), AuthorityRole::InstalledInventory, site,
                                                Dimension::Power, CapacityAssertion::Installed, 900, 1, 1);
  require_refused(fixture.window.consider(rogue), ReasonCode::UnknownAuthority, AcceptanceOutcome::Refused,
                  "unknown authority");
  CO_REQUIRE_EQ(fixture.window.size(), static_cast<std::size_t>(0));
  CO_REQUIRE_EQ(fixture.window.refusals().size(), static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(fixture.window.refusals().front().record.authority.value(), std::string("rogue"));

  const Result<Ledger> ledger = compose_ledger(fixture.window, request_for(site));
  CO_REQUIRE_OK(ledger, value);
  const LedgerLine* line = value.find(Dimension::Power);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("an empty window must still produce a line");
  }
  CO_REQUIRE(line->state == LineState::Indeterminate);
}

// ---------------------------------------------------------------------------
// Validity windows and freshness
// ---------------------------------------------------------------------------
CO_TEST(validity_windows_are_enforced_at_both_ends) {
  const ScopePath site = site_scope();
  Window fixture;

  EvidenceRecord future = committed_record(site, 10, 1, 1);
  future.validity.from = Timestamp(kNowNanos + kSecond);
  require_refused(fixture.window.consider(future), ReasonCode::EvidenceNotYetValid, AcceptanceOutcome::Refused,
                  "window not yet open");

  EvidenceRecord expired = committed_record(site, 10, 1, 2);
  expired.validity.until = Timestamp(kNowNanos - kSecond);
  require_refused(fixture.window.consider(expired), ReasonCode::EvidenceExpired, AcceptanceOutcome::Refused,
                  "window closed");

  EvidenceRecord inverted = committed_record(site, 10, 1, 3);
  inverted.validity.from = Timestamp(kNowNanos + kSecond);
  inverted.validity.until = Timestamp(kNowNanos);
  require_refused(fixture.window.consider(inverted), ReasonCode::MalformedEvidence, AcceptanceOutcome::Invalid,
                  "window closes before it opens");

  // The closed end is inclusive, and the open end is inclusive too.
  EvidenceRecord boundary = committed_record(site, 11, 1, 4);
  boundary.validity.from = Timestamp(kNowNanos);
  boundary.validity.until = Timestamp(kNowNanos);
  const AcceptanceDecision accepted = fixture.window.consider(boundary);
  co::test::note_assertion();
  if (!accepted.accepted()) {
    CO_FAIL("a validity window that closes exactly at the evaluation instant must accept: " + accepted.explanation);
  }
  CO_REQUIRE_EQ(fixture.window.refusals().size(), static_cast<std::size_t>(3));
  CO_REQUIRE(fixture.window.refusals()[0].decision.reason == ReasonCode::EvidenceNotYetValid);
  CO_REQUIRE(fixture.window.refusals()[1].decision.reason == ReasonCode::EvidenceExpired);
  CO_REQUIRE(fixture.window.refusals()[2].decision.reason == ReasonCode::MalformedEvidence);
}

CO_TEST(stale_evidence_is_reported_and_never_promoted_to_fresh) {
  const ScopePath site = site_scope();
  Window fixture;
  const LedgerRequest request = request_for(site);

  // dccp declares a 900 second staleness budget; this observation is 1000
  // seconds old, so it is stale, usable, and labelled.
  EvidenceRecord stale = committed_record(site, 250, 1, 1);
  stale.observed_at = Timestamp(kNowNanos - 1000 * kSecond);
  const AcceptanceDecision decision = fixture.window.consider(stale);
  co::test::note_assertion();
  if (!decision.accepted()) {
    CO_FAIL("stale evidence is not an acceptance failure: " + decision.explanation);
  }
  CO_REQUIRE(decision.freshness == Freshness::Stale);
  CO_REQUIRE(decision.explanation.find("freshness=stale") != std::string::npos);
  CO_REQUIRE(fixture.window.freshness_of(stale) == Freshness::Stale);

  const Result<Ledger> ledger = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(ledger, value);
  const LedgerLine* line = value.find(Dimension::Power);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE(line->worst_freshness == Freshness::Stale);
  // The figure is still used: staleness is reported, not silently discarded,
  // and not silently promoted either.
  CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Committed).value().canonical(),
                static_cast<std::int64_t>(250));

  // A fresh observation of the same slot supersedes it and clears the flag.
  EvidenceRecord fresh = committed_record(site, 260, 1, 2);
  const AcceptanceDecision fresh_decision = fixture.window.consider(fresh);
  co::test::note_assertion();
  if (!fresh_decision.accepted()) {
    CO_FAIL("a fresh observation must be accepted");
  }
  CO_REQUIRE(fresh_decision.freshness == Freshness::Fresh);
  const Result<Ledger> fresh_ledger = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(fresh_ledger, fresh_value);
  const LedgerLine* fresh_line = fresh_value.find(Dimension::Power);
  co::test::note_assertion();
  if (fresh_line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE(fresh_line->worst_freshness == Freshness::Fresh);

  // Recovery is not observation: a recovered record is never promoted to fresh.
  fixture.window.mark_all_recovered(Timestamp(kNowNanos));
  const Result<Ledger> recovered_ledger = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(recovered_ledger, recovered_value);
  const LedgerLine* recovered_line = recovered_value.find(Dimension::Power);
  co::test::note_assertion();
  if (recovered_line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE(recovered_line->worst_freshness == Freshness::Recovered);
  // Recovery marks the record the window holds, not the caller's copy.
  const EvidenceRecord* stored = nullptr;
  for (const auto& entry : fixture.window.current()) {
    if (entry.second.assertion == CapacityAssertion::Committed) {
      stored = &entry.second;
    }
  }
  co::test::note_assertion();
  if (stored == nullptr) {
    CO_FAIL("the committed slot is missing from the window");
  }
  CO_REQUIRE(stored->provenance == Provenance::Recovered);
  CO_REQUIRE(fixture.window.freshness_of(*stored) == Freshness::Recovered);
}

// ---------------------------------------------------------------------------
// Malformed scope paths and identifiers
// ---------------------------------------------------------------------------
CO_TEST(malformed_scope_paths_and_identifiers_are_refused) {
  CO_REQUIRE_ERR(ScopePath::parse(""), ReasonCode::EmptyInput);
  CO_REQUIRE_ERR(ScopePath::parse("floor=x"), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(ScopePath::parse("dc1"), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(ScopePath::parse("site=a/site=b"), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(ScopePath::parse("hall=h1/site=dc1"), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(ScopePath::parse("site=a//hall=b"), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(ScopePath::parse("site=a/"), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(ScopePath::parse("/site=a"), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(ScopePath::parse("site="), ReasonCode::InvalidIdentifier);
  CO_REQUIRE_ERR(ScopePath::parse("site=has space"), ReasonCode::InvalidIdentifier);
  CO_REQUIRE_ERR(ScopePath::parse("site=-leading"), ReasonCode::InvalidIdentifier);
  CO_REQUIRE_ERR(ScopePath::parse("site=" + std::string(97, 'a')), ReasonCode::InvalidIdentifier);
  CO_REQUIRE_ERR(ScopePath::of({{ScopeKind::Hall, "h1"}, {ScopeKind::Site, "dc1"}}), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(ScopePath::of({{ScopeKind::Site, "has space"}}), ReasonCode::InvalidIdentifier);
  CO_REQUIRE_ERR(SiteId::parse("bad id"), ReasonCode::InvalidIdentifier);
  CO_REQUIRE_ERR(SiteId::parse(""), ReasonCode::InvalidIdentifier);

  // A directly constructed ScopePath bypasses the parser, so structural
  // validation is the only line of defence left: a malformed scope must never
  // reach derived state, because the ledger parses contributing scope text back
  // and would otherwise become uncomposable.
  const ScopePath site = site_scope();
  const LedgerRequest request = request_for(site);
  Window fixture;
  const std::vector<std::pair<ScopePath, ReasonCode>> poisoned{
      {ScopePath(std::vector<ScopeSegment>{ScopeSegment{ScopeKind::Site, "has space"}}), ReasonCode::InvalidIdentifier},
      {ScopePath(std::vector<ScopeSegment>{ScopeSegment{ScopeKind::Hall, "hall-a"}, ScopeSegment{ScopeKind::Site, "dc1"}}),
       ReasonCode::InvalidArgument},
      {ScopePath(std::vector<ScopeSegment>{ScopeSegment{ScopeKind::Site, ""}}), ReasonCode::InvalidIdentifier},
  };
  for (const auto& entry : poisoned) {
    EvidenceRecord record = committed_record(site, 10, 1, 1);
    record.scope = entry.first;
    const AcceptanceDecision decision = fixture.window.consider(record);
    require_refused(decision, entry.second, AcceptanceOutcome::Invalid, "malformed scope " + entry.first.text());
  }
  CO_REQUIRE_EQ(fixture.window.size(), static_cast<std::size_t>(0));

  // A malformed authority identifier is structurally invalid, not merely an
  // authority the registry happens not to know.
  EvidenceRecord bad_authority = committed_record(site, 10, 1, 1);
  bad_authority.authority = AuthorityId("bad id");
  CO_REQUIRE_ERR(bad_authority.validate(), ReasonCode::InvalidIdentifier);
  require_refused(fixture.window.consider(bad_authority), ReasonCode::InvalidIdentifier, AcceptanceOutcome::Invalid,
                  "malformed authority identifier");

  // An empty authority is a different defect and keeps its own reason.
  EvidenceRecord empty_authority = committed_record(site, 10, 1, 1);
  empty_authority.authority = AuthorityId();
  CO_REQUIRE_ERR(empty_authority.validate(), ReasonCode::MalformedEvidence);

  // The window is not poisoned: the ledger still composes for this scope, and
  // every malformed record was refused instead of entering derived state.
  const Result<Ledger> ledger = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(ledger, value);
  const LedgerLine* line = value.find(Dimension::Power);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE(line->state == LineState::Indeterminate);
  CO_REQUIRE(!line->declared.get(CapacityAssertion::Committed).has_value());
}

// ---------------------------------------------------------------------------
// Arithmetic limits
// ---------------------------------------------------------------------------
CO_TEST(amount_arithmetic_at_the_limits) {
  CO_REQUIRE_EQ(checked_add(Amount(kMax), Amount(1)).status().code(), ReasonCode::Overflow);
  CO_REQUIRE_EQ(checked_add(Amount(kMax), Amount(0)).value().canonical(), kMax);
  CO_REQUIRE_EQ(checked_add(Amount(kMax), Amount(-1)).value().canonical(), kMax - 1);
  CO_REQUIRE_EQ(checked_add(Amount(kMin), Amount(-1)).status().code(), ReasonCode::Overflow);
  CO_REQUIRE_ERR(checked_sub(Amount(kMin), Amount(1)), ReasonCode::Overflow);
  CO_REQUIRE_EQ(checked_sub(Amount(kMax), Amount(kMax)).value().canonical(), static_cast<std::int64_t>(0));
  CO_REQUIRE_ERR(checked_mul(Amount(kMax), 2), ReasonCode::Overflow);
  CO_REQUIRE_ERR(checked_mul(Amount(kMin), -1), ReasonCode::Overflow);
  CO_REQUIRE_EQ(checked_mul(Amount(kMax), 1).value().canonical(), kMax);
  CO_REQUIRE_ERR(checked_div(Amount(kMax), 0), ReasonCode::DivisionByZero);
  CO_REQUIRE_ERR(checked_div(Amount(kMin), -1), ReasonCode::Overflow);
  CO_REQUIRE_EQ(checked_div(Amount(kMin), 2).value().canonical(), kMin / 2);
  CO_REQUIRE_ERR(checked_negate(Amount(kMin)), ReasonCode::Overflow);
  CO_REQUIRE_EQ(checked_negate(Amount(kMax)).value().canonical(), -kMax);

  CO_REQUIRE_ERR(floor_div(kMin, -1), ReasonCode::Overflow);
  CO_REQUIRE_ERR(floor_div(7, 0), ReasonCode::DivisionByZero);
  CO_REQUIRE_EQ(floor_div(-7, 2).value(), static_cast<std::int64_t>(-4));
  CO_REQUIRE_EQ(floor_div(7, -2).value(), static_cast<std::int64_t>(-4));
  CO_REQUIRE_EQ(floor_div(-8, 2).value(), static_cast<std::int64_t>(-4));

  CO_REQUIRE_ERR(scale_exact(Amount(1), 9290304, 100), ReasonCode::InexactScale);
  CO_REQUIRE_EQ(scale_exact(Amount(100), 9290304, 100).value().canonical(), static_cast<std::int64_t>(9290304));
  CO_REQUIRE_ERR(scale_exact(Amount(kMax), 1000000000, 1), ReasonCode::Overflow);
  CO_REQUIRE_ERR(scale_exact(Amount(5), 0, 1), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(scale_exact(Amount(5), 1, 0), ReasonCode::InvalidArgument);

  const std::optional<Amount> with_unknown[]{Amount(1), std::nullopt, Amount(2)};
  CO_REQUIRE_OK(checked_sum(with_unknown, 3), mixed);
  CO_REQUIRE(!mixed.total.has_value());
  CO_REQUIRE_EQ(mixed.unknown_inputs, static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(mixed.known_inputs, static_cast<std::size_t>(2));

  const std::optional<Amount> overflow_pair[]{Amount(kMax), Amount(1)};
  CO_REQUIRE_ERR(checked_sum(overflow_pair, 2), ReasonCode::Overflow);

  CO_REQUIRE_OK(checked_sum(nullptr, 0), nothing);
  CO_REQUIRE(nothing.total.has_value());
  CO_REQUIRE_EQ(nothing.total.value().canonical(), static_cast<std::int64_t>(0));

  // The same limits through the ledger: the largest representable negative
  // free figure is reported exactly, and the conservation identity still holds.
  const ScopePath site = site_scope();
  const LedgerRequest request = request_for(site);
  Window fixture;
  struct ClassRecord {
    AuthorityId authority;
    AuthorityRole role;
    CapacityAssertion assertion;
    std::int64_t amount;
  };
  const std::array<ClassRecord, 7> declarations{{
      {AuthorityId("dfi"), AuthorityRole::InstalledInventory, CapacityAssertion::Installed, 0},
      {AuthorityId("policy"), AuthorityRole::Policy, CapacityAssertion::ExcludedFailure, 0},
      {AuthorityId("policy"), AuthorityRole::Policy, CapacityAssertion::ExcludedMaintenance, 0},
      {AuthorityId("dfi"), AuthorityRole::InstalledInventory, CapacityAssertion::Stranded, 0},
      {AuthorityId("arbiter"), AuthorityRole::Arbitration, CapacityAssertion::Disputed, 0},
      {AuthorityId("asi"), AuthorityRole::Reservations, CapacityAssertion::Reserved, 0},
      {AuthorityId("dccp"), AuthorityRole::CommittedCapacity, CapacityAssertion::Committed, kMax},
  }};
  std::uint64_t generation = 1;
  for (const ClassRecord& declaration : declarations) {
    const EvidenceRecord record = canonical_record(declaration.authority, declaration.role, site, Dimension::Power,
                                                   declaration.assertion, declaration.amount, 1, generation);
    const AcceptanceDecision decision = fixture.window.consider(record);
    co::test::note_assertion();
    if (!decision.accepted()) {
      CO_FAIL("boundary evidence was refused: " + decision.explanation);
    }
    ++generation;
  }
  const Result<Ledger> ledger = compose_ledger(fixture.window, request);
  CO_REQUIRE_OK(ledger, value);
  const LedgerLine* line = value.find(Dimension::Power);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE_EQ(line->free_usable.value().canonical(), -kMax);
  CO_REQUIRE(line->overcommitted);
  CO_REQUIRE_EQ(line->overcommitment.value().canonical(), kMax);
  CO_REQUIRE(line->closures[1].holds);
}

CO_TEST(overflowing_aggregations_are_refused_not_wrapped) {
  const ScopePath site = site_scope();
  const ScopePath hall = hall_scope();
  const LedgerRequest request = request_for(site);

  // Two halves do not fit: the operation fails and produces no ledger at all.
  Window fixture;
  const EvidenceRecord first = canonical_record(AuthorityId("dfi"), AuthorityRole::InstalledInventory, site,
                                                Dimension::Power, CapacityAssertion::Installed, kMax / 2 + 1, 1, 1);
  const EvidenceRecord second = canonical_record(AuthorityId("dfi"), AuthorityRole::InstalledInventory, hall,
                                                 Dimension::Power, CapacityAssertion::Installed, kMax / 2 + 1, 1, 2);
  co::test::note_assertion();
  if (!fixture.window.consider(first).accepted() || !fixture.window.consider(second).accepted()) {
    CO_FAIL("the boundary evidence was refused before the ledger could aggregate it");
  }
  const Result<Ledger> overflowing = compose_ledger(fixture.window, request);
  CO_REQUIRE(!overflowing.has_value());
  CO_REQUIRE_ERR(compose_ledger(fixture.window, request), ReasonCode::Overflow);

  // The same shape one unit lower is representable and must succeed exactly.
  Window lower;
  const EvidenceRecord lower_first = canonical_record(AuthorityId("dfi"), AuthorityRole::InstalledInventory, site,
                                                      Dimension::Power, CapacityAssertion::Installed, kMax / 2, 1, 1);
  const EvidenceRecord lower_second = canonical_record(AuthorityId("dfi"), AuthorityRole::InstalledInventory, hall,
                                                       Dimension::Power, CapacityAssertion::Installed, kMax / 2, 1, 2);
  const EvidenceRecord lower_failure = canonical_record(AuthorityId("policy"), AuthorityRole::Policy, site,
                                                        Dimension::Power, CapacityAssertion::ExcludedFailure, 0, 1, 1);
  const EvidenceRecord lower_maintenance =
      canonical_record(AuthorityId("policy"), AuthorityRole::Policy, site, Dimension::Power,
                       CapacityAssertion::ExcludedMaintenance, 0, 1, 2);
  co::test::note_assertion();
  if (!lower.window.consider(lower_first).accepted() || !lower.window.consider(lower_second).accepted() ||
      !lower.window.consider(lower_failure).accepted() || !lower.window.consider(lower_maintenance).accepted()) {
    CO_FAIL("the representable boundary evidence was refused");
  }
  const Result<Ledger> representable = compose_ledger(lower.window, request);
  CO_REQUIRE_OK(representable, representable_value);
  const LedgerLine* line = representable_value.find(Dimension::Power);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Installed).value().canonical(), kMax - 1);
  CO_REQUIRE_EQ(line->serviceable.value().canonical(), kMax - 1);
}

// ---------------------------------------------------------------------------
// Bounded refusal audit
// ---------------------------------------------------------------------------
CO_TEST(the_refusal_audit_is_bounded_and_counts_what_it_drops) {
  const ScopePath site = site_scope();
  const LedgerRequest request = request_for(site);
  AuthorityRegistry registry = AuthorityRegistry::standard();
  std::shared_ptr<Clock> clock = manual_clock(Timestamp(kNowNanos));
  const std::size_t capacity = 3;
  EvidenceWindow window(registry, clock, capacity);

  EvidenceRecord malformed;
  malformed.scope = site;
  malformed.dimension = Dimension::Power;
  malformed.assertion = CapacityAssertion::Committed;
  malformed.unit = Unit::canonical(Dimension::Power);
  // The authority id is empty, so every one of these is structurally invalid.

  for (std::size_t i = 0; i < 10; ++i) {
    require_refused(window.consider(malformed), ReasonCode::MalformedEvidence, AcceptanceOutcome::Invalid,
                    "malformed record " + std::to_string(i));
  }
  CO_REQUIRE_EQ(window.refusals().size(), capacity);
  CO_REQUIRE_EQ(window.dropped_refusals(), static_cast<std::size_t>(10 - capacity));
  CO_REQUIRE_EQ(window.size(), static_cast<std::size_t>(0));
  CO_REQUIRE_EQ(window.accepted_count(), static_cast<std::size_t>(0));

  // A bounded audit never blocks acceptance, and derived state is unaffected by
  // everything that was refused or dropped.
  const Result<Ledger> before = compose_ledger(window, request);
  CO_REQUIRE_OK(before, before_value);
  const std::string before_dump = dump(before_value);
  const EvidenceRecord good = committed_record(site, 77, 1, 1);
  const AcceptanceDecision accepted = window.consider(good);
  co::test::note_assertion();
  if (!accepted.accepted()) {
    CO_FAIL("a valid record must still be accepted after a bounded refusal burst");
  }
  CO_REQUIRE_EQ(window.dropped_refusals(), static_cast<std::size_t>(10 - capacity));
  const Result<Ledger> after = compose_ledger(window, request);
  CO_REQUIRE_OK(after, after_value);
  CO_REQUIRE(dump(after_value) != before_dump);
  const LedgerLine* line = after_value.find(Dimension::Power);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Committed).value().canonical(),
                static_cast<std::int64_t>(77));

  // The audit keeps the refused record itself, not just a counter.
  for (const RefusedEvidence& refused : window.refusals()) {
    CO_REQUIRE_EQ(refused.decision.reason, ReasonCode::MalformedEvidence);
    CO_REQUIRE(refused.record.scope == site);
    CO_REQUIRE_EQ(refused.record.dimension, Dimension::Power);
    CO_REQUIRE(refused.record.assertion == CapacityAssertion::Committed);
  }
}

// ---------------------------------------------------------------------------
// Reordering and duplication of a complete evidence set
// ---------------------------------------------------------------------------
CO_TEST(reordered_and_duplicated_evidence_yields_identical_numbers) {
  const ScopePath site = site_scope();
  const ScopePath hall = hall_scope();
  struct Entry {
    AuthorityId authority;
    AuthorityRole role;
    ScopePath scope;
    Dimension dimension;
    CapacityAssertion assertion;
    std::int64_t amount;
  };
  const std::vector<Entry> entries{
      {AuthorityId("dfi"), AuthorityRole::InstalledInventory, site, Dimension::Power, CapacityAssertion::Installed, 1000},
      {AuthorityId("dfi"), AuthorityRole::InstalledInventory, hall, Dimension::Power, CapacityAssertion::Installed, 250},
      {AuthorityId("dccp"), AuthorityRole::CommittedCapacity, site, Dimension::Power, CapacityAssertion::Committed, 400},
      {AuthorityId("asi"), AuthorityRole::Reservations, hall, Dimension::Power, CapacityAssertion::Reserved, 100},
      {AuthorityId("policy"), AuthorityRole::Policy, site, Dimension::Power, CapacityAssertion::Nameplate, 5000},
      {AuthorityId("policy"), AuthorityRole::Policy, site, Dimension::Power, CapacityAssertion::ExcludedPolicy, 500},
      {AuthorityId("policy"), AuthorityRole::Policy, site, Dimension::Power, CapacityAssertion::ExcludedMaintenance, 50},
      {AuthorityId("policy"), AuthorityRole::Policy, site, Dimension::Power, CapacityAssertion::ExcludedFailure, 200},
      {AuthorityId("policy"), AuthorityRole::Policy, site, Dimension::Power, CapacityAssertion::OperationalReserve, 250},
      {AuthorityId("arbiter"), AuthorityRole::Arbitration, site, Dimension::Power, CapacityAssertion::Disputed, 25},
      {AuthorityId("dfi"), AuthorityRole::InstalledInventory, hall, Dimension::Power, CapacityAssertion::Stranded, 75},
      {AuthorityId("dfi"), AuthorityRole::InstalledInventory, site, Dimension::Power, CapacityAssertion::Available, 400},
      {AuthorityId("plant"), AuthorityRole::PlantTelemetry, site, Dimension::Power, CapacityAssertion::Observed, 1234},
  };

  std::vector<EvidenceRecord> records;
  std::uint64_t generation = 1;
  for (const Entry& entry : entries) {
    records.push_back(canonical_record(entry.authority, entry.role, entry.scope, entry.dimension, entry.assertion,
                                       entry.amount, 1, generation));
    ++generation;
  }
  // The same set again, byte for byte: every second copy is a duplicate.
  const std::size_t distinct = records.size();
  for (const EvidenceRecord& record : std::vector<EvidenceRecord>(records)) {
    records.push_back(record);
  }

  const LedgerRequest request = request_for(site);
  std::optional<std::string> baseline;
  for (std::size_t order = 0; order < 8; ++order) {
    co::test::Rng rng(0xABCDEF01ULL + order);
    std::vector<EvidenceRecord> shuffled = records;
    for (std::size_t i = shuffled.size(); i > 1; --i) {
      const std::size_t j = static_cast<std::size_t>(rng.below(i));
      std::swap(shuffled[i - 1], shuffled[j]);
    }
    Window fixture;
    std::size_t accepted = 0;
    for (const EvidenceRecord& record : shuffled) {
      const AcceptanceDecision decision = fixture.window.consider(record);
      if (decision.accepted()) {
        ++accepted;
      } else if (decision.reason != ReasonCode::DuplicateEvidence) {
        CO_FAIL("order " + std::to_string(order) + ": unexpected refusal " + decision.explanation);
      }
    }
    CO_REQUIRE_EQ(accepted, distinct);
    CO_REQUIRE_EQ(fixture.window.size(), distinct);
    CO_REQUIRE_EQ(fixture.window.refusals().size(), static_cast<std::size_t>(0));

    const Result<Ledger> ledger = compose_ledger(fixture.window, request);
    CO_REQUIRE_OK(ledger, value);
    const std::string text = dump(value);
    if (!baseline.has_value()) {
      baseline = text;
      // The exact numbers of the site roll-up: installed 1250, exclusions 1000,
      // committed 400, reserved 100, stranded 75, disputed 25.
      const LedgerLine* line = value.find(Dimension::Power);
      co::test::note_assertion();
      if (line == nullptr) {
        CO_FAIL("the ledger line is missing");
      }
      CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Installed).value().canonical(),
                    static_cast<std::int64_t>(1250));
      CO_REQUIRE_EQ(line->serviceable.value().canonical(), static_cast<std::int64_t>(1000));
      CO_REQUIRE_EQ(line->free_usable.value().canonical(), static_cast<std::int64_t>(400));
      CO_REQUIRE_EQ(line->exclusion_total.value().canonical(), static_cast<std::int64_t>(1000));
      CO_REQUIRE_EQ(line->governed.value().canonical(), static_cast<std::int64_t>(4000));
      CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Observed).value().canonical(),
                    static_cast<std::int64_t>(1234));
    } else {
      CO_REQUIRE_EQ(text, baseline.value());
    }
  }
}
