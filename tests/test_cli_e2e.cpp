// End-to-end tests for the capacity-observatory CLI.
//
// Every case launches the REAL binary as a subprocess through
// co::test::run_process and asserts on its exit code, on its stdout, and on the
// durable effects it left behind. The only thing that runs in this process is
// the store lock holder in the single-writer test, which is a real co::Store
// contending over a real kernel lock with the real binary.
#include "co_test.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "capacity_observatory/hash.hpp"
#include "capacity_observatory/io.hpp"
#include "capacity_observatory/json.hpp"
#include "capacity_observatory/store.hpp"

using namespace co;
using namespace co::test;

namespace {

constexpr std::int64_t kNow = 1700000000000000000LL;

struct CliRun {
  int code{-1};
  std::string output;
};

// Runs the installed-style CLI binary. CO_CLI_PATH is injected by CMake and
// points at the real capacity-observatory executable.
CliRun run_cli(const std::vector<std::string>& arguments) {
  const ProcessResult result = run_process(CO_CLI_PATH, arguments);
  if (!result.started) {
    CO_FAIL("the CLI binary could not be started: " + result.standard_error);
  }
  CliRun run;
  run.code = result.exit_code;
  run.output = result.standard_output;
  return run;
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  CO_REQUIRE(io::exists(path));
}

std::string read_text(const std::string& path) {
  const Result<std::vector<std::uint8_t>> bytes = io::read_file(path, 1U << 20U);
  CO_REQUIRE(bytes.ok());
  return std::string(bytes.value().begin(), bytes.value().end());
}

void require_contains(const std::string& haystack, const std::string& needle) {
  note_assertion();
  if (haystack.find(needle) == std::string::npos) {
    fail(__FILE__, __LINE__, "expected output to contain \"" + needle + "\"\n--- output ---\n" + haystack);
  }
}

std::size_t count_of(const std::string& haystack, const std::string& needle) {
  std::size_t total = 0;
  std::size_t from = 0;
  for (;;) {
    const std::size_t found = haystack.find(needle, from);
    if (found == std::string::npos) {
      return total;
    }
    ++total;
    from = found + needle.size();
  }
}

json::Value parse_json(const std::string& text) {
  const Result<json::Value> parsed = json::Value::parse(text);
  note_assertion();
  if (!parsed.ok()) {
    fail(__FILE__, __LINE__, "expected JSON output: " + parsed.status().render() + "\n--- output ---\n" + text);
  }
  return parsed.value();
}

const json::Value& member(const json::Value& node, const std::string& key) {
  const json::Value* found = node.find(key);
  note_assertion();
  if (found == nullptr) {
    fail(__FILE__, __LINE__, "expected a member \"" + key + "\" in " + json::dump(node, true, -1));
  }
  return *found;
}

std::int64_t int_member(const json::Value& node, const std::string& key) {
  const Result<std::int64_t> value = member(node, key).as_int();
  note_assertion();
  if (!value.ok()) {
    fail(__FILE__, __LINE__, "member \"" + key + "\" is not an integer: " + value.status().render());
  }
  return value.value();
}

std::string string_member(const json::Value& node, const std::string& key) {
  const Result<std::string> value = member(node, key).as_string();
  note_assertion();
  if (!value.ok()) {
    fail(__FILE__, __LINE__, "member \"" + key + "\" is not a string: " + value.status().render());
  }
  return value.value();
}

bool bool_member(const json::Value& node, const std::string& key) {
  const Result<bool> value = member(node, key).as_bool();
  note_assertion();
  if (!value.ok()) {
    fail(__FILE__, __LINE__, "member \"" + key + "\" is not a boolean: " + value.status().render());
  }
  return value.value();
}

const json::Array& array_member(const json::Value& node, const std::string& key) {
  const json::Array* value = member(node, key).as_array();
  note_assertion();
  if (value == nullptr) {
    fail(__FILE__, __LINE__, "member \"" + key + "\" is not an array");
  }
  return *value;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
std::string evidence_record(const std::string& mutation, const std::string& authority, const std::string& assertion,
                            std::int64_t amount, std::int64_t generation) {
  return "{\"mutation\":\"" + mutation + "\",\"authority\":\"" + authority +
         "\",\"scope\":\"site=dc1/hall=h1\",\"dimension\":\"power\",\"assertion\":\"" + assertion +
         "\",\"unit\":\"mW\",\"amount\":" + std::to_string(amount) + ",\"generation\":" +
         std::to_string(generation) + ",\"epoch\":1,\"revision\":0,\"observed_at\":" + std::to_string(kNow) +
         ",\"validity_from\":null,\"validity_until\":null,\"source\":\"fixture\"}";
}

// Every closure identity ties out exactly: nameplate = exclusions + governed,
// installed = exclusions + classes + free, available = computed free, and
// committed + reserved = governed, so the ledger line for power is Complete.
std::string complete_power_evidence() {
  const std::vector<std::string> records{
      evidence_record("m-0001", "policy", "nameplate", 1000000, 41),
      evidence_record("m-0002", "policy", "governed", 960000, 42),
      evidence_record("m-0003", "policy", "excluded-policy", 10000, 43),
      evidence_record("m-0004", "policy", "excluded-maintenance", 5000, 44),
      evidence_record("m-0005", "policy", "excluded-failure", 5000, 45),
      evidence_record("m-0006", "policy", "operational-reserve", 20000, 46),
      evidence_record("m-0007", "dfi", "installed", 1000000, 47),
      evidence_record("m-0008", "dfi", "available", 10000, 48),
      evidence_record("m-0009", "dfi", "stranded", 20000, 49),
      evidence_record("m-0010", "dccp", "committed", 700000, 50),
      evidence_record("m-0011", "asi", "reserved", 260000, 51),
      evidence_record("m-0012", "arbiter", "disputed", 0, 52),
  };
  std::string text;
  for (const std::string& record : records) {
    text += record;
    text += '\n';
  }
  return text;
}

std::string stale_generation_evidence() {
  // The same slot twice: g41 is accepted, g40 is refused.
  return evidence_record("m-1001", "dccp", "committed", 700000, 41) + "\n" +
         evidence_record("m-1002", "dccp", "committed", 300000, 40) + "\n";
}

std::string dimension_mismatch_evidence() {
  // kW denotes power; the record declares cooling.
  return std::string("{\"mutation\":\"m-2001\",\"authority\":\"dfi\",\"scope\":\"site=dc1/hall=h1\","
                     "\"dimension\":\"cooling\",\"assertion\":\"installed\",\"unit\":\"kW\",\"amount\":3,"
                     "\"generation\":1,\"epoch\":1,\"revision\":0}\n");
}

std::string inexact_scale_evidence() {
  // One square foot is 92903.04 square millimetres: not exact, so refused.
  return std::string("{\"mutation\":\"m-3001\",\"authority\":\"dfi\",\"scope\":\"site=dc1/hall=h1\","
                     "\"dimension\":\"space\",\"assertion\":\"installed\",\"unit\":\"ft2\",\"amount\":1,"
                     "\"generation\":1,\"epoch\":1,\"revision\":0}\n");
}

std::string unknown_authority_evidence() {
  return std::string("{\"mutation\":\"m-4001\",\"authority\":\"nobody\",\"scope\":\"site=dc1/hall=h1\","
                     "\"dimension\":\"power\",\"assertion\":\"committed\",\"unit\":\"mW\",\"amount\":5,"
                     "\"generation\":1,\"epoch\":1,\"revision\":0,\"observed_at\":1700000000000000000}\n");
}

std::string expired_evidence() {
  return std::string("{\"mutation\":\"m-5001\",\"authority\":\"dccp\",\"scope\":\"site=dc1/hall=h1\","
                     "\"dimension\":\"power\",\"assertion\":\"committed\",\"unit\":\"mW\",\"amount\":5,"
                     "\"generation\":1,\"epoch\":1,\"revision\":0,\"validity_from\":null,\"validity_until\":1000}\n");
}

std::string fragmentation_topology(bool with_weight) {
  const std::string amounts = with_weight ? "\"power\":2100000,\"weight\":3600000" : "\"power\":2100000";
  return std::string("{\"enclosures\":[") +
         "{\"id\":\"encl-1\",\"scope\":\"site=dc1/hall=h1/enclosure=encl-1\",\"free_ru_contiguous\":84,"
         "\"free_ru_total\":84,\"structurally_usable\":true,\"unusable_reason\":\"\",\"free\":{" + amounts + "}}," +
         "{\"id\":\"encl-2\",\"scope\":\"site=dc1/hall=h1/enclosure=encl-2\",\"free_ru_contiguous\":30,"
         "\"free_ru_total\":30,\"structurally_usable\":true,\"unusable_reason\":\"\",\"free\":{" + amounts + "}}," +
         "{\"id\":\"encl-3\",\"scope\":\"site=dc1/hall=h1/enclosure=encl-3\",\"free_ru_contiguous\":30,"
         "\"free_ru_total\":30,\"structurally_usable\":true,\"unusable_reason\":\"\",\"free\":{" + amounts + "}}]}";
}

std::string fragmentation_profile() {
  return std::string("{\"name\":\"rails-42u\",\"rack_units\":42,\"per_rack\":{\"power\":700000,\"weight\":1200000}}");
}

std::string now_text() { return std::to_string(kNow); }

}  // namespace

// ---------------------------------------------------------------------------
// Version, help, and usage errors
// ---------------------------------------------------------------------------
CO_TEST(cli_version_help_and_usage_errors) {
  const CliRun version = run_cli({"version"});
  CO_REQUIRE_EQ(version.code, 0);
  CO_REQUIRE_EQ(version.output, std::string("capacity-observatory 1.0.0\n"));

  const CliRun version_json = run_cli({"version", "--json"});
  CO_REQUIRE_EQ(version_json.code, 0);
  const json::Value document = parse_json(version_json.output);
  CO_REQUIRE_EQ(string_member(document, "name"), std::string("capacity-observatory"));
  CO_REQUIRE_EQ(string_member(document, "version"), std::string("1.0.0"));

  const CliRun help = run_cli({"help"});
  CO_REQUIRE_EQ(help.code, 0);
  require_contains(help.output, "usage: capacity-observatory <command> [options]");
  require_contains(help.output, "ingest");

  const CliRun none = run_cli({});
  CO_REQUIRE_EQ(none.code, 1);
  require_contains(none.output, "no command was given");
  require_contains(none.output, "usage: capacity-observatory <command> [options]");

  const CliRun unknown_command = run_cli({"summarize"});
  CO_REQUIRE_EQ(unknown_command.code, 1);
  require_contains(unknown_command.output, "unknown command 'summarize'");

  const CliRun unknown_option = run_cli({"explain", "--storey", "somewhere"});
  CO_REQUIRE_EQ(unknown_option.code, 1);
  require_contains(unknown_option.output, "unknown option '--storey'");

  const CliRun missing_value = run_cli({"ingest", "--store"});
  CO_REQUIRE_EQ(missing_value.code, 1);
  require_contains(missing_value.output, "option --store requires a value");

  const CliRun bad_dimension = run_cli({"explain", "--dimension", "bananas"});
  CO_REQUIRE_EQ(bad_dimension.code, 1);
  require_contains(bad_dimension.output, "unknown dimension 'bananas'");

  const CliRun bad_now = run_cli({"explain", "--store", "s", "--now", "later"});
  CO_REQUIRE_EQ(bad_now.code, 1);
  require_contains(bad_now.output, "'later' is not a base ten integer");

  const CliRun missing_store = run_cli({"ingest", "--evidence", "evidence.jsonl"});
  CO_REQUIRE_EQ(missing_store.code, 1);
  require_contains(missing_store.output, "ingest requires --store");

  const CliRun missing_source = run_cli({"explain"});
  CO_REQUIRE_EQ(missing_source.code, 1);
  require_contains(missing_source.output, "explain requires --store or --evidence");

  const CliRun missing_topology = run_cli({"fragmentation", "--profile", "profile.json"});
  CO_REQUIRE_EQ(missing_topology.code, 1);
  require_contains(missing_topology.output, "fragmentation requires --topology");

  const CliRun missing_snapshot_store = run_cli({"snapshot"});
  CO_REQUIRE_EQ(missing_snapshot_store.code, 1);
  require_contains(missing_snapshot_store.output, "snapshot requires --store");
}

// ---------------------------------------------------------------------------
// A store that survives across two separate CLI processes
// ---------------------------------------------------------------------------
CO_TEST(cli_store_survives_across_two_processes) {
  ScratchDirectory scratch("cli-ingest");
  write_text(scratch.child("evidence.jsonl"), complete_power_evidence());
  const std::string store = scratch.child("store");

  // Process 1: ingest, durably first.
  const CliRun ingest = run_cli(
      {"ingest", "--store", store, "--evidence", scratch.child("evidence.jsonl"), "--now", now_text()});
  CO_REQUIRE_EQ(ingest.code, 0);
  require_contains(ingest.output, "record 1: mutation=m-0001 disposition=applied acceptance=accepted reason=ok");
  require_contains(ingest.output, "record 12: mutation=m-0012 disposition=applied acceptance=accepted reason=ok");
  require_contains(ingest.output,
                   "[ingest summary]\n  received: 12\n  applied: 12\n  duplicates: 0\n  refused: 0\n  durable-only: 0\n");

  // The store layout the CLI owns on disk.
  CO_REQUIRE(io::exists(io::join(store, "evidence.log")));
  CO_REQUIRE(io::exists(io::join(store, "epoch")));
  CO_REQUIRE(io::exists(io::join(store, "writer.lock")));
  CO_REQUIRE(!io::exists(io::join(store, "snapshot.json")));

  // Process 2: read the durable evidence back and see the same numbers.
  const std::vector<std::string> explain_arguments{
      "explain", "--store", store, "--dimension", "power", "--now", now_text(), "--json"};
  const CliRun explain = run_cli(explain_arguments);
  CO_REQUIRE_EQ(explain.code, 0);
  const json::Value document = parse_json(explain.output);
  CO_REQUIRE_EQ(string_member(document, "scope"), std::string("site=dc1/hall=h1"));
  const json::Value& provenance = member(document, "provenance");
  CO_REQUIRE_EQ(bool_member(provenance, "store_attached"), true);
  CO_REQUIRE_EQ(bool_member(provenance, "recovered_only"), true);
  CO_REQUIRE_EQ(int_member(provenance, "evidence_records"), 12);
  CO_REQUIRE_EQ(int_member(provenance, "accepted_total"), 12);
  CO_REQUIRE_EQ(int_member(provenance, "refused_records"), 0);
  CO_REQUIRE_EQ(int_member(provenance, "durable_mutations"), 12);
  CO_REQUIRE_EQ(int_member(provenance, "session_epoch"), 1);

  const json::Value& ledger = member(document, "ledger");
  CO_REQUIRE_EQ(bool_member(ledger, "has_residuals"), false);
  CO_REQUIRE_EQ(bool_member(ledger, "has_overcommitment"), false);
  CO_REQUIRE_EQ(int_member(ledger, "incomplete_lines"), 0);
  const json::Array& lines = array_member(ledger, "lines");
  CO_REQUIRE_EQ(lines.size(), std::size_t(1));
  const json::Value& line = lines.front();
  CO_REQUIRE_EQ(string_member(line, "state"), std::string("complete"));
  CO_REQUIRE_EQ(string_member(line, "freshness"), std::string("recovered"));
  CO_REQUIRE_EQ(int_member(line, "evidence_count"), 12);
  CO_REQUIRE_EQ(int_member(line, "unknown_classes"), 0);
  const json::Value& declared = member(line, "declared");
  CO_REQUIRE_EQ(int_member(declared, "installed"), 1000000);
  CO_REQUIRE_EQ(int_member(declared, "committed"), 700000);
  CO_REQUIRE_EQ(int_member(declared, "reserved"), 260000);
  const json::Value& derived = member(line, "derived");
  CO_REQUIRE_EQ(int_member(derived, "exclusion_total"), 40000);
  CO_REQUIRE_EQ(int_member(derived, "serviceable"), 990000);
  CO_REQUIRE_EQ(int_member(derived, "free_usable"), 10000);
  CO_REQUIRE_EQ(int_member(derived, "governed"), 960000);
  CO_REQUIRE_EQ(bool_member(derived, "governed_derived"), false);

  // Process 3: the same inputs produce byte identical output.
  const CliRun repeat = run_cli(explain_arguments);
  CO_REQUIRE_EQ(repeat.code, 0);
  CO_REQUIRE_EQ(repeat.output, explain.output);

  // The durable log still holds exactly what process 1 committed.
  const CliRun verify = run_cli({"verify", "--store", store});
  CO_REQUIRE_EQ(verify.code, 0);
  require_contains(verify.output, "frames: 13");
  require_contains(verify.output, "evidence frames: 12");
  require_contains(verify.output, "snapshot frames: 0");
  require_contains(verify.output, "epoch frames: 1");
  require_contains(verify.output, "discarded bytes: 0");
  require_contains(verify.output, "torn tail: false");
  require_contains(verify.output, "session epoch: e1");

  const CliRun verify_again = run_cli({"verify", "--store", store});
  CO_REQUIRE_EQ(verify_again.output, verify.output);

  // The same report is available as a canonical JSON document.
  const CliRun verify_json = run_cli({"verify", "--store", store, "--json"});
  CO_REQUIRE_EQ(verify_json.code, 0);
  const json::Value report = parse_json(verify_json.output);
  CO_REQUIRE_EQ(int_member(report, "frames"), 13);
  CO_REQUIRE_EQ(int_member(report, "evidence_frames"), 12);
  CO_REQUIRE_EQ(int_member(report, "snapshot_frames"), 0);
  CO_REQUIRE_EQ(int_member(report, "epoch_frames"), 1);
  CO_REQUIRE_EQ(int_member(report, "discarded_bytes"), 0);
  CO_REQUIRE_EQ(bool_member(report, "torn_tail_recovered"), false);
  CO_REQUIRE_EQ(int_member(report, "session_epoch"), 1);
}

// ---------------------------------------------------------------------------
// File evidence: the ledger, the closure view, and the JSON document
// ---------------------------------------------------------------------------
CO_TEST(cli_explain_from_file_evidence) {
  ScratchDirectory scratch("cli-explain");
  write_text(scratch.child("evidence.jsonl"), complete_power_evidence());

  const CliRun explain = run_cli(
      {"explain", "--evidence", scratch.child("evidence.jsonl"), "--dimension", "power", "--now", now_text()});
  CO_REQUIRE_EQ(explain.code, 0);
  require_contains(explain.output, "generated_at: " + now_text() + " (unix nanoseconds)");
  require_contains(explain.output, "[power] state=complete freshness=fresh evidence=12 unknown_classes=0");
  require_contains(explain.output, "    free_usable = 10000");
  require_contains(explain.output, "    governed = 960000 (declared)");

  const CliRun closure = run_cli(
      {"closure", "--evidence", scratch.child("evidence.jsonl"), "--dimension", "power", "--now", now_text()});
  CO_REQUIRE_EQ(closure.code, 0);
  require_contains(closure.output, "[closure] scope=site=dc1/hall=h1");
  require_contains(closure.output,
                   "  has_residuals: false has_overcommitment: false incomplete_lines: 0");
  require_contains(closure.output, "[power] state=complete freshness=fresh evidence=12 unknown_classes=0");
  require_contains(closure.output, "  governance: residual=0 holds=true reason=ok");
  require_contains(closure.output, "  installed: residual=0 holds=true reason=ok");
  require_contains(closure.output, "  available: residual=0 holds=true reason=ok");
  require_contains(closure.output, "  allocation: residual=0 holds=true reason=ok");
  require_contains(closure.output, "  unexplained: governance=0 installed=0 available=0 overcommitment=unknown");

  // The same document without --dimension covers every dimension, so the five
  // dimensions with no evidence are indeterminate: reported, and exit 2.
  const CliRun every_dimension = run_cli(
      {"explain", "--evidence", scratch.child("evidence.jsonl"), "--now", now_text()});
  CO_REQUIRE_EQ(every_dimension.code, 2);
  require_contains(every_dimension.output, "[cooling] state=indeterminate");
  require_contains(every_dimension.output, "no current evidence exists for site=dc1/hall=h1 cooling");

  // An explicit scope must be a prefix of the evidence it explains.
  const CliRun scoped = run_cli({"explain", "--evidence", scratch.child("evidence.jsonl"), "--scope", "site=dc1",
                                 "--dimension", "power", "--now", now_text(), "--json"});
  CO_REQUIRE_EQ(scoped.code, 0);
  CO_REQUIRE_EQ(string_member(parse_json(scoped.output), "scope"), std::string("site=dc1"));

  // An explicit scope that no evidence falls under is indeterminate, not an
  // error: the line is reported with no evidence and the run exits 2.
  const CliRun empty_scope = run_cli({"explain", "--evidence", scratch.child("evidence.jsonl"), "--scope",
                                      "site=dc9", "--dimension", "power", "--now", now_text()});
  CO_REQUIRE_EQ(empty_scope.code, 2);
  require_contains(empty_scope.output, "no current evidence exists for site=dc9 power");

  // The closure view is also available as the canonical JSON document.
  const CliRun closure_json = run_cli({"closure", "--evidence", scratch.child("evidence.jsonl"), "--dimension",
                                       "power", "--now", now_text(), "--json"});
  CO_REQUIRE_EQ(closure_json.code, 0);
  const json::Value closure_document = parse_json(closure_json.output);
  CO_REQUIRE_EQ(int_member(member(closure_document, "ledger"), "incomplete_lines"), 0);

  // --dimension accumulates: two named dimensions produce two ledger lines.
  const CliRun two_dimensions =
      run_cli({"explain", "--evidence", scratch.child("evidence.jsonl"), "--dimension", "power", "--dimension",
               "rack-units", "--now", now_text(), "--json"});
  CO_REQUIRE_EQ(two_dimensions.code, 2);
  const json::Value two_dimension_document = parse_json(two_dimensions.output);
  const json::Array& two_lines = array_member(member(two_dimension_document, "ledger"), "lines");
  CO_REQUIRE_EQ(two_lines.size(), std::size_t(2));
  // Lines are ordered by the canonical dimension order, not by the order the
  // options were given, so repeated collections stay comparable.
  CO_REQUIRE_EQ(string_member(two_lines.at(0), "dimension"), std::string("rack-units"));
  CO_REQUIRE_EQ(string_member(two_lines.at(0), "state"), std::string("indeterminate"));
  CO_REQUIRE_EQ(string_member(two_lines.at(1), "dimension"), std::string("power"));
  CO_REQUIRE_EQ(string_member(two_lines.at(1), "state"), std::string("complete"));

  // A record with no observation instant is indeterminate for freshness and is
  // never assumed to be fresh.
  write_text(scratch.child("unobserved.jsonl"),
             std::string("{\"mutation\":\"m-7001\",\"authority\":\"dccp\",\"scope\":\"site=dc1/hall=h1\","
                         "\"dimension\":\"power\",\"assertion\":\"committed\",\"unit\":\"mW\","
                         "\"amount\":700000,\"generation\":1,\"epoch\":1,\"revision\":0}\n"));
  const CliRun unobserved = run_cli({"closure", "--evidence", scratch.child("unobserved.jsonl"), "--dimension",
                                     "power", "--now", now_text()});
  CO_REQUIRE_EQ(unobserved.code, 2);
  require_contains(unobserved.output, "[power] state=partial freshness=unknown");

  // A wall clock evaluation instant without --now is still a well formed run.
  const CliRun subject = run_cli({"explain", "--evidence", scratch.child("evidence.jsonl"), "--dimension", "power"});
  CO_REQUIRE_EQ(subject.code, 0);
}

// ---------------------------------------------------------------------------
// Refusals: every one is reported, and each one exits 2
// ---------------------------------------------------------------------------
CO_TEST(cli_refusals_are_reported_with_exit_two) {
  ScratchDirectory stale_scratch("cli-stale");
  write_text(stale_scratch.child("evidence.jsonl"), stale_generation_evidence());
  const CliRun stale =
      run_cli({"ingest", "--store", stale_scratch.child("store"), "--evidence", stale_scratch.child("evidence.jsonl"),
               "--now", now_text()});
  CO_REQUIRE_EQ(stale.code, 2);
  require_contains(stale.output, "disposition=durable-not-applied acceptance=stale-generation reason=stale-generation");
  require_contains(stale.output, "older than the accepted generation g41");
  require_contains(stale.output, "[ingest summary]\n  received: 2\n  applied: 1\n  duplicates: 0\n  refused: 0\n"
                                 "  durable-only: 1\n");
  require_contains(stale.output, "[refusals]\n  dccp|site=dc1/hall=h1|power|committed|e1|g40 -> durable-not-applied");

  ScratchDirectory refused_scratch("cli-refused");
  const std::string store = refused_scratch.child("store");
  write_text(refused_scratch.child("mismatch.jsonl"), dimension_mismatch_evidence());
  const CliRun mismatch = run_cli({"ingest", "--store", store, "--evidence", refused_scratch.child("mismatch.jsonl"),
                                   "--now", now_text()});
  CO_REQUIRE_EQ(mismatch.code, 2);
  require_contains(mismatch.output, "disposition=refused acceptance=none reason=dimension-mismatch");
  require_contains(mismatch.output, "unparsed: 1");

  write_text(refused_scratch.child("inexact.jsonl"), inexact_scale_evidence());
  const CliRun inexact = run_cli({"ingest", "--store", store, "--evidence", refused_scratch.child("inexact.jsonl"),
                                  "--now", now_text()});
  CO_REQUIRE_EQ(inexact.code, 2);
  require_contains(inexact.output, "reason=inexact-scale");
  require_contains(inexact.output, "was refused, not rounded");

  write_text(refused_scratch.child("authority.jsonl"), unknown_authority_evidence());
  const CliRun authority = run_cli({"ingest", "--store", store, "--evidence", refused_scratch.child("authority.jsonl"),
                                    "--now", now_text()});
  CO_REQUIRE_EQ(authority.code, 2);
  require_contains(authority.output, "disposition=durable-not-applied acceptance=refused reason=unknown-authority");

  write_text(refused_scratch.child("expired.jsonl"), expired_evidence());
  const CliRun expired = run_cli({"ingest", "--store", store, "--evidence", refused_scratch.child("expired.jsonl"),
                                  "--now", now_text()});
  CO_REQUIRE_EQ(expired.code, 2);
  require_contains(expired.output, "acceptance=refused reason=evidence-expired");
  require_contains(expired.output, "evidence validity window closed at 1000");

  // Two refusals with --limit 1 keep exactly one audit trail entry.
  write_text(refused_scratch.child("both.jsonl"),
             dimension_mismatch_evidence() + inexact_scale_evidence());
  const CliRun limited =
      run_cli({"ingest", "--store", store, "--evidence", refused_scratch.child("both.jsonl"), "--limit", "1",
               "--now", now_text()});
  CO_REQUIRE_EQ(limited.code, 2);
  require_contains(limited.output, "  refused: 2\n");
  CO_REQUIRE_EQ(count_of(limited.output, " -> refused ("), std::size_t(1));

  // A malformed document is a parse error, not a refusal.
  write_text(refused_scratch.child("broken.jsonl"), "{\"mutation\": \"m-1\", }\n");
  const CliRun broken = run_cli({"ingest", "--store", store, "--evidence", refused_scratch.child("broken.jsonl"),
                                 "--now", now_text()});
  CO_REQUIRE_EQ(broken.code, 2);
  require_contains(broken.output, "reason=parse-error");
  require_contains(broken.output, "unparsed: 1");

  // A document that is not a JSON object is a usage/parse error.
  write_text(refused_scratch.child("array.json"), "[{\"mutation\":\"m-1\"}]");
  const CliRun array = run_cli({"ingest", "--store", store, "--evidence", refused_scratch.child("array.json"),
                                "--now", now_text()});
  CO_REQUIRE_EQ(array.code, 1);
  require_contains(array.output, "must be a JSON object with a \"records\" array");

  // The wrapper form of the same evidence is accepted.
  write_text(refused_scratch.child("wrapper.json"),
             "{\"records\":[" + evidence_record("m-9001", "dccp", "committed", 5, 1) + "]}");
  const CliRun wrapper = run_cli({"ingest", "--store", store, "--evidence", refused_scratch.child("wrapper.json"),
                                  "--now", now_text()});
  CO_REQUIRE_EQ(wrapper.code, 0);
  require_contains(wrapper.output, "  applied: 1\n");
}

// ---------------------------------------------------------------------------
// Single writer: a second writer is refused with lock-held, and only lock-held
// ---------------------------------------------------------------------------
CO_TEST(cli_second_writer_is_refused_with_lock_held) {
  ScratchDirectory scratch("cli-lock");
  write_text(scratch.child("evidence.jsonl"), complete_power_evidence());
  const std::string store_directory = scratch.child("store");

  StoreOptions options;
  options.directory = store_directory;
  Result<Store> holder = Store::open(options);
  CO_REQUIRE(holder.ok());
  CO_REQUIRE(holder.value().is_open());

  const std::vector<std::string> arguments{
      "ingest", "--store", store_directory, "--evidence", scratch.child("evidence.jsonl"), "--now", now_text()};
  const CliRun refused = run_cli(arguments);
  CO_REQUIRE_EQ(refused.code, 2);
  require_contains(refused.output, "lock-held");
  require_contains(refused.output, "opening the store for writing");

  // The refusal was the lock and nothing else: the same command succeeds once
  // the holder releases it.
  holder.value().close();
  const CliRun accepted = run_cli(arguments);
  CO_REQUIRE_EQ(accepted.code, 0);
  require_contains(accepted.output, "  applied: 12\n");
}

// ---------------------------------------------------------------------------
// Determinism: identical inputs produce identical bytes across processes
// ---------------------------------------------------------------------------
CO_TEST(cli_repeated_runs_are_byte_identical) {
  ScratchDirectory first("cli-determinism-a");
  ScratchDirectory second("cli-determinism-b");
  write_text(first.child("evidence.jsonl"), complete_power_evidence());
  write_text(second.child("evidence.jsonl"), complete_power_evidence());

  const CliRun first_ingest =
      run_cli({"ingest", "--store", first.child("store"), "--evidence", first.child("evidence.jsonl"),
               "--now", now_text()});
  const CliRun second_ingest =
      run_cli({"ingest", "--store", second.child("store"), "--evidence", second.child("evidence.jsonl"),
               "--now", now_text()});
  CO_REQUIRE_EQ(first_ingest.code, 0);
  CO_REQUIRE_EQ(second_ingest.code, 0);
  CO_REQUIRE_EQ(first_ingest.output, second_ingest.output);

  const std::vector<std::string> explain_arguments{
      "explain", "--store", first.child("store"), "--dimension", "power", "--now", now_text()};
  const CliRun explain_once = run_cli(explain_arguments);
  const CliRun explain_twice = run_cli(explain_arguments);
  CO_REQUIRE_EQ(explain_once.code, 0);
  CO_REQUIRE_EQ(explain_once.output, explain_twice.output);

  const std::vector<std::string> closure_arguments{
      "closure", "--evidence", first.child("evidence.jsonl"), "--dimension", "power", "--now", now_text()};
  CO_REQUIRE_EQ(run_cli(closure_arguments).output, run_cli(closure_arguments).output);

  // Replaying the same evidence into the same store is idempotent and succeeds.
  const CliRun replay =
      run_cli({"ingest", "--store", first.child("store"), "--evidence", first.child("evidence.jsonl"),
               "--now", now_text()});
  CO_REQUIRE_EQ(replay.code, 0);
  require_contains(replay.output, "  duplicates: 12\n");
  require_contains(replay.output, "  applied: 0\n");
}

// ---------------------------------------------------------------------------
// Fragmentation
// ---------------------------------------------------------------------------
CO_TEST(cli_fragmentation_report) {
  ScratchDirectory scratch("cli-fragmentation");
  write_text(scratch.child("topology.json"), fragmentation_topology(true));
  write_text(scratch.child("profile.json"), fragmentation_profile());

  const CliRun text = run_cli({"fragmentation", "--topology", scratch.child("topology.json"), "--profile",
                               scratch.child("profile.json"), "--now", now_text()});
  CO_REQUIRE_EQ(text.code, 0);
  require_contains(text.output, "[fragmentation] profile=rails-42u rack_units=42 realizable=2 ideal=3 fragmented=1");
  require_contains(text.output, "  constrained dimensions: rack-units");
  require_contains(text.output, "  enclosure encl-1 (site=dc1/hall=h1/enclosure=encl-1): realizable=2");

  const CliRun report = run_cli({"fragmentation", "--topology", scratch.child("topology.json"), "--profile",
                                 scratch.child("profile.json"), "--now", now_text(), "--json"});
  CO_REQUIRE_EQ(report.code, 0);
  const json::Value document = parse_json(report.output);
  CO_REQUIRE_EQ(string_member(document, "profile"), std::string("rails-42u"));
  CO_REQUIRE_EQ(int_member(document, "rack_units_per_rack"), 42);
  CO_REQUIRE_EQ(int_member(document, "realizable_racks"), 2);
  CO_REQUIRE_EQ(int_member(document, "ideal_racks"), 3);
  CO_REQUIRE_EQ(int_member(document, "fragmented_racks"), 1);
  CO_REQUIRE_EQ(bool_member(document, "indeterminate"), false);
  CO_REQUIRE_EQ(string_member(document, "constrained_dimensions"), std::string("rack-units"));
  CO_REQUIRE(document.find("ledger") == nullptr);
  const json::Array& enclosures = array_member(document, "enclosures");
  CO_REQUIRE_EQ(enclosures.size(), std::size_t(3));
  CO_REQUIRE_EQ(int_member(enclosures.at(0), "realizable_racks"), 2);
  CO_REQUIRE_EQ(int_member(enclosures.at(1), "realizable_racks"), 0);
  CO_REQUIRE_EQ(int_member(enclosures.at(2), "realizable_racks"), 0);

  // A profile that demands a dimension the topology does not declare is
  // indeterminate: reported, and exit 2 rather than silently zero.
  write_text(scratch.child("no-weight.json"), fragmentation_topology(false));
  const CliRun indeterminate =
      run_cli({"fragmentation", "--topology", scratch.child("no-weight.json"), "--profile",
               scratch.child("profile.json"), "--now", now_text(), "--json"});
  CO_REQUIRE_EQ(indeterminate.code, 2);
  const json::Value unknown = parse_json(indeterminate.output);
  CO_REQUIRE_EQ(bool_member(unknown, "indeterminate"), true);
  CO_REQUIRE_EQ(int_member(unknown, "indeterminate_enclosures"), 3);

  // A topology document with an unknown member is a usage/parse error.
  write_text(scratch.child("bad-topology.json"),
             "{\"enclosures\":[{\"id\":\"encl-1\",\"scope\":\"site=dc1\",\"free\":{},\"colour\":\"blue\"}]}");
  const CliRun bad_topology = run_cli({"fragmentation", "--topology", scratch.child("bad-topology.json"),
                                       "--profile", scratch.child("profile.json")});
  CO_REQUIRE_EQ(bad_topology.code, 1);
  require_contains(bad_topology.output, "unknown member \"colour\"");
}

// ---------------------------------------------------------------------------
// Snapshot publication
// ---------------------------------------------------------------------------
CO_TEST(cli_snapshot_publishes_through_the_store) {
  ScratchDirectory scratch("cli-snapshot");
  write_text(scratch.child("evidence.jsonl"), complete_power_evidence());
  write_text(scratch.child("topology.json"), fragmentation_topology(true));
  write_text(scratch.child("profile.json"), fragmentation_profile());
  const std::string store = scratch.child("store");

  const CliRun ingest = run_cli(
      {"ingest", "--store", store, "--evidence", scratch.child("evidence.jsonl"), "--now", now_text()});
  CO_REQUIRE_EQ(ingest.code, 0);

  const std::string copy = scratch.child("snapshot-copy.json");
  const CliRun published =
      run_cli({"snapshot", "--store", store, "--dimension", "power", "--topology", scratch.child("topology.json"),
               "--profile", scratch.child("profile.json"), "--now", now_text(), "--json", "--out", copy});
  CO_REQUIRE_EQ(published.code, 0);
  const json::Value document = parse_json(published.output);
  CO_REQUIRE_EQ(string_member(document, "scope"), std::string("site=dc1/hall=h1"));
  CO_REQUIRE_EQ(int_member(member(document, "provenance"), "evidence_records"), 12);
  const json::Value& fragmentation = member(document, "fragmentation");
  CO_REQUIRE_EQ(int_member(fragmentation, "realizable_racks"), 2);

  // The published document, the --out copy, and stdout are the same bytes.
  const std::string on_disk = read_text(io::join(store, "snapshot.json"));
  CO_REQUIRE_EQ(read_text(copy), on_disk);
  CO_REQUIRE_EQ(published.output, on_disk + "\n");
  const std::string redumped = json::dump(document, true, -1);
  CO_REQUIRE_EQ(redumped, on_disk);

  // The text rendering prints the digest of exactly the bytes it published.
  const std::string text_copy = scratch.child("snapshot-copy-text.json");
  const CliRun text = run_cli({"snapshot", "--store", store, "--dimension", "power", "--now", now_text(), "--out",
                               text_copy});
  CO_REQUIRE_EQ(text.code, 0);
  require_contains(text.output, "snapshot digest: ");
  CO_REQUIRE_EQ(read_text(text_copy), read_text(io::join(store, "snapshot.json")));
  const std::string marker = "snapshot digest: ";
  const std::size_t start = text.output.find(marker) + marker.size();
  const std::string digest = text.output.substr(start, text.output.find('\n', start) - start);
  CO_REQUIRE_EQ(digest.size(), std::size_t(64));
  const std::string republished = read_text(io::join(store, "snapshot.json"));
  CO_REQUIRE_EQ(digest, sha256(republished).hex());
  require_contains(text.output, "snapshot bytes: " + std::to_string(republished.size()));

  // Both publications are framed in the log.
  const CliRun verify = run_cli({"verify", "--store", store});
  CO_REQUIRE_EQ(verify.code, 0);
  require_contains(verify.output, "snapshot frames: 2");

  // A snapshot without evidence is a usage error rather than an empty document.
  const CliRun empty = run_cli({"snapshot", "--store", scratch.child("empty-store"), "--now", now_text()});
  CO_REQUIRE_EQ(empty.code, 1);
  require_contains(empty.output, "no scope is present in the inputs");
}

// ---------------------------------------------------------------------------
// Integrity and I/O failures
// ---------------------------------------------------------------------------
CO_TEST(cli_integrity_failures_exit_three) {
  ScratchDirectory scratch("cli-integrity");

  // A log that does not begin with the store magic is rejected.
  const std::string broken = scratch.child("broken");
  CO_REQUIRE_OK_VOID(io::ensure_directory(broken));
  write_text(io::join(broken, "evidence.log"), "this is not a capacity observatory store");
  const CliRun bad_magic = run_cli({"verify", "--store", broken});
  CO_REQUIRE_EQ(bad_magic.code, 3);
  require_contains(bad_magic.output, "bad-magic");

  // A store whose log does not exist yet is reported, not rejected.
  const CliRun absent = run_cli({"verify", "--store", scratch.child("absent")});
  CO_REQUIRE_EQ(absent.code, 0);
  require_contains(absent.output, "store log does not exist");

  // Reading a store whose log was never written is refused, not crashed: the
  // library classifies an absent log as not-found rather than as damage.
  const CliRun missing_log = run_cli({"explain", "--store", scratch.child("never"), "--now", now_text()});
  CO_REQUIRE_EQ(missing_log.code, 2);
  require_contains(missing_log.output, "not-found");
  require_contains(missing_log.output, "no store log exists");

  // A store path that cannot even be created is an I/O failure.
  const CliRun uncreatable =
      run_cli({"explain", "--store", io::join(scratch.child("absent-parent"), "store"), "--now", now_text()});
  CO_REQUIRE_EQ(uncreatable.code, 3);
  require_contains(uncreatable.output, "io-failure");

  // A torn tail: the last frame is incomplete, so recovery discards it and the
  // committed records survive.
  const std::string store = scratch.child("torn-store");
  write_text(scratch.child("evidence.jsonl"), complete_power_evidence());
  const CliRun ingest = run_cli(
      {"ingest", "--store", store, "--evidence", scratch.child("evidence.jsonl"), "--now", now_text()});
  CO_REQUIRE_EQ(ingest.code, 0);
  {
    Result<io::File> log = io::File::open(io::join(store, "evidence.log"), false);
    CO_REQUIRE(log.ok());
    CO_REQUIRE_OK_VOID(log.value().seek_end());
    CO_REQUIRE_OK_VOID(log.value().write_all("TORN", 4));
    CO_REQUIRE_OK_VOID(log.value().sync());
  }

  const CliRun torn = run_cli({"verify", "--store", store});
  CO_REQUIRE_EQ(torn.code, 0);
  require_contains(torn.output, "torn tail: true");
  require_contains(torn.output, "discarded bytes: 4");
  require_contains(torn.output, "evidence frames: 12");

  const CliRun recovered = run_cli({"recover", "--store", store});
  CO_REQUIRE_EQ(recovered.code, 0);
  require_contains(recovered.output, "torn tail: true");
  require_contains(recovered.output, "discarded bytes: 4");
  require_contains(recovered.output, "durable evidence records: 12");

  const CliRun after = run_cli({"verify", "--store", store});
  CO_REQUIRE_EQ(after.code, 0);
  require_contains(after.output, "torn tail: false");
  require_contains(after.output, "evidence frames: 12");
  require_contains(after.output, "epoch frames: 2");

  // Recovery is idempotent and reports the same durable record count as JSON.
  const CliRun recovered_json = run_cli({"recover", "--store", store, "--json"});
  CO_REQUIRE_EQ(recovered_json.code, 0);
  const json::Value recovery = parse_json(recovered_json.output);
  CO_REQUIRE_EQ(int_member(recovery, "durable_records"), 12);
  CO_REQUIRE_EQ(int_member(recovery, "evidence_frames"), 12);
  CO_REQUIRE_EQ(bool_member(recovery, "torn_tail_recovered"), false);

  // Interior corruption: a damaged frame with valid frames after it is rejected
  // rather than treated as a torn tail.
  const std::string corrupt_store = scratch.child("corrupt-store");
  const CliRun corrupt_ingest =
      run_cli({"ingest", "--store", corrupt_store, "--evidence", scratch.child("evidence.jsonl"),
               "--now", now_text()});
  CO_REQUIRE_EQ(corrupt_ingest.code, 0);
  {
    const std::string log_path = io::join(corrupt_store, "evidence.log");
    Result<io::File> log = io::File::open(log_path, false);
    CO_REQUIRE(log.ok());
    std::uint8_t byte = 0;
    const Result<std::size_t> read = log.value().read_at(100, &byte, 1);
    CO_REQUIRE(read.ok());
    CO_REQUIRE_EQ(read.value(), std::size_t(1));
    const std::uint8_t flipped = static_cast<std::uint8_t>(byte ^ 0x01U);
    CO_REQUIRE_OK_VOID(log.value().seek(100));
    CO_REQUIRE_OK_VOID(log.value().write_all(&flipped, 1));
    CO_REQUIRE_OK_VOID(log.value().sync());
  }
  const CliRun corrupt = run_cli({"verify", "--store", corrupt_store});
  CO_REQUIRE_EQ(corrupt.code, 3);
  require_contains(corrupt.output, "interior-corruption");

  // The recovered store still explains the same numbers.
  const CliRun explain =
      run_cli({"explain", "--store", store, "--dimension", "power", "--now", now_text()});
  CO_REQUIRE_EQ(explain.code, 0);
  require_contains(explain.output, "[power] state=complete freshness=recovered evidence=12");
}
