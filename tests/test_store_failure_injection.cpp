// Capacity Observatory - store failure injection suite.
//
// Every case here damages real bytes on disk and then asks the store to open
// them. Damage is injected at frame boundaries, inside frame headers, inside
// payloads, past the end of the file, into the epoch file, and into the
// counters, so that the classification (torn tail versus interior corruption
// versus refused header) and the exact number of discarded bytes are proven
// rather than assumed.
#include "co_test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

namespace {

constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kFrameHeaderBytes = 8;
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::int64_t kObservedAt = 1700000000000000000LL;

#define CO_REQUIRE_STORE(result_expression, name)                                      \
  auto co_store_result_##name = (result_expression);                                   \
  ::co::test::note_assertion();                                                        \
  if (!co_store_result_##name.ok()) {                                                  \
    CO_FAIL(std::string("expected success from ") + #result_expression + ": " +        \
            co_store_result_##name.status().render());                                 \
  }                                                                                    \
  auto& name = co_store_result_##name.value()

// ---------------------------------------------------------------------------
// Raw framing helpers.
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

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (unsigned int i = 0; i < 8U; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU));
  }
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
// Filesystem helpers (used only while no store holds the log open).
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

bool same_bytes(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool prefix_matches(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& prefix,
                    std::size_t count) {
  return bytes.size() >= count && prefix.size() >= count &&
         std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count), prefix.begin());
}

// ---------------------------------------------------------------------------
// Raw log structure.
// ---------------------------------------------------------------------------
struct FrameView {
  std::size_t offset{0};
  std::uint32_t declared_size{0};
  std::uint32_t declared_crc{0};
  bool crc_ok{false};
  std::string payload;

  [[nodiscard]] std::size_t end() const { return offset + kFrameHeaderBytes + declared_size; }
};

