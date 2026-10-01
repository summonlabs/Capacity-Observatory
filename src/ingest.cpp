// Capacity Observatory - ingest pipeline implementation.
#include "capacity_observatory/ingest.hpp"

#include <memory>
#include <string>
#include <utility>

namespace co {

std::string_view ingest_disposition_text(IngestDisposition disposition) noexcept {
  switch (disposition) {
    case IngestDisposition::Applied: return "applied";
    case IngestDisposition::Duplicate: return "duplicate";
    case IngestDisposition::Refused: return "refused";
    case IngestDisposition::Durable: return "durable-not-applied";
  }
  return "unknown";
}

IngestPipeline::IngestPipeline(EvidenceWindow& window, Store* store)
    : access_(std::make_unique<DirectWindowAccess>(window)), store_(store) {}

IngestPipeline::IngestPipeline(Synchronized<EvidenceWindow>& window, Store* store)
    : access_(std::make_unique<LockedWindowAccess>(window)), store_(store) {}

IngestPipeline::IngestPipeline(std::unique_ptr<WindowAccess> access, Store* store)
    : access_(std::move(access)), store_(store) {
  // A null policy is a programming defect, not a runtime condition: without an
  // access policy the pipeline could not consider any evidence at all.
  if (access_ == nullptr) {
    fatal_status("IngestPipeline", make_error(ReasonCode::InvariantViolation, "window access policy is null"));
  }
}

Result<IngestResult> IngestPipeline::ingest(const EvidenceRecord& record, const MutationId& mutation) {
  IngestResult result;
  {
    auto statistics = statistics_.write();
    ++statistics->received;
  }

  // Durability first. Derived state may only ever be built from evidence that
  // already survived a durable commit, so a crash cannot produce a derivation
  // whose underlying evidence never existed.
  if (store_ != nullptr) {
    const Result<AppendOutcome> appended = store_->append_evidence(record, mutation);
    if (!appended.ok()) {
      result.disposition = IngestDisposition::Refused;
      result.reason = appended.status().code();
      result.explanation = appended.status().render();
      auto statistics = statistics_.write();
      ++statistics->refused;
      return Result<IngestResult>(result);
    }
    result.durable = true;
    result.sequence = appended.value().sequence;
    if (!appended.value().applied) {
      result.disposition = IngestDisposition::Duplicate;
      result.reason = ReasonCode::IdempotentReplay;
      result.explanation = appended.value().explanation;
      auto statistics = statistics_.write();
      ++statistics->duplicates;
      return Result<IngestResult>(result);
    }
  }

  const AcceptanceDecision decision = access_->consider(record);
  result.acceptance = decision;
  result.reason = decision.reason;
  result.explanation = decision.explanation;

  if (decision.accepted()) {
    result.disposition = IngestDisposition::Applied;
    result.applied = true;
    auto statistics = statistics_.write();
    ++statistics->applied;
    return Result<IngestResult>(result);
  }
  if (decision.outcome == AcceptanceOutcome::Duplicate) {
    result.disposition = IngestDisposition::Duplicate;
    auto statistics = statistics_.write();
    ++statistics->duplicates;
    return Result<IngestResult>(result);
  }

  result.disposition = result.durable ? IngestDisposition::Durable : IngestDisposition::Refused;
  auto statistics = statistics_.write();
  if (result.durable) {
    ++statistics->durable_only;
  } else {
    ++statistics->refused;
  }
  return Result<IngestResult>(result);
}

Result<void> IngestPipeline::start_worker(std::size_t queue_capacity) {
  if (queue_ != nullptr) {
    return make_error(ReasonCode::AlreadyExists, "ingest worker is already started");
  }
  queue_ = std::make_unique<BoundedQueue<std::pair<EvidenceRecord, MutationId>>>(queue_capacity);
  BoundedQueue<std::pair<EvidenceRecord, MutationId>>* queue = queue_.get();

  const Result<void> started = worker_.start("ingest", [this, queue](Cancellation& cancellation) {
    for (;;) {
      std::pair<EvidenceRecord, MutationId> item;
      const bool popped = queue->pop(item);
      if (!popped) {
        return;  // queue closed
      }
      drain_one(item.first, item.second);
      drained_.fetch_add(1, std::memory_order_relaxed);
      if (cancellation.requested()) {
        // Drain whatever is already queued, then stop; queued work is never
        // silently dropped.
        while (queue->pop(item)) {
          drain_one(item.first, item.second);
          drained_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
      }
    }
  });
  if (!started.ok()) {
    queue_.reset();
    return started.status();
  }
  return Result<void>{};
}

void IngestPipeline::stop_worker() noexcept {
  // Close first so the worker's blocking pop unblocks even with an empty queue,
  // then cancel, then join from outside every lock.
  if (queue_ != nullptr) {
    queue_->close();
  }
  worker_.stop();
}

QueueOutcome IngestPipeline::try_submit(EvidenceRecord record, MutationId mutation) {
  if (queue_ == nullptr) {
    return QueueOutcome::Closed;
  }
  const QueueOutcome outcome = queue_->try_push(std::make_pair(std::move(record), std::move(mutation)));
  auto statistics = statistics_.write();
  if (outcome == QueueOutcome::Pushed) {
    ++statistics->enqueued;
  } else if (outcome == QueueOutcome::Full) {
    ++statistics->queue_full;
  }
  return outcome;
}

std::size_t IngestPipeline::drained() const { return drained_.load(std::memory_order_relaxed); }

void IngestPipeline::drain_one(const EvidenceRecord& record, const MutationId& mutation) {
  const Result<IngestResult> outcome = ingest(record, mutation);
  if (!outcome.ok()) {
    auto errors = worker_errors_.write();
    if (errors->size() < 64) {
      errors->push_back(record.id_text() + ": " + outcome.status().render());
    }
    return;
  }
  if (outcome.value().disposition == IngestDisposition::Refused) {
    auto errors = worker_errors_.write();
    if (errors->size() < 64) {
      errors->push_back(record.id_text() + ": " + outcome.value().explanation);
    }
  }
}

}  // namespace co
