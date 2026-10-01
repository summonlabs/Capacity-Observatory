// Capacity Observatory - real multiprocess store suite.
//
// Every claim here is proven with real child processes of this same test
// binary, launched with --child-role and killed abruptly from the parent. The
// writer lock, the commit point, torn tails left by a killed writer, durable
// idempotency across a process restart, and atomic snapshot publication under a
// killed writer are all exercised against the kernel and the file system, not
// simulated in process.
#include "co_test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/hash.hpp"
#include "capacity_observatory/io.hpp"
#include "capacity_observatory/json.hpp"
#include "capacity_observatory/platform.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/store.hpp"
#include "capacity_observatory/strong.hpp"
#include "capacity_observatory/time.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace co;

namespace {

constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kFrameHeaderBytes = 8;
constexpr std::int64_t kObservedAt = 1700000000000000000LL;

// Binds the store held by a Result so that non-const operations are reachable.
#define CO_REQUIRE_STORE(result_expression, name)                                      \
  auto co_store_result_##name = (result_expression);                                   \
  ::co::test::note_assertion();                                                        \
  if (!co_store_result_##name.ok()) {                                                  \
    CO_FAIL(std::string("expected success from ") + #result_expression + ": " +        \
            co_store_result_##name.status().render());                                 \
  }                                                                                    \
  auto& name = co_store_result_##name.value()

// ---------------------------------------------------------------------------
// Filesystem helpers.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> read_bytes(const std::string& path) {
  Result<std::vector<std::uint8_t>> result = io::read_file(path, 1ULL << 28U);
  if (!result.ok()) {
    CO_FAIL("could not read " + path + ": " + result.status().render());
  }
  return std::move(result).value();
}

std::uint64_t size_of(const std::string& path) {
  Result<io::File> file = io::File::open_read_only(path);
  if (!file.ok()) {
    CO_FAIL("could not open " + path + ": " + file.status().render());
  }
  Result<std::uint64_t> size = file.value().size();
  if (!size.ok()) {
    CO_FAIL("could not size " + path + ": " + size.status().render());
  }
  return size.value();
}

std::uint32_t load_u32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool has_valid_header(const std::vector<std::uint8_t>& bytes) {
  return bytes.size() >= kHeaderBytes && std::memcmp(bytes.data(), "COBSTORE", 8U) == 0 &&
         crc32c_bytes(bytes.data(), 12U) == load_u32(bytes.data() + 12U);
}

// Frame count and the offset after the last complete frame.
struct LogSummary {
  std::size_t frames{0};
  std::size_t framed_end{0};
  bool header_ok{false};
  std::vector<std::string> payloads;
};

LogSummary summarize_log(const std::vector<std::uint8_t>& bytes) {
  LogSummary summary;
  if (!has_valid_header(bytes)) {
    return summary;
  }
  summary.header_ok = true;
  std::size_t offset = kHeaderBytes;
  while (offset + kFrameHeaderBytes <= bytes.size()) {
    const std::uint32_t size = load_u32(bytes.data() + offset);
    const std::uint32_t crc = load_u32(bytes.data() + offset + 4U);
    if (offset + kFrameHeaderBytes + size > bytes.size()) {
      break;
    }
    const std::string payload(reinterpret_cast<const char*>(bytes.data() + offset + kFrameHeaderBytes), size);
    if (crc32c(payload) != crc) {
      break;
    }
    summary.payloads.push_back(payload);
    offset += kFrameHeaderBytes + size;
    summary.framed_end = offset;
    ++summary.frames;
  }
  return summary;
}

std::size_t frame_count(const std::string& log_path) { return summarize_log(read_bytes(log_path)).frames; }

std::vector<std::uint8_t> bytes_of(std::string_view text) {
  std::vector<std::uint8_t> out;
  out.reserve(text.size());
  for (const char ch : text) {
    out.push_back(static_cast<std::uint8_t>(ch));
  }
  return out;
}

bool same_bytes(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool prefix_matches(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& prefix,
                    std::size_t count) {
  return bytes.size() >= count && prefix.size() >= count &&
         std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count), prefix.begin());
}

// ---------------------------------------------------------------------------
// Fixture records. Parent and children build byte-identical records so that a
// cross-process replay really is a replay of the same content.
// ---------------------------------------------------------------------------
ScopePath scope_of(const std::string& text) {
  Result<ScopePath> parsed = ScopePath::parse(text);
  if (!parsed.ok()) {
    CO_FAIL("could not parse scope '" + text + "': " + parsed.status().render());
  }
  return std::move(parsed).value();
}

EvidenceRecord fixture_record(std::uint64_t generation, std::int64_t declared, const std::string& source) {
  Result<EvidenceRecord> built =
      EvidenceRecord::make(AuthorityId("dfi"), AuthorityRole::InstalledInventory, scope_of("site=dc1/hall=h1"),
                           Dimension::Power, CapacityAssertion::Installed, Unit::canonical(Dimension::Power),
                           declared, Generation(generation), Epoch(1), Revision(1));
  if (!built.ok()) {
    CO_FAIL("could not build an evidence record: " + built.status().render());
  }
  EvidenceRecord record = std::move(built).value();
  record.provenance = Provenance::Observed;
  record.has_observed_at = true;
  record.observed_at = Timestamp(kObservedAt);
  record.source = source;
  return record;
}

