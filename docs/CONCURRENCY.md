# Capacity Observatory - Concurrency

This document states the ownership rules, the lock order, the worker lifecycle,
and an audit of every hazard the design names. It describes the code in
`include/capacity_observatory/concurrency.hpp`, `src/concurrency.cpp`,
`include/capacity_observatory/ingest.hpp`, and `src/ingest.cpp`, and it cites
the test cases in `tests/test_concurrency.cpp` that cover each hazard.

## 1. What is concurrent, and what is confined

The library is a single-process runtime. Exactly one component owns a thread:
`co::IngestPipeline`, through `co::Worker`. Everything else is a plain value
or a container that the caller must confine.

| type | sharing rule |
|---|---|
| `co::Synchronized<T>` | the only sanctioned way to share mutable state between threads |
| `co::BoundedQueue<T>` | internally synchronized; safe to share |
| `co::Publisher<Event>` | internally synchronized; safe to share |
| `co::Worker` | owns one thread; the owner calls start/stop |
| `co::Cancellation` | a single atomic flag; safe to share |
| `co::EvidenceWindow` | thread-confined by design: it owns mutable maps and is deliberately not internally synchronized (`include/capacity_observatory/watermark.hpp`). Confine it to one thread, or place it in a `Synchronized<EvidenceWindow>` and take a read guard around readers |
| `co::Store` | no internal locking: one thread owns it. The `writer.lock` file is a cross-process lock, not a cross-thread one |
| `co::Ledger`, `co::Snapshot`, `co::Topology`, `co::RackProfile`, `co::EvidenceRecord` | values; share them by copying or by holding a guard over the container that owns them |

`co::IngestPipeline` makes the window's access policy an explicit constructor
argument rather than an undocumented precondition, which is what keeps
`co::EvidenceWindow` thread-confined (`include/capacity_observatory/ingest.hpp`):

* `IngestPipeline(EvidenceWindow&, Store*)` uses `DirectWindowAccess`: the
  window is confined to the thread that drives ingest;
* `IngestPipeline(Synchronized<EvidenceWindow>&, Store*)` uses
  `LockedWindowAccess`: every `consider()` takes the write guard, and readers
  holding the read guard can never observe a half-applied record;
* `IngestPipeline(std::unique_ptr<WindowAccess>, Store*)` lets a deployment
  supply another policy.

## 2. Ownership rules

`co::Synchronized<T>` holds a mutable `std::shared_mutex` and the value.
Access is only possible through a guard:

* `read()` returns a `ReadGuard` holding a `std::shared_lock`; its
  `get()` and `operator->` are const;
* `write()` returns a `WriteGuard` holding a `std::unique_lock`; its
  `get()` and `operator->` are non-const.

The guards are RAII: the lock is released when the guard is destroyed. Both
guards delete their copy constructor and keep a move constructor, so a guard
cannot be duplicated into a second owner of the same lock. `Synchronized`
itself is neither copyable nor movable, so a guarded value cannot be copied out
from under its lock by accident. There is no public accessor that returns the
value without a guard.

Locking is not recursive. The standard library's `std::shared_mutex` is not a
recursive mutex, so a thread that takes two guards on the same object can
deadlock itself; section 9 records exactly how far the type system and the
auditor go towards preventing that, and where they stop.

## 3. There is no read-to-write upgrade API

`ReadGuard` and `WriteGuard` are unrelated types. There is no conversion, no
`upgrade()`, no `promote()`, and no way to obtain a write guard from a read
guard. A caller that has read state and now needs to write must release the read
guard and take the write guard, which makes the release point explicit in the
source. `ReasonCode::UpgradeRefused` exists in the vocabulary for callers that
model such a request themselves; the concurrency core never needs it because the
situation cannot be expressed.

The residual hazard is different and is *not* prevented by the type system: a
thread can hold a read guard and then call `write()` on the same
`Synchronized` object. Both calls announce `LockClass::State` to the auditor,
and equal-class nesting is not a violation (section 5), so nothing reports it.
The rule is therefore a calling rule, stated here and in the header:

> Never take a second guard on a `Synchronized` object that the current thread
> already guards. If you need to write, release first.

## 4. Callbacks are delivered outside every lock

