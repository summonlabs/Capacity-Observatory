// Foundations: reason codes, strong types, checked arithmetic, units, hashes.
#include "co_test.hpp"

#include <limits>
#include <string>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/hash.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/strong.hpp"
#include "capacity_observatory/time.hpp"

using namespace co;

namespace {

constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();

}  // namespace

CO_TEST(reason_codes_are_stable_and_total) {
  CO_REQUIRE_EQ(std::string(reason_text(ReasonCode::Ok)), std::string("ok"));
  CO_REQUIRE_EQ(std::string(reason_text(ReasonCode::Overflow)), std::string("overflow"));
  CO_REQUIRE_EQ(std::string(reason_text(ReasonCode::LockHeld)), std::string("lock-held"));
  CO_REQUIRE(reason_class(ReasonCode::Ok) == ReasonClass::Ok);
  CO_REQUIRE(reason_class(ReasonCode::StaleGeneration) == ReasonClass::Indeterminate);
  CO_REQUIRE(reason_class(ReasonCode::InteriorCorruption) == ReasonClass::Failed);
  CO_REQUIRE(reason_class(ReasonCode::DimensionMismatch) == ReasonClass::Refused);
  CO_REQUIRE(!reason_description(ReasonCode::TornTailRecovered).empty());
}

CO_TEST(status_rendering_is_deterministic) {
  const Status ok = Status::success();
  CO_REQUIRE(ok.ok());
  CO_REQUIRE_EQ(ok.render(), std::string("ok"));

  const Status failure = make_error(ReasonCode::DimensionMismatch, "power vs cooling");
  CO_REQUIRE(failure.failed());
  CO_REQUIRE_EQ(failure.render(), std::string("dimension-mismatch: power vs cooling"));
  // Adding a breadcrumb returns a new status and leaves the original untouched.
  const Status rebased = failure.with_context("ledger::compose");
  CO_REQUIRE_EQ(rebased.render(), std::string("dimension-mismatch: ledger::compose: power vs cooling"));
  CO_REQUIRE_EQ(failure.render(), std::string("dimension-mismatch: power vs cooling"));
  CO_REQUIRE(rebased.with_context("compose_ledger").render() ==
             std::string("dimension-mismatch: compose_ledger: ledger::compose: power vs cooling"));
}

CO_TEST(strong_numbers_do_not_wrap) {
  const Generation generation(41);
  CO_REQUIRE_EQ(generation.value(), 41ULL);
  CO_REQUIRE(generation == Generation(41));
  CO_REQUIRE(generation < Generation(42));

  const auto next = next_number(Generation(41));
  CO_REQUIRE_OK(next, advanced_generation);
  CO_REQUIRE_EQ(advanced_generation.value(), 42ULL);

  CO_REQUIRE_ERR(next_number(Generation((std::numeric_limits<std::uint64_t>::max)())), ReasonCode::EpochExhausted);

  // Distinct tag types make interchange a compile error; checked here for
  // documentation value at run time.
  CO_REQUIRE(Generation(1) != Generation(2));
}

CO_TEST(identifiers_are_validated) {
  CO_REQUIRE(is_valid_identifier("hall-a"));
  CO_REQUIRE(is_valid_identifier("SITE.01:zone_2"));
  CO_REQUIRE(!is_valid_identifier(""));
  CO_REQUIRE(!is_valid_identifier("has space"));
  CO_REQUIRE(!is_valid_identifier("slash/inside"));
  CO_REQUIRE(!is_valid_identifier("-leading"));
  CO_REQUIRE(!is_valid_identifier("trailing-"));
  CO_REQUIRE(!is_valid_identifier(std::string(97, 'a')));

  CO_REQUIRE_OK(SiteId::parse("dc1"), site);
  CO_REQUIRE_EQ(site.value(), std::string("dc1"));
  CO_REQUIRE_ERR(SiteId::parse("bad id"), ReasonCode::InvalidIdentifier);
}

