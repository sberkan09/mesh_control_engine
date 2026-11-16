// SignalHead (M-SIGNALHEADS 56b): an approach reads the light of the lane that runs to the junction, a lamp is lit exactly
// while its colour is the light, the head writes only its own MeshRef tint and emissive tint, and heads without a lane,
// network or `enabled` are left as authored.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/asset/container.h"
#include "mce/cook/cook.h"
#include "mce/cook/importer.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/roads.h"
#include "mce/kit/signal_head.h"

using namespace mce;
using namespace mce::kit;
using math::vec3;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const asset::Guid kNet{0x7e7a000000004000ull, 0x80000000000000c2ull};

// The crossroads of test_roads: east-west green 20 s, then north-south 10 s, amber 3, all red 1.
constexpr const char* kCross = R"({ "format": "mce-roads", "version": 1, "name": "cross",
  "nodes": [{"name": "c", "at": [0, 0, 0]}, {"name": "n", "at": [0, 0, -60]}, {"name": "s", "at": [0, 0, 60]},
            {"name": "e", "at": [60, 0, 0]}, {"name": "w", "at": [-60, 0, 0]}],
  "roads": [{"name": "north", "from": "c", "to": "n", "lanes": 1}, {"name": "south", "from": "c", "to": "s", "lanes": 1},
            {"name": "east", "from": "c", "to": "e", "lanes": [2, 2]}, {"name": "west", "from": "c", "to": "w", "lanes": [2, 2]}],
  "signals": [{"node": "c", "amber": 3, "all_red": 1,
               "phases": [{"green": ["east", "west"], "seconds": 20}, {"green": ["north", "south"], "seconds": 10}]}] })";

Array<byte> cook_roads(const char* text) {
    cook::ImporterRegistry importers;
    cook::ImporterRegistry::add_builtins(importers);
    const cook::IImporter* imp = importers.for_extension(".mcroads");
    REQUIRE(imp);
    cook::AssetRegistry registry;
    cook::DiagList diags;
    Array<byte> cooked;
    REQUIRE(cook::cook_document(registry, *imp, kNet, "net.mcroads", text, cooked, diags));
    auto view = asset::ContainerView::open(cooked.span(), "net.mcroads");
    REQUIRE(view);
    const u32 chunk = view.value().find(asset::ChunkType::RoadNetwork, kNet);
    REQUIRE(chunk < view.value().chunk_count());
    const Span<const byte> p = view.value().payload(chunk);
    Array<byte> out;
    out.resize(p.size());
    std::memcpy(out.data(), p.data(), p.size());
    return out;
}

struct Rig {
    World w;
    u64 t = 0;
    Entity net;
    Rig() {
        w.register_components<LocalTransform>();
        install_kit3d(w, nullptr);
        install_roads(w, nullptr);
        const Array<byte> payload = cook_roads(kCross);
        REQUIRE(w.resource<RoadRegistry>().define(kNet, payload.span()));
        RoadNetwork3D n;
        n.roads.guid = kNet;
        net = w.spawn_with(LocalTransform{{0, 0, 0}, math::quat::identity(), {1, 1, 1}}, n);
    }
    void run(u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) w.tick(kDt, t++);
    }
    Entity lamp(vec3 at, vec3 approach, RoadLight colour, u8 enabled = 1, Entity network = {}) {
        SignalHead h;
        h.approach = approach;
        h.lamp = colour;
        h.enabled = enabled;
        h.network = network;
        return w.spawn_with(LocalTransform{at, math::quat::identity(), {1, 1, 1}}, MeshRef{}, h);
    }
    const MeshRef& mesh(Entity e) { return *w.try_get<MeshRef>(e); }
};

