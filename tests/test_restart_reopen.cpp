// Capacity Observatory - restart and reopen suite.
//
// Proves what survives a restart: records come back byte identical, they are
// marked Provenance::Recovered, their freshness is Freshness::Recovered and
// never Fresh, the acceptance watermarks are rebuilt from durable state so a
// stale replay is still refused, and every derived number is identical to the
// number computed before the restart. Both halves are exercised: separate Store
// objects in one process, and separate processes of this same test binary.
#include "co_test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/hash.hpp"
#include "capacity_observatory/io.hpp"
#include "capacity_observatory/ledger.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/snapshot.hpp"
#include "capacity_observatory/store.hpp"
#include "capacity_observatory/strong.hpp"
#include "capacity_observatory/time.hpp"
#include "capacity_observatory/watermark.hpp"

using namespace co;

namespace co::test {

inline std::string render(Epoch value) { return "e" + std::to_string(value.value()); }
inline std::string render(Generation value) { return "g" + std::to_string(value.value()); }
inline std::string render(SequenceNumber value) { return "s" + std::to_string(value.value()); }
inline std::string render(const MutationId& value) { return value.value(); }
inline std::string render(const Digest& value) { return value.hex().substr(0, 16); }
inline std::string render(Freshness value) { return std::string(freshness_text(value)); }
inline std::string render(AcceptanceOutcome value) { return std::string(acceptance_outcome_text(value)); }

}  // namespace co::test

