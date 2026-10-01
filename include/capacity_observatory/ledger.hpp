#pragma once
// Capacity Observatory - the explainable ledger and its closure identities.
//
// Every capacity class is either declared by an authority or derived by an
// identity. Nothing is clamped: residuals are computed as declared-minus-derived
// and reported even when negative. Unknown inputs stay unknown and are counted
// so that a partial line can never be mistaken for a complete one.

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/watermark.hpp"

namespace co {

// Declared classes, one slot per CapacityAssertion.
class ClassAmounts {
 public:
  [[nodiscard]] const std::optional<Amount>& get(CapacityAssertion assertion) const noexcept {
    return values_[assertion_index(assertion)];
  }
  void set(CapacityAssertion assertion, Amount amount) noexcept {
    values_[assertion_index(assertion)] = amount;
  }
  [[nodiscard]] std::size_t known_count() const noexcept;

 private:
  std::array<std::optional<Amount>, kAssertionCount> values_{};
};

// One closure identity, evaluated or explicitly indeterminate.
struct ClosureCheck {
  std::string name;
  ReasonCode code{ReasonCode::Ok};
  std::optional<Amount> residual;  // declared minus derived; negative is meaningful
  bool holds{false};               // residual is known and exactly zero
  bool indeterminate{false};
  std::string explanation;
};

enum class LineState : std::uint8_t {
  Complete = 0,       // every identity evaluated and every residual is zero
  Residual = 1,       // every identity evaluated and at least one residual is non-zero
  Partial = 2,        // some classes are known and some are unknown
  Indeterminate = 3   // the line cannot be derived at all (no installed/committed evidence)
};

[[nodiscard]] std::string_view line_state_text(LineState state) noexcept;

struct LedgerLine {
  ScopePath scope;
  Dimension dimension{Dimension::Power};

  ClassAmounts declared;                            // exactly what the authorities asserted
  std::optional<Amount> exclusion_total;            // policy + maintenance + failure + reserve
  std::optional<Amount> serviceable;                // installed - failure - maintenance
  std::optional<Amount> free_usable;                // serviceable - committed - reserved - stranded - disputed
  std::optional<Amount> governed;                   // declared, or derived from nameplate
  bool governed_derived{false};

  std::array<ClosureCheck, 4> closures{};           // governance, installed, available, allocation
  std::optional<Amount> unexplained_governance;
  std::optional<Amount> unexplained_installed;
  std::optional<Amount> unexplained_available;
  std::optional<Amount> overcommitment;             // committed + reserved - governed, when positive
  bool overcommitted{false};

  LineState state{LineState::Indeterminate};
  std::size_t unknown_classes{0};
  std::vector<std::string> explanations;
  std::vector<std::string> evidence_ids;
  std::vector<ScopePath> contributing_scopes;
  std::size_t evidence_count{0};
  Freshness worst_freshness{Freshness::Unknown};
};

struct LedgerRequest {
  ScopePath scope;
  DimensionSet dimensions = DimensionSet::all();
};

struct Ledger {
  ScopePath scope;
  std::vector<LedgerLine> lines;  // ordered by dimension
  std::string explanation;

  [[nodiscard]] const LedgerLine* find(Dimension dimension) const noexcept;
  [[nodiscard]] bool has_residuals() const noexcept;
  [[nodiscard]] bool has_overcommitment() const noexcept;
  [[nodiscard]] std::size_t incomplete_lines() const noexcept;
};

// Aggregates every current evidence record whose scope is at or below the
// requested scope, then evaluates the closure identities per dimension.
[[nodiscard]] Result<Ledger> compose_ledger(const EvidenceWindow& window, const LedgerRequest& request);

}  // namespace co
