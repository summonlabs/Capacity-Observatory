// Capacity Observatory - dependency free JSON value, strict parser, deterministic writer.
//
// The parser is a deliberate RFC 8259 subset: integers only, no duplicate keys,
// no trailing bytes, no silently repaired input. Every refusal carries a byte
// offset and a one line explanation so that a malformed document can be pointed
// at instead of guessed about.

#include "capacity_observatory/json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace co::json {
namespace {

// The parser never descends deeper than this, whatever max_depth a caller asks
// for: recursion depth stays bounded by a compile time constant instead of by
// the size of the input document. Parsing and destroying a nested container both
// recurse once per level, so this ceiling is also the bound on destructor depth.
//
// The value is chosen from measurement, not taste: instrumented builds have far
// larger frames than release builds, and a 200 level document overflowed the
// default stack under AddressSanitizer while 128 parsed and destroyed cleanly.
// 64 is therefore the deepest document this parser promises to accept, which is
// an order of magnitude past any evidence, topology, profile, or snapshot
// document this project produces.
constexpr std::size_t kDepthCeiling = 64;

constexpr char kHexDigits[] = "0123456789abcdef";

// --- lexical helpers -------------------------------------------------------

constexpr bool is_whitespace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr bool is_hex_digit(char c) noexcept {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

constexpr std::uint32_t hex_digit_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return static_cast<std::uint32_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<std::uint32_t>(c - 'a') + 10U;
  }
  return static_cast<std::uint32_t>(c - 'A') + 10U;
}

// --- status construction ---------------------------------------------------

Status error_at(ReasonCode code, std::size_t offset, std::string_view what) {
  std::string detail(what);
  detail.append(" at byte ");
  detail.append(std::to_string(offset));
  return make_error(code, std::move(detail));
}

Status type_error(std::string_view expected, Kind actual) {
  std::string detail("expected ");
  detail.append(expected);
  detail.append(", found ");
  detail.append(kind_text(actual));
  return make_error(ReasonCode::TypeMismatch, std::move(detail));
}

Status missing_field(std::string_view key) {
  std::string detail("missing required member \"");
  detail.append(key);
  detail.push_back('"');
  return make_error(ReasonCode::MissingField, std::move(detail));
}

// Encodes one Unicode scalar value as UTF-8. Surrogates never reach this
// function: the escape reader refuses them before a code point is assembled.
void append_code_point(std::string& out, std::uint32_t code) {
  if (code <= 0x7FU) {
    out.push_back(static_cast<char>(code));
  } else if (code <= 0x7FFU) {
    out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
    out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
  } else if (code <= 0xFFFFU) {
    out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
  }
}

