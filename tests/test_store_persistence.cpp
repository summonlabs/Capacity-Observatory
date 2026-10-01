// Capacity Observatory - store persistence suite.
//
// Proves the durable store format and framing, conservative torn-tail recovery,
// interior-corruption rejection, the append commit point, durable idempotency,
// session epoch publication, atomic snapshot publication, and bounded
// resources. Every claim about the file is checked against the raw bytes on
// disk (magic, version, header CRC, frame lengths, frame CRCs), not only
// against the API that wrote them.
#include "co_test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/hash.hpp"
#include "capacity_observatory/io.hpp"
#include "capacity_observatory/json.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/store.hpp"
#include "capacity_observatory/strong.hpp"
#include "capacity_observatory/time.hpp"

using namespace co;

namespace co::test {

// Renderers for the strong identity types, so CO_REQUIRE_EQ can report them.
inline std::string render(Epoch value) { return "e" + std::to_string(value.value()); }
inline std::string render(SequenceNumber value) { return "s" + std::to_string(value.value()); }
inline std::string render(const MutationId& value) { return value.value(); }
inline std::string render(const Digest& value) { return value.hex(); }

}  // namespace co::test

namespace {

constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kFrameHeaderBytes = 8;
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::int64_t kObservedAt = 1700000000000000000LL;
constexpr std::string_view kMagic = "COBSTORE";

// Binds the store held by a Result so that non-const operations are reachable,
// and counts the assertion like every other CO_REQUIRE_* macro.
#define CO_REQUIRE_STORE(result_expression, name)                                      \
  auto co_store_result_##name = (result_expression);                                   \
  ::co::test::note_assertion();                                                        \
  if (!co_store_result_##name.ok()) {                                                  \
    CO_FAIL(std::string("expected success from ") + #result_expression + ": " +        \
            co_store_result_##name.status().render());                                 \
  }                                                                                    \
  auto& name = co_store_result_##name.value()

// ---------------------------------------------------------------------------
// Raw little-endian framing helpers, mirroring the on-disk format exactly.
// ---------------------------------------------------------------------------
std::uint32_t load_u32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t load_u64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned int i = 0; i < 8U; ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
  }
  return value;
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

std::vector<std::uint8_t> bytes_of(std::string_view text) {
  std::vector<std::uint8_t> out;
  out.reserve(text.size());
  for (const char ch : text) {
    out.push_back(static_cast<std::uint8_t>(ch));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Filesystem helpers. The store keeps its log open for the whole session with
// FILE_SHARE_READ, so every mutating helper here runs only while the store is
// shut.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> read_bytes(const std::string& path) {
  Result<std::vector<std::uint8_t>> result = io::read_file(path, 1ULL << 28U);
  if (!result.ok()) {
    CO_FAIL("could not read " + path + ": " + result.status().render());
  }
  return std::move(result).value();
}

std::uint64_t size_of(const std::string& path) {
  Result<io::File> file = io::File::open_read_only(path);
  if (!file.ok()) {
    CO_FAIL("could not open " + path + ": " + file.status().render());
  }
  Result<std::uint64_t> size = file.value().size();
  if (!size.ok()) {
    CO_FAIL("could not size " + path + ": " + size.status().render());
  }
  return size.value();
}

void append_bytes_at_end(const std::string& path, const std::vector<std::uint8_t>& extra) {
  Result<io::File> file = io::File::open(path, false);
  if (!file.ok()) {
    CO_FAIL("could not open " + path + " for append: " + file.status().render());
  }
  if (!file.value().seek_end().ok()) {
    CO_FAIL("could not seek to the end of " + path);
  }
  Result<void> written = file.value().write_all(extra.data(), extra.size());
  if (!written.ok()) {
    CO_FAIL("could not append to " + path + ": " + written.status().render());
  }
  Result<void> synced = file.value().sync();
  if (!synced.ok()) {
    CO_FAIL("could not sync " + path + ": " + synced.status().render());
  }
}

void replace_bytes(const std::string& path, const std::vector<std::uint8_t>& data) {
  Result<io::File> file = io::File::open(path, true);
  if (!file.ok()) {
    CO_FAIL("could not open " + path + " for replacement: " + file.status().render());
  }
  Result<void> truncated = file.value().truncate(0);
  if (!truncated.ok()) {
    CO_FAIL("could not truncate " + path + ": " + truncated.status().render());
  }
  Result<void> seeked = file.value().seek(0);
  if (!seeked.ok()) {
    CO_FAIL("could not rewind " + path);
  }
  if (!data.empty()) {
    Result<void> written = file.value().write_all(data.data(), data.size());
    if (!written.ok()) {
      CO_FAIL("could not rewrite " + path + ": " + written.status().render());
    }
  }
  Result<void> synced = file.value().sync();
  if (!synced.ok()) {
    CO_FAIL("could not sync " + path + ": " + synced.status().render());
  }
}

void patch_byte(const std::string& path, std::size_t offset, std::uint8_t value) {
  std::vector<std::uint8_t> bytes = read_bytes(path);
  if (offset >= bytes.size()) {
    CO_FAIL("patch offset " + std::to_string(offset) + " is outside " + path);
  }
  bytes[offset] = value;
  replace_bytes(path, bytes);
}

bool same_bytes(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool prefix_matches(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& prefix,
                    std::size_t count) {
  return bytes.size() >= count && prefix.size() >= count &&
         std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count), prefix.begin());
}

// ---------------------------------------------------------------------------
// Raw log structure: header plus the frames a strict reader can walk.
// ---------------------------------------------------------------------------
struct FrameView {
  std::size_t offset{0};
  std::uint32_t declared_size{0};
  std::uint32_t declared_crc{0};
  std::uint32_t computed_crc{0};
  bool crc_ok{false};
  std::string payload;
};

struct LogView {
  bool header_present{false};
  bool magic_ok{false};
  bool version_ok{false};
  bool header_crc_ok{false};
  std::uint16_t version{0};
  std::vector<FrameView> frames;
  std::size_t framed_end{0};

