// Capacity Observatory - property suite: ledger conservation.
//
// Every derived number is compared, exactly, against an independent arbitrary
// precision reference model (tests/reference_model.hpp) that recomputes the
// closure straight from the raw evidence list. The model shares no code with
// src/ledger.cpp, so an unknown-to-zero conversion, a wrap, or a clamp in the
// production int64 path shows up as a mismatch instead of being mirrored.
#include "co_test.hpp"
#include "reference_model.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/ledger.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/time.hpp"
#include "capacity_observatory/watermark.hpp"

using namespace co;
using co::test::reference::BigInt;
using co::test::reference::ReferenceEvidence;
using co::test::reference::ReferenceLine;

namespace {

constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kHalfMax = kMax / 2;
constexpr std::int64_t kNowNanos = 1700000000000000000LL;

const std::array<Dimension, 3> kLiveDimensions{Dimension::RackUnits, Dimension::Power, Dimension::Space};
const std::array<CapacityAssertion, kAssertionCount> kAllAssertions = [] {
  std::array<CapacityAssertion, kAssertionCount> all{};
  for (std::size_t i = 0; i < kAssertionCount; ++i) {
    all[i] = static_cast<CapacityAssertion>(i);
  }
  return all;
}();

struct ClassSource {
  const char* authority;
  AuthorityRole role;
  CapacityAssertion assertion;
};

// Exactly the (authority, class) pairs the standard registry grants.
const std::array<ClassSource, 14> kSources{{
    {"dccp", AuthorityRole::CommittedCapacity, CapacityAssertion::Committed},
    {"asi", AuthorityRole::Reservations, CapacityAssertion::Reserved},
    {"dfi", AuthorityRole::InstalledInventory, CapacityAssertion::Installed},
    {"dfi", AuthorityRole::InstalledInventory, CapacityAssertion::Planned},
    {"dfi", AuthorityRole::InstalledInventory, CapacityAssertion::Available},
    {"dfi", AuthorityRole::InstalledInventory, CapacityAssertion::Stranded},
    {"policy", AuthorityRole::Policy, CapacityAssertion::Nameplate},
    {"policy", AuthorityRole::Policy, CapacityAssertion::Governed},
    {"policy", AuthorityRole::Policy, CapacityAssertion::ExcludedPolicy},
    {"policy", AuthorityRole::Policy, CapacityAssertion::ExcludedMaintenance},
    {"policy", AuthorityRole::Policy, CapacityAssertion::ExcludedFailure},
    {"policy", AuthorityRole::Policy, CapacityAssertion::OperationalReserve},
    {"plant", AuthorityRole::PlantTelemetry, CapacityAssertion::Observed},
    {"arbiter", AuthorityRole::Arbitration, CapacityAssertion::Disputed},
}};

const ClassSource* source_for(CapacityAssertion assertion) {
  for (const ClassSource& source : kSources) {
    if (source.assertion == assertion) {
      return &source;
    }
  }
  return nullptr;
}

std::string amount_or_unknown(const std::optional<Amount>& value) {
  return value.has_value() ? std::to_string(value.value().canonical()) : std::string("unknown");
}

ScopePath scope_of(std::initializer_list<ScopeSegment> segments) {
  const Result<ScopePath> parsed = ScopePath::of(segments);
  if (!parsed.ok()) {
    CO_FAIL("test scope construction failed: " + parsed.status().render());
  }
  return parsed.value();
}

// The scope catalogue used by the generators: one site, two halls, two zones.
std::vector<ScopePath> scope_catalogue() {
  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const ScopePath hall_a = scope_of({{ScopeKind::Site, "dc1"}, {ScopeKind::Hall, "hall-a"}});
  const ScopePath hall_b = scope_of({{ScopeKind::Site, "dc1"}, {ScopeKind::Hall, "hall-b"}});
  const ScopePath zone_a = scope_of({{ScopeKind::Site, "dc1"}, {ScopeKind::Hall, "hall-a"}, {ScopeKind::Zone, "zone-1"}});
  const ScopePath zone_b = scope_of({{ScopeKind::Site, "dc1"}, {ScopeKind::Hall, "hall-b"}, {ScopeKind::Zone, "zone-2"}});
  return {site, hall_a, hall_b, zone_a, zone_b};
}

EvidenceRecord record_for(CapacityAssertion assertion, const ScopePath& scope, Dimension dimension,
                          std::int64_t amount, std::uint64_t epoch, std::uint64_t generation) {
  const ClassSource* source = source_for(assertion);
  if (source == nullptr) {
    CO_FAIL("no authority source registered for the requested class");
  }
  const Result<EvidenceRecord> made =
      EvidenceRecord::make(AuthorityId(source->authority), source->role, scope, dimension, assertion,
                           Unit::canonical(dimension), amount, Generation(generation), Epoch(epoch), Revision(1));
  if (!made.ok()) {
    CO_FAIL("EvidenceRecord::make failed: " + made.status().render());
  }
  EvidenceRecord record = made.value();
  record.has_observed_at = true;
  record.observed_at = Timestamp(kNowNanos);
  record.source = "test-property-conservation";
  return record;
}

void note_acceptance(const AcceptanceDecision& decision, std::string_view context) {
  if (!decision.accepted()) {
    CO_FAIL(std::string(context) + ": evidence was not accepted: " + decision.explanation);
  }
  if (!decision.affects_derived_state) {
    CO_FAIL(std::string(context) + ": accepted evidence must affect derived state");
  }
  if (decision.reason != ReasonCode::Ok && decision.reason != ReasonCode::GenerationGap) {
    CO_FAIL(std::string(context) + ": accepted evidence carried reason " +
            std::string(reason_text(decision.reason)));
  }
  if (decision.freshness != Freshness::Fresh) {
    CO_FAIL(std::string(context) + ": freshly observed evidence must be fresh, got " +
            std::string(freshness_text(decision.freshness)));
  }
}

// A window plus the raw record list that the reference model consumes.
struct Harness {
  AuthorityRegistry registry{AuthorityRegistry::standard()};
  std::shared_ptr<Clock> clock{manual_clock(Timestamp(kNowNanos))};
  EvidenceWindow window{registry, clock};
  std::vector<ReferenceEvidence> raw;
  std::uint64_t next_generation{1};

