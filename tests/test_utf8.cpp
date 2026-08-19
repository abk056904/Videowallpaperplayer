#include "doctest.h"

#include <string>

#include "util/utf8.h"

using vw::util::utf8ToWide;
using vw::util::wideToUtf8;

TEST_CASE("utf8: ASCII round-trip") {
    const std::wstring s = L"hello 42";
    const std::string utf8 = wideToUtf8(s);
    CHECK(utf8 == "hello 42");
    auto back = utf8ToWide(utf8);
    REQUIRE(back);
    CHECK(*back == s);
}

TEST_CASE("utf8: non-ASCII round-trip") {
    // é (2-byte), CJK (3-byte), emoji via surrogate pair (4-byte).
    const std::wstring s = L"caf\u00E9 \u4E2D\u6587 \U0001F600";
    const std::string utf8 = wideToUtf8(s);
    CHECK(utf8 == "caf\xC3\xA9 \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80");
    auto back = utf8ToWide(utf8);
    REQUIRE(back);
    CHECK(*back == s);
}

TEST_CASE("utf8: empty strings") {
    CHECK(wideToUtf8(L"") == "");
    auto back = utf8ToWide(std::string(""));
    REQUIRE(back);
    CHECK(back->empty());
}

TEST_CASE("utf8: invalid input rejected") {
    CHECK_FALSE(utf8ToWide(std::string("\xE9", 1)));         // lone lead byte
    CHECK_FALSE(utf8ToWide(std::string("a\xC3", 2)));        // truncated sequence
    CHECK_FALSE(utf8ToWide(std::string("\x80", 1)));         // stray continuation
    CHECK_FALSE(utf8ToWide(std::string("\xFF\xFE", 2)));     // invalid bytes
}
