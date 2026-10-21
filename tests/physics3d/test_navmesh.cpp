// NavMesh3D over Recast & Detour (ADR-0040; M-GAMEPLAY3D step 1): the bake (byte-identical run to
// run, areas, links, storeys), path and surface queries, carved obstacles and the crowd.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/hash.h"
#include "mce/physics3d/navmesh3d.h"

using namespace mce;
using namespace mce::physics;

namespace {

math::mat4 at(vec3 p) { return math::translation(p); }

// A 40 x 40 floor whose top is y = 0, and (optionally) a wall across x = 0 from z = -20 to z = 10,
// so the way from the left half to the right half goes around its end at z = 10.
NavGeometry3D level(bool wall = true) {
    NavGeometry3D g;
    g.add_box(at({0.0f, -0.5f, 0.0f}), {20.0f, 0.5f, 20.0f});
    if (wall) g.add_box(at({0.0f, 1.0f, -5.0f}), {0.5f, 1.0f, 15.0f}, kNavAreaBlocked);
    return g;
}

NavBakeSettings3D settings() {
    NavBakeSettings3D s;
    s.cell_size = 0.25f;
    s.tile_size = 32;
    return s;
}

NavMesh3D load(const NavGeometry3D& g, u32 agents = 16) {
    Array<byte> blob;
    REQUIRE(bake_navmesh(g, settings(), blob));
    NavMesh3D nav;
    REQUIRE(nav.load(blob.span(), agents));
    return nav;
}

f32 path_length(const Array<vec3>& p) {
    f32 len = 0.0f;
    for (usize i = 1; i < p.size(); ++i) {
        const vec3 d = p[i] - p[i - 1];
        len += std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    }
    return len;
}

f32 max_z(const Array<vec3>& p) {
    f32 z = -1e9f;
    for (const vec3& v : p) z = v.z > z ? v.z : z;
    return z;
}

}  // namespace

namespace {

// A blob whose trailing hash is made right again after an edit: damage that is not an accident.
void reseal(Array<byte>& blob) {
    const u64 h = fnv1a64(blob.data(), blob.size() - 8);
    std::memcpy(blob.data() + blob.size() - 8, &h, 8);
}

// Offset of the first tile layer's header (Detour's 'DTLR' magic, little-endian) inside a bake.
usize first_layer(const Array<byte>& blob) {
    const byte magic[4] = {byte('R'), byte('L'), byte('T'), byte('D')};
    for (usize i = 8; i + 4 <= blob.size(); ++i)
        if (std::memcmp(blob.data() + i, magic, 4) == 0) return i;
    return 0;
}

// Decodes PackBits (the bake's layer compression) after Detour's layer header.
bool unpack(const byte* in, usize size, Array<byte>& out) {
    usize i = 0;
    while (i < size) {
        const unsigned c = static_cast<unsigned>(in[i++]);
        if (c < 128) {
            if (i + c + 1 > size) return false;
            for (unsigned k = 0; k <= c; ++k) out.push_back(in[i + k]);
            i += c + 1;
        } else if (c > 128) {
            if (i >= size) return false;
            for (unsigned k = 0; k < 257 - c; ++k) out.push_back(in[i]);
            ++i;
        }
    }
    return true;
}

// The blob with its first layer's grid replaced by `edit(grid)` (heights, areas, cons, regs; width x height each),
// stored as literal PackBits runs, and the hash made right again.
Array<byte> with_edited_layer(const Array<byte>& blob, void (*edit)(Array<byte>& grid, unsigned w, unsigned h)) {
    constexpr usize kLayerHeader = 56;  // the tile layer header, aligned to 4
    const usize at = first_layer(blob);
    u32 old_size = 0;
    std::memcpy(&old_size, blob.data() + at - 4, 4);
    Array<byte> grid;
    REQUIRE(unpack(blob.data() + at + kLayerHeader, old_size - kLayerHeader, grid));
    const unsigned w = static_cast<unsigned>(blob[at + 48]), h = static_cast<unsigned>(blob[at + 49]);
    REQUIRE(grid.size() >= usize(w) * h * 3);  // heights, areas, cons (the regions are rebuilt at load)
    edit(grid, w, h);
    Array<byte> layer;
    for (usize i = 0; i < kLayerHeader; ++i) layer.push_back(blob[at + i]);
    for (usize i = 0; i < grid.size();) {
        const usize n = grid.size() - i < 128 ? grid.size() - i : 128;
        layer.push_back(static_cast<byte>(n - 1));
        for (usize k = 0; k < n; ++k) layer.push_back(grid[i + k]);
        i += n;
    }
    const usize old_padded = (old_size + 3) & ~usize(3);
    Array<byte> out;
    for (usize i = 0; i < at - 4; ++i) out.push_back(blob[i]);
    const u32 size = static_cast<u32>(layer.size());
    for (usize i = 0; i < 4; ++i) out.push_back(static_cast<byte>((size >> (8 * i)) & 0xFF));
    for (byte b : layer) out.push_back(b);
    while (out.size() % 4) out.push_back(byte{0});
    for (usize i = at + old_padded; i < blob.size(); ++i) out.push_back(blob[i]);
    reseal(out);
    return out;
}

// The bytes of a version-1 file: the same blob without the trailing hash, marked version 1.
Array<byte> as_version_1(const Array<byte>& blob) {
    Array<byte> out;
    REQUIRE(blob.size() > 8);
    for (usize i = 0; i + 8 < blob.size(); ++i) out.push_back(blob[i]);
    out[4] = byte{1};
    return out;
}

void refused(const Array<byte>& blob) {
    CHECK_FALSE(navmesh_blob_valid(blob.span()));
    NavMesh3D nav;
    CHECK_FALSE(nav.load(blob.span()));
    CHECK_FALSE(nav.loaded());
}

}  // namespace