struct LogView {
  bool header_present{false};
  bool magic_ok{false};
  bool version_ok{false};
  bool header_crc_ok{false};
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
  view.magic_ok = std::memcmp(bytes.data(), "COBSTORE", 8U) == 0;
  const std::uint16_t version = static_cast<std::uint16_t>(bytes[8] | (static_cast<std::uint16_t>(bytes[9]) << 8U));
  view.version_ok = version == kFormatVersion;
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
    frame.crc_ok = crc32c(frame.payload) == frame.declared_crc;
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

// Replaces one frame's payload, reframing it with a correct CRC.
std::vector<std::uint8_t> rebuild_with_payload(const std::vector<std::uint8_t>& clean, std::size_t index,
                                               const std::string& payload) {
  const LogView view = view_log(clean);
  std::vector<std::uint8_t> out(clean.begin(), clean.begin() + static_cast<std::ptrdiff_t>(view.frames[index].offset));
  const std::vector<std::uint8_t> frame = make_frame(payload, true);
  out.insert(out.end(), frame.begin(), frame.end());
  const std::size_t after = view.frames[index].end();
  out.insert(out.end(), clean.begin() + static_cast<std::ptrdiff_t>(after), clean.end());
  return out;
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

EvidenceRecord installed_record(std::uint64_t generation, std::int64_t declared, const std::string& source) {
  Result<EvidenceRecord> built =
      EvidenceRecord::make(AuthorityId("dfi"), AuthorityRole::InstalledInventory, scope_of("site=dc1/hall=h1"),
                           Dimension::Power, CapacityAssertion::Installed, Unit::canonical(Dimension::Power),
                           declared, Generation(generation), Epoch(1), Revision(1));
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

MutationId mutation_of(const std::string& text) {
  Result<MutationId> parsed = MutationId::parse(text);
  if (!parsed.ok()) {
    CO_FAIL("could not parse mutation '" + text + "': " + parsed.status().render());
  }
  return std::move(parsed).value();
}

StoreOptions options_for(const std::string& directory) {
  StoreOptions options;
  options.directory = directory;
  return options;
}

void append_records(Store& store, std::size_t count) {
  for (std::size_t i = 1; i <= count; ++i) {
    const Result<AppendOutcome> appended = store.append_evidence(
        installed_record(i, static_cast<std::int64_t>(i) * 1000, "fixture:row" + std::to_string(i)),
        mutation_of("m-" + std::to_string(i)));
    if (!appended.ok() || !appended.value().applied) {
      CO_FAIL("could not append record " + std::to_string(i));
    }
  }
}

// Creates a store holding one epoch frame plus 'records' evidence frames and
// returns the raw log bytes.
std::vector<std::uint8_t> build_log(const std::string& directory, std::size_t records) {
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
  StoreOptions options = options_for(directory);
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    append_records(store, records);
  }
  return read_bytes(io::join(directory, "evidence.log"));
}

// Writes a damaged log into its own directory and returns that directory.
std::string stage(const test::ScratchDirectory& scratch, const std::string& label,
                  const std::vector<std::uint8_t>& bytes) {
  const std::string directory = scratch.child(label);
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
  replace_bytes(io::join(directory, "evidence.log"), bytes);
  return directory;
}

struct CutExpectation {
  bool torn{false};
  std::uint64_t frames{0};
  std::uint64_t discarded{0};
};

// Mirrors the documented rule: every frame that ends at or before the cut is
// complete; the bytes after the last complete frame are discarded.
CutExpectation expectation_for_cut(const LogView& clean_view, std::size_t cut) {
  CutExpectation expected;
  std::size_t end = kHeaderBytes;
  for (const FrameView& frame : clean_view.frames) {
    if (frame.end() <= cut) {
      ++expected.frames;
      end = frame.end();
    } else {
      break;
    }
  }
  expected.torn = cut > end;
  expected.discarded = static_cast<std::uint64_t>(cut - end);
  return expected;
}

}  // namespace

// ---------------------------------------------------------------------------
// Damage classification
// ---------------------------------------------------------------------------
CO_TEST(damage_at_every_frame_is_classified_conservatively) {
  test::ScratchDirectory scratch("inject-frames");
  const std::string source_directory = scratch.child("source");
  const std::vector<std::uint8_t> clean = build_log(source_directory, 4U);
  const LogView view = view_log(clean);
  CO_REQUIRE(view.header_ok());
  CO_REQUIRE_EQ(view.frames.size(), 5U);
  CO_REQUIRE_EQ(view.framed_end, clean.size());

  for (std::size_t index = 0; index < view.frames.size(); ++index) {
    const bool frames_follow = index + 1U < view.frames.size();

    // Variant A: one payload byte is flipped, the frame length is untouched.
    std::vector<std::uint8_t> payload_damage = clean;
    const std::size_t middle = view.frames[index].offset + kFrameHeaderBytes + view.frames[index].declared_size / 2U;
    payload_damage[middle] = static_cast<std::uint8_t>(payload_damage[middle] ^ 0xA5U);
    const std::string payload_directory =
        stage(scratch, "payload-" + std::to_string(index), payload_damage);
    const StoreOptions payload_options = options_for(payload_directory);

    const std::uint64_t remaining_bytes =
        static_cast<std::uint64_t>(clean.size() - view.frames[index].offset);
    if (frames_follow) {
      // A complete-length frame that fails its CRC with committed data after it
      // is interior corruption, never a tail: nothing is truncated.
      CO_REQUIRE_ERR(Store::open(payload_options), ReasonCode::InteriorCorruption);
      CO_REQUIRE_ERR(inspect_store(payload_options), ReasonCode::InteriorCorruption);
      CO_REQUIRE(same_bytes(read_bytes(io::join(payload_directory, "evidence.log")), payload_damage));
    } else {
      // The damaged frame is the last thing in the file. A crash before the
      // flush really can leave a complete-length frame whose payload never
      // landed, so both classifications are defensible here; what is not
      // negotiable is that nothing is dropped without being reported, and that
      // the byte count is exact.
      const Result<Store> opened = Store::open(payload_options);
      if (opened.ok()) {
        CO_REQUIRE(opened.value().recovery().torn_tail_recovered);
        CO_REQUIRE_EQ(opened.value().recovery().bytes_discarded, remaining_bytes);
        CO_REQUIRE_EQ(opened.value().recovery().frames_read, static_cast<std::uint64_t>(index));
        CO_REQUIRE_OK(opened.value().load_evidence(), loaded);
        CO_REQUIRE_EQ(loaded.size(), index == 0U ? 0U : index - 1U);
      } else {
        CO_REQUIRE_EQ(opened.status().code(), ReasonCode::InteriorCorruption);
        CO_REQUIRE(same_bytes(read_bytes(io::join(payload_directory, "evidence.log")), payload_damage));
      }
    }

    // Variant B: the frame length claims more bytes than the frame budget.
    std::vector<std::uint8_t> length_damage = clean;
    const std::uint32_t over_long = static_cast<std::uint32_t>((1U << 20U) + 1U);
    const std::size_t length_at = view.frames[index].offset;
    length_damage[length_at] = static_cast<std::uint8_t>(over_long & 0xFFU);
    length_damage[length_at + 1U] = static_cast<std::uint8_t>((over_long >> 8U) & 0xFFU);
    length_damage[length_at + 2U] = static_cast<std::uint8_t>((over_long >> 16U) & 0xFFU);
    length_damage[length_at + 3U] = static_cast<std::uint8_t>((over_long >> 24U) & 0xFFU);
    const std::string length_directory = stage(scratch, "length-" + std::to_string(index), length_damage);
    const StoreOptions length_options = options_for(length_directory);

    // A length field damaged to claim more than the whole file holds cannot be
    // distinguished from a torn final write by the bytes alone, at any position
    // in the file: the store reports the tail and its exact byte count instead
    // of guessing. The discard is explicit in the recovery report, never silent.
    if (index == 0U) {
      // A tear at the very first frame is a separate case: see
      // a_tear_at_the_first_frame_must_not_destroy_the_store_header.
      continue;
    }
    {
      CO_REQUIRE_STORE(Store::open(length_options), store);
      CO_REQUIRE(store.recovery().torn_tail_recovered);
      CO_REQUIRE_EQ(store.recovery().bytes_discarded,
                    static_cast<std::uint64_t>(clean.size() - view.frames[index].offset));
      CO_REQUIRE_EQ(store.recovery().frames_read, index);
    }
    const std::vector<std::uint8_t> length_after = read_bytes(io::join(length_directory, "evidence.log"));
    CO_REQUIRE(prefix_matches(length_after, clean, view.frames[index].offset));
    CO_REQUIRE_EQ(view_log(length_after).framed_end, length_after.size());
  }

  // A zero length frame can never be produced by this implementation.
  const std::string zero_directory =
      stage(scratch, "zero-middle", [&clean, &view]() {
        std::vector<std::uint8_t> damaged = clean;
        for (std::size_t i = 0; i < kFrameHeaderBytes; ++i) {
          damaged[view.frames[1].offset + i] = 0;
        }
        return damaged;
      }());
  CO_REQUIRE_ERR(Store::open(options_for(zero_directory)), ReasonCode::InteriorCorruption);
  CO_REQUIRE_ERR(inspect_store(options_for(zero_directory)), ReasonCode::InteriorCorruption);
}

CO_TEST(a_zero_length_frame_at_the_tail_is_a_torn_tail) {
  test::ScratchDirectory scratch("inject-zero-tail");
  const std::string source_directory = scratch.child("source");
  const std::vector<std::uint8_t> clean = build_log(source_directory, 2U);

  std::vector<std::uint8_t> damaged = clean;
  for (std::size_t i = 0; i < kFrameHeaderBytes; ++i) {
    damaged.push_back(0);
  }
  const std::string directory = stage(scratch, "zero-tail", damaged);
  const StoreOptions options = options_for(directory);

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(store.recovery().bytes_discarded, static_cast<std::uint64_t>(kFrameHeaderBytes));
    CO_REQUIRE_EQ(store.recovery().frames_read, 3U);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 2U);
  }
  const std::vector<std::uint8_t> after = read_bytes(io::join(directory, "evidence.log"));
  CO_REQUIRE(prefix_matches(after, clean, clean.size()));
  CO_REQUIRE(view_log(after).header_ok());
  CO_REQUIRE_EQ(view_log(after).framed_end, after.size());
}

