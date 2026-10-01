#pragma once
// Capacity Observatory - dimensional quantities with exact checked arithmetic.
//
// Every capacity magnitude is an exact integer in the canonical unit of its
// dimension. Conversion from a declared unit is exact or refused; it is never
// rounded. Unknown is represented as std::nullopt and is never silently turned
// into zero.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"

namespace co {

enum class Dimension : std::uint8_t {
  Space = 0,          // canonical unit: square millimetre (mm2)
  RackUnits = 1,      // canonical unit: rack unit (U)
  Power = 2,          // canonical unit: milliwatt (mW)
  Cooling = 3,        // canonical unit: milliwatt thermal (mWth)
  Weight = 4,         // canonical unit: gram (g)
  Serviceability = 5  // canonical unit: one serviceable position (count)
};

inline constexpr std::size_t kDimensionCount = 6;

inline constexpr std::array<Dimension, kDimensionCount> kAllDimensions{
    Dimension::Space,   Dimension::RackUnits, Dimension::Power,
    Dimension::Cooling, Dimension::Weight,    Dimension::Serviceability};

[[nodiscard]] constexpr std::size_t dimension_index(Dimension dimension) noexcept {
  return static_cast<std::size_t>(dimension);
}

[[nodiscard]] std::string_view dimension_text(Dimension dimension) noexcept;
[[nodiscard]] Result<Dimension> dimension_from_text(std::string_view text);

// Canonical unit symbol for a dimension, e.g. "mW" for power.
[[nodiscard]] std::string_view canonical_unit_text(Dimension dimension) noexcept;

// ---------------------------------------------------------------------------
// Amount: a signed exact magnitude in canonical units.
// Signed because unexplained residuals and headroom deltas can be negative.
// ---------------------------------------------------------------------------
class Amount {
 public:
  constexpr Amount() noexcept = default;
  explicit constexpr Amount(std::int64_t canonical) noexcept : canonical_(canonical) {}

  [[nodiscard]] constexpr std::int64_t canonical() const noexcept { return canonical_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return canonical_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return canonical_ < 0; }

  friend constexpr bool operator==(Amount a, Amount b) noexcept { return a.canonical_ == b.canonical_; }
  friend constexpr bool operator!=(Amount a, Amount b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Amount a, Amount b) noexcept { return a.canonical_ < b.canonical_; }
  friend constexpr bool operator<=(Amount a, Amount b) noexcept { return a.canonical_ <= b.canonical_; }
  friend constexpr bool operator>(Amount a, Amount b) noexcept { return a.canonical_ > b.canonical_; }
  friend constexpr bool operator>=(Amount a, Amount b) noexcept { return a.canonical_ >= b.canonical_; }

 private:
  std::int64_t canonical_{0};
};

[[nodiscard]] Result<Amount> checked_add(Amount a, Amount b);
[[nodiscard]] Result<Amount> checked_sub(Amount a, Amount b);
[[nodiscard]] Result<Amount> checked_mul(Amount a, std::int64_t factor);
[[nodiscard]] Result<Amount> checked_div(Amount a, std::int64_t divisor);
[[nodiscard]] Result<Amount> checked_negate(Amount a);
[[nodiscard]] Result<Amount> non_negative(Amount a, std::string_view what);

// Exact evaluation of value * numerator / denominator.
// Refuses with InexactScale rather than rounding; refuses with Overflow when the
// exact result is not representable.
[[nodiscard]] Result<Amount> scale_exact(Amount value, std::int64_t numerator, std::int64_t denominator);

// Floor division that stays correct for negative dividends.
[[nodiscard]] Result<std::int64_t> floor_div(std::int64_t dividend, std::int64_t divisor);

// Sum of a sequence of amounts with overflow detection and an explicit count of
// the values that were unknown (nullopt) rather than zero.
struct SumResult {
  std::optional<Amount> total;  // nullopt when any input was unknown
  std::size_t unknown_inputs{0};
  std::size_t known_inputs{0};
};

[[nodiscard]] Result<SumResult> checked_sum(const std::optional<Amount>* values, std::size_t count);

// ---------------------------------------------------------------------------
// Unit: an exact rational multiple of the canonical unit.
// ---------------------------------------------------------------------------
class Unit {
 public:
  Unit() = default;