  [[nodiscard]] bool header_ok() const { return header_present && magic_ok && version_ok && header_crc_ok; }
};

LogView view_log(const std::vector<std::uint8_t>& bytes) {
  LogView view;
  if (bytes.size() < kHeaderBytes) {
    return view;
  }
  view.header_present = true;
  view.magic_ok = std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) == 0;
  view.version = static_cast<std::uint16_t>(bytes[8] | (static_cast<std::uint16_t>(bytes[9]) << 8U));
  view.version_ok = view.version == kFormatVersion;
  view.header_crc_ok = crc32c_bytes(bytes.data(), 12U) == load_u32(bytes.data() + 12U);
  if (!view.header_ok()) {
    return view;
  }
  view.framed_end = kHeaderBytes;
  std::size_t offset = kHeaderBytes;
  while (offset + kFrameHeaderBytes <= bytes.size()) {
    FrameView frame;
    frame.offset = offset;
    frame.declared_size = load_u32(bytes.data() + offset);
    frame.declared_crc = load_u32(bytes.data() + offset + 4U);
    const std::size_t payload_begin = offset + kFrameHeaderBytes;
    if (payload_begin + frame.declared_size > bytes.size()) {
      break;
    }
    frame.payload.assign(reinterpret_cast<const char*>(bytes.data() + payload_begin), frame.declared_size);
    frame.computed_crc = crc32c(frame.payload);
    frame.crc_ok = frame.computed_crc == frame.declared_crc;
    view.frames.push_back(std::move(frame));
    offset = payload_begin + view.frames.back().declared_size;
    view.framed_end = offset;
    if (!view.frames.back().crc_ok) {
      break;
    }
  }
  return view;
}

std::string frame_kind(const FrameView& frame) {
  const Result<json::Value> parsed = json::Value::parse(frame.payload);
  if (!parsed.ok()) {
    return "not-json";
  }
  const Result<std::string> kind = parsed.value().require_string("kind");
  return kind.ok() ? kind.value() : std::string("no-kind");
}

std::string frame_string(const FrameView& frame, std::string_view key) {
  const Result<json::Value> parsed = json::Value::parse(frame.payload);
  if (!parsed.ok()) {
    return std::string();
  }
  const Result<std::string> value = parsed.value().require_string(key);
  return value.ok() ? value.value() : std::string();
}

std::int64_t frame_int(const FrameView& frame, std::string_view key) {
  const Result<json::Value> parsed = json::Value::parse(frame.payload);
  if (!parsed.ok()) {
    return -1;
  }
  const Result<std::int64_t> value = parsed.value().require_int(key);
  return value.ok() ? value.value() : -1;
}

std::vector<std::uint8_t> make_header(std::uint16_t version, bool magic_ok, bool crc_ok) {
  std::vector<std::uint8_t> header;
  const char magic[8] = {'C', 'O', 'B', 'S', 'T', 'O', 'R', 'E'};
  for (std::size_t i = 0; i < 8U; ++i) {
    header.push_back(static_cast<std::uint8_t>(magic_ok ? magic[i] : 'X'));
  }
  header.push_back(static_cast<std::uint8_t>(version & 0xFFU));
  header.push_back(static_cast<std::uint8_t>((version >> 8U) & 0xFFU));
  header.push_back(0);
  header.push_back(0);
  std::uint32_t crc = crc32c_bytes(header.data(), header.size());
  if (!crc_ok) {
    crc ^= 0xFFFFFFFFU;
  }
  append_u32(header, crc);
  return header;
}

std::vector<std::uint8_t> make_frame(const std::string& payload, bool crc_ok) {
  std::vector<std::uint8_t> frame;
  append_u32(frame, static_cast<std::uint32_t>(payload.size()));
  std::uint32_t crc = crc32c(payload);
  if (!crc_ok) {
    crc ^= 0xFFFFFFFFU;
  }
  append_u32(frame, crc);
  for (const char ch : payload) {
    frame.push_back(static_cast<std::uint8_t>(ch));
  }
  return frame;
}

// ---------------------------------------------------------------------------
// Evidence helpers.
// ---------------------------------------------------------------------------
ScopePath scope_of(const std::string& text) {
  Result<ScopePath> parsed = ScopePath::parse(text);
  if (!parsed.ok()) {
    CO_FAIL("could not parse scope '" + text + "': " + parsed.status().render());
  }
  return std::move(parsed).value();
}

EvidenceRecord make_record(const std::string& authority, AuthorityRole role, const std::string& scope_text,
                           CapacityAssertion assertion, std::int64_t declared, std::uint64_t generation,
                           std::uint64_t epoch, const std::string& source) {
  Result<EvidenceRecord> built =
      EvidenceRecord::make(AuthorityId(authority), role, scope_of(scope_text), Dimension::Power, assertion,
                           Unit::canonical(Dimension::Power), declared, Generation(generation), Epoch(epoch),
                           Revision(1));
  if (!built.ok()) {
    CO_FAIL("could not build an evidence record: " + built.status().render());
  }
  EvidenceRecord record = std::move(built).value();
  record.provenance = Provenance::Observed;
  record.has_observed_at = true;
  record.observed_at = Timestamp(kObservedAt);
  record.source = source;
  return record;
}

EvidenceRecord installed_record(std::uint64_t generation, std::int64_t declared, const std::string& source) {
  return make_record("dfi", AuthorityRole::InstalledInventory, "site=dc1/hall=h1", CapacityAssertion::Installed,
                     declared, generation, 1, source);
}

MutationId mutation_of(const std::string& text) {
  Result<MutationId> parsed = MutationId::parse(text);
  if (!parsed.ok()) {
    CO_FAIL("could not parse mutation '" + text + "': " + parsed.status().render());
  }
  return std::move(parsed).value();
}

bool same_record(const EvidenceRecord& a, const EvidenceRecord& b) {
  return a.authority == b.authority && a.role == b.role && a.scope == b.scope && a.dimension == b.dimension &&
         a.assertion == b.assertion && a.unit == b.unit && a.declared_amount == b.declared_amount &&
         a.amount == b.amount && a.generation == b.generation && a.epoch == b.epoch && a.revision == b.revision &&
         a.provenance == b.provenance && a.has_observed_at == b.has_observed_at && a.observed_at == b.observed_at &&
         a.validity.from == b.validity.from && a.validity.until == b.validity.until && a.source == b.source;
}

StoreOptions options_for(const std::string& directory) {
  StoreOptions options;
  options.directory = directory;
  return options;
}

// Appends four records of one slot and returns them in commit order.
std::vector<EvidenceRecord> append_four(Store& store) {
  std::vector<EvidenceRecord> records;
  for (std::uint64_t i = 1; i <= 4U; ++i) {
    records.push_back(installed_record(i, static_cast<std::int64_t>(i) * 1000000, "fixture:row" + std::to_string(i)));
    Result<AppendOutcome> appended = store.append_evidence(records.back(), mutation_of("m-" + std::to_string(i)));
    if (!appended.ok()) {
      CO_FAIL("append " + std::to_string(i) + " failed: " + appended.status().render());
    }
    if (!appended.value().applied) {
      CO_FAIL("append " + std::to_string(i) + " was not applied");
    }
  }
  return records;
}

}  // namespace