  void add(const EvidenceRecord& record) {
    ReferenceEvidence reference;
    reference.authority = record.authority.value();
    reference.scope = record.scope;
    reference.dimension = record.dimension;
    reference.assertion = record.assertion;
    reference.canonical_amount = record.amount.canonical();
    const AcceptanceDecision decision = window.consider(record);
    note_acceptance(decision, "harness ingest");
    if (window.current().empty()) {
      CO_FAIL("harness ingest: the window must hold the accepted record");
    }
    raw.push_back(reference);
  }
};

void append_record(Harness& harness, CapacityAssertion assertion, const ScopePath& scope, Dimension dimension,
                   std::int64_t amount, std::uint64_t epoch) {
  harness.add(record_for(assertion, scope, dimension, amount, epoch, harness.next_generation));
  ++harness.next_generation;
}

// Declares every remaining allocation and exclusion class as an explicit zero,
// so that derived values are known rather than unknown. A class that was never
// declared stays unknown and is never treated as zero.
void append_zero_baseline(Harness& harness, const ScopePath& scope, Dimension dimension,
                          std::initializer_list<CapacityAssertion> skip = {}) {
  for (const CapacityAssertion assertion :
       {CapacityAssertion::ExcludedFailure, CapacityAssertion::ExcludedMaintenance, CapacityAssertion::Reserved,
        CapacityAssertion::Stranded, CapacityAssertion::Disputed}) {
    bool skipped = false;
    for (const CapacityAssertion unwanted : skip) {
      skipped = skipped || unwanted == assertion;
    }
    if (!skipped) {
      append_record(harness, assertion, scope, dimension, 0, 1);
    }
  }
}

std::string dump_line(const LedgerLine& line) {
  std::string out = std::string("dimension=") + std::string(dimension_text(line.dimension));
  for (const CapacityAssertion assertion : kAllAssertions) {
    out += "|";
    out += assertion_text(assertion);
    out += "=";
    out += amount_or_unknown(line.declared.get(assertion));
  }
  out += "|exclusion_total=" + amount_or_unknown(line.exclusion_total);
  out += "|serviceable=" + amount_or_unknown(line.serviceable);
  out += "|free_usable=" + amount_or_unknown(line.free_usable);
  out += "|governed=" + amount_or_unknown(line.governed);
  out += std::string("|governed_derived=") + (line.governed_derived ? "1" : "0");
  out += "|unexplained_governance=" + amount_or_unknown(line.unexplained_governance);
  out += "|unexplained_installed=" + amount_or_unknown(line.unexplained_installed);
  out += "|unexplained_available=" + amount_or_unknown(line.unexplained_available);
  out += "|overcommitment=" + amount_or_unknown(line.overcommitment);
  out += std::string("|overcommitted=") + (line.overcommitted ? "1" : "0");
  out += "|state=" + std::string(line_state_text(line.state));
  out += "|unknown_classes=" + std::to_string(line.unknown_classes);
  out += "|evidence_count=" + std::to_string(line.evidence_count);
  for (const ClosureCheck& check : line.closures) {
    out += "|closure(" + check.name + ")=" + std::string(reason_text(check.code));
    out += ":residual=" + amount_or_unknown(check.residual);
    out += std::string(":indeterminate=") + (check.indeterminate ? "1" : "0");
    out += std::string(":holds=") + (check.holds ? "1" : "0");
  }
  return out;
}

std::vector<std::string> diff_ledgers(const Ledger& left, const Ledger& right) {
  std::vector<std::string> problems;
  if (left.lines.size() != right.lines.size()) {
    problems.push_back("line count " + std::to_string(left.lines.size()) + " vs " +
                       std::to_string(right.lines.size()));
    return problems;
  }
  for (std::size_t i = 0; i < left.lines.size(); ++i) {
    const std::string a = dump_line(left.lines[i]);
    const std::string b = dump_line(right.lines[i]);
    if (a != b) {
      problems.push_back("line " + std::to_string(i) + " differs:\n    " + a + "\n    " + b);
    }
  }
  return problems;
}

void require_no_problems(const std::vector<std::string>& problems, const std::string& context) {
  if (problems.empty()) {
    return;
  }
  std::string message = context + " produced " + std::to_string(problems.size()) + " disagreement(s):";
  const std::size_t shown = (std::min)(problems.size(), static_cast<std::size_t>(4));
  for (std::size_t i = 0; i < shown; ++i) {
    message += "\n  - " + problems[i];
  }
  CO_FAIL(message);
}

// Cross-checks the whole production ledger against the reference model.
void cross_check_ledger(const Harness& harness, const Ledger& ledger, const LedgerRequest& request,
                        const std::string& context) {
  for (const Dimension dimension : kAllDimensions) {
    if (!request.dimensions.contains(dimension)) {
      continue;
    }
    const LedgerLine* line = ledger.find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL(context + ": the ledger has no line for " + std::string(dimension_text(dimension)));
    }
    const ReferenceLine expected = co::test::reference::reference_line(harness.raw, request.scope, dimension);
    std::vector<std::string> problems;
    co::test::note_assertion();
    if (!co::test::reference::compare_line(*line, expected, problems)) {
      require_no_problems(problems, context + " [" + std::string(dimension_text(dimension)) + "]");
    }
  }
}

LedgerRequest request_for(const ScopePath& scope) {
  LedgerRequest request;
  request.scope = scope;
  request.dimensions = DimensionSet::all();
  return request;
}

// A request for exactly one dimension, so that a case can assert on the single
// line it is about without also carrying evidence-free lines for the others.
LedgerRequest request_for(const ScopePath& scope, Dimension dimension) {
  LedgerRequest request;
  request.scope = scope;
  request.dimensions = DimensionSet::of(dimension);
  return request;
}

// ---------------------------------------------------------------------------
// Random ledger generation
// ---------------------------------------------------------------------------

// A ledger whose identities all hold, distributed over several scopes: used for
// the conservation identity and for the unknown-dropping property.
struct CoherentLedger {
  std::int64_t installed{0};
  std::int64_t excluded_failure{0};
  std::int64_t excluded_maintenance{0};
  std::int64_t committed{0};
  std::int64_t reserved{0};
  std::int64_t stranded{0};
  std::int64_t disputed{0};
  std::int64_t free_usable{0};
  std::int64_t nameplate{0};
  std::int64_t excluded_policy{0};
  std::int64_t operational_reserve{0};
  std::int64_t governed{0};
};

