// Capacity Observatory - durable evidence log: framing, recovery, and locking.
#include "capacity_observatory/store.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "capacity_observatory/hash.hpp"
#include "capacity_observatory/json.hpp"
#include "capacity_observatory/platform.hpp"

namespace co {
namespace {

constexpr std::size_t kHeaderBytes = 16;
constexpr char kMagic[8] = {'C', 'O', 'B', 'S', 'T', 'O', 'R', 'E'};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::size_t kFrameHeaderBytes = 8;
// How far past a corrupt frame to look, byte by byte, for evidence that the
// corruption is interior rather than a torn tail. The natural resynchronisation
// point is probed first, so this bound only matters when the length field itself
// was damaged.
constexpr std::uint64_t kInteriorSearchBytes = 1ULL << 12U;

std::string log_path(const std::string& directory) { return io::join(directory, "evidence.log"); }
std::string snapshot_path(const std::string& directory) { return io::join(directory, "snapshot.json"); }
std::string epoch_path(const std::string& directory) { return io::join(directory, "epoch"); }
std::string lock_path(const std::string& directory) { return io::join(directory, "writer.lock"); }

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (unsigned int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU));
  }
}

std::uint32_t load_u32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) | (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint64_t load_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (unsigned int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data[i]) << (8U * i);
  }
  return value;
}

std::string kind_name(StoreRecordKind kind) {
  switch (kind) {
    case StoreRecordKind::EpochAdvance: return "epoch";
    case StoreRecordKind::Evidence: return "evidence";
    case StoreRecordKind::Snapshot: return "snapshot";
    case StoreRecordKind::Marker: return "marker";
  }
  return "unknown";
}

Result<StoreRecordKind> kind_from_name(std::string_view name) {
  if (name == "epoch") return Result<StoreRecordKind>(StoreRecordKind::EpochAdvance);
  if (name == "evidence") return Result<StoreRecordKind>(StoreRecordKind::Evidence);
  if (name == "snapshot") return Result<StoreRecordKind>(StoreRecordKind::Snapshot);
  if (name == "marker") return Result<StoreRecordKind>(StoreRecordKind::Marker);
  return make_error(ReasonCode::MalformedInput, "unknown store record kind '" + std::string(name) + "'");
}

// ---------------------------------------------------------------------------
// Evidence <-> JSON
// ---------------------------------------------------------------------------
json::Value encode_evidence(const co::EvidenceRecord& record) {
  json::Object members;
  members.emplace_back("authority", json::Value(record.authority.value()));
  members.emplace_back("role", json::Value(std::string(authority_role_text(record.role))));
  members.emplace_back("scope", json::Value(record.scope.text()));
  members.emplace_back("dimension", json::Value(std::string(dimension_text(record.dimension))));
  members.emplace_back("assertion", json::Value(std::string(assertion_text(record.assertion))));
  members.emplace_back("unit", json::Value(record.unit.symbol()));
  members.emplace_back("unit_dimension", json::Value(std::string(dimension_text(record.unit.dimension()))));
  members.emplace_back("unit_numerator", json::Value(record.unit.numerator()));
  members.emplace_back("unit_denominator", json::Value(record.unit.denominator()));
  members.emplace_back("declared", json::Value(record.declared_amount));
  members.emplace_back("amount", json::Value(record.amount.canonical()));
  members.emplace_back("epoch", json::Value(static_cast<std::int64_t>(record.epoch.value())));
  members.emplace_back("generation", json::Value(static_cast<std::int64_t>(record.generation.value())));
  members.emplace_back("revision", json::Value(static_cast<std::int64_t>(record.revision.value())));
  members.emplace_back("provenance", json::Value(std::string(provenance_text(record.provenance))));
  members.emplace_back("has_observed_at", json::Value(record.has_observed_at));
  members.emplace_back("observed_at", json::Value(record.has_observed_at ? record.observed_at.unix_nanos() : 0));
  members.emplace_back("validity_from_present", json::Value(record.validity.from.has_value()));
  members.emplace_back("validity_from",
                       json::Value(record.validity.from.has_value() ? record.validity.from.value().unix_nanos() : 0));
  members.emplace_back("validity_until_present", json::Value(record.validity.until.has_value()));
  members.emplace_back("validity_until",
                       json::Value(record.validity.until.has_value() ? record.validity.until.value().unix_nanos() : 0));
  members.emplace_back("source", json::Value(record.source));
  return json::Value::object(std::move(members));
}

Result<AuthorityRole> role_from_text(std::string_view text) {
  const std::array<std::pair<std::string_view, AuthorityRole>, 7> table{{
      {"committed-capacity", AuthorityRole::CommittedCapacity},
      {"reservations", AuthorityRole::Reservations},
      {"installed-inventory", AuthorityRole::InstalledInventory},
      {"policy", AuthorityRole::Policy},
      {"plant-telemetry", AuthorityRole::PlantTelemetry},
      {"economics", AuthorityRole::Economics},
      {"arbitration", AuthorityRole::Arbitration},
  }};
  for (const auto& entry : table) {
    if (entry.first == text) {
      return Result<AuthorityRole>(entry.second);
    }
  }
  return make_error(ReasonCode::MalformedInput, "unknown authority role '" + std::string(text) + "'");
}

Result<Provenance> provenance_from_text(std::string_view text) {
  if (text == "observed") return Result<Provenance>(Provenance::Observed);
  if (text == "recovered") return Result<Provenance>(Provenance::Recovered);
  if (text == "synthetic") return Result<Provenance>(Provenance::Synthetic);
  return make_error(ReasonCode::MalformedInput, "unknown provenance '" + std::string(text) + "'");
}