MutationId mutation_of(const std::string& text) {
  Result<MutationId> parsed = MutationId::parse(text);
  if (!parsed.ok()) {
    CO_FAIL("could not parse mutation '" + text + "': " + parsed.status().render());
  }
  return std::move(parsed).value();
}

StoreOptions options_for(const std::string& directory) {
  StoreOptions options;
  options.directory = directory;
  return options;
}

std::vector<std::string> split_whitespace(const std::string& text) {
  std::vector<std::string> parts;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r')) {
      ++at;
    }
    const std::size_t begin = at;
    while (at < text.size() && text[at] != ' ' && text[at] != '\t' && text[at] != '\r') {
      ++at;
    }
    if (at > begin) {
      parts.push_back(text.substr(begin, at - begin));
    }
  }
  return parts;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Child process control. The parent reads the child's readiness line from a
// pipe (a blocking read, never a timed poll) and can terminate the child
// abruptly, exactly as a crash would.
// ---------------------------------------------------------------------------
bool parse_u64_arg(const std::string& text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    value = value * 10ULL + static_cast<std::uint64_t>(ch - '0');
  }
  out = value;
  return true;
}

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { kill_and_reap(); }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // Launches the executable with the given arguments and pipes on stdout and
  // stdin. The parent keeps the stdin write end, so a child that blocks on
  // stdin stays alive until it is killed or the parent closes the pipe.
  bool spawn(const std::string& executable, const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(SECURITY_ATTRIBUTES);
    security.bInheritHandle = TRUE;
    security.lpSecurityDescriptor = nullptr;

    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    if (::CreatePipe(&output_read, &output_write, &security, 0) == 0) {
      return false;
    }
    if (::CreatePipe(&input_read, &input_write, &security, 0) == 0) {
      ::CloseHandle(output_read);
      ::CloseHandle(output_write);
      return false;
    }
    ::SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);

    std::string command_line = "\"" + executable + "\"";
    for (const std::string& argument : arguments) {
      command_line += " \"";
      for (const char ch : argument) {
        if (ch == '"') {
          command_line += "\\\"";
        } else {
          command_line += ch;
        }
      }
      command_line += "\"";
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(STARTUPINFOW);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output_write;
    startup.hStdError = output_write;
    startup.hStdInput = input_read;

    PROCESS_INFORMATION information{};
    std::wstring wide_command(command_line.begin(), command_line.end());
    std::vector<wchar_t> mutable_command(wide_command.begin(), wide_command.end());
    mutable_command.push_back(L'\0');
    std::wstring wide_executable(executable.begin(), executable.end());
    const BOOL created = ::CreateProcessW(wide_executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, 0,
                                          nullptr, nullptr, &startup, &information);
    ::CloseHandle(output_write);
    ::CloseHandle(input_read);
    if (created == 0) {
      ::CloseHandle(output_read);
      ::CloseHandle(input_write);
      return false;
    }
    ::CloseHandle(information.hThread);
    process_ = information.hProcess;
    output_ = output_read;
    input_ = input_write;
    valid_ = true;
    return true;
#else
    int output_pipe[2];
    int input_pipe[2];
    if (::pipe(output_pipe) != 0) {
      return false;
    }
    if (::pipe(input_pipe) != 0) {
      ::close(output_pipe[0]);
      ::close(output_pipe[1]);
      return false;
    }
    const pid_t child = ::fork();
    if (child < 0) {
      ::close(output_pipe[0]);
      ::close(output_pipe[1]);
      ::close(input_pipe[0]);
      ::close(input_pipe[1]);
      return false;
    }
    if (child == 0) {
      ::dup2(output_pipe[1], STDOUT_FILENO);
      ::dup2(output_pipe[1], STDERR_FILENO);
      ::dup2(input_pipe[0], STDIN_FILENO);
      ::close(output_pipe[0]);
      ::close(output_pipe[1]);
      ::close(input_pipe[0]);
      ::close(input_pipe[1]);
      std::vector<char*> argv;
      argv.push_back(const_cast<char*>(executable.c_str()));
      for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
      }
      argv.push_back(nullptr);
      ::execv(executable.c_str(), argv.data());
      ::_exit(127);
    }
    ::close(output_pipe[1]);
    ::close(input_pipe[0]);
    pid_ = static_cast<int>(child);
    output_ = output_pipe[0];
    input_ = input_pipe[1];
    valid_ = true;
    return true;
#endif
  }

  [[nodiscard]] bool valid() const { return valid_; }

  // Blocking read of one line: it returns as soon as the child writes a
  // newline, and returns an empty string when the child closes the pipe.
  std::string read_line() {
    std::string line;
    char ch = 0;
    for (;;) {
      const int read = read_byte(&ch);
      if (read <= 0) {
        break;
      }
      if (ch == '\n') {
        break;
      }
      line.push_back(ch);
    }
    // The child's stdout is a text stream on Windows, so a line arrives as
    // "text\r\n"; the carriage return is transport, not content.
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    return line;
  }

  [[nodiscard]] bool running() const {
#if defined(_WIN32)
    return valid_ && ::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
#else
    if (!valid_) {
      return false;
    }
    int status = 0;
    return ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG) == 0;
