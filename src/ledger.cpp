// Capacity Observatory - ledger composition and closure identities.
#include "capacity_observatory/ledger.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "capacity_observatory/dimension.hpp"

namespace co {
namespace {

using Maybe = std::optional<Amount>;

// The classes that participate in the ledger identities. A line is complete only
// when every one of them is declared and the three equality closures hold.
constexpr std::array<CapacityAssertion, 9> kIdentityClasses{
    CapacityAssertion::Installed,           CapacityAssertion::Committed, CapacityAssertion::Reserved,
    CapacityAssertion::Stranded,            CapacityAssertion::Disputed,  CapacityAssertion::ExcludedFailure,
    CapacityAssertion::ExcludedMaintenance, CapacityAssertion::Available, CapacityAssertion::Nameplate};
constexpr std::size_t kIdentityClassCount = kIdentityClasses.size();

// Sum of optional parts. Unknown anywhere makes the total unknown: there is no
// missing-to-zero conversion anywhere in this file.
Result<Maybe> sum_of(const std::vector<Maybe>& parts) {
  Amount running(0);
  for (const Maybe& part : parts) {
    if (!part.has_value()) {
      return Result<Maybe>(Maybe{});
    }
    const Result<Amount> added = checked_add(running, part.value());
    if (!added.ok()) {
      return added.status();
    }
    running = added.value();
  }
  return Result<Maybe>(Maybe(running));
}

// base minus every part; unknown anywhere makes the result unknown.
Result<Maybe> subtract_all(const Maybe& base, const std::vector<Maybe>& parts) {
  if (!base.has_value()) {
    return Result<Maybe>(Maybe{});
  }
  Amount running = base.value();
  for (const Maybe& part : parts) {
    if (!part.has_value()) {
      return Result<Maybe>(Maybe{});
    }
    const Result<Amount> difference = checked_sub(running, part.value());
    if (!difference.ok()) {
      return difference.status();
    }
    running = difference.value();
  }
  return Result<Maybe>(Maybe(running));
}

Freshness worse_freshness(Freshness a, Freshness b) {
  const auto severity = [](Freshness value) {
    switch (value) {
      case Freshness::Fresh: return 0;
      case Freshness::Future: return 1;
      case Freshness::Recovered: return 2;
      case Freshness::Stale: return 3;
      case Freshness::Unknown: return 4;
    }
    return 4;
  };
  return severity(a) >= severity(b) ? a : b;
}

std::string amount_text(const Maybe& amount) {
  return amount.has_value() ? std::to_string(amount.value().canonical()) : std::string("unknown");
}

}  // namespace

std::size_t ClassAmounts::known_count() const noexcept {
  std::size_t total = 0;
  for (const std::optional<Amount>& value : values_) {
    if (value.has_value()) {
      ++total;
    }
  }
  return total;
}

std::string_view line_state_text(LineState state) noexcept {
  switch (state) {
    case LineState::Complete: return "complete";
    case LineState::Residual: return "residual";
    case LineState::Partial: return "partial";
    case LineState::Indeterminate: return "indeterminate";
  }
  return "unknown";
}

const LedgerLine* Ledger::find(Dimension dimension) const noexcept {
  for (const LedgerLine& line : lines) {
    if (line.dimension == dimension) {
      return &line;
    }
  }
  return nullptr;
}

bool Ledger::has_residuals() const noexcept {
  for (const LedgerLine& line : lines) {
    if (line.unexplained_governance.has_value() && !line.unexplained_governance.value().is_zero()) {
      return true;
    }
    if (line.unexplained_installed.has_value() && !line.unexplained_installed.value().is_zero()) {
      return true;
    }
    if (line.unexplained_available.has_value() && !line.unexplained_available.value().is_zero()) {
      return true;
    }
  }
  return false;
}

bool Ledger::has_overcommitment() const noexcept {
  for (const LedgerLine& line : lines) {
    if (line.overcommitted) {
      return true;
    }
  }
  return false;
}

std::size_t Ledger::incomplete_lines() const noexcept {
  std::size_t total = 0;
  for (const LedgerLine& line : lines) {
    if (line.state == LineState::Partial || line.state == LineState::Indeterminate) {
      ++total;
    }
  }
  return total;
}

Result<Ledger> compose_ledger(const EvidenceWindow& window, const LedgerRequest& request) {
  if (request.scope.empty()) {
    return make_error(ReasonCode::InvalidArgument, "ledger request scope is empty");
  }

  Ledger ledger;
  ledger.scope = request.scope;

  for (const Dimension dimension : kAllDimensions) {
    if (!request.dimensions.contains(dimension)) {
      continue;
    }

    LedgerLine line;
    line.scope = request.scope;
    line.dimension = dimension;
    // Starts at the best freshness and degrades as records are seen; a line with
    // no records at all is set back to Unknown below rather than reported fresh.
    line.worst_freshness = Freshness::Fresh;

    std::vector<Maybe> per_assertion(kAssertionCount);
    std::set<std::string> scopes;
    std::vector<std::string> ids;
    std::size_t matched = 0;
    std::set<std::size_t> depths;

    for (const auto& entry : window.current()) {
      const EvidenceRecord& record = entry.second;
      if (record.dimension != dimension) {
        continue;
      }
      if (!request.scope.is_prefix_of(record.scope)) {
        continue;
      }
      ++matched;
      depths.insert(record.scope.depth());
      scopes.insert(record.scope.text());
      ids.push_back(record.id_text());

      const std::size_t index = assertion_index(record.assertion);
      const Result<Amount> added = checked_add(per_assertion[index].value_or(Amount(0)), record.amount);
      if (!added.ok()) {
        return added.status().with_context("ledger aggregation of " + record.id_text());
      }
      per_assertion[index] = added.value();
      line.worst_freshness = worse_freshness(line.worst_freshness, window.freshness_of(record));
    }

    for (std::size_t i = 0; i < kAssertionCount; ++i) {
      if (per_assertion[i].has_value()) {
        line.declared.set(static_cast<CapacityAssertion>(i), per_assertion[i].value());
      }
    }
    line.evidence_count = matched;
    for (const std::string& scope_text : scopes) {
      const Result<ScopePath> parsed = ScopePath::parse(scope_text);
      if (!parsed.ok()) {
        return parsed.status().with_context("contributing scope");
      }
      line.contributing_scopes.push_back(parsed.value());
    }
    std::sort(ids.begin(), ids.end());
    line.evidence_ids = std::move(ids);

    if (matched == 0) {
      line.state = LineState::Indeterminate;
      line.worst_freshness = Freshness::Unknown;
      // Every identity class is unknown here, and saying so keeps a caller that
      // filters on unknown_classes from mistaking an unevaluated line for a
      // fully declared one.
      line.unknown_classes = kIdentityClassCount;
      line.explanations.push_back("no current evidence exists for " + request.scope.text() + " " +
                                  std::string(dimension_text(dimension)));
      line.explanations.push_back("all " + std::to_string(kIdentityClassCount) +
                                  " identity classes are unknown for this line");
      // The four closure identities keep their names and are marked
      // indeterminate. A rendered line therefore has one distinguishable member
      // per identity instead of four anonymous ones, and "not evaluated" is
      // explicit rather than a default holds=false with reason=ok.
      constexpr std::array<std::string_view, 4> kClosureNames{"governance", "installed", "available", "allocation"};
      for (std::size_t index = 0; index < line.closures.size(); ++index) {
        line.closures[index].name = std::string(kClosureNames[index]);
        line.closures[index].indeterminate = true;
        line.closures[index].code = ReasonCode::UndeterminedCapacity;
        line.closures[index].explanation =
            "no current evidence exists for this scope and dimension, so the identity cannot be evaluated";
      }
      ledger.lines.push_back(std::move(line));
      continue;
    }

    const Maybe& nameplate = line.declared.get(CapacityAssertion::Nameplate);
    const Maybe& governed_declared = line.declared.get(CapacityAssertion::Governed);
    const Maybe& planned = line.declared.get(CapacityAssertion::Planned);
    const Maybe& installed = line.declared.get(CapacityAssertion::Installed);
    const Maybe& observed = line.declared.get(CapacityAssertion::Observed);
    const Maybe& reserved = line.declared.get(CapacityAssertion::Reserved);
    const Maybe& committed = line.declared.get(CapacityAssertion::Committed);
    const Maybe& available = line.declared.get(CapacityAssertion::Available);
    const Maybe& stranded = line.declared.get(CapacityAssertion::Stranded);
    const Maybe& disputed = line.declared.get(CapacityAssertion::Disputed);
    const Maybe& excluded_policy = line.declared.get(CapacityAssertion::ExcludedPolicy);
    const Maybe& excluded_maintenance = line.declared.get(CapacityAssertion::ExcludedMaintenance);
    const Maybe& excluded_failure = line.declared.get(CapacityAssertion::ExcludedFailure);
    const Maybe& reserve = line.declared.get(CapacityAssertion::OperationalReserve);

    const Result<Maybe> excluded_total = sum_of({excluded_policy, excluded_maintenance, excluded_failure, reserve});
    if (!excluded_total.ok()) {
      return excluded_total.status().with_context("exclusion total");
    }
    line.exclusion_total = excluded_total.value();

    const Result<Maybe> serviceable = subtract_all(installed, {excluded_failure, excluded_maintenance});
    if (!serviceable.ok()) {
      return serviceable.status().with_context("serviceable capacity");
    }
    line.serviceable = serviceable.value();

    const Result<Maybe> free_usable =
        subtract_all(serviceable.value(), {committed, reserved, stranded, disputed});
    if (!free_usable.ok()) {
      return free_usable.status().with_context("free usable capacity");
    }
    line.free_usable = free_usable.value();

    const Result<Maybe> governed_derived =
        subtract_all(nameplate, {excluded_policy, excluded_maintenance, excluded_failure, reserve});
    if (!governed_derived.ok()) {
      return governed_derived.status().with_context("governed ceiling");
    }
    if (governed_declared.has_value()) {
      line.governed = governed_declared;
      line.governed_derived = false;
    } else {
      line.governed = governed_derived.value();
      line.governed_derived = true;
    }

    // --- Closure 1: governance -------------------------------------------------
    {
      ClosureCheck check;
      check.name = "governance";
      const Result<Maybe> expected =
          sum_of({excluded_policy, excluded_maintenance, excluded_failure, reserve, line.governed});
      if (!expected.ok()) {
        return expected.status().with_context("governance closure");
      }
      if (!nameplate.has_value() || !expected.value().has_value()) {
        check.indeterminate = true;
        check.code = ReasonCode::UnknownNameplate;
        check.explanation = "nameplate is " + std::string(nameplate.has_value() ? "known" : "unknown") +
                            " and the governed partition is " +
                            std::string(expected.value().has_value() ? "known" : "unknown") +
                            ", so the governance identity cannot be evaluated";
      } else {
        const Result<Amount> residual = checked_sub(nameplate.value(), expected.value().value());
        if (!residual.ok()) {
          return residual.status().with_context("governance residual");
        }
        check.residual = residual.value();
        check.holds = residual.value().is_zero();
        check.code = check.holds ? ReasonCode::Ok : ReasonCode::ResidualGovernance;
        check.explanation =
            "nameplate " + std::to_string(nameplate.value().canonical()) + " minus (policy " +
            amount_text(excluded_policy) + " + maintenance " + amount_text(excluded_maintenance) + " + failure " +
            amount_text(excluded_failure) + " + reserve " + amount_text(reserve) + " + governed " +
            amount_text(line.governed) + (line.governed_derived ? " [derived]" : " [declared]") + ") = " +
            std::to_string(residual.value().canonical());
        if (!check.holds) {
          check.explanation += "; the policy authority's declared governed ceiling disagrees with its own nameplate evidence";
        }
      }
      line.unexplained_governance = check.residual;
      line.closures[0] = check;
    }

    // --- Closure 2: installed partition ---------------------------------------
    {
      ClosureCheck check;
      check.name = "installed";
      const Result<Maybe> classes =
          sum_of({excluded_failure, excluded_maintenance, stranded, disputed, committed, reserved, line.free_usable});
      if (!classes.ok()) {
        return classes.status().with_context("installed partition");
      }
      if (!installed.has_value() || !classes.value().has_value()) {
        check.indeterminate = true;
        check.code = ReasonCode::UnknownInstalled;
        check.explanation = "installed is " + std::string(installed.has_value() ? "known" : "unknown") +
                            " and the class partition is " +
                            std::string(classes.value().has_value() ? "known" : "unknown") +
                            "; unknown classes are never treated as zero";
      } else {
        const Result<Amount> residual = checked_sub(installed.value(), classes.value().value());
        if (!residual.ok()) {
          return residual.status().with_context("installed residual");
        }
        check.residual = residual.value();
        check.holds = residual.value().is_zero();
        check.code = check.holds ? ReasonCode::Ok : ReasonCode::ResidualUnallocated;
        check.explanation = "installed " + std::to_string(installed.value().canonical()) +
                            " minus (failure, maintenance, stranded, disputed, committed, reserved, free-usable " +
                            amount_text(line.free_usable) + ") = " + std::to_string(residual.value().canonical());
        if (line.free_usable.has_value() && line.free_usable.value().is_negative()) {
          check.explanation += "; derived free capacity is negative, so allocations exceed installed capacity";
        }
      }
      line.unexplained_installed = check.residual;
      line.closures[1] = check;
    }

    // --- Closure 3: declared available vs computed free ------------------------
    {
      ClosureCheck check;
      check.name = "available";
      if (!available.has_value() || !line.free_usable.has_value()) {
        check.indeterminate = true;
        check.code = ReasonCode::UnknownExclusions;
        check.explanation = "declared available is " +
                            std::string(available.has_value() ? amount_text(available) : "unknown") +
                            " and computed free capacity is " + amount_text(line.free_usable) +
                            ", so the available identity cannot be evaluated";
      } else {
        const Result<Amount> residual = checked_sub(available.value(), line.free_usable.value());
        if (!residual.ok()) {
          return residual.status().with_context("available residual");
        }
        check.residual = residual.value();
        check.holds = residual.value().is_zero();
        check.code = check.holds ? ReasonCode::Ok : ReasonCode::AvailableMismatch;
        check.explanation = "declared available " + std::to_string(available.value().canonical()) +
                            " minus computed free " + std::to_string(line.free_usable.value().canonical()) + " = " +
                            std::to_string(residual.value().canonical());
        if (!check.holds) {
          check.explanation += "; the declared available figure is not explained by installed, exclusion, "
                               "committed, reserved, stranded, and disputed evidence";
        }
      }
      line.unexplained_available = check.residual;
      line.closures[2] = check;
    }

    // --- Closure 4: allocation against the governed ceiling ---------------------
    {
      ClosureCheck check;
      check.name = "allocation";
      const Result<Maybe> allocated = sum_of({committed, reserved});
      if (!allocated.ok()) {
        return allocated.status().with_context("allocation closure");
      }
      if (!allocated.value().has_value() || !line.governed.has_value()) {
        check.indeterminate = true;
        check.code = ReasonCode::UndeterminedCapacity;
        check.explanation = "allocated (" + amount_text(allocated.value()) + ") and governed (" +
                            amount_text(line.governed) + ") must both be known to test over-subscription";
      } else {
        const Result<Amount> residual = checked_sub(allocated.value().value(), line.governed.value());
        if (!residual.ok()) {
          return residual.status().with_context("allocation residual");
        }
        check.residual = residual.value();
        check.holds = residual.value().canonical() <= 0;
        check.code = check.holds ? ReasonCode::Ok : ReasonCode::Overcommitted;
        check.explanation = "committed " + amount_text(committed) + " + reserved " + amount_text(reserved) +
                            " minus governed " + std::to_string(line.governed.value().canonical()) + " = " +
                            std::to_string(residual.value().canonical()) +
                            (check.holds ? " (within the governed ceiling)"
                                         : " (allocations exceed the governed ceiling)");
        if (!check.holds) {
          line.overcommitted = true;
          line.overcommitment = residual.value();
        }
      }
      line.closures[3] = check;
    }

    if (line.free_usable.has_value() && line.free_usable.value().is_negative()) {
      line.overcommitted = true;
      if (!line.overcommitment.has_value() || line.overcommitment.value() < line.free_usable.value()) {
        line.overcommitment = Amount(-line.free_usable.value().canonical());
      }
    }

    // --- State, unknown accounting, and explanation ----------------------------
    std::size_t known = 0;
    std::size_t unknown = 0;
    for (std::size_t i = 0; i < kAssertionCount; ++i) {
      if (per_assertion[i].has_value()) {
        ++known;
      } else {
        ++unknown;
      }
    }
    // Only classes that participate in the ledger identities count toward the
    // completeness verdict.
    std::size_t identity_unknowns = 0;
    for (const CapacityAssertion assertion : kIdentityClasses) {
      if (!line.declared.get(assertion).has_value()) {
        ++identity_unknowns;
      }
    }
    line.unknown_classes = identity_unknowns;

    // Each identity defines what "unexplained" means for it: the three equality
    // identities must close at exactly zero, while the allocation identity is an
    // inequality whose only failure mode is over-commitment. Testing the residual
    // alone would report a healthy line that has spare headroom as residual.
    bool any_residual = false;
    bool any_indeterminate = false;
    for (const ClosureCheck& check : line.closures) {
      if (check.indeterminate) {
        any_indeterminate = true;
      } else if (!check.holds && check.residual.has_value() && !check.residual.value().is_zero()) {
        any_residual = true;
      }
    }

    if (!installed.has_value() && !committed.has_value()) {
      line.state = LineState::Indeterminate;
    } else if (any_residual) {
      line.state = LineState::Residual;
    } else if (identity_unknowns != 0 || any_indeterminate) {
      line.state = LineState::Partial;
    } else {
      line.state = LineState::Complete;
    }

    if (depths.size() > 1) {
      line.explanations.push_back("aggregated evidence from " + std::to_string(depths.size()) +
                                  " scope depths; the line is a roll-up of " + std::to_string(scopes.size()) +
                                  " scopes and is not a single authority figure");
    }
    line.explanations.push_back("declared classes known=" + std::to_string(known) + " unknown=" +
                                std::to_string(unknown) + " over " + std::to_string(matched) + " evidence records");
    line.explanations.push_back("freshness=" + std::string(freshness_text(line.worst_freshness)));
    for (const ClosureCheck& check : line.closures) {
      line.explanations.push_back(check.name + ": " + check.explanation);
    }
    if (planned.has_value()) {
      line.explanations.push_back("planned capacity " + std::to_string(planned.value().canonical()) +
                                  " is declared but is not installed and is excluded from every identity above");
    }
    if (observed.has_value()) {
      line.explanations.push_back("observed capacity " + std::to_string(observed.value().canonical()) +
                                  " is telemetry-backed and never replaces installed evidence");
    }

    ledger.lines.push_back(std::move(line));
  }

  ledger.explanation = "ledger over " + request.scope.text() + " covering " + dimension_set_text(request.dimensions);
  return Result<Ledger>(std::move(ledger));
}

}  // namespace co
