// Capacity Observatory - property suite: fragmentation and stranding.
//
// The realizable rack count is compared against a brute force reference that
// enumerates every distribution of racks over enclosures (exhaustive recursion
// over the raw enclosure numbers in tests/reference_model.hpp), independent of
// the production formulas. Ideal bounds, stranding, headroom, and the
// invariant realizable <= ideal are checked separately.
#include "co_test.hpp"
#include "reference_model.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/fragmentation.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/topology.hpp"

using namespace co;
using co::test::reference::ReferenceEnclosure;
using co::test::reference::ReferencePlacement;

namespace {

std::string amount_or_unknown(const std::optional<Amount>& value) {
  return value.has_value() ? std::to_string(value.value().canonical()) : std::string("unknown");
}

std::string optional_text(const std::optional<std::int64_t>& value) {
  return value.has_value() ? std::to_string(value.value()) : std::string("unknown");
}

ScopePath scope_of(std::initializer_list<ScopeSegment> segments) {
  const Result<ScopePath> parsed = ScopePath::of(segments);
  if (!parsed.ok()) {
    CO_FAIL("test scope construction failed: " + parsed.status().render());
  }
  return parsed.value();
}

// Raw description of one enclosure: the same numbers feed both the production
// topology and the brute force reference.
struct RawEnclosure {
  EnclosureId id;
  bool structurally_usable{true};
  std::optional<std::int64_t> contiguous_ru;
  std::optional<std::int64_t> total_ru;
  std::array<std::optional<std::int64_t>, kDimensionCount> free_amounts{};
};

// Raw description of the profile.
struct RawProfile {
  std::string name{"rack"};
  std::int64_t rack_units{1};
  std::array<std::optional<std::int64_t>, kDimensionCount> per_rack{};
};

EnclosureBudget budget_of(const RawEnclosure& raw, const ScopePath& scope) {
  EnclosureBudget budget;
  budget.id = raw.id;
  budget.scope = scope;
  budget.free_ru_contiguous = raw.contiguous_ru;
  budget.free_ru_total = raw.total_ru;
  budget.structurally_usable = raw.structurally_usable;
  budget.unusable_reason = raw.structurally_usable ? std::string() : std::string("no cooling path");
  for (const Dimension dimension : kAllDimensions) {
    if (raw.free_amounts[dimension_index(dimension)].has_value()) {
      budget.free.set(dimension, Amount(raw.free_amounts[dimension_index(dimension)].value()));
    }
  }
  return budget;
}

AmountVector vector_of(const std::array<std::optional<std::int64_t>, kDimensionCount>& values) {
  AmountVector vector;
  for (const Dimension dimension : kAllDimensions) {
    if (values[dimension_index(dimension)].has_value()) {
      vector.set(dimension, Amount(values[dimension_index(dimension)].value()));
    }
  }
  return vector;
}

ScopePath site_scope() { return scope_of({{ScopeKind::Site, "dc1"}}); }

// Per-enclosure expectation derived from the raw numbers only.
struct ExpectedEnclosure {
  std::int64_t racks{0};
  bool indeterminate{false};
  // False for an enclosure whose placement could not be decided: the report
  // still marks it indeterminate and counts no racks for it, but the exact
  // stranded/headroom split of its known budgets is a reporting choice rather
  // than a derivable number, so the property check does not pin it down.
  bool check_budgets{true};
  std::array<std::optional<std::int64_t>, kDimensionCount> stranded{};
  std::array<std::optional<std::int64_t>, kDimensionCount> headroom{};
};

std::vector<ExpectedEnclosure> expected_enclosures(const std::vector<RawEnclosure>& raw,
                                                   const ReferencePlacement& placement,
                                                   const RawProfile& profile) {
  std::vector<ExpectedEnclosure> expected;
  expected.reserve(raw.size());
  std::size_t evaluable = 0;
  for (const RawEnclosure& enclosure : raw) {
    ExpectedEnclosure entry;
    if (!enclosure.structurally_usable) {
      entry.racks = 0;
    } else {
      bool determinable = enclosure.contiguous_ru.has_value();
      for (std::size_t i = 0; determinable && i < kDimensionCount; ++i) {
        if (profile.per_rack[i].has_value() && !enclosure.free_amounts[i].has_value()) {
          determinable = false;
        }
      }
      if (!determinable) {
        entry.indeterminate = true;
        entry.check_budgets = false;
        entry.racks = 0;
      } else {
        entry.racks = placement.per_enclosure[evaluable];
        ++evaluable;
      }
    }
    // The enclosure's declared budget for a dimension: the free amount when it is
    // present, and for rack units the dedicated total_ru field, which denotes the
    // same capacity and is what the production analysis falls back to. Without
    // this fallback the reference would claim the report omits rack units it
    // legitimately derives from the topology.
    const auto budget = [&enclosure](std::size_t index) -> std::optional<std::int64_t> {
      if (enclosure.free_amounts[index].has_value()) {
        return enclosure.free_amounts[index];
      }
      if (index == dimension_index(Dimension::RackUnits) && enclosure.total_ru.has_value()) {
        return enclosure.total_ru;
      }
      return std::nullopt;
    };

    for (std::size_t i = 0; i < kDimensionCount; ++i) {
      const std::optional<std::int64_t> declared = budget(i);
      if (!declared.has_value()) {
        continue;
      }
      const std::int64_t free_amount = declared.value();
      if (entry.racks <= 0) {
        entry.stranded[i] = free_amount;
        entry.headroom[i] = free_amount;
      } else {
        entry.stranded[i] = 0;
        if (profile.per_rack[i].has_value()) {
          entry.headroom[i] = free_amount - entry.racks * profile.per_rack[i].value();
        } else {
          entry.headroom[i] = free_amount;
        }
      }
    }
    expected.push_back(entry);
  }
  return expected;
}

std::vector<ReferenceEnclosure> reference_enclosures(const std::vector<RawEnclosure>& raw) {
  std::vector<ReferenceEnclosure> enclosures;
  enclosures.reserve(raw.size());
  for (const RawEnclosure& entry : raw) {
    ReferenceEnclosure reference;
    reference.structurally_usable = entry.structurally_usable;
    reference.contiguous_ru = entry.contiguous_ru;
    reference.free_amounts = entry.free_amounts;
    enclosures.push_back(reference);
  }
  return enclosures;
}

// Cross-checks one report against the raw data and the brute force reference.
void cross_check_report(const std::vector<RawEnclosure>& raw, const RawProfile& profile,
                        const FragmentationReport& report, const std::string& context) {
  const ReferencePlacement placement = co::test::reference::reference_placement(
      reference_enclosures(raw), profile.rack_units, profile.per_rack);

  co::test::note_assertion();
  if (report.realizable_racks != placement.max_racks) {
    CO_FAIL(context + ": realizable " + std::to_string(report.realizable_racks) +
            " but the brute force enumeration places " + std::to_string(placement.max_racks));
  }
  CO_REQUIRE(report.realizable_racks <= report.ideal_racks);
  CO_REQUIRE_EQ(report.fragmented_racks, report.ideal_racks - report.realizable_racks);
  CO_REQUIRE_EQ(report.indeterminate, placement.indeterminate);
  CO_REQUIRE_EQ(report.indeterminate_enclosures, placement.indeterminate_enclosures);
  CO_REQUIRE_EQ(report.enclosures.size(), raw.size());
  CO_REQUIRE_EQ(report.rack_units_per_rack, profile.rack_units);

  const std::vector<ExpectedEnclosure> expected = expected_enclosures(raw, placement, profile);
  for (std::size_t index = 0; index < raw.size() && index < report.enclosures.size(); ++index) {
    const EnclosureCapacity& actual = report.enclosures[index];
    const ExpectedEnclosure& wanted = expected[index];
    CO_REQUIRE_EQ(actual.id.value(), raw[index].id.value());
    CO_REQUIRE_EQ(actual.indeterminate, wanted.indeterminate);
    if (actual.indeterminate) {
      CO_REQUIRE_EQ(actual.realizable_racks, static_cast<std::int64_t>(0));
    } else {
      CO_REQUIRE_EQ(actual.realizable_racks, wanted.racks);
    }
    if (!wanted.check_budgets) {
      // An indeterminate enclosure never reports a free amount it does not
      // know: the unknown dimensions stay absent instead of becoming zero.
      for (const Dimension dimension : kAllDimensions) {
        const std::size_t slot = dimension_index(dimension);
        const bool declared = raw[index].free_amounts[slot].has_value() ||
                              (dimension == Dimension::RackUnits && raw[index].total_ru.has_value());
        if (profile.per_rack[slot].has_value() && !declared) {
          CO_REQUIRE(!actual.stranded.get(dimension).has_value());
          CO_REQUIRE(!actual.headroom.get(dimension).has_value());
        }
      }
      continue;
    }
    for (const Dimension dimension : kAllDimensions) {
      const std::size_t slot = dimension_index(dimension);
      if (!wanted.stranded[slot].has_value()) {
        CO_REQUIRE(!actual.stranded.get(dimension).has_value());
        CO_REQUIRE(!actual.headroom.get(dimension).has_value());
        continue;
      }
      co::test::note_assertion();
      if (!actual.stranded.get(dimension).has_value() ||
          actual.stranded.get(dimension).value().canonical() != wanted.stranded[slot].value()) {
        CO_FAIL(context + ": enclosure " + raw[index].id.value() + " stranded " +
                std::string(dimension_text(dimension)) + " is " + amount_or_unknown(actual.stranded.get(dimension)) +
                " but the raw data implies " + optional_text(wanted.stranded[slot]));
      }
      co::test::note_assertion();
      if (!actual.headroom.get(dimension).has_value() ||
          actual.headroom.get(dimension).value().canonical() != wanted.headroom[slot].value()) {
        CO_FAIL(context + ": enclosure " + raw[index].id.value() + " headroom " +
                std::string(dimension_text(dimension)) + " is " + amount_or_unknown(actual.headroom.get(dimension)) +
                " but the raw data implies " + optional_text(wanted.headroom[slot]));
      }
    }
  }

  // The totals must be the independent sum of the per-enclosure figures. The
  // check is skipped when an enclosure is indeterminate: its reported split
  // between stranded and headroom is not derivable from the raw numbers alone.
  if (placement.indeterminate) {
    return;
  }
  for (const Dimension dimension : kAllDimensions) {
    std::optional<std::int64_t> stranded_total;
    std::optional<std::int64_t> headroom_total;
    for (const ExpectedEnclosure& entry : expected) {
      const std::size_t slot = dimension_index(dimension);
      if (entry.stranded[slot].has_value()) {
        stranded_total = stranded_total.value_or(0) + entry.stranded[slot].value();
        headroom_total = headroom_total.value_or(0) + entry.headroom[slot].value();
      }
    }
    const std::optional<Amount>& actual_stranded = report.stranded.get(dimension);
    co::test::note_assertion();
    if (actual_stranded.has_value() != stranded_total.has_value() ||
        (actual_stranded.has_value() && actual_stranded.value().canonical() != stranded_total.value())) {
      CO_FAIL(context + ": total stranded " + std::string(dimension_text(dimension)) + " is " +
              amount_or_unknown(actual_stranded) + " but the per-enclosure sum is " +
              optional_text(stranded_total));
    }
    const std::optional<Amount>& actual_headroom = report.headroom.get(dimension);
    co::test::note_assertion();
    if (actual_headroom.has_value() != headroom_total.has_value() ||
        (actual_headroom.has_value() && actual_headroom.value().canonical() != headroom_total.value())) {
      CO_FAIL(context + ": total headroom " + std::string(dimension_text(dimension)) + " is " +
              amount_or_unknown(actual_headroom) + " but the per-enclosure sum is " +
              optional_text(headroom_total));
    }
  }
}

Result<FragmentationReport> analyze_raw(const std::vector<RawEnclosure>& raw, const RawProfile& profile) {
  std::vector<EnclosureBudget> budgets;
  budgets.reserve(raw.size());
  const ScopePath scope = site_scope();
  for (const RawEnclosure& enclosure : raw) {
    budgets.push_back(budget_of(enclosure, scope));
  }
  const Result<Topology> topology = Topology::make(std::move(budgets));
  if (!topology.ok()) {
    return topology.status();
  }
  const Result<RackProfile> made = RackProfile::make(profile.name, profile.rack_units, vector_of(profile.per_rack));
  if (!made.ok()) {
    return made.status();
  }
  return analyze_fragmentation(topology.value(), made.value());
}

std::uint64_t seed_for(std::size_t index) {
  return 0xD1B54A32D192ED03ULL * (static_cast<std::uint64_t>(index) + 1) + 0x9E37ULL;
}

}  // namespace

