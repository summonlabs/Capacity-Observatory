#pragma once
// Capacity Observatory - platform services (error dialogs, OS error text, clock).

#include <cstdint>
#include <string>

namespace co::platform {

// Suppresses every interactive error surface the C runtime or the operating
// system could raise (CRT assert/abort dialogs, Windows Error Reporting).
// Called at process start by the CLI, the test harness, and the benchmarks so a
// defect is reported on stderr instead of blocking on a modal window.
void suppress_error_dialogs() noexcept;

// Text for an operating system error number (GetLastError() on Windows, errno
// elsewhere). Never throws and never allocates more than a bounded buffer.
[[nodiscard]] std::string os_error_text(unsigned long code);

// Text for the current thread's last operating system error.
[[nodiscard]] std::string last_os_error_text();

// Wall clock in nanoseconds since the Unix epoch.
[[nodiscard]] std::int64_t system_unix_nanos() noexcept;

// Number of usable hardware threads, at least one.
[[nodiscard]] unsigned int hardware_threads() noexcept;

// Process identifier, used by the real multiprocess proofs.
[[nodiscard]] std::uint64_t current_process_id() noexcept;

}  // namespace co::platform
