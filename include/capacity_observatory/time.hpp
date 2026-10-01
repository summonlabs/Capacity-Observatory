#pragma once
// Capacity Observatory - time and freshness without hidden clock reads.

#include <cstdint>
#include <memory>
#include <string>

#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"

namespace co {

// Nanoseconds since the Unix epoch, UTC. Negative values denote instants before
// the epoch and are representable, if unusual.
class Timestamp {
 public:
  constexpr Timestamp() noexcept = default;
  explicit constexpr Timestamp(std::int64_t unix_nanos) noexcept : unix_nanos_(unix_nanos) {}

  [[nodiscard]] constexpr std::int64_t unix_nanos() const noexcept { return unix_nanos_; }

  friend constexpr bool operator==(Timestamp a, Timestamp b) noexcept { return a.unix_nanos_ == b.unix_nanos_; }
  friend constexpr bool operator!=(Timestamp a, Timestamp b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Timestamp a, Timestamp b) noexcept { return a.unix_nanos_ < b.unix_nanos_; }
  friend constexpr bool operator<=(Timestamp a, Timestamp b) noexcept { return a.unix_nanos_ <= b.unix_nanos_; }
  friend constexpr bool operator>(Timestamp a, Timestamp b) noexcept { return a.unix_nanos_ > b.unix_nanos_; }
  friend constexpr bool operator>=(Timestamp a, Timestamp b) noexcept { return a.unix_nanos_ >= b.unix_nanos_; }

 private:
  std::int64_t unix_nanos_{0};
};

class Duration {
 public:
  constexpr Duration() noexcept = default;
  explicit constexpr Duration(std::int64_t nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos_ < 0; }

  friend constexpr bool operator==(Duration a, Duration b) noexcept { return a.nanos_ == b.nanos_; }
  friend constexpr bool operator!=(Duration a, Duration b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Duration a, Duration b) noexcept { return a.nanos_ < b.nanos_; }
  friend constexpr bool operator<=(Duration a, Duration b) noexcept { return a.nanos_ <= b.nanos_; }
  friend constexpr bool operator>(Duration a, Duration b) noexcept { return a.nanos_ > b.nanos_; }
  friend constexpr bool operator>=(Duration a, Duration b) noexcept { return a.nanos_ >= b.nanos_; }

 private:
  std::int64_t nanos_{0};
};

[[nodiscard]] Result<Duration> elapsed(Timestamp later, Timestamp earlier);
[[nodiscard]] Result<Timestamp> advanced(Timestamp base, Duration delta);

[[nodiscard]] constexpr Duration seconds(std::int64_t count) noexcept { return Duration(count * 1000000000LL); }
[[nodiscard]] constexpr Duration milliseconds(std::int64_t count) noexcept { return Duration(count * 1000000LL); }

// Freshness of an observation relative to an evaluation instant.
enum class Freshness : std::uint8_t {
  Fresh = 0,        // observed within the contract's staleness budget
  Stale = 1,        // older than the contract's staleness budget
  Future = 2,       // observed_at is after the evaluation instant (clock skew)
  Recovered = 3,    // restored from persistence; never promoted to Fresh by recovery alone
  Unknown = 4       // no observation instant is available
};

[[nodiscard]] std::string_view freshness_text(Freshness freshness) noexcept;

struct FreshnessEvaluation {
  Freshness freshness{Freshness::Unknown};
  Duration age{};
  bool skew_detected{false};
  std::string explanation;
};

// Evaluates freshness. A budget of zero means "no staleness budget is declared",
// which yields Fresh for a recovered=false observation and Stale=false otherwise.
[[nodiscard]] Result<FreshnessEvaluation> evaluate_freshness(bool has_observation_instant,
                                                            Timestamp observed_at,
                                                            Timestamp evaluation_instant,
                                                            Duration staleness_budget,
                                                            bool recovered);

// Deterministic clock interface. Production code never calls the system clock
// directly, so every derived value can be reproduced in tests.
class Clock {
 public:
  virtual ~Clock() = default;
  [[nodiscard]] virtual Timestamp now() const = 0;
};

[[nodiscard]] std::shared_ptr<Clock> system_clock();
[[nodiscard]] std::shared_ptr<Clock> manual_clock(Timestamp start);
// Test/simulation control surface for a manual clock.
class ManualClockController {
 public:
  virtual ~ManualClockController() = default;
  virtual void advance(Duration delta) = 0;
  virtual void set(Timestamp instant) = 0;
};

}  // namespace co