// ---------------------------------------------------------------------------
// Format and framing
// ---------------------------------------------------------------------------
CO_TEST(store_log_begins_with_an_integrity_checked_header) {
  test::ScratchDirectory scratch("persist-header");
  const StoreOptions options = options_for(scratch.path());
  const std::string log_path = io::join(scratch.path(), "evidence.log");
  CO_REQUIRE(!io::exists(log_path));

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.is_open());
    CO_REQUIRE(store.session_epoch() == Epoch(1));
  }

  const std::vector<std::uint8_t> bytes = read_bytes(log_path);
  CO_REQUIRE(bytes.size() > kHeaderBytes);
  CO_REQUIRE(std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) == 0);
  CO_REQUIRE_EQ(static_cast<std::uint16_t>(bytes[8] | (static_cast<std::uint16_t>(bytes[9]) << 8U)), kFormatVersion);
  CO_REQUIRE_EQ(bytes[10], static_cast<std::uint8_t>(0));
  CO_REQUIRE_EQ(bytes[11], static_cast<std::uint8_t>(0));
  CO_REQUIRE_EQ(crc32c_bytes(bytes.data(), 12U), load_u32(bytes.data() + 12U));

  const LogView view = view_log(bytes);
  CO_REQUIRE(view.header_ok());
  CO_REQUIRE_EQ(view.version, kFormatVersion);
  CO_REQUIRE_EQ(view.framed_end, bytes.size());
  CO_REQUIRE_EQ(view.frames.size(), 1U);
  CO_REQUIRE_EQ(frame_kind(view.frames[0]), std::string("epoch"));
  CO_REQUIRE_EQ(frame_int(view.frames[0], "epoch"), static_cast<std::int64_t>(1));
  CO_REQUIRE_EQ(frame_int(view.frames[0], "seq"), static_cast<std::int64_t>(1));
}

CO_TEST(every_frame_is_length_and_crc32c_framed) {
  test::ScratchDirectory scratch("persist-framing");
  const StoreOptions options = options_for(scratch.path());
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  std::vector<EvidenceRecord> written;
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    written = append_four(store);
    CO_REQUIRE(store.last_sequence() == SequenceNumber(5));
    CO_REQUIRE_EQ(store.mutations().size(), 4U);
  }

  const std::vector<std::uint8_t> bytes = read_bytes(log_path);
  const LogView view = view_log(bytes);
  CO_REQUIRE(view.header_ok());
  // One epoch frame plus four evidence frames, with nothing after the last one.
  CO_REQUIRE_EQ(view.frames.size(), 5U);
  CO_REQUIRE_EQ(view.framed_end, bytes.size());

  std::int64_t previous_sequence = 0;
  for (const FrameView& frame : view.frames) {
    CO_REQUIRE(frame.crc_ok);
    CO_REQUIRE_EQ(frame.declared_crc, frame.computed_crc);
    CO_REQUIRE_EQ(static_cast<std::size_t>(frame.declared_size), frame.payload.size());
    CO_REQUIRE(frame.declared_size > 0U);
    CO_REQUIRE(!frame_kind(frame).empty());
    const std::int64_t sequence = frame_int(frame, "seq");
    CO_REQUIRE(sequence > previous_sequence);
    previous_sequence = sequence;
    CO_REQUIRE(frame_int(frame, "epoch") >= 1);
    CO_REQUIRE_EQ(frame_int(frame, "v"), static_cast<std::int64_t>(kFormatVersion));
  }

  CO_REQUIRE_EQ(frame_kind(view.frames[0]), std::string("epoch"));
  for (std::size_t i = 1; i < view.frames.size(); ++i) {
    const FrameView& frame = view.frames[i];
    CO_REQUIRE_EQ(frame_kind(frame), std::string("evidence"));
    CO_REQUIRE_EQ(frame_string(frame, "mutation"), std::string("m-") + std::to_string(i));
    CO_REQUIRE_EQ(frame_string(frame, "digest"), written[i - 1U].content_digest().hex());
    const Result<json::Value> parsed = json::Value::parse(frame.payload);
    CO_REQUIRE(parsed.ok());
    const json::Value* record = parsed.ok() ? parsed.value().find("record") : nullptr;
    CO_REQUIRE(record != nullptr);
    CO_REQUIRE(record != nullptr && record->is_object());
    CO_REQUIRE(record != nullptr && record->require_string("authority").ok());
  }

  // The frames tile the file exactly: header, then length+payload back to back.
  std::size_t expected_offset = kHeaderBytes;
  for (const FrameView& frame : view.frames) {
    CO_REQUIRE_EQ(frame.offset, expected_offset);
    expected_offset += kFrameHeaderBytes + static_cast<std::size_t>(frame.declared_size);
  }
  CO_REQUIRE_EQ(expected_offset, bytes.size());
}

CO_TEST(hand_built_corrupt_headers_are_rejected) {
  test::ScratchDirectory scratch("persist-headers");

  std::vector<std::uint8_t> truncated_header = make_header(kFormatVersion, true, true);
  truncated_header.resize(10);

  struct Scenario {
    std::string label;
    std::vector<std::uint8_t> bytes;
    ReasonCode expected;
  };

  const std::vector<Scenario> scenarios{
      {"bad-magic", make_header(kFormatVersion, false, true), ReasonCode::BadMagic},
      {"truncated-header", std::move(truncated_header), ReasonCode::BadMagic},
      {"unsupported-version", make_header(99, true, true), ReasonCode::UnsupportedVersion},
      {"header-crc", make_header(kFormatVersion, true, false), ReasonCode::IntegrityMismatch},
  };

  for (const Scenario& scenario : scenarios) {
    const std::string directory = scratch.child(scenario.label);
    CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
    const std::string log_path = io::join(directory, "evidence.log");
    replace_bytes(log_path, scenario.bytes);

    const StoreOptions options = options_for(directory);
    CO_REQUIRE_ERR(Store::open(options), scenario.expected);
    CO_REQUIRE_ERR(inspect_store(options), scenario.expected);

    StoreOptions read_only = options;
    read_only.read_only = true;
    CO_REQUIRE_ERR(Store::open(read_only), scenario.expected);

    // A rejected header is never repaired behind the caller's back.
    CO_REQUIRE(same_bytes(read_bytes(log_path), scenario.bytes));
  }

  // The same corruption applied to a real, previously valid store.
  const std::string real_directory = scratch.child("real");
  CO_REQUIRE_OK_VOID(io::ensure_directory(real_directory));
  const StoreOptions real_options = options_for(real_directory);
  {
    CO_REQUIRE_STORE(Store::open(real_options), store);
    CO_REQUIRE_EQ(append_four(store).size(), 4U);
  }
  const std::string real_log = io::join(real_directory, "evidence.log");
  const std::vector<std::uint8_t> good = read_bytes(real_log);
  CO_REQUIRE(view_log(good).header_ok());

  patch_byte(real_log, 0, static_cast<std::uint8_t>('X'));
  CO_REQUIRE_ERR(Store::open(real_options), ReasonCode::BadMagic);
  CO_REQUIRE_ERR(inspect_store(real_options), ReasonCode::BadMagic);

  replace_bytes(real_log, good);
  CO_REQUIRE(view_log(read_bytes(real_log)).header_ok());
  patch_byte(real_log, 8, static_cast<std::uint8_t>(7));
  CO_REQUIRE_ERR(Store::open(real_options), ReasonCode::UnsupportedVersion);
  CO_REQUIRE_ERR(inspect_store(real_options), ReasonCode::UnsupportedVersion);

  replace_bytes(real_log, good);
  const std::uint8_t header_crc_byte = read_bytes(real_log)[14];
  patch_byte(real_log, 14, static_cast<std::uint8_t>(header_crc_byte ^ 0xFFU));
  CO_REQUIRE_ERR(Store::open(real_options), ReasonCode::IntegrityMismatch);
  CO_REQUIRE_ERR(inspect_store(real_options), ReasonCode::IntegrityMismatch);
}