CO_TEST(a_frame_length_beyond_eof_is_a_torn_tail) {
  test::ScratchDirectory scratch("inject-beyond-eof");
  const std::string source_directory = scratch.child("source");
  const std::vector<std::uint8_t> clean = build_log(source_directory, 2U);
  const LogView view = view_log(clean);

  // Claim 4 KiB of payload, deliver 100 bytes.
  const std::string claimed(4096, 'c');
  std::vector<std::uint8_t> damaged = clean;
  append_u32(damaged, static_cast<std::uint32_t>(claimed.size()));
  append_u32(damaged, crc32c(claimed));
  const std::uint8_t* claimed_bytes = reinterpret_cast<const std::uint8_t*>(claimed.data());
  damaged.insert(damaged.end(), claimed_bytes, claimed_bytes + 100);
  const std::string directory = stage(scratch, "beyond-eof", damaged);
  const StoreOptions options = options_for(directory);

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(store.recovery().bytes_discarded,
                  static_cast<std::uint64_t>(damaged.size() - clean.size()));
    CO_REQUIRE_EQ(store.recovery().frames_read, 3U);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 2U);
  }
  const std::vector<std::uint8_t> after = read_bytes(io::join(directory, "evidence.log"));
  CO_REQUIRE(prefix_matches(after, clean, clean.size()));
  CO_REQUIRE_EQ(view.frames.size(), 3U);
  CO_REQUIRE_EQ(view_log(after).framed_end, after.size());
}

CO_TEST(every_torn_tail_length_discards_exactly_that_many_bytes) {
  test::ScratchDirectory scratch("inject-exact");
  const std::string source_directory = scratch.child("source");
  const std::vector<std::uint8_t> clean = build_log(source_directory, 3U);
  const LogView clean_view = view_log(clean);
  CO_REQUIRE_EQ(clean_view.frames.size(), 4U);

  const std::string claimed(512, 'd');
  for (std::size_t length = 1; length <= 48; ++length) {
    std::vector<std::uint8_t> damaged = clean;
    if (length <= kFrameHeaderBytes) {
      damaged.insert(damaged.end(), length, static_cast<std::uint8_t>(0x77U));
    } else {
      append_u32(damaged, static_cast<std::uint32_t>(claimed.size()));
      append_u32(damaged, crc32c(claimed));
      const std::uint8_t* claimed_bytes = reinterpret_cast<const std::uint8_t*>(claimed.data());
      damaged.insert(damaged.end(), claimed_bytes, claimed_bytes + (length - kFrameHeaderBytes));
    }
    CO_REQUIRE_EQ(damaged.size(), clean.size() + length);

    const std::string directory = stage(scratch, "len-" + std::to_string(length), damaged);
    const StoreOptions options = options_for(directory);

    {
      CO_REQUIRE_STORE(Store::open(options), store);
      CO_REQUIRE(store.recovery().torn_tail_recovered);
      CO_REQUIRE_EQ(store.recovery().bytes_discarded, static_cast<std::uint64_t>(length));
      CO_REQUIRE_EQ(store.recovery().frames_read, 4U);
      CO_REQUIRE_OK(store.load_evidence(), loaded);
      CO_REQUIRE_EQ(loaded.size(), 3U);
    }

    const std::vector<std::uint8_t> after = read_bytes(io::join(directory, "evidence.log"));
    CO_REQUIRE(after.size() > clean.size());
    CO_REQUIRE(prefix_matches(after, clean, clean.size()));
    CO_REQUIRE_EQ(view_log(after).framed_end, after.size());
    CO_REQUIRE_EQ(view_log(after).frames.size(), 5U);
  }
}

