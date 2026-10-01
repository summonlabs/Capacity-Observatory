// Capacity Observatory - command line interface.
//
// The CLI is a deterministic shell over the public library. It parses documents
// into the library's own value types, drives the library's ingest, ledger,
// snapshot, fragmentation, and store APIs, and renders the results. It performs
// no capacity accounting of its own: every number it prints was produced by the
// library from inputs it was given.
//
// Layout: parse -> execute -> render. Nothing here reads a locale, an
// environment variable, or a pointer value; the only clock is the injected one
// (--now, or co::system_clock when --now is absent) and every repeated
// collection is emitted in a deterministic order.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/ingest.hpp"
#include "capacity_observatory/io.hpp"
#include "capacity_observatory/json.hpp"
#include "capacity_observatory/ledger.hpp"
#include "capacity_observatory/platform.hpp"
#include "capacity_observatory/snapshot.hpp"
#include "capacity_observatory/store.hpp"
#include "capacity_observatory/topology.hpp"
#include "capacity_observatory/watermark.hpp"

namespace co::cli {
namespace {

// ---------------------------------------------------------------------------
// Exit codes
// ---------------------------------------------------------------------------
constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitReported = 2;
constexpr int kExitIntegrity = 3;

constexpr std::uint64_t kMaxDocumentBytes = 64ULL << 20U;
constexpr std::size_t kDefaultRefusalLimit = 32;

const char kUsageText[] = R"USAGE(usage: capacity-observatory <command> [options]

commands:
  version        print the tool version
  help           print this usage block
  ingest         ingest evidence into a store, durably before it is applied
  explain        print the ledger over a scope from durable or file evidence
  closure        print only the closure identities of the ledger
  fragmentation  analyse a topology against a rack profile
  verify         inspect a store read only and report its recovery state
  snapshot       build a snapshot and publish it atomically through a store
  recover        open a store writable, recover a torn tail, report durable records

options:
  --store DIR       store directory (evidence.log/snapshot.json/epoch/writer.lock)
  --evidence FILE   evidence document (JSONL, or a JSON object with a "records" array)
  --topology FILE   topology document
  --profile FILE    rack profile document
  --scope PATH      e.g. site=dc1/hall=h1 (default: the shallowest scope present)
  --dimension NAME  repeatable; space|rack-units|power|cooling|weight|serviceability
  --now NANOS       evaluation instant as unix nanoseconds (default: the system clock)
  --json            canonical JSON on stdout instead of the text rendering
  --out FILE        snapshot: also write the JSON document to FILE
  --limit N         refusal audit trail length (default 32)

exit codes:
  0 success
  1 usage error, or an input document that could not be read as its declared shape
  2 a domain refusal or indeterminate result that was reported
  3 integrity, store, or I/O failure

ingest reports each record as "record N: mutation=... disposition=... acceptance=...
reason=... explanation=..." and exits 2 when a record was refused or was durable but
not applied. explain and closure exit 2 unless every ledger line is complete with
zero residuals and no overcommitment. verify exits 3 when the log is rejected.
)USAGE";

// Binds the value of a Result to a name, or returns the failure status. Used by
// the document readers, which are the only place where a long chain of small
// fallible steps exists.
#define CO_BIND(name, expression)               \
  const auto co_##name##_result = (expression); \
  if (!co_##name##_result.ok()) {               \
    return co_##name##_result.status();         \
  }                                             \
  const auto& name = co_##name##_result.value()

// ---------------------------------------------------------------------------
// Outcome: stdout text, stderr text, and an exit code.
// ---------------------------------------------------------------------------
struct Outcome {
  int code{kExitOk};
  std::string output;
  std::string diagnostic;
};

bool is_integrity_code(ReasonCode code) {
  switch (code) {
    case ReasonCode::BadMagic:
    case ReasonCode::UnsupportedVersion:
    case ReasonCode::VersionMismatch:
    case ReasonCode::InteriorCorruption:
    case ReasonCode::IntegrityMismatch:
    case ReasonCode::ShortWrite:
    case ReasonCode::FlushFailure:
    case ReasonCode::PublishFailure:
    case ReasonCode::IoFailure:
    case ReasonCode::WorkerFailed:
    case ReasonCode::InvariantViolation:
    case ReasonCode::InternalError:
    case ReasonCode::Overflow:
    case ReasonCode::Underflow:
    case ReasonCode::CapacityExceeded:
    case ReasonCode::RecordTooLarge:
      return true;
    default:
      return false;
  }
}

int status_exit_code(const Status& status) {
  if (status.ok()) {
    return kExitOk;
  }
  if (is_integrity_code(status.code())) {
    return kExitIntegrity;
  }
  return reason_class(status.code()) == ReasonClass::Failed ? kExitIntegrity : kExitReported;
}

// A document that cannot be read, parsed, or recognised by its declared shape is
// a usage/parse error; a file that cannot be read at all is an I/O failure.
int document_exit_code(const Status& status) {
  if (status.ok()) {
    return kExitOk;
  }
  return is_integrity_code(status.code()) ? kExitIntegrity : kExitUsage;
}

Outcome usage_failure(std::string message) {
  Outcome outcome;
  outcome.code = kExitUsage;
  outcome.diagnostic = "capacity-observatory: " + std::move(message) + "\n\n" + kUsageText;
  return outcome;
}

Outcome failure_outcome(const Status& status, std::string_view context) {
  Outcome outcome;
  outcome.code = status_exit_code(status);
  outcome.diagnostic = "capacity-observatory: " + std::string(context) + ": " + status.render() + "\n";
  return outcome;
}

Outcome document_failure(const Status& status, std::string_view context) {
  Outcome outcome;
  outcome.code = document_exit_code(status);
  outcome.diagnostic = "capacity-observatory: " + std::string(context) + ": " + status.render() + "\n";
  return outcome;
}

std::string bool_text(bool value) { return value ? std::string("true") : std::string("false"); }

std::string count_text(std::uint64_t value) { return std::to_string(value); }

std::string amount_text(const std::optional<Amount>& amount) {
  return amount.has_value() ? std::to_string(amount.value().canonical()) : std::string("unknown");
}

std::string version_text() {
  return std::to_string(CO_VERSION_MAJOR) + "." + std::to_string(CO_VERSION_MINOR) + "." +
         std::to_string(CO_VERSION_PATCH);
}

// ---------------------------------------------------------------------------
// Option parsing
// ---------------------------------------------------------------------------
struct Options {
  std::string command;
  std::string store;
  std::string evidence;
  std::string topology;
  std::string profile;
  std::string scope;
  std::vector<Dimension> dimensions;
  std::optional<std::int64_t> now;
  bool json{false};
  std::string out;
  std::size_t limit{kDefaultRefusalLimit};
};

bool is_known_command(std::string_view command) {
  constexpr std::array<std::string_view, 9> kCommands{"version",       "help",   "ingest", "explain", "closure",
                                                      "fragmentation", "verify", "snapshot", "recover"};
  for (const std::string_view candidate : kCommands) {
    if (candidate == command) {
      return true;
    }
  }
  return false;
}

Result<std::int64_t> parse_int64(std::string_view text) {
  if (text.empty()) {
    return make_error(ReasonCode::InvalidOptionValue, "a base ten integer was expected but the value is empty");
  }
  std::int64_t value = 0;
  const char* const begin = text.data();
  const char* const end = text.data() + text.size();
  const std::from_chars_result parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return make_error(ReasonCode::InvalidOptionValue, "'" + std::string(text) + "' is not a base ten integer");
  }
  return Result<std::int64_t>(value);
}