TEST_CASE("navmesh3d: a damaged bake is refused, not handed to Detour (M-FUZZ 2b)") {
    // Found by test_fuzz: a mutated layer byte passed the header and size checks, and Detour's contour
    // builder then divided by zero. The trailing hash catches damage; the layer checks catch an edit that was
    // hashed again.
    Array<byte> a;
    REQUIRE(bake_navmesh(level(), settings(), a));
    REQUIRE(navmesh_blob_valid(a.span()));
    const usize layer_bytes_from = a.size() / 2;  // well inside the compressed tile layers
    for (usize at : {layer_bytes_from, layer_bytes_from + 1, a.size() - 9, a.size() - 8, a.size() - 1, usize(60)}) {
        Array<byte> bad = a;
        bad[at] = static_cast<byte>(static_cast<unsigned>(bad[at]) ^ 0x5Au);
        refused(bad);
    }
    // A version-1 file (no trailer) is refused too: re-bake it.
    refused(as_version_1(a));

    // A layer edit with the hash made right again must still be refused.
    const usize L = first_layer(a);
    REQUIRE(L > 8);
    const auto patched = [&](usize off, byte v) {
        Array<byte> b = a;
        b[L + off] = v;
        reseal(b);
        return b;
    };
    refused(patched(0, byte{0}));    // magic
    refused(patched(4, byte{9}));    // version
    refused(patched(8, byte{200}));  // tx outside the tile grid
    refused(patched(12, byte{200}));  // ty
    refused(patched(48, byte{0}));   // width 0
    refused(patched(51, byte{255}));  // maxx beyond the width

    // The grid itself: a connection bit toward a cell outside the layer (the contour walker reads past its
    // region array), and an area beyond the ones the engine has.
    CHECK(navmesh_blob_valid(with_edited_layer(a, [](Array<byte>&, unsigned, unsigned) {}).span()));  // the helper is faithful
    refused(with_edited_layer(a, [](Array<byte>& g, unsigned w, unsigned h) {
        g[usize(w) * h * 2] = byte{0x01};  // cons of cell (0, 0): connected toward x - 1
    }));
    refused(with_edited_layer(a, [](Array<byte>& g, unsigned w, unsigned h) {
        g[usize(w) * h + usize(w) * h / 2] = byte{200};  // an area id
    }));
    // A connection nobody reciprocates, and one that joins a cell to a non-walkable one: the contour walk
    // follows them while the region builder ignores them.
    refused(with_edited_layer(a, [](Array<byte>& g, unsigned w, unsigned h) {
        const usize n = usize(w) * h;
        for (usize i = 0; i < n; ++i) {
            if (!(static_cast<unsigned>(g[2 * n + i]) & 1u) || i % w == 0) continue;  // connected toward x - 1
            g[2 * n + i - 1] = static_cast<byte>(static_cast<unsigned>(g[2 * n + i - 1]) & ~4u);  // its answer
            return;
        }
    }));
    refused(with_edited_layer(a, [](Array<byte>& g, unsigned w, unsigned h) {
        const usize n = usize(w) * h;
        for (usize i = 0; i < n; ++i) {
            if (!(static_cast<unsigned>(g[2 * n + i]) & 1u) || i % w == 0) continue;
            g[n + i - 1] = byte{0};  // the neighbour is no longer walkable
            return;
        }
    }));
}

