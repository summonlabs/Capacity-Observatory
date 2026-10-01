#pragma once
// Capacity Observatory - strong identity and counter types.
//
// Identities, generations, epochs, revisions, and quantities are distinct
// types. Interchanging them is a compile error rather than a silent defect.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"

namespace co {

// ---------------------------------------------------------------------------
// Strong numbers: Generation, Epoch, Revision, Attempt, SequenceNumber.
// ---------------------------------------------------------------------------
template <class Tag, class Rep = std::uint64_t>
class StrongNumber {
 public:
  using rep = Rep;
  using tag = Tag;

  constexpr StrongNumber() noexcept = default;
  explicit constexpr StrongNumber(Rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

  friend constexpr bool operator==(StrongNumber a, StrongNumber b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(StrongNumber a, StrongNumber b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(StrongNumber a, StrongNumber b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(StrongNumber a, StrongNumber b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>(StrongNumber a, StrongNumber b) noexcept { return a.value_ > b.value_; }
  friend constexpr bool operator>=(StrongNumber a, StrongNumber b) noexcept { return a.value_ >= b.value_; }

 private:
  Rep value_{0};
};

struct GenerationTag {};
struct EpochTag {};
struct RevisionTag {};
struct AttemptTag {};
struct SequenceTag {};

using Generation = StrongNumber<GenerationTag, std::uint64_t>;
using Epoch = StrongNumber<EpochTag, std::uint64_t>;
using Revision = StrongNumber<RevisionTag, std::uint64_t>;
using Attempt = StrongNumber<AttemptTag, std::uint32_t>;
using SequenceNumber = StrongNumber<SequenceTag, std::uint64_t>;

// Checked successor. Refuses instead of wrapping at the top of the range, so a
// saturated counter can never masquerade as a fresh one.
template <class Tag, class Rep>
[[nodiscard]] inline Result<StrongNumber<Tag, Rep>> next_number(StrongNumber<Tag, Rep> current) {
  if (current.value() == (std::numeric_limits<Rep>::max)()) {
    return make_error(ReasonCode::EpochExhausted, "counter is at the maximum representable value");
  }
  return Result<StrongNumber<Tag, Rep>>(StrongNumber<Tag, Rep>(static_cast<Rep>(current.value() + 1)));
}

// ---------------------------------------------------------------------------
// Strong identifiers: validated, case sensitive, no whitespace or separators
// that could be confused with a scope path delimiter.
// ---------------------------------------------------------------------------
[[nodiscard]] bool is_valid_identifier(std::string_view text) noexcept;

struct SiteTag {};
struct HallTag {};
struct ZoneTag {};
struct RowTag {};
struct EnclosureTag {};
struct AuthorityTag {};
struct EvidenceTag {};
struct MutationTag {};
struct SubjectTag {};

template <class Tag>
class StrongId {
 public:
  using tag = Tag;

  StrongId() = default;
  explicit StrongId(std::string text) : text_(std::move(text)) {}

  // Validating constructor: rejects empty, over-long, or structurally invalid text.
  [[nodiscard]] static Result<StrongId> parse(std::string_view text) {
    if (!is_valid_identifier(text)) {
      return make_error(ReasonCode::InvalidIdentifier,
                        "identifier must be 1..96 characters of [A-Za-z0-9._:-] and must not be empty: '" +
                            std::string(text) + "'");
    }
    return Result<StrongId>(StrongId(std::string(text)));
  }

  [[nodiscard]] const std::string& value() const noexcept { return text_; }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] std::string_view view() const noexcept { return text_; }

  friend bool operator==(const StrongId& a, const StrongId& b) noexcept { return a.text_ == b.text_; }
  friend bool operator!=(const StrongId& a, const StrongId& b) noexcept { return !(a == b); }
  friend bool operator<(const StrongId& a, const StrongId& b) noexcept { return a.text_ < b.text_; }
  friend bool operator>(const StrongId& a, const StrongId& b) noexcept { return b < a; }
  friend bool operator<=(const StrongId& a, const StrongId& b) noexcept { return !(b < a); }
  friend bool operator>=(const StrongId& a, const StrongId& b) noexcept { return !(a < b); }

 private:
  std::string text_;
};

using SiteId = StrongId<SiteTag>;
using HallId = StrongId<HallTag>;
using ZoneId = StrongId<ZoneTag>;
using RowId = StrongId<RowTag>;
using EnclosureId = StrongId<EnclosureTag>;
using AuthorityId = StrongId<AuthorityTag>;
using EvidenceId = StrongId<EvidenceTag>;
using MutationId = StrongId<MutationTag>;
using SubjectId = StrongId<SubjectTag>;

// ---------------------------------------------------------------------------
// Content digests (SHA-256).
// ---------------------------------------------------------------------------
class Digest {
 public:
  static constexpr std::size_t kBytes = 32;

  Digest() = default;

  [[nodiscard]] static Digest from_bytes(const std::uint8_t (&bytes)[kBytes]) noexcept;
  [[nodiscard]] static Result<Digest> from_hex(std::string_view hex);

  [[nodiscard]] const std::uint8_t* bytes() const noexcept { return bytes_.data(); }
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] bool is_zero() const noexcept;

  friend bool operator==(const Digest& a, const Digest& b) noexcept;
  friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
  friend bool operator<(const Digest& a, const Digest& b) noexcept;

 private:
  std::array<std::uint8_t, kBytes> bytes_{};
};

}  // namespace co

namespace std {

template <class Tag, class Rep>
struct hash<co::StrongNumber<Tag, Rep>> {
  std::size_t operator()(const co::StrongNumber<Tag, Rep>& value) const noexcept {
    return std::hash<Rep>{}(value.value());
  }
};

template <class Tag>
struct hash<co::StrongId<Tag>> {
  std::size_t operator()(const co::StrongId<Tag>& value) const noexcept {
    return std::hash<std::string>{}(value.value());
  }
};

template <>
struct hash<co::Digest> {
  std::size_t operator()(const co::Digest& value) const noexcept;
};

}  // namespace std