// ---------------------------------------------------------------------------
// Randomised topologies against brute force placement
// ---------------------------------------------------------------------------
CO_TEST(randomized_topologies_match_brute_force_placement) {
  std::size_t compared = 0;
  for (std::size_t index = 0; index < 250; ++index) {
    const std::uint64_t seed = seed_for(index);
    co::test::Rng rng(seed);

    RawProfile profile;
    profile.rack_units = static_cast<std::int64_t>(1 + rng.below(5));
    // At least one demanded dimension, so the profile is never degenerate.
    profile.per_rack[dimension_index(Dimension::Power)] = static_cast<std::int64_t>(1 + rng.below(4));
    if (rng.chance(1, 2)) {
      profile.per_rack[dimension_index(Dimension::RackUnits)] = static_cast<std::int64_t>(1 + rng.below(4));
    }
    if (rng.chance(1, 2)) {
      profile.per_rack[dimension_index(Dimension::Space)] = static_cast<std::int64_t>(1 + rng.below(4));
    }
    if (rng.chance(1, 6)) {
      // Demand a serviceability budget that no enclosure declares: every
      // enclosure must become indeterminate rather than being counted as zero.
      profile.per_rack[dimension_index(Dimension::Serviceability)] = 1;
    }

    const std::size_t count = 2 + static_cast<std::size_t>(rng.below(3));
    std::vector<RawEnclosure> raw;
    for (std::size_t i = 0; i < count; ++i) {
      RawEnclosure enclosure;
      enclosure.id = EnclosureId("enc-" + std::to_string(i));
      enclosure.structurally_usable = !rng.chance(1, 5);
      if (rng.chance(1, 6)) {
        enclosure.contiguous_ru = std::nullopt;
        if (rng.chance(1, 2)) {
          enclosure.total_ru = static_cast<std::int64_t>(rng.below(13));
        }
      } else {
        const std::int64_t contiguous = static_cast<std::int64_t>(rng.below(13));
        enclosure.contiguous_ru = contiguous;
        enclosure.total_ru = contiguous + static_cast<std::int64_t>(rng.below(7));
      }
      for (const Dimension dimension : {Dimension::RackUnits, Dimension::Power, Dimension::Space}) {
        if (rng.chance(1, 6)) {
          continue;  // unknown free amount for this dimension
        }
        enclosure.free_amounts[dimension_index(dimension)] = static_cast<std::int64_t>(rng.below(13));
      }
      raw.push_back(enclosure);
    }

    const Result<FragmentationReport> report = analyze_raw(raw, profile);
    co::test::note_assertion();
    if (!report.ok()) {
      CO_FAIL("seed " + std::to_string(seed) + ": analyze_fragmentation failed: " + report.status().render());
    }
    cross_check_report(raw, profile, report.value(), "seed " + std::to_string(seed));
    ++compared;
  }
  CO_REQUIRE(compared >= 200);
}