Result<Options> parse_options(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }
  if (arguments.empty()) {
    return make_error(ReasonCode::MissingOption, "no command was given");
  }

  Options options;
  options.command = arguments.front();
  if (!is_known_command(options.command)) {
    return make_error(ReasonCode::UnknownCommand, "unknown command '" + options.command + "'");
  }

  std::size_t index = 1;
  while (index < arguments.size()) {
    const std::string flag = arguments[index];
    if (flag == "--json") {
      options.json = true;
      ++index;
      continue;
    }
    if (index + 1 >= arguments.size()) {
      return make_error(ReasonCode::MissingOption, "option " + flag + " requires a value");
    }
    const std::string value = arguments[index + 1];
    index += 2;

    if (flag == "--store") {
      options.store = value;
    } else if (flag == "--evidence") {
      options.evidence = value;
    } else if (flag == "--topology") {
      options.topology = value;
    } else if (flag == "--profile") {
      options.profile = value;
    } else if (flag == "--scope") {
      options.scope = value;
    } else if (flag == "--out") {
      options.out = value;
    } else if (flag == "--dimension") {
      const Result<Dimension> dimension = dimension_from_text(value);
      if (!dimension.ok()) {
        return make_error(ReasonCode::InvalidOptionValue, dimension.status().render());
      }
      options.dimensions.push_back(dimension.value());
    } else if (flag == "--now") {
      const Result<std::int64_t> nanos = parse_int64(value);
      if (!nanos.ok()) {
        return nanos.status().with_context("--now");
      }
      options.now = nanos.value();
    } else if (flag == "--limit") {
      const Result<std::int64_t> limit = parse_int64(value);
      if (!limit.ok()) {
        return limit.status().with_context("--limit");
      }
      if (limit.value() < 0) {
        return make_error(ReasonCode::InvalidOptionValue, "--limit must not be negative");
      }
      options.limit = static_cast<std::size_t>(limit.value());
    } else {
      return make_error(ReasonCode::UnknownCommand, "unknown option '" + flag + "'");
    }
  }
  return Result<Options>(std::move(options));
}

// ---------------------------------------------------------------------------
// Document helpers
// ---------------------------------------------------------------------------
Status reject_unknown_members(const json::Value& node, std::initializer_list<std::string_view> allowed,
                              std::string_view where) {
  const json::Object* members = node.as_object();
  if (members == nullptr) {
    return make_error(ReasonCode::TypeMismatch, std::string(where) + " must be a JSON object");
  }
  for (const json::Object::value_type& member : *members) {
    bool known = false;
    for (const std::string_view candidate : allowed) {
      if (candidate == member.first) {
        known = true;
        break;
      }
    }
    if (!known) {
      return make_error(ReasonCode::UnknownField, "unknown member \"" + member.first + "\" in " + std::string(where));
    }
  }
  return Status::success();
}

Result<std::optional<std::int64_t>> optional_int(const json::Value& node, std::string_view key) {
  const json::Value* member = node.find(key);
  if (member == nullptr || member->is_null()) {
    return Result<std::optional<std::int64_t>>(std::optional<std::int64_t>{});
  }
  const Result<std::int64_t> value = member->as_int();
  if (!value.ok()) {
    return value.status().with_context(std::string(key));
  }
  return Result<std::optional<std::int64_t>>(std::optional<std::int64_t>(value.value()));
}

Result<std::optional<Timestamp>> optional_timestamp(const json::Value& node, std::string_view key) {
  const Result<std::optional<std::int64_t>> nanos = optional_int(node, key);
  if (!nanos.ok()) {
    return nanos.status();
  }
  if (!nanos.value().has_value()) {
    return Result<std::optional<Timestamp>>(std::optional<Timestamp>{});
  }
  return Result<std::optional<Timestamp>>(std::optional<Timestamp>(Timestamp(nanos.value().value())));
}

Result<json::Value> load_json_document(const std::string& path) {
  const Result<std::vector<std::uint8_t>> bytes = io::read_file(path, kMaxDocumentBytes);
  if (!bytes.ok()) {
    return bytes.status().with_context("reading " + path);
  }
  const std::string text(bytes.value().begin(), bytes.value().end());
  const Result<json::Value> parsed = json::Value::parse(text);
  if (!parsed.ok()) {
    return parsed.status().with_context(path);
  }
  return parsed;
}

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (;;) {
    const std::size_t newline = text.find('\n', start);
    std::string_view line =
        newline == std::string_view::npos ? text.substr(start) : text.substr(start, newline - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    lines.emplace_back(line);
    if (newline == std::string_view::npos) {
      break;
    }
    start = newline + 1;
  }
  return lines;
}

bool is_blank(std::string_view text) {
  for (const char ch : text) {
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
      return false;
    }
  }
  return true;
}

