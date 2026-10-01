#pragma once
// Capacity Observatory - independent reference model for the property suites.
//
// This header deliberately shares no code, no arithmetic, and no data structure
// with src/ledger.cpp or src/fragmentation.cpp. Every quantity is recomputed
// from the raw evidence list (or the raw enclosure list) with an arbitrary
// precision signed integer, so the reference can detect a wrap, a clamp, or an
// unknown-to-zero conversion that a 64 bit production path might hide.
//
// The reference is written from the published identities, not from the
// implementation: it is the specification expressed a second time.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/ledger.hpp"

namespace co::test::reference {

// ---------------------------------------------------------------------------
// BigInt: arbitrary precision signed integer.
//
// Representation: sign in {-1, 0, +1} plus a little endian base 2^32 magnitude
// with no leading zero limbs. Zero is sign 0 with an empty magnitude.
// ---------------------------------------------------------------------------
namespace big_detail {

using Magnitude = std::vector<std::uint32_t>;

inline void normalize(Magnitude& value) {
  while (!value.empty() && value.back() == 0u) {
    value.pop_back();
  }
}

inline int compare_magnitude(const Magnitude& a, const Magnitude& b) {
  if (a.size() != b.size()) {
    return a.size() < b.size() ? -1 : 1;
  }
  for (std::size_t i = a.size(); i-- > 0;) {
    if (a[i] != b[i]) {
      return a[i] < b[i] ? -1 : 1;
    }
  }
  return 0;
}

inline Magnitude add_magnitude(const Magnitude& a, const Magnitude& b) {
  const std::size_t count = (std::max)(a.size(), b.size());
  Magnitude out;
  out.reserve(count + 1);
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < count; ++i) {
    std::uint64_t sum = carry;
    if (i < a.size()) {
      sum += a[i];
    }
    if (i < b.size()) {
      sum += b[i];
    }
    out.push_back(static_cast<std::uint32_t>(sum & 0xFFFFFFFFULL));
    carry = sum >> 32;
  }
  if (carry != 0) {
    out.push_back(static_cast<std::uint32_t>(carry));
  }
  normalize(out);
  return out;
}

// Requires a >= b.
inline Magnitude subtract_magnitude(const Magnitude& a, const Magnitude& b) {
  Magnitude out;
  out.reserve(a.size());
  std::uint64_t borrow = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const std::uint64_t left = a[i];
    const std::uint64_t right = (i < b.size() ? b[i] : 0u) + borrow;
    if (left >= right) {
      out.push_back(static_cast<std::uint32_t>(left - right));
      borrow = 0;
    } else {
      out.push_back(static_cast<std::uint32_t>((0x100000000ULL + left) - right));
      borrow = 1;
    }
  }
  normalize(out);
  return out;
}

inline Magnitude multiply_magnitude(const Magnitude& a, const Magnitude& b) {
  if (a.empty() || b.empty()) {
    return Magnitude{};
  }
  Magnitude out(a.size() + b.size(), 0u);
  for (std::size_t i = 0; i < a.size(); ++i) {
    std::uint64_t carry = 0;
    for (std::size_t j = 0; j < b.size(); ++j) {
      const std::uint64_t current =
          static_cast<std::uint64_t>(out[i + j]) + static_cast<std::uint64_t>(a[i]) * b[j] + carry;
      out[i + j] = static_cast<std::uint32_t>(current & 0xFFFFFFFFULL);
      carry = current >> 32;
    }
    std::size_t k = i + b.size();
    while (carry != 0) {
      const std::uint64_t current = static_cast<std::uint64_t>(out[k]) + carry;
      out[k] = static_cast<std::uint32_t>(current & 0xFFFFFFFFULL);
      carry = current >> 32;
      ++k;
    }
  }
  normalize(out);
  return out;
}

inline std::size_t bit_length(const Magnitude& value) {
  if (value.empty()) {
    return 0;
  }
  std::size_t bits = (value.size() - 1) * 32;
  std::uint32_t top = value.back();
  while (top != 0u) {
    ++bits;
    top >>= 1;
  }
  return bits;
}

inline bool get_bit(const Magnitude& value, std::size_t index) {
  return ((value[index / 32] >> (index % 32)) & 1u) != 0u;
}