CoherentLedger make_coherent_ledger(co::test::Rng& rng, Harness& harness, Dimension dimension, std::uint64_t epoch) {
  CoherentLedger numbers;
  numbers.installed = rng.between(0, 4'000'000);
  numbers.excluded_failure = rng.between(0, numbers.installed);
  numbers.excluded_maintenance = rng.between(0, numbers.installed - numbers.excluded_failure);
  const std::int64_t serviceable =
      numbers.installed - numbers.excluded_failure - numbers.excluded_maintenance;
  numbers.committed = rng.between(0, serviceable);
  numbers.reserved = rng.between(0, serviceable - numbers.committed);
  numbers.stranded = rng.between(0, serviceable - numbers.committed - numbers.reserved);
  numbers.disputed = rng.between(0, serviceable - numbers.committed - numbers.reserved - numbers.stranded);
  numbers.free_usable =
      serviceable - numbers.committed - numbers.reserved - numbers.stranded - numbers.disputed;
  numbers.excluded_policy = rng.between(0, 100'000);
  numbers.operational_reserve = rng.between(0, 100'000);
  numbers.nameplate = numbers.installed + numbers.excluded_policy + numbers.operational_reserve;
  numbers.governed = numbers.nameplate - numbers.excluded_policy - numbers.operational_reserve -
                     numbers.excluded_failure - numbers.excluded_maintenance;

  const std::vector<ScopePath> scopes = scope_catalogue();
  const auto place = [&](CapacityAssertion assertion, std::int64_t amount) {
    const ScopePath& scope = scopes[rng.below(static_cast<std::uint64_t>(scopes.size()))];
    append_record(harness, assertion, scope, dimension, amount, epoch);
  };
  place(CapacityAssertion::Installed, numbers.installed);
  place(CapacityAssertion::ExcludedFailure, numbers.excluded_failure);
  place(CapacityAssertion::ExcludedMaintenance, numbers.excluded_maintenance);
  place(CapacityAssertion::Committed, numbers.committed);
  place(CapacityAssertion::Reserved, numbers.reserved);
  place(CapacityAssertion::Stranded, numbers.stranded);
  place(CapacityAssertion::Disputed, numbers.disputed);
  place(CapacityAssertion::Available, numbers.free_usable);
  place(CapacityAssertion::Nameplate, numbers.nameplate);
  place(CapacityAssertion::Governed, numbers.governed);
  place(CapacityAssertion::ExcludedPolicy, numbers.excluded_policy);
  place(CapacityAssertion::OperationalReserve, numbers.operational_reserve);
  return numbers;
}

// A dense random ledger: several classes, several scopes, several dimensions,
// random omissions so that unknown classes occur naturally.
void generate_dense_ledger(co::test::Rng& rng, Harness& harness) {
  const std::vector<ScopePath> scopes = scope_catalogue();
  const std::uint64_t epoch = 1;
  // The window keeps only the newest generation of a slot, so a random ledger
  // that must match a sum over the raw list visits every slot at most once.
  std::set<std::string> used;
  for (const Dimension dimension : kLiveDimensions) {
    if (!rng.chance(3, 4)) {
      continue;
    }
    for (const ClassSource& source : kSources) {
      if (!rng.chance(1, 2)) {
        continue;
      }
      const std::uint64_t copies = rng.below(3);
      for (std::uint64_t i = 0; i <= copies; ++i) {
        const ScopePath& scope = scopes[rng.below(static_cast<std::uint64_t>(scopes.size()))];
        const std::string slot = std::string(source.authority) + "|" + scope.text() + "|" +
                                 std::string(dimension_text(dimension)) + "|" +
                                 std::string(assertion_text(source.assertion));
        if (!used.insert(slot).second) {
          continue;
        }
        append_record(harness, source.assertion, scope, dimension, rng.between(0, 1 << 20), epoch);
      }
    }
  }
}

// A boundary-value ledger. At most one class may carry a near-INT64_MAX value
// so that the production int64 accumulation order cannot overflow on a total
// that is itself representable: any failure here is a defect.
void generate_boundary_ledger(co::test::Rng& rng, Harness& harness) {
  const std::vector<ScopePath> scopes = scope_catalogue();
  const std::uint64_t epoch = 1;
  // One class may carry a near-maximum magnitude. When that magnitude is within
  // one unit of INT64_MAX every other class must be zero, otherwise a sum that
  // is representable could still be refused because an intermediate is not.
  const bool extreme = rng.chance(1, 3);
  const std::int64_t large = extreme ? (rng.chance(1, 2) ? kMax : kMax - 1)
                                     : (rng.chance(1, 2) ? kHalfMax : kHalfMax + 1);
  const std::int64_t small_ceiling = extreme ? 0 : 4095;
  bool large_used = false;
  for (const Dimension dimension : kLiveDimensions) {
    if (!rng.chance(3, 4)) {
      continue;
    }
    for (const ClassSource& source : kSources) {
      if (!rng.chance(2, 3)) {
        continue;
      }
      std::int64_t amount = rng.between(0, small_ceiling);
      if (!large_used && rng.chance(1, 3)) {
        amount = large;
        large_used = true;
      }
      const ScopePath& scope = scopes[rng.below(static_cast<std::uint64_t>(scopes.size()))];
      append_record(harness, source.assertion, scope, dimension, amount, epoch);
    }
  }
}

std::uint64_t seed_for(std::size_t index) {
  return 0x9E3779B97F4A7C15ULL * (static_cast<std::uint64_t>(index) + 1) + 0x1234567ULL;
}

}  // namespace

// ---------------------------------------------------------------------------
// The reference model itself
// ---------------------------------------------------------------------------
CO_TEST(reference_bigint_matches_int64_arithmetic) {
  co::test::Rng rng(0xB16B00B5ULL);
  for (int iteration = 0; iteration < 2000; ++iteration) {
    // Bounded so that the 64 bit products used as the oracle cannot overflow.
    const std::int64_t left = rng.between(-(1LL << 20), 1LL << 20);
    const std::int64_t right = rng.between(-(1LL << 40), 1LL << 40);
    const BigInt a(left);
    const BigInt b(right);
    CO_REQUIRE_EQ((a + b), BigInt(left + right));
    CO_REQUIRE_EQ((a - b), BigInt(left - right));
    CO_REQUIRE_EQ((a * b), BigInt(left * right));
    CO_REQUIRE_EQ((a < b), (left < right));
    CO_REQUIRE_EQ((a == b), (left == right));
    CO_REQUIRE_EQ(BigInt(-left).to_string(), std::to_string(-left));
    if (right != 0) {
      CO_REQUIRE_EQ((a / b), BigInt(left / right));
      CO_REQUIRE_EQ((a % b), BigInt(left % right));
      CO_REQUIRE_EQ(BigInt::floor_div(a, b), BigInt(left / right - ((left % right != 0 && (left < 0) != (right < 0)) ? 1 : 0)));
    }
  }
  // Values far outside int64 must still compare and print exactly.
  const BigInt huge = BigInt(kMax) * BigInt(kMax);
  CO_REQUIRE(!huge.fits_int64());
  CO_REQUIRE_EQ(huge.to_string(), std::string("85070591730234615847396907784232501249"));
  CO_REQUIRE_EQ(huge / BigInt(kMax), BigInt(kMax));
  CO_REQUIRE(huge - BigInt(kMax) * BigInt(kMax) == BigInt(0));
  CO_REQUIRE_EQ(BigInt(kMax).to_string(), std::to_string(kMax));
  CO_REQUIRE_EQ(BigInt((std::numeric_limits<std::int64_t>::min)()).to_string(),
                std::to_string((std::numeric_limits<std::int64_t>::min)()));
}

// ---------------------------------------------------------------------------
// Randomised ledgers against the reference
// ---------------------------------------------------------------------------
CO_TEST(randomized_ledgers_match_the_reference_exactly) {
  std::size_t compared = 0;
  for (std::size_t index = 0; index < 300; ++index) {
    const std::uint64_t seed = seed_for(index);
    co::test::Rng rng(seed);
    Harness harness;
    generate_dense_ledger(rng, harness);

    const std::vector<ScopePath> scopes = scope_catalogue();
    const ScopePath& requested = scopes[rng.below(static_cast<std::uint64_t>(scopes.size()))];
    const LedgerRequest request = request_for(requested);
    const Result<Ledger> ledger = compose_ledger(harness.window, request);
    co::test::note_assertion();
    if (!ledger.ok()) {
      CO_FAIL("seed " + std::to_string(seed) + ": compose_ledger failed: " + ledger.status().render());
    }
    cross_check_ledger(harness, ledger.value(), request, "seed " + std::to_string(seed));
    ++compared;
  }
  CO_REQUIRE(compared >= 250);
}

CO_TEST(randomized_boundary_ledgers_match_the_reference_exactly) {
  std::size_t compared = 0;
  for (std::size_t index = 0; index < 150; ++index) {
    const std::uint64_t seed = seed_for(index + 10'000);
    co::test::Rng rng(seed);
    Harness harness;
    generate_boundary_ledger(rng, harness);

    const std::vector<ScopePath> scopes = scope_catalogue();
    const ScopePath& requested = scopes[rng.below(static_cast<std::uint64_t>(scopes.size()))];
    const LedgerRequest request = request_for(requested);
    const Result<Ledger> ledger = compose_ledger(harness.window, request);
    co::test::note_assertion();
    if (!ledger.ok()) {
      CO_FAIL("seed " + std::to_string(seed) +
              ": compose_ledger refused a boundary ledger whose totals are all representable: " +
              ledger.status().render());
    }
    cross_check_ledger(harness, ledger.value(), request, "seed " + std::to_string(seed));
    ++compared;
  }
  CO_REQUIRE(compared >= 120);
}

// ---------------------------------------------------------------------------
// Conservation identity over coherent ledgers
// ---------------------------------------------------------------------------
CO_TEST(conservation_identity_holds_when_every_class_is_known) {
  std::size_t checked = 0;
  for (std::size_t index = 0; index < 200; ++index) {
    const std::uint64_t seed = seed_for(index + 20'000);
    co::test::Rng rng(seed);
    Harness harness;
    const Dimension dimension = kLiveDimensions[rng.below(static_cast<std::uint64_t>(kLiveDimensions.size()))];
    const CoherentLedger numbers = make_coherent_ledger(rng, harness, dimension, 1);
    const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
    const LedgerRequest request = request_for(site, dimension);
    const Result<Ledger> ledger = compose_ledger(harness.window, request);
    co::test::note_assertion();
    if (!ledger.ok()) {
      CO_FAIL("seed " + std::to_string(seed) + ": compose_ledger failed: " + ledger.status().render());
    }
    const LedgerLine* line = ledger.value().find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL("seed " + std::to_string(seed) + ": missing ledger line");
    }

    // Every identity class is known, so nothing may be reported unknown.
    CO_REQUIRE_EQ(line->unknown_classes, static_cast<std::size_t>(0));
    co::test::note_assertion();
    if (line->state != LineState::Complete) {
      CO_FAIL("seed " + std::to_string(seed) + ": expected a complete line but the state is " +
              std::string(line_state_text(line->state)) + "\n  " + dump_line(*line));
    }
    CO_REQUIRE(!ledger.value().has_residuals());
    CO_REQUIRE(!ledger.value().has_overcommitment());
    CO_REQUIRE_EQ(ledger.value().incomplete_lines(), static_cast<std::size_t>(0));
    CO_REQUIRE(line->worst_freshness == Freshness::Fresh);

    // The conservation identity, restated independently with arbitrary
    // precision arithmetic over the production values.
    const BigInt installed(line->declared.get(CapacityAssertion::Installed).value().canonical());
    const BigInt failure(line->declared.get(CapacityAssertion::ExcludedFailure).value().canonical());
    const BigInt maintenance(line->declared.get(CapacityAssertion::ExcludedMaintenance).value().canonical());
    const BigInt stranded(line->declared.get(CapacityAssertion::Stranded).value().canonical());
    const BigInt disputed(line->declared.get(CapacityAssertion::Disputed).value().canonical());
    const BigInt committed(line->declared.get(CapacityAssertion::Committed).value().canonical());
    const BigInt reserved(line->declared.get(CapacityAssertion::Reserved).value().canonical());
    const BigInt free_usable(line->free_usable.value().canonical());
    CO_REQUIRE_EQ(installed, failure + maintenance + stranded + disputed + committed + reserved + free_usable);
    CO_REQUIRE(line->unexplained_installed.has_value());
    CO_REQUIRE_EQ(line->unexplained_installed.value().canonical(), static_cast<std::int64_t>(0));
    CO_REQUIRE(line->closures[1].holds);

    // And the declared numbers are exactly the ones that were ingested.
    CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Installed).value().canonical(), numbers.installed);
    CO_REQUIRE_EQ(line->serviceable.value().canonical(),
                  numbers.installed - numbers.excluded_failure - numbers.excluded_maintenance);
    CO_REQUIRE_EQ(line->free_usable.value().canonical(), numbers.free_usable);
    CO_REQUIRE_EQ(line->governed.value().canonical(), numbers.governed);
    CO_REQUIRE(!line->governed_derived);
    CO_REQUIRE_EQ(line->exclusion_total.value().canonical(),
                  numbers.excluded_policy + numbers.excluded_maintenance + numbers.excluded_failure +
                      numbers.operational_reserve);

    cross_check_ledger(harness, ledger.value(), request, "coherent seed " + std::to_string(seed));
    ++checked;
  }
  CO_REQUIRE(checked >= 180);
}