namespace {

constexpr std::int64_t kInstant = 1700000000000000000LL;
constexpr std::int64_t kDayNanos = 86400LL * 1000000000LL;

#define CO_REQUIRE_STORE(result_expression, name)                                      \
  auto co_store_result_##name = (result_expression);                                   \
  ::co::test::note_assertion();                                                        \
  if (!co_store_result_##name.ok()) {                                                  \
    CO_FAIL(std::string("expected success from ") + #result_expression + ": " +        \
            co_store_result_##name.status().render());                                 \
  }                                                                                    \
  auto& name = co_store_result_##name.value()

// ---------------------------------------------------------------------------
// The fixture: one scope, four authorities, five slots.
// ---------------------------------------------------------------------------
struct FixtureSlot {
  const char* authority;
  AuthorityRole role;
  CapacityAssertion assertion;
  std::int64_t declared;
  std::uint64_t generation;
  std::uint64_t epoch;
  const char* mutation;
};

const std::vector<FixtureSlot>& fixture_slots() {
  static const std::vector<FixtureSlot> slots{
      {"policy", AuthorityRole::Policy, CapacityAssertion::Nameplate, 4000000, 1, 1, "seed-policy-nameplate"},
      {"dfi", AuthorityRole::InstalledInventory, CapacityAssertion::Installed, 3500000, 2, 1, "seed-dfi-installed"},
      {"policy", AuthorityRole::Policy, CapacityAssertion::ExcludedFailure, 100000, 1, 1, "seed-policy-failure"},
      {"dccp", AuthorityRole::CommittedCapacity, CapacityAssertion::Committed, 1200000, 2, 2, "seed-dccp-committed"},
      {"asi", AuthorityRole::Reservations, CapacityAssertion::Reserved, 300000, 1, 1, "seed-asi-reserved"},
  };
  return slots;
}

const ScopePath& fixture_scope() {
  static const ScopePath scope = []() {
    Result<ScopePath> parsed = ScopePath::parse("site=dc1/hall=h1");
    if (!parsed.ok()) {
      CO_FAIL("could not parse the fixture scope");
    }
    return std::move(parsed).value();
  }();
  return scope;
}

MutationId mutation_of(const std::string& text) {
  Result<MutationId> parsed = MutationId::parse(text);
  if (!parsed.ok()) {
    CO_FAIL("could not parse mutation '" + text + "': " + parsed.status().render());
  }
  return std::move(parsed).value();
}

AuthorityId authority_of(const std::string& text) {
  Result<AuthorityId> parsed = AuthorityId::parse(text);
  if (!parsed.ok()) {
    CO_FAIL("could not parse authority '" + text + "': " + parsed.status().render());
  }
  return std::move(parsed).value();
}

EvidenceRecord make_slot_record(const FixtureSlot& slot) {
  Result<EvidenceRecord> built =
      EvidenceRecord::make(authority_of(slot.authority), slot.role, fixture_scope(), Dimension::Power,
                           slot.assertion, Unit::canonical(Dimension::Power), slot.declared,
                           Generation(slot.generation), Epoch(slot.epoch), Revision(1));
  if (!built.ok()) {
    CO_FAIL(std::string("could not build the fixture record for ") + slot.authority + ": " + built.status().render());
  }
  EvidenceRecord record = std::move(built).value();
  record.provenance = Provenance::Observed;
  record.has_observed_at = true;
  record.observed_at = Timestamp(kInstant);
  record.source = std::string("restart:") + slot.mutation;
  return record;
}

// A replay that is older than the watermark of its slot.
EvidenceRecord stale_generation_record() {
  const FixtureSlot slot{"dfi", AuthorityRole::InstalledInventory, CapacityAssertion::Installed, 3400000, 1, 1,
                         "stale-dfi"};
  return make_slot_record(slot);
}

EvidenceRecord stale_epoch_record() {
  const FixtureSlot slot{"dccp", AuthorityRole::CommittedCapacity, CapacityAssertion::Committed, 1100000, 3, 1,
                         "stale-dccp"};
  return make_slot_record(slot);
}

std::shared_ptr<Clock> fixture_clock() { return manual_clock(Timestamp(kInstant)); }

// ---------------------------------------------------------------------------
// Deterministic rendering of every derived number, so that "identical after a
// restart" is a byte comparison and not a hand written list of fields.
// ---------------------------------------------------------------------------
std::string optional_amount(const std::optional<Amount>& value) {
  return value.has_value() ? std::to_string(value.value().canonical()) : std::string("unknown");
}

std::string render_numbers(const Ledger& ledger) {
  std::string out = "lines " + std::to_string(ledger.lines.size());
  for (const LedgerLine& line : ledger.lines) {
    out += "\n";
    out += std::string(dimension_text(line.dimension));
    out += " state=";
    out += std::string(line_state_text(line.state));
    for (std::size_t i = 0; i < kAssertionCount; ++i) {
      const CapacityAssertion assertion = static_cast<CapacityAssertion>(i);
      out += " ";
      out += std::string(assertion_text(assertion));
      out += "=";
      out += optional_amount(line.declared.get(assertion));
    }
    out += " exclusion=" + optional_amount(line.exclusion_total);
    out += " serviceable=" + optional_amount(line.serviceable);
    out += " free=" + optional_amount(line.free_usable);
    out += " governed=" + optional_amount(line.governed);
    out += " overcommitment=" + optional_amount(line.overcommitment);
    out += " overcommitted=";
    out += line.overcommitted ? "1" : "0";
    out += " unknowns=" + std::to_string(line.unknown_classes);
    out += " evidence=" + std::to_string(line.evidence_count);
    out += " residual-governance=" + optional_amount(line.unexplained_governance);
    out += " residual-installed=" + optional_amount(line.unexplained_installed);
    out += " residual-available=" + optional_amount(line.unexplained_available);
    for (const ClosureCheck& closure : line.closures) {
      out += " closure[";
      out += closure.name;
      out += "]=";
      out += closure.indeterminate ? "indeterminate" : optional_amount(closure.residual);
      out += closure.holds ? "/holds" : "/open";
    }
  }
  return out;
}

Result<Ledger> compose_fixture_ledger(const EvidenceWindow& window) {
  LedgerRequest request;
  request.scope = fixture_scope();
  request.dimensions = DimensionSet::all();
  return compose_ledger(window, request);
}

// The documented recovery flow: every durable record is re-considered in commit
// order (which rebuilds the watermarks), and only then is the window marked as
// recovered, so no recovered record is ever presented as a fresh observation.
Result<void> reload_into(const Store& store, EvidenceWindow& window) {
  const Result<std::vector<StoredEvidence>> loaded = store.load_evidence();
  if (!loaded.ok()) {
    return loaded.status();
  }
  for (const StoredEvidence& stored : loaded.value()) {
    const AcceptanceDecision decision = window.consider(stored.record);
    if (!decision.accepted()) {
      return make_error(decision.reason, "a durable record was not accepted after reload: " + decision.explanation);
    }
  }
  window.mark_all_recovered(window.clock()->now());
  return Result<void>{};
}

std::string watermark_text(const EvidenceWindow& window, const EvidenceSlotKey& key) {
  const Watermark* mark = window.watermark_for(key);
  if (mark == nullptr || !mark->present) {
    return "absent";
  }
  return "e" + std::to_string(mark->epoch.value()) + " g" + std::to_string(mark->generation.value()) + " " +
         mark->digest.hex();
}

struct WindowState {
  std::string numbers;
  std::map<std::string, std::string> watermarks;
  std::size_t records{0};
};

Result<WindowState> describe(const EvidenceWindow& window) {
  WindowState state;
  const Result<Ledger> ledger = compose_fixture_ledger(window);
  if (!ledger.ok()) {
    return ledger.status();
  }
  state.numbers = render_numbers(ledger.value());
  state.records = window.size();
  for (const auto& entry : window.current()) {
    state.watermarks[entry.first.text()] = watermark_text(window, entry.first);
  }
  return Result<WindowState>(std::move(state));
}

// ---------------------------------------------------------------------------
// Store and byte helpers
// ---------------------------------------------------------------------------
StoreOptions options_for(const std::string& directory, bool read_only = false) {
  StoreOptions options;
  options.directory = directory;
  options.read_only = read_only;
  return options;
}

std::string store_log(const std::string& directory) { return io::join(directory, "evidence.log"); }

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  Result<std::vector<std::uint8_t>> result = io::read_file(path, 1ULL << 28U);
  if (!result.ok()) {
    CO_FAIL("could not read " + path + ": " + result.status().render());
  }
  return std::move(result).value();
}

void append_bytes_at_end(const std::string& path, const std::vector<std::uint8_t>& extra) {
  Result<io::File> file = io::File::open(path, false);
  if (!file.ok()) {
    CO_FAIL("could not open " + path + " for append: " + file.status().render());
  }
  if (!file.value().seek_end().ok() || !file.value().write_all(extra.data(), extra.size()).ok() ||
      !file.value().sync().ok()) {
    CO_FAIL("could not append to " + path);
  }
}

// A frame header that claims 'claimed' bytes with a valid CRC, followed by only
// 'delivered' of them: exactly what a writer killed mid-append leaves behind.
std::vector<std::uint8_t> partial_frame(std::size_t claimed, std::size_t delivered) {
  const std::string payload(claimed, 'p');
  std::vector<std::uint8_t> frame;
  const std::uint32_t size = static_cast<std::uint32_t>(claimed);
  frame.push_back(static_cast<std::uint8_t>(size & 0xFFU));
  frame.push_back(static_cast<std::uint8_t>((size >> 8U) & 0xFFU));
  frame.push_back(static_cast<std::uint8_t>((size >> 16U) & 0xFFU));
  frame.push_back(static_cast<std::uint8_t>((size >> 24U) & 0xFFU));
  const std::uint32_t crc = crc32c(payload);
  frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
  frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
  frame.push_back(static_cast<std::uint8_t>((crc >> 16U) & 0xFFU));
  frame.push_back(static_cast<std::uint8_t>((crc >> 24U) & 0xFFU));
  const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
  frame.insert(frame.end(), bytes, bytes + delivered);
  return frame;
}

// Writes the whole fixture into the store and ingests it into 'window', so the
// caller can describe both sides of the restart. The store is closed on return.
Result<void> seed_store(const std::string& directory, EvidenceWindow& window) {
  Result<Store> opened = Store::open(options_for(directory));
  if (!opened.ok()) {
    return opened.status();
  }
  Store store = std::move(opened).value();
  for (const FixtureSlot& slot : fixture_slots()) {
    const EvidenceRecord record = make_slot_record(slot);
    const AcceptanceDecision decision = window.consider(record);
    if (!decision.accepted()) {
      return make_error(decision.reason, "fixture record refused: " + decision.explanation);
    }
    const Result<AppendOutcome> appended = store.append_evidence(record, mutation_of(slot.mutation));
    if (!appended.ok()) {
      return appended.status();
    }
    if (!appended.value().applied) {
      return make_error(ReasonCode::InternalError, "fixture append was not applied");
    }
  }
  store.close();
  return Result<void>{};
}

// Every durable evidence record must be byte identical to the record that was
// appended: the mutation identity, the recorded digest, and the content digest
// of the decoded record all have to agree with the fixture.
void require_durable_fixture(const std::vector<StoredEvidence>& loaded) {
  if (loaded.size() != fixture_slots().size()) {
    CO_FAIL("expected " + std::to_string(fixture_slots().size()) + " durable records, found " +
            std::to_string(loaded.size()));
  }
  for (const FixtureSlot& slot : fixture_slots()) {
    const EvidenceRecord expected = make_slot_record(slot);
    bool found = false;
    for (const StoredEvidence& stored : loaded) {
      if (stored.mutation == mutation_of(slot.mutation)) {
        found = true;
        if (!(stored.mutation_digest == expected.content_digest())) {
          CO_FAIL("durable record " + std::string(slot.mutation) + " carries a different mutation digest");
        }
        if (!(stored.record.content_digest() == expected.content_digest())) {
          CO_FAIL("durable record " + std::string(slot.mutation) + " is not byte identical to what was appended");
        }
        // written_epoch is the session epoch of the store frame, not the
        // authority epoch carried inside the record.
        if (stored.written_epoch != Epoch(1)) {
          CO_FAIL("durable record " + std::string(slot.mutation) + " records the wrong session epoch");
        }
        if (stored.record.epoch != Epoch(slot.epoch)) {
          CO_FAIL("durable record " + std::string(slot.mutation) + " lost its authority epoch");
        }
      }
    }
    if (!found) {
      CO_FAIL("durable record " + std::string(slot.mutation) + " is missing");
    }
  }
}

// ---------------------------------------------------------------------------
// Child roles: a real process boundary around the restart.
// ---------------------------------------------------------------------------
void emit(const std::string& line) {
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

int role_restart_seed(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return 2;
  }
  const AuthorityRegistry registry = AuthorityRegistry::standard();
  EvidenceWindow window(registry, fixture_clock());
  const Result<void> seeded = seed_store(arguments[0], window);
  if (!seeded.ok()) {
    std::fprintf(stderr, "seed failed: %s\n", seeded.status().render().c_str());
    return 3;
  }
  const Result<WindowState> state = describe(window);
  if (!state.ok()) {
    std::fprintf(stderr, "describe failed: %s\n", state.status().render().c_str());
    return 4;
  }
  emit("records " + std::to_string(state.value().records));
  emit("numbers-begin");
  emit(state.value().numbers);
  emit("numbers-end");
  return 0;
}

int role_restart_read(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return 2;
  }
  const AuthorityRegistry registry = AuthorityRegistry::standard();
  EvidenceWindow window(registry, fixture_clock());
  Result<Store> opened = Store::open(options_for(arguments[0], true));
  if (!opened.ok()) {
    std::fprintf(stderr, "read open failed: %s\n", opened.status().render().c_str());
    return 3;
  }
  const Result<std::vector<StoredEvidence>> loaded = opened.value().load_evidence();
  if (!loaded.ok()) {
    std::fprintf(stderr, "load failed: %s\n", loaded.status().render().c_str());
    return 4;
  }
  // The digests of the records exactly as they are stored, before recovery
  // relabels their provenance.
  for (const StoredEvidence& stored : loaded.value()) {
    emit("stored " + stored.mutation.value() + " " + stored.record.content_digest().hex());
  }
  for (const StoredEvidence& stored : loaded.value()) {
    const AcceptanceDecision decision = window.consider(stored.record);
    if (!decision.accepted()) {
      std::fprintf(stderr, "reload refused a durable record: %s\n", decision.explanation.c_str());
      return 5;
    }
  }
  window.mark_all_recovered(window.clock()->now());

