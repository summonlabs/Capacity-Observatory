// Capacity Observatory - dimensional quantities and checked arithmetic.
#include "capacity_observatory/dimension.hpp"

#include <limits>
#include <string>

namespace co {
namespace {

constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();

std::int64_t gcd_positive(std::int64_t a, std::int64_t b) {
  if (a < 0) a = -a;
  if (b < 0) b = -b;
  while (b != 0) {
    const std::int64_t remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

struct UnitDefinition {
  std::string_view symbol;
  Dimension dimension;
  std::int64_t numerator;
  std::int64_t denominator;
};

// Declared units, each an exact rational multiple of the canonical unit.
constexpr UnitDefinition kUnitTable[] = {
    {"mm2", Dimension::Space, 1, 1},
    {"m2", Dimension::Space, 1000000, 1},
    {"ft2", Dimension::Space, 9290304, 100},
    {"U", Dimension::RackUnits, 1, 1},
    {"mW", Dimension::Power, 1, 1},
    {"W", Dimension::Power, 1000, 1},
    {"kW", Dimension::Power, 1000000, 1},
    {"MW", Dimension::Power, 1000000000, 1},
    {"mWth", Dimension::Cooling, 1, 1},
    {"Wth", Dimension::Cooling, 1000, 1},
    {"kWth", Dimension::Cooling, 1000000, 1},
    {"MWth", Dimension::Cooling, 1000000000, 1},
    {"g", Dimension::Weight, 1, 1},
    {"kg", Dimension::Weight, 1000, 1},
    {"t", Dimension::Weight, 1000000, 1},
    {"lb", Dimension::Weight, 45359237, 100000},
    {"service", Dimension::Serviceability, 1, 1},
};

const UnitDefinition* find_unit(std::string_view symbol) {
  for (const UnitDefinition& candidate : kUnitTable) {
    if (candidate.symbol == symbol) {
      return &candidate;
    }
  }
  return nullptr;
}

}  // namespace

std::string_view dimension_text(Dimension dimension) noexcept {
  switch (dimension) {
    case Dimension::Space:
      return "space";
    case Dimension::RackUnits:
      return "rack-units";
    case Dimension::Power:
      return "power";
    case Dimension::Cooling:
      return "cooling";
    case Dimension::Weight:
      return "weight";
    case Dimension::Serviceability:
      return "serviceability";
  }
  return "unknown";
}

Result<Dimension> dimension_from_text(std::string_view text) {
  for (const Dimension candidate : kAllDimensions) {
    if (dimension_text(candidate) == text) {
      return Result<Dimension>(candidate);
    }
  }
  return make_error(ReasonCode::InvalidArgument, "unknown dimension '" + std::string(text) + "'");
}

std::string_view canonical_unit_text(Dimension dimension) noexcept {
  switch (dimension) {
    case Dimension::Space:
      return "mm2";
    case Dimension::RackUnits:
      return "U";
    case Dimension::Power:
      return "mW";
    case Dimension::Cooling:
      return "mWth";
    case Dimension::Weight:
      return "g";
    case Dimension::Serviceability:
      return "service";
  }
  return "unknown";
}

Result<Amount> checked_add(Amount a, Amount b) {
  if (b.canonical() > 0 && a.canonical() > kMax - b.canonical()) {
    return make_error(ReasonCode::Overflow,
                      "addition overflow: " + std::to_string(a.canonical()) + " + " + std::to_string(b.canonical()));
  }
  if (b.canonical() < 0 && a.canonical() < kMin - b.canonical()) {
    return make_error(ReasonCode::Overflow,
                      "addition overflow: " + std::to_string(a.canonical()) + " + " + std::to_string(b.canonical()));
  }
  return Result<Amount>(Amount(a.canonical() + b.canonical()));
}

Result<Amount> checked_sub(Amount a, Amount b) {
  if (b.canonical() == kMin) {
    return make_error(ReasonCode::Overflow, "subtraction overflow: the subtrahend is the minimum representable value");
  }
  return checked_add(a, Amount(-b.canonical()));
}

Result<Amount> checked_mul(Amount a, std::int64_t factor) {
  const std::int64_t left = a.canonical();
  const std::int64_t right = factor;
  if (left == 0 || right == 0) {
    return Result<Amount>(Amount(0));
  }
  if (left == kMin && right == -1) {
    return make_error(ReasonCode::Overflow, "multiplication overflow");
  }
  if (right == kMin && left == -1) {
    return make_error(ReasonCode::Overflow, "multiplication overflow");
  }

  const std::int64_t result = left * right;
  // Verified by division: exact unless the operation overflowed.
  if (result / right != left) {
    return make_error(ReasonCode::Overflow,
                      "multiplication overflow: " + std::to_string(left) + " * " + std::to_string(right));
  }
  return Result<Amount>(Amount(result));
}

Result<Amount> checked_div(Amount a, std::int64_t divisor) {
  if (divisor == 0) {
    return make_error(ReasonCode::DivisionByZero, "checked division by zero");
  }
  if (a.canonical() == kMin && divisor == -1) {
    return make_error(ReasonCode::Overflow, "division overflow: minimum value divided by -1");
  }
  return Result<Amount>(Amount(a.canonical() / divisor));
}

Result<Amount> checked_negate(Amount a) {
  if (a.canonical() == kMin) {
    return make_error(ReasonCode::Overflow, "negation overflow");
  }
  return Result<Amount>(Amount(-a.canonical()));
}

Result<Amount> non_negative(Amount a, std::string_view what) {
  if (a.is_negative()) {
    return make_error(ReasonCode::NegativeAmount,
                      std::string(what) + " must not be negative but is " + std::to_string(a.canonical()));
  }
  return Result<Amount>(a);
}

Result<Amount> scale_exact(Amount value, std::int64_t numerator, std::int64_t denominator) {
  if (numerator <= 0 || denominator <= 0) {
    return make_error(ReasonCode::InvalidArgument, "unit scale factors must be positive");
  }
  const std::int64_t divisor = gcd_positive(numerator, denominator);
  const std::int64_t reduced_numerator = numerator / divisor;
  const std::int64_t reduced_denominator = denominator / divisor;

  const std::int64_t raw = value.canonical();
  // gcd(reduced_numerator, reduced_denominator) == 1, so exactness is decided by
  // the denominator alone. Dividing first also avoids a spurious overflow when
  // the product could not be represented but the quotient can.
  if (raw % reduced_denominator != 0) {
    return make_error(ReasonCode::InexactScale,
                      "conversion of " + std::to_string(raw) + " by " + std::to_string(numerator) + "/" +
                          std::to_string(denominator) + " is not an exact integer and was refused, not rounded");
  }
  const std::int64_t quotient = raw / reduced_denominator;
  return checked_mul(Amount(quotient), reduced_numerator);
}

Result<std::int64_t> floor_div(std::int64_t dividend, std::int64_t divisor) {
  if (divisor == 0) {
    return make_error(ReasonCode::DivisionByZero, "floor division by zero");
  }
  if (dividend == kMin && divisor == -1) {
    return make_error(ReasonCode::Overflow, "floor division overflow");
  }
  std::int64_t quotient = dividend / divisor;
  const std::int64_t remainder = dividend % divisor;
  if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
    --quotient;
  }
  return Result<std::int64_t>(quotient);
}

Result<SumResult> checked_sum(const std::optional<Amount>* values, std::size_t count) {
  SumResult result;
  std::int64_t running = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (!values[i].has_value()) {
      ++result.unknown_inputs;
      continue;
    }
    ++result.known_inputs;
    const Result<Amount> added = checked_add(Amount(running), values[i].value());
    if (!added.ok()) {
      return added.status();
    }
    running = added.value().canonical();
  }
  if (result.unknown_inputs == 0) {
    result.total = Amount(running);
  }
  return Result<SumResult>(result);
}

std::string dimension_set_text(DimensionSet set) {
  std::string out;
  for (const Dimension dimension : kAllDimensions) {
    if (!set.contains(dimension)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out.append(dimension_text(dimension));
  }
  return out;
}

Unit Unit::canonical(Dimension dimension) noexcept {
  Unit unit;
  unit.dimension_ = dimension;
  unit.numerator_ = 1;
  unit.denominator_ = 1;
  return unit;
}

Result<Unit> Unit::make(Dimension dimension, std::int64_t numerator, std::int64_t denominator) {
  if (numerator <= 0 || denominator <= 0) {
    return make_error(ReasonCode::InvalidArgument, "unit scale factors must be positive");
  }
  const std::int64_t divisor = gcd_positive(numerator, denominator);
  Unit unit;
  unit.dimension_ = dimension;
  unit.numerator_ = numerator / divisor;
  unit.denominator_ = denominator / divisor;
  return Result<Unit>(unit);
}

Result<Unit> Unit::parse(std::string_view text) {
  const UnitDefinition* definition = find_unit(text);
  if (definition == nullptr) {
    return make_error(ReasonCode::UnknownUnit, "unit '" + std::string(text) + "' is not supported");
  }
  return make(definition->dimension, definition->numerator, definition->denominator);
}

Result<Unit> Unit::parse_for(Dimension dimension, std::string_view text) {
  const Result<Unit> parsed = parse(text);
  if (!parsed.ok()) {
    return parsed.status();
  }
  if (parsed.value().dimension() != dimension) {
    return make_error(ReasonCode::DimensionMismatch,
                      "unit '" + std::string(text) + "' denotes " +
                          std::string(dimension_text(parsed.value().dimension())) + " but " +
                          std::string(dimension_text(dimension)) + " was expected");
  }
  return parsed;
}

std::string Unit::symbol() const {
  for (const UnitDefinition& candidate : kUnitTable) {
    if (candidate.dimension == dimension_ && candidate.numerator == numerator_ &&
        candidate.denominator == denominator_) {
      return std::string(candidate.symbol);
    }
  }
  return std::string(canonical_unit_text(dimension_)) + "*" + std::to_string(numerator_) + "/" +
         std::to_string(denominator_);
}

Result<Amount> Unit::to_canonical(std::int64_t declared) const {
  return scale_exact(Amount(declared), numerator_, denominator_);
}

std::size_t AmountVector::known_count() const noexcept {
  std::size_t total = 0;
  for (const std::optional<Amount>& value : values_) {
    if (value.has_value()) {
      ++total;
    }
  }
  return total;
}

Result<void> AmountVector::accumulate(const AmountVector& other) {
  AmountVector staged = *this;
  for (const Dimension dimension : kAllDimensions) {
    const std::optional<Amount>& incoming = other.get(dimension);
    if (!incoming.has_value()) {
      continue;
    }
    const std::optional<Amount>& existing = staged.get(dimension);
    if (!existing.has_value()) {
      staged.set(dimension, incoming.value());
      continue;
    }
    const Result<Amount> sum = checked_add(existing.value(), incoming.value());
    if (!sum.ok()) {
      return sum.status();
    }
    staged.set(dimension, sum.value());
  }
  *this = staged;
  return Result<void>{};
}

Result<AmountVector> AmountVector::sum(const AmountVector* values, std::size_t count) {
  AmountVector total;
  for (std::size_t i = 0; i < count; ++i) {
    const Result<void> accumulated = total.accumulate(values[i]);
    if (!accumulated.ok()) {
      return accumulated.status();
    }
  }
  return Result<AmountVector>(total);
}

}  // namespace co
