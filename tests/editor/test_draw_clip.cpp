#include <doctest/doctest.h>

#include "mce/editor/draw_clip.h"

using namespace mce;
using namespace mce::editor;

namespace {

f32 area(const Array<ClipVertex>& v, const Array<u32>& idx) {
    f32 sum = 0;
    for (usize i = 0; i + 2 < idx.size(); i += 3) {
        const ClipVertex &a = v[idx[i]], &b = v[idx[i + 1]], &c = v[idx[i + 2]];
        const f32 cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        sum += (cross < 0 ? -cross : cross) * 0.5f;
    }
    return sum;
}

struct Fixture {
    // A 10x10 quad at (0,0), UV 0..1, white-to-black color along x.
    ClipVertex quad[4] = {
        {0, 0, 0, 0, 0xFFFFFFFFu},
        {10, 0, 1, 0, 0xFF000000u},
        {10, 10, 1, 1, 0xFF000000u},
        {0, 10, 0, 1, 0xFFFFFFFFu},
    };
    u16 idx[6] = {0, 1, 2, 0, 2, 3};
    Array<ClipVertex> out_v;
    Array<u32> out_i;

    u32 run(ClipRect r) {
        out_v.clear();
        out_i.clear();
        for (const ClipVertex& v : quad) out_v.push_back(v);
        return clip_triangles(Span<const ClipVertex>(quad), 0, Span<const u16>(idx), r, out_v, out_i);
    }
};

}  // namespace

TEST_CASE("draw_clip passes inside triangles through untouched") {
    Fixture f;
    CHECK(f.run({-1, -1, 20, 20}) == 6);
    CHECK(f.out_v.size() == 4);  // no new vertices
    CHECK(f.out_i[0] == 0);
    CHECK(f.out_i[5] == 3);
}

TEST_CASE("draw_clip drops outside triangles and empty rects") {
    Fixture f;
    CHECK(f.run({20, 20, 30, 30}) == 0);
    CHECK(f.run({5, 5, 5, 9}) == 0);
    CHECK(f.out_i.empty());
}

TEST_CASE("draw_clip cuts straddling triangles to the rect") {
    Fixture f;
    f.run({2, 3, 7, 8});
    CHECK(area(f.out_v, f.out_i) == doctest::Approx(25.0f));
    for (u32 i : f.out_i) {
        const ClipVertex& v = f.out_v[i];
        CHECK(v.x >= 2.0f - 1e-4f);
        CHECK(v.x <= 7.0f + 1e-4f);
        CHECK(v.y >= 3.0f - 1e-4f);
        CHECK(v.y <= 8.0f + 1e-4f);
        // UV and color follow position.
        CHECK(v.u == doctest::Approx(v.x / 10.0f));
        CHECK(v.v == doctest::Approx(v.y / 10.0f));
    }
    // Halfway across, the white-to-black gradient is mid grey.
    f.run({5, 0, 10, 10});
    bool found_mid = false;
    for (u32 i : f.out_i) {
        const ClipVertex& v = f.out_v[i];
        if (v.x == doctest::Approx(5.0f)) {
            CHECK((v.rgba & 0xFFu) == 128u);
            CHECK((v.rgba >> 24) == 255u);
            found_mid = true;
        }
    }
    CHECK(found_mid);
}

TEST_CASE("draw_clip respects the output vertex base") {
    Fixture f;
    f.out_v.clear();
    f.out_i.clear();
    for (int i = 0; i < 5; ++i) f.out_v.push_back(ClipVertex{});
    for (const ClipVertex& v : f.quad) f.out_v.push_back(v);
    CHECK(clip_triangles(Span<const ClipVertex>(f.quad), 5, Span<const u16>(f.idx), {-1, -1, 20, 20}, f.out_v, f.out_i) ==
          6);
    CHECK(f.out_i[0] == 5);
    CHECK(f.out_i[1] == 6);
}