TEST_CASE("navmesh3d: the committed sample bake loads, and a version-1 file asks for a re-bake") {
    Array<byte> blob;
    {
        std::FILE* f = std::fopen(MCE_NAV_SAMPLE, "rb");
        REQUIRE(f);
        byte buf[4096];
        usize n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            for (usize i = 0; i < n; ++i) blob.push_back(buf[i]);
        std::fclose(f);
    }
    NavMesh3D nav;
    REQUIRE(nav.load(blob.span()));
    CHECK(nav.loaded());
    CHECK_FALSE(nav.load(as_version_1(blob).span()));
    CHECK_FALSE(nav.loaded());
}

TEST_CASE("navmesh3d: the bake is byte-identical run to run and validates") {
    const NavGeometry3D g = level();
    Array<byte> a, b;
    NavBakeStats3D st;
    REQUIRE(bake_navmesh(g, settings(), a, &st));
    REQUIRE(bake_navmesh(g, settings(), b));
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
    CHECK(st.bytes == a.size());
    CHECK(st.triangles == 24);
    CHECK(st.tiles_x * st.tiles_z >= 25);
    CHECK(st.layers > 0);
    CHECK(navmesh_blob_valid(a.span()));

    Array<byte> cut = a;
    cut.resize(cut.size() - 7);
    CHECK_FALSE(navmesh_blob_valid(cut.span()));
    NavMesh3D nav;
    CHECK_FALSE(nav.load(cut.span()));
    CHECK_FALSE(nav.loaded());
    cut = a;
    cut[0] = byte('X');
    CHECK_FALSE(navmesh_blob_valid(cut.span()));

    // Bad input fails cleanly.
    Array<byte> out;
    CHECK_FALSE(bake_navmesh(NavGeometry3D{}, settings(), out));
    NavBakeSettings3D bad = settings();
    bad.cell_size = 0.0f;
    CHECK_FALSE(bake_navmesh(g, bad, out));
    bad = settings();
    bad.cell_size = 0.01f;  // 4000 cells across: far more than 4096 tiles of 32
    bad.tile_size = 8;
    CHECK_FALSE(bake_navmesh(g, bad, out));
    CHECK(out.empty());
}