Result<EvidenceRecord> decode_evidence(const json::Value& node) {
  if (!node.is_object()) {
    return make_error(ReasonCode::MalformedInput, "stored evidence payload is not an object");
  }
  const Result<std::string> authority = node.require_string("authority");
  if (!authority.ok()) return authority.status();
  const Result<std::string> role = node.require_string("role");
  if (!role.ok()) return role.status();
  const Result<std::string> scope_text = node.require_string("scope");
  if (!scope_text.ok()) return scope_text.status();
  const Result<std::string> dimension_text_value = node.require_string("dimension");
  if (!dimension_text_value.ok()) return dimension_text_value.status();
  const Result<std::string> assertion_text_value = node.require_string("assertion");
  if (!assertion_text_value.ok()) return assertion_text_value.status();
  const Result<std::string> unit_dimension_text = node.require_string("unit_dimension");
  if (!unit_dimension_text.ok()) return unit_dimension_text.status();
  const Result<std::int64_t> unit_numerator = node.require_int("unit_numerator");
  if (!unit_numerator.ok()) return unit_numerator.status();
  const Result<std::int64_t> unit_denominator = node.require_int("unit_denominator");
  if (!unit_denominator.ok()) return unit_denominator.status();
  const Result<std::int64_t> declared = node.require_int("declared");
  if (!declared.ok()) return declared.status();
  const Result<std::int64_t> epoch = node.require_int("epoch");
  if (!epoch.ok()) return epoch.status();
  const Result<std::int64_t> generation = node.require_int("generation");
  if (!generation.ok()) return generation.status();
  const Result<std::int64_t> revision = node.require_int("revision");
  if (!revision.ok()) return revision.status();

  const Result<AuthorityId> authority_id = AuthorityId::parse(authority.value());
  if (!authority_id.ok()) return authority_id.status();
  const Result<AuthorityRole> authority_role = role_from_text(role.value());
  if (!authority_role.ok()) return authority_role.status();
  const Result<ScopePath> scope = ScopePath::parse(scope_text.value());
  if (!scope.ok()) return scope.status();
  const Result<Dimension> dimension = dimension_from_text(dimension_text_value.value());
  if (!dimension.ok()) return dimension.status();
  const Result<CapacityAssertion> assertion = assertion_from_text(assertion_text_value.value());
  if (!assertion.ok()) return assertion.status();
  const Result<Dimension> unit_dimension = dimension_from_text(unit_dimension_text.value());
  if (!unit_dimension.ok()) return unit_dimension.status();
  const Result<Unit> unit = Unit::make(unit_dimension.value(), unit_numerator.value(), unit_denominator.value());
  if (!unit.ok()) return unit.status();

  if (epoch.value() < 0 || generation.value() < 0 || revision.value() < 0) {
    return make_error(ReasonCode::MalformedInput, "stored epoch, generation, and revision must not be negative");
  }

  Result<EvidenceRecord> record = EvidenceRecord::make(authority_id.value(), authority_role.value(), scope.value(),
                                                       dimension.value(), assertion.value(), unit.value(),
                                                       declared.value(), Generation(static_cast<std::uint64_t>(generation.value())),
                                                       Epoch(static_cast<std::uint64_t>(epoch.value())),
                                                       Revision(static_cast<std::uint64_t>(revision.value())));
  if (!record.ok()) {
    return record.status();
  }
  EvidenceRecord& built = record.value();

  const Result<std::string> provenance = node.require_string("provenance");
  if (!provenance.ok()) return provenance.status();
  const Result<Provenance> provenance_value = provenance_from_text(provenance.value());
  if (!provenance_value.ok()) return provenance_value.status();
  built.provenance = provenance_value.value();

  const Result<bool> has_observed = node.require_bool("has_observed_at");
  if (!has_observed.ok()) return has_observed.status();
  built.has_observed_at = has_observed.value();
  const Result<std::int64_t> observed_at = node.require_int("observed_at");
  if (!observed_at.ok()) return observed_at.status();
  built.observed_at = Timestamp(observed_at.value());

  const Result<bool> from_present = node.require_bool("validity_from_present");
  if (!from_present.ok()) return from_present.status();
  const Result<std::int64_t> from = node.require_int("validity_from");
  if (!from.ok()) return from.status();
  if (from_present.value()) {
    built.validity.from = Timestamp(from.value());
  }
  const Result<bool> until_present = node.require_bool("validity_until_present");
  if (!until_present.ok()) return until_present.status();
  const Result<std::int64_t> until = node.require_int("validity_until");
  if (!until.ok()) return until.status();
  if (until_present.value()) {
    built.validity.until = Timestamp(until.value());
  }

  const Result<std::optional<std::string>> source = node.optional_string("source");
  if (!source.ok()) return source.status();
  built.source = source.value().value_or(std::string());

  const Result<void> valid = built.validate();
  if (!valid.ok()) {
    return valid.status().with_context("stored evidence record");
  }
  return record;
}

// ---------------------------------------------------------------------------
// Frame scanning
// ---------------------------------------------------------------------------
struct ScannedFrame {
  StoreRecordKind kind{StoreRecordKind::Evidence};
  std::string payload;
  std::uint64_t offset{0};
};

struct ScannedLog {
  std::vector<ScannedFrame> frames;
  // The truncation point for a torn tail. It starts at the end of the verified
  // header rather than at zero: a tear in the very first frame must truncate the
  // log to an empty but still valid store, never to a headerless file that no
  // later open could ever read again.
  std::uint64_t valid_bytes{kHeaderBytes};
  bool torn_tail{false};
  std::uint64_t discarded_bytes{0};
};