`co::Publisher<Event>::publish` copies the handler list while holding the
mutex, releases the mutex, and only then invokes the handlers
(`include/capacity_observatory/concurrency.hpp`). Consequences:

* a handler may take any other lock, including a guard on an object the
  publisher's caller holds, without re-entering the publisher's mutex;
* a handler may itself subscribe, unsubscribe, or publish; the delivery in
  progress uses the copy, so the current delivery is unaffected while the next
  one sees the new set;
* `subscribe`, `unsubscribe`, and `subscriber_count` all take the same
  mutex, so concurrent churn cannot corrupt the handler list;
* delivery is synchronous on the publishing thread; concurrent publishes may
  interleave per handler, and no ordering between them is promised.

No other type in the library invokes user code while holding a lock: the
`Worker` body runs on its own thread, `BoundedQueue` holds its mutex only
around deque operations, and the guards touch only the guarded value.

Tests: `publisher_delivers_outside_every_lock` (the handler re-enters another
`Synchronized` through its write guard, which would self-deadlock if delivery
happened under the lock), `concurrent_publication_keeps_subscriber_accounting_consistent`
(deterministic delivery count with 20 subscribers and 3 publishers, then
subscriber churn under concurrent publishing).

## 5. Lock order and the auditor

The canonical order is fixed and named by `co::LockClass`:

```
control (0)  <  state (1)  <  store (2)  <  publication (3)
```

`Synchronized` guards are the `state` class. The other classes are available
to a deployment that owns locks of those kinds; the auditor is the only place
they are named.

`co::LockOrderAuditor` is a thread-local stack of held classes plus a
process-wide violation counter:

* `check_and_enter(lock_class)` returns false and increments the violation
  counter when the thread's current top of stack is *greater* than the requested
  class; otherwise it pushes the class and returns true. Equal classes are
  allowed, which is what lets a function take the read guard twice in sequence
  and what makes a nested acquisition on one object invisible to the audit.
* `leave(lock_class)` pops the top frame when it matches, otherwise erases the
  first matching frame, and does nothing when the class is not held. The audit
  can therefore never wedge a thread because a releaser got confused.
* `depth()` is the current thread's stack depth; `violation_count()` is the
  current thread's violation count; `total_violations()` is process-wide, for
  tests that assert a workload never inverted the order.
* `enabled()` is `constexpr` and reports whether the guards instantiated in
  *this translation unit* announce themselves. `Synchronized` is a header
  template, so this is a property of the translation unit: the CMake option
  `CO_ENABLE_LOCK_AUDIT` defines the macro on the library target (PUBLIC) and
  on `test_concurrency`, and a translation unit compiled without the macro gets
  guards that do not announce themselves, even though the auditor API is always
  callable from it.

When the announcement is compiled in, it happens *after* the underlying mutex is
acquired: a violation is recorded and reported, never turned into a failed
acquisition. That is deliberate - an ordering bug should be diagnosable, not a
hang. The ingest pipeline instantiates guards in the library, so the library must
itself be built with `CO_ENABLE_LOCK_AUDIT=ON` for library-internal locks to be
audited; the option does exactly that.

Tests: `lock_order_auditor_rejects_inversions` (an inversion returns false and
counts; equal classes are allowed; an out-of-order release removes exactly its
frame; releasing an unheld class is a no-op), and
`an_audited_workload_never_inverts_the_lock_order` (a real workload of guards
and callbacks, asserting `total_violations()` is unchanged and the stack is
empty at the end).

## 6. Worker lifecycle

`co::Worker` owns exactly one `std::thread` and a `co::Cancellation` token.
Its state is an atomic `co::WorkerState`: Created, Running, Stopping, Stopped.

`start(name, body)`:

* refuses with `AlreadyExists` when the worker is Running;
* refuses with `ShuttingDown` when it is Stopping;
* refuses with `InvalidArgument` when the body is empty;
* joins a previously finished thread outside any lock, so a second generation is
  possible after a clean stop;
* resets the cancellation token before the thread starts, so a new generation
  never observes the previous generation's cancellation;
* sets Running, starts the thread, and returns `WorkerFailed` if the thread
  could not be created (leaving the state Stopped).

The thread runs `body(cancellation)` and sets Stopped when the body returns.

`stop()`:

* returns immediately when the worker is Stopped and there is nothing to join, so
  it is idempotent;