TEST_CASE("navmesh3d: paths go around walls and report partial goals") {
    NavMesh3D nav = load(level());
    CHECK(nav.loaded());
    const NavMesh3D::Stats s = nav.stats();
    CHECK(s.tiles > 0);
    CHECK(s.polys > 0);

    Array<vec3> path;
    REQUIRE(nav.find_path({-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, path) == NavPath3D::Complete);
    REQUIRE(path.size() >= 3);
    CHECK(path[0].x == doctest::Approx(-10.0f).epsilon(0.02));
    CHECK(path.back().x == doctest::Approx(10.0f).epsilon(0.02));
    CHECK(max_z(path) > 10.0f);         // around the wall's end
    CHECK(path_length(path) > 20.0f);

    // Snapping and the surface.
    const auto p = nav.nearest_point({3.0f, 1.0f, 3.0f});
    REQUIRE(p);
    CHECK(std::fabs(p->y) < 0.3f);
    CHECK_FALSE(nav.nearest_point({100.0f, 0.0f, 100.0f}));
    CHECK(nav.area_at({5.0f, 0.0f, 5.0f}) == kNavAreaGround);

    // The surface raycast stops at the wall, one agent radius short of it.
    const auto hit = nav.raycast({-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f});
    REQUIRE(hit);
    CHECK(hit->x == doctest::Approx(-0.9f).epsilon(0.1));
    CHECK_FALSE(nav.raycast({-10.0f, 0.0f, 15.0f}, {10.0f, 0.0f, 15.0f}));

    // Off the mesh: no path.
    CHECK(nav.find_path({-10.0f, 0.0f, 0.0f}, {300.0f, 0.0f, 0.0f}, path) == NavPath3D::None);
    CHECK(path.empty());
}

TEST_CASE("navmesh3d: area volumes set path costs and blocked volumes cut the mesh") {
    NavGeometry3D g = level(false);
    NavAreaVolume3D mud;
    mud.center = {0.0f, 0.0f, -7.0f};
    mud.half = {2.0f, 1.0f, 13.0f};  // x -2..2, z -20..6
    mud.area = 2;
    g.volumes.push_back(mud);
    NavMesh3D nav = load(g);
    CHECK(nav.area_at({0.0f, 0.0f, -5.0f}) == 2);
    CHECK(nav.area_at({0.0f, 0.0f, 10.0f}) == kNavAreaGround);

    Array<vec3> path;
    REQUIRE(nav.find_path({-10.0f, 0.0f, -5.0f}, {10.0f, 0.0f, -5.0f}, path) == NavPath3D::Complete);
    CHECK(path.size() == 2);  // straight through at cost 1
    CHECK(max_z(path) < -4.0f);

    nav.set_area_cost(2, 10.0f);
    CHECK(nav.area_cost(2) == doctest::Approx(10.0f));
    REQUIRE(nav.find_path({-10.0f, 0.0f, -5.0f}, {10.0f, 0.0f, -5.0f}, path) == NavPath3D::Complete);
    CHECK(max_z(path) > 6.0f);  // around the mud

    // A blocked volume across the whole floor splits it: the far side is unreachable.
    NavGeometry3D cut = level(false);
    NavAreaVolume3D gap;
    gap.center = {0.0f, 0.0f, 0.0f};
    gap.half = {1.0f, 1.0f, 25.0f};
    gap.area = kNavAreaBlocked;
    cut.volumes.push_back(gap);
    NavMesh3D split = load(cut);
    REQUIRE(split.find_path({-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, path) == NavPath3D::Partial);
    CHECK(path.back().x < 0.0f);
}

TEST_CASE("navmesh3d: off-mesh links join separate floors") {
    NavGeometry3D g;
    g.add_box(at({-11.5f, -0.5f, 0.0f}), {10.0f, 0.5f, 10.0f});  // x -21.5 .. -1.5
    g.add_box(at({11.5f, -0.5f, 0.0f}), {10.0f, 0.5f, 10.0f});   // x 1.5 .. 21.5
    Array<vec3> path;
    {
        NavMesh3D nav = load(g);
        CHECK(nav.find_path({-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, path) == NavPath3D::Partial);
    }
    NavLinkDesc3D jump;
    jump.start = {-2.3f, 0.0f, 0.0f};
    jump.end = {2.3f, 0.0f, 0.0f};
    jump.user_id = 7;
    g.links.push_back(jump);
    NavMesh3D nav = load(g);
    CHECK(nav.stats().links == 1);
    REQUIRE(nav.find_path({-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, path) == NavPath3D::Complete);
    bool crosses = false;
    for (usize i = 1; i < path.size(); ++i) crosses |= path[i - 1].x < -1.5f && path[i].x > 1.5f;
    CHECK(crosses);

    Array<vec3> tris, links;
    Array<u8> areas;
    nav.debug_triangles(tris, areas, 0.02f, &links);
    CHECK(tris.size() == areas.size() * 3);
    CHECK(links.size() == 2);
}

TEST_CASE("navmesh3d: stacked floors bake as layers of one tile") {
    NavGeometry3D g;
    g.add_box(at({0.0f, -0.5f, 0.0f}), {4.0f, 0.5f, 4.0f});
    g.add_box(at({0.0f, 3.5f, 0.0f}), {4.0f, 0.5f, 4.0f});  // a deck 4 m up
    Array<byte> blob;
    NavBakeStats3D st;
    REQUIRE(bake_navmesh(g, settings(), blob, &st));
    CHECK(st.layers > st.tiles_x * st.tiles_z);
    NavMesh3D nav;
    REQUIRE(nav.load(blob.span()));
    const auto top = nav.nearest_point({0.0f, 4.2f, 0.0f}, {1.0f, 1.0f, 1.0f});
    REQUIRE(top);
    CHECK(top->y == doctest::Approx(4.0f).epsilon(0.05));
}

TEST_CASE("navmesh3d: obstacles carve the mesh until removed") {
    NavMesh3D nav = load(level());
    Array<vec3> path;
    REQUIRE(nav.find_path({-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, path) == NavPath3D::Complete);

    // Close the way around the wall's end.
    const u32 id = nav.add_obstacle({0.0f, 1.0f, 15.0f}, {0.6f, 1.0f, 5.5f});
    REQUIRE(id != 0);
    CHECK(nav.obstacle_count() == 1);
    nav.update(1.0f / 60.0f);
    CHECK(nav.find_path({-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, path) == NavPath3D::Partial);

    nav.remove_obstacle(id);
    nav.remove_obstacle(id);  // stale: no-op
    CHECK(nav.obstacle_count() == 0);
    nav.update(1.0f / 60.0f);
    CHECK(nav.find_path({-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, path) == NavPath3D::Complete);

    // A turned obstacle carves too.
    const u32 turned = nav.add_obstacle({0.0f, 1.0f, 15.0f}, {0.6f, 1.0f, 6.0f}, 0.1f);
    REQUIRE(turned != 0);
    nav.update(1.0f / 60.0f);
    CHECK(nav.find_path({-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, path) == NavPath3D::Partial);
}

namespace {

// Four agents cross from the left half to the right; returns the crowd hash after `ticks`.
u64 run_crowd(NavMesh3D& nav, u32 ticks, Array<i32>& ids) {
    ids.clear();
    for (u32 i = 0; i < 4; ++i) {
        const i32 a = nav.add_agent({-10.0f, 0.0f, -6.0f + f32(i) * 1.5f});
        REQUIRE(a >= 0);
        CHECK(nav.set_agent_target(a, {10.0f, 0.0f, -6.0f + f32(i) * 1.5f}));
        ids.push_back(a);
    }
    for (u32 t = 0; t < ticks; ++t) nav.update(1.0f / 60.0f);
    return nav.hash_state(0);
}

}  // namespace

TEST_CASE("navmesh3d: crowd agents reach their targets, avoid each other, and replay identically") {
    NavMesh3D nav = load(level());
    Array<i32> ids;
    const u64 h1 = run_crowd(nav, 60 * 30, ids);
    CHECK(nav.agent_count() == 4);
    for (const i32 a : ids) {
        NavAgentStatus3D st;
        REQUIRE(nav.agent_status(a, st));
        CHECK(st.state == NavAgentState3D::Arrived);
        CHECK(st.position.x > 9.0f);
        CHECK(st.distance < 0.9f);
    }
    for (usize i = 0; i < ids.size(); ++i)
        for (usize j = i + 1; j < ids.size(); ++j) {
            NavAgentStatus3D a, b;
            nav.agent_status(ids[i], a);
            nav.agent_status(ids[j], b);
            const vec3 d = a.position - b.position;
            CHECK(d.x * d.x + d.z * d.z > 0.5f * 0.5f);  // not on top of each other
        }

    NavMesh3D again = load(level());
    Array<i32> ids2;
    CHECK(run_crowd(again, 60 * 30, ids2) == h1);

    // Mid-route state, an unreachable target, a stop and a removal.
    NavMesh3D mid = load(level());
    const i32 a = mid.add_agent({-10.0f, 0.0f, 0.0f});
    REQUIRE(mid.set_agent_target(a, {10.0f, 0.0f, 0.0f}));
    for (u32 t = 0; t < 30; ++t) mid.update(1.0f / 60.0f);
    NavAgentStatus3D st;
    REQUIRE(mid.agent_status(a, st));
    CHECK(st.state == NavAgentState3D::Moving);
    CHECK(st.velocity.x * st.velocity.x + st.velocity.z * st.velocity.z > 0.1f);
    Array<vec3> corners;
    mid.agent_corners(a, corners);
    CHECK_FALSE(corners.empty());

    CHECK_FALSE(mid.set_agent_target(a, {300.0f, 0.0f, 0.0f}));
    REQUIRE(mid.agent_status(a, st));
    CHECK(st.state == NavAgentState3D::NoPath);

    mid.stop_agent(a);
    REQUIRE(mid.agent_status(a, st));
    CHECK(st.state == NavAgentState3D::Idle);

    // A teleport onto the far side, then a short walk.
    mid.move_agent(a, {12.0f, 0.0f, 0.0f});
    REQUIRE(mid.agent_status(a, st));
    CHECK(st.position.x == doctest::Approx(12.0f).epsilon(0.01));
    REQUIRE(mid.set_agent_target(a, {14.0f, 0.0f, 0.0f}));
    for (u32 t = 0; t < 120; ++t) mid.update(1.0f / 60.0f);
    REQUIRE(mid.agent_status(a, st));
    CHECK(st.state == NavAgentState3D::Arrived);

    mid.remove_agent(a);
    CHECK(mid.agent_count() == 0);
    CHECK_FALSE(mid.agent_status(a, st));
}