#endif
  }

  // Abrupt termination, as a crash or a machine failure would be: the child
  // runs no destructors, flushes nothing, and closes no handles itself.
  void terminate_now() {
    if (!valid_ || reaped_) {
      return;
    }
#if defined(_WIN32)
    ::TerminateProcess(static_cast<HANDLE>(process_), 1);
#else
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
#endif
  }

  // Waits for the child and returns its exit code (-1 when it was killed).
  int wait_for_exit() {
    if (!valid_ || reaped_) {
      return reaped_code_;
    }
#if defined(_WIN32)
    ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 0;
    if (::GetExitCodeProcess(static_cast<HANDLE>(process_), &code) == 0) {
      code = static_cast<DWORD>(-1);
    }
    reaped_code_ = static_cast<int>(code);
#else
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    reaped_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    reaped_ = true;
    close_handles();
    return reaped_code_;
  }

  int kill_and_reap() {
    if (!valid_) {
      return reaped_code_;
    }
    if (!reaped_) {
      terminate_now();
    }
    return wait_for_exit();
  }

 private:
  int read_byte(char* out) {
    if (!valid_) {
      return 0;
    }
#if defined(_WIN32)
    DWORD read = 0;
    if (::ReadFile(static_cast<HANDLE>(output_), out, 1, &read, nullptr) == 0 || read == 0) {
      return 0;
    }
    return 1;
#else
    const ssize_t read = ::read(static_cast<int>(output_), out, 1);
    return read == 1 ? 1 : 0;
#endif
  }

  void close_handles() {
    if (!valid_) {
      return;
    }
#if defined(_WIN32)
    if (output_ != nullptr) {
      ::CloseHandle(static_cast<HANDLE>(output_));
      output_ = nullptr;
    }
    if (input_ != nullptr) {
      ::CloseHandle(static_cast<HANDLE>(input_));
      input_ = nullptr;
    }
    if (process_ != nullptr) {
      ::CloseHandle(static_cast<HANDLE>(process_));
      process_ = nullptr;
    }
#else
    if (output_ >= 0) {
      ::close(output_);
      output_ = -1;
    }
    if (input_ >= 0) {
      ::close(input_);
      input_ = -1;
    }
    pid_ = -1;
#endif
    valid_ = false;
  }

  void* process_{nullptr};
  void* output_{nullptr};
  void* input_{nullptr};
  int pid_{-1};
  int reaped_code_{-1};
  bool reaped_{false};
  bool valid_{false};
};

// ---------------------------------------------------------------------------
// Child roles. Registered once for this translation unit; the harness
// dispatches --child-role <role> in main before any case runs.
// ---------------------------------------------------------------------------
void emit(const std::string& line) {
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

void emit_failure(const std::string& detail) {
  std::fprintf(stderr, "child failure: %s\n", detail.c_str());
  std::fflush(stderr);
}

EvidenceRecord fixture_record_or_default(std::uint64_t generation, std::int64_t declared, const std::string& source) {
  Result<EvidenceRecord> built =
      EvidenceRecord::make(AuthorityId("dfi"), AuthorityRole::InstalledInventory, scope_of("site=dc1/hall=h1"),
                           Dimension::Power, CapacityAssertion::Installed, Unit::canonical(Dimension::Power),
                           declared, Generation(generation), Epoch(1), Revision(1));
  EvidenceRecord record;
  if (!built.ok()) {
    return record;
  }
  record = std::move(built).value();
  record.provenance = Provenance::Observed;
  record.has_observed_at = true;
  record.observed_at = Timestamp(kObservedAt);
  record.source = source;
  return record;
}

int role_writer_commit_then_block(const std::vector<std::string>& arguments) {
  if (arguments.size() < 2U) {
    return 2;
  }
  std::uint64_t count = 0;
  if (!parse_u64_arg(arguments[1], count)) {
    return 2;
  }
  StoreOptions options;
  options.directory = arguments[0];
  Result<Store> opened = Store::open(options);
  if (!opened.ok()) {
    emit_failure("open: " + opened.status().render());
    return 3;
  }
  Store store = std::move(opened).value();
  for (std::uint64_t i = 1; i <= count; ++i) {
    const EvidenceRecord record =
        fixture_record_or_default(i, static_cast<std::int64_t>(i) * 1000, "child:record" + std::to_string(i));
    Result<AppendOutcome> appended = store.append_evidence(record, mutation_of("child-m-" + std::to_string(i)));
    if (!appended.ok() || !appended.value().applied) {
      emit_failure("append: " + appended.status().render());
      return 4;
    }
    emit("rec " + std::to_string(i) + " " + record.content_digest().hex());
  }
  emit("session-epoch " + std::to_string(store.session_epoch().value()));
  emit("ready");

  // Block until the parent closes the pipe. No polling, no timeout: the parent
  // either kills this process or releases it deliberately.
  char buffer[1];
  while (std::fread(buffer, 1, 1, stdin) == 1) {
  }
  store.close();
  return 0;
}

int role_writer_partial_then_block(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return 2;
  }
  StoreOptions options;
  options.directory = arguments[0];
  Result<Store> opened = Store::open(options);
  if (!opened.ok()) {
    emit_failure("open: " + opened.status().render());
    return 3;
  }
  Store store = std::move(opened).value();
  const EvidenceRecord record = fixture_record_or_default(1, 1000, "child:durable");
  Result<AppendOutcome> appended = store.append_evidence(record, mutation_of("child-m-durable"));
  if (!appended.ok() || !appended.value().applied) {
    emit_failure("append: " + appended.status().render());
    return 4;
  }
  emit("rec 1 " + record.content_digest().hex());
  emit("mutation child-m-durable");

  // The store holds its log with FILE_SHARE_READ only, so a second handle that
  // requests write access cannot be opened while it is live. The writer session
  // is therefore closed here before the interrupted write; the artifact left on
  // disk is byte for byte what a writer killed between the frame header write
  // and the payload flush leaves behind.
  store.close();

  // Hand-write the first half of a frame: the commit point is the flush of a
  // complete frame, so this is exactly what a writer killed mid-append leaves.
  const std::string claimed(600, 'p');
  std::vector<std::uint8_t> partial;
  const std::uint32_t size = static_cast<std::uint32_t>(claimed.size());
  partial.push_back(static_cast<std::uint8_t>(size & 0xFFU));
  partial.push_back(static_cast<std::uint8_t>((size >> 8U) & 0xFFU));
  partial.push_back(static_cast<std::uint8_t>((size >> 16U) & 0xFFU));
  partial.push_back(static_cast<std::uint8_t>((size >> 24U) & 0xFFU));
  const std::uint32_t crc = crc32c(claimed);
  partial.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
  partial.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
  partial.push_back(static_cast<std::uint8_t>((crc >> 16U) & 0xFFU));
  partial.push_back(static_cast<std::uint8_t>((crc >> 24U) & 0xFFU));
  const std::uint8_t* claimed_bytes = reinterpret_cast<const std::uint8_t*>(claimed.data());
  partial.insert(partial.end(), claimed_bytes, claimed_bytes + 300);

  Result<io::File> log = io::File::open(io::join(arguments[0], "evidence.log"), false);
  if (!log.ok()) {
    emit_failure("log: " + log.status().render());
    return 5;
  }
  if (!log.value().seek_end().ok() || !log.value().write_all(partial.data(), partial.size()).ok() ||
      !log.value().sync().ok()) {
    emit_failure("partial frame write failed");
    return 5;
  }
  emit("partial " + std::to_string(partial.size()));
  emit("ready");

  char buffer[1];
  while (std::fread(buffer, 1, 1, stdin) == 1) {
  }
  store.close();
  return 0;
}