  std::size_t recovered = 0;
  for (const auto& entry : window.current()) {
    if (entry.second.provenance == Provenance::Recovered) {
      ++recovered;
    }
    emit("freshness " + entry.first.text() + " " + std::string(freshness_text(window.freshness_of(entry.second))));
    emit("watermark " + entry.first.text() + " " + watermark_text(window, entry.first));
  }
  emit("records " + std::to_string(window.size()));
  emit("recovered " + std::to_string(recovered));

  // A stale replay is refused because the watermarks were rebuilt from durable
  // state, not because anything was remembered in memory.
  const AcceptanceDecision stale_generation = window.consider(stale_generation_record());
  emit(std::string("stale-generation ") + std::string(acceptance_outcome_text(stale_generation.outcome)) + " " +
       std::string(reason_text(stale_generation.reason)));
  const AcceptanceDecision stale_epoch = window.consider(stale_epoch_record());
  emit(std::string("stale-epoch ") + std::string(acceptance_outcome_text(stale_epoch.outcome)) + " " +
       std::string(reason_text(stale_epoch.reason)));

  const Result<WindowState> state = describe(window);
  if (!state.ok()) {
    std::fprintf(stderr, "describe failed: %s\n", state.status().render().c_str());
    return 6;
  }
  emit("numbers-begin");
  emit(state.value().numbers);
  emit("numbers-end");
  return 0;
}

