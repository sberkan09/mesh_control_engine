// Terrain brushes (M-TERRAIN 4), headless: the brush ops on a TerrainDocument, and strokes on a
// project's terrain that save the .mcterrain, re-bake it in the live mount (the cook round trip) and
// undo and redo; the viewport's ray finds the surface under the entity's transform.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/formats.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/terrain_editor.h"
#include "mce/kit/kit3d.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr const char* kTerrain = "7e7a0000-0000-4000-8000-0000000000e1";
constexpr const char* kGrass = "7e7a0000-0000-4000-8000-0000000000e2";
constexpr const char* kRock = "7e7a0000-0000-4000-8000-0000000000e3";

asset::Guid guid(const char* s) { return asset::parse_guid(s).value(); }

std::string open_demo(EditorSession& session, const char* name) {
    const std::string root = cook_test::fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    return root;
}

// A flat 65-sample, 64 m terrain at 4 m in [0, 20] m, painted grass by an auto rule.
const char* source() {
    return R"({ "format": "mce-terrain", "version": 1, "guid": "7e7a0000-0000-4000-8000-0000000000e1", "name": "valley",
  "resolution": 65, "chunk": 16, "size": [64, 64], "height_range": [0, 20], "fill": 4,
  "layers": [ { "material": "guid:7e7a0000-0000-4000-8000-0000000000e2", "tile": 3, "auto": { "slope": [0, 90] } },
              { "material": "guid:7e7a0000-0000-4000-8000-0000000000e3", "triplanar": true } ] })";
}

// What the live mount serves for the terrain right now.
struct Mounted {
    Array<f32> heights;
    Array<u8> layer1;  // weights of layer 1
    u32 holes = 0;
    u32 flags = 0;
};
Mounted mounted(EditorSession& session) {
    Mounted m;
    asset::AssetSystem* assets = session.project().mounted();
    Result<asset::AssetHandle<asset::Terrain>> h = assets->load_sync<asset::Terrain>(assets->find(guid(kTerrain)));
    REQUIRE(h);
    Result<asset::TerrainView> v = asset::TerrainView::read(assets->resolve(h.value()));
    REQUIRE(v);
    const asset::TerrainView& t = v.value();
    m.flags = t.header->flags;
    for (u32 z = 0; z < t.resolution(); ++z)
        for (u32 x = 0; x < t.resolution(); ++x) {
            m.heights.push_back(t.sample(x, z));
            m.holes += t.hole(x, z);
        }
    for (u32 z = 0; z < t.splat_resolution(); ++z)
        for (u32 x = 0; x < t.splat_resolution(); ++x) m.layer1.push_back(t.weight(x, z, 1));
    assets->release(h.value());
    return m;
}

TerrainDocument flat_doc(u32 layers = 2) {
    asset::TerrainHeader h{};
    h.resolution = 33;
    h.chunk = 16;
    h.size[0] = h.size[1] = 32.0f;
    h.height_min = 0.0f;
    h.height_max = 10.0f;
    h.layer_count = layers;
    h.splat_resolution = layers ? 33 : 0;
    Array<u16> heights;
    heights.resize(33 * 33, u16{13107});  // 2 m
    Array<asset::TerrainLayer> ls;
    ls.resize(layers);
    for (asset::TerrainLayer& l : ls) l.tile = 4.0f;
    Array<u8> splat;
    if (layers) {
        splat.resize(33 * 33 * 8, u8{0});
        for (usize i = 0; i < 33 * 33; ++i) splat[i * 4] = 255;  // all layer 0
    }
    Array<byte> blob;
    asset::write_terrain(h, heights.span(), blob, ls.span(), splat.span());
    TerrainDocument d;
    cook::DiagList diags;
    REQUIRE(d.load(R"({ "format": "mce-terrain", "version": 1, "name": "t", "resolution": 33, "fill": 2 })", blob.span(), "t.mcterrain", diags));
    return d;
}

}  // namespace

