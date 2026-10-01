// Capacity Observatory - exact fragmentation, stranding, and binding constraints.
#include "capacity_observatory/fragmentation.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace co {
namespace {

struct EnclosureEvaluation {
  std::int64_t racks{0};
  std::string binding;
  ReasonCode binding_reason{ReasonCode::Ok};
  bool indeterminate{false};
  bool structurally_usable{true};
  bool has_slot_limit{false};
  std::int64_t slot_limit{0};
  std::array<std::optional<std::int64_t>, kDimensionCount> dimension_limits{};
  AmountVector stranded;
  AmountVector headroom;
  std::string explanation;
};

// A profile dimension is a real constraint only when the profile demands a
// positive amount of it; an undemanded dimension stays unknown and imposes
// nothing.
Result<std::int64_t> rack_limit(Amount free_amount, Amount per_rack, Dimension dimension) {
  if (per_rack.canonical() <= 0) {
    return make_error(ReasonCode::ZeroProfile,
                      "profile demands a non-positive " + std::string(dimension_text(dimension)) + " per rack");
  }
  const Result<std::int64_t> quotient = floor_div(free_amount.canonical(), per_rack.canonical());
  if (!quotient.ok()) {
    return quotient.status();
  }
  return quotient;
}

}  // namespace

Result<RackProfile> RackProfile::make(std::string name, std::int64_t rack_units, AmountVector per_rack) {
  if (rack_units <= 0) {
    return make_error(ReasonCode::ZeroProfile, "profile '" + name + "' must occupy at least one rack unit");
  }
  if (per_rack.is_empty()) {
    return make_error(ReasonCode::ZeroProfile,
                      "profile '" + name + "' demands no dimensions; a profile that demands nothing cannot be placed");
  }
  for (const Dimension dimension : kAllDimensions) {
    const std::optional<Amount>& demand = per_rack.get(dimension);
    if (!demand.has_value()) {
      continue;
    }
    if (demand.value().canonical() <= 0) {
      return make_error(ReasonCode::ZeroProfile,
                        "profile '" + name + "' demands a non-positive " + std::string(dimension_text(dimension)) +
                            " of " + std::to_string(demand.value().canonical()));
    }
  }
  RackProfile profile;
  profile.name = std::move(name);
  profile.rack_units = rack_units;
  profile.per_rack = per_rack;
  return Result<RackProfile>(std::move(profile));
}

DimensionSet RackProfile::demanded_dimensions() const noexcept {
  DimensionSet set;
  for (const Dimension dimension : kAllDimensions) {
    if (per_rack.has(dimension)) {
      set.add(dimension);
    }
  }
  return set;
}