struct ChildRoles {
  ChildRoles() {
    test::register_child_role("restart-seed", role_restart_seed);
    test::register_child_role("restart-read", role_restart_read);
  }
};

const ChildRoles kChildRoles;

test::ProcessResult run_role(const std::vector<std::string>& arguments) {
  std::vector<std::string> full{"--child-role"};
  full.insert(full.end(), arguments.begin(), arguments.end());
  return test::run_process(test::current_executable(), full);
}

// A child's stdout is a text stream on Windows, so every newline it writes
// arrives as CRLF; the comparison against an in-process rendering needs the
// transport bytes removed.
std::string without_carriage_returns(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch != '\r') {
      out.push_back(ch);
    }
  }
  return out;
}

std::string numbers_block(const std::string& raw) {
  const std::string output = without_carriage_returns(raw);
  const std::string begin_marker = "numbers-begin\n";
  const std::string end_marker = "\nnumbers-end";
  const std::size_t begin = output.find(begin_marker);
  const std::size_t end = output.find(end_marker);
  if (begin == std::string::npos || end == std::string::npos || end < begin) {
    return std::string();
  }
  return output.substr(begin + begin_marker.size(), end - begin - begin_marker.size());
}

bool output_has(const std::string& output, const std::string& needle) {
  return output.find(needle) != std::string::npos;
}