// Minimal escaping: quote, reverse solidus and the control characters RFC 8259
// requires to be escaped. Everything else, including non-ASCII UTF-8, is
// emitted byte for byte.
void append_escaped(std::string& out, std::string_view text) {
  for (const char ch : text) {
    const unsigned int byte = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (byte < 0x20U) {
          out.append("\\u00");
          out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
          out.push_back(kHexDigits[byte & 0x0FU]);
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
}

// --- parser ----------------------------------------------------------------

class Parser {
 public:
  Parser(std::string_view text, std::size_t max_depth) noexcept
      : text_(text), depth_limit_(max_depth < kDepthCeiling ? max_depth : kDepthCeiling) {
    // RFC 8259 section 8.1 forbids producers from adding a byte order mark but
    // explicitly permits an implementation to ignore one. Windows tooling emits
    // it routinely, so it is skipped and every reported byte offset still counts
    // from the start of the document the caller actually handed us.
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEFU &&
        static_cast<unsigned char>(text_[1]) == 0xBBU && static_cast<unsigned char>(text_[2]) == 0xBFU) {
      text_.remove_prefix(3);
      base_offset_ = 3;
    }
  }

  [[nodiscard]] Result<Value> document() {
    skip_whitespace();
    if (pos_ == text_.size()) {
      return make_error(ReasonCode::EmptyInput,
                        text_.empty() ? "input is empty" : "input contains only whitespace");
    }
    Result<Value> parsed = value_at_depth(1);
    if (!parsed.ok()) {
      return parsed.status();
    }
    skip_whitespace();
    if (pos_ != text_.size()) {
      return report(ReasonCode::TrailingGarbage, pos_, "unexpected byte after the top-level value");
    }
    return std::move(parsed);
  }

 private:
  void skip_whitespace() noexcept {
    while (pos_ < text_.size() && is_whitespace(text_[pos_])) {
      ++pos_;
    }
  }

  // Every parser diagnostic is expressed relative to the caller's original
  // document, so a skipped byte order mark never shifts a reported offset.
  [[nodiscard]] Status report(ReasonCode code, std::size_t offset, std::string_view what) const {
    return error_at(code, offset + base_offset_, what);
  }

  [[nodiscard]] Status depth_error(std::size_t offset) const {
    return report(ReasonCode::ParseError, offset,
                  "nesting is deeper than the permitted " + std::to_string(depth_limit_) + " levels");
  }

  Result<Value> value_at_depth(std::size_t depth) {
    if (pos_ >= text_.size()) {
      return report(ReasonCode::ParseError, pos_, "expected a JSON value but the input ended");
    }
    switch (text_[pos_]) {
      case '{':
        return object_at_depth(depth);
      case '[':
        return array_at_depth(depth);
      case '"': {
        Result<std::string> text = string_literal();
        if (!text.ok()) {
          return text.status();
        }
        return Value(std::move(text).value());
      }
      case 't':
        return literal("true", Value(true));
      case 'f':
        return literal("false", Value(false));
      case 'n':
        return literal("null", Value());
      case '+':
        return report(ReasonCode::ParseError, pos_, "a leading '+' is not a valid number prefix");
      default:
        break;
    }
    if (text_[pos_] == '-' || is_digit(text_[pos_])) {
      return number_literal();
    }
    return report(ReasonCode::ParseError, pos_, "unexpected character, expected a JSON value");
  }

  Result<Value> literal(std::string_view token, Value value) {
    if (text_.compare(pos_, token.size(), token) != 0) {
      return report(ReasonCode::ParseError, pos_, "invalid literal, expected '" + std::string(token) + "'");
    }
    pos_ += token.size();
    return value;
  }

  Result<Value> object_at_depth(std::size_t depth) {
    const std::size_t opening = pos_;
    if (depth > depth_limit_) {
      return depth_error(opening);
    }
    ++pos_;  // consume '{'
    Object members;
    skip_whitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return Value::object(std::move(members));
    }
    while (true) {
      if (pos_ >= text_.size()) {
        return report(ReasonCode::ParseError, opening, "unterminated object");
      }
      if (text_[pos_] != '"') {
        return report(ReasonCode::ParseError, pos_, "an object key must be a string");
      }
      const std::size_t key_offset = pos_;
      Result<std::string> key = string_literal();
      if (!key.ok()) {
        return key.status();
      }
      skip_whitespace();
      if (pos_ >= text_.size() || text_[pos_] != ':') {
        return report(ReasonCode::ParseError, pos_, "expected ':' after an object key");
      }
      ++pos_;
      skip_whitespace();
      Result<Value> member = value_at_depth(depth + 1);
      if (!member.ok()) {
        return member.status();
      }
      const std::string& key_text = key.value();
      for (const auto& existing : members) {
        if (existing.first == key_text) {
          return report(ReasonCode::DuplicateKey, key_offset, "duplicate object key \"" + key_text + "\"");
        }
      }
      members.emplace_back(std::move(key).value(), std::move(member).value());
      skip_whitespace();
      if (pos_ >= text_.size()) {
        return report(ReasonCode::ParseError, opening, "unterminated object");
      }
      const char separator = text_[pos_];
      if (separator == ',') {
        ++pos_;
        skip_whitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
          return report(ReasonCode::ParseError, pos_, "trailing comma in an object");
        }
        continue;
      }
      if (separator == '}') {
        ++pos_;
        return Value::object(std::move(members));
      }
      return report(ReasonCode::ParseError, pos_, "expected ',' or '}' in an object");
    }
  }

  Result<Value> array_at_depth(std::size_t depth) {
    const std::size_t opening = pos_;
    if (depth > depth_limit_) {
      return depth_error(opening);
    }
    ++pos_;  // consume '['
    Array items;
    skip_whitespace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return Value::array(std::move(items));
    }
    while (true) {
      if (pos_ >= text_.size()) {
        return report(ReasonCode::ParseError, opening, "unterminated array");
      }
      Result<Value> item = value_at_depth(depth + 1);
      if (!item.ok()) {
        return item.status();
      }
      items.push_back(std::move(item).value());
      skip_whitespace();
      if (pos_ >= text_.size()) {
        return report(ReasonCode::ParseError, opening, "unterminated array");
      }
      const char separator = text_[pos_];
      if (separator == ',') {
        ++pos_;
        skip_whitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
          return report(ReasonCode::ParseError, pos_, "trailing comma in an array");
        }
        continue;
      }
      if (separator == ']') {
        ++pos_;
        return Value::array(std::move(items));
      }
      return report(ReasonCode::ParseError, pos_, "expected ',' or ']' in an array");
    }
  }

  Result<std::string> string_literal() {
    const std::size_t opening = pos_;
    ++pos_;  // consume the opening quote
    std::string out;
    while (pos_ < text_.size()) {
      const char ch = text_[pos_];
      if (ch == '"') {
        ++pos_;
        return out;
      }
      if (ch == '\\') {
        const Status status = escape_into(out);
        if (status.failed()) {
          return status;
        }
        continue;
      }
      const unsigned int byte = static_cast<unsigned char>(ch);
      if (byte < 0x20U) {
        return report(ReasonCode::ParseError, pos_, "raw control character in a string");
      }
      if (byte < 0x80U) {
        out.push_back(ch);
        ++pos_;
        continue;
      }
      if (!append_utf8_sequence(out)) {
        return report(ReasonCode::ParseError, pos_, "invalid UTF-8 byte sequence in a string");
      }
    }
    return report(ReasonCode::ParseError, opening, "unterminated string");
  }

  Status escape_into(std::string& out) {
    const std::size_t escape_start = pos_;
    ++pos_;  // consume the reverse solidus
    if (pos_ >= text_.size()) {
      return report(ReasonCode::ParseError, escape_start, "unterminated escape sequence");
    }
    const char code = text_[pos_++];
    switch (code) {
      case '"':
        out.push_back('"');
        return Status::success();
      case '\\':
        out.push_back('\\');
        return Status::success();
      case '/':
        out.push_back('/');
        return Status::success();
      case 'b':
        out.push_back('\b');
        return Status::success();
      case 'f':
        out.push_back('\f');
        return Status::success();
      case 'n':
        out.push_back('\n');
        return Status::success();
      case 'r':
        out.push_back('\r');
        return Status::success();
      case 't':
        out.push_back('\t');
        return Status::success();
      case 'u':
        break;
      default:
        return report(ReasonCode::ParseError, pos_ - 1,
                        std::string("unsupported escape sequence '\\") + code + "'");
    }
    Result<std::uint32_t> code_point = unicode_escape();
    if (!code_point.ok()) {
      return code_point.status();
    }
    append_code_point(out, code_point.value());
    return Status::success();
  }

  // Reads the four hexadecimal digits of a \u escape and rejects lone
  // surrogates. pos_ points just past the 'u'.
  Result<std::uint32_t> unicode_escape() {
    Result<std::uint32_t> first = hex_quad();
    if (!first.ok()) {
      return first.status();
    }
    std::uint32_t code = first.value();
    if (code >= 0xD800U && code <= 0xDBFFU) {
      if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
        return report(ReasonCode::ParseError, pos_, "high surrogate escape is not followed by a low surrogate escape");
      }
      pos_ += 2;
      Result<std::uint32_t> second = hex_quad();
      if (!second.ok()) {
        return second.status();
      }
      const std::uint32_t low = second.value();
      if (low < 0xDC00U || low > 0xDFFFU) {
        return report(ReasonCode::ParseError, pos_ - 4, "high surrogate escape is not followed by a low surrogate escape");
      }
      code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
    } else if (code >= 0xDC00U && code <= 0xDFFFU) {
      return report(ReasonCode::ParseError, pos_ - 4, "lone low surrogate escape");
    }
    return code;
  }

  Result<std::uint32_t> hex_quad() {
    if (pos_ + 4 > text_.size()) {
      return report(ReasonCode::ParseError, pos_, "incomplete \\u escape");
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
      const char digit = text_[pos_ + index];
      if (!is_hex_digit(digit)) {
        return report(ReasonCode::ParseError, pos_, "invalid hexadecimal digit in \\u escape");
      }
      value = (value << 4U) | hex_digit_value(digit);
    }
    pos_ += 4;
    return value;
  }

  // Validates one complete UTF-8 sequence at pos_ and copies it verbatim. The
  // check is a full one: overlong forms, surrogate encodings and scalars above
  // U+10FFFF are refused, not passed through.
  bool append_utf8_sequence(std::string& out) {
    const std::size_t start = pos_;
    const auto byte_at = [this, start](std::size_t index) {
      return static_cast<unsigned int>(static_cast<unsigned char>(text_[start + index]));
    };
    const unsigned int lead = byte_at(0);
    std::size_t length = 0;
    if (lead >= 0xC2U && lead <= 0xDFU) {
      length = 2;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      length = 3;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      length = 4;
    } else {
      return false;
    }
    if (start + length > text_.size()) {
      return false;
    }
    for (std::size_t index = 1; index < length; ++index) {
      const unsigned int continuation = byte_at(index);
      if (continuation < 0x80U || continuation > 0xBFU) {
        return false;
      }
    }
    if (length == 3) {
      const unsigned int second = byte_at(1);
      if ((lead == 0xE0U && second < 0xA0U) || (lead == 0xEDU && second > 0x9FU)) {
        return false;
      }
    } else if (length == 4) {
      const unsigned int second = byte_at(1);
      if ((lead == 0xF0U && second < 0x90U) || (lead == 0xF4U && second > 0x8FU)) {
        return false;
      }
    }
    out.append(text_.substr(start, length));
    pos_ = start + length;
    return true;
  }

  Result<Value> number_literal() {
    const std::size_t start = pos_;
    const bool negative = text_[pos_] == '-';
    if (negative) {
      ++pos_;
    }
    if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
      return report(ReasonCode::ParseError, start, "a number requires at least one digit");
    }
    if (text_[pos_] == '0' && pos_ + 1 < text_.size() && is_digit(text_[pos_ + 1])) {
      return report(ReasonCode::ParseError, pos_, "leading zeros are not allowed in a number");
    }
    constexpr std::uint64_t kInt64MagnitudeLimit = 1ULL << 63U;
    const std::uint64_t limit = negative ? kInt64MagnitudeLimit : kInt64MagnitudeLimit - 1ULL;
    std::uint64_t magnitude = 0;
    while (pos_ < text_.size() && is_digit(text_[pos_])) {
      const std::uint64_t digit = static_cast<std::uint64_t>(text_[pos_] - '0');
      if (magnitude > (limit - digit) / 10ULL) {
        return report(ReasonCode::Overflow, start, "integer does not fit in a signed 64 bit value");
      }
      magnitude = magnitude * 10ULL + digit;
      ++pos_;
    }
    if (pos_ < text_.size()) {
      if (text_[pos_] == '.') {
        return report(ReasonCode::ParseError, pos_, "fractional numbers are not supported");
      }
      if (text_[pos_] == 'e' || text_[pos_] == 'E') {
        return report(ReasonCode::ParseError, pos_, "exponents are not supported");
      }
    }
    if (negative) {
      if (magnitude == kInt64MagnitudeLimit) {
        return Value((std::numeric_limits<std::int64_t>::min)());
      }
      return Value(-static_cast<std::int64_t>(magnitude));
    }
    return Value(static_cast<std::int64_t>(magnitude));
  }

  std::string_view text_;
  std::size_t pos_{0};
  // Bytes skipped before text_ begins (a byte order mark), so diagnostics report
  // offsets into the caller's original document.
  std::size_t base_offset_{0};
  std::size_t depth_limit_{0};
};

