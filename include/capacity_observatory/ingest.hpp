#pragma once
// Capacity Observatory - ingest pipeline.
//
// Ingest is the only path by which evidence enters derived state. Every record
// is validated, attributed to an authority, ordered against the slot watermark,
// made durable (when a store is attached), and only then applied. A record that
// fails any step is reported with its reason and leaves derived state untouched.

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "capacity_observatory/concurrency.hpp"
#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/store.hpp"
#include "capacity_observatory/time.hpp"
#include "capacity_observatory/watermark.hpp"

namespace co {

enum class IngestDisposition : std::uint8_t {
  Applied = 0,       // durable (when attached) and now drives derived state
  Duplicate = 1,     // already applied; idempotent no-op
  Refused = 2,       // rejected by the acceptance window
  Durable = 3        // durably recorded but not applied (superseded evidence)
};

[[nodiscard]] std::string_view ingest_disposition_text(IngestDisposition disposition) noexcept;

struct IngestResult {
  IngestDisposition disposition{IngestDisposition::Refused};
  ReasonCode reason{ReasonCode::Ok};
  std::string explanation;
  AcceptanceDecision acceptance;
  bool durable{false};
  bool applied{false};
  SequenceNumber sequence;
};

struct IngestStatistics {
  std::size_t received{0};
  std::size_t applied{0};
  std::size_t duplicates{0};
  std::size_t refused{0};
  std::size_t durable_only{0};
  std::size_t enqueued{0};
  std::size_t queue_full{0};
};

// The pipeline never assumes how the evidence window is guarded. The caller
// supplies the access policy explicitly, so ownership is visible at the
// construction site instead of being an undocumented precondition:
//   * DirectWindowAccess  - the window is confined to the thread that drives
//     ingest, which is the single-worker case.
//   * LockedWindowAccess  - the window is shared behind a Synchronized guard and
//     every consideration takes the write guard.
class WindowAccess {
 public:
  WindowAccess(const WindowAccess&) = delete;
  WindowAccess& operator=(const WindowAccess&) = delete;
  virtual ~WindowAccess() = default;
  [[nodiscard]] virtual AcceptanceDecision consider(const EvidenceRecord& record) = 0;
  [[nodiscard]] virtual const AuthorityRegistry& registry() const = 0;
  [[nodiscard]] virtual std::shared_ptr<Clock> clock() const = 0;
  [[nodiscard]] virtual std::size_t window_size() const = 0;

 protected:
  WindowAccess() = default;
};

class DirectWindowAccess final : public WindowAccess {
 public:
  explicit DirectWindowAccess(EvidenceWindow& window) : window_(&window) {}
  [[nodiscard]] AcceptanceDecision consider(const EvidenceRecord& record) override {
    return window_->consider(record);
  }
  [[nodiscard]] const AuthorityRegistry& registry() const override { return window_->registry(); }
  [[nodiscard]] std::shared_ptr<Clock> clock() const override { return window_->clock(); }
  [[nodiscard]] std::size_t window_size() const override { return window_->size(); }

 private:
  EvidenceWindow* window_;
};

class LockedWindowAccess final : public WindowAccess {
 public:
  explicit LockedWindowAccess(Synchronized<EvidenceWindow>& window) : window_(&window) {}
  [[nodiscard]] AcceptanceDecision consider(const EvidenceRecord& record) override {
    auto guard = window_->write();
    return guard.get().consider(record);
  }
  [[nodiscard]] const AuthorityRegistry& registry() const override { return window_->read().get().registry(); }
  [[nodiscard]] std::shared_ptr<Clock> clock() const override { return window_->read().get().clock(); }
  [[nodiscard]] std::size_t window_size() const override { return window_->read().get().size(); }

 private:
  Synchronized<EvidenceWindow>* window_;
};

// Ingest attaches a store, so the durable commit happens before the evidence is
// allowed to influence derived state. That ordering is the whole point: a
// crash between the two can lose a derivation but can never fabricate one.
class IngestPipeline {
 public:
  // Single-threaded (direct) window.
  IngestPipeline(EvidenceWindow& window, Store* store);
  // Window shared with concurrent readers.
  IngestPipeline(Synchronized<EvidenceWindow>& window, Store* store);
  // Custom access policy; must not be null.
  IngestPipeline(std::unique_ptr<WindowAccess> access, Store* store);

  [[nodiscard]] const AuthorityRegistry& registry() const { return access_->registry(); }

  // Synchronous, single-record ingest. Deterministic and reentrancy free.
  [[nodiscard]] Result<IngestResult> ingest(const EvidenceRecord& record, const MutationId& mutation);

  // Async ingest: a bounded queue plus one worker. try_submit never blocks; a
  // full queue is reported so the caller can refuse the work rather than grow
  // without bound.
  [[nodiscard]] Result<void> start_worker(std::size_t queue_capacity = 1024);
  void stop_worker() noexcept;
  [[nodiscard]] QueueOutcome try_submit(EvidenceRecord record, MutationId mutation);
  [[nodiscard]] std::size_t drained() const;

  [[nodiscard]] IngestStatistics statistics() const { return statistics_.read().get(); }
  [[nodiscard]] std::vector<std::string> worker_errors() const { return worker_errors_.read().get(); }
  [[nodiscard]] WorkerState worker_state() const noexcept { return worker_.state(); }

 private:
  void drain_one(const EvidenceRecord& record, const MutationId& mutation);

  std::unique_ptr<WindowAccess> access_;
  Store* store_;
  Worker worker_;
  std::unique_ptr<BoundedQueue<std::pair<EvidenceRecord, MutationId>>> queue_;
  Synchronized<IngestStatistics> statistics_;
  Synchronized<std::vector<std::string>> worker_errors_;
  std::atomic<std::size_t> drained_{0};
};

}  // namespace co