inline void set_bit(Magnitude& value, std::size_t index) {
  const std::size_t limb = index / 32;
  while (value.size() <= limb) {
    value.push_back(0u);
  }
  value[limb] |= (1u << (index % 32));
}

inline void shift_left_one(Magnitude& value) {
  std::uint32_t carry = 0;
  for (std::uint32_t& limb : value) {
    const std::uint32_t next = limb >> 31;
    limb = static_cast<std::uint32_t>((limb << 1) | carry);
    carry = next;
  }
  if (carry != 0u) {
    value.push_back(carry);
  }
}

// Truncating division of magnitudes (remainder has the dividend's sign).
inline Magnitude divide_magnitude(const Magnitude& numerator, const Magnitude& denominator, Magnitude& remainder) {
  Magnitude quotient;
  remainder.clear();
  for (std::size_t i = bit_length(numerator); i-- > 0;) {
    shift_left_one(remainder);
    if (get_bit(numerator, i)) {
      if (remainder.empty()) {
        remainder.push_back(1u);
      } else {
        remainder[0] |= 1u;
      }
    }
    if (compare_magnitude(remainder, denominator) >= 0) {
      remainder = subtract_magnitude(remainder, denominator);
      set_bit(quotient, i);
    }
  }
  normalize(quotient);
  normalize(remainder);
  return quotient;
}

}  // namespace big_detail

class BigInt {
 public:
  BigInt() = default;

  BigInt(std::int64_t value) {  // NOLINT(google-explicit-constructor): test convenience
    if (value == 0) {
      return;
    }
    sign_ = value < 0 ? -1 : 1;
    std::uint64_t magnitude = static_cast<std::uint64_t>(value);
    if (value < 0) {
      magnitude = 0ULL - magnitude;
    }
    magnitude_.push_back(static_cast<std::uint32_t>(magnitude & 0xFFFFFFFFULL));
    const std::uint32_t high = static_cast<std::uint32_t>(magnitude >> 32);
    if (high != 0u) {
      magnitude_.push_back(high);
    }
  }

  [[nodiscard]] bool is_zero() const noexcept { return sign_ == 0; }
  [[nodiscard]] bool is_negative() const noexcept { return sign_ < 0; }
  [[nodiscard]] bool is_positive() const noexcept { return sign_ > 0; }
  [[nodiscard]] int sign() const noexcept { return sign_; }

  [[nodiscard]] bool fits_int64() const noexcept {
    if (sign_ == 0) {
      return true;
    }
    if (magnitude_.size() > 2) {
      return false;
    }
    std::uint64_t value = magnitude_[0];
    if (magnitude_.size() > 1) {
      value |= static_cast<std::uint64_t>(magnitude_[1]) << 32;
    }
    const std::uint64_t limit = sign_ > 0 ? 0x7FFFFFFFFFFFFFFFULL : 0x8000000000000000ULL;
    return value <= limit;
  }

  // Precondition: fits_int64().
  [[nodiscard]] std::int64_t to_int64() const {
    std::uint64_t value = 0;
    if (!magnitude_.empty()) {
      value = magnitude_[0];
    }
    if (magnitude_.size() > 1) {
      value |= static_cast<std::uint64_t>(magnitude_[1]) << 32;
    }
    if (sign_ >= 0) {
      return static_cast<std::int64_t>(value);
    }
    if (value == 0x8000000000000000ULL) {
      return (std::numeric_limits<std::int64_t>::min)();
    }
    return -static_cast<std::int64_t>(value);
  }

  [[nodiscard]] std::string to_string() const {
    if (sign_ == 0) {
      return "0";
    }
    big_detail::Magnitude work = magnitude_;
    std::vector<std::uint32_t> chunks;  // base 10^9, least significant first
    while (!work.empty()) {
      std::uint64_t remainder = 0;
      for (std::size_t i = work.size(); i-- > 0;) {
        const std::uint64_t current = (remainder << 32) | work[i];
        work[i] = static_cast<std::uint32_t>(current / 1000000000ULL);
        remainder = current % 1000000000ULL;
      }
      big_detail::normalize(work);
      chunks.push_back(static_cast<std::uint32_t>(remainder));
    }
    std::string out = sign_ < 0 ? "-" : "";
    out += std::to_string(chunks.back());
    for (std::size_t i = chunks.size() - 1; i-- > 0;) {
      const std::string part = std::to_string(chunks[i]);
      out.append(static_cast<std::size_t>(9) - part.size(), '0');
      out += part;
    }
    return out;
  }

