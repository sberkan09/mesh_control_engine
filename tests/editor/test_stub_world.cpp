#include <doctest/doctest.h>

#include "mce/editor/stub_world.h"
#include "mce/math/math.h"

using namespace mce;
using namespace mce::editor;

TEST_CASE("StubWorld ticks at a fixed rate and stays bounded") {
    StubWorld w;
    for (int i = 0; i < 60; ++i) w.tick(1.0 / 60.0);
    CHECK(w.ticks() == 60);
    CHECK(w.angle() == doctest::Approx(0.25f * math::kTau).epsilon(1e-4));
    for (int i = 0; i < 60 * 600; ++i) w.tick(1.0 / 60.0);  // ten minutes
    CHECK(w.angle() >= 0.0f);
    CHECK(w.angle() <= 2.0f * math::kTau);
}

TEST_CASE("StubWorld records one pass without a device (pipeline-less clear)") {
    StubWorld w;
    rhi::CommandBuffer cmd;
    w.record(cmd, {}, rhi::Extent2D{64, 64}, 0.5f);
    CHECK_FALSE(cmd.in_pass());
    CHECK(cmd.command_count() == 2);  // BeginPass + EndPass
}
