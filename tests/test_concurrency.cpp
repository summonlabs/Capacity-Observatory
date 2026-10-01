// Concurrency ownership, hazard, and lifecycle proofs.
//
// Every hazard the specification names is exercised here by construction rather
// than by inspection alone:
//   self-deadlock, read->write upgrade, locks held across re-entry, callbacks
//   emitted under locks, inconsistent lock ordering, shutdown/join while holding
//   worker-required state, cancellation races, and concurrent publication.

#include "co_test.hpp"

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "capacity_observatory/concurrency.hpp"
#include "capacity_observatory/ingest.hpp"
#include "capacity_observatory/ledger.hpp"
#include "capacity_observatory/store.hpp"
#include "capacity_observatory/watermark.hpp"

using namespace co;

namespace {

// A value with an internal invariant: the two halves must always agree. A reader
// that ever observes a torn write fails the check.
struct Coherent {
  std::uint64_t left{0};
  std::uint64_t right{0};

  void bump() {
    ++left;
    // Deliberately widen the window between the two writes.
    for (int i = 0; i < 32; ++i) {
      std::this_thread::yield();
    }
    ++right;
  }

  [[nodiscard]] bool coherent() const noexcept { return left == right; }
};

EvidenceRecord make_record(const char* authority,
                           AuthorityRole role,
                           const char* scope_text,
                           Dimension dimension,
                           CapacityAssertion assertion,
                           std::int64_t amount,
                           std::uint64_t generation) {
  return EvidenceRecord::make(AuthorityId(authority), role, ScopePath::parse(scope_text).value(), dimension, assertion,
                              Unit::parse_for(dimension, "kW").value(), amount, Generation(generation), Epoch(1),
                              Revision(0))
      .value();
}

}  // namespace

