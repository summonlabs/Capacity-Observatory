#pragma once
// Capacity Observatory - the concurrency core.
//
// Design rules enforced structurally rather than by convention:
//   * There is no read-to-write upgrade API. A caller that needs a write either
//     takes the write lock directly or releases the read guard first, so a
//     self-deadlock through upgrade is not expressible.
//   * Callbacks are never invoked while a lock is held: publish() copies the
//     subscriber list under the lock and delivers after releasing it.
//   * Lock acquisition order is audited in build configurations that enable
//     CO_ENABLE_LOCK_AUDIT; a violation is reported with a stable reason instead
//     of deadlocking.
//   * Background workers own their thread and are joined only from outside any
//     state lock; shutdown is a state machine, never a bare flag.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"

namespace co {

// ---------------------------------------------------------------------------
// Lock order audit
// ---------------------------------------------------------------------------
// Canonical order: control < state < store < publication. A thread that takes a
// lock out of order is reported; the audit is compiled in only when requested so
// that the release path pays nothing.
enum class LockClass : std::uint8_t {
  Control = 0,
  State = 1,
  Store = 2,
  Publication = 3,
};

[[nodiscard]] std::string_view lock_class_text(LockClass lock_class) noexcept;

class LockOrderAuditor {
 public:
  // Returns false when taking 'lock_class' would violate the canonical order.
  [[nodiscard]] static bool check_and_enter(LockClass lock_class);
  static void leave(LockClass lock_class) noexcept;
  // Depth of the current thread's held-lock stack; used by the audit tests.
  [[nodiscard]] static std::size_t depth() noexcept;
  // Violations observed on the current thread.
  [[nodiscard]] static std::size_t violation_count() noexcept;
  // Violations observed by the whole process, for tests that assert a workload
  // never inverted the order.
  [[nodiscard]] static std::size_t total_violations() noexcept;
  // True when the Synchronized guards instantiated in THIS translation unit call
  // the auditor themselves. Synchronized is a header template, so the answer is a
  // property of the translation unit: automatic auditing is compiled in only when
  // CO_ENABLE_LOCK_AUDIT is defined for it. The auditor API itself (check_and_enter,
  // leave, depth, violation counts) is always available from any translation unit.
  [[nodiscard]] static constexpr bool enabled() noexcept {
#if defined(CO_ENABLE_LOCK_AUDIT)
    return true;
#else
    return false;
#endif
  }
};

// ---------------------------------------------------------------------------
// Synchronized<T>: the only sanctioned way to share mutable state.
// ---------------------------------------------------------------------------
template <class T>
class Synchronized {
 public:
  // Both guards announce themselves to the lock order auditor when automatic
  // auditing is compiled in. The announcement happens after the lock is taken, so
  // a violation is recorded and reported rather than turning into a deadlock: the
  // auditor's purpose is diagnosis, not enforcement that could hang a process.
  class ReadGuard {
   public:
    ReadGuard(const Synchronized* owner, std::shared_lock<std::shared_mutex> guard)
        : owner_(owner), guard_(std::move(guard)) {
#if defined(CO_ENABLE_LOCK_AUDIT)
      const bool ordered = LockOrderAuditor::check_and_enter(LockClass::State);
      (void)ordered;
#endif
    }
    ~ReadGuard() {
#if defined(CO_ENABLE_LOCK_AUDIT)
      LockOrderAuditor::leave(LockClass::State);
#endif
    }
    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;
    ReadGuard(ReadGuard&& other) noexcept : owner_(other.owner_), guard_(std::move(other.guard_)) {}
    [[nodiscard]] const T& get() const noexcept { return owner_->value_; }
    [[nodiscard]] const T* operator->() const noexcept { return &owner_->value_; }

   private:
    const Synchronized* owner_;
    std::shared_lock<std::shared_mutex> guard_;
  };

  class WriteGuard {
   public:
    WriteGuard(Synchronized* owner, std::unique_lock<std::shared_mutex> guard)
        : owner_(owner), guard_(std::move(guard)) {
#if defined(CO_ENABLE_LOCK_AUDIT)
      const bool ordered = LockOrderAuditor::check_and_enter(LockClass::State);
      (void)ordered;
#endif
    }
    ~WriteGuard() {
#if defined(CO_ENABLE_LOCK_AUDIT)
      LockOrderAuditor::leave(LockClass::State);
#endif
    }
    WriteGuard(const WriteGuard&) = delete;
    WriteGuard& operator=(const WriteGuard&) = delete;
    WriteGuard(WriteGuard&& other) noexcept : owner_(other.owner_), guard_(std::move(other.guard_)) {}
    [[nodiscard]] T& get() noexcept { return owner_->value_; }
    [[nodiscard]] T* operator->() noexcept { return &owner_->value_; }

