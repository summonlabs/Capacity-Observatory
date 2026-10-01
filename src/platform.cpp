// Capacity Observatory - platform services and durable file primitives.
#include "capacity_observatory/io.hpp"

#include "capacity_observatory/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace co::platform {
namespace {

#if defined(_WIN32)
std::string narrow_from_wide(const wchar_t* text) {
  if (text == nullptr) {
    return std::string();
  }
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return std::string();
  }
  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  if (needed > 1) {
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
  }
  return out;
}
#endif

}  // namespace

void suppress_error_dialogs() noexcept {
#if defined(_WIN32)
  // No critical-error, GPF, or "open file" dialogs; the process keeps running
  // headless and failures reach stderr.
  ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  // CRT: report assert/abort on stderr, never a modal window.
  ::_set_error_mode(_OUT_TO_STDERR);
  ::_set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
}

std::string os_error_text(unsigned long code) {
#if defined(_WIN32)
  wchar_t* buffer = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
      static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<wchar_t*>(&buffer), 0,
      nullptr);
  std::string text;
  if (length != 0 && buffer != nullptr) {
    text = narrow_from_wide(buffer);
    // Trim the trailing CR/LF that FormatMessage appends.
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
      text.pop_back();
    }
  }
  if (buffer != nullptr) {
    ::LocalFree(buffer);
  }
  if (text.empty()) {
    text = "operating system error " + std::to_string(code);
  } else {
    text += " (os error " + std::to_string(code) + ")";
  }
  return text;
#else
  const std::string text = std::strerror(static_cast<int>(code));
  return text + " (errno " + std::to_string(code) + ")";
#endif
}

std::string last_os_error_text() {
#if defined(_WIN32)
  return os_error_text(static_cast<unsigned long>(::GetLastError()));
#else
  return os_error_text(static_cast<unsigned long>(errno));
#endif
}

std::int64_t system_unix_nanos() noexcept {
#if defined(_WIN32)
  FILETIME file_time{};
  ::GetSystemTimeAsFileTime(&file_time);
  const std::uint64_t ticks = (static_cast<std::uint64_t>(file_time.dwHighDateTime) << 32) |
                              static_cast<std::uint64_t>(file_time.dwLowDateTime);
  // FILETIME counts 100 ns intervals since 1601-01-01.
  constexpr std::uint64_t kUnixEpochInFileTime = 116444736000000000ULL;
  if (ticks < kUnixEpochInFileTime) {
    return -static_cast<std::int64_t>((kUnixEpochInFileTime - ticks) * 100ULL);
  }
  return static_cast<std::int64_t>((ticks - kUnixEpochInFileTime) * 100ULL);
#else
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

unsigned int hardware_threads() noexcept {
  const unsigned int count = std::thread::hardware_concurrency();
  return count == 0 ? 1u : count;
}

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

}  // namespace co::platform

namespace co::io {
namespace {

#if defined(_WIN32)

constexpr std::size_t kMaxChunk = 1U << 30U;

bool to_wide(std::string_view text, std::wstring& out) {
  if (text.empty() || text.size() > 0x7FFFFFF0ULL) {
    return false;
  }
  const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return false;
  }
  out.assign(static_cast<std::size_t>(needed), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), out.data(), needed);
  return written == needed;
}

HANDLE native(void* handle) { return static_cast<HANDLE>(handle); }

Status last_error_status(ReasonCode code, std::string_view context) {
  const DWORD error = ::GetLastError();
  return make_error(code, std::string(context) + " failed: " + platform::os_error_text(error));
}

#else

int native(void* handle) { return static_cast<int>(reinterpret_cast<std::intptr_t>(handle)); }

Status last_error_status(ReasonCode code, std::string_view context) {
  const int error = errno;
  return make_error(code, std::string(context) + " failed: " + platform::os_error_text(static_cast<unsigned long>(error)));
}

#endif

}  // namespace

File::~File() { close(); }

