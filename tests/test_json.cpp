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