// --- writer ----------------------------------------------------------------

class Writer {
 public:
  Writer(bool canonical, int indent) noexcept
      : canonical_(canonical), indent_(indent), pretty_(indent >= 0) {}

  [[nodiscard]] std::string render(const Value& value) {
    write_value(value, 0);
    if (pretty_) {
      out_.push_back('\n');
    }
    return std::move(out_);
  }

 private:
  void write_break(std::size_t level) {
    out_.push_back('\n');
    out_.append(static_cast<std::size_t>(indent_) * level, ' ');
  }

  void write_value(const Value& value, std::size_t level) {
    switch (value.kind()) {
      case Kind::Null:
        out_.append("null");
        return;
      case Kind::Bool:
        out_.append(value.as_bool().value() ? "true" : "false");
        return;
      case Kind::Number:
        out_.append(std::to_string(value.as_int().value()));
        return;
      case Kind::String: {
        const Result<std::string> text = value.as_string();
        out_.push_back('"');
        append_escaped(out_, text.value());
        out_.push_back('"');
        return;
      }
      case Kind::Array: {
        const Array* items = value.as_array();
        if (items == nullptr || items->empty()) {
          out_.append("[]");
          return;
        }
        out_.push_back('[');
        bool first = true;
        for (const Value& item : *items) {
          if (!first) {
            out_.push_back(',');
          }
          first = false;
          if (pretty_) {
            write_break(level + 1);
          }
          write_value(item, level + 1);
        }
        if (pretty_) {
          write_break(level);
        }
        out_.push_back(']');
        return;
      }
      case Kind::Object: {
        const Object* members = value.as_object();
        if (members == nullptr || members->empty()) {
          out_.append("{}");
          return;
        }
        std::vector<const Object::value_type*> ordered;
        ordered.reserve(members->size());
        for (const Object::value_type& member : *members) {
          ordered.push_back(&member);
        }
        if (canonical_) {
          // Byte-wise key order. stable_sort keeps a deterministic order even
          // for a hand built object that repeated a key.
          std::stable_sort(ordered.begin(), ordered.end(),
                           [](const Object::value_type* left, const Object::value_type* right) {
                             return left->first < right->first;
                           });
        }
        out_.push_back('{');
        bool first = true;
        for (const Object::value_type* member : ordered) {
          if (!first) {
            out_.push_back(',');
          }
          first = false;
          if (pretty_) {
            write_break(level + 1);
          }
          out_.push_back('"');
          append_escaped(out_, member->first);
          out_.append(pretty_ ? "\": " : "\":");
          write_value(member->second, level + 1);
        }
        if (pretty_) {
          write_break(level);
        }
        out_.push_back('}');
        return;
      }
    }
  }