// True when the eight bytes at 'offset' describe a frame whose length fits in the
// file and whose payload passes its integrity check.
bool validates_as_frame(io::File& file, std::uint64_t offset, std::uint64_t file_size,
                        const StoreOptions& options) {
  if (offset + kFrameHeaderBytes > file_size) {
    return false;
  }
  std::array<std::uint8_t, kFrameHeaderBytes> header{};
  const Result<std::size_t> read = file.read_at(offset, header.data(), header.size());
  if (!read.ok() || read.value() != header.size()) {
    return false;
  }
  const std::uint32_t size = load_u32(header.data());
  if (size == 0 || size > options.max_frame_bytes) {
    return false;
  }
  if (offset + kFrameHeaderBytes + size > file_size) {
    return false;
  }
  std::vector<std::uint8_t> payload(size);
  const Result<std::size_t> payload_read = file.read_at(offset + kFrameHeaderBytes, payload.data(), payload.size());
  if (!payload_read.ok() || payload_read.value() != payload.size()) {
    return false;
  }
  return crc32c_bytes(payload.data(), payload.size()) == load_u32(header.data() + 4);
}

// Looks for a well formed frame at or after 'from'. This is what separates a torn
// tail (nothing valid follows, so truncating at the damage loses nothing that was
// ever committed) from interior corruption (valid committed data follows, so the
// damage is not a tail and the file must be rejected rather than truncated).
//
// Two probes are used, both bounded:
//   1. the natural resynchronisation point, which is where the next frame would
//      begin if the damaged frame's length field were intact - this is the
//      realistic case of a bit flip inside a payload;
//   2. a bounded byte-by-byte search, for the case where the length field itself
//      was damaged.
bool find_later_valid_frame(io::File& file, std::uint64_t damage_offset, std::uint64_t file_size,
                            const StoreOptions& options) {
  if (damage_offset + kFrameHeaderBytes <= file_size) {
    std::array<std::uint8_t, kFrameHeaderBytes> header{};
    const Result<std::size_t> read = file.read_at(damage_offset, header.data(), header.size());
    if (read.ok() && read.value() == header.size()) {
      const std::uint32_t size = load_u32(header.data());
      if (size != 0 && size <= options.max_frame_bytes && damage_offset + kFrameHeaderBytes + size <= file_size) {
        if (validates_as_frame(file, damage_offset + kFrameHeaderBytes + size, file_size, options)) {
          return true;
        }
      }
    }
  }
  const std::uint64_t limit = (std::min)(file_size, damage_offset + kInteriorSearchBytes);
  for (std::uint64_t offset = damage_offset + 1; offset + kFrameHeaderBytes <= limit; ++offset) {
    if (validates_as_frame(file, offset, file_size, options)) {
      return true;
    }
  }
  return false;
}

Result<ScannedLog> scan_log(io::File& file, const StoreOptions& options, bool writable, RecoveryReport& report) {
  const Result<std::uint64_t> size_result = file.size();
  if (!size_result.ok()) {
    return size_result.status();
  }
  const std::uint64_t file_size = size_result.value();
  ScannedLog log;
  if (file_size == 0) {
    return Result<ScannedLog>(std::move(log));
  }
  if (file_size < kHeaderBytes) {
    return make_error(ReasonCode::BadMagic,
                      "store header is " + std::to_string(file_size) + " bytes, which is shorter than the " +
                          std::to_string(kHeaderBytes) + " byte header");
  }

  std::array<std::uint8_t, kHeaderBytes> header{};
  const Result<std::size_t> header_read = file.read_at(0, header.data(), header.size());
  if (!header_read.ok() || header_read.value() != header.size()) {
    return make_error(ReasonCode::IoFailure, "could not read the store header");
  }
  if (std::memcmp(header.data(), kMagic, sizeof(kMagic)) != 0) {
    return make_error(ReasonCode::BadMagic, "file does not begin with the Capacity Observatory store magic");
  }
  const std::uint16_t version = static_cast<std::uint16_t>(header[8] | (static_cast<std::uint16_t>(header[9]) << 8U));
  if (version != kFormatVersion) {
    return make_error(ReasonCode::UnsupportedVersion,
                      "store format version " + std::to_string(version) + " is not supported by this build");
  }
  if (crc32c_bytes(header.data(), 12) != load_u32(header.data() + 12)) {
    return make_error(ReasonCode::IntegrityMismatch, "store header integrity check failed");
  }

  std::uint64_t offset = kHeaderBytes;
  std::array<std::uint8_t, kFrameHeaderBytes> frame_header{};
  while (offset < file_size) {
    const std::uint64_t remaining = file_size - offset;
    if (remaining < kFrameHeaderBytes) {
      log.torn_tail = true;
      log.discarded_bytes = remaining;
      break;
    }
    const Result<std::size_t> read = file.read_at(offset, frame_header.data(), frame_header.size());
    if (!read.ok() || read.value() != frame_header.size()) {
      return make_error(ReasonCode::IoFailure, "could not read a frame header");
    }
    const std::uint32_t payload_size = load_u32(frame_header.data());
    const std::uint32_t expected_crc = load_u32(frame_header.data() + 4);

    if (payload_size == 0) {
      // A zero length frame can never be written by this implementation.
      if (find_later_valid_frame(file, offset, file_size, options)) {
        return make_error(ReasonCode::InteriorCorruption,
                          "a zero length frame at offset " + std::to_string(offset) +
                              " is followed by valid frames, so the damage is not a torn tail");
      }
      log.torn_tail = true;
      log.discarded_bytes = file_size - offset;
      break;
    }
    if (payload_size > options.max_frame_bytes) {
      if (offset + kFrameHeaderBytes + payload_size <= file_size) {
        // The frame is fully present in the file but larger than this
        // configuration is willing to read. That is a configuration mismatch,
        // not corruption, and saying so is more useful than truncating it away.
        return make_error(ReasonCode::CapacityExceeded,
                          "stored frame at offset " + std::to_string(offset) + " declares " +
                              std::to_string(payload_size) + " bytes which exceeds the configured maximum of " +
                              std::to_string(options.max_frame_bytes) +
                              "; raise StoreOptions::max_frame_bytes to read this store");
      }
      log.torn_tail = true;
      log.discarded_bytes = file_size - offset;
      break;
    }
    if (offset + kFrameHeaderBytes + payload_size > file_size) {
      // The frame claims more bytes than the file holds: a partially written
      // tail. Nothing can follow it, so truncating at the frame start is safe.
      log.torn_tail = true;
      log.discarded_bytes = file_size - offset;
      break;
    }

    std::vector<std::uint8_t> payload(payload_size);
    const Result<std::size_t> payload_read =
        file.read_at(offset + kFrameHeaderBytes, payload.data(), payload.size());
    if (!payload_read.ok() || payload_read.value() != payload.size()) {
      return make_error(ReasonCode::IoFailure, "could not read a frame payload");
    }
    if (crc32c_bytes(payload.data(), payload.size()) != expected_crc) {
      const std::uint64_t frame_end = offset + kFrameHeaderBytes + payload_size;
      if (frame_end < file_size) {
        // A complete frame followed by more bytes cannot be a torn tail: a torn
        // write leaves an incomplete final record, never a complete one with
        // unexplained bytes after it. Treating this as a tail would silently
        // discard every committed record that follows the damage.
        return make_error(ReasonCode::InteriorCorruption,
                          "frame at offset " + std::to_string(offset) +
                              " fails its integrity check while " + std::to_string(file_size - frame_end) +
                              " byte(s) of later data remain, so the damage is not a torn tail");
      }
      if (find_later_valid_frame(file, offset, file_size, options)) {
        return make_error(ReasonCode::InteriorCorruption,
                          "frame at offset " + std::to_string(offset) +
                              " fails its integrity check and valid frames follow it");
      }
      log.torn_tail = true;
      log.discarded_bytes = file_size - offset;
      break;
    }

    ScannedFrame frame;
    frame.offset = offset;
    frame.payload.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
    log.frames.push_back(std::move(frame));
    offset += kFrameHeaderBytes + payload_size;
    log.valid_bytes = offset;
  }

  report.frames_read = log.frames.size();
  report.torn_tail_recovered = log.torn_tail;
  report.bytes_discarded = log.discarded_bytes;

  if (log.torn_tail) {
    if (writable) {
      const Result<void> truncated = file.truncate(log.valid_bytes);
      if (!truncated.ok()) {
        return truncated.status().with_context("torn tail truncation");
      }
      const Result<void> synced = file.sync();
      if (!synced.ok()) {
        return synced.status().with_context("torn tail truncation");
      }
    }
  }
  return Result<ScannedLog>(std::move(log));
}