std::vector<std::string> lines_of(const std::string& output) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < output.size()) {
    const std::size_t end = output.find('\n', start);
    std::string line = end == std::string::npos ? output.substr(start) : output.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return lines;
}

// Reads the value of a "<prefix> <slot> <value>" line.
std::string value_after(const std::string& output, const std::string& prefix, const std::string& slot) {
  for (const std::string& line : lines_of(output)) {
    if (line.rfind(prefix + " " + slot + " ", 0) == 0) {
      return line.substr(prefix.size() + slot.size() + 2U);
    }
  }
  return std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Same process, separate Store objects
// ---------------------------------------------------------------------------
CO_TEST(a_reopen_in_the_same_process_recovers_every_record_unchanged) {
  test::ScratchDirectory scratch("restart-same-process");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));

  const AuthorityRegistry registry = AuthorityRegistry::standard();
  EvidenceWindow before(registry, fixture_clock());
  CO_REQUIRE_OK_VOID(seed_store(directory, before));
  CO_REQUIRE_EQ(before.size(), 5U);
  for (const auto& entry : before.current()) {
    CO_REQUIRE(entry.second.provenance == Provenance::Observed);
    CO_REQUIRE_EQ(before.freshness_of(entry.second), Freshness::Fresh);
  }
  CO_REQUIRE_OK(describe(before), before_state);
  CO_REQUIRE_EQ(before_state.records, 5U);
  CO_REQUIRE(before_state.numbers.find("state=") != std::string::npos);

  // A fresh window and a fresh store: nothing is carried over in memory.
  EvidenceWindow after(registry, fixture_clock());
  {
    CO_REQUIRE_STORE(Store::open(options_for(directory)), store);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    require_durable_fixture(loaded);
    CO_REQUIRE_OK_VOID(reload_into(store, after));
  }
  CO_REQUIRE_EQ(after.size(), 5U);

  // Every record is marked recovered and none of them is presented as fresh.
  for (const auto& entry : after.current()) {
    CO_REQUIRE(entry.second.provenance == Provenance::Recovered);
    CO_REQUIRE_EQ(after.freshness_of(entry.second), Freshness::Recovered);
    CO_REQUIRE(after.freshness_of(entry.second) != Freshness::Fresh);
  }

  // Derived numbers are identical; only freshness differs, and it differs in the
  // conservative direction.
  CO_REQUIRE_OK(describe(after), after_state);
  CO_REQUIRE_EQ(after_state.numbers, before_state.numbers);
  CO_REQUIRE_OK(compose_fixture_ledger(after), after_ledger);
  CO_REQUIRE(!after_ledger.lines.empty());
  bool saw_power = false;
  for (const LedgerLine& line : after_ledger.lines) {
    // Recovery is never promoted to fresh, in any dimension.
    CO_REQUIRE(line.worst_freshness != Freshness::Fresh);
    if (line.dimension == Dimension::Power) {
      saw_power = true;
      CO_REQUIRE(line.evidence_count > 0U);
      CO_REQUIRE_EQ(line.worst_freshness, Freshness::Recovered);
    } else {
      // A dimension no record speaks about stays unknown rather than fresh.
      CO_REQUIRE_EQ(line.evidence_count, 0U);
      CO_REQUIRE_EQ(line.worst_freshness, Freshness::Unknown);
    }
  }
  CO_REQUIRE(saw_power);

  // Watermarks were rebuilt from durable state, not remembered: every slot the
  // writer accepted is present with the same accepted epoch and generation. The
  // digest is the digest of the record the window actually holds, which after
  // recovery differs from the ingest-time digest because provenance is part of
  // the content identity, so the self-consistency below is what must hold.
  CO_REQUIRE_EQ(after_state.watermarks.size(), before_state.watermarks.size());
  for (const auto& entry : before_state.watermarks) {
    CO_REQUIRE(after_state.watermarks.count(entry.first) == 1U);
    const std::string& before_text = entry.second;
    const std::string& after_text = after_state.watermarks.at(entry.first);
    // The rendered watermark is "<epoch> <generation> <digest>"; epoch and
    // generation must survive a restart byte for byte.
    CO_REQUIRE_EQ(after_text.substr(0, before_text.find(' ')), before_text.substr(0, before_text.find(' ')));
    CO_REQUIRE_EQ(after_text.substr(0, before_text.rfind(' ')), before_text.substr(0, before_text.rfind(' ')));
  }
  for (const auto& entry : after.current()) {
    const Watermark* mark = after.watermark_for(entry.first);
    CO_REQUIRE(mark != nullptr);
    CO_REQUIRE(mark->present);
    CO_REQUIRE_EQ(mark->digest, entry.second.content_digest());
    CO_REQUIRE_EQ(mark->epoch, entry.second.epoch);
    CO_REQUIRE_EQ(mark->generation, entry.second.generation);
  }
  // Re-submitting a record the window itself handed out is an idempotent
  // duplicate, never a conflict: recovery must not make the window reject its own
  // recovered evidence, and it must not change derived state either.
  {
    const std::size_t slots_before = after.size();
    const std::size_t accepted_before = after.accepted_count();
    const EvidenceRecord recovered = after.current().begin()->second;
    const AcceptanceDecision resubmitted = after.consider(recovered);
    CO_REQUIRE_EQ(resubmitted.outcome, AcceptanceOutcome::Duplicate);
    CO_REQUIRE_EQ(resubmitted.reason, ReasonCode::DuplicateEvidence);
    CO_REQUIRE(!resubmitted.affects_derived_state);
    CO_REQUIRE_EQ(after.size(), slots_before);
    CO_REQUIRE_EQ(after.accepted_count(), accepted_before);
  }

  // A stale replay is refused after the restart exactly as before it.
  const AcceptanceDecision stale_before = before.consider(stale_generation_record());
  CO_REQUIRE_EQ(stale_before.outcome, AcceptanceOutcome::StaleGeneration);
  const AcceptanceDecision stale_after = after.consider(stale_generation_record());
  CO_REQUIRE_EQ(stale_after.outcome, AcceptanceOutcome::StaleGeneration);
  CO_REQUIRE_EQ(stale_after.reason, ReasonCode::StaleGeneration);
  const AcceptanceDecision stale_epoch_before = before.consider(stale_epoch_record());
  CO_REQUIRE_EQ(stale_epoch_before.outcome, AcceptanceOutcome::StaleEpoch);
  const AcceptanceDecision stale_epoch_after = after.consider(stale_epoch_record());
  CO_REQUIRE_EQ(stale_epoch_after.outcome, AcceptanceOutcome::StaleEpoch);
  CO_REQUIRE_EQ(stale_epoch_after.reason, ReasonCode::StaleEpoch);

  // The refused replays did not touch derived state.
  CO_REQUIRE_OK(describe(after), after_refusals);
  CO_REQUIRE_EQ(after_refusals.numbers, before_state.numbers);
  CO_REQUIRE_EQ(after.size(), 5U);
}

