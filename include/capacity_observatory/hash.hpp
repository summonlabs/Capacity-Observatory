#pragma once
// Capacity Observatory - integrity primitives.
//
// SHA-256 is used for content identity and is validated against published NIST
// vectors. CRC-32C is used for record framing integrity and is validated against
// the published Castagnoli check value.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "capacity_observatory/strong.hpp"

namespace co {

// Distinct names keep string and buffer call sites unambiguous.
[[nodiscard]] std::uint32_t crc32c(std::string_view data, std::uint32_t seed = 0) noexcept;
[[nodiscard]] std::uint32_t crc32c_bytes(const void* data, std::size_t size, std::uint32_t seed = 0) noexcept;

[[nodiscard]] std::uint64_t fnv1a64(std::string_view data) noexcept;

class Sha256 {
 public:
  Sha256() noexcept;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view data) noexcept { update(data.data(), data.size()); }
  void update(std::uint8_t byte) noexcept { update(&byte, 1); }
  [[nodiscard]] Digest finish() noexcept;

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::uint32_t state_[8]{};
  std::uint64_t bit_count_{0};
  std::uint8_t buffer_[64]{};
  std::size_t buffer_size_{0};
  bool finished_{false};
};

[[nodiscard]] Digest sha256(const void* data, std::size_t size) noexcept;
[[nodiscard]] Digest sha256(std::string_view data) noexcept;

}  // namespace co
