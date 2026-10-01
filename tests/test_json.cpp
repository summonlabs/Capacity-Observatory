// JSON: strict parsing, deterministic writing, round trips, adversarial input.
#include "co_test.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "capacity_observatory/json.hpp"
#include "capacity_observatory/reason.hpp"

using namespace co;

namespace {

using co::json::Kind;
using co::json::Value;

constexpr std::int64_t kMaxInt = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kMinInt = (std::numeric_limits<std::int64_t>::min)();

// Parses 'document' or fails the case, quoting the parser's own explanation.
Value parse_ok(std::string_view document, std::size_t max_depth = 64) {
  auto parsed = Value::parse(document, max_depth);
  if (!parsed.ok()) {
    CO_FAIL(std::string("expected ") + std::string(document) + " to parse: " + parsed.status().render());
  }
  return std::move(parsed).value();
}

std::string compact(const Value& value) { return co::json::dump(value, true, -1); }

std::string written_order(const Value& value) { return co::json::dump(value, false, -1); }

// Value(bool) and Value(std::int64_t) are both viable for an int literal, so
// the tests always name the type instead of relying on overload resolution.
Value make_number(std::int64_t value) { return Value(value); }

Value make_string(std::string value) { return Value(std::move(value)); }

}  // namespace

CO_TEST(json_parses_scalars) {
  CO_REQUIRE(parse_ok("null").is_null());
  CO_REQUIRE(parse_ok("true").is_bool());
  CO_REQUIRE(parse_ok("false").is_bool());
  CO_REQUIRE(parse_ok("0").is_number());
  CO_REQUIRE(parse_ok("\"\"").is_string());
  CO_REQUIRE(parse_ok("[]").is_array());
  CO_REQUIRE(parse_ok("{}").is_object());
  CO_REQUIRE(parse_ok("  \r\n\t 42 \t\n").is_number());

  CO_REQUIRE_OK(parse_ok("true").as_bool(), true_value);
  CO_REQUIRE(true_value);
  CO_REQUIRE_OK(parse_ok("false").as_bool(), false_value);
  CO_REQUIRE(!false_value);
  CO_REQUIRE_OK(parse_ok("42").as_int(), forty_two);
  CO_REQUIRE_EQ(forty_two, std::int64_t{42});
  CO_REQUIRE_OK(parse_ok("\"capacity\"").as_string(), word);
  CO_REQUIRE_EQ(word, std::string("capacity"));

  CO_REQUIRE_EQ(compact(parse_ok("null")), std::string("null"));
  CO_REQUIRE_EQ(compact(parse_ok("true")), std::string("true"));
  CO_REQUIRE_EQ(compact(parse_ok("false")), std::string("false"));
  CO_REQUIRE_EQ(compact(parse_ok("-17")), std::string("-17"));
  CO_REQUIRE_EQ(compact(parse_ok("\"capacity\"")), std::string("\"capacity\""));
  CO_REQUIRE_EQ(compact(parse_ok("[]")), std::string("[]"));
  CO_REQUIRE_EQ(compact(parse_ok("{}")), std::string("{}"));

  // The constructors and the parser agree on what a value is.
  CO_REQUIRE(Value() == parse_ok("null"));
  CO_REQUIRE(Value(true) == parse_ok("true"));
  CO_REQUIRE(make_number(-3) == parse_ok("-3"));
  CO_REQUIRE(make_string("x") == parse_ok("\"x\""));
  CO_REQUIRE(Value("literal") == parse_ok("\"literal\""));
  CO_REQUIRE(Value::array() == parse_ok("[]"));
  CO_REQUIRE(Value::object() == parse_ok("{}"));
}

CO_TEST(json_number_syntax_is_strict) {
  const char* const malformed[] = {"01",   "-01", "00",   "007",  "+1",  "-+1", "--1", "-",   "1.5", "-0.5",
                                   "0.0",  ".5",  "1.",   "1e5",  "1E5", "0e0", "1e",  "0.",  "-0.", "[01]",
                                   "[+1]", "[1.5]", "{\"a\":1e5}", "[0e0]", "{\"a\":.5}"};
  for (const char* document : malformed) {
    CO_REQUIRE_ERR(Value::parse(document), ReasonCode::ParseError);
  }

  // Out of range integers are refused as an overflow, never truncated.
  const char* const too_large[] = {"9223372036854775808",
                                   "-9223372036854775809",
                                   "99999999999999999999",
                                   "18446744073709551616",
                                   "-99999999999999999999999999",
                                   "[9223372036854775808]",
                                   "{\"a\":-9223372036854775809}"};
  for (const char* document : too_large) {
    CO_REQUIRE_ERR(Value::parse(document), ReasonCode::Overflow);
  }

  // Every refusal explains itself and points at a byte.
  const auto refusal = Value::parse("01");
  CO_REQUIRE(!refusal.ok());
  CO_REQUIRE(refusal.status().detail().find("byte") != std::string::npos);
  CO_REQUIRE(Value::parse("9223372036854775808").status().detail().find("64 bit") != std::string::npos);
}