   private:
    Synchronized* owner_;
    std::unique_lock<std::shared_mutex> guard_;
  };

  Synchronized() = default;
  explicit Synchronized(T initial) : value_(std::move(initial)) {}
  Synchronized(const Synchronized&) = delete;
  Synchronized& operator=(const Synchronized&) = delete;

  [[nodiscard]] ReadGuard read() const {
    std::shared_lock<std::shared_mutex> guard(mutex_);
    return ReadGuard(this, std::move(guard));
  }

  [[nodiscard]] WriteGuard write() {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    return WriteGuard(this, std::move(guard));
  }

 private:
  mutable std::shared_mutex mutex_;
  T value_{};
};

// ---------------------------------------------------------------------------
// Bounded queue
// ---------------------------------------------------------------------------
enum class QueueOutcome : std::uint8_t { Pushed = 0, Full = 1, Closed = 2 };

template <class T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  // Never blocks: a full queue reports Full so the caller can refuse the work
  // instead of stalling a producer or growing without bound.
  [[nodiscard]] QueueOutcome try_push(T item) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (closed_) {
        return QueueOutcome::Closed;
      }
      if (queue_.size() >= capacity_) {
        return QueueOutcome::Full;
      }
      queue_.push_back(std::move(item));
    }
    not_empty_.notify_one();
    return QueueOutcome::Pushed;
  }

  // Blocks until an item is available or the queue closes.
  [[nodiscard]] bool pop(T& out) {
    std::unique_lock<std::mutex> guard(mutex_);
    not_empty_.wait(guard, [this]() { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void close() {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      closed_ = true;
    }
    not_empty_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return closed_;
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return queue_.size();
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::deque<T> queue_;
  std::size_t capacity_;
  bool closed_{false};
};

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------
class Cancellation {
 public:
  void request() noexcept { requested_.store(true, std::memory_order_release); }
  // Re-arms the token for a subsequent worker generation.
  void reset() noexcept { requested_.store(false, std::memory_order_release); }
  [[nodiscard]] bool requested() const noexcept { return requested_.load(std::memory_order_acquire); }

 private:
  std::atomic<bool> requested_{false};
};

// ---------------------------------------------------------------------------
// Worker lifecycle
// ---------------------------------------------------------------------------
enum class WorkerState : std::uint8_t { Created = 0, Running = 1, Stopping = 2, Stopped = 3 };

[[nodiscard]] std::string_view worker_state_text(WorkerState state) noexcept;

// Owns one thread. start()/stop() are idempotent; stop() joins and may be called
// from outside any lock the worker might need. Destroying a running worker stops
// it, so a worker can never outlive its owner.
class Worker {
 public:
  using Body = std::function<void(Cancellation&)>;

  Worker() = default;
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  [[nodiscard]] Result<void> start(std::string name, Body body);
  // Requests cancellation and joins. The owner must have released any state lock
  // the worker could need, and must have woken whatever the worker is blocked on
  // (IngestPipeline, for example, closes its queue before calling this).
  void stop() noexcept;
  [[nodiscard]] WorkerState state() const noexcept { return state_.load(std::memory_order_acquire); }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] Cancellation& cancellation() noexcept { return cancellation_; }

 private:
  std::string name_;
  Body body_;
  std::thread thread_;
  Cancellation cancellation_;
  std::atomic<WorkerState> state_{WorkerState::Created};
};

// ---------------------------------------------------------------------------
// Publication: subscribers are notified after the lock is released.
// ---------------------------------------------------------------------------
template <class Event>
class Publisher {
 public:
  using Handler = std::function<void(const Event&)>;
  using Token = std::uint64_t;

  [[nodiscard]] Token subscribe(Handler handler) {
    std::lock_guard<std::mutex> guard(mutex_);
    const Token token = next_token_++;
    handlers_.emplace_back(token, std::move(handler));
    return token;
  }

  void unsubscribe(Token token) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto it = handlers_.begin(); it != handlers_.end(); ++it) {
      if (it->first == token) {
        handlers_.erase(it);
        return;
      }
    }
  }

  // Copies the handler list under the lock, then delivers with no lock held.
  void publish(const Event& event) const {
    std::vector<Handler> copy;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      copy.reserve(handlers_.size());
      for (const auto& entry : handlers_) {
        copy.push_back(entry.second);
      }
    }
    for (const Handler& handler : copy) {
      handler(event);
    }
  }

  [[nodiscard]] std::size_t subscriber_count() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return handlers_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::pair<Token, Handler>> handlers_;
  Token next_token_{1};
};

}  // namespace co