  friend BigInt operator+(const BigInt& a, const BigInt& b) {
    BigInt out;
    if (a.sign_ == 0) {
      return b;
    }
    if (b.sign_ == 0) {
      return a;
    }
    if (a.sign_ == b.sign_) {
      out.sign_ = a.sign_;
      out.magnitude_ = big_detail::add_magnitude(a.magnitude_, b.magnitude_);
      return out;
    }
    const int order = big_detail::compare_magnitude(a.magnitude_, b.magnitude_);
    if (order == 0) {
      return out;
    }
    if (order > 0) {
      out.sign_ = a.sign_;
      out.magnitude_ = big_detail::subtract_magnitude(a.magnitude_, b.magnitude_);
    } else {
      out.sign_ = b.sign_;
      out.magnitude_ = big_detail::subtract_magnitude(b.magnitude_, a.magnitude_);
    }
    return out;
  }

  friend BigInt operator-(const BigInt& a, const BigInt& b) { return a + (-b); }

  friend BigInt operator-(const BigInt& a) {
    BigInt out = a;
    out.sign_ = -out.sign_;
    return out;
  }

  friend BigInt operator*(const BigInt& a, const BigInt& b) {
    BigInt out;
    if (a.sign_ == 0 || b.sign_ == 0) {
      return out;
    }
    out.sign_ = a.sign_ * b.sign_;
    out.magnitude_ = big_detail::multiply_magnitude(a.magnitude_, b.magnitude_);
    return out;
  }

  // Truncating division, matching C++ integer division.
  friend BigInt operator/(const BigInt& a, const BigInt& b) {
    BigInt quotient;
    BigInt::divide(a, b, &quotient, nullptr);
    return quotient;
  }

  friend BigInt operator%(const BigInt& a, const BigInt& b) {
    BigInt remainder;
    BigInt::divide(a, b, nullptr, &remainder);
    return remainder;
  }

  // Floor division: the quotient rounds toward negative infinity.
  [[nodiscard]] static BigInt floor_div(const BigInt& a, const BigInt& b) {
    BigInt quotient;
    BigInt remainder;
    BigInt::divide(a, b, &quotient, &remainder);
    if (!remainder.is_zero() && (remainder.sign_ != b.sign_)) {
      quotient = quotient - BigInt(1);
    }
    return quotient;
  }

  friend bool operator==(const BigInt& a, const BigInt& b) noexcept {
    return a.sign_ == b.sign_ && a.magnitude_ == b.magnitude_;
  }
  friend bool operator!=(const BigInt& a, const BigInt& b) noexcept { return !(a == b); }
  friend bool operator<(const BigInt& a, const BigInt& b) noexcept {
    if (a.sign_ != b.sign_) {
      return a.sign_ < b.sign_;
    }
    if (a.sign_ == 0) {
      return false;
    }
    const int order = big_detail::compare_magnitude(a.magnitude_, b.magnitude_);
    return a.sign_ > 0 ? order < 0 : order > 0;
  }
  friend bool operator>(const BigInt& a, const BigInt& b) noexcept { return b < a; }
  friend bool operator<=(const BigInt& a, const BigInt& b) noexcept { return !(b < a); }
  friend bool operator>=(const BigInt& a, const BigInt& b) noexcept { return !(a < b); }

 private:
  // Preconditions: b is not zero. 'quotient' and 'remainder' may be null.
  static void divide(const BigInt& a, const BigInt& b, BigInt* quotient, BigInt* remainder) {
    if (b.sign_ == 0) {
      // The reference never divides by zero; a zero divisor inside the model is
      // a defect in the model, reported by the caller's invariant checks.
      if (quotient != nullptr) {
        *quotient = BigInt();
      }
      if (remainder != nullptr) {
        *remainder = BigInt();
      }
      return;
    }
    big_detail::Magnitude rem;
    const big_detail::Magnitude mag = big_detail::divide_magnitude(a.magnitude_, b.magnitude_, rem);
    if (quotient != nullptr) {
      quotient->sign_ = mag.empty() ? 0 : a.sign_ * b.sign_;
      quotient->magnitude_ = mag;
    }
    if (remainder != nullptr) {
      remainder->sign_ = rem.empty() ? 0 : a.sign_;
      remainder->magnitude_ = rem;
    }
  }