// ---------------------------------------------------------------------------
// Torn tails
// ---------------------------------------------------------------------------
CO_TEST(torn_tail_partial_payload_is_discarded_and_truncated_in_place) {
  test::ScratchDirectory scratch("persist-torn-payload");
  const StoreOptions options = options_for(scratch.path());
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  std::vector<EvidenceRecord> written;
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    written = append_four(store);
  }

  const std::vector<std::uint8_t> clean = read_bytes(log_path);
  const LogView clean_view = view_log(clean);
  CO_REQUIRE(clean_view.header_ok());
  CO_REQUIRE_EQ(clean_view.frames.size(), 5U);
  CO_REQUIRE_EQ(clean_view.framed_end, clean.size());

  // A frame whose length claims far more bytes than the file holds: the classic
  // partially written tail. The CRC written is the CRC of the full claimed
  // payload, so only the length check can classify it.
  const std::string claimed_payload(4096, 'x');
  std::vector<std::uint8_t> torn;
  append_u32(torn, static_cast<std::uint32_t>(claimed_payload.size()));
  append_u32(torn, crc32c(claimed_payload));
  const std::uint8_t* claimed_bytes = reinterpret_cast<const std::uint8_t*>(claimed_payload.data());
  torn.insert(torn.end(), claimed_bytes, claimed_bytes + 64);
  append_bytes_at_end(log_path, torn);

  const std::vector<std::uint8_t> damaged = read_bytes(log_path);
  CO_REQUIRE_EQ(damaged.size(), clean.size() + torn.size());
  CO_REQUIRE_EQ(view_log(damaged).framed_end, clean.size());

  // A read-only open reports the torn tail and modifies nothing.
  StoreOptions read_only = options;
  read_only.read_only = true;
  const std::uint64_t size_before_read_only = size_of(log_path);
  {
    CO_REQUIRE_STORE(Store::open(read_only), reader);
    CO_REQUIRE(reader.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(reader.recovery().bytes_discarded, static_cast<std::uint64_t>(torn.size()));
    CO_REQUIRE_EQ(reader.recovery().frames_read, 5U);
    CO_REQUIRE_EQ(reader.recovery().evidence_frames, 4U);
    CO_REQUIRE(reader.recovery().explanation.find("truncated at the last complete frame") != std::string::npos);
    CO_REQUIRE_OK(reader.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 4U);
    for (std::size_t i = 0; i < loaded.size(); ++i) {
      CO_REQUIRE(same_record(loaded[i].record, written[i]));
      CO_REQUIRE(loaded[i].record.content_digest() == written[i].content_digest());
      CO_REQUIRE(loaded[i].mutation == mutation_of("m-" + std::to_string(i + 1U)));
      CO_REQUIRE(loaded[i].sequence == SequenceNumber(i + 2U));
      CO_REQUIRE(loaded[i].written_epoch == Epoch(1));
    }
  }
  CO_REQUIRE_EQ(size_of(log_path), size_before_read_only);
  CO_REQUIRE(same_bytes(read_bytes(log_path), damaged));

  // A writable open discards the torn tail conservatively and exactly.
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(store.recovery().bytes_discarded, static_cast<std::uint64_t>(torn.size()));
    CO_REQUIRE_EQ(store.recovery().frames_read, 5U);
    CO_REQUIRE(store.session_epoch() == Epoch(2));
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 4U);
    for (std::size_t i = 0; i < loaded.size(); ++i) {
      CO_REQUIRE(same_record(loaded[i].record, written[i]));
    }
  }

  // The file now ends at the last complete frame plus this session's epoch
  // frame; the torn bytes are gone.
  const std::vector<std::uint8_t> after = read_bytes(log_path);
  CO_REQUIRE(after.size() < damaged.size());
  CO_REQUIRE(after.size() > clean.size());
  CO_REQUIRE(prefix_matches(after, clean, clean.size()));
  const LogView after_view = view_log(after);
  CO_REQUIRE_EQ(after_view.frames.size(), 6U);
  CO_REQUIRE_EQ(after_view.framed_end, after.size());
  CO_REQUIRE_EQ(frame_kind(after_view.frames.back()), std::string("epoch"));
  CO_REQUIRE_EQ(frame_int(after_view.frames.back(), "epoch"), static_cast<std::int64_t>(2));

  // The recovered file is clean again.
  CO_REQUIRE_OK(inspect_store(options), report);
  CO_REQUIRE(!report.torn_tail_recovered);
  CO_REQUIRE_EQ(report.bytes_discarded, 0U);
  CO_REQUIRE_EQ(report.frames_read, 6U);
}

CO_TEST(torn_tail_partial_frame_header_is_discarded) {
  test::ScratchDirectory scratch("persist-torn-header");
  const StoreOptions options = options_for(scratch.path());
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  std::vector<EvidenceRecord> written;
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    written = append_four(store);
  }
  const std::vector<std::uint8_t> clean = read_bytes(log_path);
  const std::vector<std::uint8_t> tail{0x40U, 0x00U, 0x00U};
  append_bytes_at_end(log_path, tail);

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(store.recovery().bytes_discarded, static_cast<std::uint64_t>(tail.size()));
    CO_REQUIRE_EQ(store.recovery().frames_read, 5U);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 4U);
    for (std::size_t i = 0; i < loaded.size(); ++i) {
      CO_REQUIRE(same_record(loaded[i].record, written[i]));
    }
  }

  const std::vector<std::uint8_t> after = read_bytes(log_path);
  CO_REQUIRE(prefix_matches(after, clean, clean.size()));
  CO_REQUIRE_EQ(view_log(after).frames.size(), 6U);
  CO_REQUIRE_EQ(view_log(after).framed_end, after.size());
}