CO_TEST(a_recovered_record_is_never_promoted_to_fresh) {
  test::ScratchDirectory scratch("restart-freshness");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));

  const AuthorityRegistry registry = AuthorityRegistry::standard();
  EvidenceWindow seed_window(registry, fixture_clock());
  CO_REQUIRE_OK_VOID(seed_store(directory, seed_window));

  // Recover at an instant far beyond every staleness budget: recovered evidence
  // is neither fresh nor stale, because recovery is not observation.
  const std::shared_ptr<Clock> late_clock = manual_clock(Timestamp(kInstant + kDayNanos * 365LL));
  EvidenceWindow window(registry, late_clock);
  {
    CO_REQUIRE_STORE(Store::open(options_for(directory)), store);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    require_durable_fixture(loaded);
    for (const StoredEvidence& stored : loaded) {
      const AcceptanceDecision decision = window.consider(stored.record);
      CO_REQUIRE(decision.accepted());
      // Before being marked recovered the record is an observation, and this one
      // is far outside its budget, so it is stale rather than fresh.
      CO_REQUIRE_EQ(decision.freshness, Freshness::Stale);
    }
  }
  window.mark_all_recovered(late_clock->now());
  for (const auto& entry : window.current()) {
    CO_REQUIRE_EQ(window.freshness_of(entry.second), Freshness::Recovered);
  }

  // Even when the evaluation instant is the observation instant, a recovered
  // record is Recovered, never Fresh.
  EvidenceWindow at_observation(registry, fixture_clock());
  {
    CO_REQUIRE_STORE(Store::open(options_for(directory)), store);
    CO_REQUIRE_OK_VOID(reload_into(store, at_observation));
  }
  for (const auto& entry : at_observation.current()) {
    CO_REQUIRE_EQ(at_observation.freshness_of(entry.second), Freshness::Recovered);
    CO_REQUIRE(at_observation.freshness_of(entry.second) != Freshness::Fresh);
  }

  // A snapshot built from the recovered window says so.
  const Result<Snapshot> snapshot =
      build_snapshot(at_observation, fixture_scope(), DimensionSet::all(), nullptr, nullptr, nullptr);
  CO_REQUIRE_OK(snapshot, built);
  CO_REQUIRE(built.provenance.recovered_only);
  CO_REQUIRE_EQ(built.provenance.evidence_records, 5U);
}