  int sign_{0};
  big_detail::Magnitude magnitude_;
};

// Deterministic rendering so the test harness can report a BigInt in a
// failure message.
inline std::ostream& operator<<(std::ostream& out, const BigInt& value) { return out << value.to_string(); }

// ---------------------------------------------------------------------------
// Reference ledger closure, computed from the raw evidence list.
// ---------------------------------------------------------------------------

// A raw record as it entered the window: only the fields that change meaning.
struct ReferenceEvidence {
  std::string authority;
  ScopePath scope;
  Dimension dimension{Dimension::Power};
  CapacityAssertion assertion{CapacityAssertion::Installed};
  std::int64_t canonical_amount{0};
};

using Maybe = std::optional<BigInt>;

// The nine classes that participate in the ledger identities (the production
// comment lists exactly this set; the reference restates it independently).
inline const std::array<CapacityAssertion, 9>& identity_classes() {
  static const std::array<CapacityAssertion, 9> kClasses{
      CapacityAssertion::Installed,           CapacityAssertion::Committed,
      CapacityAssertion::Reserved,            CapacityAssertion::Stranded,
      CapacityAssertion::Disputed,            CapacityAssertion::ExcludedFailure,
      CapacityAssertion::ExcludedMaintenance, CapacityAssertion::Available,
      CapacityAssertion::Nameplate};
  return kClasses;
}

struct ReferenceLine {
  bool has_evidence{false};
  std::size_t evidence_count{0};
  std::array<Maybe, kAssertionCount> declared{};
  Maybe exclusion_total;
  Maybe serviceable;
  Maybe free_usable;
  Maybe governed;
  bool governed_derived{false};
  Maybe unexplained_governance;
  Maybe unexplained_installed;
  Maybe unexplained_available;
  Maybe overcommitment;
  bool overcommitted{false};
  bool installed_closure_indeterminate{false};
  std::size_t unknown_identity_classes{0};

  [[nodiscard]] const Maybe& get(CapacityAssertion assertion) const { return declared[assertion_index(assertion)]; }
};

namespace ref_detail {

inline Maybe sum_of(std::initializer_list<Maybe> parts) {
  BigInt total(0);
  for (const Maybe& part : parts) {
    if (!part.has_value()) {
      return Maybe{};
    }
    total = total + part.value();
  }
  return Maybe{total};
}

// base minus every part; unknown anywhere makes the result unknown.
inline Maybe subtract_all(const Maybe& base, std::initializer_list<Maybe> parts) {
  if (!base.has_value()) {
    return Maybe{};
  }
  BigInt running = base.value();
  for (const Maybe& part : parts) {
    if (!part.has_value()) {
      return Maybe{};
    }
    running = running - part.value();
  }
  return Maybe{running};
}

}  // namespace ref_detail

