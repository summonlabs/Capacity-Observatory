#pragma once
// Capacity Observatory - explicit outcome types.
//
// The runtime never signals an expected domain outcome with an exception or a
// sentinel value. Every fallible operation returns either a value or a Status
// carrying a stable ReasonCode plus human/machine readable detail.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "capacity_observatory/reason.hpp"

namespace co {

// A non-throwing description of why an operation did not produce a value.
class Status {
 public:
  Status() noexcept = default;

  [[nodiscard]] static Status success() noexcept { return Status{}; }

  [[nodiscard]] static Status failure(ReasonCode code, std::string detail) {
    Status s;
    s.code_ = code;
    s.detail_ = std::move(detail);
    return s;
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == ReasonCode::Ok; }
  [[nodiscard]] bool failed() const noexcept { return !ok(); }
  [[nodiscard]] ReasonCode code() const noexcept { return code_; }
  [[nodiscard]] ReasonClass klass() const noexcept { return reason_class(code_); }

  // Free-form, deterministic explanation. Only meaningful when !ok().
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  // Machine readable "reason: detail" rendering, with the detail omitted when empty.
  [[nodiscard]] std::string render() const {
    std::string out(reason_text(code_));
    if (!detail_.empty()) {
      out.append(": ");
      out.append(detail_);
    }
    return out;
  }

  // Prefix the detail with a call-site breadcrumb and return the result by
  // value, so a breadcrumb can be added to a status obtained from a const
  // accessor without mutating the original.
  [[nodiscard]] Status with_context(std::string_view context) const {
    Status copy = *this;
    if (!copy.ok() && !context.empty()) {
      std::string merged(context);
      if (!copy.detail_.empty()) {
        merged.append(": ");
        merged.append(copy.detail_);
      }
      copy.detail_ = std::move(merged);
    }
    return copy;
  }

  friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_ && a.detail_ == b.detail_;
  }
  friend bool operator!=(const Status& a, const Status& b) noexcept { return !(a == b); }

 private:
  ReasonCode code_{ReasonCode::Ok};
  std::string detail_;
};

[[nodiscard]] inline Status make_error(ReasonCode code, std::string detail) {
  return Status::failure(code, std::move(detail));
}

// Convenience alias used throughout the code base for "expected value or status".
template <class T>
class Result {
 public:
  using value_type = T;
  using error_type = Status;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Status status) : storage_(std::in_place_index<1>, std::move(status)) {}  // NOLINT

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] bool ok() const noexcept { return has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const Status& status() const noexcept {
    static const Status kOk{};
    return has_value() ? kOk : std::get<1>(storage_);
  }
  [[nodiscard]] ReasonCode code() const noexcept { return status().code(); }

  // Unchecked accessors. Callers must have proven has_value(); the CLI and the
  // test harness use expect() below, which fails loudly instead of misbehaving.
  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::move(std::get<0>(storage_)); }
  [[nodiscard]] T* operator->() { return &std::get<0>(storage_); }
  [[nodiscard]] const T* operator->() const { return &std::get<0>(storage_); }
  [[nodiscard]] T& operator*() { return std::get<0>(storage_); }
  [[nodiscard]] const T& operator*() const { return std::get<0>(storage_); }

  template <class U>
  [[nodiscard]] T value_or(U&& fallback) const {
    return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

  [[nodiscard]] std::optional<T> to_optional() const {
    if (has_value()) {
      return std::get<0>(storage_);
    }
    return std::nullopt;
  }

 private:
  std::variant<T, Status> storage_;
};

// Result<void> shares the same vocabulary; the value arm stores nothing.
template <>
class Result<void> {
 public:
  using value_type = void;
  using error_type = Status;

  Result() noexcept = default;
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return status_.ok(); }
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] ReasonCode code() const noexcept { return status_.code(); }

 private:
  Status status_{};
};

[[nodiscard]] inline Result<void> ok_status() { return Result<void>{}; }

// Terminates the process after rendering the status, never through a GUI dialog.
// Reserved for invariant violations and for test/CLI code paths where a failure
// means the harness itself is broken.
[[noreturn]] void fatal_status(const char* what, const Status& status);

// Test/CLI convenience: abort unless the result carries a value.
template <class T>
[[nodiscard]] T&& expect(Result<T>&& result, const char* what = "expect(result)") {
  if (!result.has_value()) {
    fatal_status(what, result.status());
  }
  return std::move(result).value();
}

template <class T>
[[nodiscard]] const T& expect(const Result<T>& result, const char* what = "expect(result)") {
  if (!result.has_value()) {
    fatal_status(what, result.status());
  }
  return result.value();
}

inline void expect(const Result<void>& result, const char* what = "expect(result)") {
  if (!result.ok()) {
    fatal_status(what, result.status());
  }
}

}  // namespace co