  bool canonical_{true};
  int indent_{-1};
  bool pretty_{false};
  std::string out_;
};

}  // namespace

// --- value -----------------------------------------------------------------

std::string_view kind_text(Kind kind) noexcept {
  switch (kind) {
    case Kind::Null:
      return "null";
    case Kind::Bool:
      return "bool";
    case Kind::Number:
      return "number";
    case Kind::String:
      return "string";
    case Kind::Array:
      return "array";
    case Kind::Object:
      return "object";
  }
  return "unknown";
}

Value::Value(bool boolean) noexcept : kind_(Kind::Bool), boolean_(boolean) {}

Value::Value(std::int64_t number) noexcept : kind_(Kind::Number), number_(number) {}

Value::Value(std::string text) : kind_(Kind::String), text_(std::move(text)) {}

Value::Value(const char* text) : kind_(Kind::String), text_(text != nullptr ? text : "") {}

Value Value::array(Array items) {
  Value value;
  value.kind_ = Kind::Array;
  value.items_ = std::move(items);
  return value;
}

Value Value::object(Object members) {
  Value value;
  value.kind_ = Kind::Object;
  value.members_ = std::move(members);
  return value;
}

Result<Value> Value::parse(std::string_view text, std::size_t max_depth) {
  Parser parser(text, max_depth);
  return parser.document();
}