// Recomputes one ledger line for 'dimension' over every raw record whose scope
// is at or below 'scope'. Unknown classes stay unknown: a class with no raw
// record is nullopt, never zero.
[[nodiscard]] inline ReferenceLine reference_line(const std::vector<ReferenceEvidence>& raw, const ScopePath& scope,
                                                  Dimension dimension) {
  ReferenceLine line;
  std::array<BigInt, kAssertionCount> sums{};
  std::array<bool, kAssertionCount> present{};

  for (const ReferenceEvidence& evidence : raw) {
    if (evidence.dimension != dimension) {
      continue;
    }
    if (!scope.is_prefix_of(evidence.scope)) {
      continue;
    }
    const std::size_t index = assertion_index(evidence.assertion);
    sums[index] = sums[index] + BigInt(evidence.canonical_amount);
    present[index] = true;
    ++line.evidence_count;
  }
  line.has_evidence = line.evidence_count != 0;
  for (std::size_t i = 0; i < kAssertionCount; ++i) {
    if (present[i]) {
      line.declared[i] = sums[i];
    }
  }
  for (const CapacityAssertion assertion : identity_classes()) {
    if (!line.declared[assertion_index(assertion)].has_value()) {
      ++line.unknown_identity_classes;
    }
  }
  if (!line.has_evidence) {
    // No identity is evaluated for a line with no evidence: the whole verdict is
    // "indeterminate", which the closure flags make explicit.
    line.installed_closure_indeterminate = true;
    return line;
  }

  const Maybe& nameplate = line.declared[assertion_index(CapacityAssertion::Nameplate)];
  const Maybe& governed_declared = line.declared[assertion_index(CapacityAssertion::Governed)];
  const Maybe& installed = line.declared[assertion_index(CapacityAssertion::Installed)];
  const Maybe& reserved = line.declared[assertion_index(CapacityAssertion::Reserved)];
  const Maybe& committed = line.declared[assertion_index(CapacityAssertion::Committed)];
  const Maybe& available = line.declared[assertion_index(CapacityAssertion::Available)];
  const Maybe& stranded = line.declared[assertion_index(CapacityAssertion::Stranded)];
  const Maybe& disputed = line.declared[assertion_index(CapacityAssertion::Disputed)];
  const Maybe& excluded_policy = line.declared[assertion_index(CapacityAssertion::ExcludedPolicy)];
  const Maybe& excluded_maintenance = line.declared[assertion_index(CapacityAssertion::ExcludedMaintenance)];
  const Maybe& excluded_failure = line.declared[assertion_index(CapacityAssertion::ExcludedFailure)];
  const Maybe& reserve = line.declared[assertion_index(CapacityAssertion::OperationalReserve)];

  line.exclusion_total = ref_detail::sum_of({excluded_policy, excluded_maintenance, excluded_failure, reserve});
  line.serviceable = ref_detail::subtract_all(installed, {excluded_failure, excluded_maintenance});
  line.free_usable =
      ref_detail::subtract_all(line.serviceable, {committed, reserved, stranded, disputed});

  const Maybe governed_from_nameplate =
      ref_detail::subtract_all(nameplate, {excluded_policy, excluded_maintenance, excluded_failure, reserve});
  if (governed_declared.has_value()) {
    line.governed = governed_declared;
    line.governed_derived = false;
  } else {
    line.governed = governed_from_nameplate;
    line.governed_derived = true;
  }

  // Closure 1: nameplate == exclusions + governed.
  {
    const Maybe partition =
        ref_detail::sum_of({excluded_policy, excluded_maintenance, excluded_failure, reserve, line.governed});
    if (nameplate.has_value() && partition.has_value()) {
      line.unexplained_governance = nameplate.value() - partition.value();
    }
  }

  // Closure 2: installed == failure + maintenance + stranded + disputed +
  // committed + reserved + free_usable.
  {
    const Maybe classes = ref_detail::sum_of(
        {excluded_failure, excluded_maintenance, stranded, disputed, committed, reserved, line.free_usable});
    if (installed.has_value() && classes.has_value()) {
      line.unexplained_installed = installed.value() - classes.value();
    } else {
      line.installed_closure_indeterminate = true;
    }
  }

  // Closure 3: declared available == computed free usable.
  if (available.has_value() && line.free_usable.has_value()) {
    line.unexplained_available = available.value() - line.free_usable.value();
  }

  // Closure 4: committed + reserved against the governed ceiling.
  {
    const Maybe allocated = ref_detail::sum_of({committed, reserved});
    if (allocated.has_value() && line.governed.has_value()) {
      const BigInt residual = allocated.value() - line.governed.value();
      if (residual.is_positive()) {
        line.overcommitted = true;
        line.overcommitment = residual;
      }
    }
  }
  // A negative free figure is also an overcommitment, but the reported figure
  // stays the allocation residual when the allocation identity was evaluable:
  // overcommitment means "committed + reserved - governed, when positive".
  if (line.free_usable.has_value() && line.free_usable.value().is_negative()) {
    line.overcommitted = true;
    if (!line.overcommitment.has_value()) {
      line.overcommitment = -line.free_usable.value();
    }
  }
  return line;
}