CO_TEST(torn_tail_wrong_crc_at_the_very_end_is_discarded) {
  test::ScratchDirectory scratch("persist-torn-crc");
  const StoreOptions options = options_for(scratch.path());
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  std::vector<EvidenceRecord> written;
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    written = append_four(store);
  }
  const std::vector<std::uint8_t> clean = read_bytes(log_path);

  // A complete frame of the right shape whose CRC does not match its payload:
  // what a torn write of a durable append looks like when the payload landed
  // but the frame was never committed in full.
  const json::Object members{{"v", json::Value(static_cast<std::int64_t>(kFormatVersion))},
                             {"kind", json::Value(std::string("evidence"))},
                             {"seq", json::Value(static_cast<std::int64_t>(99))},
                             {"epoch", json::Value(static_cast<std::int64_t>(1))},
                             {"mutation", json::Value(std::string("m-torn"))},
                             {"digest", json::Value(written.back().content_digest().hex())}};
  const std::string payload = json::dump(json::Value::object(members), true, -1);
  const std::vector<std::uint8_t> torn = make_frame(payload, false);
  append_bytes_at_end(log_path, torn);

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(store.recovery().bytes_discarded, static_cast<std::uint64_t>(torn.size()));
    CO_REQUIRE_EQ(store.recovery().frames_read, 5U);
    // The uncommitted mutation identity is not durable and is not remembered.
    CO_REQUIRE(store.mutations().find(mutation_of("m-torn")) == store.mutations().end());
    CO_REQUIRE_EQ(store.mutations().size(), 4U);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 4U);
    for (std::size_t i = 0; i < loaded.size(); ++i) {
      CO_REQUIRE(same_record(loaded[i].record, written[i]));
    }
  }

  const std::vector<std::uint8_t> after = read_bytes(log_path);
  CO_REQUIRE(prefix_matches(after, clean, clean.size()));
  CO_REQUIRE_EQ(view_log(after).frames.size(), 6U);
  CO_REQUIRE_EQ(view_log(after).framed_end, after.size());
}

CO_TEST(read_only_open_never_modifies_a_damaged_file) {
  test::ScratchDirectory scratch("persist-readonly");
  const StoreOptions options = options_for(scratch.path());
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_EQ(append_four(store).size(), 4U);
  }
  const std::vector<std::uint8_t> clean = read_bytes(log_path);
  const std::vector<std::uint8_t> torn = make_frame(std::string(512, 'y'), false);
  append_bytes_at_end(log_path, torn);
  const std::vector<std::uint8_t> damaged = read_bytes(log_path);
  const std::uint64_t size_before = size_of(log_path);

  for (int attempt = 0; attempt < 3; ++attempt) {
    StoreOptions read_only = options;
    read_only.read_only = true;
    CO_REQUIRE_STORE(Store::open(read_only), reader);
    CO_REQUIRE(reader.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(reader.recovery().frames_read, 5U);
    CO_REQUIRE_EQ(size_of(log_path), size_before);
    CO_REQUIRE(same_bytes(read_bytes(log_path), damaged));
    reader.close();
  }

  CO_REQUIRE_OK(inspect_store(options), inspected);
  CO_REQUIRE(inspected.torn_tail_recovered);
  CO_REQUIRE_EQ(inspected.frames_read, 5U);
  CO_REQUIRE_EQ(size_of(log_path), size_before);
  CO_REQUIRE(same_bytes(read_bytes(log_path), damaged));
  CO_REQUIRE(prefix_matches(damaged, clean, clean.size()));
}

// ---------------------------------------------------------------------------
// Interior corruption
// ---------------------------------------------------------------------------
CO_TEST(interior_corruption_is_rejected_by_open_and_by_inspect) {
  test::ScratchDirectory scratch("persist-interior");
  const StoreOptions options = options_for(scratch.path());
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  std::vector<EvidenceRecord> written;
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    written = append_four(store);
  }
  const std::vector<std::uint8_t> clean = read_bytes(log_path);
  const LogView clean_view = view_log(clean);
  CO_REQUIRE_EQ(clean_view.frames.size(), 5U);

  // Damage the payload of the second evidence frame; valid frames follow it.
  const FrameView& victim = clean_view.frames[2];
  CO_REQUIRE_EQ(frame_kind(victim), std::string("evidence"));
  std::vector<std::uint8_t> damaged = clean;
  const std::size_t payload_offset = victim.offset + kFrameHeaderBytes + 12U;
  damaged[payload_offset] = static_cast<std::uint8_t>(damaged[payload_offset] ^ 0x5AU);
  replace_bytes(log_path, damaged);

  const LogView damaged_view = view_log(damaged);
  CO_REQUIRE_EQ(damaged_view.frames.size(), 3U);
  CO_REQUIRE(!damaged_view.frames.back().crc_ok);

  CO_REQUIRE_ERR(Store::open(options), ReasonCode::InteriorCorruption);
  CO_REQUIRE_ERR(inspect_store(options), ReasonCode::InteriorCorruption);

  StoreOptions read_only = options;
  read_only.read_only = true;
  CO_REQUIRE_ERR(Store::open(read_only), ReasonCode::InteriorCorruption);

  // The rejection is total: no later record was silently dropped, and the file
  // was left exactly as it was found.
  CO_REQUIRE(same_bytes(read_bytes(log_path), damaged));
  const std::size_t first_later_frame = clean_view.frames[3].offset;
  CO_REQUIRE(std::equal(damaged.begin() + static_cast<std::ptrdiff_t>(first_later_frame), damaged.end(),
                        clean.begin() + static_cast<std::ptrdiff_t>(first_later_frame)));
  for (std::size_t i = 3; i < clean_view.frames.size(); ++i) {
    const FrameView& frame = clean_view.frames[i];
    CO_REQUIRE(frame.crc_ok);
    CO_REQUIRE_EQ(frame_string(frame, "mutation"), std::string("m-") + std::to_string(i));
  }
  CO_REQUIRE_EQ(written.size(), 4U);
}