CO_TEST(governed_is_derived_only_when_the_authority_is_silent) {
  co::test::Rng rng(0x5EED1234ULL);
  for (std::size_t index = 0; index < 100; ++index) {
    const std::uint64_t seed = seed_for(index + 30'000);
    co::test::Rng local(seed);
    Harness harness;
    const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
    const Dimension dimension = Dimension::Power;
    const std::int64_t nameplate = local.between(1000, 100'000);
    const std::int64_t policy = local.between(0, nameplate);
    const std::int64_t reserve = local.between(0, nameplate - policy);
    append_record(harness, CapacityAssertion::Nameplate, site, dimension, nameplate, 1);
    append_record(harness, CapacityAssertion::ExcludedPolicy, site, dimension, policy, 1);
    append_record(harness, CapacityAssertion::OperationalReserve, site, dimension, reserve, 1);
    // The two remaining exclusion classes are declared as zero: absent is
    // unknown, and an unknown exclusion makes the derived ceiling unknown.
    append_record(harness, CapacityAssertion::ExcludedMaintenance, site, dimension, 0, 1);
    append_record(harness, CapacityAssertion::ExcludedFailure, site, dimension, 0, 1);
    append_record(harness, CapacityAssertion::Installed, site, dimension, nameplate, 1);

    const LedgerRequest request = request_for(site, dimension);
    const Result<Ledger> derived_ledger = compose_ledger(harness.window, request);
    CO_REQUIRE_OK(derived_ledger, derived);
    const LedgerLine* line = derived.find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL("seed " + std::to_string(seed) + ": missing ledger line");
    }
    CO_REQUIRE(line->governed_derived);
    CO_REQUIRE_EQ(line->governed.value().canonical(), nameplate - policy - reserve);

    // Declaring governed explicitly must override the derivation, not blend.
    const std::int64_t declared = local.between(0, nameplate - policy - reserve);
    append_record(harness, CapacityAssertion::Governed, site, dimension, declared, 1);
    const Result<Ledger> declared_ledger = compose_ledger(harness.window, request);
    CO_REQUIRE_OK(declared_ledger, declared_result);
    const LedgerLine* declared_line = declared_result.find(dimension);
    co::test::note_assertion();
    if (declared_line == nullptr) {
      CO_FAIL("seed " + std::to_string(seed) + ": missing ledger line after declaring governed");
    }
    CO_REQUIRE(!declared_line->governed_derived);
    CO_REQUIRE_EQ(declared_line->governed.value().canonical(), declared);
    cross_check_ledger(harness, declared_result, request, "governed seed " + std::to_string(seed));
  }
}

// ---------------------------------------------------------------------------
// Unknown is never zero
// ---------------------------------------------------------------------------
CO_TEST(dropping_a_class_makes_its_derived_value_unknown_and_counts_it) {
  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const Dimension dimension = Dimension::Power;
  const LedgerRequest request = request_for(site, dimension);

  // The baseline: every identity class declared, every identity holding.
  const auto build = [&](std::optional<CapacityAssertion> dropped) {
    Harness harness;
    const std::array<std::pair<CapacityAssertion, std::int64_t>, 12> declarations{{
        {CapacityAssertion::Installed, 1000},
        {CapacityAssertion::ExcludedFailure, 100},
        {CapacityAssertion::ExcludedMaintenance, 50},
        {CapacityAssertion::Committed, 200},
        {CapacityAssertion::Reserved, 100},
        {CapacityAssertion::Stranded, 50},
        {CapacityAssertion::Disputed, 25},
        {CapacityAssertion::Available, 475},
        {CapacityAssertion::Nameplate, 2000},
        // 2000 - policy 200 - maintenance 50 - failure 100 - reserve 50.
        {CapacityAssertion::Governed, 1600},
        {CapacityAssertion::ExcludedPolicy, 200},
        {CapacityAssertion::OperationalReserve, 50},
    }};
    for (const auto& declaration : declarations) {
      if (dropped.has_value() && dropped.value() == declaration.first) {
        continue;
      }
      append_record(harness, declaration.first, site, dimension, declaration.second, 1);
    }
    return harness;
  };

  Harness baseline = build(std::nullopt);
  const Result<Ledger> baseline_ledger = compose_ledger(baseline.window, request);
  CO_REQUIRE_OK(baseline_ledger, baseline_value);
  const LedgerLine* baseline_line = baseline_value.find(dimension);
  co::test::note_assertion();
  if (baseline_line == nullptr) {
    CO_FAIL("baseline ledger line is missing");
  }
  CO_REQUIRE_EQ(baseline_line->unknown_classes, static_cast<std::size_t>(0));
  CO_REQUIRE(baseline_line->state == LineState::Complete);
  CO_REQUIRE_EQ(baseline_line->free_usable.value().canonical(), static_cast<std::int64_t>(475));

  for (const CapacityAssertion assertion : co::test::reference::identity_classes()) {
    Harness reduced = build(assertion);
    const Result<Ledger> ledger = compose_ledger(reduced.window, request);
    CO_REQUIRE_OK(ledger, reduced_value);
    const LedgerLine* line = reduced_value.find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL("ledger line is missing after dropping " + std::string(assertion_text(assertion)));
    }
    const std::string context = std::string("dropping ") + std::string(assertion_text(assertion));

    // The class itself must be unknown, and the completeness verdict must move.
    CO_REQUIRE(!line->declared.get(assertion).has_value());
    CO_REQUIRE_EQ(line->unknown_classes, static_cast<std::size_t>(1));
    CO_REQUIRE(line->state != LineState::Complete);

    // The reference must agree that the class is unknown, never zero.
    const ReferenceLine expected = co::test::reference::reference_line(reduced.raw, request.scope, dimension);
    CO_REQUIRE(!expected.get(assertion).has_value());
    std::vector<std::string> problems;
    co::test::note_assertion();
    if (!co::test::reference::compare_line(*line, expected, problems)) {
      require_no_problems(problems, context);
    }

    switch (assertion) {
      case CapacityAssertion::Installed:
        CO_REQUIRE(!line->serviceable.has_value());
        CO_REQUIRE(!line->free_usable.has_value());
        CO_REQUIRE(line->closures[1].indeterminate);
        break;
      case CapacityAssertion::Committed:
      case CapacityAssertion::Reserved:
      case CapacityAssertion::Stranded:
      case CapacityAssertion::Disputed:
        CO_REQUIRE(line->serviceable.has_value());
        CO_REQUIRE(!line->free_usable.has_value());
        CO_REQUIRE(line->closures[1].indeterminate);
        break;
      case CapacityAssertion::ExcludedFailure:
      case CapacityAssertion::ExcludedMaintenance:
        CO_REQUIRE(!line->serviceable.has_value());
        CO_REQUIRE(!line->free_usable.has_value());
        CO_REQUIRE(!line->exclusion_total.has_value());
        CO_REQUIRE(line->closures[1].indeterminate);
        break;
      case CapacityAssertion::Available:
        CO_REQUIRE(line->free_usable.has_value());
        CO_REQUIRE(!line->unexplained_available.has_value());
        CO_REQUIRE(line->closures[2].indeterminate);
        break;
      case CapacityAssertion::Nameplate:
        // Governed is declared here, so it stays known; what becomes unknown is
        // the governance residual, which must not be invented as zero.
        CO_REQUIRE(line->governed.has_value());
        CO_REQUIRE(!line->governed_derived);
        CO_REQUIRE(line->closures[0].indeterminate);
        CO_REQUIRE(!line->unexplained_governance.has_value());
        break;
      default:
        CO_FAIL(context + ": unexpected identity class in the drop set");
    }

    // Nothing else may silently become zero: the values that are still known
    // must equal the baseline.
    if (assertion != CapacityAssertion::ExcludedFailure && assertion != CapacityAssertion::ExcludedMaintenance &&
        assertion != CapacityAssertion::Installed) {
      CO_REQUIRE_EQ(line->serviceable.value().canonical(), baseline_line->serviceable.value().canonical());
    }
    if (assertion != CapacityAssertion::Committed && assertion != CapacityAssertion::Reserved &&
        assertion != CapacityAssertion::Stranded && assertion != CapacityAssertion::Disputed &&
        assertion != CapacityAssertion::Installed && assertion != CapacityAssertion::ExcludedFailure &&
        assertion != CapacityAssertion::ExcludedMaintenance) {
      CO_REQUIRE_EQ(line->free_usable.value().canonical(), baseline_line->free_usable.value().canonical());
    }
  }

  // Classes outside the identity set still poison the derived totals, but they
  // are not part of the completeness verdict: the ledger documents that
  // unknown_classes counts the nine identity classes only.
  for (const CapacityAssertion assertion : {CapacityAssertion::ExcludedPolicy, CapacityAssertion::OperationalReserve}) {
    Harness reduced = build(assertion);
    const Result<Ledger> ledger = compose_ledger(reduced.window, request);
    CO_REQUIRE_OK(ledger, outside_value);
    const LedgerLine* line = outside_value.find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL("ledger line is missing after dropping a non-identity class");
    }
    CO_REQUIRE(!line->exclusion_total.has_value());
    // Governed is declared in this ledger, so it stays known; what the unknown
    // exclusion does destroy is the governance residual and the derived total.
    CO_REQUIRE(!line->governed_derived);
    CO_REQUIRE(line->governed.has_value());
    CO_REQUIRE(line->closures[0].indeterminate);
    CO_REQUIRE_EQ(line->unknown_classes, static_cast<std::size_t>(0));
  }
}