// Compares a production line against the reference and appends a description of
// every disagreement. Returns true when the line matches exactly.
[[nodiscard]] inline bool compare_line(const LedgerLine& actual, const ReferenceLine& expected,
                                       std::vector<std::string>& problems) {
  const std::size_t before = problems.size();
  const auto note = [&problems](const std::string& text) { problems.push_back(text); };
  const auto compare_maybe = [&note](const std::string& what, const std::optional<Amount>& got, const Maybe& want) {
    if (got.has_value() != want.has_value()) {
      note(what + ": production " + (got.has_value() ? "known=" + std::to_string(got.value().canonical())
                                                     : std::string("unknown")) +
           " but reference " + (want.has_value() ? "known=" + want.value().to_string() : std::string("unknown")));
      return;
    }
    if (!want.has_value()) {
      return;
    }
    if (!want.value().fits_int64()) {
      note(what + ": reference value " + want.value().to_string() + " does not fit int64 but production reported " +
           std::to_string(got.value().canonical()));
      return;
    }
    if (got.value().canonical() != want.value().to_int64()) {
      note(what + ": production " + std::to_string(got.value().canonical()) + " but reference " +
           want.value().to_string());
    }
  };

  for (std::size_t i = 0; i < kAssertionCount; ++i) {
    const auto assertion = static_cast<CapacityAssertion>(i);
    compare_maybe(std::string("declared.") + std::string(assertion_text(assertion)), actual.declared.get(assertion),
                  expected.declared[i]);
  }
  compare_maybe("exclusion_total", actual.exclusion_total, expected.exclusion_total);
  compare_maybe("serviceable", actual.serviceable, expected.serviceable);
  compare_maybe("free_usable", actual.free_usable, expected.free_usable);
  compare_maybe("governed", actual.governed, expected.governed);
  compare_maybe("unexplained_governance", actual.unexplained_governance, expected.unexplained_governance);
  compare_maybe("unexplained_installed", actual.unexplained_installed, expected.unexplained_installed);
  compare_maybe("unexplained_available", actual.unexplained_available, expected.unexplained_available);
  compare_maybe("overcommitment", actual.overcommitment, expected.overcommitment);

  if (actual.governed_derived != expected.governed_derived) {
    note("governed_derived: production " + std::string(actual.governed_derived ? "true" : "false") +
         " but reference " + std::string(expected.governed_derived ? "true" : "false"));
  }
  if (actual.overcommitted != expected.overcommitted) {
    note("overcommitted: production " + std::string(actual.overcommitted ? "true" : "false") + " but reference " +
         std::string(expected.overcommitted ? "true" : "false"));
  }
  // For a line with no evidence at all the production contract reports
  // unknown_classes = 0 and states the whole verdict through LineState::
  // Indeterminate instead; the comparison therefore applies to evaluated lines.
  if (expected.has_evidence && actual.unknown_classes != expected.unknown_identity_classes) {
    note("unknown_classes: production " + std::to_string(actual.unknown_classes) + " but reference " +
         std::to_string(expected.unknown_identity_classes));
  }
  if (actual.evidence_count != expected.evidence_count) {
    note("evidence_count: production " + std::to_string(actual.evidence_count) + " but reference " +
         std::to_string(expected.evidence_count));
  }
  if (actual.closures[1].indeterminate != expected.installed_closure_indeterminate) {
    note("installed closure indeterminate: production " +
         std::string(actual.closures[1].indeterminate ? "true" : "false") + " but reference " +
         std::string(expected.installed_closure_indeterminate ? "true" : "false"));
  }
  return problems.size() == before;
}

// ---------------------------------------------------------------------------
// Reference placement model: the true maximum number of placeable racks for a
// small topology, found by exhaustive enumeration over every distribution of
// racks across enclosures. Computed from the raw enclosure numbers only.
// ---------------------------------------------------------------------------
struct ReferenceEnclosure {
  bool structurally_usable{true};
  std::optional<std::int64_t> contiguous_ru;
  // Indexed by dimension_index(); nullopt means the free amount is unknown.
  std::array<std::optional<std::int64_t>, kDimensionCount> free_amounts{};
};

struct ReferencePlacement {
  std::int64_t max_racks{0};
  bool indeterminate{false};
  std::size_t indeterminate_enclosures{0};
  // Per-enclosure maximum, derived independently of the recursion.
  std::vector<std::int64_t> per_enclosure{};
};

