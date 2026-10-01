// Capacity Observatory - concurrency core implementation.
#include "capacity_observatory/concurrency.hpp"

#include <atomic>
#include <vector>

namespace co {

std::string_view lock_class_text(LockClass lock_class) noexcept {
  switch (lock_class) {
    case LockClass::Control: return "control";
    case LockClass::State: return "state";
    case LockClass::Store: return "store";
    case LockClass::Publication: return "publication";
  }
  return "unknown";
}

std::string_view worker_state_text(WorkerState state) noexcept {
  switch (state) {
    case WorkerState::Created: return "created";
    case WorkerState::Running: return "running";
    case WorkerState::Stopping: return "stopping";
    case WorkerState::Stopped: return "stopped";
  }
  return "unknown";
}

namespace {
// Always compiled: automatic auditing of the Synchronized locks is opt-in, but
// the auditor semantics themselves are exercised by the concurrency suite in
// every build configuration.
std::vector<LockClass>& held_stack() {
  thread_local std::vector<LockClass> stack;
  return stack;
}

std::size_t& violation_counter() {
  thread_local std::size_t count = 0;
  return count;
}

std::atomic<std::size_t>& total_violation_counter() {
  static std::atomic<std::size_t> count{0};
  return count;
}
}  // namespace

bool LockOrderAuditor::check_and_enter(LockClass lock_class) {
  std::vector<LockClass>& stack = held_stack();
  if (!stack.empty() && stack.back() > lock_class) {
    ++violation_counter();
    total_violation_counter().fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  stack.push_back(lock_class);
  return true;
}

void LockOrderAuditor::leave(LockClass lock_class) noexcept {
  std::vector<LockClass>& stack = held_stack();
  if (!stack.empty() && stack.back() == lock_class) {
    stack.pop_back();
    return;
  }
  // Out of order release means the audit itself observed an inconsistent
  // sequence; drop the frame anyway so the audit cannot wedge the thread.
  for (auto it = stack.begin(); it != stack.end(); ++it) {
    if (*it == lock_class) {
      stack.erase(it);
      return;
    }
  }
}

std::size_t LockOrderAuditor::depth() noexcept { return held_stack().size(); }

std::size_t LockOrderAuditor::violation_count() noexcept { return violation_counter(); }

std::size_t LockOrderAuditor::total_violations() noexcept {
  return total_violation_counter().load(std::memory_order_relaxed);
}

Worker::~Worker() { stop(); }

Result<void> Worker::start(std::string name, Body body) {
  const WorkerState current = state_.load(std::memory_order_acquire);
  if (current == WorkerState::Running) {
    return make_error(ReasonCode::AlreadyExists, "worker '" + name_ + "' is already running");
  }
  if (current == WorkerState::Stopping) {
    return make_error(ReasonCode::ShuttingDown, "worker '" + name_ + "' is still stopping");
  }
  if (!body) {
    return make_error(ReasonCode::InvalidArgument, "worker body is empty");
  }

  // A previous run may have left a joinable thread behind; join it outside any
  // lock before starting the next generation.
  if (thread_.joinable()) {
    thread_.join();
  }

  name_ = std::move(name);
  body_ = std::move(body);
  cancellation_.reset();
  state_.store(WorkerState::Running, std::memory_order_release);

  try {
    thread_ = std::thread([this]() {
      body_(cancellation_);
      state_.store(WorkerState::Stopped, std::memory_order_release);
    });
  } catch (...) {
    state_.store(WorkerState::Stopped, std::memory_order_release);
    return make_error(ReasonCode::WorkerFailed, "could not start worker '" + name_ + "'");
  }
  return Result<void>{};
}

void Worker::stop() noexcept {
  const WorkerState current = state_.load(std::memory_order_acquire);
  if (current == WorkerState::Stopped && !thread_.joinable()) {
    return;
  }
  cancellation_.request();
  state_.store(WorkerState::Stopping, std::memory_order_release);
  if (thread_.joinable()) {
    // Joined with no lock held: the worker only ever needs its own queue mutex,
    // and every caller of stop() must have released state locks already.
    thread_.join();
  }
  state_.store(WorkerState::Stopped, std::memory_order_release);
}

}  // namespace co