CO_TEST(dropping_nameplate_makes_a_derived_governed_ceiling_unknown) {
  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const Dimension dimension = Dimension::Power;
  const LedgerRequest request = request_for(site, dimension);

  Harness derived;
  append_record(derived, CapacityAssertion::Nameplate, site, dimension, 5000, 1);
  append_record(derived, CapacityAssertion::ExcludedPolicy, site, dimension, 500, 1);
  append_record(derived, CapacityAssertion::ExcludedMaintenance, site, dimension, 50, 1);
  append_record(derived, CapacityAssertion::ExcludedFailure, site, dimension, 200, 1);
  append_record(derived, CapacityAssertion::OperationalReserve, site, dimension, 250, 1);
  append_record(derived, CapacityAssertion::Installed, site, dimension, 5000, 1);
  const Result<Ledger> derived_ledger = compose_ledger(derived.window, request);
  CO_REQUIRE_OK(derived_ledger, derived_value);
  const LedgerLine* derived_line = derived_value.find(dimension);
  co::test::note_assertion();
  if (derived_line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE(derived_line->governed_derived);
  CO_REQUIRE_EQ(derived_line->governed.value().canonical(), static_cast<std::int64_t>(4000));
  cross_check_ledger(derived, derived_value, request, "derived governed ceiling");

  // The same ledger without the nameplate: the derived ceiling becomes unknown,
  // never zero, and the governance identity becomes indeterminate.
  Harness missing;
  append_record(missing, CapacityAssertion::ExcludedPolicy, site, dimension, 500, 1);
  append_record(missing, CapacityAssertion::ExcludedMaintenance, site, dimension, 50, 1);
  append_record(missing, CapacityAssertion::ExcludedFailure, site, dimension, 200, 1);
  append_record(missing, CapacityAssertion::OperationalReserve, site, dimension, 250, 1);
  append_record(missing, CapacityAssertion::Installed, site, dimension, 5000, 1);
  const Result<Ledger> missing_ledger = compose_ledger(missing.window, request);
  CO_REQUIRE_OK(missing_ledger, missing_value);
  const LedgerLine* missing_line = missing_value.find(dimension);
  co::test::note_assertion();
  if (missing_line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE(missing_line->governed_derived);
  CO_REQUIRE(!missing_line->governed.has_value());
  CO_REQUIRE(missing_line->closures[0].indeterminate);
  CO_REQUIRE_EQ(missing_line->closures[0].code, ReasonCode::UnknownNameplate);
  CO_REQUIRE(!missing_line->unexplained_governance.has_value());
  cross_check_ledger(missing, missing_value, request, "nameplate-less governed ceiling");
}

CO_TEST(a_class_declared_as_zero_is_known_and_not_unknown) {
  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const Dimension dimension = Dimension::RackUnits;
  const LedgerRequest request = request_for(site);
  Harness harness;
  for (const CapacityAssertion assertion :
       {CapacityAssertion::Installed, CapacityAssertion::Committed, CapacityAssertion::Reserved,
        CapacityAssertion::Stranded, CapacityAssertion::Disputed, CapacityAssertion::ExcludedFailure,
        CapacityAssertion::ExcludedMaintenance}) {
    append_record(harness, assertion, site, dimension, 0, 1);
  }
  const Result<Ledger> ledger = compose_ledger(harness.window, request);
  CO_REQUIRE_OK(ledger, value);
  const LedgerLine* line = value.find(dimension);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("ledger line is missing");
  }
  CO_REQUIRE(line->declared.get(CapacityAssertion::Installed).has_value());
  CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Installed).value().canonical(), static_cast<std::int64_t>(0));
  CO_REQUIRE(line->free_usable.has_value());
  CO_REQUIRE_EQ(line->free_usable.value().canonical(), static_cast<std::int64_t>(0));
  cross_check_ledger(harness, value, request, "explicit zero");

  // A scope with no evidence at all is indeterminate, never zero.
  const ScopePath other = scope_of({{ScopeKind::Site, "dc2"}});
  const Result<Ledger> empty = compose_ledger(harness.window, request_for(other));
  CO_REQUIRE_OK(empty, empty_value);
  const LedgerLine* empty_line = empty_value.find(dimension);
  co::test::note_assertion();
  if (empty_line == nullptr) {
    CO_FAIL("an empty scope must still produce a line");
  }
  CO_REQUIRE(empty_line->state == LineState::Indeterminate);
  CO_REQUIRE(!empty_line->declared.get(CapacityAssertion::Installed).has_value());
  CO_REQUIRE(!empty_line->free_usable.has_value());
  CO_REQUIRE(!empty_line->serviceable.has_value());
  CO_REQUIRE(!empty_line->exclusion_total.has_value());
  CO_REQUIRE(!empty_line->governed.has_value());
  CO_REQUIRE_EQ(empty_line->evidence_count, static_cast<std::size_t>(0));
  CO_REQUIRE(empty_line->worst_freshness == Freshness::Unknown);
  // An identity that was never evaluated is reported as indeterminate with a
  // stable reason rather than as a default holds=false with reason ok.
  for (const ClosureCheck& check : empty_line->closures) {
    CO_REQUIRE(check.indeterminate);
    CO_REQUIRE(!check.holds);
    CO_REQUIRE(!check.residual.has_value());
    CO_REQUIRE_EQ(check.code, ReasonCode::UndeterminedCapacity);
    CO_REQUIRE(!check.name.empty());
    CO_REQUIRE(!check.explanation.empty());
  }
}

