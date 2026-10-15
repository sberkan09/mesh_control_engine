// NavMesh3D over Recast & Detour (ADR-0040; M-GAMEPLAY3D step 1): the bake (byte-identical run to
// run, areas, links, storeys), path and surface queries, carved obstacles and the crowd.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

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