CO_TEST(json_integer_boundaries) {
  CO_REQUIRE_OK(parse_ok("9223372036854775807").as_int(), parsed_max);
  CO_REQUIRE_EQ(parsed_max, kMaxInt);
  CO_REQUIRE_OK(parse_ok("-9223372036854775808").as_int(), parsed_min);
  CO_REQUIRE_EQ(parsed_min, kMinInt);

  CO_REQUIRE_EQ(compact(make_number(kMaxInt)), std::string("9223372036854775807"));
  CO_REQUIRE_EQ(compact(make_number(kMinInt)), std::string("-9223372036854775808"));

  CO_REQUIRE_ERR(Value::parse("9223372036854775808"), ReasonCode::Overflow);
  CO_REQUIRE_ERR(Value::parse("-9223372036854775809"), ReasonCode::Overflow);
  CO_REQUIRE_ERR(Value::parse("92233720368547758080"), ReasonCode::Overflow);

  // A signed zero is still zero and prints without a sign.
  CO_REQUIRE_OK(parse_ok("-0").as_int(), negative_zero);
  CO_REQUIRE_EQ(negative_zero, std::int64_t{0});
  CO_REQUIRE_EQ(compact(parse_ok("-0")), std::string("0"));

  // Extremes survive a canonical round trip byte for byte.
  CO_REQUIRE_EQ(compact(parse_ok(compact(make_number(kMinInt)))), std::string("-9223372036854775808"));
  CO_REQUIRE_EQ(compact(parse_ok(compact(make_number(kMaxInt)))), std::string("9223372036854775807"));
}