// ---------------------------------------------------------------------------
// State verdict
// ---------------------------------------------------------------------------
CO_TEST(a_failing_identity_makes_the_line_residual) {
  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const Dimension dimension = Dimension::Power;
  const LedgerRequest request = request_for(site, dimension);
  Harness harness;
  append_zero_baseline(harness, site, dimension);
  append_record(harness, CapacityAssertion::Installed, site, dimension, 100, 1);
  append_record(harness, CapacityAssertion::Committed, site, dimension, 0, 1);
  // 100 installed, no allocations: the computed free figure is 100, so a
  // declared available of 50 is a residual, not a healthy line.
  append_record(harness, CapacityAssertion::Available, site, dimension, 50, 1);
  const Result<Ledger> ledger = compose_ledger(harness.window, request);
  CO_REQUIRE_OK(ledger, value);
  const LedgerLine* line = value.find(dimension);
  co::test::note_assertion();
  if (line == nullptr) {
    CO_FAIL("the ledger line is missing");
  }
  CO_REQUIRE_EQ(line->free_usable.value().canonical(), static_cast<std::int64_t>(100));
  CO_REQUIRE(line->state == LineState::Residual);
  CO_REQUIRE(value.has_residuals());
  CO_REQUIRE_EQ(line->unexplained_available.value().canonical(), static_cast<std::int64_t>(-50));
  CO_REQUIRE_EQ(line->closures[2].code, ReasonCode::AvailableMismatch);
  CO_REQUIRE(!line->closures[2].holds);
  CO_REQUIRE(!line->closures[2].indeterminate);
  cross_check_ledger(harness, value, request, "declared available mismatch");
}

// ---------------------------------------------------------------------------
// Integer boundaries
// ---------------------------------------------------------------------------
CO_TEST(exact_int64_boundaries_are_preserved) {
  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const Dimension dimension = Dimension::Power;
  const LedgerRequest request = request_for(site);

  {
    Harness harness;
    append_zero_baseline(harness, site, dimension, {CapacityAssertion::Reserved});
    append_record(harness, CapacityAssertion::Installed, site, dimension, kMax, 1);
    // Committed and reserved together exhaust the installed maximum exactly.
    append_record(harness, CapacityAssertion::Committed, site, dimension, kHalfMax, 1);
    append_record(harness, CapacityAssertion::Reserved, site, dimension, kMax - kHalfMax, 1);
    const Result<Ledger> ledger = compose_ledger(harness.window, request);
    CO_REQUIRE_OK(ledger, value);
    const LedgerLine* line = value.find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL("ledger line is missing");
    }
    CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Installed).value().canonical(), kMax);
    CO_REQUIRE_EQ(line->serviceable.value().canonical(), kMax);
    CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Committed).value().canonical(), kHalfMax);
    CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Reserved).value().canonical(), kMax - kHalfMax);
    CO_REQUIRE_EQ(line->free_usable.value().canonical(), static_cast<std::int64_t>(0));
    CO_REQUIRE(!line->overcommitted);
    CO_REQUIRE(line->closures[1].holds);
    cross_check_ledger(harness, value, request, "int64 maximum");
  }

  {
    // The smallest non-zero quantities are exact and never confused with
    // unknown or with zero.
    Harness harness;
    append_zero_baseline(harness, site, dimension);
    append_record(harness, CapacityAssertion::Installed, site, dimension, 1, 1);
    append_record(harness, CapacityAssertion::Committed, site, dimension, 1, 1);
    const Result<Ledger> ledger = compose_ledger(harness.window, request);
    CO_REQUIRE_OK(ledger, value);
    const LedgerLine* line = value.find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL("ledger line is missing");
    }
    CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Installed).value().canonical(),
                  static_cast<std::int64_t>(1));
    CO_REQUIRE_EQ(line->serviceable.value().canonical(), static_cast<std::int64_t>(1));
    CO_REQUIRE_EQ(line->free_usable.value().canonical(), static_cast<std::int64_t>(0));
    CO_REQUIRE(line->closures[1].holds);
    cross_check_ledger(harness, value, request, "unit magnitudes");
  }

  {
    // The exact midpoint must survive untouched.
    Harness harness;
    append_zero_baseline(harness, site, dimension);
    append_record(harness, CapacityAssertion::Installed, site, dimension, kHalfMax, 1);
    append_record(harness, CapacityAssertion::Committed, site, dimension, kHalfMax, 1);
    const Result<Ledger> ledger = compose_ledger(harness.window, request);
    CO_REQUIRE_OK(ledger, value);
    const LedgerLine* line = value.find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL("ledger line is missing");
    }
    CO_REQUIRE_EQ(line->free_usable.value().canonical(), static_cast<std::int64_t>(0));
    CO_REQUIRE(line->closures[1].holds);
  }

  {
    // Allocations exceeding installed capacity produce a negative free figure,
    // reported rather than clamped to zero.
    Harness harness;
    append_zero_baseline(harness, site, dimension);
    append_record(harness, CapacityAssertion::Installed, site, dimension, 10, 1);
    append_record(harness, CapacityAssertion::Committed, site, dimension, 25, 1);
    const Result<Ledger> ledger = compose_ledger(harness.window, request);
    CO_REQUIRE_OK(ledger, value);
    const LedgerLine* line = value.find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL("ledger line is missing");
    }
    CO_REQUIRE_EQ(line->free_usable.value().canonical(), static_cast<std::int64_t>(-15));
    CO_REQUIRE(line->overcommitted);
    CO_REQUIRE_EQ(line->overcommitment.value().canonical(), static_cast<std::int64_t>(15));
    CO_REQUIRE(line->closures[1].holds);
    cross_check_ledger(harness, value, request, "negative free capacity");
  }
}