TEST_CASE("terrain brushes: falloff, raise and lower, smooth, flatten, noise, paint, holes") {
    TerrainBrush b;
    b.radius = 4.0f;
    b.hardness = 0.5f;
    CHECK(terrain_brush_falloff(b, 0.0f) == 1.0f);
    CHECK(terrain_brush_falloff(b, 2.0f) == 1.0f);
    CHECK(terrain_brush_falloff(b, 3.0f) == doctest::Approx(0.5f));
    CHECK(terrain_brush_falloff(b, 4.0f) == 0.0f);

    TerrainDocument d = flat_doc();
    CHECK(d.height(16, 16) == doctest::Approx(2.0f).epsilon(1e-3));
    b.op = TerrainBrushOp::Raise;
    b.strength = 1.5f;
    CHECK(d.apply(b, {0.0f, 0.0f}) > 0);
    CHECK(d.height(16, 16) == doctest::Approx(3.5f).epsilon(1e-3));  // the centre: full strength
    CHECK(d.height(16 + 5, 16) == doctest::Approx(2.0f).epsilon(1e-3));  // past the rim: untouched
    CHECK(d.height(16 + 3, 16) > 2.0f);
    CHECK(d.height(16 + 3, 16) < 3.5f);
    b.op = TerrainBrushOp::Lower;
    b.strength = 100.0f;  // clamped to the height range
    d.apply(b, {0.0f, 0.0f});
    CHECK(d.height(16, 16) == 0.0f);

    // Smooth evens out a spike; flatten pulls toward the target.
    TerrainDocument s = flat_doc();
    s.heights[16 * 33 + 16] = 8.0f;
    TerrainBrush sm;
    sm.op = TerrainBrushOp::Smooth;
    sm.radius = 3.0f;
    sm.strength = 1.0f;
    sm.hardness = 1.0f;
    s.apply(sm, {0.0f, 0.0f});
    CHECK(s.height(16, 16) < 3.0f);
    CHECK(s.height(17, 16) > 2.0f);
    TerrainBrush fl;
    fl.op = TerrainBrushOp::Flatten;
    fl.radius = 3.0f;
    fl.strength = 1.0f;
    fl.hardness = 1.0f;
    fl.target = 6.0f;
    s.apply(fl, {0.0f, 0.0f});
    CHECK(s.height(16, 16) == doctest::Approx(6.0f));
    CHECK(s.height(17, 17) == doctest::Approx(6.0f));

    // Noise is fixed per position and seed: two documents get the same bumps.
    TerrainDocument n1 = flat_doc(), n2 = flat_doc();
    TerrainBrush nb;
    nb.op = TerrainBrushOp::Noise;
    nb.radius = 8.0f;
    nb.strength = 1.0f;
    nb.hardness = 1.0f;
    n1.apply(nb, {0.0f, 0.0f});
    n2.apply(nb, {0.0f, 0.0f});
    u32 moved = 0;
    for (usize i = 0; i < n1.heights.size(); ++i) {
        CHECK(n1.heights[i] == n2.heights[i]);
        moved += std::fabs(n1.heights[i] - 2.0f) > 0.01f;
    }
    CHECK(moved > 50);

    // Paint moves weight to the layer and keeps every sample summing to 255.
    TerrainDocument p = flat_doc();
    TerrainBrush pb;
    pb.op = TerrainBrushOp::Paint;
    pb.layer = 1;
    pb.radius = 4.0f;
    pb.strength = 0.5f;
    pb.hardness = 1.0f;
    CHECK(p.apply(pb, {0.0f, 0.0f}) > 0);
    CHECK(p.weight(16, 16, 1) == doctest::Approx(128).epsilon(0.01));
    CHECK(p.weight(16, 16, 0) + p.weight(16, 16, 1) == 255);
    p.apply(pb, {0.0f, 0.0f}, 2.0f);  // a full-strength dab
    CHECK(p.weight(16, 16, 1) == 255);
    CHECK(p.weight(16, 16, 0) == 0);
    CHECK(p.weight(25, 16, 1) == 0);
    pb.layer = 5;  // no such layer
    CHECK(p.apply(pb, {0.0f, 0.0f}) == 0);
    CHECK(flat_doc(0).apply(pb, {0.0f, 0.0f}) == 0);

    // Holes cut inside the radius and fill closes them again.
    TerrainDocument h = flat_doc();
    TerrainBrush hb;
    hb.op = TerrainBrushOp::Hole;
    hb.radius = 1.5f;
    CHECK(h.apply(hb, {0.0f, 0.0f}) == 9);  // the 3 x 3 samples within 1.5 m (1 m spacing)
    CHECK(h.hole(16, 16));
    CHECK(h.hole(17, 17));
    CHECK_FALSE(h.hole(18, 16));
    hb.op = TerrainBrushOp::Fill;
    CHECK(h.apply(hb, {0.0f, 0.0f}) == 9);
    CHECK_FALSE(h.hole(16, 16));

    // A brush off the terrain changes nothing.
    b.op = TerrainBrushOp::Raise;
    b.strength = 1.0f;
    CHECK(d.apply(b, {100.0f, 0.0f}) == 0);
}

