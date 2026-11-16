// DynamicResolution3D (M-DYNRES 46d): the component defaults match the render setting (except `enabled`: the component is
// the opt-in, so adding one turns the controller on), the explicit pad is zero, the layout has no implicit padding, and
// every authored field is reflected and saved.

#include <doctest/doctest.h>

#include <bit>

#include "mce/kit/dynamic_resolution.h"
#include "mce/refl/type.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

}  // namespace

TEST_CASE("dynamic resolution: component defaults match the render setting") {
    const DynamicResolution3D c;
    const render::DynamicResolution r;
    CHECK(c.enabled == 1u);
    CHECK_FALSE(r.enabled);  // the render default is off; the component is the opt-in
    CHECK(same_bits(c.target_ms, r.target_ms));
    CHECK(same_bits(c.min_scale, r.min_scale));
    CHECK(same_bits(c.max_scale, r.max_scale));
    CHECK(same_bits(c.headroom, r.headroom));
    CHECK(same_bits(c.target_ms, 14.0f));
    CHECK(same_bits(c.min_scale, 0.6f));
    CHECK(same_bits(c.max_scale, 1.0f));
    CHECK(same_bits(c.headroom, 0.15f));
    CHECK(c.pad_[0] == 0u);
    CHECK(c.pad_[1] == 0u);
    CHECK(c.pad_[2] == 0u);
    CHECK(sizeof(DynamicResolution3D) == 4 * sizeof(f32) + 4);  // no implicit padding (state_hash hashes whole columns)
}

TEST_CASE("dynamic resolution: every authored field is reflected and saved") {
    const refl::TypeRecord* t = refl::find_type(StringId("DynamicResolution3D"));
    REQUIRE(t != nullptr);
    CHECK(t->is_component());
    CHECK(t->fields.size() == 5);
    for (const char* n : {"enabled", "target_ms", "min_scale", "max_scale", "headroom"}) {
        CAPTURE(n);
        const refl::FieldRecord* f = t->find_field(StringId(n));
        REQUIRE(f != nullptr);
        CHECK(f->has(refl::FieldFlags::Save));
        CHECK(f->has(refl::FieldFlags::EditorRange));
    }
}