Result<void> write_header(io::File& file) {
  std::vector<std::uint8_t> header;
  header.reserve(kHeaderBytes);
  header.insert(header.end(), kMagic, kMagic + sizeof(kMagic));
  header.push_back(static_cast<std::uint8_t>(kFormatVersion & 0xFFU));
  header.push_back(static_cast<std::uint8_t>((kFormatVersion >> 8U) & 0xFFU));
  header.push_back(0);
  header.push_back(0);
  put_u32(header, crc32c_bytes(header.data(), header.size()));
  const Result<void> written = file.write_all(header.data(), header.size());
  if (!written.ok()) {
    return written.status();
  }
  return file.sync();
}

// The session epoch file is a 12 byte integrity checked record: the epoch as a
// little-endian 64 bit integer followed by its CRC-32C.
Result<std::uint64_t> read_epoch_file(const std::string& directory) {
  const std::string path = epoch_path(directory);
  if (!io::exists(path)) {
    return Result<std::uint64_t>(0ULL);
  }
  const Result<std::vector<std::uint8_t>> bytes = io::read_file(path, 64);
  if (!bytes.ok()) {
    return bytes.status();
  }
  if (bytes.value().size() != 12) {
    return make_error(ReasonCode::IntegrityMismatch,
                      "epoch file is " + std::to_string(bytes.value().size()) + " bytes, expected 12");
  }
  if (crc32c_bytes(bytes.value().data(), 8) != load_u32(bytes.value().data() + 8)) {
    return make_error(ReasonCode::IntegrityMismatch, "epoch file integrity check failed");
  }
  return Result<std::uint64_t>(load_u64(bytes.value().data()));
}

Result<void> write_epoch_file(const std::string& directory, std::uint64_t epoch) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(12);
  put_u64(bytes, epoch);
  put_u32(bytes, crc32c_bytes(bytes.data(), bytes.size()));
  return io::write_file_atomic(epoch_path(directory), bytes.data(), bytes.size());
}

}  // namespace

std::string_view store_record_kind_text(StoreRecordKind kind) noexcept {
  switch (kind) {
    case StoreRecordKind::EpochAdvance: return "epoch";
    case StoreRecordKind::Evidence: return "evidence";
    case StoreRecordKind::Snapshot: return "snapshot";
    case StoreRecordKind::Marker: return "marker";
  }
  return "unknown";
}

Store::~Store() { close(); }

Store::Store(Store&& other) noexcept
    : directory_(std::move(other.directory_)),
      options_(std::move(other.options_)),
      log_(std::move(other.log_)),
      lock_(std::move(other.lock_)),
      recovery_(other.recovery_),
      session_epoch_(other.session_epoch_),
      last_sequence_(other.last_sequence_),
      mutations_(std::move(other.mutations_)),
      read_only_(other.read_only_) {
  other.session_epoch_ = Epoch{};
  other.last_sequence_ = SequenceNumber{};
  other.read_only_ = false;
}

Store& Store::operator=(Store&& other) noexcept {
  if (this != &other) {
    close();
    directory_ = std::move(other.directory_);
    options_ = std::move(other.options_);
    log_ = std::move(other.log_);
    lock_ = std::move(other.lock_);
    recovery_ = other.recovery_;
    session_epoch_ = other.session_epoch_;
    last_sequence_ = other.last_sequence_;
    mutations_ = std::move(other.mutations_);
    read_only_ = other.read_only_;
    other.read_only_ = false;
  }
  return *this;
}