TEST_CASE("terrain editor: strokes save, re-bake and round-trip through the cook; undo and redo; the ray finds the ground") {
    EditorSession session;
    const std::string root = open_demo(session, "terrain_brush");
    REQUIRE(session.write_asset("materials/grass.mcmat",
                                std::string(R"({ "format": "mce-material", "version": 1, "guid": ")") + kGrass + R"(", "base_color": [0.2, 0.5, 0.1, 1] })"));
    REQUIRE(session.write_asset("materials/rock.mcmat",
                                std::string(R"({ "format": "mce-material", "version": 1, "guid": ")") + kRock + R"(", "base_color": [0.5, 0.5, 0.5, 1] })"));
    REQUIRE(session.write_asset("terrain/valley.mcterrain", source()));
    REQUIRE(session.project().find(guid(kTerrain)));

    EditorScene& s = session.scene();
    const EntityId e = s.create_entity("Ground");
    LocalTransform t;
    t.position = {10.0f, -1.0f, 0.0f};
    t.scale = {1.0f, 2.0f, 1.0f};
    s.set(e, t);
    s.add_component(e, refl::type_of<kit::Terrain3D>());
    kit::Terrain3D terrain = *s.get<kit::Terrain3D>(e);
    terrain.terrain.guid = guid(kTerrain);
    s.set(e, terrain);

    TerrainEditor ed;
    CHECK_FALSE(ed.bind(session, kNoEntity));
    REQUIRE(ed.bind(session, e));
    const TerrainDocument& d = ed.doc();
    CHECK(d.resolution == 65);
    CHECK(d.layer_count == 2);
    CHECK(d.height(10, 10) == doctest::Approx(4.0f).epsilon(1e-3));
    CHECK(d.weight(10, 10, 0) == 255);  // the auto rule, as the cook made it

    // The viewport's ray: straight down onto the flat ground at 4 m x 2 - 1 = 7 m.
    math::vec3 hit;
    math::vec2 local;
    REQUIRE(ed.raycast(s, {13.0f, 50.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, hit, local));
    CHECK(hit.y == doctest::Approx(7.0f).epsilon(1e-3));
    CHECK(local.x == doctest::Approx(3.0f).epsilon(1e-3));
    CHECK(local.y == doctest::Approx(5.0f).epsilon(1e-3));
    REQUIRE(ed.raycast(s, {-40.0f, 30.0f, 0.0f}, math::normalize(math::vec3{1.0f, -0.5f, 0.0f}), hit, local));
    CHECK(hit.y == doctest::Approx(7.0f).epsilon(1e-3));
    CHECK(hit.x == doctest::Approx(6.0f).epsilon(1e-2));  // 30 - 7 = 23 m down at 2:1
    CHECK_FALSE(ed.raycast(s, {100.0f, 50.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, hit, local));  // beside it
    CHECK_FALSE(ed.raycast(s, {13.0f, 50.0f, 5.0f}, {0.0f, 1.0f, 0.0f}, hit, local));    // away from it
    Array<math::vec3> rim;
    ed.brush_outline(s, {0.0f, 0.0f}, 2.0f, rim);
    CHECK(rim.size() == 49);
    CHECK(rim[0].y == doctest::Approx(7.1f).epsilon(1e-2));

    // Stroke 1: raise a hill (several dabs, one flush mid-stroke, one undo step).
    ed.brush.op = TerrainBrushOp::Raise;
    ed.brush.radius = 6.0f;
    ed.brush.strength = 2.0f;
    ed.begin_stroke();
    CHECK(ed.dab({0.0f, 0.0f}) > 0);
    REQUIRE(ed.flush(session));
    CHECK(mounted(session).heights[32 * 65 + 32] == doctest::Approx(6.0f).epsilon(1e-3));
    CHECK(ed.dab({0.0f, 0.0f}) > 0);
    Result<f64> took = ed.end_stroke(session);
    REQUIRE(took);
    MESSAGE("terrain stroke save + re-bake: " << took.value() * 1000.0 << " ms");
    CHECK(mounted(session).heights[32 * 65 + 32] == doctest::Approx(8.0f).epsilon(1e-3));
    CHECK(mounted(session).heights[10 * 65 + 10] == doctest::Approx(4.0f).epsilon(1e-3));

    // Stroke 2: paint rock; stroke 3: cut a hole.
    ed.brush.op = TerrainBrushOp::Paint;
    ed.brush.layer = 1;
    ed.brush.strength = 1.0f;
    ed.brush.hardness = 1.0f;
    ed.begin_stroke();
    ed.dab({-20.0f, -20.0f});
    REQUIRE(ed.end_stroke(session));
    CHECK(mounted(session).layer1[12 * 65 + 12] == 255);
    CHECK(mounted(session).layer1[32 * 65 + 32] == 0);
    ed.brush.op = TerrainBrushOp::Hole;
    ed.brush.radius = 1.5f;
    ed.begin_stroke();
    ed.dab({20.0f, 20.0f});
    REQUIRE(ed.end_stroke(session));
    CHECK(mounted(session).holes == 9);
    CHECK((mounted(session).flags & asset::TerrainFlags::kHoles) != 0);

    // An empty stroke adds no undo step.
    ed.begin_stroke();
    ed.dab({500.0f, 0.0f});
    REQUIRE(ed.end_stroke(session));

    // The source now carries the data explicitly and keeps everything else as written.
    std::ifstream f(root + "/assets/terrain/valley.mcterrain");
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"heights\"") != std::string::npos);
    CHECK(text.find("\"splat\"") != std::string::npos);
    CHECK(text.find("\"holes\"") != std::string::npos);
    CHECK(text.find("\"fill\"") == std::string::npos);
    CHECK(text.find("\"valley\"") != std::string::npos);
    CHECK(text.find("triplanar") != std::string::npos);
    CHECK(text.find(kRock) != std::string::npos);

    // Re-binding (a fresh read of source + cooked) gives the same document: the round trip holds.
    TerrainEditor again;
    REQUIRE(again.bind(session, e));
    for (usize i = 0; i < d.heights.size(); ++i) REQUIRE(again.doc().heights[i] == doctest::Approx(d.heights[i]).epsilon(1e-3));
    CHECK(again.doc().weights.size() == d.weights.size());
    bool same = true;
    for (usize i = 0; i < d.weights.size(); ++i) same = same && again.doc().weights[i] == d.weights[i];
    CHECK(same);
    CHECK(again.doc().hole(52, 52));

    // Undo the hole, the paint and the hill; redo them all.
    REQUIRE(ed.undo(session));
    CHECK(mounted(session).holes == 0);
    CHECK((mounted(session).flags & asset::TerrainFlags::kHoles) == 0);
    REQUIRE(ed.undo(session));
    CHECK(mounted(session).layer1[12 * 65 + 12] == 0);
    REQUIRE(ed.undo(session));
    CHECK(mounted(session).heights[32 * 65 + 32] == doctest::Approx(4.0f).epsilon(1e-3));
    CHECK_FALSE(ed.can_undo());
    REQUIRE(ed.redo(session));
    REQUIRE(ed.redo(session));
    REQUIRE(ed.redo(session));
    CHECK_FALSE(ed.can_redo());
    const Mounted m = mounted(session);
    CHECK(m.heights[32 * 65 + 32] == doctest::Approx(8.0f).epsilon(1e-3));
    CHECK(m.layer1[12 * 65 + 12] == 255);
    CHECK(m.holes == 9);
    CHECK(session.cook().diags.error_count() == 0);
}