CO_TEST(truncation_inside_a_frame_never_loses_a_complete_record) {
  test::ScratchDirectory scratch("inject-cuts");
  const std::string source_directory = scratch.child("source");
  const std::vector<std::uint8_t> clean = build_log(source_directory, 3U);
  const LogView clean_view = view_log(clean);
  CO_REQUIRE_EQ(clean_view.frames.size(), 4U);

  std::size_t checked = 0;
  for (std::size_t cut = kHeaderBytes; cut < clean.size(); ++cut) {
    // Every offset inside the first frame, then a regular sample of the rest.
    const bool near_start = cut < kHeaderBytes + 64U;
    if (!near_start && cut % 4U != 0U) {
      continue;
    }
    const std::vector<std::uint8_t> truncated(clean.begin(), clean.begin() + static_cast<std::ptrdiff_t>(cut));
    const std::string directory = stage(scratch, "cut-" + std::to_string(cut), truncated);
    const StoreOptions options = options_for(directory);
    const CutExpectation expected = expectation_for_cut(clean_view, cut);

    CO_REQUIRE_OK(inspect_store(options), report);
    CO_REQUIRE_EQ(report.torn_tail_recovered, expected.torn);
    CO_REQUIRE_EQ(report.bytes_discarded, expected.discarded);
    CO_REQUIRE_EQ(report.frames_read, expected.frames);

    StoreOptions read_only = options;
    read_only.read_only = true;
    CO_REQUIRE_STORE(Store::open(read_only), store);
    CO_REQUIRE_EQ(store.recovery().torn_tail_recovered, expected.torn);
    CO_REQUIRE_EQ(store.recovery().bytes_discarded, expected.discarded);
    CO_REQUIRE_EQ(store.recovery().frames_read, expected.frames);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    // Every complete evidence frame before the cut is still readable; nothing
    // else is invented.
    CO_REQUIRE_EQ(loaded.size(), expected.frames == 0U ? 0U : expected.frames - 1U);
    // The read-only open performed the truncation arithmetic but did not touch
    // the file.
    CO_REQUIRE_EQ(size_of(io::join(directory, "evidence.log")), static_cast<std::uint64_t>(cut));
    ++checked;
  }
  CO_REQUIRE(checked > 100U);
}

CO_TEST(truncation_inside_the_header_is_bad_magic) {
  test::ScratchDirectory scratch("inject-header");
  const std::string source_directory = scratch.child("source");
  const std::vector<std::uint8_t> clean = build_log(source_directory, 1U);

  for (std::size_t cut = 1; cut < kHeaderBytes; ++cut) {
    const std::vector<std::uint8_t> truncated(clean.begin(), clean.begin() + static_cast<std::ptrdiff_t>(cut));
    const std::string directory = stage(scratch, "head-" + std::to_string(cut), truncated);
    const StoreOptions options = options_for(directory);
    CO_REQUIRE_ERR(Store::open(options), ReasonCode::BadMagic);
    CO_REQUIRE_ERR(inspect_store(options), ReasonCode::BadMagic);
  }

  // An empty log is what a crash before the first write leaves behind: a
  // writable open recreates the header, a read-only open reports nothing.
  const std::string empty_directory = stage(scratch, "empty", std::vector<std::uint8_t>());
  const StoreOptions empty_options = options_for(empty_directory);
  {
    CO_REQUIRE_STORE(Store::open(empty_options), store);
    CO_REQUIRE_EQ(store.recovery().frames_read, 0U);
    CO_REQUIRE(!store.recovery().torn_tail_recovered);
    CO_REQUIRE(store.session_epoch() == Epoch(1));
  }
  const std::vector<std::uint8_t> recreated = read_bytes(io::join(empty_directory, "evidence.log"));
  CO_REQUIRE(view_log(recreated).header_ok());
  CO_REQUIRE_EQ(view_log(recreated).frames.size(), 1U);

  const std::string second_empty = stage(scratch, "empty-read-only", std::vector<std::uint8_t>());
  StoreOptions read_only = options_for(second_empty);
  read_only.read_only = true;
  {
    CO_REQUIRE_STORE(Store::open(read_only), reader);
    CO_REQUIRE_EQ(reader.recovery().frames_read, 0U);
    CO_REQUIRE(reader.recovery().explanation.find("does not exist yet") != std::string::npos);
    CO_REQUIRE_OK(reader.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 0U);
  }
}