namespace place_detail {

inline std::int64_t enclosure_max(const ReferenceEnclosure& enclosure, std::int64_t rack_units,
                                  const std::array<std::optional<std::int64_t>, kDimensionCount>& per_rack) {
  if (!enclosure.structurally_usable) {
    return 0;
  }
  std::int64_t limit = enclosure.contiguous_ru.value() / rack_units;
  for (std::size_t i = 0; i < kDimensionCount; ++i) {
    if (!per_rack[i].has_value()) {
      continue;
    }
    const std::int64_t demand = per_rack[i].value();
    const std::int64_t free_amount = enclosure.free_amounts[i].value();
    limit = (std::min)(limit, free_amount / demand);
  }
  return limit;
}

// Exhaustive recursion: choose how many racks each enclosure hosts, from zero
// to its own maximum, and keep the best feasible total. Feasibility re-checks
// every budget from the raw numbers.
inline void enumerate(const std::vector<ReferenceEnclosure>& enclosures, std::size_t index, std::int64_t rack_units,
                      const std::array<std::optional<std::int64_t>, kDimensionCount>& per_rack,
                      const std::vector<std::int64_t>& maxima, std::int64_t placed, std::int64_t& best) {
  if (index == enclosures.size()) {
    best = (std::max)(best, placed);
    return;
  }
  const ReferenceEnclosure& enclosure = enclosures[index];
  for (std::int64_t count = 0; count <= maxima[index]; ++count) {
    bool feasible = count * rack_units <= enclosure.contiguous_ru.value();
    for (std::size_t i = 0; feasible && i < kDimensionCount; ++i) {
      if (!per_rack[i].has_value()) {
        continue;
      }
      feasible = count * per_rack[i].value() <= enclosure.free_amounts[i].value();
    }
    if (feasible) {
      enumerate(enclosures, index + 1, rack_units, per_rack, maxima, placed + count, best);
    }
  }
}

}  // namespace place_detail

// 'per_rack' is indexed by dimension_index(); nullopt means the profile places
// no requirement on that dimension.
[[nodiscard]] inline ReferencePlacement reference_placement(
    const std::vector<ReferenceEnclosure>& enclosures, std::int64_t rack_units,
    const std::array<std::optional<std::int64_t>, kDimensionCount>& per_rack) {
  ReferencePlacement placement;
  std::vector<ReferenceEnclosure> evaluable;
  for (const ReferenceEnclosure& enclosure : enclosures) {
    if (!enclosure.structurally_usable) {
      continue;
    }
    bool determinable = enclosure.contiguous_ru.has_value();
    for (std::size_t i = 0; determinable && i < kDimensionCount; ++i) {
      if (per_rack[i].has_value() && !enclosure.free_amounts[i].has_value()) {
        determinable = false;
      }
    }
    if (!determinable) {
      placement.indeterminate = true;
      ++placement.indeterminate_enclosures;
      continue;
    }
    evaluable.push_back(enclosure);
  }

  // Indeterminacy is a property of the enclosures, not of the aggregate: a
  // topology whose enclosures are all structurally unusable is perfectly
  // determinate (nothing can be placed and every free amount is stranded), and a
  // topology with an enclosure that could not be evaluated is already flagged by
  // the loop above. Having no evaluable enclosure for another reason is not a
  // separate source of uncertainty.

  std::vector<std::int64_t> maxima;
  maxima.reserve(evaluable.size());
  for (const ReferenceEnclosure& enclosure : evaluable) {
    maxima.push_back(place_detail::enclosure_max(enclosure, rack_units, per_rack));
  }
  std::int64_t best = 0;
  place_detail::enumerate(evaluable, 0, rack_units, per_rack, maxima, 0, best);
  placement.max_racks = best;
  placement.per_enclosure = maxima;

  std::int64_t sum_of_maxima = 0;
  for (const std::int64_t value : maxima) {
    sum_of_maxima += value;
  }
  if (sum_of_maxima != best) {
    // The recursion is the authority; this only guards the reference itself.
    placement.per_enclosure.push_back(-sum_of_maxima);
  }
  return placement;
}

}  // namespace co::test::reference
