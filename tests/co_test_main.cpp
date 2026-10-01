#include "co_test.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace co::test {
namespace {

std::vector<std::pair<std::string, Registrar::Function>>& registry() {
  static std::vector<std::pair<std::string, Registrar::Function>> cases;
  return cases;
}

std::map<std::string, ChildHandler>& child_roles() {
  static std::map<std::string, ChildHandler> roles;
  return roles;
}

std::atomic<std::size_t> g_assertions{0};
std::atomic<unsigned long long> g_scratch_counter{0};

std::string scratch_root() {
  const std::filesystem::path base = std::filesystem::temp_directory_path() / "capacity-observatory-tests";
  return base.string();
}

}  // namespace

void note_assertion() { g_assertions.fetch_add(1, std::memory_order_relaxed); }

Registrar::Registrar(const char* name, Function function) { registry().emplace_back(name, function); }

int run_all(int argc, char** argv) {
  platform::suppress_error_dialogs();

  std::string filter;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    }
  }

  Statistics statistics;
  for (const auto& entry : registry()) {
    if (list_only) {
      std::cout << entry.first << "\n";
      continue;
    }
    if (!filter.empty() && entry.first.find(filter) == std::string::npos) {
      continue;
    }
    ++statistics.cases;
    const std::size_t before = g_assertions.load(std::memory_order_relaxed);
    std::cout << "[ RUN      ] " << entry.first << std::endl;
    try {
      entry.second();
      ++statistics.passed;
      std::cout << "[       OK ] " << entry.first << " (" << (g_assertions.load() - before) << " assertions)"
                << std::endl;
    } catch (const TestFailure& failure) {
      ++statistics.failed;
      std::cout << "[  FAILED  ] " << entry.first << "\n" << failure.message << std::endl;
    } catch (const std::exception& error) {
      ++statistics.failed;
      std::cout << "[  FAILED  ] " << entry.first << "\n  unexpected exception: " << error.what() << std::endl;
    } catch (...) {
      ++statistics.failed;
      std::cout << "[  FAILED  ] " << entry.first << "\n  unexpected non-standard exception" << std::endl;
    }
  }

  if (list_only) {
    return 0;
  }

  statistics.assertions = g_assertions.load(std::memory_order_relaxed);
  std::cout << "\n[ SUMMARY  ] cases=" << statistics.cases << " passed=" << statistics.passed
            << " failed=" << statistics.failed << " assertions=" << statistics.assertions << std::endl;
  return statistics.failed == 0 ? 0 : 1;
}

Rng::Rng(std::uint64_t seed) noexcept {
  // splitmix64 expansion of the seed.
  std::uint64_t state = seed;
  for (std::size_t i = 0; i < 4; ++i) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    state_[i] = z ^ (z >> 31U);
  }
}

std::uint64_t Rng::next_u64() noexcept {
  const auto rotate_left = [](std::uint64_t value, unsigned int amount) {
    return (value << amount) | (value >> (64U - amount));
  };
  // xoshiro256** with the standard scrambler.
  const std::uint64_t result = rotate_left(state_[1] * 5ULL, 7U) * 9ULL;
  const std::uint64_t temporary = state_[1] << 17U;

  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= temporary;
  state_[3] = rotate_left(state_[3], 45U);
  return result;
}

std::uint64_t Rng::below(std::uint64_t bound) noexcept {
  if (bound <= 1) {
    return 0;
  }
  // Rejection sampling keeps the distribution uniform.
  const std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)() - ((std::numeric_limits<std::uint64_t>::max)() % bound);
  for (;;) {
    const std::uint64_t candidate = next_u64();
    if (candidate < limit) {
      return candidate % bound;
    }
  }
}

std::int64_t Rng::between(std::int64_t low, std::int64_t high) noexcept {
  if (high <= low) {
    return low;
  }
  const std::uint64_t span = static_cast<std::uint64_t>(high - low);
  return low + static_cast<std::int64_t>(below(span));
}

bool Rng::chance(unsigned int numerator, unsigned int denominator) noexcept {
  return below(denominator) < numerator;
}

ScratchDirectory::ScratchDirectory(std::string_view label) {
  const unsigned long long counter = g_scratch_counter.fetch_add(1, std::memory_order_relaxed);
  std::string name(label);
  std::replace_if(name.begin(), name.end(), [](char ch) { return !std::isalnum(static_cast<unsigned char>(ch)) && ch != '-'; }, '-');
  const std::filesystem::path path = std::filesystem::path(scratch_root()) /
                                     (name + "-" + std::to_string(platform::current_process_id()) + "-" +
                                      std::to_string(counter));
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  if (error) {
    throw std::runtime_error("could not create scratch directory: " + path.string() + ": " + error.message());
  }
  path_ = path.string();
}