int role_lock_probe(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return 2;
  }
  StoreOptions options;
  options.directory = arguments[0];
  Result<Store> opened = Store::open(options);
  if (opened.ok()) {
    emit("OPENED");
    opened.value().close();
    return 0;
  }
  emit(std::string("REFUSED ") + std::string(reason_text(opened.status().code())));
  return 0;
}

int role_inspect_probe(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return 2;
  }
  StoreOptions options;
  options.directory = arguments[0];
  Result<RecoveryReport> inspected = inspect_store(options);
  if (!inspected.ok()) {
    emit(std::string("INSPECT-REFUSED ") + std::string(reason_text(inspected.status().code())));
    return 0;
  }
  emit("INSPECTED frames " + std::to_string(inspected.value().frames_read) + " evidence " +
       std::to_string(inspected.value().evidence_frames));
  return 0;
}

int role_reader_dump(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return 2;
  }
  StoreOptions options;
  options.directory = arguments[0];
  options.read_only = true;
  Result<Store> opened = Store::open(options);
  if (!opened.ok()) {
    emit(std::string("open-refused ") + std::string(reason_text(opened.status().code())));
    return 3;
  }
  Result<std::vector<StoredEvidence>> loaded = opened.value().load_evidence();
  if (!loaded.ok()) {
    emit(std::string("load-refused ") + std::string(reason_text(loaded.status().code())));
    return 4;
  }
  emit("torn-tail " + std::to_string(opened.value().recovery().torn_tail_recovered ? 1 : 0));
  emit("discarded " + std::to_string(opened.value().recovery().bytes_discarded));
  emit("count " + std::to_string(loaded.value().size()));
  for (const StoredEvidence& stored : loaded.value()) {
    emit("rec " + stored.record.content_digest().hex());
  }
  return 0;
}

int role_idempotent_replay(const std::vector<std::string>& arguments) {
  if (arguments.size() < 5U) {
    return 2;
  }
  std::uint64_t generation = 0;
  std::uint64_t declared = 0;
  if (!parse_u64_arg(arguments[2], generation) || !parse_u64_arg(arguments[3], declared)) {
    return 2;
  }
  StoreOptions options;
  options.directory = arguments[0];
  Result<Store> opened = Store::open(options);
  if (!opened.ok()) {
    emit_failure("open: " + opened.status().render());
    return 3;
  }
  Store store = std::move(opened).value();
  const std::string log_path = io::join(arguments[0], "evidence.log");
  const std::size_t before = frame_count(log_path);
  const EvidenceRecord record =
      fixture_record_or_default(generation, static_cast<std::int64_t>(declared), arguments[4]);
  Result<AppendOutcome> outcome = store.append_evidence(record, mutation_of(arguments[1]));
  const std::size_t after = frame_count(log_path);
  if (outcome.ok()) {
    emit(std::string("outcome ") + std::string(reason_text(outcome.value().reason)));
    emit(std::string("applied ") + (outcome.value().applied ? "1" : "0"));
  } else {
    emit(std::string("outcome ") + std::string(reason_text(outcome.status().code())));
    emit("applied 0");
  }
  emit("frames-before " + std::to_string(before));
  emit("frames-after " + std::to_string(after));
  emit("mutation-digest " + record.content_digest().hex());
  emit("ready");
  return 0;
}