Result<bool> Value::as_bool() const {
  if (kind_ != Kind::Bool) {
    return type_error("bool", kind_);
  }
  return boolean_;
}

Result<std::int64_t> Value::as_int() const {
  if (kind_ != Kind::Number) {
    return type_error("number", kind_);
  }
  return number_;
}

Result<std::string> Value::as_string() const {
  if (kind_ != Kind::String) {
    return type_error("string", kind_);
  }
  return text_;
}

const Array* Value::as_array() const noexcept { return kind_ == Kind::Array ? &items_ : nullptr; }

const Object* Value::as_object() const noexcept { return kind_ == Kind::Object ? &members_ : nullptr; }

const Value* Value::find(std::string_view key) const noexcept {
  if (kind_ != Kind::Object) {
    return nullptr;
  }
  for (const Object::value_type& member : members_) {
    if (std::string_view(member.first) == key) {
      return &member.second;
    }
  }
  return nullptr;
}

Result<const Value*> Value::require(std::string_view key) const {
  if (kind_ != Kind::Object) {
    return type_error("object", kind_);
  }
  const Value* member = find(key);
  if (member == nullptr) {
    return missing_field(key);
  }
  return member;
}

Result<std::string> Value::require_string(std::string_view key) const {
  if (kind_ != Kind::Object) {
    return type_error("object", kind_);
  }
  const Value* member = find(key);
  if (member == nullptr) {
    return missing_field(key);
  }
  return member->as_string();
}