CO_TEST(the_invariant_holds_when_enclosures_are_deeply_fragmented) {
  // Many tiny gaps, several enclosures, and a tall rack: the aggregate bound is
  // large while reality is small.
  co::test::Rng rng(0xF00DF00DULL);
  for (std::size_t index = 0; index < 120; ++index) {
    std::vector<RawEnclosure> raw;
    RawProfile profile;
    profile.rack_units = static_cast<std::int64_t>(3 + rng.below(4));
    profile.per_rack[dimension_index(Dimension::Power)] = 1;
    for (std::size_t i = 0; i < 4; ++i) {
      RawEnclosure enclosure;
      enclosure.id = EnclosureId("gap-" + std::to_string(i));
      const std::int64_t contiguous = static_cast<std::int64_t>(rng.below(static_cast<std::uint64_t>(profile.rack_units)));
      enclosure.contiguous_ru = contiguous;
      enclosure.total_ru = contiguous + static_cast<std::int64_t>(rng.below(20));
      enclosure.free_amounts[dimension_index(Dimension::Power)] = enclosure.total_ru.value();
      raw.push_back(enclosure);
    }
    const Result<FragmentationReport> report = analyze_raw(raw, profile);
    co::test::note_assertion();
    if (!report.ok()) {
      CO_FAIL("iteration " + std::to_string(index) + ": analyze_fragmentation failed: " + report.status().render());
    }
    cross_check_report(raw, profile, report.value(), "gap iteration " + std::to_string(index));
  }
}

