// Independent downstream consumer of the installed Capacity Observatory package.
//
// It uses only installed public headers and the exported CMake target, and it
// exercises the parts of the contract a neighbouring runtime would depend on:
// authority contracts, the acceptance window, the explainable ledger with its
// closure residuals, and the fragmentation analysis.

#include <cstdio>
#include <string>
#include <vector>

#include <capacity_observatory/evidence.hpp>
#include <capacity_observatory/fragmentation.hpp>
#include <capacity_observatory/ledger.hpp>
#include <capacity_observatory/platform.hpp>
#include <capacity_observatory/time.hpp>
#include <capacity_observatory/topology.hpp>
#include <capacity_observatory/watermark.hpp>

namespace {

using namespace co;

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "consumer check failed: %s\n", what);
    ++failures;
  }
}

std::string amount_text(const std::optional<Amount>& amount) {
  return amount.has_value() ? std::to_string(amount.value().canonical()) : std::string("unknown");
}

EvidenceRecord make_record(const char* authority,
                           AuthorityRole role,
                           const char* scope_text,
                           Dimension dimension,
                           CapacityAssertion assertion,
                           const char* unit_text,
                           std::int64_t amount,
                           std::uint64_t generation) {
  const Result<AuthorityId> authority_id = AuthorityId::parse(authority);
  const Result<ScopePath> scope = ScopePath::parse(scope_text);
  const Result<Unit> unit = Unit::parse_for(dimension, unit_text);
  const Result<EvidenceRecord> record =
      EvidenceRecord::make(authority_id.value(), role, scope.value(), dimension, assertion, unit.value(), amount,
                           Generation(generation), Epoch(1), Revision(0));
  return record.value();
}

}  // namespace