// ---------------------------------------------------------------------------
// Commit point and idempotency
// ---------------------------------------------------------------------------
CO_TEST(committed_records_are_durable_and_failed_appends_leave_no_trace) {
  test::ScratchDirectory scratch("persist-commit");
  const StoreOptions options = options_for(scratch.path());
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  std::vector<EvidenceRecord> written;
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    written = append_four(store);
    const std::vector<std::uint8_t> before = read_bytes(log_path);
    const std::size_t frames_before = view_log(before).frames.size();

    // A structurally invalid record is refused before anything is written.
    EvidenceRecord invalid = installed_record(9, 1000, "fixture:invalid");
    invalid.authority = AuthorityId();
    CO_REQUIRE_ERR(store.append_evidence(invalid, mutation_of("m-invalid")), ReasonCode::MalformedEvidence);

    // An empty mutation identity is refused.
    CO_REQUIRE_ERR(store.append_evidence(installed_record(9, 1000, "fixture:empty"), MutationId()),
                   ReasonCode::InvalidIdentifier);
    CO_REQUIRE_ERR(store.append_idempotent_marker(MutationId(), written[0].content_digest()),
                   ReasonCode::InvalidIdentifier);

    CO_REQUIRE(same_bytes(read_bytes(log_path), before));
    CO_REQUIRE_EQ(view_log(read_bytes(log_path)).frames.size(), frames_before);
    CO_REQUIRE(store.mutations().find(mutation_of("m-invalid")) == store.mutations().end());
    CO_REQUIRE_EQ(store.mutations().size(), 4U);
    CO_REQUIRE(store.last_sequence() == SequenceNumber(5));

    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 4U);
    CO_REQUIRE_OK(store.durable_bytes(), durable);
    CO_REQUIRE_EQ(durable, size_of(log_path));
  }

  // Every committed record survived the close and reopens byte-identically.
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 4U);
    for (std::size_t i = 0; i < loaded.size(); ++i) {
      CO_REQUIRE(same_record(loaded[i].record, written[i]));
      CO_REQUIRE(loaded[i].record.content_digest() == written[i].content_digest());
    }
  }

  // A frame count is a stronger statement than a record count: the refused
  // appends left no frames at all behind the four committed ones.
  const LogView view = view_log(read_bytes(log_path));
  CO_REQUIRE_EQ(view.frames.size(), 6U);
  CO_REQUIRE_EQ(view.framed_end, read_bytes(log_path).size());
}

CO_TEST(idempotent_replay_appends_no_second_frame) {
  test::ScratchDirectory scratch("persist-idempotent");
  const StoreOptions options = options_for(scratch.path());
  const std::string log_path = io::join(scratch.path(), "evidence.log");
  const EvidenceRecord record = installed_record(1, 4000000, "fixture:replay");
  const MutationId mutation = mutation_of("m-replay");

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_OK(store.append_evidence(record, mutation), first);
    CO_REQUIRE(first.applied);
    CO_REQUIRE_EQ(first.reason, ReasonCode::Ok);
    CO_REQUIRE(first.mutation_digest == record.content_digest());
    CO_REQUIRE(first.sequence == SequenceNumber(2));

    const std::vector<std::uint8_t> before = read_bytes(log_path);
    const std::size_t frames_before = view_log(before).frames.size();
    CO_REQUIRE_EQ(frames_before, 2U);

    CO_REQUIRE_OK(store.append_evidence(record, mutation), replay);
    CO_REQUIRE(!replay.applied);
    CO_REQUIRE_EQ(replay.reason, ReasonCode::IdempotentReplay);
    CO_REQUIRE(replay.mutation_digest == record.content_digest());
    CO_REQUIRE(store.last_sequence() == SequenceNumber(2));
    CO_REQUIRE_EQ(store.mutations().size(), 1U);
    const std::vector<std::uint8_t> after = read_bytes(log_path);
    CO_REQUIRE(same_bytes(after, before));
    CO_REQUIRE_EQ(view_log(after).frames.size(), frames_before);
    CO_REQUIRE_EQ(size_of(log_path), static_cast<std::uint64_t>(before.size()));

    // The same identity with different content is a conflict, not a replay.
    const EvidenceRecord different = installed_record(2, 4000000, "fixture:replay");
    CO_REQUIRE_ERR(store.append_evidence(different, mutation), ReasonCode::IdempotencyConflict);
    CO_REQUIRE(same_bytes(read_bytes(log_path), before));

    // Markers share the same identity space.
    const Digest marker_digest = sha256("marker-payload");
    CO_REQUIRE_OK(store.append_idempotent_marker(mutation_of("m-marker"), marker_digest), marker);
    CO_REQUIRE(marker.applied);
    CO_REQUIRE_OK(store.append_idempotent_marker(mutation_of("m-marker"), marker_digest), marker_replay);
    CO_REQUIRE_EQ(marker_replay.reason, ReasonCode::IdempotentReplay);
    CO_REQUIRE_ERR(store.append_idempotent_marker(mutation_of("m-marker"), sha256("other")),
                   ReasonCode::IdempotencyConflict);
  }

  // Durable, not in-memory: a fresh open still recognises the identity.
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_EQ(store.mutations().size(), 2U);
    CO_REQUIRE(store.mutations().at(mutation) == record.content_digest());
    const std::size_t frames_after_open = view_log(read_bytes(log_path)).frames.size();
    CO_REQUIRE_OK(store.append_evidence(record, mutation), replay);
    CO_REQUIRE_EQ(replay.reason, ReasonCode::IdempotentReplay);
    CO_REQUIRE(!replay.applied);
    CO_REQUIRE_EQ(view_log(read_bytes(log_path)).frames.size(), frames_after_open);
    CO_REQUIRE_ERR(store.append_evidence(installed_record(7, 7, "fixture:replay"), mutation),
                   ReasonCode::IdempotencyConflict);
    CO_REQUIRE_EQ(view_log(read_bytes(log_path)).frames.size(), frames_after_open);
  }
}

// ---------------------------------------------------------------------------
// Session epochs
// ---------------------------------------------------------------------------
CO_TEST(session_epoch_advances_and_is_recorded_in_the_log) {
  test::ScratchDirectory scratch("persist-epoch");
  const StoreOptions options = options_for(scratch.path());
  const std::string log_path = io::join(scratch.path(), "evidence.log");
  const std::string epoch_path = io::join(scratch.path(), "epoch");

  for (std::uint64_t session = 1; session <= 3U; ++session) {
    std::vector<std::uint8_t> before_open;
    if (session > 1U) {
      before_open = read_bytes(log_path);
    }
    {
      CO_REQUIRE_STORE(Store::open(options), store);
      CO_REQUIRE(store.session_epoch() == Epoch(session));
      CO_REQUIRE(store.recovery().session_epoch == Epoch(session));
      CO_REQUIRE(store.recovery().epoch_advanced);

      if (session == 2U) {
        CO_REQUIRE_OK(store.append_evidence(installed_record(1, 250000, "fixture:epoch"), mutation_of("m-epoch")),
                      outcome);
        CO_REQUIRE(outcome.applied);
      }
    }

    // The epoch file is a 12 byte integrity checked record, published atomically.
    const std::vector<std::uint8_t> epoch_bytes = read_bytes(epoch_path);
    CO_REQUIRE_EQ(epoch_bytes.size(), 12U);
    CO_REQUIRE_EQ(load_u64(epoch_bytes.data()), session);
    CO_REQUIRE_EQ(crc32c_bytes(epoch_bytes.data(), 8U), load_u32(epoch_bytes.data() + 8U));
    CO_REQUIRE(!io::exists(io::temp_sibling(epoch_path, "publish")));

    // The newest epoch frame in the log is this session's epoch record.
    const std::vector<std::uint8_t> now = read_bytes(log_path);
    const LogView view = view_log(now);
    CO_REQUIRE_EQ(view.framed_end, now.size());
    const FrameView* last_epoch = nullptr;
    for (const FrameView& frame : view.frames) {
      if (frame_kind(frame) == "epoch") {
        last_epoch = &frame;
      }
    }
    CO_REQUIRE(last_epoch != nullptr);
    CO_REQUIRE_EQ(frame_int(*last_epoch, "epoch"), static_cast<std::int64_t>(session));
    if (session > 1U) {
      CO_REQUIRE(prefix_matches(now, before_open, before_open.size()));
    }

    CO_REQUIRE_OK(inspect_store(options), report);
    CO_REQUIRE(!report.torn_tail_recovered);
    CO_REQUIRE_EQ(report.epoch_frames, session);
  }

  // Evidence written in session 2 carries session epoch 2 in its frame.
  const LogView view = view_log(read_bytes(log_path));
  bool saw_evidence = false;
  for (const FrameView& frame : view.frames) {
    if (frame_kind(frame) == "evidence") {
      saw_evidence = true;
      CO_REQUIRE_EQ(frame_int(frame, "epoch"), static_cast<std::int64_t>(2));
    }
  }
  CO_REQUIRE(saw_evidence);
}

