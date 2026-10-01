#pragma once
// Capacity Observatory - dependency free JSON value, strict parser, deterministic writer.
//
// The observatory exchanges evidence and explanation documents as JSON. The
// parser is deliberately strict (no trailing garbage, no duplicate keys, no
// implicit numeric coercions) because a silently accepted malformed document
// would become silently wrong capacity accounting.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"

namespace co::json {

class Value;

using Array = std::vector<Value>;
// Member order is preserved so that human facing output keeps the order the
// producer chose; canonical output sorts by key.
using Object = std::vector<std::pair<std::string, Value>>;

enum class Kind : std::uint8_t { Null = 0, Bool = 1, Number = 2, String = 3, Array = 4, Object = 5 };

[[nodiscard]] std::string_view kind_text(Kind kind) noexcept;

class Value {
 public:
  Value() noexcept = default;  // null
  explicit Value(bool boolean) noexcept;
  explicit Value(std::int64_t number) noexcept;
  explicit Value(std::string text);
  explicit Value(const char* text);

  [[nodiscard]] static Value array(Array items = {});
  [[nodiscard]] static Value object(Object members = {});
  // Strict parse. 'max_depth' can lower the accepted nesting but never raise it
  // past the compile time ceiling of 64 levels, so recursion - and the recursion
  // in a nested Value's destructor - is bounded by a constant rather than by the
  // size of the input document.
  [[nodiscard]] static Result<Value> parse(std::string_view text, std::size_t max_depth = 64);

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return kind_ == Kind::Bool; }
  [[nodiscard]] bool is_number() const noexcept { return kind_ == Kind::Number; }
  [[nodiscard]] bool is_string() const noexcept { return kind_ == Kind::String; }
  [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::Array; }
  [[nodiscard]] bool is_object() const noexcept { return kind_ == Kind::Object; }

  [[nodiscard]] Result<bool> as_bool() const;
  [[nodiscard]] Result<std::int64_t> as_int() const;
  [[nodiscard]] Result<std::string> as_string() const;
  [[nodiscard]] const Array* as_array() const noexcept;
  [[nodiscard]] const Object* as_object() const noexcept;

  // Object access. find() returns nullptr for a missing member or a non-object.
  [[nodiscard]] const Value* find(std::string_view key) const noexcept;
  // The returned pointer is never null on success. Result is a std::variant,
  // whose alternatives must be object types, so it cannot hold a reference.
  [[nodiscard]] Result<const Value*> require(std::string_view key) const;
  // require_* combine lookup, presence and type checking into one status.
  [[nodiscard]] Result<std::string> require_string(std::string_view key) const;
  [[nodiscard]] Result<std::int64_t> require_int(std::string_view key) const;
  [[nodiscard]] Result<bool> require_bool(std::string_view key) const;
  [[nodiscard]] Result<const Array*> require_array(std::string_view key) const;
  // Returns nullopt when the member is absent, an error when it is present with
  // the wrong type. Absence and malformed are never conflated.
  [[nodiscard]] Result<std::optional<std::string>> optional_string(std::string_view key) const;

  void set(std::string key, Value value);
  void push(Value value);

  [[nodiscard]] std::size_t size() const noexcept;

  friend bool operator==(const Value& a, const Value& b) noexcept;

 private:
  Kind kind_{Kind::Null};
  bool boolean_{false};
  std::int64_t number_{0};
  std::string text_;
  Array items_;
  Object members_;
};

inline namespace literals {
// Reserved for future literal helpers; presence keeps the namespace stable.
}

// Deterministic serialization. With canonical=true, object members are sorted by
// key and no insignificant whitespace is emitted. indent < 0 means compact.
[[nodiscard]] std::string dump(const Value& value, bool canonical = true, int indent = -1);

}  // namespace co::json