  [[nodiscard]] static Unit canonical(Dimension dimension) noexcept;
  [[nodiscard]] static Result<Unit> make(Dimension dimension, std::int64_t numerator, std::int64_t denominator);
  [[nodiscard]] static Result<Unit> parse(std::string_view text);
  [[nodiscard]] static Result<Unit> parse_for(Dimension dimension, std::string_view text);

  [[nodiscard]] constexpr Dimension dimension() const noexcept { return dimension_; }
  [[nodiscard]] constexpr std::int64_t numerator() const noexcept { return numerator_; }
  [[nodiscard]] constexpr std::int64_t denominator() const noexcept { return denominator_; }
  [[nodiscard]] std::string symbol() const;

  // Converts a declared magnitude into canonical units, exactly.
  [[nodiscard]] Result<Amount> to_canonical(std::int64_t declared) const;
  [[nodiscard]] bool is_canonical() const noexcept { return numerator_ == 1 && denominator_ == 1; }

  friend bool operator==(const Unit& a, const Unit& b) noexcept {
    return a.dimension_ == b.dimension_ && a.numerator_ == b.numerator_ && a.denominator_ == b.denominator_;
  }

 private:
  Dimension dimension_{Dimension::Space};
  std::int64_t numerator_{1};
  std::int64_t denominator_{1};
};

// ---------------------------------------------------------------------------
// AmountVector: one optional amount per dimension. nullopt means UNKNOWN.
// ---------------------------------------------------------------------------
class AmountVector {
 public:
  AmountVector() = default;

  [[nodiscard]] const std::optional<Amount>& get(Dimension dimension) const noexcept {
    return values_[dimension_index(dimension)];
  }
  void set(Dimension dimension, Amount amount) noexcept { values_[dimension_index(dimension)] = amount; }
  void set_unknown(Dimension dimension) noexcept { values_[dimension_index(dimension)] = std::nullopt; }

  [[nodiscard]] bool has(Dimension dimension) const noexcept { return get(dimension).has_value(); }
  [[nodiscard]] std::size_t known_count() const noexcept;
  [[nodiscard]] std::size_t unknown_count() const noexcept { return kDimensionCount - known_count(); }
  [[nodiscard]] bool is_complete() const noexcept { return known_count() == kDimensionCount; }
  [[nodiscard]] bool is_empty() const noexcept { return known_count() == 0; }

  [[nodiscard]] const std::optional<Amount>* data() const noexcept { return values_.data(); }

  // Adds every known component of 'other' into this vector. Unknown components
  // stay unknown. Fails on overflow; a failed accumulate leaves the vector
  // unchanged.
  [[nodiscard]] Result<void> accumulate(const AmountVector& other);

  [[nodiscard]] static Result<AmountVector> sum(const AmountVector* values, std::size_t count);

  friend bool operator==(const AmountVector& a, const AmountVector& b) noexcept { return a.values_ == b.values_; }

 private:
  std::array<std::optional<Amount>, kDimensionCount> values_{};
};

// ---------------------------------------------------------------------------
// DimensionSet: an explicit set of dimensions.
// ---------------------------------------------------------------------------
class DimensionSet {
 public:
  constexpr DimensionSet() noexcept = default;

  [[nodiscard]] static constexpr DimensionSet all() noexcept {
    DimensionSet set;
    set.mask_ = 0x3F;
    return set;
  }
  [[nodiscard]] static constexpr DimensionSet of(Dimension dimension) noexcept {
    DimensionSet set;
    set.mask_ = static_cast<std::uint8_t>(1u << dimension_index(dimension));
    return set;
  }

  [[nodiscard]] constexpr bool contains(Dimension dimension) const noexcept {
    return (mask_ & static_cast<std::uint8_t>(1u << dimension_index(dimension))) != 0;
  }
  constexpr void add(Dimension dimension) noexcept {
    mask_ = static_cast<std::uint8_t>(mask_ | (1u << dimension_index(dimension)));
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return mask_ == 0; }
  [[nodiscard]] constexpr std::uint8_t mask() const noexcept { return mask_; }
  [[nodiscard]] constexpr std::size_t count() const noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < kDimensionCount; ++i) {
      if ((mask_ & static_cast<std::uint8_t>(1u << i)) != 0) {
        ++total;
      }
    }
    return total;
  }

  friend constexpr bool operator==(DimensionSet a, DimensionSet b) noexcept { return a.mask_ == b.mask_; }

 private:
  std::uint8_t mask_{0};
};

[[nodiscard]] std::string dimension_set_text(DimensionSet set);

}  // namespace co
