// Capacity Observatory - measured benchmark harness.
//
// Every number printed by this program was measured on this machine with
// std::chrono::steady_clock around completed work in the real library (and,
// where a row writes, a real filesystem store). Each row carries an honest
// label:
//
//   REAL        the work measured is the real library on this machine
//   SYNTHETIC   the row measures data generation, not library work
//   UNSUPPORTED the machine cannot measure the row; no number is printed
//
// A second field, "data", states where the inputs came from. Plant, DCIM,
// telemetry, and economic evidence is always SYNTHETIC unless a deployment
// attaches real plant data; that is a property of the deployment, not of this
// harness, so this harness always labels its own inputs SYNTHETIC.
//
// The harness also self-checks: every measured row verifies that the work it
// timed actually completed (records accepted, frames committed, identities
// closed, documents round-tripped). A failed check makes the process exit
// non-zero, so a "fast" row that did nothing cannot pass unnoticed.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/fragmentation.hpp"
#include "capacity_observatory/ingest.hpp"
#include "capacity_observatory/json.hpp"
#include "capacity_observatory/ledger.hpp"
#include "capacity_observatory/platform.hpp"
#include "capacity_observatory/snapshot.hpp"
#include "capacity_observatory/store.hpp"
#include "capacity_observatory/time.hpp"
#include "capacity_observatory/topology.hpp"
#include "capacity_observatory/watermark.hpp"

#if !defined(CO_BENCH_BUILD_TYPE)
#define CO_BENCH_BUILD_TYPE "unknown"
#endif
#if !defined(CO_BENCH_GENERATOR)
#define CO_BENCH_GENERATOR "unknown"
#endif
#if !defined(CO_BENCH_WARNINGS_AS_ERRORS)
#define CO_BENCH_WARNINGS_AS_ERRORS 0
#endif
#if !defined(CO_BENCH_LOCK_AUDIT)
#define CO_BENCH_LOCK_AUDIT 0
#endif

namespace {

using SteadyClock = std::chrono::steady_clock;

// The single seed every synthetic input in this run derives from. Printed in
// the header so any row can be reproduced exactly.
constexpr std::uint64_t kSeed = 0x5EED0000C0FFEE01ULL;

std::uint64_t g_checks_passed = 0;
std::uint64_t g_checks_failed = 0;

[[noreturn]] void fail(const std::string& message) {
  std::fprintf(stderr, "[bench] FAILED: %s\n", message.c_str());
  std::fflush(stderr);
  std::exit(1);
}

void check(bool condition, const std::string& what) {
  if (condition) {
    ++g_checks_passed;
    return;
  }
  ++g_checks_failed;
  std::fprintf(stderr, "[check] FAILED: %s\n", what.c_str());
  std::fflush(stderr);
}

template <class T>
T value_or_fail(co::Result<T>&& result, std::string_view what) {
  if (!result.ok()) {
    fail(std::string(what) + ": " + result.status().render());
  }
  return std::move(result).value();
}

void require_void(co::Result<void>&& result, std::string_view what) {
  if (!result.ok()) {
    fail(std::string(what) + ": " + result.status().render());
  }
}

std::string number(double value, int precision) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

std::string count_text(std::uint64_t value) { return std::to_string(value); }

std::string pad_right(std::string text, std::size_t width) {
  if (text.size() < width) {
    text.append(width - text.size(), ' ');
  }
  return text;
}

std::string pad_left(std::string text, std::size_t width) {
  if (text.size() < width) {
    text.insert(0, width - text.size(), ' ');
  }
  return text;
}

// Deterministic splitmix64. The benchmark owns its data generation so that a
// run is reproducible from the printed seed alone.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  std::int64_t between(std::int64_t low, std::int64_t high) noexcept {
    if (high <= low) {
      return low;
    }
    const std::uint64_t span = static_cast<std::uint64_t>(high - low);
    return low + static_cast<std::int64_t>(next() % span);
  }

 private:
  std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------
struct Row {
  std::string id;
  std::string label;  // REAL | SYNTHETIC | UNSUPPORTED
  std::string data;   // SYNTHETIC | REAL | -
  std::string unit;   // operations unit, "-" when unmeasured
  bool measured{false};
  std::uint64_t ops{0};
  double seconds{0.0};
  std::uint64_t bytes{0};
  std::string detail;
};

// Runs 'body' once and returns the measured row. 'body' returns the number of
// bytes the operation durably produced or consumed, or 0 when the row is not a
// byte-rate row.
template <class Body>
Row measure_row(std::string id, std::string label, std::string data, std::string unit, std::uint64_t ops,
                Body&& body) {
  const SteadyClock::time_point start = SteadyClock::now();
  const std::uint64_t bytes = static_cast<std::uint64_t>(body());
  const SteadyClock::time_point stop = SteadyClock::now();
  Row row;
  row.id = std::move(id);
  row.label = std::move(label);
  row.data = std::move(data);
  row.unit = std::move(unit);
  row.measured = true;
  row.ops = ops;
  row.seconds = std::chrono::duration<double>(stop - start).count();
  row.bytes = bytes;
  return row;
}

Row unsupported_row(std::string id, std::string detail) {
  Row row;
  row.id = std::move(id);
  row.label = "UNSUPPORTED";
  row.data = "-";
  row.unit = "-";
  row.measured = false;
  row.detail = std::move(detail);
  return row;
}

void print_table(const std::vector<Row>& rows) {
  constexpr std::size_t kId = 28;
  constexpr std::size_t kLabel = 11;
  constexpr std::size_t kData = 12;
  constexpr std::size_t kUnit = 13;
  constexpr std::size_t kOps = 10;
  constexpr std::size_t kSeconds = 12;
  constexpr std::size_t kNs = 13;
  constexpr std::size_t kOpsPerSec = 15;
  constexpr std::size_t kBytes = 12;
  constexpr std::size_t kBytesPerSec = 15;

  std::ostringstream header;
  header << pad_right("id", kId) << ' ' << pad_right("label", kLabel) << ' ' << pad_right("data", kData) << ' '
         << pad_right("unit", kUnit) << ' ' << pad_left("ops", kOps) << ' ' << pad_left("seconds", kSeconds) << ' '
         << pad_left("ns/op", kNs) << ' ' << pad_left("ops/sec", kOpsPerSec) << ' ' << pad_left("bytes", kBytes) << ' '
         << pad_left("bytes/sec", kBytesPerSec) << "  detail";
  std::cout << "[table] " << header.str() << '\n';

  for (const Row& row : rows) {
    std::ostringstream line;
    line << pad_right(row.id, kId) << ' ' << pad_right(row.label, kLabel) << ' ' << pad_right(row.data, kData) << ' '
         << pad_right(row.unit, kUnit) << ' ';
    if (row.measured) {
      const double ns_per_op = row.ops == 0 ? 0.0 : (row.seconds * 1e9) / static_cast<double>(row.ops);
      const double ops_per_sec = row.seconds <= 0.0 ? 0.0 : static_cast<double>(row.ops) / row.seconds;
      const double bytes_per_sec = row.seconds <= 0.0 ? 0.0 : static_cast<double>(row.bytes) / row.seconds;
      line << pad_left(count_text(row.ops), kOps) << ' ' << pad_left(number(row.seconds, 6), kSeconds) << ' '
           << pad_left(number(ns_per_op, 2), kNs) << ' ' << pad_left(number(ops_per_sec, 2), kOpsPerSec) << ' '
           << pad_left(count_text(row.bytes), kBytes) << ' ' << pad_left(number(bytes_per_sec, 2), kBytesPerSec);
    } else {
      line << pad_left("-", kOps) << ' ' << pad_left("-", kSeconds) << ' ' << pad_left("-", kNs) << ' '
           << pad_left("-", kOpsPerSec) << ' ' << pad_left("-", kBytes) << ' ' << pad_left("-", kBytesPerSec);
    }
    line << "  " << row.detail;
    std::cout << "[row]   " << line.str() << '\n';
  }
}