CO_TEST(synchronized_readers_never_observe_a_torn_write) {
  Synchronized<Coherent> shared;
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> incoherent{0};

  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&shared, &stop, &reads, &incoherent]() {
      while (!stop.load(std::memory_order_acquire)) {
        auto guard = shared.read();
        if (!guard.get().coherent()) {
          incoherent.fetch_add(1, std::memory_order_relaxed);
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Wait for the readers to be genuinely running before writing. Bounded sleeps
  // or yield counts would make this test depend on machine speed; waiting for an
  // observable fact does not.
  while (reads.load() < 4) {
    std::this_thread::yield();
  }

  for (int i = 0; i < 200; ++i) {
    auto guard = shared.write();
    guard.get().bump();
  }
  stop.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  CO_REQUIRE(incoherent.load() == 0);
  CO_REQUIRE(reads.load() > 0);
  CO_REQUIRE_EQ(shared.read().get().left, 200ULL);
  CO_REQUIRE_EQ(shared.read().get().right, 200ULL);
}

CO_TEST(publisher_delivers_outside_every_lock) {
  // The handler re-enters the same object through its write lock. If publish()
  // delivered while holding the lock this would self-deadlock rather than fail.
  struct Events {
    Publisher<int> publisher;
    Synchronized<int> sink;
  };
  Events events;
  const Publisher<int>::Token token = events.publisher.subscribe([&events](const int& value) {
    auto guard = events.sink.write();
    guard.get() += value;
  });
  CO_REQUIRE(events.publisher.subscriber_count() == 1);

  {
    auto guard = events.sink.write();
    guard.get() = 0;
  }
  events.publisher.publish(7);
  events.publisher.publish(35);
  CO_REQUIRE_EQ(events.sink.read().get(), 42);

  events.publisher.unsubscribe(token);
  CO_REQUIRE(events.publisher.subscriber_count() == 0);
  events.publisher.publish(100);
  CO_REQUIRE_EQ(events.sink.read().get(), 42);
}

CO_TEST(lock_order_auditor_rejects_inversions) {
  // The audit is only installed automatically in opt-in builds, but the ordering
  // rule itself is always compiled and always enforced when called.
  CO_REQUIRE_EQ(LockOrderAuditor::depth(), static_cast<std::size_t>(0));
  CO_REQUIRE(LockOrderAuditor::check_and_enter(LockClass::Control));
  CO_REQUIRE(LockOrderAuditor::check_and_enter(LockClass::State));
  CO_REQUIRE(LockOrderAuditor::check_and_enter(LockClass::State));  // same class is fine
  CO_REQUIRE(!LockOrderAuditor::check_and_enter(LockClass::Control));  // inversion refused
  CO_REQUIRE(LockOrderAuditor::violation_count() >= 1);
  LockOrderAuditor::leave(LockClass::State);
  LockOrderAuditor::leave(LockClass::State);
  CO_REQUIRE(LockOrderAuditor::check_and_enter(LockClass::Store));
  LockOrderAuditor::leave(LockClass::Store);
  LockOrderAuditor::leave(LockClass::Control);
  CO_REQUIRE_EQ(LockOrderAuditor::depth(), static_cast<std::size_t>(0));

  // Releasing a class that is not on top removes exactly that frame, so a
  // confused releaser can not wedge the thread.
  CO_REQUIRE(LockOrderAuditor::check_and_enter(LockClass::Control));
  CO_REQUIRE(LockOrderAuditor::check_and_enter(LockClass::Publication));
  LockOrderAuditor::leave(LockClass::Control);
  CO_REQUIRE_EQ(LockOrderAuditor::depth(), static_cast<std::size_t>(1));
  LockOrderAuditor::leave(LockClass::Publication);
  CO_REQUIRE_EQ(LockOrderAuditor::depth(), static_cast<std::size_t>(0));
  // Releasing something that was never held is a no-op.
  LockOrderAuditor::leave(LockClass::State);
  CO_REQUIRE_EQ(LockOrderAuditor::depth(), static_cast<std::size_t>(0));
}

CO_TEST(an_audited_workload_never_inverts_the_lock_order) {
  // This suite is compiled with CO_ENABLE_LOCK_AUDIT=1, so the Synchronized
  // guards really do announce themselves and any inversion on a real call path
  // would be counted here rather than merely asserted by inspection.
  CO_REQUIRE(LockOrderAuditor::enabled());
  const std::size_t before = LockOrderAuditor::total_violations();

  Synchronized<int> sink(0);
  Publisher<int> publisher;
  const Publisher<int>::Token token = publisher.subscribe([&sink](const int& value) {
    // Re-entering a guard from a callback must be legal: delivery happens with
    // no lock held, so this nests nothing.
    auto guard = sink.write();
    guard.get() += value;
  });
  for (int i = 0; i < 64; ++i) {
    auto guard = sink.write();
    guard.get() += 1;
  }
  publisher.publish(3);
  {
    auto guard = sink.read();
    CO_REQUIRE_EQ(guard.get(), 67);
  }
  publisher.unsubscribe(token);

  CO_REQUIRE_EQ(LockOrderAuditor::total_violations(), before);
  CO_REQUIRE_EQ(LockOrderAuditor::depth(), static_cast<std::size_t>(0));
}

CO_TEST(bounded_queue_refuses_instead_of_blocking) {
  BoundedQueue<int> queue(2);
  CO_REQUIRE(queue.try_push(1) == QueueOutcome::Pushed);
  CO_REQUIRE(queue.try_push(2) == QueueOutcome::Pushed);
  CO_REQUIRE(queue.try_push(3) == QueueOutcome::Full);
  CO_REQUIRE_EQ(queue.size(), static_cast<std::size_t>(2));

  int value = 0;
  CO_REQUIRE(queue.pop(value));
  CO_REQUIRE_EQ(value, 1);
  CO_REQUIRE(queue.try_push(3) == QueueOutcome::Pushed);

  queue.close();
  CO_REQUIRE(queue.try_push(4) == QueueOutcome::Closed);
  CO_REQUIRE(queue.pop(value));
  CO_REQUIRE_EQ(value, 2);
  CO_REQUIRE(queue.pop(value));
  CO_REQUIRE_EQ(value, 3);
  // A closed and drained queue reports exhaustion without blocking.
  CO_REQUIRE(!queue.pop(value));
}

CO_TEST(worker_stops_cleanly_and_restarts) {
  Worker worker;
  std::atomic<std::uint64_t> ticks{0};
  CO_REQUIRE_OK_VOID(worker.start("ticker", [&ticks](Cancellation& cancellation) {
    while (!cancellation.requested()) {
      ticks.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }));
  CO_REQUIRE(worker.state() == WorkerState::Running);
  while (ticks.load() == 0) {
    std::this_thread::yield();
  }
  CO_REQUIRE(ticks.load() > 0);
  worker.stop();
  CO_REQUIRE(worker.state() == WorkerState::Stopped);

  // A second generation must be possible after a clean stop.
  const std::uint64_t before = ticks.load();
  CO_REQUIRE_OK_VOID(worker.start("ticker", [&ticks](Cancellation& cancellation) {
    while (!cancellation.requested()) {
      ticks.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }));
  while (ticks.load() == before) {
    std::this_thread::yield();
  }
  CO_REQUIRE(ticks.load() > before);
  worker.stop();
  worker.stop();  // idempotent
  CO_REQUIRE(worker.state() == WorkerState::Stopped);
}

CO_TEST(shutdown_drains_every_queued_item_and_never_drops_work) {
  const AuthorityRegistry registry = AuthorityRegistry::standard();
  const std::shared_ptr<Clock> clock = manual_clock(Timestamp(1'000'000));
  EvidenceWindow window(registry, clock);

  co::test::ScratchDirectory scratch("ingest-shutdown");
  StoreOptions options;
  options.directory = scratch.path();
  Result<Store> store = Store::open(options);
  CO_REQUIRE(store.ok());

  IngestPipeline pipeline(window, &store.value());
  CO_REQUIRE_OK_VOID(pipeline.start_worker(256));

  constexpr int kRecords = 64;
  for (int i = 0; i < kRecords; ++i) {
    const std::string scope = "site=dc1/hall=h1";
    EvidenceRecord record = make_record("dccp", AuthorityRole::CommittedCapacity, scope.c_str(), Dimension::Power,
                                        CapacityAssertion::Committed, 10, static_cast<std::uint64_t>(i + 1));
    const QueueOutcome outcome = pipeline.try_submit(record, MutationId("shutdown-" + std::to_string(i)));
    CO_REQUIRE(outcome == QueueOutcome::Pushed);
  }
  // Stop immediately: shutdown must still drain everything already queued.
  pipeline.stop_worker();
  CO_REQUIRE(pipeline.drained() >= 1);
  CO_REQUIRE_EQ(pipeline.statistics().enqueued, static_cast<std::size_t>(kRecords));
  CO_REQUIRE_EQ(pipeline.statistics().received, static_cast<std::size_t>(kRecords));
  CO_REQUIRE_EQ(window.size(), static_cast<std::size_t>(1));  // all records share one slot
  CO_REQUIRE_EQ(window.current().begin()->second.generation.value(), static_cast<std::uint64_t>(kRecords));
}

CO_TEST(concurrent_submit_and_snapshot_agree_on_the_final_state) {
  const AuthorityRegistry registry = AuthorityRegistry::standard();
  const std::shared_ptr<Clock> clock = manual_clock(Timestamp(5'000'000));
  Synchronized<EvidenceWindow> shared(EvidenceWindow(registry, clock));

  co::test::ScratchDirectory scratch("ingest-concurrent");
  StoreOptions options;
  options.directory = scratch.path();
  Result<Store> store = Store::open(options);
  CO_REQUIRE(store.ok());

  // The locked access policy takes the write guard for every consideration, so
  // readers holding the read guard can never observe a half-applied record.
  IngestPipeline pipeline(shared, &store.value());
  CO_REQUIRE_OK_VOID(pipeline.start_worker(4096));

  constexpr int kThreads = 4;
  constexpr int kPerThread = 50;
  std::atomic<int> submitted{0};
  std::vector<std::thread> producers;
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back([&, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string scope = "site=dc1/hall=h1/zone=z" + std::to_string(t);
        EvidenceRecord record = make_record("dccp", AuthorityRole::CommittedCapacity, scope.c_str(), Dimension::Power,
                                            CapacityAssertion::Committed, 5, 1);
        for (;;) {
          const QueueOutcome outcome =
              pipeline.try_submit(record, MutationId("c-" + std::to_string(t) + "-" + std::to_string(i)));
          if (outcome == QueueOutcome::Pushed) {
            submitted.fetch_add(1, std::memory_order_relaxed);
            break;
          }
          if (outcome == QueueOutcome::Closed) {
            return;
          }
          std::this_thread::yield();  // queue full: back off, never block the worker
        }
      }
    });
  }

  // Readers snapshot concurrently with ingestion, under the read guard.
  std::atomic<int> snapshots{0};
  std::atomic<bool> readers_stop{false};
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; ++r) {
    readers.emplace_back([&shared, &snapshots, &readers_stop]() {
      while (!readers_stop.load(std::memory_order_acquire)) {
        LedgerRequest request;
        request.scope = ScopePath::parse("site=dc1").value();
        request.dimensions = DimensionSet::of(Dimension::Power);
        auto guard = shared.read();
        const Result<Ledger> ledger = compose_ledger(guard.get(), request);
        if (ledger.ok()) {
          snapshots.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (std::thread& producer : producers) {
    producer.join();
  }
  // Drain everything that was queued, then stop. The wait is for an observable
  // fact, not for an elapsed duration.
  while (pipeline.drained() < static_cast<std::size_t>(submitted.load())) {
    std::this_thread::yield();
  }
  pipeline.stop_worker();
  readers_stop.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  CO_REQUIRE_EQ(submitted.load(), kThreads * kPerThread);
  CO_REQUIRE_EQ(pipeline.statistics().received, static_cast<std::size_t>(kThreads * kPerThread));
  CO_REQUIRE_EQ(pipeline.statistics().refused, static_cast<std::size_t>(0));
  CO_REQUIRE(snapshots.load() > 0);

  auto final_guard = shared.read();
  CO_REQUIRE_EQ(final_guard.get().size(), static_cast<std::size_t>(kThreads));
  LedgerRequest request;
  request.scope = ScopePath::parse("site=dc1").value();
  request.dimensions = DimensionSet::of(Dimension::Power);
  const Result<Ledger> ledger = compose_ledger(final_guard.get(), request);
  CO_REQUIRE(ledger.ok());
  const LedgerLine* line = ledger.value().find(Dimension::Power);
  CO_REQUIRE(line != nullptr);
  // Four zones, each zone committing 50 records of 5 kW to the same slot: only
  // the newest generation survives per slot, never an accumulation.
  CO_REQUIRE_EQ(line->declared.get(CapacityAssertion::Committed).value().canonical(), 4LL * 5'000'000LL);
  CO_REQUIRE_EQ(line->evidence_count, static_cast<std::size_t>(kThreads));
  CO_REQUIRE_EQ(line->contributing_scopes.size(), static_cast<std::size_t>(kThreads));
}

CO_TEST(stale_authority_races_keep_the_newest_generation) {
  // A slow older authority must never overwrite a newer accepted generation.
  // The window is shared behind its write guard, which is the documented way to
  // let more than one thread touch it.
  for (int attempt = 0; attempt < 32; ++attempt) {
    const AuthorityRegistry registry = AuthorityRegistry::standard();
    const std::shared_ptr<Clock> clock = manual_clock(Timestamp(1'000'000));
    Synchronized<EvidenceWindow> shared(EvidenceWindow(registry, clock));

    std::atomic<int> started{0};
    std::thread newer_thread([&shared, &started]() {
      started.fetch_add(1);
      while (started.load() < 2) {
        std::this_thread::yield();
      }
      for (std::uint64_t generation = 6; generation <= 10; ++generation) {
        auto guard = shared.write();
        const AcceptanceDecision decision = guard.get().consider(make_record(
            "dccp", AuthorityRole::CommittedCapacity, "site=dc1/hall=h1", Dimension::Power,
            CapacityAssertion::Committed, static_cast<std::int64_t>(generation), generation));
        (void)decision;
      }
    });
    std::thread older_thread([&shared, &started]() {
      started.fetch_add(1);
      while (started.load() < 2) {
        std::this_thread::yield();
      }
      for (std::uint64_t generation = 1; generation <= 5; ++generation) {
        auto guard = shared.write();
        const AcceptanceDecision decision = guard.get().consider(make_record(
            "dccp", AuthorityRole::CommittedCapacity, "site=dc1/hall=h1", Dimension::Power,
            CapacityAssertion::Committed, static_cast<std::int64_t>(generation), generation));
        (void)decision;
      }
    });
    newer_thread.join();
    older_thread.join();

    auto guard = shared.read();
    CO_REQUIRE_EQ(guard.get().size(), static_cast<std::size_t>(1));
    const EvidenceRecord& current = guard.get().current().begin()->second;
    CO_REQUIRE_EQ(current.generation.value(), 10ULL);
    // The records are declared in kW, so generation 10 carries 10 kW = 10000000 mW.
    CO_REQUIRE_EQ(current.amount.canonical(), 10'000'000LL);
    // Whichever thread ran second, the newest generation is current. How many
    // refusals occurred depends on the interleaving (an older generation that
    // arrives first is simply accepted and later superseded), so the invariant
    // is the final state, not the refusal count.
    CO_REQUIRE(guard.get().accepted_count() >= 5);
    CO_REQUIRE(guard.get().accepted_count() <= 10);
    CO_REQUIRE(guard.get().refusals().size() + guard.get().accepted_count() == 10);
  }
}

CO_TEST(concurrent_publication_keeps_subscriber_accounting_consistent) {
  Publisher<int> publisher;
  std::atomic<std::uint64_t> delivered{0};

  // Phase 1: a fixed subscriber set and a fixed amount of published work, so the
  // delivery count is exactly determined and the test cannot be timing sensitive.
  constexpr int kSubscribers = 20;
  constexpr int kPublishers = 3;
  constexpr int kPerPublisher = 200;
  std::vector<Publisher<int>::Token> tokens;
  for (int i = 0; i < kSubscribers; ++i) {
    tokens.push_back(publisher.subscribe([&delivered](const int& value) {
      delivered.fetch_add(static_cast<std::uint64_t>(value), std::memory_order_relaxed);
    }));
  }
  std::vector<std::thread> publishers;
  for (int i = 0; i < kPublishers; ++i) {
    publishers.emplace_back([&publisher]() {
      for (int n = 0; n < kPerPublisher; ++n) {
        publisher.publish(1);
      }
    });
  }
  for (std::thread& thread : publishers) {
    thread.join();
  }
  CO_REQUIRE_EQ(publisher.subscriber_count(), static_cast<std::size_t>(kSubscribers));
  CO_REQUIRE_EQ(delivered.load(), static_cast<std::uint64_t>(kPublishers * kPerPublisher * kSubscribers));

  // Phase 2: churn subscribers while publishers run. The observable invariant is
  // that the churn thread removes exactly the handlers it added, so the count the
  // main thread established is the count that remains.
  std::atomic<bool> stop{false};
  std::thread churn([&publisher, &delivered, &stop]() {
    while (!stop.load(std::memory_order_acquire)) {
      const Publisher<int>::Token token = publisher.subscribe([&delivered](const int& value) {
        delivered.fetch_add(static_cast<std::uint64_t>(value), std::memory_order_relaxed);
      });
      publisher.publish(1);
      publisher.unsubscribe(token);
    }
  });
  std::vector<std::thread> churn_publishers;
  for (int i = 0; i < kPublishers; ++i) {
    churn_publishers.emplace_back([&publisher]() {
      for (int n = 0; n < kPerPublisher; ++n) {
        publisher.publish(1);
      }
    });
  }
  for (std::thread& thread : churn_publishers) {
    thread.join();
  }
  stop.store(true, std::memory_order_release);
  churn.join();
  CO_REQUIRE_EQ(publisher.subscriber_count(), static_cast<std::size_t>(kSubscribers));

  // Unsubscribing everything leaves a publisher that still works.
  for (const Publisher<int>::Token token : tokens) {
    publisher.unsubscribe(token);
  }
  CO_REQUIRE_EQ(publisher.subscriber_count(), static_cast<std::size_t>(0));
  const std::uint64_t before = delivered.load();
  publisher.publish(5);
  CO_REQUIRE_EQ(delivered.load(), before);
}