void Store::close() noexcept {
  log_.close();
  lock_ = io::ExclusiveLock{};
  mutations_.clear();
  last_sequence_ = SequenceNumber{};
  session_epoch_ = Epoch{};
}

Result<Store> Store::open(const StoreOptions& options) {
  if (options.directory.empty()) {
    return make_error(ReasonCode::InvalidArgument, "store directory is empty");
  }
  const Result<void> ensured = io::ensure_directory(options.directory);
  if (!ensured.ok()) {
    return ensured.status();
  }

  Store store;
  store.directory_ = options.directory;
  store.options_ = options;
  store.read_only_ = options.read_only;

  if (!options.read_only) {
    Result<io::ExclusiveLock> acquired = io::ExclusiveLock::try_acquire(lock_path(options.directory));
    if (!acquired.ok()) {
      return acquired.status();
    }
    store.lock_ = std::move(acquired).value();
  }

  if (!io::exists(log_path(options.directory)) && options.read_only) {
    return make_error(ReasonCode::NotFound, "no store log exists at " + log_path(options.directory));
  }
  // A read-only open asks for read access only. Opening with write access would
  // fail on Windows while another process holds the log, which would make a
  // lock-free inspection of a live store impossible there.
  Result<io::File> opened = options.read_only ? io::File::open_read_only(log_path(options.directory))
                                              : io::File::open(log_path(options.directory), true);
  if (!opened.ok()) {
    return opened.status();
  }
  store.log_ = std::move(opened).value();

  const Result<std::uint64_t> existing = store.log_.size();
  if (!existing.ok()) {
    return existing.status();
  }
  if (existing.value() > options.max_file_bytes) {
    return make_error(ReasonCode::CapacityExceeded,
                      "store log is " + std::to_string(existing.value()) + " bytes which exceeds the limit of " +
                          std::to_string(options.max_file_bytes) + " bytes");
  }

  if (existing.value() == 0) {
    if (options.read_only) {
      RecoveryReport report;
      report.explanation = "store log does not exist yet";
      store.recovery_ = report;
      return Result<Store>(std::move(store));
    }
    const Result<void> header = write_header(store.log_);
    if (!header.ok()) {
      return header.status().with_context("store header creation");
    }
    const Result<void> seek = store.log_.seek_end();
    if (!seek.ok()) {
      return seek.status();
    }
  }

  std::uint64_t last_sequence = 0;
  std::vector<ScannedFrame> frames;
  {
    const Result<ScannedLog> scanned = scan_log(store.log_, options, !options.read_only, store.recovery_);
    if (!scanned.ok()) {
      return scanned.status();
    }
    frames = scanned.value().frames;
  }

  for (const ScannedFrame& frame : frames) {
    const Result<json::Value> parsed = json::Value::parse(frame.payload);
    if (!parsed.ok()) {
      return make_error(ReasonCode::MalformedInput,
                        "frame at offset " + std::to_string(frame.offset) + " is not valid JSON: " +
                            parsed.status().detail());
    }
    const Result<std::string> kind = parsed.value().require_string("kind");
    if (!kind.ok()) {
      return kind.status().with_context("frame at offset " + std::to_string(frame.offset));
    }
    const Result<StoreRecordKind> record_kind = kind_from_name(kind.value());
    if (!record_kind.ok()) {
      return record_kind.status().with_context("frame at offset " + std::to_string(frame.offset));
    }
    const Result<std::int64_t> sequence = parsed.value().require_int("seq");
    if (!sequence.ok()) {
      return sequence.status().with_context("frame at offset " + std::to_string(frame.offset));
    }
    if (sequence.value() < 0) {
      return make_error(ReasonCode::MalformedInput, "frame sequence must not be negative");
    }
    const std::uint64_t seq = static_cast<std::uint64_t>(sequence.value());
    if (seq > last_sequence) {
      last_sequence = seq;
    }

    switch (record_kind.value()) {
      case StoreRecordKind::Evidence: {
        ++store.recovery_.evidence_frames;
        const Result<std::string> mutation = parsed.value().require_string("mutation");
        if (!mutation.ok()) return mutation.status();
        const Result<std::string> digest_hex = parsed.value().require_string("digest");
        if (!digest_hex.ok()) return digest_hex.status();
        const Result<MutationId> mutation_id = MutationId::parse(mutation.value());
        if (!mutation_id.ok()) return mutation_id.status();
        const Result<Digest> digest = Digest::from_hex(digest_hex.value());
        if (!digest.ok()) return digest.status();
        store.mutations_[mutation_id.value()] = digest.value();
        break;
      }
      case StoreRecordKind::Marker: {
        const Result<std::string> mutation = parsed.value().require_string("mutation");
        if (!mutation.ok()) return mutation.status();
        const Result<std::string> digest_hex = parsed.value().require_string("digest");
        if (!digest_hex.ok()) return digest_hex.status();
        const Result<MutationId> mutation_id = MutationId::parse(mutation.value());
        if (!mutation_id.ok()) return mutation_id.status();
        const Result<Digest> digest = Digest::from_hex(digest_hex.value());
        if (!digest.ok()) return digest.status();
        store.mutations_[mutation_id.value()] = digest.value();
        break;
      }
      case StoreRecordKind::Snapshot:
        ++store.recovery_.snapshot_frames;
        break;
      case StoreRecordKind::EpochAdvance: {
        ++store.recovery_.epoch_frames;
        const Result<std::int64_t> epoch = parsed.value().require_int("epoch");
        if (!epoch.ok()) return epoch.status();
        if (epoch.value() < 0) {
          return make_error(ReasonCode::MalformedInput, "stored epoch must not be negative");
        }
        store.session_epoch_ = Epoch(static_cast<std::uint64_t>(epoch.value()));
        store.recovery_.epoch_advanced = true;
        break;
      }
    }
  }

  store.last_sequence_ = SequenceNumber(last_sequence);

  if (!options.read_only) {
    // Resolve the durable session epoch from the log and the epoch file, then
    // publish the next one. Publishing the epoch before any other write means a
    // reader can always tell which session produced a frame.
    const Result<std::uint64_t> published = read_epoch_file(options.directory);
    if (!published.ok()) {
      return published.status();
    }
    std::uint64_t durable_epoch = store.session_epoch_.value();
    if (published.value() > durable_epoch) {
      durable_epoch = published.value();
    }
    if (durable_epoch == (std::numeric_limits<std::uint64_t>::max)()) {
      return make_error(ReasonCode::EpochExhausted, "session epoch counter is exhausted");
    }
    const std::uint64_t next_epoch = durable_epoch + 1;
    const Result<void> epoch_published = write_epoch_file(options.directory, next_epoch);
    if (!epoch_published.ok()) {
      return epoch_published.status().with_context("session epoch publication");
    }
    store.session_epoch_ = Epoch(next_epoch);

    const Result<SequenceNumber> sequence = store.next_sequence();
    if (!sequence.ok()) {
      return sequence.status();
    }
    json::Object members;
    members.emplace_back("v", json::Value(static_cast<std::int64_t>(kFormatVersion)));
    members.emplace_back("kind", json::Value(std::string("epoch")));
    members.emplace_back("seq", json::Value(static_cast<std::int64_t>(sequence.value().value())));
    members.emplace_back("epoch", json::Value(static_cast<std::int64_t>(next_epoch)));
    const std::string payload = json::dump(json::Value::object(std::move(members)), true, -1);
    const Result<AppendOutcome> appended = store.append_frame(StoreRecordKind::EpochAdvance, payload);
    if (!appended.ok()) {
      return appended.status().with_context("session epoch record");
    }
    store.last_sequence_ = sequence.value();
    store.recovery_.epoch_advanced = true;
  }

  store.recovery_.session_epoch = store.session_epoch_;
  store.recovery_.explanation =
      "opened " + options.directory + ": " + std::to_string(store.recovery_.frames_read) + " frame(s), " +
      std::to_string(store.recovery_.evidence_frames) + " evidence, " +
      std::to_string(store.recovery_.bytes_discarded) + " byte(s) discarded" +
      (store.recovery_.torn_tail_recovered ? " (torn tail truncated at the last complete frame)" : "") +
      "; session epoch e" + std::to_string(store.session_epoch_.value());

  const Result<void> seek = store.log_.seek_end();
  if (!seek.ok()) {
    return seek.status();
  }
  return Result<Store>(std::move(store));
}