// ---------------------------------------------------------------------------
// Synthetic evidence fixture
// ---------------------------------------------------------------------------
struct ClassPlan {
  const char* authority;
  co::AuthorityRole role;
  co::CapacityAssertion assertion;
  co::Provenance provenance;
};

// Thirteen classes per enclosure and dimension. Together they make every one of
// the four ledger closure identities evaluable exactly.
constexpr std::array<ClassPlan, 13> kEnclosureClasses{{
    {"policy", co::AuthorityRole::Policy, co::CapacityAssertion::Nameplate, co::Provenance::Observed},
    {"policy", co::AuthorityRole::Policy, co::CapacityAssertion::Governed, co::Provenance::Observed},
    {"policy", co::AuthorityRole::Policy, co::CapacityAssertion::ExcludedPolicy, co::Provenance::Observed},
    {"policy", co::AuthorityRole::Policy, co::CapacityAssertion::ExcludedMaintenance, co::Provenance::Observed},
    {"policy", co::AuthorityRole::Policy, co::CapacityAssertion::ExcludedFailure, co::Provenance::Observed},
    {"policy", co::AuthorityRole::Policy, co::CapacityAssertion::OperationalReserve, co::Provenance::Observed},
    {"dfi", co::AuthorityRole::InstalledInventory, co::CapacityAssertion::Installed, co::Provenance::Observed},
    {"dfi", co::AuthorityRole::InstalledInventory, co::CapacityAssertion::Available, co::Provenance::Observed},
    {"dfi", co::AuthorityRole::InstalledInventory, co::CapacityAssertion::Stranded, co::Provenance::Observed},
    {"asi", co::AuthorityRole::Reservations, co::CapacityAssertion::Reserved, co::Provenance::Observed},
    {"dccp", co::AuthorityRole::CommittedCapacity, co::CapacityAssertion::Committed, co::Provenance::Observed},
    {"plant", co::AuthorityRole::PlantTelemetry, co::CapacityAssertion::Observed, co::Provenance::Synthetic},
    {"arbiter", co::AuthorityRole::Arbitration, co::CapacityAssertion::Disputed, co::Provenance::Observed},
}};

// One hall-level class, so a roll-up aggregates two scope depths while every
// closure identity still closes. The class is plant telemetry: a hall meter
// reading equal to the sum of its enclosures' observed capacity. It is
// deliberately not a policy class, because re-declaring a subtractive class
// (maintenance, failure) at a coarser scope would subtract capacity that the
// enclosure-level available figure already accounted for. That double
// subtraction is exercised on purpose by the inconsistent-ledger row below.
constexpr std::array<ClassPlan, 1> kHallClasses{{
    {"plant", co::AuthorityRole::PlantTelemetry, co::CapacityAssertion::Observed, co::Provenance::Synthetic},
}};

constexpr std::array<co::Dimension, 3> kFixtureDimensions{co::Dimension::Power, co::Dimension::RackUnits,
                                                          co::Dimension::Cooling};

struct EnclosureValues {
  co::Amount nameplate{};
  co::Amount excluded_policy{};
  co::Amount excluded_maintenance{};
  co::Amount excluded_failure{};
  co::Amount reserve{};
  co::Amount governed{};
  co::Amount installed{};
  co::Amount stranded{};
  co::Amount disputed{};
  co::Amount committed{};
  co::Amount reserved{};
  co::Amount available{};
  co::Amount observed{};

  void add_aggregate(const EnclosureValues& other) noexcept {
    nameplate = co::Amount(nameplate.canonical() + other.nameplate.canonical());
    excluded_policy = co::Amount(excluded_policy.canonical() + other.excluded_policy.canonical());
    excluded_maintenance = co::Amount(excluded_maintenance.canonical() + other.excluded_maintenance.canonical());
    excluded_failure = co::Amount(excluded_failure.canonical() + other.excluded_failure.canonical());
    reserve = co::Amount(reserve.canonical() + other.reserve.canonical());
    governed = co::Amount(governed.canonical() + other.governed.canonical());
    observed = co::Amount(observed.canonical() + other.observed.canonical());
  }
};

EnclosureValues enclosure_values(std::size_t enclosure, co::Dimension dimension, SplitMix64& rng) {
  std::int64_t base = 0;
  switch (dimension) {
    case co::Dimension::Power:
      base = (100 + static_cast<std::int64_t>(enclosure % 50)) * 1000000LL;
      break;
    case co::Dimension::RackUnits:
      base = 42 * (10 + static_cast<std::int64_t>(enclosure % 10));
      break;
    case co::Dimension::Cooling:
      base = (100 + static_cast<std::int64_t>(enclosure % 40) * 2) * 1000000LL;
      break;
    case co::Dimension::Weight:
      base = 40000 * (10 + static_cast<std::int64_t>(enclosure % 10));
      break;
    case co::Dimension::Space:
      base = 30000000 * (10 + static_cast<std::int64_t>(enclosure % 10));
      break;
    case co::Dimension::Serviceability:
      base = 8 + static_cast<std::int64_t>(enclosure % 8);
      break;
  }
  // Deterministic jitter from the printed seed; the identities below are all
  // derived from 'base', so the fixture closes for every seed.
  base += rng.between(0, 4) * (base / 100);

  EnclosureValues values;
  values.nameplate = co::Amount(base);
  values.excluded_policy = co::Amount(base / 20);
  values.excluded_maintenance = co::Amount(base / 50);
  values.excluded_failure = co::Amount(base / 100);
  values.reserve = co::Amount(base / 100);
  values.governed = co::Amount(base - (values.excluded_policy.canonical() + values.excluded_maintenance.canonical() +
                                       values.excluded_failure.canonical() + values.reserve.canonical()));
  values.installed = co::Amount(base - base / 10);
  values.stranded = co::Amount(values.installed.canonical() / 50);
  values.disputed = co::Amount(values.installed.canonical() / 100);
  values.committed = co::Amount(values.governed.canonical() / 2);
  values.reserved = co::Amount(values.governed.canonical() / 4);
  values.available = co::Amount(values.installed.canonical() - values.excluded_failure.canonical() -
                                values.excluded_maintenance.canonical() - values.committed.canonical() -
                                values.reserved.canonical() - values.stranded.canonical() -
                                values.disputed.canonical());
  values.observed = values.installed;
  return values;
}