// ---------------------------------------------------------------------------
// Refusals with no side effects
// ---------------------------------------------------------------------------
CO_TEST(a_read_only_store_refuses_every_mutation) {
  test::ScratchDirectory scratch("inject-read-only");
  const std::string directory = scratch.child("store");
  const std::vector<std::uint8_t> clean = build_log(directory, 2U);
  const std::string log_path = io::join(directory, "evidence.log");
  const std::string snapshot_path = io::join(directory, "snapshot.json");
  const std::vector<std::uint8_t> epoch_before = read_bytes(io::join(directory, "epoch"));

  StoreOptions options = options_for(directory);
  options.read_only = true;
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    // A read-only reader reports the epoch of the session that wrote the log
    // and publishes nothing of its own.
    CO_REQUIRE(store.session_epoch() == Epoch(1));
    CO_REQUIRE_ERR(store.append_evidence(installed_record(9, 9000, "fixture:ro"), mutation_of("m-ro")),
                   ReasonCode::StoreClosed);
    CO_REQUIRE_ERR(store.append_idempotent_marker(mutation_of("m-ro"), sha256("x")), ReasonCode::StoreClosed);
    CO_REQUIRE_ERR(store.publish_snapshot("{\"ro\":true}"), ReasonCode::StoreClosed);
  }

  CO_REQUIRE(same_bytes(read_bytes(log_path), clean));
  CO_REQUIRE_EQ(size_of(log_path), static_cast<std::uint64_t>(clean.size()));
  CO_REQUIRE(!io::exists(snapshot_path));
  CO_REQUIRE(same_bytes(read_bytes(io::join(directory, "epoch")), epoch_before));
}

CO_TEST(a_closed_store_refuses_every_mutation) {
  test::ScratchDirectory scratch("inject-closed");
  const std::string directory = scratch.child("store");
  const std::vector<std::uint8_t> clean = build_log(directory, 2U);
  const std::string log_path = io::join(directory, "evidence.log");

  StoreOptions options = options_for(directory);
  CO_REQUIRE_STORE(Store::open(options), store);
  // This session's epoch frame is already durable; freeze the image now so the
  // refusals below are measured against the state the closed store left.
  const std::vector<std::uint8_t> before_close = read_bytes(log_path);
  store.close();
  CO_REQUIRE(!store.is_open());
  CO_REQUIRE_ERR(store.append_evidence(installed_record(9, 9000, "fixture:closed"), mutation_of("m-closed")),
                 ReasonCode::StoreClosed);
  CO_REQUIRE_ERR(store.append_idempotent_marker(mutation_of("m-closed"), sha256("y")), ReasonCode::StoreClosed);
  CO_REQUIRE_ERR(store.publish_snapshot("{\"closed\":true}"), ReasonCode::StoreClosed);
  CO_REQUIRE_ERR(store.durable_bytes(), ReasonCode::StoreClosed);
  CO_REQUIRE(same_bytes(read_bytes(log_path), before_close));

  // Reopening the same directory afterwards sees exactly the durable records.
  {
    CO_REQUIRE_STORE(Store::open(options), reopened);
    CO_REQUIRE(reopened.is_open());
    CO_REQUIRE_OK(reopened.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 2U);
  }
}

CO_TEST(refused_appends_leave_the_log_byte_identical) {
  test::ScratchDirectory scratch("inject-refusals");
  const std::string directory = scratch.child("store");
  StoreOptions options = options_for(directory);
  options.max_frame_bytes = 2048;
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
  const std::string log_path = io::join(directory, "evidence.log");

  std::uint64_t clean_size = 0;
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    append_records(store, 2U);
    const std::vector<std::uint8_t> before = read_bytes(log_path);
    clean_size = static_cast<std::uint64_t>(before.size());

    EvidenceRecord invalid = installed_record(9, 1000, "fixture:invalid");
    invalid.scope = ScopePath();
    CO_REQUIRE_ERR(store.append_evidence(invalid, mutation_of("m-bad")), ReasonCode::MalformedEvidence);

    EvidenceRecord mismatched = installed_record(9, 1000, "fixture:mismatch");
    mismatched.amount = Amount(1);
    CO_REQUIRE_ERR(store.append_evidence(mismatched, mutation_of("m-mismatch")), ReasonCode::MalformedEvidence);

    CO_REQUIRE_ERR(store.append_evidence(installed_record(9, 1000, "fixture:empty-mutation"), MutationId()),
                   ReasonCode::InvalidIdentifier);

    CO_REQUIRE_ERR(store.append_evidence(installed_record(9, 1000, std::string(4096, 'q')), mutation_of("m-big")),
                   ReasonCode::RecordTooLarge);
    CO_REQUIRE_ERR(store.publish_snapshot(std::string(4096, 'q')), ReasonCode::RecordTooLarge);
    CO_REQUIRE_ERR(store.append_idempotent_marker(mutation_of("m-1"), sha256("conflict")),
                   ReasonCode::IdempotencyConflict);

    CO_REQUIRE(same_bytes(read_bytes(log_path), before));
    CO_REQUIRE_EQ(store.mutations().size(), 2U);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 2U);
    CO_REQUIRE(!io::exists(io::join(directory, "snapshot.json")));
  }

  // The same refusals at the very end of the file's capacity bound. The limit
  // leaves room for exactly one more session epoch frame and nothing else.
  StoreOptions tight = options;
  tight.max_file_bytes = clean_size + 200U;
  {
    CO_REQUIRE_STORE(Store::open(tight), store);
    ReasonCode refusal = ReasonCode::Ok;
    for (std::uint64_t i = 100; i < 200U; ++i) {
      const Result<AppendOutcome> outcome =
          store.append_evidence(installed_record(i, 1, "fixture:tight"), mutation_of("m-tight-" + std::to_string(i)));
      if (!outcome.ok()) {
        refusal = outcome.status().code();
        break;
      }
    }
    CO_REQUIRE_EQ(refusal, ReasonCode::CapacityExceeded);

    const std::vector<std::uint8_t> full = read_bytes(log_path);
    const Result<AppendOutcome> again =
        store.append_evidence(installed_record(300, 1, "fixture:tight"), mutation_of("m-tight-300"));
    CO_REQUIRE_ERR(again, ReasonCode::CapacityExceeded);
    CO_REQUIRE(same_bytes(read_bytes(log_path), full));
    CO_REQUIRE(store.durable_bytes().value() <= tight.max_file_bytes);
  }
}

