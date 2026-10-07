#include <doctest/doctest.h>

#include "mce/hash.h"
#include "mce/string_id.h"

using namespace mce;

TEST_CASE("crc32 matches the IEEE check value") {
    const char* s = "123456789";
    CHECK(crc32(s, 9) == 0xCBF43926u);
    // Incremental equals one-shot.
    CHECK(crc32(s + 4, 5, crc32(s, 4)) == 0xCBF43926u);
}

TEST_CASE("fnv1a64 reference values") {
    CHECK(fnv1a64(StringView("")) == 0xcbf29ce484222325ull);
    CHECK(fnv1a64(StringView("a")) == 0xaf63dc4c8601ec8cull);
    CHECK(fnv1a64("a", 1) == fnv1a64(StringView("a")));
}

TEST_CASE("StringId is compile-time and stable") {
    constexpr StringId id = MCE_SID("time_scale");
    static_assert(id.value == fnv1a64(StringView("time_scale")));
    CHECK(StringId("time_scale") == id);
    CHECK(StringId("r.vsync") != id);
}

TEST_CASE("Hasher64 is order sensitive") {
    Hasher64 a, b;
    a.u32v(1);
    a.u32v(2);
    b.u32v(2);
    b.u32v(1);
    CHECK(a.value() != b.value());
    Hasher64 c;
    c.u32v(1);
    c.u32v(2);
    CHECK(a.value() == c.value());
}