CO_TEST(digest_hex_round_trip) {
  const Digest digest = sha256("abc");
  CO_REQUIRE_EQ(digest.hex(),
                std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CO_REQUIRE_OK(Digest::from_hex(digest.hex()), parsed);
  CO_REQUIRE(parsed == digest);
  CO_REQUIRE_ERR(Digest::from_hex("00"), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(Digest::from_hex(std::string(64, 'z')), ReasonCode::InvalidArgument);
  CO_REQUIRE(!digest.is_zero());
  CO_REQUIRE(Digest{}.is_zero());
}

CO_TEST(sha256_matches_published_vectors) {
  CO_REQUIRE_EQ(sha256("").hex(),
                std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CO_REQUIRE_EQ(sha256("abc").hex(),
                std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CO_REQUIRE_EQ(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hex(),
                std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  CO_REQUIRE_EQ(
      sha256("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")
          .hex(),
      std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));

  // One million 'a', the classic long-message vector. Streaming update proves
  // the incremental path, not only the one-shot path.
  Sha256 streaming;
  const std::string block(1000, 'a');
  for (int i = 0; i < 1000; ++i) {
    streaming.update(block);
  }
  CO_REQUIRE_EQ(streaming.finish().hex(),
                std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

  // Byte-at-a-time streaming must agree with the one-shot digest.
  Sha256 drip;
  const std::string message = "capacity observatory streaming equivalence";
  for (const char ch : message) {
    drip.update(static_cast<std::uint8_t>(ch));
  }
  CO_REQUIRE(drip.finish() == sha256(message));
}

CO_TEST(crc32c_matches_castagnoli_check_value) {
  CO_REQUIRE_EQ(crc32c("123456789"), 0xE3069283U);
  CO_REQUIRE_EQ(crc32c(""), 0x00000000U);
  CO_REQUIRE_EQ(crc32c("a"), 0xC1D04330U);
  // Incremental use must agree with the one-shot value.
  const std::uint32_t partial = crc32c("12345");
  CO_REQUIRE_EQ(crc32c("6789", partial), 0xE3069283U);
  CO_REQUIRE_EQ(crc32c_bytes("123456789", 9), 0xE3069283U);
  CO_REQUIRE(fnv1a64("capacity") != fnv1a64("observatory"));
}

CO_TEST(checked_addition_reports_overflow) {
  CO_REQUIRE_OK(checked_add(Amount(2), Amount(3)), sum);
  CO_REQUIRE_EQ(sum.canonical(), 5);

  CO_REQUIRE_ERR(checked_add(Amount(kMax), Amount(1)), ReasonCode::Overflow);
  CO_REQUIRE_ERR(checked_add(Amount(kMin), Amount(-1)), ReasonCode::Overflow);
  CO_REQUIRE_ERR(checked_sub(Amount(kMin), Amount(1)), ReasonCode::Overflow);
  CO_REQUIRE_ERR(checked_sub(Amount(kMin), Amount(kMin)), ReasonCode::Overflow);

  CO_REQUIRE_OK(checked_add(Amount(kMax), Amount(-1)), highest_minus_one);
  CO_REQUIRE_EQ(highest_minus_one.canonical(), kMax - 1);
  CO_REQUIRE_OK(checked_sub(Amount(kMax), Amount(kMax)), zero);
  CO_REQUIRE_EQ(zero.canonical(), 0);

  // Negation of the minimum value is not representable.
  CO_REQUIRE_ERR(checked_negate(Amount(kMin)), ReasonCode::Overflow);
}

CO_TEST(checked_multiplication_reports_overflow) {
  CO_REQUIRE_OK(checked_mul(Amount(1'000'000), 1'000'000), product);
  CO_REQUIRE_EQ(product.canonical(), 1'000'000'000'000LL);
  CO_REQUIRE_ERR(checked_mul(Amount(kMax), 2), ReasonCode::Overflow);
  CO_REQUIRE_ERR(checked_mul(Amount(kMin), -1), ReasonCode::Overflow);
  CO_REQUIRE_ERR(checked_mul(Amount(-1), kMin), ReasonCode::Overflow);
  CO_REQUIRE_OK(checked_mul(Amount(0), kMax), zero_product);
  CO_REQUIRE_EQ(zero_product.canonical(), 0);
  CO_REQUIRE_OK(checked_mul(Amount(kMin), 1), minimum);
  CO_REQUIRE_EQ(minimum.canonical(), kMin);

  CO_REQUIRE_ERR(checked_div(Amount(1), 0), ReasonCode::DivisionByZero);
  CO_REQUIRE_ERR(checked_div(Amount(kMin), -1), ReasonCode::Overflow);
}

CO_TEST(floor_division_is_correct_for_negative_values) {
  CO_REQUIRE_OK(floor_div(7, 2), seven_halves);
  CO_REQUIRE_EQ(seven_halves, 3LL);
  CO_REQUIRE_OK(floor_div(-7, 2), minus_seven_halves);
  CO_REQUIRE_EQ(minus_seven_halves, -4LL);
  CO_REQUIRE_OK(floor_div(-1, 3), minus_one_third);
  CO_REQUIRE_EQ(minus_one_third, -1LL);
  CO_REQUIRE_OK(floor_div(0, 3), zero_thirds);
  CO_REQUIRE_EQ(zero_thirds, 0LL);
  CO_REQUIRE_ERR(floor_div(1, 0), ReasonCode::DivisionByZero);
}

CO_TEST(unit_conversion_is_exact_or_refused) {
  CO_REQUIRE_OK(Unit::parse("kW"), kilowatts);
  CO_REQUIRE(kilowatts.dimension() == Dimension::Power);
  CO_REQUIRE_OK(kilowatts.to_canonical(3), three_kilowatts);
  CO_REQUIRE_EQ(three_kilowatts.canonical(), 3'000'000LL);  // 3 kW in mW

  CO_REQUIRE_OK(Unit::parse("MW"), megawatts);
  CO_REQUIRE_OK(megawatts.to_canonical(10), ten_megawatts);
  CO_REQUIRE_EQ(ten_megawatts.canonical(), 10'000'000'000LL);

  // m2 and kg are whole multiples of the canonical unit, so they are always exact.
  CO_REQUIRE_OK(Unit::parse("m2"), square_metres);
  CO_REQUIRE_OK(square_metres.to_canonical(1), one_square_metre);
  CO_REQUIRE_EQ(one_square_metre.canonical(), 1'000'000LL);
  CO_REQUIRE_OK(Unit::parse("kg"), kilograms);
  CO_REQUIRE_OK(kilograms.to_canonical(7), seven_kilograms);
  CO_REQUIRE_EQ(seven_kilograms.canonical(), 7'000LL);

  // ft2 is 9290304/100 mm2, which reduces to 2322576/25: only multiples of 25
  // square feet are exactly representable in mm2 and everything else is refused.
  CO_REQUIRE_OK(Unit::parse("ft2"), square_feet);
  CO_REQUIRE_OK(square_feet.to_canonical(25), twenty_five_square_feet);
  CO_REQUIRE_EQ(twenty_five_square_feet.canonical(), 2'322'576LL);
  CO_REQUIRE_ERR(square_feet.to_canonical(1), ReasonCode::InexactScale);
  CO_REQUIRE_ERR(square_feet.to_canonical(3), ReasonCode::InexactScale);

  // lb is 45359237/100000 grams (already reduced): only multiples of 100000 are exact.
  CO_REQUIRE_OK(Unit::parse("lb"), pounds);
  CO_REQUIRE_OK(pounds.to_canonical(100'000), one_hundred_thousand_pounds);
  // 100000 lb x (45359237/100000) g = 45359237 g exactly.
  CO_REQUIRE_EQ(one_hundred_thousand_pounds.canonical(), 45'359'237LL);
  CO_REQUIRE_ERR(pounds.to_canonical(1), ReasonCode::InexactScale);
  CO_REQUIRE_ERR(pounds.to_canonical(3), ReasonCode::InexactScale);

  CO_REQUIRE_ERR(Unit::parse("furlong"), ReasonCode::UnknownUnit);
  CO_REQUIRE_ERR(Unit::parse_for(Dimension::Power, "kg"), ReasonCode::DimensionMismatch);

  CO_REQUIRE(kilowatts.is_canonical() == false);
  CO_REQUIRE(Unit::canonical(Dimension::Power).is_canonical());
  CO_REQUIRE_EQ(Unit::canonical(Dimension::Power).symbol(), std::string("mW"));
  CO_REQUIRE_EQ(kilowatts.symbol(), std::string("kW"));
}

CO_TEST(scale_exact_refuses_rounding_and_detects_overflow) {
  CO_REQUIRE_OK(scale_exact(Amount(9), 1, 3), three);
  CO_REQUIRE_EQ(three.canonical(), 3);
  CO_REQUIRE_ERR(scale_exact(Amount(10), 1, 3), ReasonCode::InexactScale);
  CO_REQUIRE_ERR(scale_exact(Amount(1), 1, 0), ReasonCode::InvalidArgument);
  CO_REQUIRE_ERR(scale_exact(Amount(1), -1, 3), ReasonCode::InvalidArgument);

  // 4611686018427387904 * 2 fits after dividing by 2; a naive multiply first
  // would have overflowed.
  CO_REQUIRE_OK(scale_exact(Amount(4611686018427387904LL), 2, 2), exact_large);
  CO_REQUIRE_EQ(exact_large.canonical(), 4611686018427387904LL);
  CO_REQUIRE_ERR(scale_exact(Amount(kMax), 3, 1), ReasonCode::Overflow);
}

CO_TEST(amount_vectors_preserve_unknown_and_never_zero_fill) {
  AmountVector vector;
  CO_REQUIRE_EQ(vector.known_count(), static_cast<std::size_t>(0));
  CO_REQUIRE_EQ(vector.unknown_count(), kDimensionCount);
  CO_REQUIRE(!vector.is_complete());
  CO_REQUIRE(!vector.get(Dimension::Power).has_value());

  vector.set(Dimension::Power, Amount(1000));
  CO_REQUIRE_EQ(vector.known_count(), static_cast<std::size_t>(1));
  CO_REQUIRE(vector.has(Dimension::Power));
  CO_REQUIRE(!vector.has(Dimension::Weight));

  AmountVector other;
  other.set(Dimension::Power, Amount(500));
  other.set(Dimension::Weight, Amount(7));
  CO_REQUIRE_OK_VOID(vector.accumulate(other));
  CO_REQUIRE_EQ(vector.get(Dimension::Power).value().canonical(), 1500);
  CO_REQUIRE_EQ(vector.get(Dimension::Weight).value().canonical(), 7);
  // Cooling stays explicitly unknown: it was never converted to zero.
  CO_REQUIRE(!vector.get(Dimension::Cooling).has_value());

  std::optional<Amount> values[3] = {Amount(1), std::nullopt, Amount(3)};
  CO_REQUIRE_OK(checked_sum(values, 3), sum); 
  CO_REQUIRE(!sum.total.has_value());
  CO_REQUIRE_EQ(sum.unknown_inputs, static_cast<std::size_t>(1));
  CO_REQUIRE_EQ(sum.known_inputs, static_cast<std::size_t>(2));

  std::optional<Amount> overflow_values[2] = {Amount(kMax), Amount(1)};
  CO_REQUIRE_ERR(checked_sum(overflow_values, 2), ReasonCode::Overflow);
}

CO_TEST(dimension_sets_and_text) {
  DimensionSet set = DimensionSet::of(Dimension::Power);
  CO_REQUIRE(set.contains(Dimension::Power));
  CO_REQUIRE(!set.contains(Dimension::Cooling));
  set.add(Dimension::Cooling);
  CO_REQUIRE_EQ(dimension_set_text(set), std::string("power,cooling"));
  CO_REQUIRE_EQ(DimensionSet::all().count(), kDimensionCount);
  CO_REQUIRE(DimensionSet{}.empty());

  CO_REQUIRE_EQ(std::string(canonical_unit_text(Dimension::Cooling)), std::string("mWth"));
  CO_REQUIRE_OK(dimension_from_text("rack-units"), rack_units);
  CO_REQUIRE(rack_units == Dimension::RackUnits);
  CO_REQUIRE_ERR(dimension_from_text("flux"), ReasonCode::InvalidArgument);
}

CO_TEST(freshness_distinguishes_stale_future_and_recovered) {
  const Timestamp base(1'000'000'000'000LL);

  CO_REQUIRE_OK(evaluate_freshness(true, base, base, seconds(60), false), fresh);
  CO_REQUIRE(fresh.freshness == Freshness::Fresh);

  CO_REQUIRE_OK(evaluate_freshness(true, base, Timestamp(base.unix_nanos() + 120'000'000'000LL), seconds(60), false),
                stale);
  CO_REQUIRE(stale.freshness == Freshness::Stale);
  CO_REQUIRE_EQ(stale.age.nanos(), 120'000'000'000LL);

  CO_REQUIRE_OK(evaluate_freshness(true, base, Timestamp(base.unix_nanos() - 5), seconds(60), false), future);
  CO_REQUIRE(future.freshness == Freshness::Future);
  CO_REQUIRE(future.skew_detected);

  // Recovered evidence is never promoted to fresh by recovery alone.
  CO_REQUIRE_OK(evaluate_freshness(true, base, base, seconds(60), true), recovered);
  CO_REQUIRE(recovered.freshness == Freshness::Recovered);

  CO_REQUIRE_OK(evaluate_freshness(false, base, base, seconds(60), false), no_instant);
  CO_REQUIRE(no_instant.freshness == Freshness::Unknown);

  CO_REQUIRE_EQ(std::string(freshness_text(Freshness::Recovered)), std::string("recovered"));
}

CO_TEST(manual_clock_is_controllable) {
  const std::shared_ptr<Clock> clock = manual_clock(Timestamp(500));
  CO_REQUIRE_EQ(clock->now().unix_nanos(), 500LL);
  const auto controller = std::dynamic_pointer_cast<ManualClockController>(clock);
  CO_REQUIRE(controller != nullptr);
  controller->advance(Duration(250));
  CO_REQUIRE_EQ(clock->now().unix_nanos(), 750LL);
  controller->set(Timestamp(-9));
  CO_REQUIRE_EQ(clock->now().unix_nanos(), -9LL);
}