// ---------------------------------------------------------------------------
// Misconfiguration and counter exhaustion
// ---------------------------------------------------------------------------
CO_TEST(shrinking_max_frame_bytes_refuses_rather_than_truncating) {
  test::ScratchDirectory scratch("inject-shrink");
  const std::string directory = scratch.child("store");
  const StoreOptions options = options_for(directory);
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_OK(store.append_evidence(installed_record(1, 1000, std::string(3000, 'w')), mutation_of("m-wide")),
                  wide);
    CO_REQUIRE(wide.applied);
    append_records(store, 2U);
  }
  const std::string log_path = io::join(directory, "evidence.log");
  const std::vector<std::uint8_t> clean = read_bytes(log_path);
  CO_REQUIRE(clean.size() > 3000U);

  // A stored frame that is larger than the new limit is a configuration
  // mismatch, not corruption: it is refused with an actionable reason instead of
  // being truncated away, and the file is left exactly as it was found.
  StoreOptions narrow = options;
  narrow.max_frame_bytes = 1024;
  const Result<Store> refused = Store::open(narrow);
  CO_REQUIRE(!refused.ok());
  CO_REQUIRE_EQ(refused.status().code(), ReasonCode::CapacityExceeded);
  CO_REQUIRE(refused.status().detail().find("max_frame_bytes") != std::string::npos);
  CO_REQUIRE(refused.status().detail().find("1024") != std::string::npos);
  const Result<RecoveryReport> inspected_narrow = inspect_store(narrow);
  CO_REQUIRE(!inspected_narrow.ok());
  CO_REQUIRE_EQ(inspected_narrow.status().code(), ReasonCode::CapacityExceeded);
  CO_REQUIRE(same_bytes(read_bytes(log_path), clean));
  CO_REQUIRE_EQ(size_of(log_path), static_cast<std::uint64_t>(clean.size()));

  // With a limit that accommodates every frame the same log opens normally.
  StoreOptions wide_enough = options;
  wide_enough.max_frame_bytes = 8192;
  {
    CO_REQUIRE_STORE(Store::open(wide_enough), store);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 3U);
  }
}

CO_TEST(an_exhausted_epoch_counter_is_refused) {
  test::ScratchDirectory scratch("inject-epoch-max");
  const std::string directory = scratch.child("store");
  const std::vector<std::uint8_t> clean = build_log(directory, 2U);
  const std::string log_path = io::join(directory, "evidence.log");
  const std::string epoch_path = io::join(directory, "epoch");

  std::vector<std::uint8_t> max_epoch;
  append_u64(max_epoch, (std::numeric_limits<std::uint64_t>::max)());
  append_u32(max_epoch, crc32c_bytes(max_epoch.data(), max_epoch.size()));
  replace_bytes(epoch_path, max_epoch);

  const StoreOptions options = options_for(directory);
  CO_REQUIRE_ERR(Store::open(options), ReasonCode::EpochExhausted);
  // The log is untouched by the refused open.
  CO_REQUIRE(same_bytes(read_bytes(log_path), clean));
  // Reading is still possible: exhaustion only blocks the writer session.
  StoreOptions read_only = options;
  read_only.read_only = true;
  {
    CO_REQUIRE_STORE(Store::open(read_only), store);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 2U);
  }
}