CO_TEST(aggregate_overflow_is_refused_and_never_wraps) {
  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const ScopePath hall = scope_of({{ScopeKind::Site, "dc1"}, {ScopeKind::Hall, "hall-a"}});
  const Dimension dimension = Dimension::Power;
  const LedgerRequest request = request_for(site);

  // Two records in the same class, each already half of the maximum: the true
  // sum is not representable, so the composition must fail rather than wrap.
  {
    Harness harness;
    append_record(harness, CapacityAssertion::Installed, site, dimension, kHalfMax + 1, 1);
    append_record(harness, CapacityAssertion::Installed, hall, dimension, kHalfMax + 1, 1);
    const ReferenceLine reference = co::test::reference::reference_line(harness.raw, site, dimension);
    CO_REQUIRE(reference.get(CapacityAssertion::Installed).has_value());
    CO_REQUIRE(!reference.get(CapacityAssertion::Installed).value().fits_int64());
    CO_REQUIRE_EQ(reference.get(CapacityAssertion::Installed).value().to_string(),
                  std::string("9223372036854775808"));
    CO_REQUIRE_ERR(compose_ledger(harness.window, request), ReasonCode::Overflow);
  }

  // The same boundary one unit lower is representable and must succeed exactly.
  {
    Harness harness;
    append_record(harness, CapacityAssertion::Installed, site, dimension, kHalfMax, 1);
    append_record(harness, CapacityAssertion::Installed, hall, dimension, kHalfMax, 1);
    const Result<Ledger> ledger = compose_ledger(harness.window, request);
    CO_REQUIRE_OK(ledger, value);
    const LedgerLine* line = value.find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL("ledger line is missing");
    }
    CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Installed).value().canonical(), kMax - 1);
    cross_check_ledger(harness, value, request, "sum at the int64 maximum minus one");
  }

  // The exclusion total is a second accumulation site and must refuse too.
  {
    Harness harness;
    const std::int64_t each = kHalfMax;
    append_record(harness, CapacityAssertion::ExcludedPolicy, site, dimension, each, 1);
    append_record(harness, CapacityAssertion::ExcludedMaintenance, site, dimension, each, 1);
    append_record(harness, CapacityAssertion::ExcludedFailure, site, dimension, each, 1);
    append_record(harness, CapacityAssertion::OperationalReserve, site, dimension, 0, 1);
    const ReferenceLine reference = co::test::reference::reference_line(harness.raw, site, dimension);
    CO_REQUIRE(reference.exclusion_total.has_value());
    CO_REQUIRE(!reference.exclusion_total.value().fits_int64());
    CO_REQUIRE_ERR(compose_ledger(harness.window, request), ReasonCode::Overflow);
  }

  // Three quarters of the maximum, twice, is still representable.
  {
    Harness harness;
    const std::int64_t each = kMax / 4;
    append_record(harness, CapacityAssertion::ExcludedPolicy, site, dimension, each, 1);
    append_record(harness, CapacityAssertion::ExcludedMaintenance, hall, dimension, each, 1);
    append_record(harness, CapacityAssertion::ExcludedFailure, site, dimension, each, 1);
    append_record(harness, CapacityAssertion::OperationalReserve, hall, dimension, each, 1);
    const Result<Ledger> ledger = compose_ledger(harness.window, request);
    CO_REQUIRE_OK(ledger, value);
    const LedgerLine* line = value.find(dimension);
    co::test::note_assertion();
    if (line == nullptr) {
      CO_FAIL("ledger line is missing");
    }
    CO_REQUIRE_EQ(line->exclusion_total.value().canonical(), kMax / 4 * 4);
    cross_check_ledger(harness, value, request, "exclusion total below the boundary");
  }
}