* requests cancellation and sets Stopping;
* joins the thread if it is joinable, then sets Stopped.

The destructor calls `stop()`, so a worker cannot outlive its owner.

**stop() joins outside every lock**, and it does not wake anything by itself.
The documented contract is that the caller must have released any lock the worker
could need and must have woken whatever the worker is blocked on.
`co::IngestPipeline::stop_worker()` is the reference implementation of that
order:

```
queue_->close();   // a worker parked in pop() can now return false
worker_.stop();    // cancel, then join
```

The reason is concrete: cancellation is cooperative and
`BoundedQueue::pop` blocks on a condition variable. Cancelling alone cannot
interrupt a blocked pop, so a join without closing the queue would hang.
`Worker` has no queue to close and no wake hook, which is why the header states
the obligation on the caller.

Tests: `worker_stops_cleanly_and_restarts` (start, observe real progress, stop,
restart, stop twice) and `shutdown_drains_every_queued_item_and_never_drops_work`
(64 records queued, then an immediate stop, and all 64 are received).

## 7. The bounded queue refuses instead of blocking

`co::BoundedQueue<T>` is a `std::deque` behind a mutex and a condition
variable. Its capacity is clamped to at least one.

* `try_push` never blocks. It returns `QueueOutcome::Pushed`,
  `QueueOutcome::Full` when the queue is at capacity, or
  `QueueOutcome::Closed` after `close()`. The full check and the push happen
  under one lock, so no producer can exceed the capacity.
* `pop` blocks until an item is available or the queue is closed. It returns
  false only when the queue is closed *and* empty, so closing never discards
  items that were already pushed.
* `close()` sets the flag and wakes every waiter. It is idempotent.
* `size()`, `capacity()`, and `closed()` are lock-protected observations.

Back-pressure is therefore explicit: the producer is told that the queue is
full and must refuse the work. `co::IngestPipeline::try_submit` returns the
outcome unchanged and counts `enqueued` and `queue_full` in
`co::IngestStatistics`. Nothing in the library blocks a producer or grows a
buffer without a bound.

Test: `bounded_queue_refuses_instead_of_blocking` (capacity 2, third push
returns Full, a closed queue still drains its items and then reports exhaustion
without blocking).

## 8. Cancellation semantics

`co::Cancellation` is one `std::atomic<bool>` written with release ordering
and read with acquire ordering. `request()` is idempotent; `reset()` re-arms
the token for a subsequent worker generation, and `Worker::start` calls it, so
a restarted worker starts uncancelled.

Cancellation is cooperative and is observed only at points the worker body
chooses. The ingest worker observes it between records and then behaves as
follows (`src/ingest.cpp`):

* it drains everything already queued before returning, so queued work is never
  silently dropped;
* it returns from the body, which sets the state to Stopped;
* it relies on the queue being closed to leave a blocking pop, which is why
  `stop_worker()` closes first.

A long-running ingest is never interrupted mid-record: the durable commit and the
acceptance decision of the current record complete first. After the queue is
closed, `try_submit` returns `QueueOutcome::Closed` and
`IngestStatistics::enqueued` stops advancing.

Tests: `shutdown_drains_every_queued_item_and_never_drops_work`,
`concurrent_submit_and_snapshot_agree_on_the_final_state` (producers back off
and yield on a full queue, stop only on Closed, and the final state agrees with
the submitted count).

## 9. Hazard audit

Each row names a hazard, the structural reason it cannot occur, and the test
that covers it. Where a hazard is a calling rule rather than a structural
guarantee, the row says so.