CO_TEST(a_corrupt_epoch_file_is_rejected_with_integrity_mismatch) {
  test::ScratchDirectory scratch("persist-epoch-corrupt");
  const StoreOptions options = options_for(scratch.path());
  const std::string epoch_path = io::join(scratch.path(), "epoch");
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_EQ(append_four(store).size(), 4U);
  }
  const std::vector<std::uint8_t> good_epoch = read_bytes(epoch_path);
  CO_REQUIRE_EQ(good_epoch.size(), 12U);
  const std::vector<std::uint8_t> good_log = read_bytes(log_path);

  // Truncated epoch file.
  replace_bytes(epoch_path, std::vector<std::uint8_t>(good_epoch.begin(), good_epoch.begin() + 8));
  CO_REQUIRE_ERR(Store::open(options), ReasonCode::IntegrityMismatch);
  CO_REQUIRE_EQ(read_bytes(epoch_path).size(), 8U);

  // Corrupt CRC.
  std::vector<std::uint8_t> bad_crc = good_epoch;
  bad_crc[11] = static_cast<std::uint8_t>(bad_crc[11] ^ 0xFFU);
  replace_bytes(epoch_path, bad_crc);
  CO_REQUIRE_ERR(Store::open(options), ReasonCode::IntegrityMismatch);

  // Corrupt epoch value.
  std::vector<std::uint8_t> bad_value = good_epoch;
  bad_value[0] = static_cast<std::uint8_t>(bad_value[0] ^ 0x01U);
  replace_bytes(epoch_path, bad_value);
  CO_REQUIRE_ERR(Store::open(options), ReasonCode::IntegrityMismatch);

  // Empty epoch file.
  replace_bytes(epoch_path, std::vector<std::uint8_t>());
  CO_REQUIRE_ERR(Store::open(options), ReasonCode::IntegrityMismatch);

  // A rejected epoch never damages the log: all four records survive intact.
  CO_REQUIRE(same_bytes(read_bytes(log_path), good_log));

  // Restoring a valid epoch file recovers, and the session epoch continues from
  // the durable counter rather than restarting.
  replace_bytes(epoch_path, good_epoch);
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.session_epoch() == Epoch(2));
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 4U);
  }
}

// ---------------------------------------------------------------------------
// Snapshot publication
// ---------------------------------------------------------------------------
CO_TEST(snapshot_publication_is_atomic_and_digest_recorded) {
  test::ScratchDirectory scratch("persist-snapshot");
  const StoreOptions options = options_for(scratch.path());
  const std::string snapshot_path = io::join(scratch.path(), "snapshot.json");
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  const std::string document_one = "{\"snapshot\":1,\"power_mw\":4000}";
  const std::string document_two = "{\"snapshot\":2,\"power_mw\":2500,\"note\":\"second\"}";

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(!io::exists(snapshot_path));
    CO_REQUIRE_OK(store.read_snapshot(), empty_snapshot);
    CO_REQUIRE(!empty_snapshot.has_value());

    CO_REQUIRE_OK(store.publish_snapshot(document_one), digest_one);
    CO_REQUIRE(digest_one == sha256(document_one));
    CO_REQUIRE(io::exists(snapshot_path));
    CO_REQUIRE(!io::exists(io::temp_sibling(snapshot_path, "publish")));
    CO_REQUIRE_EQ(size_of(snapshot_path), static_cast<std::uint64_t>(document_one.size()));
    CO_REQUIRE(same_bytes(read_bytes(snapshot_path), bytes_of(document_one)));
    CO_REQUIRE_OK(store.read_snapshot(), read_one);
    CO_REQUIRE(read_one.has_value());
    CO_REQUIRE_EQ(read_one.value(), document_one);

    CO_REQUIRE_OK(store.publish_snapshot(document_two), digest_two);
    CO_REQUIRE(digest_two == sha256(document_two));
    CO_REQUIRE_OK(store.read_snapshot(), read_two);
    CO_REQUIRE(read_two.has_value());
    CO_REQUIRE_EQ(read_two.value(), document_two);

    const LogView view = view_log(read_bytes(log_path));
    CO_REQUIRE_EQ(frame_kind(view.frames[0]), std::string("epoch"));
    CO_REQUIRE_EQ(frame_kind(view.frames[1]), std::string("snapshot"));
    CO_REQUIRE_EQ(frame_string(view.frames[1], "digest"), digest_one.hex());
    CO_REQUIRE_EQ(frame_int(view.frames[1], "bytes"), static_cast<std::int64_t>(document_one.size()));
    CO_REQUIRE_EQ(frame_kind(view.frames[2]), std::string("snapshot"));
    CO_REQUIRE_EQ(frame_string(view.frames[2], "digest"), digest_two.hex());
    CO_REQUIRE_EQ(frame_int(view.frames[2], "bytes"), static_cast<std::int64_t>(document_two.size()));
    CO_REQUIRE_EQ(view.framed_end, read_bytes(log_path).size());
  }

  // The published file survives a reopen and is still complete.
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_OK(store.read_snapshot(), read_after_reopen);
    CO_REQUIRE(read_after_reopen.has_value());
    CO_REQUIRE_EQ(read_after_reopen.value(), document_two);
    CO_REQUIRE_EQ(size_of(snapshot_path), static_cast<std::uint64_t>(document_two.size()));
  }
}