File::File(File&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool File::is_open() const noexcept { return handle_ != nullptr; }

void File::close() noexcept {
  if (handle_ == nullptr) {
    return;
  }
#if defined(_WIN32)
  ::CloseHandle(native(handle_));
#else
  ::close(native(handle_));
#endif
  handle_ = nullptr;
}

Result<File> File::open(const std::string& path, bool create_if_missing) {
#if defined(_WIN32)
  std::wstring wide;
  if (!to_wide(path, wide)) {
    return make_error(ReasonCode::InvalidArgument, "path is not valid UTF-8: " + path);
  }
  const DWORD disposition = create_if_missing ? OPEN_ALWAYS : OPEN_EXISTING;
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE | FILE_APPEND_DATA,
                                FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return last_error_status(ReasonCode::IoFailure, "CreateFileW(" + path + ")");
  }
  File file;
  file.handle_ = handle;
  return Result<File>(std::move(file));
#else
  const int flags = O_RDWR | (create_if_missing ? O_CREAT : 0);
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    return last_error_status(ReasonCode::IoFailure, "open(" + path + ")");
  }
  File file;
  file.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  return Result<File>(std::move(file));
#endif
}

Result<File> File::open_read_only(const std::string& path) {
#if defined(_WIN32)
  std::wstring wide;
  if (!to_wide(path, wide)) {
    return make_error(ReasonCode::InvalidArgument, "path is not valid UTF-8: " + path);
  }
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return last_error_status(ReasonCode::IoFailure, "CreateFileW(" + path + ")");
  }
  File file;
  file.handle_ = handle;
  return Result<File>(std::move(file));
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return last_error_status(ReasonCode::IoFailure, "open(" + path + ")");
  }
  File file;
  file.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  return Result<File>(std::move(file));
#endif
}