Result<AuthorityRole> role_from_text(std::string_view text) {
  constexpr std::array<std::pair<std::string_view, AuthorityRole>, 7> kRoles{{
      {"committed-capacity", AuthorityRole::CommittedCapacity},
      {"reservations", AuthorityRole::Reservations},
      {"installed-inventory", AuthorityRole::InstalledInventory},
      {"policy", AuthorityRole::Policy},
      {"plant-telemetry", AuthorityRole::PlantTelemetry},
      {"economics", AuthorityRole::Economics},
      {"arbitration", AuthorityRole::Arbitration},
  }};
  for (const auto& entry : kRoles) {
    if (entry.first == text) {
      return Result<AuthorityRole>(entry.second);
    }
  }
  return make_error(ReasonCode::InvalidArgument, "unknown authority role '" + std::string(text) + "'");
}

// ---------------------------------------------------------------------------
// Evidence records
// ---------------------------------------------------------------------------
struct RecordEntry {
  std::string mutation;  // as declared, empty when the record could not be parsed
  std::string origin;    // document position, e.g. "evidence.jsonl:3"
  std::optional<EvidenceRecord> record;
  Status problem;  // set only when the record could not be built
};

Result<EvidenceRecord> build_evidence_record(const json::Value& node, const AuthorityRegistry& registry,
                                             const std::string& origin) {
  const Status schema =
      reject_unknown_members(node,
                             {"mutation", "authority", "role", "scope", "dimension", "assertion", "unit", "amount",
                              "generation", "epoch", "revision", "observed_at", "validity_from", "validity_until",
                              "source"},
                             "evidence record " + origin);
  if (!schema.ok()) {
    return schema;
  }

  CO_BIND(mutation_text, node.require_string("mutation"));
  CO_BIND(mutation, MutationId::parse(mutation_text));
  (void)mutation;
  CO_BIND(authority_text, node.require_string("authority"));
  CO_BIND(authority, AuthorityId::parse(authority_text));
  CO_BIND(scope_text, node.require_string("scope"));
  CO_BIND(scope, ScopePath::parse(scope_text));
  CO_BIND(dimension_name, node.require_string("dimension"));
  CO_BIND(dimension, dimension_from_text(dimension_name));
  CO_BIND(assertion_name, node.require_string("assertion"));
  CO_BIND(assertion, assertion_from_text(assertion_name));
  CO_BIND(unit_name, node.require_string("unit"));
  // A unit of another dimension is refused with DimensionMismatch and a magnitude
  // that cannot be converted exactly is refused with InexactScale: never rounded.
  CO_BIND(unit, Unit::parse_for(dimension, unit_name));
  CO_BIND(amount, node.require_int("amount"));
  CO_BIND(generation, node.require_int("generation"));
  CO_BIND(epoch, node.require_int("epoch"));
  CO_BIND(revision, node.require_int("revision"));
  if (generation < 0 || epoch < 0 || revision < 0) {
    return make_error(ReasonCode::NegativeAmount,
                      "generation, epoch, and revision must not be negative in " + origin);
  }
  CO_BIND(observed_at, optional_timestamp(node, "observed_at"));
  CO_BIND(validity_from, optional_timestamp(node, "validity_from"));
  CO_BIND(validity_until, optional_timestamp(node, "validity_until"));
  CO_BIND(source, node.optional_string("source"));

  // "role" is optional: when absent the authority registry supplies it.
  CO_BIND(role_name, node.optional_string("role"));
  AuthorityRole role = AuthorityRole::Economics;
  if (role_name.has_value()) {
    CO_BIND(declared_role, role_from_text(role_name.value()));
    role = declared_role;
  } else if (const AuthorityContract* contract = registry.find(authority); contract != nullptr) {
    role = contract->role;
  }

  Result<EvidenceRecord> built =
      EvidenceRecord::make(authority, role, scope, dimension, assertion, unit, amount,
                           Generation(static_cast<std::uint64_t>(generation)),
                           Epoch(static_cast<std::uint64_t>(epoch)), Revision(static_cast<std::uint64_t>(revision)));
  if (!built.ok()) {
    return built.status();
  }
  EvidenceRecord record = std::move(built).value();
  record.source = source.has_value() ? source.value() : origin;
  // An absent observation instant stays absent: the record is indeterminate for
  // freshness and is never assumed to be fresh.
  if (observed_at.has_value()) {
    record.has_observed_at = true;
    record.observed_at = observed_at.value();
  }
  record.validity.from = validity_from;
  record.validity.until = validity_until;
  return Result<EvidenceRecord>(std::move(record));
}

RecordEntry parse_evidence_record(const json::Value& node, const AuthorityRegistry& registry,
                                  const std::string& origin) {
  RecordEntry entry;
  entry.origin = origin;
  if (const json::Value* mutation = node.find("mutation"); mutation != nullptr && mutation->is_string()) {
    entry.mutation = mutation->as_string().value();
  }
  Result<EvidenceRecord> record = build_evidence_record(node, registry, origin);
  if (!record.ok()) {
    entry.problem = record.status();
    return entry;
  }
  entry.record = std::move(record).value();
  return entry;
}