Result<SequenceNumber> Store::next_sequence() const {
  constexpr std::uint64_t kMaxJsonSequence = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
  if (last_sequence_.value() >= kMaxJsonSequence) {
    return make_error(ReasonCode::EpochExhausted,
                      "the durable sequence counter has reached the largest integer a frame can carry (" +
                          std::to_string(kMaxJsonSequence) + ")");
  }
  return next_number(last_sequence_);
}

Result<AppendOutcome> Store::append_frame(StoreRecordKind kind, const std::string& payload) {
  if (!log_.is_open()) {
    return make_error(ReasonCode::StoreClosed, "store is not open");
  }
  if (read_only_) {
    return make_error(ReasonCode::StoreClosed, "store was opened read only");
  }
  if (payload.size() > options_.max_frame_bytes) {
    return make_error(ReasonCode::RecordTooLarge,
                      "frame payload of " + std::to_string(payload.size()) + " bytes exceeds the limit of " +
                          std::to_string(options_.max_frame_bytes) + " bytes");
  }
  const Result<std::uint64_t> position = log_.position();
  if (!position.ok()) {
    return position.status();
  }
  if (position.value() + kFrameHeaderBytes + payload.size() > options_.max_file_bytes) {
    return make_error(ReasonCode::CapacityExceeded,
                      "appending would grow the store log beyond the limit of " +
                          std::to_string(options_.max_file_bytes) + " bytes");
  }

  std::vector<std::uint8_t> frame;
  frame.reserve(kFrameHeaderBytes + payload.size());
  put_u32(frame, static_cast<std::uint32_t>(payload.size()));
  put_u32(frame, crc32c(payload));
  const Result<void> written = log_.write_all(frame.data(), frame.size());
  if (!written.ok()) {
    return written.status();
  }
  const Result<void> written_payload = log_.write_all(payload.data(), payload.size());
  if (!written_payload.ok()) {
    return written_payload.status();
  }
  // Commit point: after this flush returns, the whole frame is durable.
  const Result<void> synced = log_.sync();
  if (!synced.ok()) {
    return synced.status();
  }

  AppendOutcome outcome;
  outcome.applied = true;
  outcome.reason = ReasonCode::Ok;
  outcome.sequence = last_sequence_;
  outcome.explanation = "committed " + std::string(store_record_kind_text(kind)) + " frame of " +
                        std::to_string(payload.size()) + " bytes";
  return Result<AppendOutcome>(outcome);
}

