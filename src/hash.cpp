// Capacity Observatory - CRC-32C (Castagnoli) and SHA-256.
#include "capacity_observatory/hash.hpp"

#include <array>
#include <cstring>

namespace co {
namespace {

// Reflected Castagnoli polynomial; the table holds one entry per byte value.
constexpr std::size_t kCrc32cTableEntries = 256;

struct Crc32cTable {
  std::array<std::uint32_t, kCrc32cTableEntries> entries{};

  constexpr Crc32cTable() noexcept {
    constexpr std::uint32_t kPolynomial = 0x82F63B78U;
    for (std::size_t i = 0; i < kCrc32cTableEntries; ++i) {
      std::uint32_t crc = static_cast<std::uint32_t>(i);
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1U) != 0U ? (crc >> 1U) ^ kPolynomial : (crc >> 1U);
      }
      // at() carries its own bound and this constructor is constexpr, so the
      // check is evaluated once at compile time and costs nothing at run time.
      // It is also the form the static analyzer can prove.
      entries.at(i) = crc;
    }
  }
};

constexpr Crc32cTable kCrc32cTable{};

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants{
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned int amount) noexcept {
  return (value >> amount) | (value << (32U - amount));
}

constexpr std::uint32_t big_endian_load(const std::uint8_t* bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

}  // namespace

std::uint32_t crc32c_bytes(const void* data, std::size_t size, std::uint32_t seed) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = ~seed;
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint8_t index = static_cast<std::uint8_t>((crc ^ bytes[i]) & 0xFFU);
    crc = kCrc32cTable.entries[index] ^ (crc >> 8U);
  }
  return ~crc;
}

std::uint32_t crc32c(std::string_view data, std::uint32_t seed) noexcept {
  return crc32c_bytes(data.data(), data.size(), seed);
}

std::uint64_t fnv1a64(std::string_view data) noexcept {
  std::uint64_t accumulator = 14695981039346656037ULL;
  for (const char raw : data) {
    accumulator ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
    accumulator *= 1099511628211ULL;
  }
  return accumulator;
}

Sha256::Sha256() noexcept {
  state_[0] = 0x6A09E667U;
  state_[1] = 0xBB67AE85U;
  state_[2] = 0x3C6EF372U;
  state_[3] = 0xA54FF53AU;
  state_[4] = 0x510E527FU;
  state_[5] = 0x9B05688CU;
  state_[6] = 0x1F83D9ABU;
  state_[7] = 0x5BE0CD19U;
}

void Sha256::compress(const std::uint8_t block[64]) noexcept {
  std::uint32_t schedule[64];
  for (std::size_t i = 0; i < 16; ++i) {
    schedule[i] = big_endian_load(block + i * 4);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        rotr(schedule[i - 15], 7) ^ rotr(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3U);
    const std::uint32_t s1 =
        rotr(schedule[i - 2], 17) ^ rotr(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10U);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + choose + kSha256RoundConstants[i] + schedule[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  if (finished_ || size == 0) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  bit_count_ += static_cast<std::uint64_t>(size) * 8ULL;
  std::size_t offset = 0;
  if (buffer_size_ != 0) {
    const std::size_t needed = 64 - buffer_size_;
    const std::size_t take = (size < needed) ? size : needed;
    std::memcpy(buffer_ + buffer_size_, bytes, take);
    buffer_size_ += take;
    offset += take;
    if (buffer_size_ == 64) {
      compress(buffer_);
      buffer_size_ = 0;
    }
  }
  while (size - offset >= 64) {
    compress(bytes + offset);
    offset += 64;
  }
  if (offset < size) {
    std::memcpy(buffer_, bytes + offset, size - offset);
    buffer_size_ = size - offset;
  }
}

Digest Sha256::finish() noexcept {
  if (!finished_) {
    // Padding: 0x80, zeros, then the 64-bit big-endian message length.
    const std::uint64_t total_bits = bit_count_;
    std::uint8_t padding[128]{};
    std::size_t padding_size = (buffer_size_ < 56) ? (56 - buffer_size_) : (120 - buffer_size_);
    padding[0] = 0x80U;
    for (std::size_t i = 0; i < 8; ++i) {
      padding[padding_size + i] = static_cast<std::uint8_t>((total_bits >> (56U - 8U * i)) & 0xFFU);
    }
    // update() would change bit_count_, which is already recorded.
    const std::uint64_t saved_count = bit_count_;
    update(padding, padding_size + 8);
    bit_count_ = saved_count;
    finished_ = true;
  }

  std::uint8_t digest_bytes[Digest::kBytes];
  for (std::size_t i = 0; i < 8; ++i) {
    digest_bytes[i * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24U) & 0xFFU);
    digest_bytes[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16U) & 0xFFU);
    digest_bytes[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8U) & 0xFFU);
    digest_bytes[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFU);
  }
  return Digest::from_bytes(digest_bytes);
}

Digest sha256(const void* data, std::size_t size) noexcept {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

Digest sha256(std::string_view data) noexcept { return sha256(data.data(), data.size()); }

}  // namespace co