Result<std::vector<RecordEntry>> load_evidence_document(const std::string& path, const AuthorityRegistry& registry) {
  const Result<std::vector<std::uint8_t>> bytes = io::read_file(path, kMaxDocumentBytes);
  if (!bytes.ok()) {
    return bytes.status().with_context("reading " + path);
  }
  const std::string text(bytes.value().begin(), bytes.value().end());
  if (is_blank(text)) {
    return make_error(ReasonCode::EmptyInput, "evidence document " + path + " is empty");
  }

  std::vector<RecordEntry> entries;
  const Result<json::Value> whole = json::Value::parse(text);
  if (whole.ok() && !whole.value().is_object()) {
    return make_error(ReasonCode::TypeMismatch,
                      "evidence document " + path + " must be a JSON object with a \"records\" array, or JSONL");
  }
  // The wrapper form is the JSON object that carries a "records" array. Every
  // other document is JSONL, which also covers a one line document that holds a
  // single record.
  if (whole.ok() && whole.value().find("records") != nullptr) {
    const Status schema = reject_unknown_members(whole.value(), {"records"}, "evidence document " + path);
    if (!schema.ok()) {
      return schema;
    }
    const Result<const json::Array*> records = whole.value().require_array("records");
    if (!records.ok()) {
      return records.status().with_context(path);
    }
    if (records.value()->empty()) {
      return make_error(ReasonCode::EmptyInput, "evidence document " + path + " carries no records");
    }
    for (std::size_t i = 0; i < records.value()->size(); ++i) {
      entries.push_back(parse_evidence_record((*records.value())[i], registry, path + ":" + std::to_string(i + 1)));
    }
    return Result<std::vector<RecordEntry>>(std::move(entries));
  }

  // JSONL: one record per non-blank line.
  const std::vector<std::string> lines = split_lines(text);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (is_blank(lines[i])) {
      continue;
    }
    const std::string origin = path + ":" + std::to_string(i + 1);
    const Result<json::Value> parsed = json::Value::parse(lines[i]);
    if (!parsed.ok()) {
      RecordEntry entry;
      entry.origin = origin;
      entry.problem = parsed.status().with_context("JSONL line");
      entries.push_back(std::move(entry));
      continue;
    }
    entries.push_back(parse_evidence_record(parsed.value(), registry, origin));
  }
  if (entries.empty()) {
    return make_error(ReasonCode::EmptyInput, "evidence document " + path + " carries no records");
  }
  return Result<std::vector<RecordEntry>>(std::move(entries));
}

// ---------------------------------------------------------------------------
// Topology and profile documents
// ---------------------------------------------------------------------------
Result<Topology> load_topology_document(const std::string& path) {
  CO_BIND(document, load_json_document(path));
  const Status schema = reject_unknown_members(document, {"enclosures"}, "topology document " + path);
  if (!schema.ok()) {
    return schema;
  }
  CO_BIND(enclosures, document.require_array("enclosures"));

  std::vector<EnclosureBudget> budgets;
  for (std::size_t i = 0; i < enclosures->size(); ++i) {
    const json::Value& node = (*enclosures)[i];
    const std::string where = "enclosure " + std::to_string(i + 1) + " of " + path;
    const Status enclosure_schema =
        reject_unknown_members(node,
                               {"id", "scope", "free_ru_contiguous", "free_ru_total", "structurally_usable",
                                "unusable_reason", "free"},
                               where);
    if (!enclosure_schema.ok()) {
      return enclosure_schema;
    }
    CO_BIND(id_text, node.require_string("id"));
    CO_BIND(id, EnclosureId::parse(id_text));
    CO_BIND(scope_text, node.require_string("scope"));
    CO_BIND(scope, ScopePath::parse(scope_text));
    CO_BIND(contiguous, optional_int(node, "free_ru_contiguous"));
    CO_BIND(total, optional_int(node, "free_ru_total"));
    CO_BIND(reason, node.optional_string("unusable_reason"));

    EnclosureBudget budget;
    budget.id = id;
    budget.scope = scope;
    budget.free_ru_contiguous = contiguous;
    budget.free_ru_total = total;
    budget.unusable_reason = reason.value_or(std::string());
    if (const json::Value* usable = node.find("structurally_usable"); usable != nullptr && !usable->is_null()) {
      const Result<bool> flag = usable->as_bool();
      if (!flag.ok()) {
        return flag.status().with_context("structurally_usable in " + where);
      }
      budget.structurally_usable = flag.value();
    }

    // "free" values are canonical units; an absent dimension stays UNKNOWN.
    if (const json::Value* free = node.find("free"); free != nullptr && !free->is_null()) {
      const Status free_schema = reject_unknown_members(
          *free, {"space", "rack-units", "power", "cooling", "weight", "serviceability"}, "free budget of " + where);
      if (!free_schema.ok()) {
        return free_schema;
      }
      for (const Dimension dimension : kAllDimensions) {
        const json::Value* value = free->find(dimension_text(dimension));
        if (value == nullptr || value->is_null()) {
          continue;
        }
        const Result<std::int64_t> amount = value->as_int();
        if (!amount.ok()) {
          return amount.status().with_context(std::string(dimension_text(dimension)) + " in " + where);
        }
        budget.free.set(dimension, Amount(amount.value()));
      }
    }
    budgets.push_back(std::move(budget));
  }
  return Topology::make(std::move(budgets));
}

Result<RackProfile> load_profile_document(const std::string& path) {
  CO_BIND(document, load_json_document(path));
  const Status schema = reject_unknown_members(document, {"name", "rack_units", "per_rack"}, "profile document " + path);
  if (!schema.ok()) {
    return schema;
  }
  CO_BIND(name, document.require_string("name"));
  CO_BIND(rack_units, document.require_int("rack_units"));
  CO_BIND(per_rack_node, document.require("per_rack"));

  const Status demand_schema =
      reject_unknown_members(*per_rack_node,
                             {"space", "rack-units", "power", "cooling", "weight", "serviceability"},
                             "per_rack of " + path);
  if (!demand_schema.ok()) {
    return demand_schema;
  }
  AmountVector per_rack;
  for (const Dimension dimension : kAllDimensions) {
    const json::Value* value = per_rack_node->find(dimension_text(dimension));
    if (value == nullptr || value->is_null()) {
      continue;
    }
    const Result<std::int64_t> amount = value->as_int();
    if (!amount.ok()) {
      return amount.status().with_context(std::string(dimension_text(dimension)) + " per_rack in " + path);
    }
    per_rack.set(dimension, Amount(amount.value()));
  }
  return RackProfile::make(name, rack_units, per_rack);
}

// ---------------------------------------------------------------------------
// Session: registry, clock, and evidence window wired to one evaluation instant
// ---------------------------------------------------------------------------
class Session {
 public:
  explicit Session(const std::optional<std::int64_t>& now)
      : registry_(AuthorityRegistry::standard()),
        clock_(now.has_value() ? manual_clock(Timestamp(now.value())) : system_clock()),
        window_(registry_, clock_) {}

  [[nodiscard]] const AuthorityRegistry& registry() const noexcept { return registry_; }
  [[nodiscard]] EvidenceWindow& window() noexcept { return window_; }

 private:
  AuthorityRegistry registry_;
  std::shared_ptr<Clock> clock_;
  EvidenceWindow window_;
};