Result<AppendOutcome> Store::append_evidence(const EvidenceRecord& record, const MutationId& mutation) {
  if (mutation.empty()) {
    return make_error(ReasonCode::InvalidIdentifier, "mutation identity is empty");
  }
  const Result<void> valid = record.validate();
  if (!valid.ok()) {
    return valid.status();
  }
  const Digest digest = record.content_digest();

  const auto existing = mutations_.find(mutation);
  if (existing != mutations_.end()) {
    if (existing->second == digest) {
      AppendOutcome outcome;
      outcome.applied = false;
      outcome.reason = ReasonCode::IdempotentReplay;
      outcome.sequence = last_sequence_;
      outcome.mutation_digest = digest;
      outcome.explanation = "mutation '" + mutation.value() + "' was already committed with digest " +
                            digest.hex().substr(0, 16) + "; no second append was performed";
      return Result<AppendOutcome>(outcome);
    }
    return make_error(ReasonCode::IdempotencyConflict,
                      "mutation '" + mutation.value() + "' was already committed with digest " +
                          existing->second.hex().substr(0, 16) + " but this record has digest " +
                          digest.hex().substr(0, 16));
  }

  // Authority epoch and generation staleness is decided by the acceptance
  // window, not here: the store's session epoch identifies the writer session,
  // while an evidence record's epoch belongs to the authority that asserted it.
  // Conflating the two would refuse legitimate evidence after a store restart.
  // Replay protection at this layer is the mutation identity above and the
  // durable sequence number written into the same frame.

  const Result<SequenceNumber> next = next_sequence();
  if (!next.ok()) {
    return next.status();
  }

  json::Object members;
  members.emplace_back("v", json::Value(static_cast<std::int64_t>(kFormatVersion)));
  members.emplace_back("kind", json::Value(std::string("evidence")));
  members.emplace_back("seq", json::Value(static_cast<std::int64_t>(next.value().value())));
  members.emplace_back("epoch", json::Value(static_cast<std::int64_t>(session_epoch_.value())));
  members.emplace_back("mutation", json::Value(mutation.value()));
  members.emplace_back("digest", json::Value(digest.hex()));
  members.emplace_back("record", encode_evidence(record));
  const std::string payload = json::dump(json::Value::object(std::move(members)), true, -1);

  const Result<AppendOutcome> appended = append_frame(StoreRecordKind::Evidence, payload);
  if (!appended.ok()) {
    return appended.status();
  }
  last_sequence_ = next.value();
  mutations_[mutation] = digest;

  AppendOutcome outcome = appended.value();
  outcome.sequence = last_sequence_;
  outcome.mutation_digest = digest;
  return Result<AppendOutcome>(outcome);
}

Result<AppendOutcome> Store::append_idempotent_marker(const MutationId& mutation, const Digest& digest) {
  if (mutation.empty()) {
    return make_error(ReasonCode::InvalidIdentifier, "mutation identity is empty");
  }
  const auto existing = mutations_.find(mutation);
  if (existing != mutations_.end()) {
    if (existing->second == digest) {
      AppendOutcome outcome;
      outcome.applied = false;
      outcome.reason = ReasonCode::IdempotentReplay;
      outcome.sequence = last_sequence_;
      outcome.mutation_digest = digest;
      outcome.explanation = "mutation '" + mutation.value() + "' was already committed";
      return Result<AppendOutcome>(outcome);
    }
    return make_error(ReasonCode::IdempotencyConflict,
                      "mutation '" + mutation.value() + "' was already committed with a different digest");
  }

  const Result<SequenceNumber> next = next_sequence();
  if (!next.ok()) {
    return next.status();
  }
  json::Object members;
  members.emplace_back("v", json::Value(static_cast<std::int64_t>(kFormatVersion)));
  members.emplace_back("kind", json::Value(std::string("marker")));
  members.emplace_back("seq", json::Value(static_cast<std::int64_t>(next.value().value())));
  members.emplace_back("epoch", json::Value(static_cast<std::int64_t>(session_epoch_.value())));
  members.emplace_back("mutation", json::Value(mutation.value()));
  members.emplace_back("digest", json::Value(digest.hex()));
  const std::string payload = json::dump(json::Value::object(std::move(members)), true, -1);

  const Result<AppendOutcome> appended = append_frame(StoreRecordKind::Marker, payload);
  if (!appended.ok()) {
    return appended.status();
  }
  last_sequence_ = next.value();
  mutations_[mutation] = digest;
  AppendOutcome outcome = appended.value();
  outcome.sequence = last_sequence_;
  outcome.mutation_digest = digest;
  return Result<AppendOutcome>(outcome);
}

Result<std::vector<StoredEvidence>> Store::load_evidence() const {
  RecoveryReport scratch;
  Result<io::File> reader = io::File::open_read_only(log_path(directory_));
  if (!reader.ok()) {
    return reader.status();
  }
  const Result<ScannedLog> scanned = scan_log(reader.value(), options_, false, scratch);
  if (!scanned.ok()) {
    return scanned.status();
  }

  std::vector<StoredEvidence> records;
  for (const ScannedFrame& frame : scanned.value().frames) {
    const Result<json::Value> parsed = json::Value::parse(frame.payload);
    if (!parsed.ok()) {
      return make_error(ReasonCode::MalformedInput, "frame is not valid JSON: " + parsed.status().detail());
    }
    const Result<std::string> kind = parsed.value().require_string("kind");
    if (!kind.ok()) return kind.status();
    const Result<StoreRecordKind> record_kind = kind_from_name(kind.value());
    if (!record_kind.ok()) return record_kind.status();
    if (record_kind.value() != StoreRecordKind::Evidence) {
      continue;
    }
    const Result<std::optional<std::string>> mutation = parsed.value().optional_string("mutation");
    if (!mutation.ok()) return mutation.status();
    const Result<std::string> digest_hex = parsed.value().require_string("digest");
    if (!digest_hex.ok()) return digest_hex.status();
    const Result<std::int64_t> sequence = parsed.value().require_int("seq");
    if (!sequence.ok()) return sequence.status();
    const Result<std::int64_t> epoch = parsed.value().require_int("epoch");
    if (!epoch.ok()) return epoch.status();
    if (sequence.value() < 0 || epoch.value() < 0) {
      return make_error(ReasonCode::MalformedInput, "stored sequence and epoch must not be negative");
    }
    const json::Value* record_node = parsed.value().find("record");
    if (record_node == nullptr) {
      return make_error(ReasonCode::MissingField, "stored evidence frame has no record");
    }
    const Result<EvidenceRecord> record = decode_evidence(*record_node);
    if (!record.ok()) {
      return record.status();
    }
    const Result<MutationId> mutation_id = MutationId::parse(mutation.value().value_or(std::string()));
    if (!mutation_id.ok()) {
      return mutation_id.status();
    }
    const Result<Digest> digest = Digest::from_hex(digest_hex.value());
    if (!digest.ok()) {
      return digest.status();
    }
    StoredEvidence stored;
    stored.record = record.value();
    stored.mutation = mutation_id.value();
    stored.mutation_digest = digest.value();
    stored.sequence = SequenceNumber(static_cast<std::uint64_t>(sequence.value()));
    stored.written_epoch = Epoch(static_cast<std::uint64_t>(epoch.value()));
    records.push_back(std::move(stored));
  }
  return Result<std::vector<StoredEvidence>>(std::move(records));
}

