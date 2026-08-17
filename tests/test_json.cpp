#include "doctest.h"

#include "util/json.h"

using vw::util::Json;

TEST_CASE("json: primitives parse") {
    auto t = Json::parse(L"true");
    REQUIRE(t);
    CHECK(t->asBool(false) == true);

    auto f = Json::parse(L"false");
    REQUIRE(f);
    CHECK(f->asBool(true) == false);

    auto n = Json::parse(L"null");
    REQUIRE(n);
    CHECK(n->isNull());

    auto s = Json::parse(L"\"hello\\nworld\"");
    REQUIRE(s);
    CHECK(s->asString() == L"hello\nworld");

    auto num = Json::parse(L"-12.5");
    REQUIRE(num);
    CHECK(num->asNumber() == doctest::Approx(-12.5));

    auto intNum = Json::parse(L"85");
    REQUIRE(intNum);
    CHECK(intNum->asInt() == 85);
}

TEST_CASE("json: arrays and objects") {
    auto arr = Json::parse(L"[1, 2, 3]");
    REQUIRE(arr);
    CHECK(arr->asArray().size() == 3);
    CHECK(arr->asArray()[2].asInt() == 3);

    auto obj = Json::parse(L"{\"a\": 1, \"b\": {\"c\": \"x\"}}");
    REQUIRE(obj);
    CHECK(obj->isObject());
    CHECK(obj->get(L"a").asInt() == 1);
    CHECK(obj->get(L"b").get(L"c").asString() == L"x");
    CHECK(obj->get(L"missing").isNull());
}

TEST_CASE("json: round-trip serialize") {
    auto parsed = Json::parse(L"{\"n\": 42, \"s\": \"a\\\"b\", \"arr\": [true, null, 1.5]}");
    REQUIRE(parsed);
    auto reparsed = Json::parse(parsed->serialize());
    REQUIRE(reparsed);
    CHECK(reparsed->get(L"n").asInt() == 42);
    CHECK(reparsed->get(L"s").asString() == L"a\"b");
    CHECK(reparsed->get(L"arr").asArray().size() == 3);
}

TEST_CASE("json: unicode escape") {
    auto v = Json::parse(L"\"\\u0041\"");
    REQUIRE(v);
    CHECK(v->asString() == L"A");
}

TEST_CASE("json: malformed input rejected") {
    CHECK_FALSE(Json::parse(L""));
    CHECK_FALSE(Json::parse(L"{"));
    CHECK_FALSE(Json::parse(L"[1,]"));
    CHECK_FALSE(Json::parse(L"{\"a\":}"));
    CHECK_FALSE(Json::parse(L"nul"));
    CHECK_FALSE(Json::parse(L"01"));
    CHECK_FALSE(Json::parse(L"\"unterminated"));
    CHECK_FALSE(Json::parse(L"{\"a\": 1} trailing"));
    CHECK_FALSE(Json::parse(L"'single'"));
}

TEST_CASE("json: whitespace tolerated") {
    auto v = Json::parse(L"  { \"a\" : 1 }  ");
    REQUIRE(v);
    CHECK(v->get(L"a").asInt() == 1);
}

TEST_CASE("json: number edge cases") {
    auto exp = Json::parse(L"1e3");
    REQUIRE(exp);
    CHECK(exp->asNumber() == doctest::Approx(1000.0));
    CHECK_FALSE(Json::parse(L"1e"));
    CHECK_FALSE(Json::parse(L"-"));
}