int role_snapshot_partial_then_block(const std::vector<std::string>& arguments) {
  if (arguments.size() < 3U) {
    return 2;
  }
  StoreOptions options;
  options.directory = arguments[0];
  Result<Store> opened = Store::open(options);
  if (!opened.ok()) {
    emit_failure("open: " + opened.status().render());
    return 3;
  }
  Store store = std::move(opened).value();

  const std::string first = arguments[1];
  Result<Digest> published = store.publish_snapshot(first);
  if (!published.ok()) {
    emit_failure("publish: " + published.status().render());
    return 4;
  }
  emit("published " + published.value().hex());

  // The next publication is interrupted after half of the temporary file has
  // been written: the same artifact a killed writer leaves behind, at the same
  // path, produced through the same file API.
  const std::string second = arguments[2];
  const std::string temp = io::temp_sibling(io::join(arguments[0], "snapshot.json"), "publish");
  Result<io::File> file = io::File::open(temp, true);
  if (!file.ok()) {
    emit_failure("temp: " + file.status().render());
    return 5;
  }
  const std::size_t half = second.size() / 2U;
  if (!file.value().truncate(0).ok() || !file.value().write_all(second.data(), half).ok() ||
      !file.value().sync().ok()) {
    emit_failure("partial publication write failed");
    return 5;
  }
  file.value().close();
  emit("partial-temp " + std::to_string(half));
  emit("ready");

  char buffer[1];
  while (std::fread(buffer, 1, 1, stdin) == 1) {
  }
  store.close();
  return 0;
}

struct ChildRoles {
  ChildRoles() {
    test::register_child_role("writer-commit-then-block", role_writer_commit_then_block);
    test::register_child_role("writer-partial-then-block", role_writer_partial_then_block);
    test::register_child_role("lock-probe", role_lock_probe);
    test::register_child_role("inspect-probe", role_inspect_probe);
    test::register_child_role("reader-dump", role_reader_dump);
    test::register_child_role("idempotent-replay", role_idempotent_replay);
    test::register_child_role("snapshot-partial-then-block", role_snapshot_partial_then_block);
  }
};

const ChildRoles kChildRoles;

// Runs a short lived child to completion and returns its captured output.
test::ProcessResult run_role(const std::vector<std::string>& arguments) {
  std::vector<std::string> full{"--child-role"};
  full.insert(full.end(), arguments.begin(), arguments.end());
  return test::run_process(test::current_executable(), full);
}

}  // namespace

// ---------------------------------------------------------------------------
// The writer lock is kernel enforced and cross process
// ---------------------------------------------------------------------------
CO_TEST(a_second_process_cannot_hold_the_writer_lock) {
  test::ScratchDirectory scratch("mp-lock");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
  const StoreOptions options = options_for(directory);

  CO_REQUIRE_STORE(Store::open(options), store);
  CO_REQUIRE_OK(store.append_evidence(fixture_record(1, 1000, "parent:lock"), mutation_of("parent-m-1")), first);
  CO_REQUIRE(first.applied);
  CO_REQUIRE(io::exists(io::join(directory, "writer.lock")));

  // A second open in this process is refused too: the lock is a property of the
  // file, not of the process that happens to hold it.
  CO_REQUIRE_ERR(Store::open(options), ReasonCode::LockHeld);

  // A second process is refused with the same reason.
  const test::ProcessResult blocked = run_role({"lock-probe", directory});
  CO_REQUIRE(blocked.started);
  CO_REQUIRE_EQ(blocked.exit_code, 0);
  CO_REQUIRE(contains(blocked.standard_output, "REFUSED lock-held"));

  // Inspection takes no lock at all and works while the writer is live, in this
  // process and in another one.
  CO_REQUIRE_OK(inspect_store(options), inspected);
  CO_REQUIRE_EQ(inspected.frames_read, 2U);
  CO_REQUIRE_EQ(inspected.evidence_frames, 1U);
  CO_REQUIRE(inspected.session_epoch == Epoch(1));
  const test::ProcessResult inspector = run_role({"inspect-probe", directory});
  CO_REQUIRE_EQ(inspector.exit_code, 0);
  CO_REQUIRE(contains(inspector.standard_output, "INSPECTED frames 2"));

  store.close();

  // With the writer gone, a read-only open reads the committed records.
  {
    StoreOptions read_only = options;
    read_only.read_only = true;
    CO_REQUIRE_STORE(Store::open(read_only), reader);
    CO_REQUIRE_OK(reader.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 1U);
  }
  const test::ProcessResult reader = run_role({"reader-dump", directory});
  CO_REQUIRE_EQ(reader.exit_code, 0);
  CO_REQUIRE(contains(reader.standard_output, "count 1"));

  // Once released, another process can take the lock.
  const test::ProcessResult free_probe = run_role({"lock-probe", directory});
  CO_REQUIRE_EQ(free_probe.exit_code, 0);
  CO_REQUIRE(contains(free_probe.standard_output, "OPENED"));
}