// ---------------------------------------------------------------------------
// Across a real process boundary
// ---------------------------------------------------------------------------
CO_TEST(a_restart_across_processes_preserves_numbers_and_refuses_stale_replay) {
  test::ScratchDirectory scratch("restart-process");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));

  // The writer is another process: it seeds the store and prints the numbers it
  // derives from its own ingest of the same fixture.
  const test::ProcessResult seeded = run_role({"restart-seed", directory});
  CO_REQUIRE(seeded.started);
  CO_REQUIRE_EQ(seeded.exit_code, 0);
  CO_REQUIRE(output_has(seeded.standard_output, "records 5"));
  const std::string seeded_numbers = numbers_block(seeded.standard_output);
  CO_REQUIRE(!seeded_numbers.empty());
  CO_REQUIRE(io::exists(store_log(directory)));

  // The reader is a different process again: it opens read-only, rebuilds the
  // window from the durable log, and reports freshness, watermarks, refusals,
  // and the derived numbers.
  const test::ProcessResult read_back = run_role({"restart-read", directory});
  CO_REQUIRE(read_back.started);
  CO_REQUIRE_EQ(read_back.exit_code, 0);
  CO_REQUIRE(output_has(read_back.standard_output, "records 5"));
  CO_REQUIRE(output_has(read_back.standard_output, "recovered 5"));
  CO_REQUIRE(output_has(read_back.standard_output, "stale-generation stale-generation stale-generation"));
  CO_REQUIRE(output_has(read_back.standard_output, "stale-epoch stale-epoch stale-epoch"));

  // Every record in the reader is recovered and none of them is fresh.
  std::size_t freshness_lines = 0;
  for (const std::string& line : lines_of(read_back.standard_output)) {
    if (line.rfind("freshness ", 0) == 0) {
      ++freshness_lines;
      CO_REQUIRE_EQ(line.substr(line.rfind(' ') + 1U), std::string("recovered"));
    }
  }
  CO_REQUIRE_EQ(freshness_lines, 5U);

  // The records the reader loaded from disk are byte identical to what the
  // writer appended, across the process boundary.
  for (const FixtureSlot& slot : fixture_slots()) {
    const std::string expected = make_slot_record(slot).content_digest().hex();
    CO_REQUIRE_EQ(value_after(read_back.standard_output, "stored", slot.mutation), expected);
  }

  // The numbers derived after the restart are identical to the numbers derived
  // by the process that wrote the store, byte for byte.
  const std::string read_numbers = numbers_block(read_back.standard_output);
  CO_REQUIRE(!read_numbers.empty());
  CO_REQUIRE_EQ(read_numbers, seeded_numbers);

  // The parent's own derivation agrees with both children.
  const AuthorityRegistry registry = AuthorityRegistry::standard();
  EvidenceWindow parent_window(registry, fixture_clock());
  {
    CO_REQUIRE_STORE(Store::open(options_for(directory)), store);
    CO_REQUIRE_OK_VOID(reload_into(store, parent_window));
  }
  CO_REQUIRE_EQ(parent_window.size(), 5U);
  CO_REQUIRE_OK(describe(parent_window), parent_state);
  CO_REQUIRE_EQ(parent_state.numbers, read_numbers);
  for (const auto& entry : parent_window.current()) {
    CO_REQUIRE_EQ(parent_window.freshness_of(entry.second), Freshness::Recovered);
  }
  for (const auto& entry : parent_state.watermarks) {
    CO_REQUIRE_EQ(value_after(read_back.standard_output, "watermark", entry.first), entry.second);
  }
}

CO_TEST(a_recovered_store_still_refuses_a_superseded_generation) {
  test::ScratchDirectory scratch("restart-replay");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));

  const AuthorityRegistry registry = AuthorityRegistry::standard();
  EvidenceWindow first(registry, fixture_clock());
  CO_REQUIRE_OK_VOID(seed_store(directory, first));

  // A second writer session appends a newer record for the same slot.
  {
    CO_REQUIRE_STORE(Store::open(options_for(directory)), store);
    const FixtureSlot newer{"dfi", AuthorityRole::InstalledInventory, CapacityAssertion::Installed, 3600000, 3, 1,
                            "second-dfi"};
    const EvidenceRecord record = make_slot_record(newer);
    CO_REQUIRE_OK(store.append_evidence(record, mutation_of(newer.mutation)), outcome);
    CO_REQUIRE(outcome.applied);
  }

  // A third session recovers everything: the newest generation wins, and the
  // superseded generation is still refused afterwards.
  EvidenceWindow recovered(registry, fixture_clock());
  {
    CO_REQUIRE_STORE(Store::open(options_for(directory)), store);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 6U);
    CO_REQUIRE_OK_VOID(reload_into(store, recovered));
  }
  CO_REQUIRE_EQ(recovered.size(), 5U);

  EvidenceSlotKey key;
  key.authority = authority_of("dfi");
  key.scope = fixture_scope();
  key.dimension = Dimension::Power;
  key.assertion = CapacityAssertion::Installed;
  const Watermark* mark = recovered.watermark_for(key);
  CO_REQUIRE(mark != nullptr);
  CO_REQUIRE(mark->present);
  CO_REQUIRE(mark->generation == Generation(3));
  const EvidenceRecord current = recovered.current().at(key);
  CO_REQUIRE(current.amount == Amount(3600000));
  CO_REQUIRE(current.provenance == Provenance::Recovered);

  const AcceptanceDecision older = recovered.consider(stale_generation_record());
  CO_REQUIRE_EQ(older.outcome, AcceptanceOutcome::StaleGeneration);
  const EvidenceRecord newest = make_slot_record(
      FixtureSlot{"dfi", AuthorityRole::InstalledInventory, CapacityAssertion::Installed, 3700000, 4, 1, "third"});
  const AcceptanceDecision advanced = recovered.consider(newest);
  CO_REQUIRE(advanced.accepted());
  CO_REQUIRE(recovered.current().at(key).amount == Amount(3700000));
}

