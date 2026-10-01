// Capacity Observatory - core value semantics: statuses, identities, digests, time.
#include "capacity_observatory/hash.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/strong.hpp"
#include "capacity_observatory/time.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include "capacity_observatory/platform.hpp"
#endif

namespace co {

void fatal_status(const char* what, const Status& status) {
#if defined(_WIN32)
  platform::suppress_error_dialogs();
#endif
  std::fprintf(stderr, "fatal: %s: %s\n", what == nullptr ? "(null)" : what, status.render().c_str());
  std::fflush(stderr);
  std::abort();
}

bool is_valid_identifier(std::string_view text) noexcept {
  if (text.empty() || text.size() > 96) {
    return false;
  }
  for (const char raw : text) {
    const unsigned char ch = static_cast<unsigned char>(raw);
    const bool alpha = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    const bool digit = ch >= '0' && ch <= '9';
    const bool punctuation = ch == '.' || ch == '_' || ch == ':' || ch == '-';
    if (!alpha && !digit && !punctuation) {
      return false;
    }
  }
  // A leading or trailing separator would make prefix matching ambiguous.
  const auto is_separator = [](char ch) { return ch == ':' || ch == '.' || ch == '-' || ch == '_'; };
  if (is_separator(text.front()) || is_separator(text.back())) {
    return false;
  }
  return true;
}

Digest Digest::from_bytes(const std::uint8_t (&bytes)[kBytes]) noexcept {
  Digest digest;
  std::memcpy(digest.bytes_.data(), bytes, kBytes);
  return digest;
}

Result<Digest> Digest::from_hex(std::string_view hex) {
  if (hex.size() != kBytes * 2) {
    return make_error(ReasonCode::InvalidArgument,
                      "digest hex must be exactly 64 characters, got " + std::to_string(hex.size()));
  }
  Digest digest;
  const auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < kBytes; ++i) {
    const int high = nibble(hex[i * 2]);
    const int low = nibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return make_error(ReasonCode::InvalidArgument, "digest hex contains a non hexadecimal character");
    }
    digest.bytes_[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return Result<Digest>(digest);
}

std::string Digest::hex() const {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(kBytes * 2);
  for (std::size_t i = 0; i < kBytes; ++i) {
    const std::uint8_t byte = bytes_[i];
    out[i * 2] = kHexDigits[byte >> 4];
    out[i * 2 + 1] = kHexDigits[byte & 0x0F];
  }
  return out;
}

bool Digest::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

bool operator==(const Digest& a, const Digest& b) noexcept {
  return std::memcmp(a.bytes_.data(), b.bytes_.data(), Digest::kBytes) == 0;
}

bool operator<(const Digest& a, const Digest& b) noexcept {
  return std::memcmp(a.bytes_.data(), b.bytes_.data(), Digest::kBytes) < 0;
}

Result<Duration> elapsed(Timestamp later, Timestamp earlier) {
  const std::int64_t delta = later.unix_nanos() - earlier.unix_nanos();
  // Subtraction of two in-range int64 values can still overflow; detect by sign.
  if (earlier.unix_nanos() > 0 && later.unix_nanos() < 0 && delta > 0) {
    return make_error(ReasonCode::Overflow, "elapsed duration is not representable");
  }
  if (earlier.unix_nanos() < 0 && later.unix_nanos() > 0 && delta < 0) {
    return make_error(ReasonCode::Overflow, "elapsed duration is not representable");
  }
  return Result<Duration>(Duration(delta));
}

Result<Timestamp> advanced(Timestamp base, Duration delta) {
  const std::int64_t sum = base.unix_nanos() + delta.nanos();
  if (delta.nanos() > 0 && sum < base.unix_nanos()) {
    return make_error(ReasonCode::Overflow, "timestamp advance is not representable");
  }
  if (delta.nanos() < 0 && sum > base.unix_nanos()) {
    return make_error(ReasonCode::Overflow, "timestamp advance is not representable");
  }
  return Result<Timestamp>(Timestamp(sum));
}

std::string_view freshness_text(Freshness freshness) noexcept {
  switch (freshness) {
    case Freshness::Fresh:
      return "fresh";
    case Freshness::Stale:
      return "stale";
    case Freshness::Future:
      return "future";
    case Freshness::Recovered:
      return "recovered";
    case Freshness::Unknown:
      return "unknown";
  }
  return "unknown";
}

Result<FreshnessEvaluation> evaluate_freshness(bool has_observation_instant,
                                               Timestamp observed_at,
                                               Timestamp evaluation_instant,
                                               Duration staleness_budget,
                                               bool recovered) {
  FreshnessEvaluation evaluation;
  if (!has_observation_instant) {
    evaluation.freshness = Freshness::Unknown;
    evaluation.explanation = "no observation instant is recorded for this evidence";
    return Result<FreshnessEvaluation>(evaluation);
  }

  const Result<Duration> age = elapsed(evaluation_instant, observed_at);
  if (!age.ok()) {
    return make_error(ReasonCode::Overflow, std::string("freshness age is not representable: ") + age.status().detail());
  }
  evaluation.age = age.value();

  if (age.value().is_negative()) {
    // The observation claims to come from the future. This is reported, never
    // hidden, and is treated as fresh with an explicit skew note.
    evaluation.freshness = recovered ? Freshness::Recovered : Freshness::Future;
    evaluation.skew_detected = true;
    evaluation.explanation = "observation instant is after the evaluation instant; clock skew of " +
                             std::to_string(-age.value().nanos()) + " ns";
    return Result<FreshnessEvaluation>(evaluation);
  }

  const bool budgeted = staleness_budget.nanos() > 0;
  const bool over_budget = budgeted && age.value().nanos() > staleness_budget.nanos();

  if (recovered) {
    // Recovered evidence is never promoted to fresh: recovery is not observation.
    evaluation.freshness = Freshness::Recovered;
    evaluation.explanation = "evidence was recovered from durable storage rather than re-observed";
    return Result<FreshnessEvaluation>(evaluation);
  }
  if (over_budget) {
    evaluation.freshness = Freshness::Stale;
    evaluation.explanation = "observation is " + std::to_string(age.value().nanos()) +
                             " ns old which exceeds the declared staleness budget of " +
                             std::to_string(staleness_budget.nanos()) + " ns";
    return Result<FreshnessEvaluation>(evaluation);
  }
  evaluation.freshness = Freshness::Fresh;
  evaluation.explanation = budgeted ? "observation is within the declared staleness budget"
                                    : "no staleness budget is declared for this authority contract";
  return Result<FreshnessEvaluation>(evaluation);
}

namespace {

class SystemClock final : public Clock {
 public:
  [[nodiscard]] Timestamp now() const override {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
    return Timestamp(static_cast<std::int64_t>(nanos));
  }
};

class ManualClock final : public Clock, public ManualClockController {
 public:
  explicit ManualClock(Timestamp start) noexcept : instant_(start.unix_nanos()) {}

  [[nodiscard]] Timestamp now() const override {
    return Timestamp(instant_.load(std::memory_order_relaxed));
  }
  void advance(Duration delta) override {
    instant_.fetch_add(delta.nanos(), std::memory_order_relaxed);
  }
  void set(Timestamp instant) override { instant_.store(instant.unix_nanos(), std::memory_order_relaxed); }

 private:
  std::atomic<std::int64_t> instant_;
};

}  // namespace

std::shared_ptr<Clock> system_clock() { return std::make_shared<SystemClock>(); }

std::shared_ptr<Clock> manual_clock(Timestamp start) { return std::make_shared<ManualClock>(start); }

}  // namespace co

namespace std {

std::size_t hash<co::Digest>::operator()(const co::Digest& value) const noexcept {
  // FNV-1a over the digest bytes: stable across processes and standard libraries.
  std::size_t accumulator = 1469598103934665603ULL;
  for (std::size_t i = 0; i < co::Digest::kBytes; ++i) {
    accumulator ^= static_cast<std::size_t>(value.bytes()[i]);
    accumulator *= 1099511628211ULL;
  }
  return accumulator;
}

}  // namespace std