CO_TEST(a_stale_publish_temp_never_becomes_the_published_snapshot) {
  test::ScratchDirectory scratch("persist-snapshot-tmp");
  const StoreOptions options = options_for(scratch.path());
  const std::string snapshot_path = io::join(scratch.path(), "snapshot.json");
  const std::string temp_path = io::temp_sibling(snapshot_path, "publish");

  const std::string document_one = "{\"snapshot\":\"complete-one\"}";
  const std::string document_two = "{\"snapshot\":\"complete-two\"}";

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_OK(store.publish_snapshot(document_one), digest_one);
    CO_REQUIRE(digest_one == sha256(document_one));
    CO_REQUIRE(!io::exists(temp_path));

    // A writer that died mid-publication leaves a partial temp sibling behind.
    const std::string partial = "{\"snapshot\":\"complete-tw";
    replace_bytes(temp_path, bytes_of(partial));
    CO_REQUIRE(io::exists(temp_path));

    // The temp sibling is not the published file: readers see the old document.
    CO_REQUIRE_OK(store.read_snapshot(), still_one);
    CO_REQUIRE(still_one.has_value());
    CO_REQUIRE_EQ(still_one.value(), document_one);
    CO_REQUIRE_EQ(size_of(snapshot_path), static_cast<std::uint64_t>(document_one.size()));
  }

  // A fresh open after the crash still sees only the complete old document.
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_OK(store.read_snapshot(), still_one);
    CO_REQUIRE(still_one.has_value());
    CO_REQUIRE_EQ(still_one.value(), document_one);
    CO_REQUIRE(io::exists(temp_path));

    // The next real publication replaces the published file and consumes the
    // stale temp sibling.
    CO_REQUIRE_OK(store.publish_snapshot(document_two), digest_two);
    CO_REQUIRE(digest_two == sha256(document_two));
    CO_REQUIRE(!io::exists(temp_path));
    CO_REQUIRE_OK(store.read_snapshot(), now_two);
    CO_REQUIRE(now_two.has_value());
    CO_REQUIRE_EQ(now_two.value(), document_two);
    CO_REQUIRE(same_bytes(read_bytes(snapshot_path), bytes_of(document_two)));
  }
}

// ---------------------------------------------------------------------------
// Bounded resources
// ---------------------------------------------------------------------------
CO_TEST(an_oversized_record_is_refused_with_record_too_large) {
  test::ScratchDirectory scratch("persist-too-large");
  StoreOptions options = options_for(scratch.path());
  options.max_frame_bytes = 2048;
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    const std::vector<std::uint8_t> before = read_bytes(log_path);
    const std::size_t frames_before = view_log(before).frames.size();

    const EvidenceRecord huge = installed_record(1, 1000, std::string(4096, 's'));
    CO_REQUIRE_ERR(store.append_evidence(huge, mutation_of("m-huge")), ReasonCode::RecordTooLarge);
    CO_REQUIRE(same_bytes(read_bytes(log_path), before));
    CO_REQUIRE_EQ(view_log(read_bytes(log_path)).frames.size(), frames_before);
    CO_REQUIRE(store.mutations().find(mutation_of("m-huge")) == store.mutations().end());

    // The same identity with a small payload is accepted: only size was refused.
    CO_REQUIRE_OK(store.append_evidence(installed_record(1, 1000, "small"), mutation_of("m-huge")), accepted);
    CO_REQUIRE(accepted.applied);
    CO_REQUIRE_EQ(store.mutations().size(), 1U);

    CO_REQUIRE_ERR(store.publish_snapshot(std::string(4096, 'z')), ReasonCode::RecordTooLarge);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 1U);
  }
}

CO_TEST(appending_past_max_file_bytes_is_refused_with_capacity_exceeded) {
  test::ScratchDirectory scratch("persist-capacity");
  StoreOptions options = options_for(scratch.path());
  options.max_file_bytes = 4096;
  const std::string log_path = io::join(scratch.path(), "evidence.log");

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.durable_bytes().value() < options.max_file_bytes);

    std::size_t applied = 0;
    ReasonCode refusal = ReasonCode::Ok;
    EvidenceRecord refused = installed_record(1, 1, "fixture:capacity");
    for (std::uint64_t i = 1; i <= 200U; ++i) {
      const EvidenceRecord record = installed_record(i, static_cast<std::int64_t>(i) * 1000, "fixture:capacity");
      const Result<AppendOutcome> outcome = store.append_evidence(record, mutation_of("m-cap-" + std::to_string(i)));
      if (!outcome.ok()) {
        refusal = outcome.status().code();
        refused = record;
        break;
      }
      ++applied;
    }

    CO_REQUIRE_EQ(refusal, ReasonCode::CapacityExceeded);
    CO_REQUIRE(applied > 0U);
    CO_REQUIRE(applied < 200U);
    CO_REQUIRE(store.durable_bytes().value() <= options.max_file_bytes);
    CO_REQUIRE_EQ(size_of(log_path), store.durable_bytes().value());

    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), applied);
    for (const StoredEvidence& stored : loaded) {
      CO_REQUIRE(stored.record.content_digest() != refused.content_digest());
    }
    CO_REQUIRE(store.mutations().find(mutation_of("m-cap-" + std::to_string(applied + 1U))) ==
               store.mutations().end());
  }

  // Everything that fit is still readable after a reopen, and the bound holds.
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.durable_bytes().value() <= options.max_file_bytes);
    CO_REQUIRE_EQ(store.recovery().frames_read, 1U + store.mutations().size());
    CO_REQUIRE(store.mutations().size() > 0U);
  }
}

CO_TEST(opening_a_log_larger_than_max_file_bytes_is_refused) {
  test::ScratchDirectory scratch("persist-open-capacity");
  const StoreOptions options = options_for(scratch.path());

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_EQ(append_four(store).size(), 4U);
  }
  const std::uint64_t size = size_of(io::join(scratch.path(), "evidence.log"));
  CO_REQUIRE(size > 256U);

  StoreOptions tiny = options;
  tiny.max_file_bytes = 256;
  CO_REQUIRE_ERR(Store::open(tiny), ReasonCode::CapacityExceeded);

  // A writable open must also fit this session's epoch frame, so a log already
  // at the limit is refused rather than grown past it.
  StoreOptions exact = options;
  exact.max_file_bytes = size;
  CO_REQUIRE_ERR(Store::open(exact), ReasonCode::CapacityExceeded);

  // With room for the epoch frame the same log opens and reads every record.
  StoreOptions headroom = options;
  headroom.max_file_bytes = size + 1024U;
  {
    CO_REQUIRE_STORE(Store::open(headroom), store);
    CO_REQUIRE_EQ(store.recovery().frames_read, 5U);
    CO_REQUIRE(!store.recovery().torn_tail_recovered);
  }
}