CO_TEST(a_sequence_at_the_signed_json_ceiling_does_not_wrap) {
  test::ScratchDirectory scratch("inject-seq-max");
  const std::string directory = scratch.child("store");
  const std::vector<std::uint8_t> clean = build_log(directory, 2U);
  const LogView view = view_log(clean);
  CO_REQUIRE_EQ(view.frames.size(), 3U);

  // The highest sequence number a stored frame can carry: the frame field is a
  // signed JSON integer, so INT64_MAX is the representable ceiling.
  Result<json::Value> parsed = json::Value::parse(view.frames.back().payload);
  CO_REQUIRE(parsed.ok());
  parsed.value().set("seq", json::Value((std::numeric_limits<std::int64_t>::max)()));
  const std::vector<std::uint8_t> at_ceiling =
      rebuild_with_payload(clean, view.frames.size() - 1U, json::dump(parsed.value(), true, -1));

  const std::string staged_directory = stage(scratch, "seq-max", at_ceiling);
  const StoreOptions options = options_for(staged_directory);
  const std::string log_path = io::join(staged_directory, "evidence.log");
  const LogView staged = view_log(at_ceiling);
  CO_REQUIRE_EQ(staged.frames.size(), 3U);
  const std::uint64_t signed_ceiling = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

  // A writer cannot add a session epoch frame without a sequence that fits in a
  // signed JSON integer, so the open is refused with an actionable reason rather
  // than writing a sequence this build would then refuse to read back.
  const Result<Store> refused = Store::open(options);
  CO_REQUIRE(!refused.ok());
  CO_REQUIRE_EQ(refused.status().code(), ReasonCode::EpochExhausted);
  CO_REQUIRE(refused.status().detail().find("sequence") != std::string::npos);
  CO_REQUIRE(refused.status().detail().find("9223372036854775807") != std::string::npos);

  // Nothing was written: not one byte of the log changed.
  CO_REQUIRE(same_bytes(read_bytes(log_path), at_ceiling));
  CO_REQUIRE_EQ(size_of(log_path), static_cast<std::uint64_t>(at_ceiling.size()));

  // Refusing to append must never make committed evidence unreadable.
  StoreOptions read_only = options;
  read_only.read_only = true;
  {
    CO_REQUIRE_STORE(Store::open(read_only), reader);
    CO_REQUIRE(reader.last_sequence() == SequenceNumber(signed_ceiling));
    CO_REQUIRE(!reader.recovery().torn_tail_recovered);
    CO_REQUIRE_OK(reader.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 2U);
    CO_REQUIRE(loaded[0].record.content_digest() == installed_record(1, 1000, "fixture:row1").content_digest());
    CO_REQUIRE(loaded[1].record.content_digest() == installed_record(2, 2000, "fixture:row2").content_digest());
  }

  // Inspection reports exactly the same frames, and the file is still untouched.
  CO_REQUIRE_OK(inspect_store(options), inspected);
  CO_REQUIRE_EQ(inspected.frames_read, 3U);
  CO_REQUIRE_EQ(inspected.evidence_frames, 2U);
  CO_REQUIRE_EQ(inspected.epoch_frames, 1U);
  CO_REQUIRE(same_bytes(read_bytes(log_path), at_ceiling));
  CO_REQUIRE_EQ(signed_ceiling, static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()));
}

CO_TEST(a_store_directory_that_is_not_a_store_is_refused) {
  test::ScratchDirectory scratch("inject-paths");

  // An empty directory name is refused before any file system work.
  StoreOptions empty;
  CO_REQUIRE_ERR(Store::open(empty), ReasonCode::InvalidArgument);

  // A store directory that is really a file cannot host an evidence log.
  const std::string file_path = scratch.child("not-a-directory");
  replace_bytes(file_path, bytes_of("this is a file, not a store directory"));
  StoreOptions as_file = options_for(file_path);
  CO_REQUIRE_ERR(Store::open(as_file), ReasonCode::IoFailure);

  // An evidence.log that is really a directory cannot be opened for appending.
  const std::string blocked_directory = scratch.child("blocked-log");
  CO_REQUIRE_OK_VOID(io::ensure_directory(blocked_directory));
  CO_REQUIRE_OK_VOID(io::ensure_directory(io::join(blocked_directory, "evidence.log")));
  StoreOptions blocked = options_for(blocked_directory);
  CO_REQUIRE_ERR(Store::open(blocked), ReasonCode::IoFailure);
}

// Interior damage at a distance: the damaged frame is complete and 120 KiB
// payloads sit between it and the next valid frame, so no bounded search window
// can be the reason this is caught. Requirement: interior corruption is
// REJECTED by both entry points and the file is left untouched.
CO_TEST(interior_damage_beyond_any_search_window_is_rejected) {
  test::ScratchDirectory scratch("inject-wide");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
  const StoreOptions options = options_for(directory);
  const std::string wide_source(120000, 'L');

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    for (std::size_t i = 1; i <= 3U; ++i) {
      CO_REQUIRE_OK(store.append_evidence(installed_record(i, static_cast<std::int64_t>(i) * 1000, wide_source),
                                          mutation_of("m-wide-" + std::to_string(i))),
                    outcome);
      CO_REQUIRE(outcome.applied);
    }
  }
  const std::string log_path = io::join(directory, "evidence.log");
  const std::vector<std::uint8_t> clean = read_bytes(log_path);
  const LogView view = view_log(clean);
  CO_REQUIRE_EQ(view.frames.size(), 4U);
  // The frames really are further apart than the interior search window.
  CO_REQUIRE(view.frames[2].offset - view.frames[1].offset > 65536U);

  std::vector<std::uint8_t> damaged = clean;
  const std::size_t middle = view.frames[1].offset + kFrameHeaderBytes + view.frames[1].declared_size / 2U;
  damaged[middle] = static_cast<std::uint8_t>(damaged[middle] ^ 0x33U);
  replace_bytes(log_path, damaged);
  const std::uint64_t distance_to_next = static_cast<std::uint64_t>(view.frames[2].offset - view.frames[1].offset);
  const std::uint64_t later_bytes =
      static_cast<std::uint64_t>(clean.size() - (view.frames[1].offset + kFrameHeaderBytes +
                                                 static_cast<std::size_t>(view.frames[1].declared_size)));
  CO_REQUIRE(later_bytes > distance_to_next);

  const Result<Store> opened = Store::open(options);
  CO_REQUIRE(!opened.ok());
  CO_REQUIRE_EQ(opened.status().code(), ReasonCode::InteriorCorruption);
  // The refusal names the exact number of bytes that would have been destroyed.
  CO_REQUIRE(opened.status().detail().find("later data remain") != std::string::npos);
  CO_REQUIRE(opened.status().detail().find(std::to_string(later_bytes)) != std::string::npos);
  CO_REQUIRE_ERR(inspect_store(options), ReasonCode::InteriorCorruption);

  // Rejected means untouched: the damage and every complete later record are
  // still on disk, and the middle record is still not readable.
  CO_REQUIRE_EQ(size_of(log_path), static_cast<std::uint64_t>(damaged.size()));
  CO_REQUIRE(same_bytes(read_bytes(log_path), damaged));

  // The same store still opens and reads all three records once the damage is
  // repaired, which proves the rejection did not cost any committed record.
  replace_bytes(log_path, clean);
  StoreOptions read_only = options;
  read_only.read_only = true;
  {
    CO_REQUIRE_STORE(Store::open(read_only), store);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 3U);
  }
}