Result<FragmentationReport> analyze_fragmentation(const Topology& topology, const RackProfile& profile) {
  if (topology.empty()) {
    return make_error(ReasonCode::NoTopology, "no enclosures are known for the requested scope");
  }
  if (profile.rack_units <= 0) {
    return make_error(ReasonCode::ZeroProfile, "profile rack height must be positive");
  }
  const DimensionSet demanded = profile.demanded_dimensions();
  if (demanded.empty()) {
    return make_error(ReasonCode::ZeroProfile, "profile demands no dimensions");
  }
  for (const Dimension dimension : kAllDimensions) {
    if (!demanded.contains(dimension)) {
      continue;
    }
    const std::optional<Amount>& demand = profile.per_rack.get(dimension);
    if (!demand.has_value() || demand.value().canonical() <= 0) {
      return make_error(ReasonCode::ZeroProfile,
                        "profile demand for " + std::string(dimension_text(dimension)) + " must be positive");
    }
  }

  FragmentationReport report;
  report.profile_name = profile.name;
  report.rack_units_per_rack = profile.rack_units;

  // Aggregate totals used for the ideal bound. Only enclosures that are
  // structurally usable and fully evaluable contribute; anything else is
  // reported as stranded or as indeterminate rather than being folded in.
  //
  // The ideal ignores gaps as well as enclosure boundaries, so it aggregates
  // free_ru_total (every free slot) while realizable placement uses
  // free_ru_contiguous (the longest run a single rack can occupy). Falling back
  // to the contiguous figure when the total is not declared keeps the bound
  // conservative rather than optimistic.
  std::int64_t total_free_ru = 0;
  std::array<std::int64_t, kDimensionCount> total_free{};
  std::array<bool, kDimensionCount> total_free_known{};
  std::size_t unusable_enclosures = 0;

  for (const EnclosureBudget& enclosure : topology.enclosures()) {
    EnclosureEvaluation evaluation;

    if (!enclosure.structurally_usable) {
      evaluation.structurally_usable = false;
      evaluation.binding = "structurally-unusable";
      evaluation.binding_reason = ReasonCode::StrandedCapacity;
      evaluation.explanation = "enclosure '" + enclosure.id.value() + "' is structurally unusable: " +
                               (enclosure.unusable_reason.empty() ? std::string("no reason recorded")
                                                                  : enclosure.unusable_reason);
      ++unusable_enclosures;
    } else if (!enclosure.free_ru_contiguous.has_value()) {
      evaluation.indeterminate = true;
      evaluation.binding = "rack-units-contiguity";
      evaluation.binding_reason = ReasonCode::AmountUnknown;
      evaluation.explanation = "enclosure '" + enclosure.id.value() +
                               "' does not declare contiguous free rack units, so placement cannot be decided";
    } else {
      const Result<std::int64_t> slot_limit =
          floor_div(enclosure.free_ru_contiguous.value(), profile.rack_units);
      if (!slot_limit.ok()) {
        return slot_limit.status();
      }
      evaluation.has_slot_limit = true;
      evaluation.slot_limit = slot_limit.value();
      evaluation.racks = slot_limit.value();
      evaluation.binding = "rack-units-contiguity";

      for (const Dimension dimension : kAllDimensions) {
        const std::optional<Amount>& demand = profile.per_rack.get(dimension);
        if (!demand.has_value()) {
          continue;
        }
        const std::optional<Amount>& free_amount = enclosure.free.get(dimension);
        if (!free_amount.has_value()) {
          evaluation.indeterminate = true;
          evaluation.binding = std::string(dimension_text(dimension));
          evaluation.binding_reason = ReasonCode::AmountUnknown;
          evaluation.explanation = "enclosure '" + enclosure.id.value() + "' does not declare free " +
                                   std::string(dimension_text(dimension)) +
                                   ", so placement cannot be decided for a profile that demands it";
          break;
        }
        const Result<std::int64_t> limit = rack_limit(free_amount.value(), demand.value(), dimension);
        if (!limit.ok()) {
          return limit.status();
        }
        evaluation.dimension_limits[dimension_index(dimension)] = limit.value();
        if (limit.value() < evaluation.racks) {
          evaluation.racks = limit.value();
          evaluation.binding = std::string(dimension_text(dimension));
          evaluation.binding_reason = ReasonCode::BindingConstraint;
        }
      }
      if (evaluation.racks == evaluation.slot_limit && evaluation.binding == "rack-units-contiguity") {
        evaluation.binding_reason = ReasonCode::BindingConstraint;
      }
    }

    // Stranded: everything free in an enclosure that can host none of the
    // profile. Headroom: what remains after the realizable racks are placed.
    // An indeterminate enclosure reports neither: claiming a split for capacity
    // that could not be evaluated would turn an unknown into a number.
    //
    // Rack units are declared either as a dimension budget or through the
    // dedicated free_ru_total field; both denote the same free capacity, so the
    // dedicated field is used as the budget when the vector does not carry it.
    const auto free_budget = [&enclosure](Dimension dimension) -> std::optional<Amount> {
      const std::optional<Amount>& declared = enclosure.free.get(dimension);
      if (declared.has_value()) {
        return declared;
      }
      if (dimension == Dimension::RackUnits && enclosure.free_ru_total.has_value()) {
        return Amount(enclosure.free_ru_total.value());
      }
      return std::nullopt;
    };

    for (const Dimension dimension : kAllDimensions) {
      if (evaluation.indeterminate) {
        break;
      }
      const std::optional<Amount> free_amount = free_budget(dimension);
      if (!free_amount.has_value()) {
        continue;
      }
      if (evaluation.racks <= 0) {
        evaluation.stranded.set(dimension, free_amount.value());
        evaluation.headroom.set(dimension, free_amount.value());
      } else {
        evaluation.stranded.set(dimension, Amount(0));
        const std::optional<Amount>& demand = profile.per_rack.get(dimension);
        if (!demand.has_value()) {
          evaluation.headroom.set(dimension, free_amount.value());
        } else {
          const Result<Amount> used = checked_mul(demand.value(), evaluation.racks);
          if (!used.ok()) {
            return used.status().with_context("headroom for enclosure " + enclosure.id.value());
          }
          const Result<Amount> remaining = checked_sub(free_amount.value(), used.value());
          if (!remaining.ok()) {
            return remaining.status().with_context("headroom for enclosure " + enclosure.id.value());
          }
          evaluation.headroom.set(dimension, remaining.value());
        }
      }
    }

    if (evaluation.explanation.empty()) {
      evaluation.explanation = "enclosure '" + enclosure.id.value() + "' hosts " +
                               std::to_string(evaluation.racks) + " rack(s) of " +
                               std::to_string(profile.rack_units) + "U; binding constraint: " + evaluation.binding;
    }

    if (evaluation.structurally_usable && !evaluation.indeterminate) {
      const std::int64_t free_ru = enclosure.free_ru_total.has_value()
                                       ? enclosure.free_ru_total.value()
                                       : enclosure.free_ru_contiguous.value();
      const Result<Amount> contiguity = checked_add(Amount(total_free_ru), Amount(free_ru));
      if (!contiguity.ok()) {
        return contiguity.status();
      }
      total_free_ru = contiguity.value().canonical();
      for (const Dimension dimension : kAllDimensions) {
        if (!demanded.contains(dimension)) {
          continue;
        }
        const std::optional<Amount>& free_amount = enclosure.free.get(dimension);
        if (!free_amount.has_value()) {
          continue;
        }
        const Result<Amount> summed = checked_add(Amount(total_free[dimension_index(dimension)]), free_amount.value());
        if (!summed.ok()) {
          return summed.status();
        }
        total_free[dimension_index(dimension)] = summed.value().canonical();
        total_free_known[dimension_index(dimension)] = true;
      }
    }

    EnclosureCapacity capacity;
    capacity.id = enclosure.id;
    capacity.scope = enclosure.scope;
    capacity.realizable_racks = evaluation.indeterminate ? 0 : evaluation.racks;
    capacity.binding = evaluation.binding;
    capacity.binding_reason = evaluation.binding_reason;
    capacity.indeterminate = evaluation.indeterminate;
    capacity.stranded = evaluation.stranded;
    capacity.headroom = evaluation.headroom;
    capacity.explanation = evaluation.explanation;

    if (evaluation.indeterminate) {
      report.indeterminate = true;
      ++report.indeterminate_enclosures;
    } else {
      const Result<Amount> summed_racks = checked_add(Amount(report.realizable_racks), Amount(evaluation.racks));
      if (!summed_racks.ok()) {
        return summed_racks.status();
      }
      report.realizable_racks = summed_racks.value().canonical();
      if (evaluation.racks == 0 && evaluation.structurally_usable) {
        report.constrained_dimensions.add(Dimension::RackUnits);
      }
    }

    if (evaluation.binding_reason == ReasonCode::BindingConstraint && evaluation.binding != "rack-units-contiguity") {
      const Result<Dimension> binding_dimension = dimension_from_text(evaluation.binding);
      if (binding_dimension.ok()) {
        report.constrained_dimensions.add(binding_dimension.value());
      }
    }

    const Result<void> stranded_total = report.stranded.accumulate(evaluation.stranded);
    if (!stranded_total.ok()) {
      return stranded_total.status();
    }
    const Result<void> headroom_total = report.headroom.accumulate(evaluation.headroom);
    if (!headroom_total.ok()) {
      return headroom_total.status();
    }

    report.enclosures.push_back(std::move(capacity));
  }

  // Ideal bound: aggregate free capacity divided by per-rack demand, ignoring
  // enclosure boundaries and gaps. Excludes structurally unusable capacity.
  std::int64_t ideal = 0;
  {
    const Result<std::int64_t> ideal_slots = floor_div(total_free_ru, profile.rack_units);
    if (!ideal_slots.ok()) {
      return ideal_slots.status();
    }
    ideal = ideal_slots.value();
    report.ideal_racks_by_dimension[dimension_index(Dimension::RackUnits)] = ideal_slots.value();
  }
  for (const Dimension dimension : kAllDimensions) {
    if (!demanded.contains(dimension)) {
      continue;
    }
    if (!total_free_known[dimension_index(dimension)]) {
      // No evaluable enclosure declared this dimension, so no aggregate bound can
      // be stated for it. report.indeterminate is driven by the enclosure loop:
      // a topology whose enclosures are all unusable is determinate (nothing can
      // be placed, everything is stranded), not indeterminate.
      continue;
    }
    const std::optional<Amount>& demand = profile.per_rack.get(dimension);
    const Result<std::int64_t> limit =
        floor_div(total_free[dimension_index(dimension)], demand.value().canonical());
    if (!limit.ok()) {
      return limit.status();
    }
    report.ideal_racks_by_dimension[dimension_index(dimension)] = limit.value();
    if (limit.value() < ideal) {
      ideal = limit.value();
      report.constrained_dimensions.add(dimension);
    }
  }
  report.ideal_racks = ideal;

  // Proved invariant: sum over enclosures of floor(free_i/demand) never exceeds
  // floor(sum(free_i)/demand), so the ideal can never be smaller than reality.
  // A violation here means the analysis itself is defective, not the topology.
  if (report.realizable_racks > ideal) {
    return make_error(ReasonCode::InvariantViolation,
                      "fragmentation invariant violated: realizable " + std::to_string(report.realizable_racks) +
                          " exceeds the aggregate ideal " + std::to_string(ideal));
  }
  report.fragmented_racks = ideal - report.realizable_racks;

  for (const Dimension dimension : kAllDimensions) {
    const std::optional<std::int64_t>& ideal_for_dimension = report.ideal_racks_by_dimension[dimension_index(dimension)];
    if (!ideal_for_dimension.has_value()) {
      continue;
    }
    if (dimension == Dimension::RackUnits) {
      report.fragmented_by_dimension[dimension_index(dimension)] =
          ideal_for_dimension.value() - report.realizable_racks;
      continue;
    }
    if (!demanded.contains(dimension)) {
      continue;
    }
    report.fragmented_by_dimension[dimension_index(dimension)] =
        ideal_for_dimension.value() - report.realizable_racks;
  }

  report.explanations.push_back("profile '" + profile.name + "' places one rack of " +
                                std::to_string(profile.rack_units) + "U per placement, demanding " +
                                std::to_string(demanded.count()) + " dimension(s)");
  report.explanations.push_back("realizable racks across " + std::to_string(report.enclosures.size()) +
                                " enclosure(s) is " + std::to_string(report.realizable_racks) +
                                "; the aggregate bound that ignores enclosure boundaries and gaps is " +
                                std::to_string(report.ideal_racks));
  report.explanations.push_back("fragmentation is " + std::to_string(report.fragmented_racks) +
                                " rack(s): capacity that the aggregate suggests and enclosure reality denies");
  if (unusable_enclosures != 0) {
    report.explanations.push_back(std::to_string(unusable_enclosures) +
                                  " enclosure(s) are structurally unusable and their free capacity is reported as "
                                  "stranded, not as fragmented or ideal");
  }
  if (report.indeterminate) {
    report.explanations.push_back(std::to_string(report.indeterminate_enclosures) +
                                  " enclosure(s) could not be evaluated, so realizable and ideal counts are lower "
                                  "bounds over the evaluable set");
  }

  return Result<FragmentationReport>(std::move(report));
}

}  // namespace co