struct Declared {
  co::Unit unit;
  std::int64_t amount{0};
};

// Picks the largest declared unit in which 'canonical' is exactly representable
// and returns the declared magnitude. Uses the library's own exact conversion;
// a dimension with no coarser unit falls back to the canonical unit.
Declared declare_exact(co::Dimension dimension, co::Amount canonical) {
  static const std::array<std::vector<std::string_view>, co::kDimensionCount> kCandidates{{
      {std::string_view("m2"), std::string_view("mm2")},
      {std::string_view("U")},
      {std::string_view("MW"), std::string_view("kW"), std::string_view("W"), std::string_view("mW")},
      {std::string_view("MWth"), std::string_view("kWth"), std::string_view("Wth"), std::string_view("mWth")},
      {std::string_view("t"), std::string_view("kg"), std::string_view("g")},
      {std::string_view("service")},
  }};

  for (const std::string_view symbol : kCandidates[co::dimension_index(dimension)]) {
    const co::Unit unit = value_or_fail(co::Unit::parse(symbol), "Unit::parse");
    const co::Result<co::Amount> declared = co::scale_exact(canonical, unit.denominator(), unit.numerator());
    if (declared.ok()) {
      Declared result;
      result.unit = unit;
      result.amount = declared.value().canonical();
      return result;
    }
  }
  Declared result;
  result.unit = co::Unit::canonical(dimension);
  result.amount = canonical.canonical();
  return result;
}

co::EvidenceRecord make_record(const ClassPlan& plan,
                               const co::ScopePath& scope,
                               co::Dimension dimension,
                               const Declared& declared,
                               co::Generation generation,
                               co::Epoch epoch,
                               co::Timestamp observed_at,
                               std::string source) {
  co::Result<co::EvidenceRecord> built =
      co::EvidenceRecord::make(co::AuthorityId(plan.authority), plan.role, scope, dimension, plan.assertion,
                               declared.unit, declared.amount, generation, epoch, co::Revision(1));
  if (!built.ok()) {
    fail(std::string("EvidenceRecord::make for ") + plan.authority + "/" + std::string(co::assertion_text(plan.assertion)) +
         ": " + built.status().render());
  }
  co::EvidenceRecord record = std::move(built).value();
  record.provenance = plan.provenance;
  record.has_observed_at = true;
  record.observed_at = observed_at;
  record.source = std::move(source);
  require_void(record.validate(), "fixture record validation");
  return record;
}

co::ScopePath enclosure_scope(std::size_t hall, std::size_t zone, std::size_t row, std::size_t enclosure) {
  const std::string text = "site=bench/hall=h" + std::to_string(hall) + "/zone=z" + std::to_string(zone) + "/row=r" +
                           std::to_string(row) + "/enclosure=e" + std::to_string(enclosure);
  return value_or_fail(co::ScopePath::parse(text), "ScopePath::parse");
}

co::ScopePath hall_scope(std::size_t hall) {
  const std::string text = "site=bench/hall=h" + std::to_string(hall);
  return value_or_fail(co::ScopePath::parse(text), "ScopePath::parse");
}

struct Fixture {
  std::vector<co::EvidenceRecord> records;
  std::vector<co::MutationId> mutations;
  std::vector<co::ScopePath> enclosure_scopes;
  std::vector<co::ScopePath> hall_scopes;
  std::size_t hall_size{0};
  std::size_t classes_per_enclosure{0};
  std::size_t classes_per_hall{0};
};

Fixture make_fixture(std::size_t enclosures, std::size_t hall_size, SplitMix64& rng, co::Timestamp observed_at) {
  Fixture fixture;
  if (hall_size == 0) {
    fail("hall size must be positive");
  }
  fixture.hall_size = hall_size;
  fixture.classes_per_enclosure = kEnclosureClasses.size();
  fixture.classes_per_hall = kHallClasses.size();

  const std::size_t hall_count = (enclosures + hall_size - 1) / hall_size;
  std::size_t sequence = 0;
  for (std::size_t hall = 0; hall < hall_count; ++hall) {
    const std::size_t begin = hall * hall_size;
    const std::size_t end = (std::min)(begin + hall_size, enclosures);
    std::array<EnclosureValues, kFixtureDimensions.size()> totals{};
    for (std::size_t enclosure = begin; enclosure < end; ++enclosure) {
      const co::ScopePath scope = enclosure_scope(hall, enclosure / 16, enclosure / 4, enclosure);
      fixture.enclosure_scopes.push_back(scope);
      for (std::size_t d = 0; d < kFixtureDimensions.size(); ++d) {
        const co::Dimension dimension = kFixtureDimensions[d];
        const EnclosureValues values = enclosure_values(enclosure, dimension, rng);
        totals[d].add_aggregate(values);
        const std::array<co::Amount, kEnclosureClasses.size()> amounts{{
            values.nameplate,       values.governed,   values.excluded_policy, values.excluded_maintenance,
            values.excluded_failure, values.reserve,   values.installed,       values.available,
            values.stranded,        values.reserved,   values.committed,       values.observed,
            values.disputed,
        }};
        for (std::size_t c = 0; c < kEnclosureClasses.size(); ++c) {
          const Declared declared = declare_exact(dimension, amounts[c]);
          const std::string source = "bench-fixture:e" + std::to_string(enclosure) + ":" +
                                     std::string(co::dimension_text(dimension)) + ":" +
                                     std::string(co::assertion_text(kEnclosureClasses[c].assertion));
          fixture.records.push_back(make_record(kEnclosureClasses[c], scope, dimension, declared, co::Generation(1),
                                                co::Epoch(1), observed_at, source));
          fixture.mutations.emplace_back("mut-" + std::to_string(sequence));
          ++sequence;
        }
      }
    }
    const co::ScopePath scope = hall_scope(hall);
    fixture.hall_scopes.push_back(scope);
    for (std::size_t d = 0; d < kFixtureDimensions.size(); ++d) {
      const co::Dimension dimension = kFixtureDimensions[d];
      const std::array<co::Amount, kHallClasses.size()> amounts{{totals[d].observed}};
      for (std::size_t c = 0; c < kHallClasses.size(); ++c) {
        const Declared declared = declare_exact(dimension, amounts[c]);
        const std::string source = "bench-fixture:h" + std::to_string(hall) + ":" +
                                   std::string(co::dimension_text(dimension)) + ":" +
                                   std::string(co::assertion_text(kHallClasses[c].assertion));
        fixture.records.push_back(make_record(kHallClasses[c], scope, dimension, declared, co::Generation(1),
                                              co::Epoch(1), observed_at, source));
        fixture.mutations.emplace_back("mut-" + std::to_string(sequence));
        ++sequence;
      }
    }
  }
  return fixture;
}