// ---------------------------------------------------------------------------
// Duplicates, generations, and ordering
// ---------------------------------------------------------------------------
CO_TEST(only_the_newest_generation_of_a_slot_drives_derived_state) {
  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const Dimension dimension = Dimension::Power;
  const LedgerRequest request = request_for(site);
  Harness harness;

  const EvidenceRecord older = record_for(CapacityAssertion::Installed, site, dimension, 100, 1, 4);
  const EvidenceRecord newer = record_for(CapacityAssertion::Installed, site, dimension, 250, 1, 5);
  const EvidenceRecord newest = record_for(CapacityAssertion::Installed, site, dimension, 900, 1, 6);

  const AcceptanceDecision first = harness.window.consider(newer);
  note_acceptance(first, "newest generation baseline");
  const AcceptanceDecision stale = harness.window.consider(older);
  CO_REQUIRE(!stale.accepted());
  CO_REQUIRE(stale.reason == ReasonCode::StaleGeneration);
  CO_REQUIRE(stale.outcome == AcceptanceOutcome::StaleGeneration);
  CO_REQUIRE(!stale.affects_derived_state);
  CO_REQUIRE_EQ(harness.window.refusals().size(), static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(harness.window.refusals().back().decision.reason, ReasonCode::StaleGeneration);
  CO_REQUIRE_EQ(harness.window.refusals().back().record.amount.canonical(), static_cast<std::int64_t>(100));

  const Result<Ledger> after_stale = compose_ledger(harness.window, request);
  CO_REQUIRE_OK(after_stale, stale_value);
  const LedgerLine* stale_line = stale_value.find(dimension);
  co::test::note_assertion();
  if (stale_line == nullptr) {
    CO_FAIL("ledger line is missing");
  }
  CO_REQUIRE_EQ(stale_line->declared.get(CapacityAssertion::Installed).value().canonical(),
                static_cast<std::int64_t>(250));

  const AcceptanceDecision advanced = harness.window.consider(newest);
  note_acceptance(advanced, "newest generation supersedes");
  const Result<Ledger> after_newest = compose_ledger(harness.window, request);
  CO_REQUIRE_OK(after_newest, newest_value);
  const LedgerLine* newest_line = newest_value.find(dimension);
  co::test::note_assertion();
  if (newest_line == nullptr) {
    CO_FAIL("ledger line is missing");
  }
  CO_REQUIRE_EQ(newest_line->declared.get(CapacityAssertion::Installed).value().canonical(),
                static_cast<std::int64_t>(900));

  const ReferenceEvidence reference{newer.authority.value(), site, dimension, CapacityAssertion::Installed, 900};
  const ReferenceLine expected = co::test::reference::reference_line({reference}, site, dimension);
  std::vector<std::string> problems;
  co::test::note_assertion();
  if (!co::test::reference::compare_line(*newest_line, expected, problems)) {
    require_no_problems(problems, "newest generation");
  }
}

CO_TEST(a_shuffled_evidence_set_produces_identical_derived_numbers) {
  // Several slots carry several generations each, so the ingest order decides
  // which records are superseded and which are refused as stale. The final
  // state must be the newest generation of every slot, in every order.
  struct SlotPlan {
    CapacityAssertion assertion;
    Dimension dimension;
    std::vector<std::int64_t> amounts;
  };
  const std::vector<ScopePath> scopes = scope_catalogue();
  const std::vector<SlotPlan> plan{
      {CapacityAssertion::Installed, Dimension::Power, {10, 20, 30}},
      {CapacityAssertion::Committed, Dimension::Power, {5, 7}},
      {CapacityAssertion::Reserved, Dimension::Power, {1, 2, 3, 4}},
      {CapacityAssertion::Nameplate, Dimension::Power, {100, 90}},
      {CapacityAssertion::Installed, Dimension::RackUnits, {8, 12}},
      {CapacityAssertion::Committed, Dimension::RackUnits, {2, 3, 6}},
      {CapacityAssertion::Stranded, Dimension::Space, {11, 13}},
      {CapacityAssertion::ExcludedPolicy, Dimension::Power, {50, 60}},
  };

  std::vector<EvidenceRecord> records;
  std::vector<ReferenceEvidence> newest_raw;
  for (std::size_t slot = 0; slot < plan.size(); ++slot) {
    const ScopePath& scope = scopes[slot % scopes.size()];
    const SlotPlan& entry = plan[slot];
    for (std::size_t generation = 0; generation < entry.amounts.size(); ++generation) {
      const EvidenceRecord record = record_for(entry.assertion, scope, entry.dimension, entry.amounts[generation],
                                               1, static_cast<std::uint64_t>(generation + 1));
      records.push_back(record);
    }
    ReferenceEvidence reference;
    reference.authority = "dfi";
    reference.scope = scope;
    reference.dimension = entry.dimension;
    reference.assertion = entry.assertion;
    reference.canonical_amount = entry.amounts.back();
    const ClassSource* source = source_for(entry.assertion);
    if (source != nullptr) {
      reference.authority = source->authority;
    }
    newest_raw.push_back(reference);
  }

  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const LedgerRequest request = request_for(site);

  std::optional<std::string> baseline_dump;
  for (std::size_t order = 0; order < 12; ++order) {
    co::test::Rng rng(seed_for(order + 40'000));
    std::vector<EvidenceRecord> shuffled = records;
    for (std::size_t i = shuffled.size(); i > 1; --i) {
      const std::size_t j = static_cast<std::size_t>(rng.below(i));
      std::swap(shuffled[i - 1], shuffled[j]);
    }

    Harness harness;
    for (const EvidenceRecord& record : shuffled) {
      const AcceptanceDecision decision = harness.window.consider(record);
      if (!decision.accepted() && decision.reason != ReasonCode::StaleGeneration &&
          decision.reason != ReasonCode::DuplicateEvidence) {
        CO_FAIL("order " + std::to_string(order) + ": unexpected refusal " + decision.explanation);
      }
    }
    const Result<Ledger> ledger = compose_ledger(harness.window, request);
    CO_REQUIRE_OK(ledger, value);

    std::string dump;
    for (const LedgerLine& line : value.lines) {
      dump += dump_line(line) + "\n";
    }
    if (!baseline_dump.has_value()) {
      baseline_dump = dump;
    } else {
      CO_REQUIRE_EQ(dump, baseline_dump.value());
    }

    // Every slot now holds its newest generation, so the reference computed
    // from the newest set must match the composed ledger exactly.
    for (const Dimension dimension : kAllDimensions) {
      const LedgerLine* line = value.find(dimension);
      co::test::note_assertion();
      if (line == nullptr) {
        CO_FAIL("order " + std::to_string(order) + ": ledger line is missing");
      }
      const ReferenceLine expected = co::test::reference::reference_line(newest_raw, site, dimension);
      std::vector<std::string> problems;
      co::test::note_assertion();
      if (!co::test::reference::compare_line(*line, expected, problems)) {
        require_no_problems(problems, "order " + std::to_string(order) + " [" +
                                          std::string(dimension_text(dimension)) + "]");
      }
    }
  }
  CO_REQUIRE(baseline_dump.has_value());
}

CO_TEST(a_duplicate_record_is_idempotent) {
  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const Dimension dimension = Dimension::Power;
  const LedgerRequest request = request_for(site);
  Harness harness;
  const EvidenceRecord record = record_for(CapacityAssertion::Committed, site, dimension, 42, 1, 7);
  const AcceptanceDecision first = harness.window.consider(record);
  note_acceptance(first, "duplicate baseline");
  CO_REQUIRE_EQ(harness.window.accepted_count(), static_cast<std::size_t>(1));

  const Result<Ledger> before = compose_ledger(harness.window, request);
  CO_REQUIRE_OK(before, before_value);

  const AcceptanceDecision duplicate = harness.window.consider(record);
  CO_REQUIRE(duplicate.outcome == AcceptanceOutcome::Duplicate);
  CO_REQUIRE(duplicate.reason == ReasonCode::DuplicateEvidence);
  CO_REQUIRE(!duplicate.affects_derived_state);
  CO_REQUIRE_EQ(harness.window.accepted_count(), static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(harness.window.size(), static_cast<std::size_t>(1));
  // A duplicate is not a refusal: it changed nothing and needs no audit entry.
  CO_REQUIRE_EQ(harness.window.refusals().size(), static_cast<std::size_t>(0));

  const Result<Ledger> after = compose_ledger(harness.window, request);
  CO_REQUIRE_OK(after, after_value);
  const std::vector<std::string> problems = diff_ledgers(before_value, after_value);
  co::test::note_assertion();
  require_no_problems(problems, "duplicate record");
}

CO_TEST(the_request_scope_bounds_what_is_aggregated) {
  const ScopePath site = scope_of({{ScopeKind::Site, "dc1"}});
  const ScopePath hall = scope_of({{ScopeKind::Site, "dc1"}, {ScopeKind::Hall, "hall-a"}});
  const ScopePath other_hall = scope_of({{ScopeKind::Site, "dc1"}, {ScopeKind::Hall, "hall-b"}});
  const Dimension dimension = Dimension::Power;
  Harness harness;
  append_record(harness, CapacityAssertion::Installed, hall, dimension, 100, 1);
  append_record(harness, CapacityAssertion::Installed, other_hall, dimension, 250, 1);

  const Result<Ledger> site_ledger = compose_ledger(harness.window, request_for(site));
  CO_REQUIRE_OK(site_ledger, site_value);
  const LedgerLine* site_line = site_value.find(dimension);
  co::test::note_assertion();
  if (site_line == nullptr) {
    CO_FAIL("site ledger line is missing");
  }
  CO_REQUIRE_EQ(site_line->declared.get(CapacityAssertion::Installed).value().canonical(),
                static_cast<std::int64_t>(350));
  CO_REQUIRE_EQ(site_line->evidence_count, static_cast<std::size_t>(2));
  cross_check_ledger(harness, site_value, request_for(site), "site roll-up");

  const Result<Ledger> hall_ledger = compose_ledger(harness.window, request_for(hall));
  CO_REQUIRE_OK(hall_ledger, hall_value);
  const LedgerLine* hall_line = hall_value.find(dimension);
  co::test::note_assertion();
  if (hall_line == nullptr) {
    CO_FAIL("hall ledger line is missing");
  }
  CO_REQUIRE_EQ(hall_line->declared.get(CapacityAssertion::Installed).value().canonical(),
                static_cast<std::int64_t>(100));
  CO_REQUIRE_EQ(hall_line->evidence_count, static_cast<std::size_t>(1));
  cross_check_ledger(harness, hall_value, request_for(hall), "hall roll-up");

  // An empty request scope is refused, not treated as the whole tree.
  LedgerRequest empty;
  CO_REQUIRE_ERR(compose_ledger(harness.window, empty), ReasonCode::InvalidArgument);
}