Result<Store> open_store(const std::string& directory, bool read_only) {
  StoreOptions options;
  options.directory = directory;
  options.read_only = read_only;
  return Store::open(options);
}

// Rebuilds window state from durable frames. Recovery is not observation: every
// record is marked recovered, so no recovered record is ever promoted to fresh.
Status load_durable_evidence(const Store& store, EvidenceWindow& window) {
  const Result<std::vector<StoredEvidence>> stored = store.load_evidence();
  if (!stored.ok()) {
    return stored.status();
  }
  for (const StoredEvidence& evidence : stored.value()) {
    (void)window.consider(evidence.record);
  }
  window.mark_all_recovered(window.clock()->now());
  return Status::success();
}

// Opens the store and rebuilds the window from its durable frames. Read only
// opens never take the writer lock and never advance the session epoch.
Result<Store> open_store_with_evidence(const std::string& directory, bool read_only, EvidenceWindow& window) {
  Result<Store> opened = open_store(directory, read_only);
  if (!opened.ok()) {
    return opened.status();
  }
  const Status loaded = load_durable_evidence(opened.value(), window);
  if (!loaded.ok()) {
    return loaded;
  }
  return opened;
}

// Considers file evidence into the window. A record that cannot be built is
// reported in 'diagnostic' and never reaches derived state.
Result<std::size_t> consider_file_evidence(const std::string& path, EvidenceWindow& window,
                                           std::string& diagnostic) {
  const Result<std::vector<RecordEntry>> entries = load_evidence_document(path, window.registry());
  if (!entries.ok()) {
    return entries.status();
  }
  std::size_t refused = 0;
  for (const RecordEntry& entry : entries.value()) {
    if (!entry.record.has_value()) {
      ++refused;
      diagnostic += "capacity-observatory: refused " + entry.origin + ": " +
                    std::string(reason_text(entry.problem.code())) + ": " + entry.problem.detail() + "\n";
      continue;
    }
    (void)window.consider(entry.record.value());
  }
  return Result<std::size_t>(refused);
}

std::vector<ScopePath> present_scopes(const EvidenceWindow& window, const Topology* topology) {
  std::set<std::string> seen;
  std::vector<ScopePath> scopes;
  const auto add = [&seen, &scopes](const ScopePath& scope) {
    if (!scope.empty() && seen.insert(scope.text()).second) {
      scopes.push_back(scope);
    }
  };
  for (const auto& entry : window.current()) {
    add(entry.second.scope);
  }
  if (topology != nullptr) {
    for (const EnclosureBudget& enclosure : topology->enclosures()) {
      ScopePath scope = enclosure.scope;
      while (!scope.empty()) {
        add(scope);
        scope = scope.parent();
      }
    }
  }
  std::sort(scopes.begin(), scopes.end(), [](const ScopePath& left, const ScopePath& right) {
    return left.depth() != right.depth() ? left.depth() < right.depth() : left < right;
  });
  return scopes;
}

Result<ScopePath> resolve_scope(const Options& options, const EvidenceWindow& window, const Topology* topology) {
  if (!options.scope.empty()) {
    return ScopePath::parse(options.scope);
  }
  const std::vector<ScopePath> scopes = present_scopes(window, topology);
  if (scopes.empty()) {
    return make_error(ReasonCode::MissingOption,
                      "no scope is present in the inputs; pass --scope to name one explicitly");
  }
  return Result<ScopePath>(scopes.front());
}