CO_TEST(a_killed_writer_releases_the_lock_and_keeps_committed_records) {
  test::ScratchDirectory scratch("mp-kill");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
  const StoreOptions options = options_for(directory);
  const std::string log_path = io::join(directory, "evidence.log");

  ChildProcess child;
  CO_REQUIRE(child.spawn(test::current_executable(),
                         {"--child-role", "writer-commit-then-block", directory, "3"}));
  CO_REQUIRE(child.valid());

  std::vector<std::string> committed;
  std::uint64_t child_epoch = 0;
  for (;;) {
    const std::string line = child.read_line();
    if (line == "ready") {
      break;
    }
    CO_REQUIRE(!line.empty());
    const std::vector<std::string> parts = split_whitespace(line);
    if (parts.size() == 3U && parts[0] == "rec") {
      committed.push_back(parts[2]);
    } else if (parts.size() == 2U && parts[0] == "session-epoch") {
      CO_REQUIRE(parse_u64_arg(parts[1], child_epoch));
    } else {
      CO_FAIL("unexpected child output: " + line);
    }
  }
  CO_REQUIRE_EQ(committed.size(), 3U);
  CO_REQUIRE(child_epoch >= 1U);
  CO_REQUIRE(child.running());

  // The child really holds the lock while it is alive.
  const test::ProcessResult blocked = run_role({"lock-probe", directory});
  CO_REQUIRE(contains(blocked.standard_output, "REFUSED lock-held"));

  // Abrupt termination: no destructors, no polite close.
  child.terminate_now();
  const int exit_code = child.wait_for_exit();
  CO_REQUIRE_EQ(exit_code, 1);
  CO_REQUIRE(!child.running());

  // The kernel released the lock with the process, and every record the child
  // had committed before dying is still there, byte identical.
  std::size_t frames_after_kill = 0;
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(!store.recovery().torn_tail_recovered);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 3U);
    for (std::size_t i = 0; i < loaded.size(); ++i) {
      CO_REQUIRE(loaded[i].record.content_digest().hex() == committed[i]);
      CO_REQUIRE(loaded[i].mutation == mutation_of("child-m-" + std::to_string(i + 1U)));
      CO_REQUIRE(loaded[i].written_epoch == Epoch(child_epoch));
    }
    CO_REQUIRE_EQ(store.mutations().size(), 3U);
  }
  frames_after_kill = frame_count(log_path);
  CO_REQUIRE_EQ(frames_after_kill, 5U);

  // A completely fresh process reads exactly the same records.
  const test::ProcessResult reader = run_role({"reader-dump", directory});
  CO_REQUIRE_EQ(reader.exit_code, 0);
  CO_REQUIRE(contains(reader.standard_output, "count 3"));
  CO_REQUIRE(contains(reader.standard_output, "torn-tail 0"));
  for (const std::string& digest : committed) {
    CO_REQUIRE(contains(reader.standard_output, digest));
  }
}

CO_TEST(a_killed_writer_with_a_partial_frame_leaves_a_recoverable_torn_tail) {
  test::ScratchDirectory scratch("mp-partial");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
  const StoreOptions options = options_for(directory);
  const std::string log_path = io::join(directory, "evidence.log");

  ChildProcess child;
  CO_REQUIRE(child.spawn(test::current_executable(), {"--child-role", "writer-partial-then-block", directory}));
  CO_REQUIRE(child.valid());

  std::string committed_digest;
  std::uint64_t partial_bytes = 0;
  for (;;) {
    const std::string line = child.read_line();
    if (line == "ready") {
      break;
    }
    CO_REQUIRE(!line.empty());
    const std::vector<std::string> parts = split_whitespace(line);
    if (parts.size() == 3U && parts[0] == "rec") {
      committed_digest = parts[2];
    } else if (parts.size() == 2U && parts[0] == "partial") {
      CO_REQUIRE(parse_u64_arg(parts[1], partial_bytes));
    }
  }
  CO_REQUIRE(!committed_digest.empty());
  CO_REQUIRE(partial_bytes > 0U);
  CO_REQUIRE(child.running());

  // The torn bytes really are on disk while the writer is still alive.
  const std::uint64_t size_before_kill = size_of(log_path);
  const std::vector<std::uint8_t> killed_bytes = read_bytes(log_path);
  const LogSummary before = summarize_log(killed_bytes);
  CO_REQUIRE_EQ(before.frames, 2U);
  CO_REQUIRE(before.framed_end < size_before_kill);

  child.terminate_now();
  CO_REQUIRE_EQ(child.wait_for_exit(), 1);
  CO_REQUIRE_EQ(size_of(log_path), size_before_kill);

  // The next process recovers conservatively: the torn tail is discarded and
  // the committed record is untouched.
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE(store.recovery().torn_tail_recovered);
    CO_REQUIRE_EQ(store.recovery().bytes_discarded, partial_bytes);
    CO_REQUIRE_EQ(store.recovery().frames_read, 2U);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 1U);
    CO_REQUIRE(loaded[0].record.content_digest().hex() == committed_digest);
    // The interrupted append is not durable and is not remembered, so redoing
    // the mutation is a first append rather than a replay or a conflict.
    CO_REQUIRE(store.mutations().find(mutation_of("child-m-torn")) == store.mutations().end());
    CO_REQUIRE_EQ(store.mutations().size(), 1U);
  }

  const std::vector<std::uint8_t> after = read_bytes(log_path);
  // The recovered file keeps every complete frame byte for byte, drops the torn
  // bytes, and gains exactly this session's epoch frame: it is neither a prefix
  // of the killed file nor an extension of it, and its size says nothing on its
  // own because the discarded tail and the appended epoch frame are unrelated.
  CO_REQUIRE(prefix_matches(after, killed_bytes, before.framed_end));
  CO_REQUIRE(after.size() > before.framed_end);
  CO_REQUIRE(after.size() < size_before_kill + 1024U);
  const LogSummary recovered = summarize_log(after);
  CO_REQUIRE_EQ(recovered.frames, 3U);
  CO_REQUIRE_EQ(recovered.framed_end, after.size());

  // Another process sees a clean store with the recovered record.
  const test::ProcessResult reader = run_role({"reader-dump", directory});
  CO_REQUIRE_EQ(reader.exit_code, 0);
  CO_REQUIRE(contains(reader.standard_output, "count 1"));
  CO_REQUIRE(contains(reader.standard_output, committed_digest));
}

