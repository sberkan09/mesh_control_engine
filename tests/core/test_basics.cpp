// Small pure helpers in core that other tests only use in passing: the power-of-two and alignment
// arithmetic, the instancing draw-cause names and hints, the error code names, compile-time string ids
// and the engine version string.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/draw_cause.h"
#include "mce/result.h"
#include "mce/string_id.h"
#include "mce/types.h"
#include "mce/version.h"

using namespace mce;

TEST_CASE("types: is_pow2, align_up and next_pow2") {
    CHECK_FALSE(is_pow2(0));
    CHECK(is_pow2(1));
    CHECK(is_pow2(2));
    CHECK_FALSE(is_pow2(3));
    CHECK(is_pow2(usize(1) << 20));
    CHECK_FALSE(is_pow2((usize(1) << 20) + 1));

    CHECK(align_up(0, 16) == 0);
    CHECK(align_up(1, 16) == 16);
    CHECK(align_up(16, 16) == 16);
    CHECK(align_up(17, 16) == 32);
    CHECK(align_up(5, 1) == 5);

    CHECK(next_pow2(0) == 1);
    CHECK(next_pow2(1) == 1);
    CHECK(next_pow2(2) == 2);
    CHECK(next_pow2(3) == 4);
    CHECK(next_pow2(1000) == 1024);
    CHECK(next_pow2(1024) == 1024);
    CHECK(next_pow2(1025) == 2048);
    static_assert(next_pow2(33) == 64);  // usable at compile time
}

TEST_CASE("draw causes: every cause has a distinct name and a hint, Count has neither") {
    for (u32 i = 0; i < kDrawCauseCount; ++i) {
        const DrawCause c = static_cast<DrawCause>(i);
        INFO("cause " << i);
        CHECK(std::strlen(draw_cause_name(c)) > 0);
        CHECK(std::strcmp(draw_cause_name(c), "?") != 0);
        CHECK(std::strlen(draw_cause_hint(c)) > 0);
        for (u32 j = i + 1; j < kDrawCauseCount; ++j)
            CHECK(std::strcmp(draw_cause_name(c), draw_cause_name(static_cast<DrawCause>(j))) != 0);
    }
    CHECK(std::strcmp(draw_cause_name(DrawCause::Disabled), "instancing_off") == 0);
    CHECK(std::strcmp(draw_cause_name(DrawCause::Count), "?") == 0);
    CHECK(std::strlen(draw_cause_hint(DrawCause::Count)) == 0);
}

TEST_CASE("result: errc_name names a code and falls back for an unknown one") {
    CHECK(std::strcmp(errc_name(Errc::Ok), "Ok") == 0);
    CHECK(std::strcmp(errc_name(Errc::FileNotFound), "FileNotFound") == 0);
    CHECK(std::strcmp(errc_name(Errc::CrcMismatch), "CrcMismatch") == 0);
    CHECK(std::strcmp(errc_name(Errc::ModuleInstallFailed), "ModuleInstallFailed") == 0);
    CHECK(std::strcmp(errc_name(static_cast<Errc>(0x7fff)), "Errc(?)") == 0);
}

TEST_CASE("string id: make_sid and MCE_SID are the runtime hash at compile time") {
    constexpr StringId a = detail::make_sid("player.spawn");
    static_assert(a == StringId("player.spawn"));
    CHECK(a == StringId(StringView("player.spawn")));
    CHECK(a != StringId("player.despawn"));
    CHECK(MCE_SID("player.spawn") == a);
}

TEST_CASE("version: engine_describe is a non-empty string") {
    const char* d = engine_describe();
    REQUIRE(d != nullptr);
    CHECK(std::strlen(d) > 0);
}
