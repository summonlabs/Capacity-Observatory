#pragma once
// Capacity Observatory - first party test harness.
//
// Deliberately dependency free. Failure unwinds the current case through a
// private exception so that a failing assertion reports once and the remaining
// cases still run. There are no timeouts anywhere: a hang is a defect.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include "capacity_observatory/platform.hpp"
#include "capacity_observatory/result.hpp"

namespace co::test {

struct TestFailure {
  std::string message;
};

// Declares a test case: a static function plus a registrar that runs it.
#define CO_TEST(test_name)                                                            \
  static void test_name();                                                            \
  static const ::co::test::Registrar co_registrar_##test_name(#test_name, &test_name); \
  static void test_name()

[[noreturn]] inline void fail(const char* file, int line, std::string message) {
  throw TestFailure{std::string(file) + ":" + std::to_string(line) + ": " + std::move(message)};
}

class Registrar {
 public:
  using Function = void (*)();
  Registrar(const char* name, Function function);
};

struct Statistics {
  std::size_t cases{0};
  std::size_t passed{0};
  std::size_t failed{0};
  std::size_t assertions{0};
};

// Runs every registered case; returns 0 when all pass. Supports --list and
// --filter <substring> for diagnosis.
int run_all(int argc, char** argv);

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------
// Detects whether a value can be streamed, so that a type without an
// operator<< still produces a diagnostic instead of failing to compile.
template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <class T>
std::string render(const T& value) {
  if constexpr (is_streamable<T>::value) {
    std::ostringstream out;
    if constexpr (std::is_enum_v<T>) {
      out << static_cast<long long>(value);
    } else {
      out << value;
    }
    return out.str();
  } else {
    // Types that are not streamable still need a stable rendering; add a
    // render() overload for the type to get a useful diagnostic instead.
    return std::string("<") + typeid(T).name() + ">";
  }
}

inline std::string render(bool value) { return value ? "true" : "false"; }
inline std::string render(std::string_view value) { return std::string(value); }
inline std::string render(ReasonCode code) { return std::string(reason_text(code)); }
inline std::string render(const Status& status) { return status.render(); }

template <class T>
std::string render(const Result<T>& result) {
  if (result.ok()) {
    return std::string("ok");
  }
  return std::string("error(") + result.status().render() + ")";
}

#define CO_FAIL(message) ::co::test::fail(__FILE__, __LINE__, (message))

#define CO_REQUIRE(condition)                                                       \
  do {                                                                              \
    ::co::test::note_assertion();                                                    \
    if (!::co::test::truthy(condition)) {                                            \
      CO_FAIL(std::string("requirement failed: ") + #condition);                     \
    }                                                                                \
  } while (false)

// The compared values are copied rather than bound by reference: binding a const
// reference to a value reached through a temporary (for example result.value())
// leaves the reference dangling as soon as that declaration statement ends. The
// AddressSanitizer run caught exactly that defect in this harness.
#define CO_REQUIRE_EQ(actual, expected)                                              \
  do {                                                                              \
    ::co::test::note_assertion();                                                    \
    const auto co_actual_value = (actual);                                          \
    const auto co_expected_value = (expected);                                      \
    if (!::co::test::equal_values(co_actual_value, co_expected_value)) {              \
      CO_FAIL(std::string("expected ") + #actual + " == " + #expected + "\n  actual:   " + \
              ::co::test::render(co_actual_value) + "\n  expected: " +                  \
              ::co::test::render(co_expected_value));                                \
    }                                                                                \
  } while (false)

#define CO_REQUIRE_NE(actual, other)                                                 \
  do {                                                                              \
    ::co::test::note_assertion();                                                    \
    const auto co_actual_value = (actual);                                          \
    const auto co_other_value = (other);                                            \
    if (::co::test::equal_values(co_actual_value, co_other_value)) {                  \
      CO_FAIL(std::string("expected ") + #actual + " != " + #other + ", both are " +  \
              ::co::test::render(co_actual_value));                                  \
    }                                                                                \
  } while (false)

// Requires a Result to carry a value and binds it to 'name'.
#define CO_REQUIRE_OK(result_expression, name)                                       \
  auto co_result_##name = (result_expression);                                       \
  ::co::test::note_assertion();                                                      \
  if (!co_result_##name.ok()) {                                                      \
    CO_FAIL(std::string("expected success from ") + #result_expression + ": " +       \
            co_result_##name.status().render());                                     \
  }                                                                                  \
  const auto& name = co_result_##name.value()

#define CO_REQUIRE_ERR(result_expression, expected_code)                             \
  do {                                                                              \
    ::co::test::note_assertion();                                                    \
    auto co_result_error = (result_expression);                                      \
    if (co_result_error.ok()) {                                                      \
      CO_FAIL(std::string("expected failure ") + #expected_code + " from " +          \
              #result_expression + " but the operation succeeded");                  \
    }                                                                                \
    if (co_result_error.status().code() != (expected_code)) {                        \
      CO_FAIL(std::string("expected ") + #expected_code + " from " + #result_expression + \
              " but observed " + ::co::test::render(co_result_error.status().code()) + \
              " (" + co_result_error.status().detail() + ")");                       \
    }                                                                                \
  } while (false)

// For Result<void> expressions.
#define CO_REQUIRE_OK_VOID(result_expression)                                        \
  do {                                                                              \
    ::co::test::note_assertion();                                                    \
    auto co_void_result = (result_expression);                                       \
    if (!co_void_result.ok()) {                                                      \
      CO_FAIL(std::string("expected success from ") + #result_expression + ": " +     \
              co_void_result.status().render());                                     \
    }                                                                                \
  } while (false)

// Comparisons are routed through a non-constexpr function so that a comparison of
// two constant expressions (a constexpr accessor against kDimensionCount, say)
// does not trip the "conditional expression is constant" warning under /W4 /WX.
template <class A, class B>
[[nodiscard]] inline bool equal_values(const A& left, const B& right) {
  return left == right;
}

[[nodiscard]] inline bool truthy(bool value) { return value; }

void note_assertion();

// ---------------------------------------------------------------------------
// Deterministic pseudo random numbers (splitmix64 + xoshiro256**).
// ---------------------------------------------------------------------------
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept;

  [[nodiscard]] std::uint64_t next_u64() noexcept;
  // Uniform in [0, bound) for bound > 0; unbiased for small bounds.
  [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept;
  [[nodiscard]] std::int64_t between(std::int64_t low, std::int64_t high) noexcept;
  [[nodiscard]] bool chance(unsigned int numerator, unsigned int denominator) noexcept;

 private:
  std::uint64_t state_[4]{};
};

// ---------------------------------------------------------------------------
// Scratch directories
// ---------------------------------------------------------------------------
class ScratchDirectory {
 public:
  explicit ScratchDirectory(std::string_view label);
  ~ScratchDirectory();
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;
  ScratchDirectory(ScratchDirectory&&) = delete;
  ScratchDirectory& operator=(ScratchDirectory&&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string child(std::string_view leaf) const;
  // Deletes and recreates the directory contents.
  void reset();
  void remove_all() noexcept;

 private:
  std::string path_;
};

// ---------------------------------------------------------------------------
// Child processes
// ---------------------------------------------------------------------------
struct ProcessResult {
  int exit_code{-1};
  std::string standard_output;
  std::string standard_error;
  bool started{false};
};

// Launches 'executable' with the given arguments, capturing both streams.
// 'executable' is normally the running test binary with a --child-role argument
// so that real multiprocess behavior is exercised rather than simulated.
[[nodiscard]] ProcessResult run_process(const std::string& executable,
                                        const std::vector<std::string>& arguments);

// Path of the currently running executable.
[[nodiscard]] std::string current_executable();

// Dispatches '--child-role <role>' to a handler registered by a test file.
using ChildHandler = std::function<int(const std::vector<std::string>& arguments)>;
void register_child_role(std::string role, ChildHandler handler);
[[nodiscard]] bool dispatch_child_role(int argc, char** argv);

}  // namespace co::test