// A tear at the very first frame is the worst case for a truncating recovery:
// the header has already been verified when the frame loop starts, so the
// truncation point for a damage at the first frame must be the end of the
// header, leaving a valid empty store. The store must never truncate its own
// header away.
CO_TEST(a_tear_at_the_first_frame_must_not_destroy_the_store_header) {
  test::ScratchDirectory scratch("inject-first-tear");
  const std::string source_directory = scratch.child("source");
  const std::vector<std::uint8_t> clean = build_log(source_directory, 1U);
  const LogView view = view_log(clean);
  CO_REQUIRE_EQ(view.frames.size(), 2U);

  // Damage the length field of the first frame so that it claims more than the
  // whole file holds.
  std::vector<std::uint8_t> damaged = clean;
  const std::uint32_t over_long = static_cast<std::uint32_t>((1U << 20U) + 1U);
  const std::size_t at = view.frames[0].offset;
  damaged[at] = static_cast<std::uint8_t>(over_long & 0xFFU);
  damaged[at + 1U] = static_cast<std::uint8_t>((over_long >> 8U) & 0xFFU);
  damaged[at + 2U] = static_cast<std::uint8_t>((over_long >> 16U) & 0xFFU);
  damaged[at + 3U] = static_cast<std::uint8_t>((over_long >> 24U) & 0xFFU);
  const std::string read_only_directory = stage(scratch, "first-tear-read-only", damaged);
  const std::string writer_directory = stage(scratch, "first-tear", damaged);

  // A read-only open reports the tear and leaves the damaged file exactly as it
  // found it, header included.
  StoreOptions read_only = options_for(read_only_directory);
  read_only.read_only = true;
  {
    CO_REQUIRE_STORE(Store::open(read_only), reader);
    CO_REQUIRE(reader.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(reader.recovery().bytes_discarded,
                  static_cast<std::uint64_t>(damaged.size() - kHeaderBytes));
    CO_REQUIRE_EQ(reader.recovery().frames_read, 0U);
    CO_REQUIRE_OK(reader.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 0U);
  }
  CO_REQUIRE(same_bytes(read_bytes(io::join(read_only_directory, "evidence.log")), damaged));

  // A writable open discards the torn bytes but keeps the verified header: the
  // truncation point for a tear at the first frame is the end of the header.
  const std::string log_path = io::join(writer_directory, "evidence.log");
  const StoreOptions options = options_for(writer_directory);
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(store.recovery().bytes_discarded,
                  static_cast<std::uint64_t>(damaged.size() - kHeaderBytes));
    CO_REQUIRE_EQ(store.recovery().frames_read, 0U);
  }

  const std::vector<std::uint8_t> after = read_bytes(log_path);
  CO_REQUIRE(view_log(after).header_ok());
  CO_REQUIRE_EQ(view_log(after).frames.size(), 1U);
  CO_REQUIRE_EQ(view_log(after).framed_end, after.size());
  CO_REQUIRE(prefix_matches(after, clean, kHeaderBytes));
  CO_REQUIRE_OK(inspect_store(options), reopened);
  CO_REQUIRE_EQ(reopened.frames_read, 1U);
  CO_REQUIRE_EQ(reopened.evidence_frames, 0U);
  CO_REQUIRE_EQ(reopened.epoch_frames, 1U);
  CO_REQUIRE(!reopened.torn_tail_recovered);

  // The recovered store is fully usable: the fixture can be written again and
  // read back, so the tear cost no committed record beyond the damaged frame.
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_OK(store.append_evidence(installed_record(9, 9000, "fixture:after-tear"), mutation_of("m-after-tear")),
                  outcome);
    CO_REQUIRE(outcome.applied);
    CO_REQUIRE_OK(store.load_evidence(), reloaded);
    CO_REQUIRE_EQ(reloaded.size(), 1U);
    CO_REQUIRE(reloaded[0].record.content_digest() ==
               installed_record(9, 9000, "fixture:after-tear").content_digest());
  }
}