ScratchDirectory::~ScratchDirectory() { remove_all(); }

std::string ScratchDirectory::child(std::string_view leaf) const {
  return (std::filesystem::path(path_) / std::string(leaf)).string();
}

void ScratchDirectory::reset() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
  std::filesystem::create_directories(path_, error);
  if (error) {
    throw std::runtime_error("could not reset scratch directory: " + path_ + ": " + error.message());
  }
}

void ScratchDirectory::remove_all() noexcept {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::string current_executable() {
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(needed > 0 ? needed - 1 : 0), '\0');
  if (needed > 1) {
    ::WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), -1, out.data(), needed, nullptr, nullptr);
  }
  return out;
#else
  std::vector<char> buffer(32768, '\0');
  const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  return std::string(buffer.data(), static_cast<std::size_t>(length > 0 ? length : 0));
#endif
}

ProcessResult run_process(const std::string& executable, const std::vector<std::string>& arguments) {
  ProcessResult result;
#if defined(_WIN32)
  const std::string command_line = [&]() {
    std::string line = "\"" + executable + "\"";
    for (const std::string& argument : arguments) {
      line += " \"";
      for (const char ch : argument) {
        if (ch == '"') {
          line += "\\\"";
        } else {
          line += ch;
        }
      }
      line += "\"";
    }
    return line;
  }();

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(SECURITY_ATTRIBUTES);
  security.bInheritHandle = TRUE;
  security.lpSecurityDescriptor = nullptr;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (::CreatePipe(&read_end, &write_end, &security, 0) == 0) {
    result.standard_error = "CreatePipe failed";
    return result;
  }
  ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(STARTUPINFOW);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process{};
  std::wstring wide_command(command_line.begin(), command_line.end());
  // CreateProcessW may modify the command line buffer, which must be writable.
  std::vector<wchar_t> mutable_command(wide_command.begin(), wide_command.end());
  mutable_command.push_back(L'\0');
  std::wstring wide_executable(executable.begin(), executable.end());

  const BOOL created = ::CreateProcessW(wide_executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, 0,
                                        nullptr, nullptr, &startup, &process);
  ::CloseHandle(write_end);
  if (created == 0) {
    ::CloseHandle(read_end);
    result.standard_error = "CreateProcessW failed with error " + std::to_string(::GetLastError());
    return result;
  }
  result.started = true;

  std::string captured;
  char buffer[4096];
  for (;;) {
    DWORD read = 0;
    const BOOL ok = ::ReadFile(read_end, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr);
    if (ok == 0 || read == 0) {
      break;
    }
    captured.append(buffer, read);
  }
  ::CloseHandle(read_end);

  ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  ::GetExitCodeProcess(process.hProcess, &exit_code);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);

  result.exit_code = static_cast<int>(exit_code);
  result.standard_output = captured;
  return result;
#else
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    result.standard_error = "pipe failed";
    return result;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    result.standard_error = "fork failed";
    return result;
  }
  if (child == 0) {
    ::dup2(pipe_fds[1], STDOUT_FILENO);
    ::dup2(pipe_fds[1], STDERR_FILENO);
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(pipe_fds[1]);
  std::string captured;
  char buffer[4096];
  for (;;) {
    const ssize_t read = ::read(pipe_fds[0], buffer, sizeof(buffer));
    if (read <= 0) {
      break;
    }
    captured.append(buffer, static_cast<std::size_t>(read));
  }
  ::close(pipe_fds[0]);
  int status = 0;
  ::waitpid(child, &status, 0);
  result.started = true;
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  result.standard_output = captured;
  return result;
#endif
}

void register_child_role(std::string role, ChildHandler handler) {
  child_roles().emplace(std::move(role), std::move(handler));
}

bool dispatch_child_role(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--child-role") == 0) {
      const std::string role(argv[i + 1]);
      const auto found = child_roles().find(role);
      if (found == child_roles().end()) {
        std::fprintf(stderr, "unknown child role '%s'\n", role.c_str());
        return true;
      }
      std::vector<std::string> remaining;
      for (int j = 1; j < argc; ++j) {
        if (j == i || j == i + 1) {
          continue;
        }
        remaining.emplace_back(argv[j]);
      }
      const int code = found->second(remaining);
      std::exit(code);
    }
  }
  return false;
}

}  // namespace co::test

int main(int argc, char** argv) {
  co::platform::suppress_error_dialogs();
  if (co::test::dispatch_child_role(argc, argv)) {
    return 0;
  }
  return co::test::run_all(argc, argv);
}