Result<std::uint64_t> File::size() const {
  if (handle_ == nullptr) {
    return make_error(ReasonCode::StoreClosed, "file handle is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER length{};
  if (::GetFileSizeEx(native(handle_), &length) == 0) {
    return last_error_status(ReasonCode::IoFailure, "GetFileSizeEx");
  }
  return Result<std::uint64_t>(static_cast<std::uint64_t>(length.QuadPart));
#else
  struct stat info {};
  if (::fstat(native(handle_), &info) != 0) {
    return last_error_status(ReasonCode::IoFailure, "fstat");
  }
  return Result<std::uint64_t>(static_cast<std::uint64_t>(info.st_size));
#endif
}

Result<std::uint64_t> File::position() const {
  if (handle_ == nullptr) {
    return make_error(ReasonCode::StoreClosed, "file handle is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER zero{};
  LARGE_INTEGER current{};
  if (::SetFilePointerEx(native(handle_), zero, &current, FILE_CURRENT) == 0) {
    return last_error_status(ReasonCode::IoFailure, "SetFilePointerEx(query)");
  }
  return Result<std::uint64_t>(static_cast<std::uint64_t>(current.QuadPart));
#else
  const off_t current = ::lseek(native(handle_), 0, SEEK_CUR);
  if (current < 0) {
    return last_error_status(ReasonCode::IoFailure, "lseek(query)");
  }
  return Result<std::uint64_t>(static_cast<std::uint64_t>(current));
#endif
}

Result<void> File::seek(std::uint64_t offset) {
  if (handle_ == nullptr) {
    return make_error(ReasonCode::StoreClosed, "file handle is not open");
  }
  if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
    return make_error(ReasonCode::InvalidArgument, "seek offset exceeds the representable file range");
  }
#if defined(_WIN32)
  LARGE_INTEGER distance{};
  distance.QuadPart = static_cast<LONGLONG>(offset);
  if (::SetFilePointerEx(native(handle_), distance, nullptr, FILE_BEGIN) == 0) {
    return last_error_status(ReasonCode::IoFailure, "SetFilePointerEx");
  }
  return Result<void>{};
#else
  if (::lseek(native(handle_), static_cast<off_t>(offset), SEEK_SET) < 0) {
    return last_error_status(ReasonCode::IoFailure, "lseek");
  }
  return Result<void>{};
#endif
}

Result<void> File::seek_end() {
  if (handle_ == nullptr) {
    return make_error(ReasonCode::StoreClosed, "file handle is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER zero{};
  if (::SetFilePointerEx(native(handle_), zero, nullptr, FILE_END) == 0) {
    return last_error_status(ReasonCode::IoFailure, "SetFilePointerEx(end)");
  }
  return Result<void>{};
#else
  if (::lseek(native(handle_), 0, SEEK_END) < 0) {
    return last_error_status(ReasonCode::IoFailure, "lseek(end)");
  }
  return Result<void>{};
#endif
}

Result<void> File::write_all(const void* data, std::size_t size) {
  if (handle_ == nullptr) {
    return make_error(ReasonCode::StoreClosed, "file handle is not open");
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t written_total = 0;
  while (written_total < size) {
    const std::size_t remaining = size - written_total;
    const std::size_t chunk = (std::min)(remaining, kMaxChunk);
#if defined(_WIN32)
    DWORD written = 0;
    if (::WriteFile(native(handle_), bytes + written_total, static_cast<DWORD>(chunk), &written, nullptr) == 0) {
      return last_error_status(ReasonCode::IoFailure, "WriteFile");
    }
    if (written == 0) {
      return make_error(ReasonCode::ShortWrite,
                        "WriteFile reported zero bytes written with " + std::to_string(remaining) + " pending");
    }
#else
    const ssize_t written = ::write(native(handle_), bytes + written_total, chunk);
    if (written < 0) {
      return last_error_status(ReasonCode::IoFailure, "write");
    }
    if (written == 0) {
      return make_error(ReasonCode::ShortWrite, "write reported zero bytes");
    }
#endif
    written_total += static_cast<std::size_t>(written);
  }
  return Result<void>{};
}

Result<std::size_t> File::read_at(std::uint64_t offset, void* buffer, std::size_t size) {
  if (handle_ == nullptr) {
    return make_error(ReasonCode::StoreClosed, "file handle is not open");
  }
  auto* bytes = static_cast<std::uint8_t*>(buffer);
  std::size_t read_total = 0;
  while (read_total < size) {
    const std::size_t remaining = size - read_total;
    const std::size_t chunk = (std::min)(remaining, kMaxChunk);
#if defined(_WIN32)
    OVERLAPPED overlapped{};
    const std::uint64_t position = offset + static_cast<std::uint64_t>(read_total);
    overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>((position >> 32U) & 0xFFFFFFFFULL);
    DWORD read = 0;
    if (::ReadFile(native(handle_), bytes + read_total, static_cast<DWORD>(chunk), &read, &overlapped) == 0) {
      return last_error_status(ReasonCode::IoFailure, "ReadFile");
    }
#else
    const ssize_t read = ::pread(native(handle_), bytes + read_total, chunk,
                                 static_cast<off_t>(offset + static_cast<std::uint64_t>(read_total)));
    if (read < 0) {
      return last_error_status(ReasonCode::IoFailure, "pread");
    }
#endif
    if (read == 0) {
      break;  // clean end of file
    }
    read_total += static_cast<std::size_t>(read);
  }
  return Result<std::size_t>(read_total);
}

Result<void> File::truncate(std::uint64_t size) {
  if (handle_ == nullptr) {
    return make_error(ReasonCode::StoreClosed, "file handle is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER distance{};
  distance.QuadPart = static_cast<LONGLONG>(size);
  if (::SetFilePointerEx(native(handle_), distance, nullptr, FILE_BEGIN) == 0) {
    return last_error_status(ReasonCode::IoFailure, "SetFilePointerEx(truncate)");
  }
  if (::SetEndOfFile(native(handle_)) == 0) {
    return last_error_status(ReasonCode::IoFailure, "SetEndOfFile");
  }
  return Result<void>{};
#else
  if (::ftruncate(native(handle_), static_cast<off_t>(size)) != 0) {
    return last_error_status(ReasonCode::IoFailure, "ftruncate");
  }
  return Result<void>{};
#endif
}

Result<void> File::sync() {
  if (handle_ == nullptr) {
    return make_error(ReasonCode::StoreClosed, "file handle is not open");
  }
#if defined(_WIN32)
  if (::FlushFileBuffers(native(handle_)) == 0) {
    return last_error_status(ReasonCode::FlushFailure, "FlushFileBuffers");
  }
  return Result<void>{};
#else
  if (::fsync(native(handle_)) != 0) {
    return last_error_status(ReasonCode::FlushFailure, "fsync");
  }
  return Result<void>{};
#endif
}

bool exists(const std::string& path) {
#if defined(_WIN32)
  std::wstring wide;
  if (!to_wide(path, wide)) {
    return false;
  }
  const DWORD attributes = ::GetFileAttributesW(wide.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES;
#else
  struct stat info {};
  return ::stat(path.c_str(), &info) == 0;
#endif
}

Result<void> remove_file(const std::string& path) {
#if defined(_WIN32)
  std::wstring wide;
  if (!to_wide(path, wide)) {
    return make_error(ReasonCode::InvalidArgument, "path is not valid UTF-8: " + path);
  }
  if (::DeleteFileW(wide.c_str()) == 0) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return Result<void>{};  // removal is idempotent
    }
    return make_error(ReasonCode::IoFailure, "DeleteFileW(" + path + ") failed: " + platform::os_error_text(error));
  }
  return Result<void>{};
#else
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return last_error_status(ReasonCode::IoFailure, "unlink(" + path + ")");
  }
  return Result<void>{};
#endif
}

Result<void> ensure_directory(const std::string& path) {
  if (path.empty()) {
    return make_error(ReasonCode::InvalidArgument, "directory path is empty");
  }
  if (exists(path)) {
    return Result<void>{};
  }
#if defined(_WIN32)
  std::wstring wide;
  if (!to_wide(path, wide)) {
    return make_error(ReasonCode::InvalidArgument, "path is not valid UTF-8: " + path);
  }
  if (::CreateDirectoryW(wide.c_str(), nullptr) == 0) {
    const DWORD error = ::GetLastError();
    if (error != ERROR_ALREADY_EXISTS) {
      return make_error(ReasonCode::IoFailure,
                        "CreateDirectoryW(" + path + ") failed: " + platform::os_error_text(error));
    }
  }
  return Result<void>{};
#else
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return last_error_status(ReasonCode::IoFailure, "mkdir(" + path + ")");
  }
  return Result<void>{};
#endif
}

Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::uint64_t max_bytes) {
  Result<File> file = File::open_read_only(path);
  if (!file.ok()) {
    return file.status();
  }
  const Result<std::uint64_t> length = file.value().size();
  if (!length.ok()) {
    return length.status();
  }
  if (length.value() > max_bytes) {
    return make_error(ReasonCode::CapacityExceeded,
                      "file " + path + " is " + std::to_string(length.value()) + " bytes which exceeds the limit of " +
                          std::to_string(max_bytes) + " bytes");
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(length.value()));
  if (!buffer.empty()) {
    const Result<std::size_t> read = file.value().read_at(0, buffer.data(), buffer.size());
    if (!read.ok()) {
      return read.status();
    }
    if (read.value() != buffer.size()) {
      return make_error(ReasonCode::ShortWrite,
                        "file " + path + " changed size while being read: expected " +
                            std::to_string(buffer.size()) + " bytes, read " + std::to_string(read.value()));
    }
  }
  return Result<std::vector<std::uint8_t>>(std::move(buffer));
}

Result<void> atomic_replace(const std::string& temp_path, const std::string& destination) {
#if defined(_WIN32)
  std::wstring wide_source;
  std::wstring wide_destination;
  if (!to_wide(temp_path, wide_source) || !to_wide(destination, wide_destination)) {
    return make_error(ReasonCode::InvalidArgument, "path is not valid UTF-8");
  }
  if (::MoveFileExW(wide_source.c_str(), wide_destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return last_error_status(ReasonCode::PublishFailure, "MoveFileExW(" + destination + ")");
  }
  return Result<void>{};
#else
  if (::rename(temp_path.c_str(), destination.c_str()) != 0) {
    return last_error_status(ReasonCode::PublishFailure, "rename(" + destination + ")");
  }
  return Result<void>{};
#endif
}

Result<void> write_file_atomic(const std::string& path, const void* data, std::size_t size) {
  const std::string temp = temp_sibling(path, "publish");
  {
    Result<File> file = File::open(temp, true);
    if (!file.ok()) {
      return file.status();
    }
    if (!file.value().truncate(0).ok()) {
      const Result<void> removed = remove_file(temp);
      (void)removed;
      return make_error(ReasonCode::IoFailure, "could not truncate temporary publication file " + temp);
    }
    const Result<void> written = file.value().write_all(data, size);
    if (!written.ok()) {
      const Result<void> removed = remove_file(temp);
      (void)removed;
      return written.status();
    }
    const Result<void> synced = file.value().sync();
    if (!synced.ok()) {
      const Result<void> removed = remove_file(temp);
      (void)removed;
      return synced.status();
    }
  }
  const Result<void> published = atomic_replace(temp, path);
  if (!published.ok()) {
    const Result<void> removed = remove_file(temp);
    (void)removed;
    return published.status();
  }
  return Result<void>{};
}

std::string join(std::string_view directory, std::string_view leaf) {
  std::string out(directory);
  if (!out.empty() && out.back() != '/' && out.back() != '\\') {
    out.push_back('/');
  }
  out.append(leaf);
  return out;
}

std::string parent_directory(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) {
    return std::string(".");
  }
  if (slash == 0) {
    return path.substr(0, 1);
  }
  return path.substr(0, slash);
}

std::string file_name(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string temp_sibling(const std::string& path, std::string_view marker) {
  return path + "." + std::string(marker) + ".tmp";
}

void ExclusiveLock::release() noexcept {
  if (handle_ == nullptr) {
    return;
  }
#if defined(_WIN32)
  OVERLAPPED overlapped{};
  ::UnlockFileEx(native(handle_), 0, 1, 0, &overlapped);
  ::CloseHandle(native(handle_));
#else
  ::flock(native(handle_), LOCK_UN);
  ::close(native(handle_));
#endif
  handle_ = nullptr;
}

ExclusiveLock::~ExclusiveLock() { release(); }

ExclusiveLock::ExclusiveLock(ExclusiveLock&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

ExclusiveLock& ExclusiveLock::operator=(ExclusiveLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool ExclusiveLock::held() const noexcept { return handle_ != nullptr; }

Result<ExclusiveLock> ExclusiveLock::try_acquire(const std::string& path) {
#if defined(_WIN32)
  std::wstring wide;
  if (!to_wide(path, wide)) {
    return make_error(ReasonCode::InvalidArgument, "path is not valid UTF-8: " + path);
  }
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return last_error_status(ReasonCode::IoFailure, "CreateFileW(" + path + ")");
  }
  OVERLAPPED overlapped{};
  if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) == 0) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(handle);
    if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING || error == ERROR_SHARING_VIOLATION) {
      return make_error(ReasonCode::LockHeld, "exclusive lock on " + path + " is held by another process");
    }
    return make_error(ReasonCode::IoFailure, "LockFileEx(" + path + ") failed: " + platform::os_error_text(error));
  }
  ExclusiveLock lock;
  lock.handle_ = handle;
  return Result<ExclusiveLock>(std::move(lock));
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return last_error_status(ReasonCode::IoFailure, "open(" + path + ")");
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int error = errno;
    ::close(fd);
    if (error == EWOULDBLOCK || error == EAGAIN) {
      return make_error(ReasonCode::LockHeld, "exclusive lock on " + path + " is held by another process");
    }
    return make_error(ReasonCode::IoFailure, "flock(" + path + ") failed: " + platform::os_error_text(static_cast<unsigned long>(error)));
  }
  ExclusiveLock lock;
  lock.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  return Result<ExclusiveLock>(std::move(lock));
#endif
}

}  // namespace co::io