// ---------------------------------------------------------------------------
// Durable idempotency across a real process restart
// ---------------------------------------------------------------------------
CO_TEST(a_replayed_mutation_is_recognised_after_a_process_restart) {
  test::ScratchDirectory scratch("mp-idempotent");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
  const StoreOptions options = options_for(directory);
  const std::string log_path = io::join(directory, "evidence.log");

  const EvidenceRecord record = fixture_record(7, 250000, "parent:durable-mutation");
  const std::string digest = record.content_digest().hex();
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_OK(store.append_evidence(record, mutation_of("parent-m-replay")), outcome);
    CO_REQUIRE(outcome.applied);
    CO_REQUIRE_EQ(outcome.reason, ReasonCode::Ok);
  }
  const std::uint64_t durable_size = size_of(log_path);
  const std::size_t frames = frame_count(log_path);
  CO_REQUIRE_EQ(frames, 2U);

  // A separate process replays the same identity with identical content.
  const test::ProcessResult replay =
      run_role({"idempotent-replay", directory, "parent-m-replay", "7", "250000", "parent:durable-mutation"});
  CO_REQUIRE_EQ(replay.exit_code, 0);
  CO_REQUIRE(contains(replay.standard_output, "outcome idempotent-replay"));
  CO_REQUIRE(contains(replay.standard_output, "applied 0"));
  CO_REQUIRE(contains(replay.standard_output, "mutation-digest " + digest));
  // The replay appended its own session epoch frame and nothing else.
  // The replay added no frame at all: the counts before and after the replay
  // call inside the child are equal, and both include that session's epoch frame.
  CO_REQUIRE(contains(replay.standard_output, "frames-before 3"));
  CO_REQUIRE(contains(replay.standard_output, "frames-after 3"));
  CO_REQUIRE_EQ(frame_count(log_path), 3U);
  CO_REQUIRE(size_of(log_path) > durable_size);

  // A separate process replays the same identity with different content.
  const test::ProcessResult conflict =
      run_role({"idempotent-replay", directory, "parent-m-replay", "8", "250000", "parent:durable-mutation"});
  CO_REQUIRE_EQ(conflict.exit_code, 0);
  CO_REQUIRE(contains(conflict.standard_output, "outcome idempotency-conflict"));
  CO_REQUIRE(contains(conflict.standard_output, "applied 0"));

  // Nothing the two children did changed the durable record set.
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_EQ(store.mutations().size(), 1U);
    CO_REQUIRE_OK(store.load_evidence(), loaded);
    CO_REQUIRE_EQ(loaded.size(), 1U);
    CO_REQUIRE(loaded[0].record.content_digest().hex() == digest);
    CO_REQUIRE(loaded[0].mutation == mutation_of("parent-m-replay"));
  }
}

