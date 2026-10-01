// Capacity Observatory - placement topology construction and scoping.
#include "capacity_observatory/topology.hpp"

#include <string>
#include <utility>

namespace co {
namespace {

Result<void> validate_free_amounts(const EnclosureBudget& enclosure) {
  for (const Dimension dimension : kAllDimensions) {
    const std::optional<Amount>& value = enclosure.free.get(dimension);
    if (!value.has_value()) {
      continue;
    }
    if (value.value().is_negative()) {
      return make_error(ReasonCode::NegativeAmount,
                        "enclosure '" + enclosure.id.value() + "' declares negative free " +
                            std::string(dimension_text(dimension)) + " of " +
                            std::to_string(value.value().canonical()));
    }
  }
  return Result<void>{};
}

}  // namespace

Result<Topology> Topology::make(std::vector<EnclosureBudget> enclosures) {
  for (const EnclosureBudget& enclosure : enclosures) {
    if (enclosure.id.empty()) {
      return make_error(ReasonCode::InvalidIdentifier, "enclosure id is empty");
    }
    if (enclosure.scope.empty()) {
      return make_error(ReasonCode::InvalidArgument, "enclosure '" + enclosure.id.value() + "' has an empty scope");
    }
    if (enclosure.free_ru_contiguous.has_value() && enclosure.free_ru_contiguous.value() < 0) {
      return make_error(ReasonCode::NegativeAmount,
                        "enclosure '" + enclosure.id.value() + "' declares negative contiguous rack units");
    }
    if (enclosure.free_ru_total.has_value() && enclosure.free_ru_total.value() < 0) {
      return make_error(ReasonCode::NegativeAmount,
                        "enclosure '" + enclosure.id.value() + "' declares negative total rack units");
    }
    if (enclosure.free_ru_contiguous.has_value() && enclosure.free_ru_total.has_value() &&
        enclosure.free_ru_contiguous.value() > enclosure.free_ru_total.value()) {
      return make_error(ReasonCode::SlotOverflow,
                        "enclosure '" + enclosure.id.value() + "' declares " +
                            std::to_string(enclosure.free_ru_contiguous.value()) +
                            " contiguous rack units which exceeds its total free rack units of " +
                            std::to_string(enclosure.free_ru_total.value()));
    }
    const Result<void> amounts = validate_free_amounts(enclosure);
    if (!amounts.ok()) {
      return amounts.status();
    }
  }

  for (std::size_t i = 0; i < enclosures.size(); ++i) {
    for (std::size_t j = i + 1; j < enclosures.size(); ++j) {
      if (enclosures[i].id == enclosures[j].id) {
        return make_error(ReasonCode::AlreadyExists,
                          "enclosure '" + enclosures[i].id.value() + "' appears more than once");
      }
    }
  }

  Topology topology;
  topology.enclosures_ = std::move(enclosures);
  return Result<Topology>(std::move(topology));
}

Result<Topology> Topology::narrowed_to(const ScopePath& scope) const {
  if (scope.empty()) {
    return make_error(ReasonCode::InvalidArgument, "scope filter is empty");
  }
  std::vector<EnclosureBudget> kept;
  for (const EnclosureBudget& enclosure : enclosures_) {
    if (scope.is_prefix_of(enclosure.scope)) {
      kept.push_back(enclosure);
    }
  }
  return make(std::move(kept));
}

const EnclosureBudget* Topology::find(const EnclosureId& id) const noexcept {
  for (const EnclosureBudget& enclosure : enclosures_) {
    if (enclosure.id == id) {
      return &enclosure;
    }
  }
  return nullptr;
}

}  // namespace co