int main() {
  platform::suppress_error_dialogs();

  // A deterministic clock keeps this program's output reproducible.
  const std::shared_ptr<Clock> clock = manual_clock(Timestamp(1'700'000'000'000'000'000LL));
  const AuthorityRegistry registry = AuthorityRegistry::standard();
  EvidenceWindow window(registry, clock);

  // A complete, self consistent evidence set for one hall and one dimension:
  // 2 MW of nameplate, 1.8 MW governed, 1.5 MW installed, 900 kW committed,
  // 300 kW reserved, and a declared available figure that does NOT match the
  // derived free capacity, so the residual the observatory reports is a real one.
  const std::vector<EvidenceRecord> records{
      make_record("policy", AuthorityRole::Policy, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::Nameplate, "kW", 2000, 7),
      make_record("policy", AuthorityRole::Policy, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::Governed, "kW", 1800, 7),
      make_record("policy", AuthorityRole::Policy, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::ExcludedPolicy, "kW", 100, 7),
      make_record("policy", AuthorityRole::Policy, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::ExcludedMaintenance, "kW", 0, 7),
      make_record("policy", AuthorityRole::Policy, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::ExcludedFailure, "kW", 0, 7),
      make_record("policy", AuthorityRole::Policy, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::OperationalReserve, "kW", 100, 7),
      make_record("dfi", AuthorityRole::InstalledInventory, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::Installed, "kW", 1500, 11),
      make_record("dfi", AuthorityRole::InstalledInventory, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::Available, "kW", 350, 11),
      make_record("dfi", AuthorityRole::InstalledInventory, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::Stranded, "kW", 0, 11),
      make_record("dccp", AuthorityRole::CommittedCapacity, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::Committed, "kW", 900, 33),
      make_record("asi", AuthorityRole::Reservations, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::Reserved, "kW", 300, 4),
      make_record("arbiter", AuthorityRole::Arbitration, "site=dc1/hall=h1", Dimension::Power,
                  CapacityAssertion::Disputed, "kW", 0, 2),
  };

  for (const EvidenceRecord& record : records) {
    const AcceptanceDecision decision = window.consider(record);
    check(decision.accepted(), "every well formed record from its own authority is accepted");
  }
  check(window.size() == records.size(), "the window holds one current record per slot");

  // Evidence from an authority that owns no capacity authority must be refused
  // even though it is perfectly well formed and clearly visible here.
  const EvidenceRecord outsider = make_record("economics", AuthorityRole::Economics, "site=dc1/hall=h1",
                                              Dimension::Power, CapacityAssertion::Installed, "kW", 9999, 1);
  const AcceptanceDecision refused = window.consider(outsider);
  check(!refused.accepted(), "visibility of a slot does not transfer authority to another runtime");
  check(refused.reason == ReasonCode::EvidenceRefused, "the refusal carries a stable reason code");

  const Result<ScopePath> scope = ScopePath::parse("site=dc1/hall=h1");
  LedgerRequest request;
  request.scope = scope.value();
  request.dimensions = DimensionSet::of(Dimension::Power);
  const Result<Ledger> ledger = compose_ledger(window, request);
  check(ledger.ok(), "ledger composition succeeds");
  if (!ledger.ok()) {
    std::printf("capacity-observatory consumer: FAILED\n");
    return 1;
  }

  const LedgerLine* line = ledger.value().find(Dimension::Power);
  check(line != nullptr, "the power line exists");
  if (line != nullptr) {
    check(line->state == LineState::Residual, "a declared figure that the evidence does not explain is a residual");
    check(line->unknown_classes == 0, "every identity class is declared");
    check(!line->governed_derived, "the governed ceiling is declared, not derived");
    // installed 1500 kW - failure 0 - maintenance 0 = 1500 kW = 1500000000 mW
    check(line->serviceable.has_value() && line->serviceable.value().canonical() == 1'500'000'000,
          "serviceable capacity is derived exactly");
    // 1500 - 900 - 300 - 0 - 0 = 300 kW = 300000000 mW
    check(line->free_usable.has_value() && line->free_usable.value().canonical() == 300'000'000,
          "free usable capacity is derived exactly");
    // declared available 350 kW against a derived 300 kW: a +50 kW residual that
    // is reported rather than clamped to zero or hidden.
    check(line->unexplained_available.has_value() &&
              line->unexplained_available.value().canonical() == 50'000'000,
          "the available residual is reported exactly and signed");
    // committed 900 + reserved 300 against a governed 1800 kW ceiling: headroom,
    // not over-commitment.
    check(!line->overcommitted, "allocations within the governed ceiling are not over-commitment");
    check(line->closures[0].holds, "the governance identity closes at zero");
    check(line->closures[1].holds, "the installed partition closes at zero");
    check(line->closures[3].holds, "the allocation identity holds");

    std::printf("power: state=%s unknown_classes=%zu serviceable=%s free_usable=%s residual_available=%s\n",
                std::string(line_state_text(line->state)).c_str(), line->unknown_classes,
                amount_text(line->serviceable).c_str(), amount_text(line->free_usable).c_str(),
                amount_text(line->unexplained_available).c_str());
  }

  // Fragmentation: three enclosures, none of which can host a 42U rack because
  // the longest free run is 20U. The aggregate bound still suggests two racks.
  std::vector<EnclosureBudget> budgets;
  {
    EnclosureBudget first;
    first.id = EnclosureId("encl-1");
    first.scope = ScopePath::parse("site=dc1/hall=h1/enclosure=encl-1").value();
    first.free_ru_contiguous = 20;
    first.free_ru_total = 20;
    first.free.set(Dimension::Power, Amount(280'000));
    budgets.push_back(first);

    EnclosureBudget second;
    second.id = EnclosureId("encl-2");
    second.scope = ScopePath::parse("site=dc1/hall=h1/enclosure=encl-2").value();
    second.free_ru_contiguous = 20;
    second.free_ru_total = 20;
    second.free.set(Dimension::Power, Amount(280'000));
    budgets.push_back(second);

    EnclosureBudget third;
    third.id = EnclosureId("encl-3");
    third.scope = ScopePath::parse("site=dc1/hall=h1/enclosure=encl-3").value();
    third.free_ru_contiguous = 3;  // a 42U rack cannot fit, whatever the totals say
    third.free_ru_total = 60;
    third.free.set(Dimension::Power, Amount(500'000));
    budgets.push_back(third);
  }
  const Result<Topology> topology = Topology::make(std::move(budgets));
  check(topology.ok(), "topology is accepted");

  AmountVector demand;
  demand.set(Dimension::Power, Amount(7'000));  // 7 kW per rack
  const Result<RackProfile> profile = RackProfile::make("rails-42u", 42, demand);
  check(profile.ok(), "profile is accepted");

  if (topology.ok() && profile.ok()) {
    const Result<FragmentationReport> report = analyze_fragmentation(topology.value(), profile.value());
    check(report.ok(), "fragmentation analysis succeeds");
    if (report.ok()) {
      check(report.value().realizable_racks == 0, "no enclosure can host the profile: the capacity is stranded");
      check(report.value().ideal_racks == 2, "the aggregate bound that ignores enclosure boundaries suggests 2");
      check(report.value().fragmented_racks == 2, "the difference is reported as fragmentation");
      check(report.value().stranded.get(Dimension::RackUnits).has_value() &&
                report.value().stranded.get(Dimension::RackUnits).value().canonical() == 100,
            "every free rack unit is stranded, exactly");
      std::printf("fragmentation: realizable=%lld ideal=%lld fragmented=%lld stranded_rack_units=%s\n",
                  static_cast<long long>(report.value().realizable_racks),
                  static_cast<long long>(report.value().ideal_racks),
                  static_cast<long long>(report.value().fragmented_racks),
                  amount_text(report.value().stranded.get(Dimension::RackUnits)).c_str());
    }
  }

  std::printf("capacity-observatory consumer: %s\n", failures == 0 ? "OK" : "FAILED");
  return failures == 0 ? 0 : 1;
}