// ---------------------------------------------------------------------------
// Atomic snapshot publication under a killed writer
// ---------------------------------------------------------------------------
CO_TEST(a_writer_killed_mid_publication_leaves_the_previous_complete_snapshot) {
  test::ScratchDirectory scratch("mp-snapshot");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
  const StoreOptions options = options_for(directory);
  const std::string snapshot_path = io::join(directory, "snapshot.json");
  const std::string temp_path = io::temp_sibling(snapshot_path, "publish");

  const std::string first_document = "SNAPSHOT-ONE-COMPLETE-0123456789";
  const std::string second_document = "SNAPSHOT-TWO-NEVER-PUBLISHED-9876543210-abcdefghij";

  ChildProcess child;
  CO_REQUIRE(child.spawn(test::current_executable(),
                         {"--child-role", "snapshot-partial-then-block", directory, first_document, second_document}));
  CO_REQUIRE(child.valid());

  std::string published_digest;
  std::uint64_t partial_half = 0;
  for (;;) {
    const std::string line = child.read_line();
    if (line == "ready") {
      break;
    }
    CO_REQUIRE(!line.empty());
    const std::vector<std::string> parts = split_whitespace(line);
    if (parts.size() == 2U && parts[0] == "published") {
      published_digest = parts[1];
    } else if (parts.size() == 2U && parts[0] == "partial-temp") {
      CO_REQUIRE(parse_u64_arg(parts[1], partial_half));
    }
  }
  CO_REQUIRE(!published_digest.empty());
  CO_REQUIRE(partial_half > 0U);
  CO_REQUIRE(child.running());
  CO_REQUIRE(published_digest == sha256(first_document).hex());

  child.terminate_now();
  CO_REQUIRE_EQ(child.wait_for_exit(), 1);

  // The published file is the complete first document, never the mixture that
  // the killed writer left in the temporary sibling.
  CO_REQUIRE(io::exists(snapshot_path));
  CO_REQUIRE(io::exists(temp_path));
  CO_REQUIRE(same_bytes(read_bytes(snapshot_path), bytes_of(first_document)));
  CO_REQUIRE_EQ(size_of(snapshot_path), static_cast<std::uint64_t>(first_document.size()));
  CO_REQUIRE_EQ(size_of(temp_path), partial_half);
  const std::vector<std::uint8_t> temp_bytes = read_bytes(temp_path);
  CO_REQUIRE(temp_bytes.size() < second_document.size());
  CO_REQUIRE(std::equal(temp_bytes.begin(), temp_bytes.end(), bytes_of(second_document).begin()));

  // A fresh process reads the old complete document and ignores the stale
  // temporary sibling entirely.
  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_OK(store.read_snapshot(), document);
    CO_REQUIRE(document.has_value());
    CO_REQUIRE_EQ(document.value(), first_document);
    CO_REQUIRE(io::exists(temp_path));

    // The next publication replaces the file atomically and consumes the stale
    // sibling; nothing of the mixture was ever observable.
    CO_REQUIRE_OK(store.publish_snapshot(second_document), digest);
    CO_REQUIRE(digest == sha256(second_document));
    CO_REQUIRE(!io::exists(temp_path));
    CO_REQUIRE(same_bytes(read_bytes(snapshot_path), bytes_of(second_document)));
    CO_REQUIRE_OK(store.read_snapshot(), updated);
    CO_REQUIRE(updated.has_value());
    CO_REQUIRE_EQ(updated.value(), second_document);
  }

  // The digest recorded in the log matches the published bytes for both
  // publications; the interrupted one was never recorded as published.
  const LogSummary summary = summarize_log(read_bytes(io::join(directory, "evidence.log")));
  CO_REQUIRE(summary.header_ok);
  std::vector<std::string> snapshot_digests;
  for (const std::string& payload : summary.payloads) {
    const Result<json::Value> parsed = json::Value::parse(payload);
    CO_REQUIRE(parsed.ok());
    const Result<std::string> kind = parsed.value().require_string("kind");
    CO_REQUIRE(kind.ok());
    if (kind.value() == "snapshot") {
      const Result<std::string> digest_text = parsed.value().require_string("digest");
      CO_REQUIRE(digest_text.ok());
      snapshot_digests.push_back(digest_text.value());
    }
  }
  CO_REQUIRE_EQ(snapshot_digests.size(), 2U);
  CO_REQUIRE_EQ(snapshot_digests[0], sha256(first_document).hex());
  CO_REQUIRE_EQ(snapshot_digests[1], sha256(second_document).hex());
}

// A read-only open is documented as a lock-free reader. On Windows the log is
// opened read/write even for a read-only store, so the open is refused with
// IoFailure while a live writer holds the file (reported to the library owner:
// read-only opens should use the read-only file handle). Until then this case
// pins the safe part of the contract: inspection never needs the lock, and a
// read-only open never modifies the store.
CO_TEST(a_read_only_open_alongside_a_live_writer_never_corrupts_the_store) {
  test::ScratchDirectory scratch("mp-readonly-live");
  const std::string directory = scratch.child("store");
  CO_REQUIRE_OK_VOID(io::ensure_directory(directory));
  const StoreOptions options = options_for(directory);
  const std::string log_path = io::join(directory, "evidence.log");

  {
    CO_REQUIRE_STORE(Store::open(options), store);
    CO_REQUIRE_OK(store.append_evidence(fixture_record(1, 1000, "parent:live"), mutation_of("parent-m-live")), first);
    CO_REQUIRE(first.applied);

    const std::vector<std::uint8_t> before = read_bytes(log_path);
    StoreOptions read_only = options;
    read_only.read_only = true;
    const Result<Store> reader = Store::open(read_only);
    if (reader.ok()) {
      CO_REQUIRE_OK(reader.value().load_evidence(), loaded);
      CO_REQUIRE_EQ(loaded.size(), 1U);
      CO_REQUIRE_EQ(reader.value().session_epoch() == store.session_epoch(), true);
    } else {
      CO_REQUIRE_EQ(reader.status().code(), ReasonCode::IoFailure);
      // Inspection still works without any lock, from this process and another.
      CO_REQUIRE_OK(inspect_store(options), inspected);
      CO_REQUIRE_EQ(inspected.frames_read, 2U);
      const test::ProcessResult inspector = run_role({"inspect-probe", directory});
      CO_REQUIRE(contains(inspector.standard_output, "INSPECTED frames 2"));
    }
    CO_REQUIRE(same_bytes(read_bytes(log_path), before));
    CO_REQUIRE(store.is_open());
    CO_REQUIRE_ERR(Store::open(options), ReasonCode::LockHeld);
  }
}
