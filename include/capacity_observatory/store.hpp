#pragma once
// Capacity Observatory - durable, versioned, integrity checked evidence store.
//
// Layout of a store directory:
//   evidence.log   append-only frame log; the only file that grows
//   snapshot.json  atomically published derived snapshot (optional, disposable)
//   epoch          atomically published writer session epoch
//   writer.lock    kernel enforced single-writer lock file
//
// The commit point for an appended record is a successful flush of the complete
// frame: before that call returns, the record is either fully durable or not
// durable at all. A torn tail is truncated conservatively on reopen; interior
// corruption is rejected rather than skipped.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/io.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/strong.hpp"
#include "capacity_observatory/time.hpp"

namespace co {

enum class StoreRecordKind : std::uint8_t { EpochAdvance = 1, Evidence = 2, Snapshot = 3, Marker = 4 };

[[nodiscard]] std::string_view store_record_kind_text(StoreRecordKind kind) noexcept;

struct StoreOptions {
  std::string directory;
  // Refuses frames larger than this rather than growing without bound.
  std::uint64_t max_frame_bytes{1U << 20U};
  // Refuses to open (and refuses to append to) logs larger than this.
  std::uint64_t max_file_bytes{1ULL << 32U};
  // Read-only opens skip the writer lock and never write anything.
  bool read_only{false};
};

struct RecoveryReport {
  bool torn_tail_recovered{false};
  std::uint64_t bytes_discarded{0};
  std::uint64_t frames_read{0};
  std::uint64_t evidence_frames{0};
  std::uint64_t snapshot_frames{0};
  std::uint64_t epoch_frames{0};
  Epoch session_epoch;
  bool epoch_advanced{false};
  std::string explanation;
};

// Everything the store durably knows about one evidence slot.
struct StoredEvidence {
  EvidenceRecord record;
  // Identity of the mutation that produced this record. Kept in the same durable
  // frame as the record so a retry can be answered without a second lookup.
  MutationId mutation;
  Digest mutation_digest;
  SequenceNumber sequence;
  Epoch written_epoch;
};

struct AppendOutcome {
  bool applied{false};
  ReasonCode reason{ReasonCode::Ok};
  std::string explanation;
  SequenceNumber sequence;
  Digest mutation_digest;
};

class Store {
 public:
  Store() = default;
  ~Store();
  Store(Store&& other) noexcept;
  Store& operator=(Store&& other) noexcept;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  [[nodiscard]] static Result<Store> open(const StoreOptions& options);

  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept { return log_.is_open(); }
  [[nodiscard]] const std::string& directory() const noexcept { return directory_; }
  [[nodiscard]] Epoch session_epoch() const noexcept { return session_epoch_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }

  // Appends an evidence record. Idempotent by mutation identity: replaying the
  // same identity with the same content is answered with IdempotentReplay and
  // no second append; the same identity with different content is refused with
  // IdempotencyConflict. A record whose epoch is behind the durable epoch is
  // refused with StaleEpochReplay.
  [[nodiscard]] Result<AppendOutcome> append_evidence(const EvidenceRecord& record, const MutationId& mutation);

  // Reads every durable evidence frame in commit order.
  [[nodiscard]] Result<std::vector<StoredEvidence>> load_evidence() const;

  // Publishes a derived snapshot document atomically and records its digest in
  // the log. Snapshots are disposable: deleting the file never loses evidence.
  [[nodiscard]] Result<Digest> publish_snapshot(std::string_view document);

  [[nodiscard]] Result<std::optional<std::string>> read_snapshot() const;

  // Re-reads the log from disk and verifies every frame.
  [[nodiscard]] Result<RecoveryReport> verify() const;

  [[nodiscard]] Result<std::uint64_t> durable_bytes() const;

  // Durably records the mutation identity as applied without an evidence
  // payload. Used by callers that commit their own payload in the same call.
  [[nodiscard]] Result<AppendOutcome> append_idempotent_marker(const MutationId& mutation, const Digest& digest);

  [[nodiscard]] const std::map<MutationId, Digest>& mutations() const noexcept { return mutations_; }
  [[nodiscard]] SequenceNumber last_sequence() const noexcept { return last_sequence_; }

 private:
  [[nodiscard]] Result<AppendOutcome> append_frame(StoreRecordKind kind, const std::string& payload);
  // Sequence numbers are serialized as signed JSON integers, so the durable
  // counter is refused at INT64_MAX rather than wrapping into a negative number
  // that this build would then refuse to read back.
  [[nodiscard]] Result<SequenceNumber> next_sequence() const;

  std::string directory_;
  StoreOptions options_;
  io::File log_;
  io::ExclusiveLock lock_;
  RecoveryReport recovery_;
  Epoch session_epoch_;
  SequenceNumber last_sequence_;
  std::map<MutationId, Digest> mutations_;
  bool read_only_{false};
};

// Reads a store directory without taking the writer lock. Used by inspection
// commands and by the verification path.
[[nodiscard]] Result<RecoveryReport> inspect_store(const StoreOptions& options);

}  // namespace co
