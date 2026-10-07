#include <doctest/doctest.h>

#include "mce/result.h"
#include "test_support.h"

using namespace mce;

namespace {
Result<int> parse_digit(char c) {
    if (c < '0' || c > '9') return Error(Errc::InvalidArgument, "test.parse", "digit", static_cast<u32>(c));
    return c - '0';
}

Result<int> sum_two(char a, char b) {
    MCE_TRY_ASSIGN(int x, parse_digit(a));
    MCE_TRY_ASSIGN(int y, parse_digit(b));
    return x + y;
}

ResultV check_digit(char c) {
    MCE_TRY(parse_digit(c));
    return ok();
}
}  // namespace

TEST_CASE("Result carries value or error") {
    Result<int> good = parse_digit('7');
    REQUIRE(good);
    CHECK(good.value() == 7);

    Result<int> bad = parse_digit('x');
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == Errc::InvalidArgument);
    CHECK(bad.error().path == "digit");
    CHECK(bad.error().detail == static_cast<u32>('x'));
    CHECK(bad.value_or(-1) == -1);
}

TEST_CASE("MCE_TRY propagates errors") {
    CHECK(sum_two('2', '3').value() == 5);
    CHECK_FALSE(sum_two('2', 'z'));
    CHECK(check_digit('1'));
    CHECK(check_digit('q').error().code == Errc::InvalidArgument);
}

#if MCE_ASSERTS_ENABLED
TEST_CASE("value() on error asserts") {
    Result<int> bad = parse_digit('x');
    test::ExpectAssert expect;
    (void)bad.value();
}
#endif

TEST_CASE("format_error is diagnosable") {
    FixedString<512> s;
    format_error(Error(Errc::CrcMismatch, "asset.load", "content://level1.mce", 42), s);
    CHECK(s == "CrcMismatch [asset.load] content://level1.mce (detail 42)");
}

TEST_CASE("Result with non-trivial payload copies and moves") {
    struct Payload {
        int* counter;
        explicit Payload(int* c) : counter(c) { ++*counter; }
        Payload(const Payload& o) : counter(o.counter) { ++*counter; }
        ~Payload() { --*counter; }
    };
    int live = 0;
    {
        Result<Payload> a = Payload(&live);
        Result<Payload> b = a;
        CHECK(live == 2);
        b = Error(Errc::Unknown, "x");
        CHECK(live == 1);
    }
    CHECK(live == 0);
}