bool same(const math::vec3& a, const math::vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool same(const math::vec4& a, const math::vec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

bool lit(const MeshRef& m) { return m.emissive_tint.x > 1.0f || m.emissive_tint.y > 1.0f; }

// Traffic arriving from the east heads -x on the east arm; from the south it heads -z on the south arm.
const vec3 kEastAt{30.0f, 0.0f, -2.0f}, kEastDir{-1.0f, 0.0f, 0.0f};
const vec3 kSouthAt{2.0f, 0.0f, 30.0f}, kSouthDir{0.0f, 0.0f, -1.0f};

}  // namespace

TEST_CASE("signal head: the lit lamp follows the light of its approach") {
    Rig r;
    const Entity er = r.lamp(kEastAt, kEastDir, RoadLight::Red), ea = r.lamp(kEastAt, kEastDir, RoadLight::Amber),
                 eg = r.lamp(kEastAt, kEastDir, RoadLight::Green);
    const Entity sr = r.lamp(kSouthAt, kSouthDir, RoadLight::Red), sg = r.lamp(kSouthAt, kSouthDir, RoadLight::Green);
    r.run(1);  // 1/60 s: east-west green, north-south red
    CHECK_FALSE(lit(r.mesh(er)));
    CHECK_FALSE(lit(r.mesh(ea)));
    CHECK(lit(r.mesh(eg)));
    CHECK(lit(r.mesh(sr)));
    CHECK_FALSE(lit(r.mesh(sg)));
    r.run(60 * 21 - 1);  // 21 s: east-west amber
    CHECK(lit(r.mesh(ea)));
    CHECK_FALSE(lit(r.mesh(eg)));
    CHECK_FALSE(lit(r.mesh(er)));
    CHECK(lit(r.mesh(sr)));
    r.run(60 * 9);  // 30 s: north-south green, east-west red
    CHECK(lit(r.mesh(er)));
    CHECK_FALSE(lit(r.mesh(eg)));
    CHECK(lit(r.mesh(sg)));
    CHECK_FALSE(lit(r.mesh(sr)));
}

TEST_CASE("signal head: the look is a pure function and the lit lamp has its own colour") {
    const SignalLampLook a = signal_lamp_look(RoadLight::Red, RoadLight::Red);
    const SignalLampLook b = signal_lamp_look(RoadLight::Red, RoadLight::Red);
    CHECK(same(a.tint, b.tint));
    CHECK(same(a.emissive_tint, b.emissive_tint));
    CHECK(a.tint.x > a.tint.y);
    CHECK(signal_lamp_look(RoadLight::Green, RoadLight::Green).tint.y > signal_lamp_look(RoadLight::Green, RoadLight::Green).tint.x);
    CHECK(signal_lamp_look(RoadLight::Amber, RoadLight::Amber).emissive_tint.x > 1.0f);
    const SignalLampLook dark = signal_lamp_look(RoadLight::Red, RoadLight::Green);
    CHECK(dark.emissive_tint.x == 0.0f);
    CHECK(dark.emissive_tint.y == 0.0f);
    CHECK(dark.emissive_tint.z == 0.0f);
}

TEST_CASE("signal head: only the head's own tint and emissive tint change") {
    Rig r;
    const Entity e = r.lamp(kEastAt, kEastDir, RoadLight::Green);
    MeshRef* m = r.w.try_get_mut<MeshRef>(e);
    m->cast_shadows = 0;
    const LocalTransform before = *r.w.try_get<LocalTransform>(e);
    r.run(5);
    const LocalTransform after = *r.w.try_get<LocalTransform>(e);
    CHECK(same(before.position, after.position));
    CHECK(before.rotation.x == after.rotation.x);
    CHECK(before.rotation.y == after.rotation.y);
    CHECK(before.rotation.z == after.rotation.z);
    CHECK(before.rotation.w == after.rotation.w);
    CHECK(same(before.scale, after.scale));
    CHECK(r.mesh(e).cast_shadows == 0);
    CHECK(lit(r.mesh(e)));
}

TEST_CASE("signal head: a head with no lane, no network or enabled 0 is left as authored") {
    Rig r;
    const MeshRef plain{};
    const Entity far = r.lamp({500.0f, 0.0f, 500.0f}, kEastDir, RoadLight::Green);
    const Entity flat = r.lamp(kEastAt, {0.0f, 0.0f, 0.0f}, RoadLight::Green);
    const Entity off = r.lamp(kEastAt, kEastDir, RoadLight::Green, 0);
    const Entity gone = r.lamp(kEastAt, kEastDir, RoadLight::Green, 1, r.w.spawn_with(LocalTransform{}));  // no RoadNetwork3D
    r.run(3);
    for (const Entity e : {far, flat, off, gone}) {
        CHECK(same(r.mesh(e).tint, plain.tint));
        CHECK(same(r.mesh(e).emissive_tint, plain.emissive_tint));
    }
}

TEST_CASE("signal head: signal_approach_light names the light and says when there is no lane") {
    Rig r;
    RoadNet net;
    REQUIRE(road_net(r.w, r.net, net));
    RoadLight l;
    CHECK(signal_approach_light(net, kEastAt, kEastDir, l));
    CHECK(l == RoadLight::Green);
    CHECK(signal_approach_light(net, kSouthAt, kSouthDir, l));
    CHECK(l == RoadLight::Red);
    CHECK_FALSE(signal_approach_light(net, {500.0f, 0.0f, 500.0f}, kEastDir, l));
    CHECK(l == RoadLight::Green);
}