Result<Digest> Store::publish_snapshot(std::string_view document) {
  if (!log_.is_open()) {
    return make_error(ReasonCode::StoreClosed, "store is not open");
  }
  if (read_only_) {
    return make_error(ReasonCode::StoreClosed, "store was opened read only");
  }
  if (document.size() > options_.max_frame_bytes) {
    return make_error(ReasonCode::RecordTooLarge, "snapshot document exceeds the maximum frame size");
  }
  const Result<void> published =
      io::write_file_atomic(snapshot_path(directory_), document.data(), document.size());
  if (!published.ok()) {
    return published.status();
  }
  const Digest digest = sha256(document);
  const Result<SequenceNumber> next = next_sequence();
  if (!next.ok()) {
    return next.status();
  }
  json::Object members;
  members.emplace_back("v", json::Value(static_cast<std::int64_t>(kFormatVersion)));
  members.emplace_back("kind", json::Value(std::string("snapshot")));
  members.emplace_back("seq", json::Value(static_cast<std::int64_t>(next.value().value())));
  members.emplace_back("epoch", json::Value(static_cast<std::int64_t>(session_epoch_.value())));
  members.emplace_back("digest", json::Value(digest.hex()));
  members.emplace_back("bytes", json::Value(static_cast<std::int64_t>(document.size())));
  const std::string payload = json::dump(json::Value::object(std::move(members)), true, -1);
  const Result<AppendOutcome> appended = append_frame(StoreRecordKind::Snapshot, payload);
  if (!appended.ok()) {
    return appended.status();
  }
  last_sequence_ = next.value();
  return Result<Digest>(digest);
}

Result<std::optional<std::string>> Store::read_snapshot() const {
  const std::string path = snapshot_path(directory_);
  if (!io::exists(path)) {
    return Result<std::optional<std::string>>(std::optional<std::string>{});
  }
  const Result<std::vector<std::uint8_t>> bytes = io::read_file(path, options_.max_frame_bytes);
  if (!bytes.ok()) {
    return bytes.status();
  }
  return Result<std::optional<std::string>>(
      std::optional<std::string>(std::string(bytes.value().begin(), bytes.value().end())));
}

Result<RecoveryReport> Store::verify() const {
  return inspect_store(options_);
}

Result<std::uint64_t> Store::durable_bytes() const { return log_.size(); }

Result<RecoveryReport> inspect_store(const StoreOptions& options) {
  RecoveryReport report;
  const std::string path = log_path(options.directory);
  if (!io::exists(path)) {
    report.explanation = "store log does not exist at " + path;
    return Result<RecoveryReport>(report);
  }
  Result<io::File> opened = io::File::open_read_only(path);
  if (!opened.ok()) {
    return opened.status();
  }
  StoreOptions read_options = options;
  read_options.read_only = true;
  const Result<ScannedLog> scanned = scan_log(opened.value(), read_options, false, report);
  if (!scanned.ok()) {
    return scanned.status();
  }
  // Classification happens here as well as in open(): a verification report that
  // claimed zero evidence frames for a log full of them would be worse than no
  // report at all.
  for (const ScannedFrame& frame : scanned.value().frames) {
    const Result<json::Value> parsed = json::Value::parse(frame.payload);
    if (!parsed.ok()) {
      return make_error(ReasonCode::MalformedInput,
                        "frame at offset " + std::to_string(frame.offset) + " is not valid JSON: " +
                            parsed.status().detail());
    }
    const Result<std::string> kind = parsed.value().require_string("kind");
    if (!kind.ok()) {
      return kind.status().with_context("frame at offset " + std::to_string(frame.offset));
    }
    const Result<StoreRecordKind> record_kind = kind_from_name(kind.value());
    if (!record_kind.ok()) {
      return record_kind.status().with_context("frame at offset " + std::to_string(frame.offset));
    }
    switch (record_kind.value()) {
      case StoreRecordKind::Evidence:
        ++report.evidence_frames;
        break;
      case StoreRecordKind::Snapshot:
        ++report.snapshot_frames;
        break;
      case StoreRecordKind::EpochAdvance: {
        ++report.epoch_frames;
        const Result<std::int64_t> epoch = parsed.value().require_int("epoch");
        if (!epoch.ok()) {
          return epoch.status();
        }
        if (epoch.value() >= 0) {
          report.session_epoch = Epoch(static_cast<std::uint64_t>(epoch.value()));
          report.epoch_advanced = true;
        }
        break;
      }
      case StoreRecordKind::Marker:
        break;
    }
  }
  report.explanation = "verified " + std::to_string(report.frames_read) + " frame(s); " +
                       std::to_string(report.evidence_frames) + " evidence, " +
                       std::to_string(report.snapshot_frames) + " snapshot, " +
                       std::to_string(report.epoch_frames) + " epoch" +
                       (report.torn_tail_recovered
                            ? "; torn tail of " + std::to_string(report.bytes_discarded) + " byte(s) discarded"
                            : "");
  return Result<RecoveryReport>(report);
}

}  // namespace co