Result<std::int64_t> Value::require_int(std::string_view key) const {
  if (kind_ != Kind::Object) {
    return type_error("object", kind_);
  }
  const Value* member = find(key);
  if (member == nullptr) {
    return missing_field(key);
  }
  return member->as_int();
}

Result<bool> Value::require_bool(std::string_view key) const {
  if (kind_ != Kind::Object) {
    return type_error("object", kind_);
  }
  const Value* member = find(key);
  if (member == nullptr) {
    return missing_field(key);
  }
  return member->as_bool();
}

Result<const Array*> Value::require_array(std::string_view key) const {
  if (kind_ != Kind::Object) {
    return type_error("object", kind_);
  }
  const Value* member = find(key);
  if (member == nullptr) {
    return missing_field(key);
  }
  if (member->kind() != Kind::Array) {
    return type_error("array", member->kind());
  }
  return member->as_array();
}

Result<std::optional<std::string>> Value::optional_string(std::string_view key) const {
  if (kind_ != Kind::Object) {
    return type_error("object", kind_);
  }
  const Value* member = find(key);
  if (member == nullptr) {
    return std::optional<std::string>{};
  }
  if (member->kind() != Kind::String) {
    return type_error("string", member->kind());
  }
  return std::optional<std::string>(member->text_);
}

// Setting a member replaces an existing key in place, so a Value never carries
// two members with the same key.
void Value::set(std::string key, Value value) {
  if (kind_ != Kind::Object) {
    kind_ = Kind::Object;
    boolean_ = false;
    number_ = 0;
    text_.clear();
    items_.clear();
    members_.clear();
  }
  for (Object::value_type& member : members_) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  members_.emplace_back(std::move(key), std::move(value));
}

void Value::push(Value value) {
  if (kind_ != Kind::Array) {
    kind_ = Kind::Array;
    boolean_ = false;
    number_ = 0;
    text_.clear();
    items_.clear();
    members_.clear();
  }
  items_.push_back(std::move(value));
}

std::size_t Value::size() const noexcept {
  switch (kind_) {
    case Kind::String:
      return text_.size();
    case Kind::Array:
      return items_.size();
    case Kind::Object:
      return members_.size();
    default:
      return 0;
  }
}

// Objects compare as a set of members: member order is presentational, and the
// canonical writer is free to reorder, so equality must not depend on it.
bool operator==(const Value& a, const Value& b) noexcept {
  if (a.kind_ != b.kind_) {
    return false;
  }
  switch (a.kind_) {
    case Kind::Null:
      return true;
    case Kind::Bool:
      return a.boolean_ == b.boolean_;
    case Kind::Number:
      return a.number_ == b.number_;
    case Kind::String:
      return a.text_ == b.text_;
    case Kind::Array:
      return a.items_ == b.items_;
    case Kind::Object: {
      if (a.members_.size() != b.members_.size()) {
        return false;
      }
      for (const Object::value_type& member : a.members_) {
        bool matched = false;
        for (const Object::value_type& other : b.members_) {
          if (member.first == other.first) {
            if (!(member.second == other.second)) {
              return false;
            }
            matched = true;
            break;
          }
        }
        if (!matched) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

std::string dump(const Value& value, bool canonical, int indent) {
  Writer writer(canonical, indent);
  return writer.render(value);
}

}  // namespace co::json