// ---------------------------------------------------------------------------
// Store fixture
// ---------------------------------------------------------------------------
struct StoreFixture {
  std::vector<co::EvidenceRecord> records;
  std::vector<co::MutationId> mutations;
  std::uint64_t payload_bytes{0};
};

StoreFixture make_store_fixture(std::size_t count, SplitMix64& rng, co::Timestamp observed_at) {
  StoreFixture fixture;
  const co::ScopePath scope = enclosure_scope(0, 0, 0, 0);
  for (std::size_t i = 0; i < count; ++i) {
    const std::int64_t base = 1000000LL * (100 + static_cast<std::int64_t>(i % 50)) + rng.between(0, 999);
    const Declared declared = declare_exact(co::Dimension::Power, co::Amount(base));
    const ClassPlan plan{"dfi", co::AuthorityRole::InstalledInventory, co::CapacityAssertion::Installed,
                         co::Provenance::Observed};
    fixture.records.push_back(make_record(plan, scope, co::Dimension::Power, declared,
                                          co::Generation(static_cast<std::uint64_t>(i) + 1), co::Epoch(1),
                                          observed_at, "bench-store:" + std::to_string(i)));
    fixture.mutations.emplace_back("store-mut-" + std::to_string(i));
  }
  return fixture;
}

// RAII scratch directory under the system temporary directory.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(const std::string& leaf) {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::temp_directory_path(error);
    if (error) {
      fail("temp_directory_path: " + error.message());
    }
    path_ = (root / "capacity-observatory-bench" / leaf).string();
    std::filesystem::remove_all(path_, error);
    error.clear();
    std::filesystem::create_directories(path_, error);
    if (error) {
      fail("create_directories(" + path_ + "): " + error.message());
    }
  }

  ~ScratchDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;
  ScratchDirectory(ScratchDirectory&&) = delete;
  ScratchDirectory& operator=(ScratchDirectory&&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

// ---------------------------------------------------------------------------
// Fragmentation fixture
// ---------------------------------------------------------------------------
struct FragmentationFixture {
  co::Topology topology;
  co::RackProfile profile;
  std::size_t unusable{0};
};

FragmentationFixture make_fragmentation_fixture(std::size_t enclosures) {
  std::vector<co::EnclosureBudget> budgets;
  budgets.reserve(enclosures);
  std::size_t unusable = 0;
  for (std::size_t e = 0; e < enclosures; ++e) {
    co::EnclosureBudget budget;
    budget.id = co::EnclosureId("e" + std::to_string(e));
    budget.scope = enclosure_scope(e / 64, e / 16, e / 4, e);
    const std::int64_t contiguous = 42 * (1 + static_cast<std::int64_t>(e % 5));
    budget.free_ru_contiguous = contiguous;
    budget.free_ru_total = contiguous + 42 * static_cast<std::int64_t>(e % 3);
    budget.free.set(co::Dimension::RackUnits, co::Amount(contiguous));
    budget.free.set(co::Dimension::Power,
                    co::Amount(7000000LL * (2 + static_cast<std::int64_t>(e % 4))));
    budget.free.set(co::Dimension::Cooling,
                    co::Amount(7000000LL * (2 + static_cast<std::int64_t>((e + 1) % 4))));
    budget.free.set(co::Dimension::Weight,
                    co::Amount(800000LL * (2 + static_cast<std::int64_t>(e % 4))));
    budget.free.set(co::Dimension::Space,
                    co::Amount(600000LL * (2 + static_cast<std::int64_t>(e % 4))));
    if (e % 17 == 0) {
      budget.structurally_usable = false;
      budget.unusable_reason = "condemned floor (synthetic)";
      ++unusable;
    }
    budgets.push_back(std::move(budget));
  }

  co::AmountVector per_rack;
  per_rack.set(co::Dimension::RackUnits, co::Amount(42));
  per_rack.set(co::Dimension::Power, co::Amount(7000000));
  per_rack.set(co::Dimension::Cooling, co::Amount(7000000));
  per_rack.set(co::Dimension::Weight, co::Amount(800000));
  per_rack.set(co::Dimension::Space, co::Amount(600000));

  FragmentationFixture fixture;
  fixture.topology = value_or_fail(co::Topology::make(std::move(budgets)), "Topology::make");
  fixture.profile = value_or_fail(co::RackProfile::make("bench-rack", 42, per_rack), "RackProfile::make");
  fixture.unusable = unusable;
  return fixture;
}

// ---------------------------------------------------------------------------
// Machine facts
// ---------------------------------------------------------------------------
std::string compiler_text() {
  std::ostringstream out;
#if defined(_MSC_VER)
  out << "MSVC _MSC_VER=" << _MSC_VER << " _MSC_FULL_VER=" << _MSC_FULL_VER;
#elif defined(__clang__)
  out << "Clang " << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
#elif defined(__GNUC__)
  out << "GCC " << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#else
  out << "unknown";
#endif
  return out.str();
}

struct Config {
  bool quick{false};
  std::size_t enclosures{0};
  std::size_t hall_size{64};
  std::size_t ledger_reps{0};
  std::size_t fragmentation_reps{0};
  std::size_t append_records{0};
  std::size_t snapshot_reps{0};
};

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------
int run(const Config& config) {
  const std::string mode = config.quick ? "quick" : "default";
  std::cout << "[bench] capacity-observatory-bench " << CO_VERSION_MAJOR << '.' << CO_VERSION_MINOR << '.'
            << CO_VERSION_PATCH << " mode=" << mode << '\n';
  std::cout << "[machine] cpu_threads=" << co::platform::hardware_threads()
            << " build_type=" << CO_BENCH_BUILD_TYPE << " generator=" << CO_BENCH_GENERATOR
            << " compiler=\"" << compiler_text() << "\" __cplusplus=" << __cplusplus
            << " pointer_bits=" << (sizeof(void*) * 8U)
            << " warnings_as_errors=" << (CO_BENCH_WARNINGS_AS_ERRORS ? "on" : "off")
            << " lock_audit=" << (CO_BENCH_LOCK_AUDIT ? "on" : "off") << '\n';
  std::cout << "[clock] std::chrono::steady_clock; seconds are measured around completed work only\n";
  std::cout << "[seed] 0x" << std::hex << kSeed << std::dec << " (splitmix64; all synthetic inputs derive from it)\n";
  std::cout << "[process] pid=" << co::platform::current_process_id() << '\n';

  const co::Timestamp instant(1700000000000000000LL);
  const std::shared_ptr<co::Clock> clock = co::manual_clock(instant);

  std::vector<Row> rows;

  // --- Synthetic fixture generation (measured, labelled SYNTHETIC) ----------
  SplitMix64 rng(kSeed);
  Fixture fixture;
  rows.push_back(measure_row("fixture-generation", "SYNTHETIC", "SYNTHETIC", "records", 0, [&]() -> std::uint64_t {
    fixture = make_fixture(config.enclosures, config.hall_size, rng, instant);
    return 0;
  }));
  rows.back().ops = fixture.records.size();
  rows.back().detail = "splitmix64-generated policy/installed/reserved/committed/plant/arbiter evidence; "
                       "input data for every REAL row below; the generator itself is not library work";

  const std::size_t fixture_records = fixture.records.size();
  std::cout << "[input] enclosures=" << config.enclosures << " hall_size=" << config.hall_size
            << " fixture_records=" << fixture_records << " fixture_dimensions=" << kFixtureDimensions.size()
            << " classes_per_enclosure_dimension=" << fixture.classes_per_enclosure
            << " classes_per_hall_dimension=" << fixture.classes_per_hall << " ledger_reps=" << config.ledger_reps
            << " fragmentation_reps=" << config.fragmentation_reps << " append_records=" << config.append_records
            << " snapshot_reps=" << config.snapshot_reps << '\n';

  // --- Row 1: evidence ingest + acceptance window ---------------------------
  co::AuthorityRegistry registry = co::AuthorityRegistry::standard();
  co::EvidenceWindow window(registry, clock);
  co::IngestPipeline pipeline(window, nullptr);
  std::size_t applied = 0;
  std::size_t refused = 0;
  rows.push_back(measure_row("evidence-ingest-acceptance", "REAL", "SYNTHETIC", "records", fixture_records, [&]() {
    for (std::size_t i = 0; i < fixture.records.size(); ++i) {
      const co::Result<co::IngestResult> outcome = pipeline.ingest(fixture.records[i], fixture.mutations[i]);
      if (!outcome.ok()) {
        fail("IngestPipeline::ingest: " + outcome.status().render());
      }
      if (outcome.value().disposition == co::IngestDisposition::Applied) {
        ++applied;
      } else {
        ++refused;
      }
    }
    return 0;  // no bytes are written: this row measures in-process derived state only
  }));
  {
    const co::IngestStatistics statistics = pipeline.statistics();
    check(applied == fixture_records && refused == 0, "every fixture record was applied by the ingest pipeline");
    check(statistics.received == fixture_records && statistics.applied == fixture_records,
          "ingest statistics account for every received record");
    check(window.size() == fixture_records, "the acceptance window holds one current record per slot");
    check(window.accepted_count() == fixture_records, "the acceptance window accepted every record");
    check(window.refusals().empty(), "no fixture record was refused");
    rows.back().detail = "IngestPipeline::ingest (no store attached) -> EvidenceWindow::consider; applied=" +
                         std::to_string(applied) + " refused=" + std::to_string(refused) +
                         " current_slots=" + std::to_string(window.size()) +
                         "; each record carries a real exact unit conversion from " +
                         std::to_string(kFixtureDimensions.size()) + " dimensions";
  }

  // --- Row 2: ledger composition, per scope and roll-up ---------------------
  const co::ScopePath per_scope = fixture.enclosure_scopes.front();
  const co::ScopePath rollup_scope = fixture.hall_scopes.front();
  // The fixture declares three dimensions; the other three are deliberately
  // left without evidence so the no-evidence path stays visible in the run.
  co::DimensionSet fixture_dimensions = co::DimensionSet::of(kFixtureDimensions[0]);
  for (std::size_t i = 1; i < kFixtureDimensions.size(); ++i) {
    fixture_dimensions.add(kFixtureDimensions[i]);
  }
  co::LedgerRequest per_request;
  per_request.scope = per_scope;
  per_request.dimensions = fixture_dimensions;
  co::LedgerRequest rollup_request;
  rollup_request.scope = rollup_scope;
  rollup_request.dimensions = fixture_dimensions;

  const auto verify_ledger = [&](const co::Ledger& ledger, std::size_t expected_records, const char* what) {
    check(ledger.lines.size() == kFixtureDimensions.size(), std::string(what) + ": one line per requested dimension");
    for (const co::LedgerLine& line : ledger.lines) {
      check(line.evidence_count == expected_records, std::string(what) + ": evidence count per line");
      // Every identity was evaluated and every residual is explained: the line
      // is complete, not residual, not partial, not indeterminate.
      check(line.state == co::LineState::Complete,
            std::string(what) + ": every closure identity was evaluated and every residual is explained (state=" +
                std::string(co::line_state_text(line.state)) + ")");
      check(line.unknown_classes == 0, std::string(what) + ": every identity class is known");
      check(!line.overcommitted, std::string(what) + ": line is not overcommitted");
      for (const co::ClosureCheck& closure : line.closures) {
        check(!closure.name.empty(), std::string(what) + ": every closure identity is named");
        check(!closure.indeterminate, std::string(what) + ": closure '" + closure.name + "' was evaluated");
        check(closure.holds, std::string(what) + ": closure '" + closure.name + "' holds");
      }
      check(line.unexplained_governance.has_value() && line.unexplained_governance.value().is_zero(),
            std::string(what) + ": the governance partition is fully explained");
      check(line.unexplained_installed.has_value() && line.unexplained_installed.value().is_zero(),
            std::string(what) + ": the installed partition is fully explained");
      check(line.unexplained_available.has_value() && line.unexplained_available.value().is_zero(),
            std::string(what) + ": the declared available figure is fully explained");
    }
    check(!ledger.has_residuals(), std::string(what) + ": no residual is left unexplained");
    check(!ledger.has_overcommitment(), std::string(what) + ": no dimension is overcommitted");
  };

  co::Ledger per_ledger;
  rows.push_back(measure_row("ledger-compose-per-scope", "REAL", "SYNTHETIC", "ledgers", config.ledger_reps, [&]() {
    for (std::size_t i = 0; i < config.ledger_reps; ++i) {
      per_ledger = value_or_fail(co::compose_ledger(window, per_request), "compose_ledger");
    }
    return 0;
  }));
  verify_ledger(per_ledger, kEnclosureClasses.size(), "per-scope ledger");
  {
    const double records_per_sec = static_cast<double>(config.ledger_reps) *
                                   static_cast<double>(window.size()) / rows.back().seconds;
    rows.back().detail = "compose_ledger over window_records=" + std::to_string(window.size()) +
                         " matching_records=" + std::to_string(kEnclosureClasses.size()) +
                         " per dimension; scans the whole window then aggregates one enclosure scope; "
                         "records_scanned/sec=" + number(records_per_sec, 0) + "; line_state=" +
                         std::string(co::line_state_text(per_ledger.lines.front().state)) +
                         " (closure 4 reports its negative residual as headroom under the governed ceiling; "
                         "the three unexplained partitions are exactly zero)";
  }

  co::Ledger rollup_ledger;
  const std::size_t rollup_matches =
      (std::min)(fixture.hall_size, config.enclosures) * kEnclosureClasses.size() + kHallClasses.size();
  rows.push_back(measure_row("ledger-compose-rollup", "REAL", "SYNTHETIC", "ledgers", config.ledger_reps, [&]() {
    for (std::size_t i = 0; i < config.ledger_reps; ++i) {
      rollup_ledger = value_or_fail(co::compose_ledger(window, rollup_request), "compose_ledger");
    }
    return 0;
  }));
  verify_ledger(rollup_ledger, rollup_matches, "roll-up ledger");
  {
    check(rollup_ledger.lines.front().contributing_scopes.size() > 1,
          "roll-up aggregates more than one contributing scope");
    check(rollup_ledger.lines.front().explanations.size() > 4, "roll-up produces per-line explanations");
    bool mentions_depths = false;
    for (const std::string& text : rollup_ledger.lines.front().explanations) {
      if (text.find("scope depths") != std::string::npos) {
        mentions_depths = true;
      }
    }
    check(mentions_depths, "roll-up records how many scope depths it aggregated");
    const double records_per_sec = static_cast<double>(config.ledger_reps) *
                                   static_cast<double>(window.size()) / rows.back().seconds;
    rows.back().detail = "compose_ledger over window_records=" + std::to_string(window.size()) +
                         " matching_records=" + std::to_string(rollup_matches) +
                         " per dimension across " +
                         std::to_string(rollup_ledger.lines.front().contributing_scopes.size()) +
                         " scopes; records_scanned/sec=" + number(records_per_sec, 0) + "; line_state=" +
                         std::string(co::line_state_text(rollup_ledger.lines.front().state)) +
                         "; the four identities still hold after aggregation because hall-level plant "
                         "telemetry carries no identity class";
  }

  // --- Row 3: a deliberately inconsistent declaration is reported ----------
  {
    // The classic double subtraction: the policy authority re-declares
    // maintenance at hall scope that its enclosures already excluded. No
    // identity may silently absorb it, so every affected residual must be the
    // exact declared-minus-derived figure.
    constexpr std::int64_t kDoubleDeclared = 1000000;
    const Declared declared = declare_exact(co::Dimension::Power, co::Amount(kDoubleDeclared));
    const ClassPlan plan{"policy", co::AuthorityRole::Policy, co::CapacityAssertion::ExcludedMaintenance,
                         co::Provenance::Observed};
    const co::EvidenceRecord extra = make_record(plan, rollup_scope, co::Dimension::Power, declared,
                                                 co::Generation(1), co::Epoch(1), instant, "bench-double-declaration");
    const co::Result<co::IngestResult> ingested = pipeline.ingest(extra, co::MutationId("mut-double-declaration"));
    check(ingested.ok() && ingested.value().applied,
          "a well formed but inconsistent declaration is still accepted as evidence");
    co::Ledger inconsistent;
    rows.push_back(measure_row("ledger-compose-inconsistent", "REAL", "SYNTHETIC", "ledgers", config.ledger_reps,
                               [&]() {
                                 for (std::size_t i = 0; i < config.ledger_reps; ++i) {
                                   inconsistent = value_or_fail(co::compose_ledger(window, rollup_request),
                                                                "compose_ledger (inconsistent)");
                                 }
                                 return 0;
                               }));
    const co::LedgerLine* line = inconsistent.find(co::Dimension::Power);
    check(line != nullptr, "the inconsistent roll-up still produces a power line");
    if (line != nullptr) {
      check(line->closures[0].residual.has_value() &&
                line->closures[0].residual.value().canonical() == -kDoubleDeclared,
            "the governance closure reports the double subtraction as an exact negative residual");
      check(line->closures[1].holds, "the installed partition still closes exactly under the inconsistency");
      check(line->closures[2].residual.has_value() &&
                line->closures[2].residual.value().canonical() == kDoubleDeclared,
            "the available closure reports the same double subtraction as an exact positive residual");
      check(line->state == co::LineState::Residual, "the inconsistent line is reported as residual");
      check(inconsistent.has_residuals(), "the ledger reports that a residual is left unexplained");
      check(!inconsistent.has_overcommitment(), "a reported residual is not an overcommitment");
    }
    rows.back().detail = "the same roll-up after the policy authority re-declares " +
                         std::to_string(kDoubleDeclared) +
                         " canonical units of maintenance at hall scope that its enclosures already excluded; "
                         "governance residual=-" + std::to_string(kDoubleDeclared) + ", available residual=+" +
                         std::to_string(kDoubleDeclared) + "; declared-minus-derived is reported, never clamped";
  }

  // --- Row 4: fragmentation analysis ----------------------------------------
  const FragmentationFixture fragmentation = make_fragmentation_fixture(config.enclosures);
  co::FragmentationReport report;
  rows.push_back(measure_row("fragmentation-analysis", "REAL", "SYNTHETIC", "analyses", config.fragmentation_reps,
                             [&]() {
                               for (std::size_t i = 0; i < config.fragmentation_reps; ++i) {
                                 report = value_or_fail(
                                     co::analyze_fragmentation(fragmentation.topology, fragmentation.profile),
                                     "analyze_fragmentation");
                               }
                               return 0;
                             }));
  {
    check(report.enclosures.size() == config.enclosures, "fragmentation reports every enclosure");
    check(report.realizable_racks <= report.ideal_racks, "the aggregate ideal bound is never below reality");
    check(report.fragmented_racks == report.ideal_racks - report.realizable_racks,
          "fragmentation is exactly ideal minus realizable");
    std::int64_t summed_realizable = 0;
    bool saw_unusable = false;
    for (const co::EnclosureCapacity& enclosure : report.enclosures) {
      summed_realizable += enclosure.realizable_racks;
      if (enclosure.binding == "structurally-unusable") {
        saw_unusable = true;
      }
    }
    check(summed_realizable == report.realizable_racks, "per-enclosure realizable racks sum to the report total");
    check(saw_unusable, "structurally unusable enclosures are reported as such");
    check(report.stranded.has(co::Dimension::Power) && report.stranded.get(co::Dimension::Power).value().canonical() > 0,
          "capacity in unusable enclosures is reported as stranded");
    rows.back().detail = "analyze_fragmentation over " + std::to_string(config.enclosures) + " enclosures (" +
                         std::to_string(fragmentation.unusable) + " structurally unusable), profile demands 5 of 6 "
                         "dimensions; realizable=" + std::to_string(report.realizable_racks) +
                         " ideal=" + std::to_string(report.ideal_racks) +
                         " fragmented=" + std::to_string(report.fragmented_racks) +
                         " indeterminate_enclosures=" + std::to_string(report.indeterminate_enclosures) +
                         "; enclosures/sec=" +
                         number(static_cast<double>(config.fragmentation_reps) *
                                    static_cast<double>(config.enclosures) / rows.back().seconds, 0);
  }

  // --- Rows 4 and 5: durable append, then reopen and recovery scan ----------
  const std::string scratch_leaf = "run-" + std::to_string(co::platform::current_process_id());
  ScratchDirectory scratch(scratch_leaf);
  std::cout << "[scratch] " << scratch.path() << " (real filesystem store; removed on exit)\n";
  const std::string store_directory = (std::filesystem::path(scratch.path()) / "store").string();

  StoreFixture store_fixture = make_store_fixture(config.append_records, rng, instant);
  co::StoreOptions store_options;
  store_options.directory = store_directory;

  std::uint64_t committed_bytes = 0;
  std::uint64_t frames_committed = 0;
  {
    co::Store store = value_or_fail(co::Store::open(store_options), "Store::open (fresh)");
    check(store.session_epoch().value() == 1, "a fresh store starts at session epoch e1");
    const std::uint64_t before = value_or_fail(store.durable_bytes(), "durable_bytes");
    rows.push_back(measure_row("store-durable-append", "REAL", "SYNTHETIC", "records", config.append_records, [&]() {
      for (std::size_t i = 0; i < store_fixture.records.size(); ++i) {
        const co::Result<co::AppendOutcome> outcome =
            store.append_evidence(store_fixture.records[i], store_fixture.mutations[i]);
        if (!outcome.ok()) {
          fail("Store::append_evidence: " + outcome.status().render());
        }
        if (!outcome.value().applied) {
          fail("Store::append_evidence reported a non-applied outcome for a fresh mutation: " +
               outcome.value().explanation);
        }
        ++frames_committed;
      }
      // The bytes this row committed are the log growth the appends produced.
      return value_or_fail(store.durable_bytes(), "durable_bytes") - before;
    }));
    committed_bytes = rows.back().bytes;
    check(store.mutations().size() == config.append_records, "every mutation identity is durable");
    check(store.last_sequence().value() == config.append_records + 1,
          "the durable sequence advanced once per committed frame (plus the epoch frame)");
    check(frames_committed == config.append_records, "every append committed a frame");
    const double per_record_bytes = static_cast<double>(committed_bytes) / static_cast<double>(config.append_records);
    rows.back().detail = "Store::append_evidence against " + store_directory +
                         "; each frame is a length+CRC32C framed JSON payload flushed with FlushFileBuffers "
                         "before the call returns (the commit point); bytes measured as log growth; bytes/record=" +
                         number(per_record_bytes, 1) + "; replaying mutation 'store-mut-0' next must be answered "
                         "IdempotentReplay";

    // Idempotent replay must be answered from the same durable commit.
    const co::Result<co::AppendOutcome> replay =
        store.append_evidence(store_fixture.records.front(), store_fixture.mutations.front());
    check(replay.ok() && !replay.value().applied && replay.value().reason == co::ReasonCode::IdempotentReplay,
          "a replayed mutation identity is answered with IdempotentReplay and no second append");
    const co::Result<co::AppendOutcome> conflict = store.append_evidence(
        store_fixture.records.back(), store_fixture.mutations.front());
    check(!conflict.ok() && conflict.status().code() == co::ReasonCode::IdempotencyConflict,
          "a reused mutation identity with different content is refused with IdempotencyConflict");
    store.close();
  }

  {
    const std::uint64_t log_bytes = committed_bytes;
    co::Store reopened;
    rows.push_back(measure_row("store-reopen-recovery", "REAL", "SYNTHETIC", "frames",
                               static_cast<std::uint64_t>(config.append_records) + 1U, [&]() -> std::uint64_t {
                                 reopened = value_or_fail(co::Store::open(store_options), "Store::open (reopen)");
                                 return log_bytes;
                               }));
    const co::RecoveryReport& recovery = reopened.recovery();
    check(recovery.frames_read == config.append_records + 1, "reopen scans every durable frame");
    check(recovery.evidence_frames == config.append_records, "reopen counts every evidence frame");
    // The recovery report describes the scan: it counts the epoch frames that
    // were already in the log, not the frame this open appends.
    check(recovery.epoch_frames == 1, "reopen counts the epoch frame found in the log");
    check(!recovery.torn_tail_recovered, "a cleanly closed log has no torn tail");
    check(reopened.last_sequence().value() == config.append_records + 2,
          "reopen resumes the durable sequence after the new epoch frame");
    check(reopened.session_epoch().value() == 2, "reopen publishes the next session epoch");
    check(reopened.mutations().size() == config.append_records, "reopen restores every mutation identity");
    rows.back().detail = "Store::open over the log written above: header CRC, then every length+CRC32C frame is "
                         "re-read, JSON parsed, and revalidated; frames=" + std::to_string(recovery.frames_read) +
                         " evidence=" + std::to_string(recovery.evidence_frames) +
                         " epoch=" + std::to_string(recovery.epoch_frames) +
                         " durable_mutations=" + std::to_string(reopened.mutations().size()) +
                         "; timing includes the next session epoch publication and the new epoch frame flush";

    const std::uint64_t log_bytes_now = committed_bytes;
    co::RecoveryReport verification;
    rows.push_back(measure_row("store-inspect-scan", "REAL", "SYNTHETIC", "frames",
                               static_cast<std::uint64_t>(config.append_records) + 2U, [&]() -> std::uint64_t {
                                 verification = value_or_fail(co::inspect_store(store_options), "inspect_store");
                                 return log_bytes_now;
                               }));
    check(verification.frames_read == config.append_records + 2,
          "the read-only verification scan sees every frame of both sessions");
    check(verification.evidence_frames == config.append_records, "the read-only scan counts every evidence frame");
    rows.back().detail = "inspect_store: read-only scan of the same real log without taking the writer lock; "
                         "frames=" + std::to_string(verification.frames_read) +
                         " evidence=" + std::to_string(verification.evidence_frames) + "; " +
                         verification.explanation;
    reopened.close();
  }

  // --- Row 6: snapshot JSON serialization -----------------------------------
  const co::ScopePath snapshot_scope = fixture.hall_scopes.front();
  const co::DimensionSet snapshot_dimensions = fixture_dimensions;
  co::Snapshot snapshot = value_or_fail(
      co::build_snapshot(window, snapshot_scope, snapshot_dimensions, &fragmentation.topology, &fragmentation.profile,
                         nullptr),
      "build_snapshot");
  std::string document;
  rows.push_back(measure_row("snapshot-json-serialize", "REAL", "SYNTHETIC", "documents", config.snapshot_reps, [&]() {
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < config.snapshot_reps; ++i) {
      const co::json::Value value = co::snapshot_to_json(snapshot);
      document = co::json::dump(value, true, -1);
      bytes = document.size();
    }
    return bytes;
  }));
  {
    check(snapshot.fragmentation.has_value(), "the snapshot carries a fragmentation section");
    check(snapshot.ledger.lines.size() == kFixtureDimensions.size(),
          "the snapshot ledger carries one line per requested dimension");
    check(!document.empty(), "the snapshot document is non-empty");
    const co::json::Value reparsed = value_or_fail(co::json::Value::parse(document), "parse snapshot");
    const std::string redumped = co::json::dump(reparsed, true, -1);
    check(redumped == document, "canonical snapshot rendering is byte-identical after a parse round trip");
    // A request that includes dimensions with no evidence at all must still
    // render a document this library's own strict parser accepts: the closure
    // identities keep their names on the no-evidence path, so the rendered
    // "closures" object has four distinct members.
    {
      const co::Snapshot wide = value_or_fail(
          co::build_snapshot(window, snapshot_scope, co::DimensionSet::all(), &fragmentation.topology,
                             &fragmentation.profile, nullptr),
          "build_snapshot (every dimension)");
      const std::string wide_document = co::json::dump(co::snapshot_to_json(wide), true, -1);
      const co::Result<co::json::Value> wide_reparsed = co::json::Value::parse(wide_document);
      check(wide_reparsed.ok(),
            std::string("a snapshot covering dimensions with no evidence re-parses: ") +
                (wide_reparsed.ok() ? std::string("ok") : wide_reparsed.status().render()));
      check(wide.ledger.lines.size() == co::kDimensionCount,
            "the all-dimension snapshot carries one line per dimension");
      std::size_t indeterminate_lines = 0;
      for (const co::LedgerLine& line : wide.ledger.lines) {
        if (line.state == co::LineState::Indeterminate) {
          ++indeterminate_lines;
          for (const co::ClosureCheck& closure : line.closures) {
            check(!closure.name.empty() && closure.indeterminate,
                  "a line without evidence names each closure identity and marks it indeterminate");
          }
        }
      }
      check(indeterminate_lines == co::kDimensionCount - kFixtureDimensions.size(),
            "dimensions without evidence are reported as indeterminate rather than as zero");
    }
    rows.back().detail = "build_snapshot -> snapshot_to_json -> json::dump(canonical, compact) over scope " +
                         snapshot_scope.text() + "; document_bytes=" + std::to_string(document.size()) +
                         " ledger_lines=" + std::to_string(snapshot.ledger.lines.size()) +
                         " contributing_records=" + std::to_string(snapshot.provenance.evidence_records) +
                         " fragmentation_enclosures=" +
                         std::to_string(snapshot.fragmentation.value().enclosures.size()) +
                         "; serialization is deterministic and round-trips byte for byte";
  }

  rows.push_back(measure_row("snapshot-json-parse", "REAL", "SYNTHETIC", "documents", config.snapshot_reps, [&]() {
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < config.snapshot_reps; ++i) {
      const co::json::Value parsed = value_or_fail(co::json::Value::parse(document), "parse snapshot");
      bytes = document.size();
      if (parsed.find("ledger") == nullptr) {
        fail("parsed snapshot has no ledger member");
      }
    }
    return bytes;
  }));
  rows.back().detail = "json::Value::parse of the document produced by the row above; the same bytes are counted, so "
                       "bytes/sec is the parser's input rate";

  // --- Rows this machine cannot measure -------------------------------------
  rows.push_back(unsupported_row("network-fabric-ingest",
                                 "not attempted: this library contains no network transport (no socket, HTTP, or "
                                 "message-queue client in src/); ingest is an in-process API call"));
  rows.push_back(unsupported_row("multi-node-cluster-throughput",
                                 "not attempted: the runtime is a single-process library with no clustering, "
                                 "replication, or consensus code; a single-node measurement cannot be reported as "
                                 "a multi-node one"));
  rows.push_back(unsupported_row("bms-dcim-protocol-ingest",
                                 "not attempted: no BMS, DCIM, Modbus, SNMP, or Redfish protocol implementation exists "
                                 "in this tree; plant evidence is a modelled authority labelled SYNTHETIC"));
  rows.push_back(unsupported_row("electrical-power-measurement",
                                 "not attempted: power is an exact integer capacity dimension (mW) asserted by an "
                                 "authority, not a measurement taken by this process; no electrical instrumentation "
                                 "is read and no measured-watt claim is made"));

  print_table(rows);

  std::size_t real_rows = 0;
  std::size_t synthetic_rows = 0;
  std::size_t unsupported_rows = 0;
  for (const Row& row : rows) {
    if (row.label == "REAL") {
      ++real_rows;
    } else if (row.label == "SYNTHETIC") {
      ++synthetic_rows;
    } else {
      ++unsupported_rows;
    }
  }
  std::cout << "[summary] rows=" << rows.size() << " real=" << real_rows << " synthetic=" << synthetic_rows
            << " unsupported=" << unsupported_rows << " checks_passed=" << g_checks_passed
            << " checks_failed=" << g_checks_failed << '\n';
  std::cout << "[result] " << (g_checks_failed == 0 ? "PASS" : "FAIL") << '\n';
  return g_checks_failed == 0 ? 0 : 1;
}

void print_usage() {
  std::cout << "capacity-observatory-bench [--quick]\n"
               "  --quick   measure every row with smaller inputs (still real work)\n";
}

}  // namespace

int main(int argc, char** argv) {
  co::platform::suppress_error_dialogs();
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--quick") {
      config.quick = true;
    } else if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", std::string(argument).c_str());
      print_usage();
      return 2;
    }
  }

  if (config.quick) {
    config.enclosures = 500;
    config.ledger_reps = 20;
    config.fragmentation_reps = 20;
    config.append_records = 1000;
    config.snapshot_reps = 20;
  } else {
    config.enclosures = 2000;
    config.ledger_reps = 50;
    config.fragmentation_reps = 50;
    config.append_records = 5000;
    config.snapshot_reps = 50;
  }

  try {
    return run(config);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "[bench] FAILED with an unexpected exception: %s\n", error.what());
    return 1;
  }
}