// ---------------------------------------------------------------------------
// Boundaries
// ---------------------------------------------------------------------------
CO_TEST(exact_fit_boundaries) {
  // Exactly three racks fit: contiguity and power are both exhausted, while the
  // undemanded space budget stays as headroom.
  RawProfile profile;
  profile.rack_units = 4;
  profile.per_rack[dimension_index(Dimension::RackUnits)] = 4;
  profile.per_rack[dimension_index(Dimension::Power)] = 500;

  RawEnclosure enclosure;
  enclosure.id = EnclosureId("exact");
  enclosure.contiguous_ru = 12;
  enclosure.total_ru = 12;
  enclosure.free_amounts[dimension_index(Dimension::RackUnits)] = 12;
  enclosure.free_amounts[dimension_index(Dimension::Power)] = 1500;
  enclosure.free_amounts[dimension_index(Dimension::Space)] = 100;
  const std::vector<RawEnclosure> raw{enclosure};
  const Result<FragmentationReport> report = analyze_raw(raw, profile);
  CO_REQUIRE_OK(report, value);
  CO_REQUIRE_EQ(value.realizable_racks, static_cast<std::int64_t>(3));
  CO_REQUIRE_EQ(value.ideal_racks, static_cast<std::int64_t>(3));
  CO_REQUIRE_EQ(value.fragmented_racks, static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(value.stranded.get(Dimension::Power).value().canonical(), static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(value.headroom.get(Dimension::Power).value().canonical(), static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(value.headroom.get(Dimension::RackUnits).value().canonical(), static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(value.headroom.get(Dimension::Space).value().canonical(), static_cast<std::int64_t>(100));
  CO_REQUIRE(!value.indeterminate);
  cross_check_report(raw, profile, value, "exact fit");

  // One rack unit short of an exact fit: two racks and three units of headroom.
  RawEnclosure short_enclosure = enclosure;
  short_enclosure.id = EnclosureId("short");
  short_enclosure.contiguous_ru = 11;
  short_enclosure.total_ru = 11;
  short_enclosure.free_amounts[dimension_index(Dimension::RackUnits)] = 11;
  short_enclosure.free_amounts[dimension_index(Dimension::Power)] = 1500;
  const std::vector<RawEnclosure> short_raw{short_enclosure};
  const Result<FragmentationReport> short_report = analyze_raw(short_raw, profile);
  CO_REQUIRE_OK(short_report, short_value);
  CO_REQUIRE_EQ(short_value.realizable_racks, static_cast<std::int64_t>(2));
  CO_REQUIRE_EQ(short_value.ideal_racks, static_cast<std::int64_t>(2));
  CO_REQUIRE_EQ(short_value.fragmented_racks, static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(short_value.stranded.get(Dimension::RackUnits).value().canonical(), static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(short_value.headroom.get(Dimension::RackUnits).value().canonical(), static_cast<std::int64_t>(3));
  cross_check_report(short_raw, profile, short_value, "one unit short");

  // One unit of power short of a third rack: power binds, not contiguity.
  RawEnclosure power_short = enclosure;
  power_short.id = EnclosureId("power-short");
  power_short.free_amounts[dimension_index(Dimension::Power)] = 1499;
  const std::vector<RawEnclosure> power_raw{power_short};
  const Result<FragmentationReport> power_report = analyze_raw(power_raw, profile);
  CO_REQUIRE_OK(power_report, power_value);
  CO_REQUIRE_EQ(power_value.realizable_racks, static_cast<std::int64_t>(2));
  CO_REQUIRE_EQ(power_value.enclosures[0].binding, std::string("power"));
  CO_REQUIRE_EQ(power_value.enclosures[0].binding_reason, ReasonCode::BindingConstraint);
  CO_REQUIRE_EQ(power_value.headroom.get(Dimension::Power).value().canonical(), static_cast<std::int64_t>(499));
  cross_check_report(power_raw, profile, power_value, "one power unit short");
}

CO_TEST(gaps_between_enclosures_are_reported_as_fragmentation) {
  // Two enclosures each hold two racks of 4U; the aggregate suggests five.
  RawProfile profile;
  profile.rack_units = 4;
  profile.per_rack[dimension_index(Dimension::Power)] = 1;
  std::vector<RawEnclosure> raw;
  for (std::size_t i = 0; i < 2; ++i) {
    RawEnclosure enclosure;
    enclosure.id = EnclosureId("frag-" + std::to_string(i));
    enclosure.contiguous_ru = 11;
    enclosure.total_ru = 11;
    enclosure.free_amounts[dimension_index(Dimension::Power)] = 1000;
    raw.push_back(enclosure);
  }
  const Result<FragmentationReport> report = analyze_raw(raw, profile);
  CO_REQUIRE_OK(report, value);
  CO_REQUIRE_EQ(value.realizable_racks, static_cast<std::int64_t>(4));
  CO_REQUIRE_EQ(value.ideal_racks, static_cast<std::int64_t>(5));
  CO_REQUIRE_EQ(value.fragmented_racks, static_cast<std::int64_t>(1));
  for (const EnclosureCapacity& entry : value.enclosures) {
    CO_REQUIRE_EQ(entry.binding, std::string("rack-units-contiguity"));
    CO_REQUIRE_EQ(entry.binding_reason, ReasonCode::BindingConstraint);
    CO_REQUIRE_EQ(entry.realizable_racks, static_cast<std::int64_t>(2));
    // 1000 mW of free power, one mW per rack, two racks placed.
    CO_REQUIRE_EQ(entry.headroom.get(Dimension::Power).value().canonical(), static_cast<std::int64_t>(998));
  }
  cross_check_report(raw, profile, value, "inter-enclosure fragmentation");

  // A single enclosure whose longest free run is shorter than its total free
  // rack units: contiguity binds and the remaining run is headroom.
  RawEnclosure gapped;
  gapped.id = EnclosureId("gapped");
  gapped.contiguous_ru = 5;
  gapped.total_ru = 12;
  gapped.free_amounts[dimension_index(Dimension::RackUnits)] = 12;
  const std::vector<RawEnclosure> gapped_raw{gapped};
  RawProfile tall;
  tall.rack_units = 3;
  tall.per_rack[dimension_index(Dimension::RackUnits)] = 3;
  const Result<FragmentationReport> gapped_report = analyze_raw(gapped_raw, tall);
  CO_REQUIRE_OK(gapped_report, gapped_value);
  CO_REQUIRE_EQ(gapped_value.realizable_racks, static_cast<std::int64_t>(1));
  CO_REQUIRE_EQ(gapped_value.enclosures[0].binding, std::string("rack-units-contiguity"));
  CO_REQUIRE_EQ(gapped_value.indeterminate_enclosures, static_cast<std::size_t>(0));
  CO_REQUIRE_EQ(gapped_value.headroom.get(Dimension::RackUnits).value().canonical(), static_cast<std::int64_t>(9));
  cross_check_report(gapped_raw, tall, gapped_value, "gapped enclosure");
}

CO_TEST(structurally_unusable_capacity_is_stranded_and_never_ideal) {
  RawProfile profile;
  profile.rack_units = 2;
  profile.per_rack[dimension_index(Dimension::Power)] = 100;

  RawEnclosure usable;
  usable.id = EnclosureId("usable");
  usable.contiguous_ru = 6;
  usable.total_ru = 6;
  usable.free_amounts[dimension_index(Dimension::Power)] = 400;
  usable.free_amounts[dimension_index(Dimension::RackUnits)] = 6;

  RawEnclosure condemned;
  condemned.id = EnclosureId("condemned");
  condemned.structurally_usable = false;
  condemned.contiguous_ru = 40;
  condemned.total_ru = 40;
  condemned.free_amounts[dimension_index(Dimension::Power)] = 9999;
  condemned.free_amounts[dimension_index(Dimension::RackUnits)] = 40;

  RawEnclosure unknown_and_unusable;
  unknown_and_unusable.id = EnclosureId("condemned-unknown");
  unknown_and_unusable.structurally_usable = false;

  const std::vector<RawEnclosure> raw{usable, condemned, unknown_and_unusable};
  const Result<FragmentationReport> report = analyze_raw(raw, profile);
  CO_REQUIRE_OK(report, value);
  // Only the usable enclosure contributes; the condemned capacity is stranded.
  CO_REQUIRE_EQ(value.realizable_racks, static_cast<std::int64_t>(3));
  CO_REQUIRE_EQ(value.ideal_racks, static_cast<std::int64_t>(3));
  CO_REQUIRE_EQ(value.fragmented_racks, static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(value.stranded.get(Dimension::Power).value().canonical(), static_cast<std::int64_t>(9999));
  CO_REQUIRE_EQ(value.stranded.get(Dimension::RackUnits).value().canonical(), static_cast<std::int64_t>(40));
  CO_REQUIRE(!value.indeterminate);
  CO_REQUIRE_EQ(value.indeterminate_enclosures, static_cast<std::size_t>(0));
  const EnclosureCapacity* condemned_capacity = nullptr;
  for (const EnclosureCapacity& entry : value.enclosures) {
    if (entry.id.value() == "condemned") {
      condemned_capacity = &entry;
    }
  }
  co::test::note_assertion();
  if (condemned_capacity == nullptr) {
    CO_FAIL("the condemned enclosure is missing from the report");
  }
  CO_REQUIRE_EQ(condemned_capacity->realizable_racks, static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(condemned_capacity->binding_reason, ReasonCode::StrandedCapacity);
  CO_REQUIRE_EQ(condemned_capacity->stranded.get(Dimension::Power).value().canonical(),
                static_cast<std::int64_t>(9999));
  cross_check_report(raw, profile, value, "structurally unusable");
}

CO_TEST(unknown_free_dimensions_make_an_enclosure_indeterminate) {
  RawProfile profile;
  profile.rack_units = 2;
  profile.per_rack[dimension_index(Dimension::Power)] = 100;

  RawEnclosure complete;
  complete.id = EnclosureId("complete");
  complete.contiguous_ru = 6;
  complete.total_ru = 6;
  complete.free_amounts[dimension_index(Dimension::Power)] = 400;

  RawEnclosure partial;
  partial.id = EnclosureId("partial");
  partial.contiguous_ru = 8;
  partial.total_ru = 8;
  // Power is deliberately absent: unknown is not zero.
  partial.free_amounts[dimension_index(Dimension::Space)] = 1234;

  const std::vector<RawEnclosure> raw{complete, partial};
  const Result<FragmentationReport> report = analyze_raw(raw, profile);
  CO_REQUIRE_OK(report, value);
  CO_REQUIRE(value.indeterminate);
  CO_REQUIRE_EQ(value.indeterminate_enclosures, static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(value.realizable_racks, static_cast<std::int64_t>(3));
  CO_REQUIRE_EQ(value.enclosures[1].realizable_racks, static_cast<std::int64_t>(0));
  CO_REQUIRE(value.enclosures[1].indeterminate);
  CO_REQUIRE_EQ(value.enclosures[1].binding, std::string("power"));
  CO_REQUIRE_EQ(value.enclosures[1].binding_reason, ReasonCode::AmountUnknown);
  // The unknown power figure stays unknown: it is absent from both vectors
  // rather than reported as zero.
  CO_REQUIRE(!value.enclosures[1].stranded.get(Dimension::Power).has_value());
  CO_REQUIRE(!value.enclosures[1].headroom.get(Dimension::Power).has_value());
  // An enclosure that could not be evaluated reports NEITHER stranded nor
  // headroom for ANY dimension: whether it hosts anything is exactly what could
  // not be decided, so neither "all of it is stranded" nor "all of it is
  // headroom" is derivable. Its declared free amounts stay visible in the
  // topology that was analysed, and indeterminate_enclosures says why they are
  // absent from the totals.
  for (const Dimension dimension : kAllDimensions) {
    CO_REQUIRE(!value.enclosures[1].stranded.get(dimension).has_value());
    CO_REQUIRE(!value.enclosures[1].headroom.get(dimension).has_value());
  }
  // The evaluable enclosure still contributes its own exact numbers.
  CO_REQUIRE_EQ(value.stranded.get(Dimension::Power).value().canonical(), static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(value.headroom.get(Dimension::Power).value().canonical(), static_cast<std::int64_t>(100));
  CO_REQUIRE_EQ(value.ideal_racks_by_dimension[dimension_index(Dimension::Power)].value(),
                static_cast<std::int64_t>(4));
  cross_check_report(raw, profile, value, "unknown free dimension");
}

CO_TEST(unknown_contiguous_rack_units_make_an_enclosure_indeterminate) {
  RawProfile profile;
  profile.rack_units = 2;
  profile.per_rack[dimension_index(Dimension::Power)] = 1;

  RawEnclosure known;
  known.id = EnclosureId("known");
  known.contiguous_ru = 4;
  known.total_ru = 4;
  known.free_amounts[dimension_index(Dimension::Power)] = 100;

  RawEnclosure unknown;
  unknown.id = EnclosureId("unknown");
  unknown.contiguous_ru = std::nullopt;
  unknown.total_ru = 20;
  unknown.free_amounts[dimension_index(Dimension::Power)] = 500;

  const std::vector<RawEnclosure> raw{known, unknown};
  const Result<FragmentationReport> report = analyze_raw(raw, profile);
  CO_REQUIRE_OK(report, value);
  CO_REQUIRE(value.indeterminate);
  CO_REQUIRE_EQ(value.indeterminate_enclosures, static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(value.realizable_racks, static_cast<std::int64_t>(2));
  CO_REQUIRE(value.enclosures[1].indeterminate);
  CO_REQUIRE_EQ(value.enclosures[1].binding, std::string("rack-units-contiguity"));
  CO_REQUIRE_EQ(value.enclosures[1].binding_reason, ReasonCode::AmountUnknown);
  CO_REQUIRE_EQ(value.enclosures[1].realizable_racks, static_cast<std::int64_t>(0));
  cross_check_report(raw, profile, value, "unknown contiguity");
}

CO_TEST(a_rack_taller_than_every_free_run_strands_everything) {
  RawProfile profile;
  profile.rack_units = 10;
  profile.per_rack[dimension_index(Dimension::Power)] = 1000;

  std::vector<RawEnclosure> raw;
  for (std::size_t i = 0; i < 3; ++i) {
    RawEnclosure enclosure;
    enclosure.id = EnclosureId("tiny-" + std::to_string(i));
    enclosure.contiguous_ru = static_cast<std::int64_t>(3 * (i + 1));
    enclosure.total_ru = enclosure.contiguous_ru;
    enclosure.free_amounts[dimension_index(Dimension::Power)] = 5000;
    enclosure.free_amounts[dimension_index(Dimension::RackUnits)] = enclosure.contiguous_ru.value();
    raw.push_back(enclosure);
  }
  const Result<FragmentationReport> report = analyze_raw(raw, profile);
  CO_REQUIRE_OK(report, value);
  CO_REQUIRE_EQ(value.realizable_racks, static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(value.ideal_racks, static_cast<std::int64_t>(1));  // floor(18 / 10)
  CO_REQUIRE_EQ(value.fragmented_racks, static_cast<std::int64_t>(1));
  CO_REQUIRE(value.constrained_dimensions.contains(Dimension::RackUnits));
  CO_REQUIRE_EQ(value.stranded.get(Dimension::Power).value().canonical(), static_cast<std::int64_t>(15000));
  CO_REQUIRE_EQ(value.stranded.get(Dimension::RackUnits).value().canonical(), static_cast<std::int64_t>(18));
  CO_REQUIRE_EQ(value.headroom.get(Dimension::Power).value().canonical(), static_cast<std::int64_t>(15000));
  cross_check_report(raw, profile, value, "tall rack");
}

CO_TEST(empty_topology_and_degenerate_profiles_are_refused) {
  const Result<Topology> empty = Topology::make({});
  CO_REQUIRE_OK(empty, empty_value);
  CO_REQUIRE(empty_value.empty());

  std::array<std::optional<std::int64_t>, kDimensionCount> nothing{};
  CO_REQUIRE_ERR(RackProfile::make("rack", 2, vector_of(nothing)), ReasonCode::ZeroProfile);

  AmountVector demands_nothing;
  CO_REQUIRE_ERR(RackProfile::make("nothing", 2, demands_nothing), ReasonCode::ZeroProfile);

  AmountVector zero_demand;
  zero_demand.set(Dimension::Power, Amount(0));
  CO_REQUIRE_ERR(RackProfile::make("zero", 2, zero_demand), ReasonCode::ZeroProfile);

  AmountVector negative_demand;
  negative_demand.set(Dimension::Power, Amount(-5));
  CO_REQUIRE_ERR(RackProfile::make("negative", 2, negative_demand), ReasonCode::ZeroProfile);
  CO_REQUIRE_ERR(RackProfile::make("no-height", 0, negative_demand), ReasonCode::ZeroProfile);

  // The topology is checked before the profile, so nothing at all is analysed
  // for an empty topology.
  AmountVector demands_power;
  demands_power.set(Dimension::Power, Amount(10));
  const Result<RackProfile> made = RackProfile::make("rack", 2, demands_power);
  CO_REQUIRE_OK(made, made_value);
  CO_REQUIRE_ERR(analyze_fragmentation(empty_value, made_value), ReasonCode::NoTopology);
  CO_REQUIRE_ERR(analyze_fragmentation(empty_value, RackProfile{}), ReasonCode::NoTopology);

  // Against a real topology the degenerate profile is refused as ZeroProfile.
  EnclosureBudget enclosure;
  enclosure.id = EnclosureId("only");
  enclosure.scope = site_scope();
  enclosure.free_ru_contiguous = 4;
  enclosure.free_ru_total = 4;
  const Result<Topology> populated = Topology::make({enclosure});
  CO_REQUIRE_OK(populated, populated_value);
  CO_REQUIRE_ERR(analyze_fragmentation(populated_value, RackProfile{}), ReasonCode::ZeroProfile);

  // A profile that demands a dimension this enclosure leaves unknown cannot be
  // placed: the enclosure is reported indeterminate and never as zero racks of
  // free capacity.
  const Result<FragmentationReport> undecidable = analyze_fragmentation(populated_value, made_value);
  CO_REQUIRE_OK(undecidable, undecidable_value);
  CO_REQUIRE(undecidable_value.indeterminate);
  CO_REQUIRE_EQ(undecidable_value.realizable_racks, static_cast<std::int64_t>(0));
  CO_REQUIRE_EQ(undecidable_value.indeterminate_enclosures, static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(undecidable_value.enclosures[0].binding, std::string("power"));
  CO_REQUIRE_EQ(undecidable_value.enclosures[0].binding_reason, ReasonCode::AmountUnknown);
  CO_REQUIRE(!undecidable_value.enclosures[0].stranded.get(Dimension::Power).has_value());
}

CO_TEST(topology_input_is_validated_before_analysis) {
  const ScopePath site = site_scope();
  const auto budget = [&site](const char* id, std::optional<std::int64_t> contiguous,
                              std::optional<std::int64_t> total) {
    EnclosureBudget enclosure;
    enclosure.id = EnclosureId(id);
    enclosure.scope = site;
    enclosure.free_ru_contiguous = contiguous;
    enclosure.free_ru_total = total;
    return enclosure;
  };

  CO_REQUIRE_ERR(Topology::make({budget("dup", 1, 1), budget("dup", 2, 2)}), ReasonCode::AlreadyExists);
  CO_REQUIRE_ERR(Topology::make({budget("negative", -1, 4)}), ReasonCode::NegativeAmount);
  CO_REQUIRE_ERR(Topology::make({budget("total-negative", 1, -4)}), ReasonCode::NegativeAmount);
  CO_REQUIRE_ERR(Topology::make({budget("inconsistent", 5, 4)}), ReasonCode::SlotOverflow);

  EnclosureBudget empty_id = budget("no-id", 1, 1);
  empty_id.id = EnclosureId();
  CO_REQUIRE_ERR(Topology::make({empty_id}), ReasonCode::InvalidIdentifier);

  EnclosureBudget empty_scope = budget("no-scope", 1, 1);
  empty_scope.scope = ScopePath{};
  CO_REQUIRE_ERR(Topology::make({empty_scope}), ReasonCode::InvalidArgument);

  EnclosureBudget negative_free = budget("negative-free", 1, 1);
  negative_free.free.set(Dimension::Power, Amount(-1));
  CO_REQUIRE_ERR(Topology::make({negative_free}), ReasonCode::NegativeAmount);
}

CO_TEST(scoping_narrows_the_analysed_topology) {
  const ScopePath hall = scope_of({{ScopeKind::Site, "dc1"}, {ScopeKind::Hall, "hall-a"}});

  EnclosureBudget first;
  first.id = EnclosureId("hall-a-1");
  first.scope = hall;
  first.free_ru_contiguous = 8;
  first.free_ru_total = 8;
  EnclosureBudget second;
  second.id = EnclosureId("hall-b-1");
  second.scope = scope_of({{ScopeKind::Site, "dc1"}, {ScopeKind::Hall, "hall-b"}});
  second.free_ru_contiguous = 100;
  second.free_ru_total = 100;

  const Result<Topology> topology = Topology::make({first, second});
  CO_REQUIRE_OK(topology, value);
  CO_REQUIRE_EQ(value.size(), static_cast<std::size_t>(2));

  const Result<Topology> narrowed = value.narrowed_to(hall);
  CO_REQUIRE_OK(narrowed, narrowed_value);
  CO_REQUIRE_EQ(narrowed_value.size(), static_cast<std::size_t>(1));
  co::test::note_assertion();
  if (narrowed_value.find(EnclosureId("hall-a-1")) == nullptr) {
    CO_FAIL("narrowed_to kept the wrong enclosure");
  }
  CO_REQUIRE(narrowed_value.find(EnclosureId("hall-b-1")) == nullptr);
  CO_REQUIRE_ERR(value.narrowed_to(ScopePath{}), ReasonCode::InvalidArgument);
}