DimensionSet requested_dimensions(const Options& options) {
  if (options.dimensions.empty()) {
    return DimensionSet::all();
  }
  DimensionSet set;
  for (const Dimension dimension : options.dimensions) {
    set.add(dimension);
  }
  return set;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
int ledger_exit_code(const Ledger& ledger) {
  for (const LedgerLine& line : ledger.lines) {
    if (line.state != LineState::Complete || line.overcommitted) {
      return kExitReported;
    }
  }
  return kExitOk;
}

std::string render_closure_text(const Snapshot& snapshot) {
  std::string out = "[closure] scope=" + snapshot.scope.text() + "\n";
  out += "  has_residuals: " + bool_text(snapshot.ledger.has_residuals()) +
         " has_overcommitment: " + bool_text(snapshot.ledger.has_overcommitment()) +
         " incomplete_lines: " + std::to_string(snapshot.ledger.incomplete_lines()) + "\n";
  for (const LedgerLine& line : snapshot.ledger.lines) {
    out += "[" + std::string(dimension_text(line.dimension)) + "] state=" + std::string(line_state_text(line.state)) +
           " freshness=" + std::string(freshness_text(line.worst_freshness)) +
           " evidence=" + std::to_string(line.evidence_count) +
           " unknown_classes=" + std::to_string(line.unknown_classes) + (line.overcommitted ? " OVERCOMMITTED" : "") +
           "\n";
    for (const ClosureCheck& check : line.closures) {
      out += "  " + check.name + ": residual=" +
             (check.indeterminate ? std::string("indeterminate") : amount_text(check.residual)) +
             " holds=" + bool_text(check.holds) + " reason=" + std::string(reason_text(check.code)) + "\n";
      out += "    " + check.explanation + "\n";
    }
    out += "  unexplained: governance=" + amount_text(line.unexplained_governance) +
           " installed=" + amount_text(line.unexplained_installed) +
           " available=" + amount_text(line.unexplained_available) +
           " overcommitment=" + amount_text(line.overcommitment) + "\n";
  }
  return out;
}

std::string render_recovery_report(std::string_view label, const std::string& directory, const RecoveryReport& report) {
  std::string out = "[" + std::string(label) + "] " + directory + "\n";
  out += "  frames: " + count_text(report.frames_read) + "\n";
  out += "  evidence frames: " + count_text(report.evidence_frames) + "\n";
  out += "  snapshot frames: " + count_text(report.snapshot_frames) + "\n";
  out += "  epoch frames: " + count_text(report.epoch_frames) + "\n";
  out += "  discarded bytes: " + count_text(report.bytes_discarded) + "\n";
  out += "  torn tail: " + bool_text(report.torn_tail_recovered) + "\n";
  out += "  session epoch: e" + std::to_string(report.session_epoch.value()) + "\n";
  out += "  explanation: " + report.explanation + "\n";
  return out;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
Outcome cmd_version(const Options& options) {
  Outcome outcome;
  if (options.json) {
    json::Object root;
    root.emplace_back("name", json::Value(std::string("capacity-observatory")));
    root.emplace_back("version", json::Value(version_text()));
    root.emplace_back("boundary",
                      json::Value(std::string("reads and explains capacity state and deltas; owns no canonical "
                                              "capacity, reservation, placement, admission, entitlement, or "
                                              "reconciliation mutation")));
    outcome.output = json::dump(json::Value::object(std::move(root)), true, -1) + "\n";
    return outcome;
  }
  outcome.output = "capacity-observatory " + version_text() + "\n";
  return outcome;
}

Outcome cmd_help() {
  Outcome outcome;
  outcome.output = kUsageText;
  return outcome;
}

Outcome cmd_ingest(const Options& options) {
  if (options.store.empty()) {
    return usage_failure("ingest requires --store");
  }
  if (options.evidence.empty()) {
    return usage_failure("ingest requires --evidence");
  }

  Session session(options.now);
  Result<Store> opened = open_store(options.store, false);
  if (!opened.ok()) {
    return failure_outcome(opened.status(), "opening the store for writing");
  }
  Store store = std::move(opened).value();
  const Result<std::vector<RecordEntry>> entries = load_evidence_document(options.evidence, session.registry());
  if (!entries.ok()) {
    return document_failure(entries.status(), "reading " + options.evidence);
  }

  IngestPipeline pipeline(session.window(), &store);
  std::size_t unparsed = 0;
  std::string body;
  std::vector<std::string> trail;
  int code = kExitOk;

  for (const RecordEntry& entry : entries.value()) {
    const std::size_t position = pipeline.statistics().received + unparsed + 1;
    if (!entry.record.has_value()) {
      ++unparsed;
      const std::string label = entry.mutation.empty() ? entry.origin : entry.mutation;
      body += "record " + std::to_string(position) + ": mutation=" + label +
              " disposition=refused acceptance=none reason=" + std::string(reason_text(entry.problem.code())) +
              " explanation=" + entry.problem.detail() + "\n";
      trail.push_back(entry.origin + " -> refused (" + std::string(reason_text(entry.problem.code())) +
                      "): " + entry.problem.detail());
      code = (std::max)(code, kExitReported);
      continue;
    }

    const EvidenceRecord& record = entry.record.value();
    const Result<MutationId> mutation = MutationId::parse(entry.mutation);
    if (!mutation.ok()) {
      ++unparsed;
      body += "record " + std::to_string(position) + ": mutation=" + entry.mutation +
              " disposition=refused acceptance=none reason=" +
              std::string(reason_text(mutation.status().code())) + " explanation=" + mutation.status().detail() + "\n";
      trail.push_back(entry.origin + " -> refused (" + std::string(reason_text(mutation.status().code())) +
                      "): " + mutation.status().detail());
      code = (std::max)(code, kExitReported);
      continue;
    }

    const Result<IngestResult> ingested = pipeline.ingest(record, mutation.value());
    if (!ingested.ok()) {
      return failure_outcome(ingested.status(), "ingesting " + record.id_text());
    }
    const IngestResult& result = ingested.value();
    // A store layer refusal never reaches the acceptance window, so it reports
    // "acceptance=none" rather than a fabricated window outcome.
    const bool decided = !result.acceptance.explanation.empty();
    body += "record " + std::to_string(position) + ": mutation=" + entry.mutation +
            " disposition=" + std::string(ingest_disposition_text(result.disposition)) + " acceptance=" +
            (decided ? std::string(acceptance_outcome_text(result.acceptance.outcome)) : std::string("none")) +
            " reason=" + std::string(reason_text(result.reason)) + " explanation=" + result.explanation + "\n";

    if (result.disposition == IngestDisposition::Refused || result.disposition == IngestDisposition::Durable) {
      trail.push_back(record.id_text() + " -> " + std::string(ingest_disposition_text(result.disposition)) + " (" +
                      std::string(reason_text(result.reason)) + "): " + result.explanation);
      code = (std::max)(code, decided ? kExitReported
                                      : status_exit_code(Status::failure(result.reason, result.explanation)));
    }
  }

  const IngestStatistics statistics = pipeline.statistics();
  Outcome outcome;
  outcome.code = code;
  outcome.output = body;
  outcome.output += "[ingest summary]\n";
  outcome.output += "  received: " + std::to_string(statistics.received + unparsed) + "\n";
  outcome.output += "  applied: " + std::to_string(statistics.applied) + "\n";
  outcome.output += "  duplicates: " + std::to_string(statistics.duplicates) + "\n";
  outcome.output += "  refused: " + std::to_string(statistics.refused + unparsed) + "\n";
  outcome.output += "  durable-only: " + std::to_string(statistics.durable_only) + "\n";
  if (unparsed != 0) {
    outcome.output += "  unparsed: " + std::to_string(unparsed) + "\n";
  }
  if (!trail.empty() && options.limit != 0) {
    const std::size_t start = trail.size() > options.limit ? trail.size() - options.limit : 0;
    outcome.output += "[refusals]\n";
    for (std::size_t i = start; i < trail.size(); ++i) {
      outcome.output += "  " + trail[i] + "\n";
    }
  }
  return outcome;
}

Outcome explain_like(const Options& options, bool closure_only) {
  if (options.store.empty() && options.evidence.empty()) {
    return usage_failure(options.command + " requires --store or --evidence");
  }

  Session session(options.now);
  Store store;
  const Store* store_view = nullptr;
  std::string diagnostic;
  if (!options.store.empty()) {
    Result<Store> opened = open_store_with_evidence(options.store, true, session.window());
    if (!opened.ok()) {
      return failure_outcome(opened.status(), "opening the store read only");
    }
    store = std::move(opened).value();
    store_view = &store;
  } else {
    const Result<std::size_t> considered = consider_file_evidence(options.evidence, session.window(), diagnostic);
    if (!considered.ok()) {
      return document_failure(considered.status(), "reading " + options.evidence);
    }
  }

  const Result<ScopePath> scope = resolve_scope(options, session.window(), nullptr);
  if (!scope.ok()) {
    return usage_failure(scope.status().render());
  }
  const Result<Snapshot> snapshot =
      build_snapshot(session.window(), scope.value(), requested_dimensions(options), nullptr, nullptr, store_view);
  if (!snapshot.ok()) {
    return failure_outcome(snapshot.status(), "building the snapshot");
  }

  Outcome outcome;
  outcome.diagnostic = diagnostic;
  outcome.output = options.json
                       ? json::dump(snapshot_to_json(snapshot.value()), true, -1) + "\n"
                       : (closure_only ? render_closure_text(snapshot.value()) : render_snapshot_text(snapshot.value()));
  outcome.code = (std::max)(diagnostic.empty() ? kExitOk : kExitReported, ledger_exit_code(snapshot.value().ledger));
  return outcome;
}

Outcome cmd_fragmentation(const Options& options) {
  if (options.topology.empty()) {
    return usage_failure("fragmentation requires --topology");
  }
  if (options.profile.empty()) {
    return usage_failure("fragmentation requires --profile");
  }

  Session session(options.now);
  const Result<Topology> topology = load_topology_document(options.topology);
  if (!topology.ok()) {
    return document_failure(topology.status(), "reading " + options.topology);
  }
  const Result<RackProfile> profile = load_profile_document(options.profile);
  if (!profile.ok()) {
    return document_failure(profile.status(), "reading " + options.profile);
  }

  const bool has_ledger_source = !options.store.empty() || !options.evidence.empty();
  Store store;
  const Store* store_view = nullptr;
  std::string diagnostic;
  if (!options.store.empty()) {
    Result<Store> opened = open_store_with_evidence(options.store, true, session.window());
    if (!opened.ok()) {
      return failure_outcome(opened.status(), "opening the store read only");
    }
    store = std::move(opened).value();
    store_view = &store;
  } else if (!options.evidence.empty()) {
    const Result<std::size_t> considered = consider_file_evidence(options.evidence, session.window(), diagnostic);
    if (!considered.ok()) {
      return document_failure(considered.status(), "reading " + options.evidence);
    }
  }

  const Result<ScopePath> scope = resolve_scope(options, session.window(), &topology.value());
  if (!scope.ok()) {
    return usage_failure(scope.status().render());
  }
  Topology scoped = topology.value();
  if (!options.scope.empty()) {
    const Result<Topology> narrowed = topology.value().narrowed_to(scope.value());
    if (!narrowed.ok()) {
      return failure_outcome(narrowed.status(), "narrowing the topology to " + scope.value().text());
    }
    scoped = narrowed.value();
  }

  // Without a ledger source the ledger is left empty rather than reported as an
  // all-indeterminate roll-up of nothing.
  const DimensionSet dimensions = has_ledger_source ? requested_dimensions(options) : DimensionSet{};
  const Result<Snapshot> snapshot =
      build_snapshot(session.window(), scope.value(), dimensions, &scoped, &profile.value(), store_view);
  if (!snapshot.ok()) {
    return failure_outcome(snapshot.status(), "analysing fragmentation");
  }

  Outcome outcome;
  outcome.diagnostic = diagnostic;
  if (options.json) {
    const json::Value document = snapshot_to_json(snapshot.value());
    const json::Value* fragment = document.find("fragmentation");
    if (fragment == nullptr) {
      return failure_outcome(make_error(ReasonCode::InvariantViolation, "the snapshot carried no fragmentation"),
                             "rendering fragmentation");
    }
    // The fragmentation object is the top level document; the ledger is attached
    // as an extra member when one was requested.
    json::Value result = *fragment;
    if (has_ledger_source) {
      if (const json::Value* ledger = document.find("ledger"); ledger != nullptr) {
        result.set("ledger", *ledger);
      }
    }
    outcome.output = json::dump(result, true, -1) + "\n";
  } else {
    outcome.output = render_snapshot_text(snapshot.value());
  }

  const bool indeterminate = snapshot.value().fragmentation.has_value() &&
                             snapshot.value().fragmentation.value().indeterminate;
  outcome.code = indeterminate ? kExitReported : kExitOk;
  if (has_ledger_source) {
    outcome.code = (std::max)(outcome.code, ledger_exit_code(snapshot.value().ledger));
  }
  outcome.code = (std::max)(outcome.code, diagnostic.empty() ? kExitOk : kExitReported);
  return outcome;
}

std::string recovery_json(std::uint64_t frames, std::uint64_t evidence, std::uint64_t snapshots,
                          std::uint64_t epochs, std::uint64_t discarded, bool torn, Epoch session_epoch,
                          std::int64_t durable_records, const std::string& explanation) {
  json::Object root;
  root.emplace_back("frames", json::Value(static_cast<std::int64_t>(frames)));
  root.emplace_back("evidence_frames", json::Value(static_cast<std::int64_t>(evidence)));
  root.emplace_back("snapshot_frames", json::Value(static_cast<std::int64_t>(snapshots)));
  root.emplace_back("epoch_frames", json::Value(static_cast<std::int64_t>(epochs)));
  root.emplace_back("discarded_bytes", json::Value(static_cast<std::int64_t>(discarded)));
  root.emplace_back("torn_tail_recovered", json::Value(torn));
  root.emplace_back("session_epoch", json::Value(static_cast<std::int64_t>(session_epoch.value())));
  root.emplace_back("durable_records", json::Value(durable_records));
  root.emplace_back("explanation", json::Value(explanation));
  return json::dump(json::Value::object(std::move(root)), true, -1) + "\n";
}

Outcome cmd_verify(const Options& options) {
  if (options.store.empty()) {
    return usage_failure("verify requires --store");
  }
  StoreOptions store_options;
  store_options.directory = options.store;
  store_options.read_only = true;

  const Result<RecoveryReport> report = inspect_store(store_options);
  if (!report.ok()) {
    return failure_outcome(report.status(), "verifying " + options.store);
  }
  Outcome outcome;
  if (options.json) {
    outcome.output =
        recovery_json(report.value().frames_read, report.value().evidence_frames, report.value().snapshot_frames,
                      report.value().epoch_frames, report.value().bytes_discarded, report.value().torn_tail_recovered,
                      report.value().session_epoch, 0, report.value().explanation);
    return outcome;
  }
  outcome.output = render_recovery_report("verify", options.store, report.value());
  return outcome;
}

Outcome cmd_recover(const Options& options) {
  if (options.store.empty()) {
    return usage_failure("recover requires --store");
  }
  Result<Store> opened = open_store(options.store, false);
  if (!opened.ok()) {
    return failure_outcome(opened.status(), "opening the store for recovery");
  }
  Store store = std::move(opened).value();
  const Result<std::vector<StoredEvidence>> durable = store.load_evidence();
  if (!durable.ok()) {
    return failure_outcome(durable.status(), "reading durable evidence");
  }
  const std::int64_t records = static_cast<std::int64_t>(durable.value().size());
  Outcome outcome;
  if (options.json) {
    outcome.output = recovery_json(store.recovery().frames_read, store.recovery().evidence_frames,
                                   store.recovery().snapshot_frames, store.recovery().epoch_frames,
                                   store.recovery().bytes_discarded, store.recovery().torn_tail_recovered,
                                   store.session_epoch(), records, store.recovery().explanation);
    return outcome;
  }
  outcome.output = render_recovery_report("recover", options.store, store.recovery());
  outcome.output += "  durable evidence records: " + std::to_string(records) + "\n";
  return outcome;
}

Outcome cmd_snapshot(const Options& options) {
  if (options.store.empty()) {
    return usage_failure("snapshot requires --store");
  }
  if (!options.evidence.empty()) {
    return usage_failure("snapshot publishes derived state from durable evidence; --evidence is refused because a "
                         "snapshot must never be derived from evidence that is not in the store");
  }
  if (options.topology.empty() != options.profile.empty()) {
    return usage_failure("--topology and --profile must be given together");
  }

  Session session(options.now);
  Result<Store> opened = open_store(options.store, false);
  if (!opened.ok()) {
    return failure_outcome(opened.status(), "opening the store for writing");
  }
  Store store = std::move(opened).value();
  const Status loaded = load_durable_evidence(store, session.window());
  if (!loaded.ok()) {
    return failure_outcome(loaded, "loading durable evidence");
  }
  const Result<ScopePath> scope = resolve_scope(options, session.window(), nullptr);
  if (!scope.ok()) {
    return usage_failure(scope.status().render());
  }

  Topology topology;
  RackProfile profile;
  const Topology* topology_view = nullptr;
  const RackProfile* profile_view = nullptr;
  if (!options.topology.empty()) {
    const Result<Topology> loaded_topology = load_topology_document(options.topology);
    if (!loaded_topology.ok()) {
      return document_failure(loaded_topology.status(), "reading " + options.topology);
    }
    topology = loaded_topology.value();
    const Result<RackProfile> loaded_profile = load_profile_document(options.profile);
    if (!loaded_profile.ok()) {
      return document_failure(loaded_profile.status(), "reading " + options.profile);
    }
    profile = loaded_profile.value();
    topology_view = &topology;
    profile_view = &profile;
  }

  const Result<Snapshot> snapshot = build_snapshot(session.window(), scope.value(), requested_dimensions(options),
                                                   topology_view, profile_view, &store);
  if (!snapshot.ok()) {
    return failure_outcome(snapshot.status(), "building the snapshot");
  }

  const std::string document = json::dump(snapshot_to_json(snapshot.value()), true, -1);
  const Result<Digest> published = store.publish_snapshot(document);
  if (!published.ok()) {
    return failure_outcome(published.status(), "publishing the snapshot");
  }
  if (!options.out.empty()) {
    const Result<void> written = io::write_file_atomic(options.out, document.data(), document.size());
    if (!written.ok()) {
      return failure_outcome(written.status(), "writing " + options.out);
    }
  }

  Outcome outcome;
  if (options.json) {
    outcome.output = document + "\n";
  } else {
    outcome.output = render_snapshot_text(snapshot.value());
    outcome.output += "snapshot digest: " + published.value().hex() + "\n";
    outcome.output += "snapshot bytes: " + std::to_string(document.size()) + "\n";
  }
  outcome.code = ledger_exit_code(snapshot.value().ledger);
  if (snapshot.value().fragmentation.has_value() && snapshot.value().fragmentation.value().indeterminate) {
    outcome.code = kExitReported;
  }
  return outcome;
}

Outcome dispatch(const Options& options) {
  if (options.command == "version") {
    return cmd_version(options);
  }
  if (options.command == "help") {
    return cmd_help();
  }
  if (options.command == "ingest") {
    return cmd_ingest(options);
  }
  if (options.command == "explain") {
    return explain_like(options, false);
  }
  if (options.command == "closure") {
    return explain_like(options, true);
  }
  if (options.command == "fragmentation") {
    return cmd_fragmentation(options);
  }
  if (options.command == "verify") {
    return cmd_verify(options);
  }
  if (options.command == "snapshot") {
    return cmd_snapshot(options);
  }
  if (options.command == "recover") {
    return cmd_recover(options);
  }
  return usage_failure("unknown command '" + options.command + "'");
}

}  // namespace
}  // namespace co::cli

int main(int argc, char** argv) {
  co::platform::suppress_error_dialogs();
#if defined(_WIN32)
  // Byte exact streams: JSON printed by the CLI is compared with the bytes it
  // publishes, so no newline translation may happen underneath.
  ::_setmode(::_fileno(stdout), _O_BINARY);
  ::_setmode(::_fileno(stderr), _O_BINARY);
#endif
  const co::Result<co::cli::Options> options = co::cli::parse_options(argc, argv);
  if (!options.ok()) {
    std::fputs(("capacity-observatory: " + options.status().render() + "\n\n").c_str(), stderr);
    std::fputs(co::cli::kUsageText, stderr);
    std::fflush(stderr);
    return co::cli::kExitUsage;
  }
  const co::cli::Outcome outcome = co::cli::dispatch(options.value());
  if (!outcome.output.empty()) {
    std::fwrite(outcome.output.data(), 1, outcome.output.size(), stdout);
  }
  if (!outcome.diagnostic.empty()) {
    std::fwrite(outcome.diagnostic.data(), 1, outcome.diagnostic.size(), stderr);
  }
  std::fflush(stdout);
  std::fflush(stderr);
  return outcome.code;
}
