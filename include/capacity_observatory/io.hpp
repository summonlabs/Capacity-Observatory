#pragma once
// Capacity Observatory - durable file primitives.
//
// All file access funnels through this header so that durability points, atomic
// publication, and single-writer locking have exactly one implementation.
// Every operation returns a Status; none of them throw.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"

namespace co::io {

class File {
 public:
  File() noexcept = default;
  ~File();
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  // Opens for read/write, positioned at the start. Creates the file when
  // 'create_if_missing' is set, otherwise the file must already exist.
  [[nodiscard]] static Result<File> open(const std::string& path, bool create_if_missing);
  [[nodiscard]] static Result<File> open_read_only(const std::string& path);

  [[nodiscard]] bool is_open() const noexcept;
  void close() noexcept;

  [[nodiscard]] Result<std::uint64_t> size() const;
  [[nodiscard]] Result<std::uint64_t> position() const;
  [[nodiscard]] Result<void> seek(std::uint64_t offset);
  [[nodiscard]] Result<void> seek_end();

  [[nodiscard]] Result<void> write_all(const void* data, std::size_t size);
  [[nodiscard]] Result<std::size_t> read_at(std::uint64_t offset, void* buffer, std::size_t size);
  [[nodiscard]] Result<void> truncate(std::uint64_t size);

  // Durability point for every byte written before this call.
  [[nodiscard]] Result<void> sync();

 private:
  void* handle_{nullptr};
};

[[nodiscard]] bool exists(const std::string& path);
[[nodiscard]] Result<void> remove_file(const std::string& path);
[[nodiscard]] Result<void> ensure_directory(const std::string& path);

// Reads at most max_bytes; larger files are refused with CapacityExceeded.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::uint64_t max_bytes);

// Writes 'path + ".tmp"', flushes it, then atomically replaces 'path'. The
// replacement is the publication point: readers observe either the previous or
// the new complete file, never a mixture.
[[nodiscard]] Result<void> write_file_atomic(const std::string& path, const void* data, std::size_t size);

// Renames temp_path over destination with write-through semantics.
[[nodiscard]] Result<void> atomic_replace(const std::string& temp_path, const std::string& destination);

[[nodiscard]] std::string join(std::string_view directory, std::string_view leaf);
[[nodiscard]] std::string parent_directory(const std::string& path);
[[nodiscard]] std::string file_name(const std::string& path);
[[nodiscard]] std::string temp_sibling(const std::string& path, std::string_view marker);

// Kernel enforced single-writer lock. The lock is released when the object is
// destroyed or the process dies, including on abnormal termination.
class ExclusiveLock {
 public:
  ExclusiveLock() noexcept = default;
  ~ExclusiveLock();
  ExclusiveLock(ExclusiveLock&& other) noexcept;
  ExclusiveLock& operator=(ExclusiveLock&& other) noexcept;
  ExclusiveLock(const ExclusiveLock&) = delete;
  ExclusiveLock& operator=(const ExclusiveLock&) = delete;

  // Non-blocking. Returns ReasonCode::LockHeld when another holder exists.
  [[nodiscard]] static Result<ExclusiveLock> try_acquire(const std::string& path);

  [[nodiscard]] bool held() const noexcept;

 private:
  void release() noexcept;
  void* handle_{nullptr};
};

}  // namespace co::io