TEST_CASE("json: all string escapes decode") {
    auto v = Json::parse(L"\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"");
    REQUIRE(v);
    CHECK(v->asString() == L"\"\\/\b\f\n\r\t");
}

TEST_CASE("json: unicode raw and escaped") {
    // Raw non-ASCII char round-trips without escaping.
    auto raw = Json::parse(L"\"\xE9\"");
    REQUIRE(raw);
    CHECK(raw->asString() == L"\xE9");
    CHECK(raw->serialize() == L"\"\xE9\"");

    // \u escape with lowercase and uppercase hex digits.
    auto low = Json::parse(L"\"\\u00e9\"");
    REQUIRE(low);
    CHECK(low->asString() == L"\xE9");
    auto up = Json::parse(L"\"\\u00E9\"");
    REQUIRE(up);
    CHECK(up->asString() == L"\xE9");

    // \u0000 yields an embedded NUL (only raw control chars are rejected).
    auto nul = Json::parse(L"\"\\u0000\"");
    REQUIRE(nul);
    CHECK(nul->asString() == std::wstring(1, L'\0'));
    CHECK(nul->serialize() == L"\"\\u0000\"");

    // Empty string.
    auto empty = Json::parse(L"\"\"");
    REQUIRE(empty);
    CHECK(empty->asString().empty());
}

TEST_CASE("json: surrogate pair round-trip") {
    // \ud83d\ude00 = U+1F600; wchar_t is UTF-16 code units, so the pair is
    // preserved verbatim on parse and serialize (no combining/splitting).
    auto v = Json::parse(L"\"\\ud83d\\ude00\"");
    REQUIRE(v);
    CHECK(v->asString().size() == 2);
    CHECK(v->asString()[0] == static_cast<wchar_t>(0xD83D));
    CHECK(v->asString()[1] == static_cast<wchar_t>(0xDE00));
    auto again = Json::parse(v->serialize());
    REQUIRE(again);
    CHECK(again->asString() == v->asString());
}

TEST_CASE("json: number format edge cases") {
    // Valid formats.
    CHECK(Json::parse(L"0")->asNumber() == doctest::Approx(0.0));
    CHECK(Json::parse(L"-0")->asNumber() == doctest::Approx(0.0));
    CHECK(Json::parse(L"0.0")->asNumber() == doctest::Approx(0.0));
    CHECK(Json::parse(L"1.5e3")->asNumber() == doctest::Approx(1500.0));
    CHECK(Json::parse(L"1e+5")->asNumber() == doctest::Approx(100000.0));
    CHECK(Json::parse(L"1e-5")->asNumber() == doctest::Approx(0.00001));
    CHECK(Json::parse(L"0e0")->asNumber() == doctest::Approx(0.0));

    // Rejected formats.
    CHECK_FALSE(Json::parse(L".5"));      // leading dot
    CHECK_FALSE(Json::parse(L"5."));      // trailing dot
    CHECK_FALSE(Json::parse(L"1.e2"));    // dot with no fraction digit
    CHECK_FALSE(Json::parse(L"1e5.5"));   // fraction after exponent
    CHECK_FALSE(Json::parse(L"1e+"));     // exponent with no digits
    CHECK_FALSE(Json::parse(L"+1"));      // leading plus
    CHECK_FALSE(Json::parse(L"- 1"));     // whitespace inside number
    CHECK_FALSE(Json::parse(L"-01"));     // leading zero after sign
    CHECK_FALSE(Json::parse(L"1e999"));   // overflows to inf -> rejected
}

TEST_CASE("json: structural edge cases") {
    // Empty containers and nesting.
    auto emptyObj = Json::parse(L"{}");
    REQUIRE(emptyObj);
    CHECK(emptyObj->isObject());
    CHECK(emptyObj->asObject().empty());
    auto emptyArr = Json::parse(L"[]");
    REQUIRE(emptyArr);
    CHECK(emptyArr->asArray().empty());
    auto nested = Json::parse(L"[[],{}]");
    REQUIRE(nested);
    CHECK(nested->asArray().size() == 2);
    CHECK(nested->asArray()[1].isObject());
    auto emptyKey = Json::parse(L"{\"\":1}");
    REQUIRE(emptyKey);
    CHECK(emptyKey->get(L"").asInt() == 1);

    // Trailing commas rejected.
    CHECK_FALSE(Json::parse(L"{\"a\":1,}"));
    CHECK_FALSE(Json::parse(L"[,]"));
    CHECK_FALSE(Json::parse(L"{:}"));

    // Missing separators rejected.
    CHECK_FALSE(Json::parse(L"{\"a\" 1}"));        // no colon
    CHECK_FALSE(Json::parse(L"{\"a\":1 \"b\":2}")); // no comma
}

TEST_CASE("json: duplicate keys rejected (strict)") {
    // Strict validation: ambiguous duplicate keys are a parse error (the config
    // manager then applies its corrupt-file recovery instead of silently
    // picking a value).
    CHECK_FALSE(Json::parse(L"{\"a\":1,\"a\":2}"));
}

TEST_CASE("json: literal word boundary enforced") {
    CHECK_FALSE(Json::parse(L"truex"));
    CHECK_FALSE(Json::parse(L"falsey"));
    CHECK_FALSE(Json::parse(L"nulll"));
    CHECK_FALSE(Json::parse(L"tru"));   // truncated literal
    CHECK_FALSE(Json::parse(L"truefalse"));
}

TEST_CASE("json: whitespace-only and empty input rejected") {
    CHECK_FALSE(Json::parse(L""));
    CHECK_FALSE(Json::parse(L"   "));
    CHECK_FALSE(Json::parse(L"\t\r\n"));
}

TEST_CASE("json: recursion depth bounded") {
    // Deeply nested user-writable input must not smash the stack: rejected
    // cleanly past the 512 cap, accepted within it.
    std::wstring tooDeep(1000, L'[');
    tooDeep += std::wstring(1000, L']');
    CHECK_FALSE(Json::parse(tooDeep));

    std::wstring ok(100, L'[');
    ok += L"1";
    ok += std::wstring(100, L']');
    auto v = Json::parse(ok);
    REQUIRE(v);

    std::wstring atCap(512, L'[');
    atCap += L"1";
    atCap += std::wstring(512, L']');
    REQUIRE(Json::parse(atCap));
}

TEST_CASE("json: type accessors on wrong types return defaults") {
    auto num = Json::parse(L"42");
    REQUIRE(num);
    CHECK(num->asString(L"d") == L"d");
    CHECK(num->asArray().empty());
    CHECK(num->asObject().empty());
    CHECK(num->get(L"x").isNull());
    CHECK(num->asBool(true));        // wrong type -> default returned
    CHECK_FALSE(num->asBool());

    auto str = Json::parse(L"\"5\"");
    REQUIRE(str);
    CHECK(str->asInt(7) == 7);
    CHECK(str->asNumber(3.5) == doctest::Approx(3.5));
    CHECK(str->asBool(true));

    auto arr = Json::parse(L"[1]");
    REQUIRE(arr);
    CHECK(arr->asObject().empty());
    CHECK(arr->get(L"k").isNull());
}

TEST_CASE("json: asInt truncates toward zero") {
    CHECK(Json::parse(L"1.9")->asInt() == 1);
    CHECK(Json::parse(L"-1.9")->asInt() == -1);
}

TEST_CASE("json: control characters serialize as \\u escapes and reparse") {
    Json::Object obj;
    obj.emplace(L"ctrl", Json::string(std::wstring(1, L'\x01')));
    const std::wstring text = Json::object(std::move(obj)).serialize();
    CHECK(text == L"{\"ctrl\": \"\\u0001\"}");
    auto v = Json::parse(text);
    REQUIRE(v);
    CHECK(v->get(L"ctrl").asString() == std::wstring(1, L'\x01'));
}

TEST_CASE("json: keys with quotes/backslashes escape on serialize") {
    // Raw literal so the JSON text is exactly {"a\"b\\c": 1} (key = a"b\c).
    const std::wstring text = LR"({"a\"b\\c": 1})";
    auto v = Json::parse(text);
    REQUIRE(v);
    CHECK(v->get(L"a\"b\\c").asInt() == 1);
    auto again = Json::parse(v->serialize());
    REQUIRE(again);
    CHECK(again->get(L"a\"b\\c").asInt() == 1);
}