CO_TEST(json_string_escapes) {
  CO_REQUIRE_OK(parse_ok("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"").as_string(), escaped);
  CO_REQUIRE_EQ(escaped, std::string("a\"b\\c/d\b\f\n\r\t"));
  CO_REQUIRE_EQ(compact(parse_ok("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"")), std::string("\"a\\\"b\\\\c/d\\b\\f\\n\\r\\t\""));

  CO_REQUIRE_OK(parse_ok("\"\"").as_string(), empty_text);
  CO_REQUIRE(empty_text.empty());

  // \u escapes are encoded to UTF-8: two and three byte forms.
  CO_REQUIRE_OK(parse_ok("\"\\u0041\\u00e9\\u20ac\"").as_string(), unicode_text);
  CO_REQUIRE_EQ(unicode_text, std::string("A\xC3\xA9\xE2\x82\xAC"));
  CO_REQUIRE_EQ(compact(parse_ok("\"\\u0041\\u00e9\\u20ac\"")), std::string("\"A\xC3\xA9\xE2\x82\xAC\""));

  // An escaped NUL is a real byte in the string and is escaped again on output.
  CO_REQUIRE_OK(parse_ok("\"\\u0000\"").as_string(), nul_text);
  CO_REQUIRE_EQ(nul_text.size(), std::size_t{1});
  CO_REQUIRE_EQ(static_cast<int>(nul_text[0]), 0);
  CO_REQUIRE_EQ(compact(parse_ok("\"\\u0000\"")), std::string("\"\\u0000\""));

  // Keys take the same escapes and denote the same key. The receiver owns the
  // member the pointer refers to, so it has to outlive the pointer.
  const Value keyed = parse_ok("{\"\\u0041\":1}");
  CO_REQUIRE_OK(keyed.require("A"), member_a);
  CO_REQUIRE(member_a != nullptr);
  CO_REQUIRE_OK(member_a->as_int(), member_number);
  CO_REQUIRE_EQ(member_number, std::int64_t{1});
}

CO_TEST(json_surrogate_pairs) {
  // U+1F600 GRINNING FACE, four UTF-8 bytes.
  CO_REQUIRE_OK(parse_ok("\"\\ud83d\\ude00\"").as_string(), grinning);
  CO_REQUIRE_EQ(grinning, std::string("\xF0\x9F\x98\x80"));
  CO_REQUIRE_EQ(compact(parse_ok("\"\\ud83d\\ude00\"")), std::string("\"\xF0\x9F\x98\x80\""));
  CO_REQUIRE_EQ(grinning.size(), std::size_t{4});

  // The extreme scalar values reachable through a surrogate pair.
  CO_REQUIRE_OK(parse_ok("\"\\ud800\\udc00\"").as_string(), lowest_pair);
  CO_REQUIRE_EQ(lowest_pair, std::string("\xF0\x90\x80\x80"));
  CO_REQUIRE_OK(parse_ok("\"\\udbff\\udfff\"").as_string(), highest_pair);
  CO_REQUIRE_EQ(highest_pair, std::string("\xF4\x8F\xBF\xBF"));

  // A pair round trips through canonical output and back.
  CO_REQUIRE(parse_ok(compact(parse_ok("\"\\ud83d\\ude00\""))) == parse_ok("\"\\ud83d\\ude00\""));

  // Lone and mismatched surrogates are refusals, not replacement characters.
  CO_REQUIRE_ERR(Value::parse("\"\\ud800\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\udc00\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\ud800x\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\ud800\\u0041\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\ud83d\\ud83d\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\udc00\\ud800\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\ud800\\u\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\ud800\\uzzzz\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("[\"\\ud800\"]"), ReasonCode::ParseError);
  CO_REQUIRE(Value::parse("\"\\ud800\"").status().detail().find("surrogate") != std::string::npos);
}

CO_TEST(json_rejects_bad_escapes_and_control_bytes) {
  CO_REQUIRE_ERR(Value::parse("\"\\x41\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\'\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\q\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\u12\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\uzzzz\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"\\u12g4\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"unterminated"), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"trailing backslash\\"), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"raw\nnewline\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"tab\there\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("\"carriage\rreturn\""), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse(std::string_view("\"nul\0byte\"", 10)), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("[\"raw\x1f\"]"), ReasonCode::ParseError);

  // A control byte is reported, not silently accepted.
  CO_REQUIRE(Value::parse("\"raw\nnewline\"").status().detail().find("control") != std::string::npos);
  CO_REQUIRE(Value::parse("\"unterminated").status().detail().find("unterminated") != std::string::npos);
}

CO_TEST(json_rejects_invalid_utf8) {
  const std::string quote(1, '"');
  const auto refuse = [&quote](std::string_view bytes) {
    const std::string document = quote + std::string(bytes) + quote;
    CO_REQUIRE_ERR(Value::parse(document), ReasonCode::ParseError);
  };
  refuse("\xC0\x80");          // overlong encoding of U+0000
  refuse("\xC1\xBF");          // overlong
  refuse("\xE0\x80\x80");      // overlong three byte form
  refuse("\xED\xA0\x80");      // UTF-8 encoded surrogate D800
  refuse("\xED\xBF\xBF");      // UTF-8 encoded surrogate DFFF
  refuse("\xF0\x80\x80\x80");  // overlong four byte form
  refuse("\xF0\x8F\xBF\xBF");  // overlong, above U+10FFFF when decoded
  refuse("\xF4\x90\x80\x80");  // above U+10FFFF
  refuse("\xF5\x80\x80\x80");  // invalid lead byte
  refuse("\xFF\xFE");          // invalid lead bytes
  refuse("\x80");              // lone continuation byte
  refuse("\xBF");              // lone continuation byte
  refuse("\xC3");              // truncated two byte sequence
  refuse("\xE2\x82");          // truncated three byte sequence
  refuse("\xF0\x9F\x98");      // truncated four byte sequence

  // Well formed UTF-8 passes through byte for byte.
  const std::string well_formed = quote + "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" + quote;
  CO_REQUIRE_OK(parse_ok(well_formed).as_string(), decoded);
  CO_REQUIRE_EQ(decoded, std::string("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"));
  CO_REQUIRE_EQ(compact(parse_ok(well_formed)), well_formed);

  // Invalid bytes are refused outside a string too.
  CO_REQUIRE_ERR(Value::parse("\xC3\xA4"), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse("[\xC3\xA4]"), ReasonCode::ParseError);
}

CO_TEST(json_objects_and_duplicate_keys) {
  const Value document = parse_ok("{\"a\":1,\"b\":[true,null],\"c\":{\"d\":\"x\"}}");
  CO_REQUIRE(document.is_object());
  CO_REQUIRE_EQ(document.size(), std::size_t{3});
  CO_REQUIRE(document.find("a") != nullptr);
  CO_REQUIRE(document.find("missing") == nullptr);
  CO_REQUIRE_OK(document.require("a"), member_a);
  CO_REQUIRE(member_a != nullptr);
  CO_REQUIRE_OK(member_a->as_int(), member_number);
  CO_REQUIRE_EQ(member_number, std::int64_t{1});
  CO_REQUIRE_EQ(compact(document), std::string("{\"a\":1,\"b\":[true,null],\"c\":{\"d\":\"x\"}}"));

  CO_REQUIRE_ERR(Value::parse("{\"a\":1,\"a\":2}"), ReasonCode::DuplicateKey);
  CO_REQUIRE_ERR(Value::parse("{\"a\":1,\"b\":2,\"a\":3}"), ReasonCode::DuplicateKey);
  CO_REQUIRE_ERR(Value::parse("{\"a\":1,\"a\":\"1\"}"), ReasonCode::DuplicateKey);
  CO_REQUIRE_ERR(Value::parse("{\"a\":{\"b\":1,\"b\":2}}"), ReasonCode::DuplicateKey);
  CO_REQUIRE_ERR(Value::parse("[{\"a\":1,\"a\":1}]"), ReasonCode::DuplicateKey);
  CO_REQUIRE_ERR(Value::parse("{\"\":1,\"\":2}"), ReasonCode::DuplicateKey);
  CO_REQUIRE_ERR(Value::parse("{\"\\u0041\":1,\"A\":2}"), ReasonCode::DuplicateKey);
  CO_REQUIRE(Value::parse("{\"a\":1,\"a\":2}").status().detail().find("duplicate") != std::string::npos);

  // Keys that differ only in case or in escape spelling are distinct.
  CO_REQUIRE_EQ(parse_ok("{\"a\":1,\"A\":2}").size(), std::size_t{2});
  CO_REQUIRE_EQ(parse_ok("{\"\\u0041\":1,\"a\":2}").size(), std::size_t{2});
}

CO_TEST(json_array_and_object_structure) {
  const char* const malformed[] = {"{",   "}",       "[",        "]",         "{]",       "[}",     "{\"a\"}",
                                   "{\"a\":}", "{:1}", "{\"a\":1,}", "{,}", "{\"a\":1 \"b\":2}",
                                   "{\"a\" 1}", "[1,]", "[,1]", "[1 2]", "[1;2]", "{1:2}",
                                   "{\"a\":1]}", "[[", "{\"a\":{\"b\":1}", "[[]", "{\"a\":1,,\"b\":2}",
                                   "{\"a\"\"b\":1}"};
  for (const char* document : malformed) {
    CO_REQUIRE_ERR(Value::parse(document), ReasonCode::ParseError);
  }

  // The well formed shapes around them still parse.
  CO_REQUIRE_EQ(parse_ok("[1,2,3]").size(), std::size_t{3});
  CO_REQUIRE_EQ(parse_ok("[[],{},[[]]]").size(), std::size_t{3});
  CO_REQUIRE_EQ(parse_ok("{\"a\":{},\"b\":[]}").size(), std::size_t{2});
  CO_REQUIRE_EQ(compact(parse_ok("[[1],[2,3]]")), std::string("[[1],[2,3]]"));
  CO_REQUIRE_EQ(compact(parse_ok("{\"a\":{\"b\":{\"c\":[]}}}")), std::string("{\"a\":{\"b\":{\"c\":[]}}}"));
}

CO_TEST(json_empty_and_whitespace_input) {
  CO_REQUIRE_ERR(Value::parse(""), ReasonCode::EmptyInput);
  CO_REQUIRE_ERR(Value::parse(" "), ReasonCode::EmptyInput);
  CO_REQUIRE_ERR(Value::parse("\t\r\n  "), ReasonCode::EmptyInput);
  const auto empty_result = Value::parse("");
  CO_REQUIRE_EQ(empty_result.status().detail(), std::string("input is empty"));
  const auto blank_result = Value::parse("   ");
  CO_REQUIRE(!blank_result.status().detail().empty());

  // Whitespace around a value is not input.
  CO_REQUIRE_OK(parse_ok("  \n\t 0 \r\n ").as_int(), zero);
  CO_REQUIRE_EQ(zero, std::int64_t{0});
  CO_REQUIRE_EQ(compact(parse_ok("\n[]\t")), std::string("[]"));
}

CO_TEST(json_rejects_trailing_garbage) {
  CO_REQUIRE_ERR(Value::parse("1 2"), ReasonCode::TrailingGarbage);
  CO_REQUIRE_ERR(Value::parse("null null"), ReasonCode::TrailingGarbage);
  CO_REQUIRE_ERR(Value::parse("true,false"), ReasonCode::TrailingGarbage);
  CO_REQUIRE_ERR(Value::parse("[] []"), ReasonCode::TrailingGarbage);
  CO_REQUIRE_ERR(Value::parse("{} {}"), ReasonCode::TrailingGarbage);
  CO_REQUIRE_ERR(Value::parse("123abc"), ReasonCode::TrailingGarbage);
  CO_REQUIRE_ERR(Value::parse("0x10"), ReasonCode::TrailingGarbage);
  CO_REQUIRE_ERR(Value::parse("{\"a\":1}x"), ReasonCode::TrailingGarbage);
  CO_REQUIRE_ERR(Value::parse("\"a\" \"b\""), ReasonCode::TrailingGarbage);
  const auto trailing = Value::parse("1 2");
  CO_REQUIRE_EQ(trailing.status().detail(), std::string("unexpected byte after the top-level value at byte 2"));
  CO_REQUIRE(Value::parse("  [1]  ").ok());
}

CO_TEST(json_depth_is_bounded) {
  const auto nested_arrays = [](std::size_t depth) {
    std::string document(depth, '[');
    document.push_back('1');
    document.append(depth, ']');
    return document;
  };

  CO_REQUIRE(Value::parse(nested_arrays(8), 8).ok());
  CO_REQUIRE_ERR(Value::parse(nested_arrays(8), 7), ReasonCode::ParseError);
  CO_REQUIRE(Value::parse(nested_arrays(64)).ok());
  CO_REQUIRE_ERR(Value::parse(nested_arrays(65)), ReasonCode::ParseError);
  // The caller's max_depth can lower the ceiling but never raise it: documents
  // nested past the compile time ceiling are refused however permissive the
  // caller asks to be. This is what keeps recursion bounded by a constant rather
  // than by the input, and it is why a 200 level document is a rejection rather
  // than an accepted value that would later recurse on destruction.
  CO_REQUIRE_ERR(Value::parse(nested_arrays(200), 200), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse(nested_arrays(65), 1000000), ReasonCode::ParseError);
  CO_REQUIRE(Value::parse(nested_arrays(64), 1000000).ok());

  // Objects consume depth exactly like arrays.
  const std::string objects = "{\"a\":{\"b\":{\"c\":1}}}";
  CO_REQUIRE(Value::parse(objects, 3).ok());
  CO_REQUIRE_ERR(Value::parse(objects, 2), ReasonCode::ParseError);

  // Scalars are not nesting.
  CO_REQUIRE(Value::parse("1", 0).ok());
  CO_REQUIRE(Value::parse("\"text\"", 0).ok());
  CO_REQUIRE_ERR(Value::parse("[]", 0), ReasonCode::ParseError);

  // Neither max_depth nor the input length may drive unbounded recursion.
  const std::string bomb(100000, '[');
  CO_REQUIRE_ERR(Value::parse(bomb), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse(bomb, 1000000), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse(std::string(50000, '{'), 1000000), ReasonCode::ParseError);
  CO_REQUIRE(Value::parse(bomb, 1000000).status().detail().find("nesting") != std::string::npos);
}

CO_TEST(json_ignores_a_leading_byte_order_mark) {
  // RFC 8259 section 8.1: a producer must not add a byte order mark, but an
  // implementation may ignore one. Windows tooling emits it routinely, so a
  // document written by PowerShell must not look malformed to this parser.
  const std::string mark("\xEF\xBB\xBF");
  const auto parsed = Value::parse(mark + "{\"a\":1}");
  CO_REQUIRE(parsed.ok());
  CO_REQUIRE_EQ(parsed.value().require_int("a").value(), 1LL);

  // Diagnostics still report offsets into the caller's original document.
  const auto trailing = Value::parse(mark + "1 2");
  CO_REQUIRE_ERR(trailing, ReasonCode::TrailingGarbage);
  CO_REQUIRE_EQ(trailing.status().detail(), std::string("unexpected byte after the top-level value at byte 5"));

  // A mark with nothing after it is still empty input.
  CO_REQUIRE_ERR(Value::parse(mark), ReasonCode::EmptyInput);

  // Nested documents after a mark behave exactly as without one.
  const auto nested = Value::parse(mark + "[1,[2,{\"b\":3}]]");
  CO_REQUIRE(nested.ok());
  CO_REQUIRE_EQ(nested.value().as_array()->size(), static_cast<std::size_t>(2));

  // A mark in the middle of a document is not a mark.
  CO_REQUIRE_ERR(Value::parse("[" + mark + "]"), ReasonCode::ParseError);
}

CO_TEST(json_accessors_report_type_mismatch) {
  CO_REQUIRE_ERR(make_number(1).as_bool(), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(Value(true).as_int(), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(make_number(1).as_string(), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(Value().as_int(), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(Value().as_bool(), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(Value().as_string(), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(Value::array().as_int(), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(Value::object().as_string(), ReasonCode::TypeMismatch);

  CO_REQUIRE(Value().as_array() == nullptr);
  CO_REQUIRE(Value().as_object() == nullptr);
  CO_REQUIRE(make_number(1).as_array() == nullptr);
  CO_REQUIRE(make_number(1).find("a") == nullptr);
  CO_REQUIRE(Value::array({make_number(1)}).as_array() != nullptr);
  CO_REQUIRE(Value::object().as_object() != nullptr);
  CO_REQUIRE_EQ(Value::array({make_number(1), make_number(2)}).size(), std::size_t{2});
  CO_REQUIRE_EQ(make_number(1).size(), std::size_t{0});
  CO_REQUIRE_EQ(Value().size(), std::size_t{0});
  CO_REQUIRE_EQ(Value(true).size(), std::size_t{0});
  CO_REQUIRE_EQ(make_string("abc").size(), std::size_t{3});

  // The refusal names both shapes.
  const auto mismatch = make_number(1).as_bool();
  CO_REQUIRE(mismatch.status().detail().find("bool") != std::string::npos);
  CO_REQUIRE(mismatch.status().detail().find("number") != std::string::npos);
}

CO_TEST(json_require_and_optional_members) {
  const Value document = parse_ok("{\"s\":\"text\",\"i\":7,\"b\":false,\"a\":[1,2],\"n\":null}");

  CO_REQUIRE_OK(document.require_string("s"), s_text);
  CO_REQUIRE_EQ(s_text, std::string("text"));
  CO_REQUIRE_OK(document.require_int("i"), i_number);
  CO_REQUIRE_EQ(i_number, std::int64_t{7});
  CO_REQUIRE_OK(document.require_bool("b"), b_flag);
  CO_REQUIRE_EQ(b_flag, false);
  CO_REQUIRE_OK(document.require_array("a"), a_items);
  CO_REQUIRE(a_items != nullptr);
  CO_REQUIRE_EQ(a_items->size(), std::size_t{2});
  CO_REQUIRE_OK((*a_items)[1].as_int(), second_item);
  CO_REQUIRE_EQ(second_item, std::int64_t{2});
  CO_REQUIRE_OK(document.require("n"), n_member);
  CO_REQUIRE(n_member != nullptr);
  CO_REQUIRE(n_member->is_null());

  CO_REQUIRE_ERR(document.require("missing"), ReasonCode::MissingField);
  CO_REQUIRE_ERR(document.require_string("missing"), ReasonCode::MissingField);
  CO_REQUIRE_ERR(document.require_int("missing"), ReasonCode::MissingField);
  CO_REQUIRE_ERR(document.require_bool("missing"), ReasonCode::MissingField);
  CO_REQUIRE_ERR(document.require_array("missing"), ReasonCode::MissingField);

  CO_REQUIRE_ERR(document.require_string("i"), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(document.require_int("s"), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(document.require_bool("i"), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(document.require_array("s"), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(document.require_string("n"), ReasonCode::TypeMismatch);

  // A key that is present but null is not absent, and a non-object receiver is
  // a shape mismatch rather than a missing member.
  CO_REQUIRE_ERR(make_number(1).require("a"), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(make_number(1).require_int("a"), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(Value::array().require_string("a"), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(Value().require("a"), ReasonCode::TypeMismatch);
  CO_REQUIRE(document.require("missing").status().detail().find("missing") != std::string::npos);

  CO_REQUIRE_OK(document.optional_string("s"), optional_s);
  CO_REQUIRE(optional_s.has_value());
  CO_REQUIRE_EQ(optional_s.value(), std::string("text"));
  CO_REQUIRE_OK(document.optional_string("absent"), optional_absent);
  CO_REQUIRE(!optional_absent.has_value());
  CO_REQUIRE_ERR(document.optional_string("i"), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(document.optional_string("n"), ReasonCode::TypeMismatch);
  CO_REQUIRE_ERR(make_number(1).optional_string("s"), ReasonCode::TypeMismatch);
}

CO_TEST(json_set_and_push_builders) {
  Value built;
  built.set("b", make_number(2));
  built.set("a", make_number(1));
  CO_REQUIRE(built.is_object());
  CO_REQUIRE_EQ(built.size(), std::size_t{2});
  CO_REQUIRE_EQ(compact(built), std::string("{\"a\":1,\"b\":2}"));
  CO_REQUIRE_EQ(written_order(built), std::string("{\"b\":2,\"a\":1}"));

  // Setting an existing key replaces it in place instead of repeating it.
  built.set("b", make_number(3));
  CO_REQUIRE_EQ(built.size(), std::size_t{2});
  CO_REQUIRE_EQ(compact(built), std::string("{\"a\":1,\"b\":3}"));
  CO_REQUIRE_EQ(written_order(built), std::string("{\"b\":3,\"a\":1}"));

  // Nested building round trips.
  Value nested;
  nested.set("outer", make_string("value"));
  Value list;
  list.push(make_number(1));
  list.push(make_string("two"));
  list.push(Value(true));
  nested.set("list", list);
  CO_REQUIRE_EQ(compact(nested), std::string("{\"list\":[1,\"two\",true],\"outer\":\"value\"}"));

  // push() promotes a non array, set() promotes a non object.
  Value promoted;
  promoted.push(make_number(4));
  CO_REQUIRE(promoted.is_array());
  CO_REQUIRE_EQ(compact(promoted), std::string("[4]"));
  promoted.set("k", make_number(5));
  CO_REQUIRE(promoted.is_object());
  CO_REQUIRE_EQ(compact(promoted), std::string("{\"k\":5}"));
}

CO_TEST(json_kind_text_is_total) {
  CO_REQUIRE_EQ(std::string(co::json::kind_text(Kind::Null)), std::string("null"));
  CO_REQUIRE_EQ(std::string(co::json::kind_text(Kind::Bool)), std::string("bool"));
  CO_REQUIRE_EQ(std::string(co::json::kind_text(Kind::Number)), std::string("number"));
  CO_REQUIRE_EQ(std::string(co::json::kind_text(Kind::String)), std::string("string"));
  CO_REQUIRE_EQ(std::string(co::json::kind_text(Kind::Array)), std::string("array"));
  CO_REQUIRE_EQ(std::string(co::json::kind_text(Kind::Object)), std::string("object"));
  CO_REQUIRE_EQ(std::string(co::json::kind_text(static_cast<Kind>(200))), std::string("unknown"));
  CO_REQUIRE_EQ(Value::object({{"a", make_number(1)}}).size(), std::size_t{1});
}

CO_TEST(json_dump_is_canonical) {
  const Value document = parse_ok("{\"b\":[1,2,{\"y\":true,\"x\":null}],\"a\":\"text\"}");
  CO_REQUIRE_EQ(compact(document), std::string("{\"a\":\"text\",\"b\":[1,2,{\"x\":null,\"y\":true}]}"));

  // Canonical order is applied at every level.
  CO_REQUIRE_EQ(compact(parse_ok("{\"z\":{\"b\":1,\"a\":2},\"y\":3}")), std::string("{\"y\":3,\"z\":{\"a\":2,\"b\":1}}"));

  // Byte-wise order, not case folding and not signed char order.
  CO_REQUIRE_EQ(compact(parse_ok("{\"b\":1,\"A\":2,\"a\":3,\"_\":4,\"0\":5}")),
                std::string("{\"0\":5,\"A\":2,\"_\":4,\"a\":3,\"b\":1}"));
  CO_REQUIRE_EQ(compact(parse_ok("{\"\xC3\xA9\":1,\"z\":2}")), std::string("{\"z\":2,\"\xC3\xA9\":1}"));

  // Canonical output is a fixed point of dump(parse(.)).
  const std::string once = compact(document);
  CO_REQUIRE_EQ(compact(parse_ok(once)), once);
  CO_REQUIRE_EQ(compact(document), once);
}

CO_TEST(json_dump_preserves_insertion_order_when_not_canonical) {
  const Value document = parse_ok("{\"b\":1,\"a\":2,\"c\":[{\"z\":0,\"y\":1}]}");
  CO_REQUIRE_EQ(written_order(document), std::string("{\"b\":1,\"a\":2,\"c\":[{\"z\":0,\"y\":1}]}"));
  CO_REQUIRE_EQ(compact(document), std::string("{\"a\":2,\"b\":1,\"c\":[{\"y\":1,\"z\":0}]}"));

  // Both spellings denote the same value.
  CO_REQUIRE(parse_ok(written_order(document)) == document);
  CO_REQUIRE(parse_ok(compact(document)) == document);
}

CO_TEST(json_dump_pretty_printing) {
  const Value document = parse_ok("{\"b\":[1,2],\"a\":{}}");
  CO_REQUIRE_EQ(co::json::dump(document, true, 2), std::string("{\n  \"a\": {},\n  \"b\": [\n    1,\n    2\n  ]\n}\n"));
  CO_REQUIRE_EQ(co::json::dump(document, false, 0), std::string("{\n\"b\": [\n1,\n2\n],\n\"a\": {}\n}\n"));
  CO_REQUIRE_EQ(co::json::dump(document, true, 4),
                std::string("{\n    \"a\": {},\n    \"b\": [\n        1,\n        2\n    ]\n}\n"));

  // Nesting indents relative to its own level.
  CO_REQUIRE_EQ(co::json::dump(parse_ok("{\"a\":{\"b\":[1]}}"), true, 2),
                std::string("{\n  \"a\": {\n    \"b\": [\n      1\n    ]\n  }\n}\n"));
  CO_REQUIRE_EQ(co::json::dump(Value::array({Value::array(), Value::object()}), true, 2),
                std::string("[\n  [],\n  {}\n]\n"));

  // Scalars and empty containers gain only the trailing newline.
  CO_REQUIRE_EQ(co::json::dump(Value(), true, 2), std::string("null\n"));
  CO_REQUIRE_EQ(co::json::dump(make_number(5), true, 0), std::string("5\n"));
  CO_REQUIRE_EQ(co::json::dump(make_string("x"), false, 2), std::string("\"x\"\n"));
  CO_REQUIRE_EQ(co::json::dump(Value::array(), true, 2), std::string("[]\n"));
  CO_REQUIRE_EQ(co::json::dump(Value::object(), true, 2), std::string("{}\n"));

  // Compact output carries no trailing newline.
  CO_REQUIRE_EQ(co::json::dump(document), std::string("{\"a\":{},\"b\":[1,2]}"));
}

CO_TEST(json_dump_escapes_minimally) {
  CO_REQUIRE_EQ(compact(make_string("quote\" backslash\\ slash/")), std::string("\"quote\\\" backslash\\\\ slash/\""));
  CO_REQUIRE_EQ(compact(make_string("\b\f\n\r\t")), std::string("\"\\b\\f\\n\\r\\t\""));
  CO_REQUIRE_EQ(compact(make_string("\x01\x1f")), std::string("\"\\u0001\\u001f\""));
  CO_REQUIRE_EQ(compact(make_string("\x7f")), std::string("\"\x7f\""));
  CO_REQUIRE_EQ(compact(make_string("\xC3\xA9\xE2\x82\xAC")), std::string("\"\xC3\xA9\xE2\x82\xAC\""));
  CO_REQUIRE_EQ(compact(make_string("")), std::string("\"\""));

  // Every control character round trips and none is emitted raw.
  std::string controls;
  for (int code = 1; code < 0x20; ++code) {
    controls.push_back(static_cast<char>(code));
  }
  const std::string dumped_controls = compact(make_string(controls));
  CO_REQUIRE_OK(parse_ok(dumped_controls).as_string(), decoded_controls);
  CO_REQUIRE_EQ(decoded_controls, controls);
  CO_REQUIRE(dumped_controls.find('\n') == std::string::npos);
  CO_REQUIRE(dumped_controls.find('\t') == std::string::npos);
  CO_REQUIRE(dumped_controls.find('\r') == std::string::npos);
  CO_REQUIRE(dumped_controls.find("\\u001f") != std::string::npos);

  // Object keys are escaped exactly like string values.
  Value keyed;
  keyed.set("a\nb", make_number(1));
  CO_REQUIRE_EQ(compact(keyed), std::string("{\"a\\nb\":1}"));
}

CO_TEST(json_round_trips_are_stable) {
  const std::string source =
      "{\"n\":null,\"t\":true,\"f\":false,\"i\":-42,\"s\":\"a\\\"b\\\\c\\u00e9\\ud83d\\ude00\",\"a\":[1,[2,[3]]],\"o\":{\"z\":1,\"y\":{}}}";
  const Value first = parse_ok(source);
  const std::string once = compact(first);
  const std::string twice = compact(parse_ok(once));
  CO_REQUIRE_EQ(twice, once);
  CO_REQUIRE(parse_ok(once) == first);
  CO_REQUIRE(parse_ok(written_order(first)) == first);
  CO_REQUIRE(parse_ok(co::json::dump(first, true, 4)) == first);
  CO_REQUIRE(parse_ok(co::json::dump(first, false, 2)) == first);

  // A document with duplicate free keys in every shape stays equal through
  // every writing mode.
  const Value shapes = parse_ok("[[],[{}],{\"a\":[]},{\"b\":{}},\"\",0,-1,true,false,null]");
  CO_REQUIRE(parse_ok(compact(shapes)) == shapes);
  CO_REQUIRE(parse_ok(written_order(shapes)) == shapes);
}

CO_TEST(json_object_equality_ignores_member_order) {
  CO_REQUIRE(parse_ok("{\"a\":1,\"b\":2}") == parse_ok("{\"b\":2,\"a\":1}"));
  CO_REQUIRE(parse_ok("{\"a\":1,\"b\":2}") != parse_ok("{\"a\":1,\"b\":3}"));
  CO_REQUIRE(parse_ok("{\"a\":1,\"b\":2}") != parse_ok("{\"a\":1}"));
  CO_REQUIRE(parse_ok("{\"a\":{\"x\":1,\"y\":2}}") == parse_ok("{\"a\":{\"y\":2,\"x\":1}}"));

  // Arrays are ordered; scalars are type sensitive.
  CO_REQUIRE(parse_ok("[1,2]") == parse_ok("[1,2]"));
  CO_REQUIRE(parse_ok("[1,2]") != parse_ok("[2,1]"));
  CO_REQUIRE(parse_ok("[1,2]") != parse_ok("[1,2,3]"));
  CO_REQUIRE(parse_ok("1") != parse_ok("true"));
  CO_REQUIRE(parse_ok("1") != parse_ok("\"1\""));
  CO_REQUIRE(parse_ok("null") != parse_ok("0"));
  CO_REQUIRE(make_number(0) != Value(false));
  CO_REQUIRE(parse_ok("null") == Value());
}

CO_TEST(json_handles_megabyte_documents) {
  // A one megabyte string value with no escapes in it.
  std::string big;
  big.reserve(1U << 20U);
  while (big.size() < (1U << 20U)) {
    big.append("0123456789abcdef");
  }
  const Value big_string(big);
  const std::string big_dump = compact(big_string);
  CO_REQUIRE_EQ(big_dump.size(), big.size() + 2);
  CO_REQUIRE_OK(parse_ok(big_dump).as_string(), reparsed_big);
  CO_REQUIRE_EQ(reparsed_big.size(), big.size());
  CO_REQUIRE(reparsed_big == big);

  // Half a megabyte of a large array of small objects.
  constexpr int kCount = 20000;
  std::string document = "[";
  for (int index = 0; index < kCount; ++index) {
    if (index != 0) {
      document.push_back(',');
    }
    document.append("{\"i\":");
    document.append(std::to_string(index));
    document.append(",\"k\":\"v\"}");
  }
  document.push_back(']');
  CO_REQUIRE(document.size() > 300000);

  const Value parsed = parse_ok(document);
  CO_REQUIRE(parsed.is_array());
  CO_REQUIRE_EQ(parsed.size(), static_cast<std::size_t>(kCount));
  const std::string once = compact(parsed);
  CO_REQUIRE_EQ(once.size(), document.size());
  CO_REQUIRE_EQ(compact(parse_ok(once)), once);
  CO_REQUIRE(parse_ok(once) == parsed);
}

CO_TEST(json_rejects_adversarial_documents) {
  struct RejectionCase {
    std::string_view document;
    ReasonCode code;
  };
  const RejectionCase cases[] = {
      {"", ReasonCode::EmptyInput},
      {"   \t\n", ReasonCode::EmptyInput},
      {"nul", ReasonCode::ParseError},
      {"nulll", ReasonCode::TrailingGarbage},
      {"tru", ReasonCode::ParseError},
      {"fals", ReasonCode::ParseError},
      {"TRUE", ReasonCode::ParseError},
      {"NULL", ReasonCode::ParseError},
      {"NaN", ReasonCode::ParseError},
      {"Infinity", ReasonCode::ParseError},
      {"-Infinity", ReasonCode::ParseError},
      {"undefined", ReasonCode::ParseError},
      {"\\", ReasonCode::ParseError},
      {"\"", ReasonCode::ParseError},
      {"'single'", ReasonCode::ParseError},
      {"/*comment*/1", ReasonCode::ParseError},
      {"//comment", ReasonCode::ParseError},
      {"{", ReasonCode::ParseError},
      {"[", ReasonCode::ParseError},
      {"{\"a\":1", ReasonCode::ParseError},
      {"[1,2", ReasonCode::ParseError},
      {"[\"\\u00\"]", ReasonCode::ParseError},
      {"[\"\\q\"]", ReasonCode::ParseError},
      {"[01]", ReasonCode::ParseError},
      {"[1,]", ReasonCode::ParseError},
      {"{\"a\":1,}", ReasonCode::ParseError},
      {"{\"a\":1,\"a\":2}", ReasonCode::DuplicateKey},
      {"{\"a\":{\"b\":1,\"b\":1}}", ReasonCode::DuplicateKey},
      {"{\"a\":1}extra", ReasonCode::TrailingGarbage},
      {"1,2", ReasonCode::TrailingGarbage},
      {"[1][2]", ReasonCode::TrailingGarbage},
      {"\"a\"\"b\"", ReasonCode::TrailingGarbage},
      {"9223372036854775808", ReasonCode::Overflow},
      {"-9223372036854775809", ReasonCode::Overflow},
      {"[1,2,3,4,5,6,7,8,9,10", ReasonCode::ParseError},
      {"{\"a\":[{\"b\":[{\"c\":1}]}]}", ReasonCode::Ok},
  };
  for (const RejectionCase& test_case : cases) {
    const auto result = Value::parse(test_case.document);
    if (test_case.code == ReasonCode::Ok) {
      CO_REQUIRE(result.ok());
      continue;
    }
    CO_REQUIRE(!result.ok());
    CO_REQUIRE_EQ(result.status().code(), test_case.code);
    CO_REQUIRE(!result.status().detail().empty());
  }

  // A deep bomb built from alternating containers is refused as well.
  std::string bomb;
  for (int index = 0; index < 20000; ++index) {
    bomb.push_back(index % 2 == 0 ? '[' : '{');
  }
  CO_REQUIRE_ERR(Value::parse(bomb, 1U << 20U), ReasonCode::ParseError);
  CO_REQUIRE_ERR(Value::parse(bomb), ReasonCode::ParseError);
}