| hazard | structural reason | covering test |
|---|---|---|
| self-deadlock (a thread taking a second guard on a `Synchronized` it already guards) | Not structural. Guards are move-only and a guard cannot be duplicated, and there is no accessor that returns the value without a guard, but a second explicit `read()`/`write()` call on the same object from the same thread still deadlocks, and the auditor allows equal-class nesting. This is a stated calling rule (section 3). | none: a test that does it would hang by design. `synchronized_readers_never_observe_a_torn_write` covers the guard discipline itself under 4 concurrent readers and 200 writes |
| read-to-write upgrade | Structural: there is no upgrade API and `ReadGuard`/`WriteGuard` are unrelated types with no conversion, so an upgrade is not expressible. | none needed: the absence is a compile-time property. Covered indirectly by every read-guard test |
| locks held across re-entry (a callback re-entering a lock) | Structural: `Publisher::publish` copies the handler list under the mutex, releases it, then delivers; no handler runs while a publisher lock is held. | `publisher_delivers_outside_every_lock` (the handler takes another object's write guard; a delivery under the lock would self-deadlock), `an_audited_workload_never_inverts_the_lock_order` (guards taken from inside a handler produce no violation) |
| callbacks under locks (general) | Structural: the only callback surface is `Publisher`; `Worker` bodies run on their own thread, and `BoundedQueue` and `Synchronized` invoke no user code under a lock. | `publisher_delivers_outside_every_lock` |
| inconsistent lock ordering | Structural for guarded state: the canonical order is fixed (control < state < store < publication), every `Synchronized` guard announces `State` when the audit is compiled in, and an inversion is counted and reported instead of deadlocking. | `lock_order_auditor_rejects_inversions`, `an_audited_workload_never_inverts_the_lock_order` |
| shutdown/join while holding worker-required state | Structural for the library's own path: `Worker::stop()` joins with no lock held, and `IngestPipeline::stop_worker()` closes the queue before cancelling and joining, so the worker is never waiting for a lock the joiner holds. The obligation on a caller that owns its own locks is documented in the header. | `worker_stops_cleanly_and_restarts`, `shutdown_drains_every_queued_item_and_never_drops_work` |
| cancellation races | Structural: one atomic flag with release/acquire ordering; `request()` and `reset()` are idempotent; `Worker::start` resets the flag before starting the thread, so a generation boundary cannot leak a stale cancellation; queued items are drained before the worker returns. | `worker_stops_cleanly_and_restarts`, `shutdown_drains_every_queued_item_and_never_drops_work` |
| stale-authority races (an older generation overwriting a newer one) | Structural: acceptance is a decision against the per-slot watermark, and `LockedWindowAccess` takes the write guard for every `consider()`, so two records for one slot cannot interleave; an older generation or epoch is refused with `StaleGeneration`/`StaleEpoch` and never reaches derived state. | `stale_authority_races_keep_the_newest_generation` (32 attempts, two threads racing generations 1-5 against 6-10, the newest generation must be current), `concurrent_submit_and_snapshot_agree_on_the_final_state` (4 producer threads, 200 records, one slot per zone, the newest generation survives) |
| concurrent publication | Structural: `publish` copies under the mutex and delivers outside it, and subscribe/unsubscribe/subscriber_count take the same mutex, so concurrent churn cannot corrupt the list or the accounting. | `concurrent_publication_keeps_subscriber_accounting_consistent` |

## 10. What the tests do not cover

For this document: the primitives, the lock-order auditor, the worker lifecycle,
and the queue semantics described above are **REAL** - they are implemented in
the cited files and the cited test cases in `tests/test_concurrency.cpp` run
them on this machine with real threads and real locks. Nothing in the concurrency
suite is simulated or modelled, so there is no **SYNTHETIC** claim to make. What
is **UNSUPPORTED** here is stated at the end of this section: nested
self-acquisition is not tested (it would hang), no thread-sanitizer build is
reported, and the auditor does not enforce, only report.

Stated so that a reader does not over-read the table above.

* No test performs a nested acquisition of the same `Synchronized` object, and
  none can: it would deadlock by design. The rule is documented, not enforced.
* The auditor reports; nothing in the library fails an operation because of a
  violation, and no production code path asserts that the process-wide violation
  count is zero. The tests assert it for the workloads they drive.
* Guard announcements are per translation unit. A build that defines
  `CO_ENABLE_LOCK_AUDIT` for the library but not for a consumer gets audited
  library locks and unaudited consumer locks.
* There is no test for a `Worker` body that blocks on something other than a
  `BoundedQueue`; the documented obligation is on the caller, and the library
  provides no generic wake hook.
* `Publisher` delivery order between concurrent publishes is unspecified and
  is not tested.
* The concurrency suite proves the properties above for the types it exercises.
  It is not a data-race detector: a race on a type that the table in section 1
  declares confined to one thread is a caller defect, and only AddressSanitizer
  or a thread sanitizer build would find it.