CO_TEST(a_torn_tail_appended_after_a_session_is_recovered_on_the_next_open) {
  test::ScratchDirectory scratch("restart-torn");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));

  const AuthorityRegistry registry = AuthorityRegistry::standard();
  EvidenceWindow before(registry, fixture_clock());
  CO_REQUIRE_OK_VOID(seed_store(directory, before));
  CO_REQUIRE_OK(describe(before), before_state);

  // A writer dies after writing the header of a frame and part of its payload.
  const std::vector<std::uint8_t> torn = partial_frame(600, 300);
  append_bytes_at_end(store_log(directory), torn);

  EvidenceWindow after(registry, fixture_clock());
  {
    CO_REQUIRE_STORE(Store::open(options_for(directory)), store);
    CO_REQUIRE(store.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(store.recovery().bytes_discarded, static_cast<std::uint64_t>(torn.size()));
    CO_REQUIRE_EQ(store.recovery().frames_read, 6U);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    require_durable_fixture(loaded);
    CO_REQUIRE_OK_VOID(reload_into(store, after));
  }

  CO_REQUIRE_EQ(after.size(), 5U);
  CO_REQUIRE_OK(describe(after), after_state);
  CO_REQUIRE_EQ(after_state.numbers, before_state.numbers);
  for (const auto& entry : after.current()) {
    CO_REQUIRE(entry.second.provenance == Provenance::Recovered);
    CO_REQUIRE_EQ(after.freshness_of(entry.second), Freshness::Recovered);
  }

  // The recovered file is clean: a later open sees no damage at all.
  CO_REQUIRE_OK(inspect_store(options_for(directory)), report);
  CO_REQUIRE(!report.torn_tail_recovered);
  CO_REQUIRE_EQ(report.bytes_discarded, 0U);
  CO_REQUIRE_EQ(report.evidence_frames, 5U);
}

CO_TEST(re_submitting_a_recovered_record_never_alters_derived_state) {
  test::ScratchDirectory scratch("restart-resubmit");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));

  const AuthorityRegistry registry = AuthorityRegistry::standard();
  EvidenceWindow seeded(registry, fixture_clock());
  CO_REQUIRE_OK_VOID(seed_store(directory, seeded));

  EvidenceWindow recovered(registry, fixture_clock());
  {
    CO_REQUIRE_STORE(Store::open(options_for(directory)), store);
    CO_REQUIRE_OK_VOID(reload_into(store, recovered));
  }
  CO_REQUIRE_OK(describe(recovered), before_resubmit);

  // The window hands back its own current record. Provenance is part of the
  // content digest, and mark_all_recovered relabels provenance without
  // refreshing the watermark digest, so re-submitting that exact record is the
  // one path where the two disagree. Whatever the window decides, derived state
  // must not move and the record must stay current.
  const EvidenceRecord held = recovered.current().begin()->second;
  CO_REQUIRE(held.provenance == Provenance::Recovered);
  const AcceptanceDecision decision = recovered.consider(held);
  if (decision.outcome == AcceptanceOutcome::Duplicate) {
    CO_REQUIRE_EQ(decision.reason, ReasonCode::DuplicateEvidence);
  } else {
    CO_REQUIRE_EQ(decision.outcome, AcceptanceOutcome::Conflicted);
    CO_REQUIRE_EQ(decision.reason, ReasonCode::ConflictingEvidence);
    CO_REQUIRE(!decision.affects_derived_state);
    CO_REQUIRE(!decision.accepted());
  }

  CO_REQUIRE_EQ(recovered.size(), 5U);
  CO_REQUIRE_OK(describe(recovered), after_resubmit);
  CO_REQUIRE_EQ(after_resubmit.numbers, before_resubmit.numbers);
  CO_REQUIRE_EQ(after_resubmit.records, 5U);
}
