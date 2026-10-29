// Animation graphs, version 2 (M-GAMEPLAY3D step 5): the .mcanim extras round-trip through the
// cook and the CHNK_ANIMG extension; on a small rig, 2D blend spaces, override and additive layers
// on bone masks, root motion (the entity, a CharacterController3D, a turn), two-bone and look-at
// IK on IKTarget3D goals, foot placement on a step up and a step down, and a replayed hash.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "kit_mesh_tree.h"
#include "mce/cook/animgraph_text.h"
#include "mce/cook/cook.h"
#include "mce/cook/json.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/anim3d.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;
using kit_test::write_mesh_tree;
using math::quat;
using math::vec3;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const asset::Guid kRig{0x3d00000000000091ull, 0x8000000000000001ull};
constexpr const char* kGraphGuid = "9a270000-0000-4000-8000-0000000000a1";

// The rig, standing on y = 0: hips 1 m up, a spine and a head with a nose 0.2 m in front, two
// straight legs (thigh, knee, foot; 0.5 m bones) and a right arm hanging down (0.4 m bones).
enum Node : u32 { Root, Hips, Spine, Head, Nose, ThighL, KneeL, FootL, ThighR, KneeR, FootR, UpperR, ElbowR, HandR };

quat yaw(f32 deg) { return math::quat_from_axis_angle({0, 1, 0}, deg * math::kPi / 180.0f); }
quat pitch(f32 deg) { return math::quat_from_axis_angle({1, 0, 0}, deg * math::kPi / 180.0f); }

mesh_fixture::Builder::Track hips_at(std::vector<f32> times, std::vector<f32> values) {
    return {Hips, asset::AnimPath::Translation, std::move(times), std::move(values)};
}
mesh_fixture::Builder::Track turn_of(u32 node, std::vector<f32> times, std::initializer_list<quat> keys) {
    std::vector<f32> v;
    for (const quat& q : keys) v.insert(v.end(), {q.x, q.y, q.z, q.w});
    return {node, asset::AnimPath::Rotation, std::move(times), std::move(v)};
}

Array<byte> rig_blob() {
    mesh_fixture::Builder b;
    b.add_node(-1, {}, "root");
    b.add_node(Root, {0, 1, 0}, "hips");
    b.add_node(Hips, {0, 0.5f, 0}, "spine");
    b.add_node(Spine, {0, 0.5f, 0}, "head");
    b.add_node(Head, {0, 0, 0.2f}, "nose");
    b.add_node(Hips, {0.2f, 0, 0}, "thigh_l");
    b.add_node(ThighL, {0, -0.5f, 0}, "knee_l");
    b.add_node(KneeL, {0, -0.5f, 0}, "foot_l");
    b.add_node(Hips, {-0.2f, 0, 0}, "thigh_r");
    b.add_node(ThighR, {0, -0.5f, 0}, "knee_r");
    b.add_node(KneeR, {0, -0.5f, 0}, "foot_r");
    b.add_node(Spine, {-0.3f, 0.4f, 0}, "upper_r");
    b.add_node(UpperR, {0, -0.4f, 0}, "elbow_r");
    b.add_node(ElbowR, {0, -0.4f, 0}, "hand_r");
    mesh_fixture::Box box;
    box.half = {0.3f, 1.0f, 0.2f};
    box.center = {0, 1.0f, 0};
    b.add_box(box);
    b.add_clip("idle", 1.0f, {hips_at({0, 1}, {0, 1, 0, 0, 1, 0})});
    b.add_clip("pose_r", 1.0f, {hips_at({0, 1}, {1, 1, 0, 1, 1, 0})});
    b.add_clip("pose_l", 1.0f, {hips_at({0, 1}, {-1, 1, 0, -1, 1, 0})});
    b.add_clip("pose_f", 1.0f, {hips_at({0, 1}, {0, 1, 1, 0, 1, 1})});
    b.add_clip("walk", 1.0f, {hips_at({0, 1}, {0, 1, 0, 0, 1, 2})});
    b.add_clip("turn", 1.0f, {turn_of(Hips, {0, 0.5f, 1}, {yaw(0), yaw(45), yaw(90)})});
    b.add_clip("lift", 1.0f, {hips_at({0, 1}, {0, 2, 0, 0, 2, 0}), turn_of(Head, {0, 1}, {pitch(90), pitch(90)})});
    b.add_clip("bob", 1.0f, {hips_at({0, 1}, {0, 1, 0, 0, 1.5f, 0})});
    return b.build();
}

std::string graph(const std::string& body) {
    return std::string(R"({ "format": "mce-animgraph", "version": 2, "guid": ")") + kGraphGuid + R"(", "name": "rig", )" + body + "}";
}

struct Rig {
    asset::AssetSystem assets;
    World w;
    u64 t = 0;
    Entity e;

    Rig(const char* name, const std::string& graph_json, bool floor = false, bool hierarchy = false) {
        const Array<byte> blob = rig_blob();
        const std::string mesh_root = write_mesh_tree((std::string(name) + "_mesh").c_str(), kRig, blob.span());
        const std::string src = cook_test::fresh_dir(name);
        cook_test::write_file(src + "/assets/rig.mcanim", graph_json);
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(src.c_str()), cook::CookOptions{});
        REQUIRE(r.ok());
        const std::string cooked = src + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str()), StringView(mesh_root.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 2)));
        w.register_components<LocalTransform>();
        install_kit3d(w, &assets);
        install_bodies3d(w, &assets);
        if (hierarchy) ecs::install_hierarchy(w);  // after the kit, as scene::load installs it
        if (floor) {
            BoxCollider3D f;
            f.half = {20.0f, 0.5f, 20.0f};
            w.spawn_with(LocalTransform{{0, -0.5f, 0}, quat::identity(), {1, 1, 1}}, f);
        }
        MeshRef m;
        m.mesh.guid = kRig;
        Animator a;
        a.graph.guid = asset::parse_guid(kGraphGuid).value();
        e = w.spawn_with(LocalTransform{{}, quat::identity(), {1, 1, 1}}, m, a);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Animator& anim() { return *w.try_get_mut<Animator>(e); }
    // An entity pinned to `bone`, so the test sees where the bone is drawn.
    Entity marker(const char* bone) {
        AttachToBone a;
        a.target = e;
        a.bone = bone;
        return w.spawn_with(LocalTransform{}, a);
    }
    vec3 pos(Entity x) { return w.try_get<LocalTransform>(x)->position; }
};

void check_near(vec3 a, vec3 b, f32 eps = 1e-3f) {
    CHECK(a.x == doctest::Approx(b.x).epsilon(eps).scale(1.0));
    CHECK(a.y == doctest::Approx(b.y).epsilon(eps).scale(1.0));
    CHECK(a.z == doctest::Approx(b.z).epsilon(eps).scale(1.0));
}

}  // namespace

TEST_CASE("anim3d: version 2 graphs round-trip through the text and the CHNK_ANIMG extension") {
    const std::string text = graph(R"("params": ["x", "y", "aim"],
        "states": [ { "name": "move", "blend2d": { "x": "x", "y": "y", "points": [
            { "clip": "idle", "at": [0, 0] }, { "clip": "pose_r", "at": [1, 0] }, { "clip": "pose_f", "at": [0, 1] } ] } } ],
        "root_motion": "hips",
        "layers": [ { "name": "lift", "clip": "lift", "mask": ["spine"], "param": "aim", "speed": 2 },
                    { "name": "bob", "clip": "bob", "additive": true, "weight": 0.5, "loop": false } ],
        "ik": [ { "name": "hand", "type": "two_bone", "bone": "hand_r", "pole": [0, 0, -1] },
                { "name": "head", "type": "look_at", "bone": "head", "limit": 45, "param": "aim" },
                { "name": "foot", "type": "foot", "bone": "foot_l", "pelvis": "hips", "reach": 0.3 } ])");
    cook::JsonDoc doc;
    cook::JsonError err;
    REQUIRE(doc.parse(StringView(text.c_str()), err));
    cook::AnimGraphDocument d;
    cook::DiagList diags;
    REQUIRE(cook::read_animgraph_document(doc.root(), "rig.mcanim", d, diags));
    CHECK(d.extended());
    REQUIRE(d.states[0].blend_2d);
    CHECK(d.states[0].blend_param_y == 1);
    CHECK(d.states[0].points2d[2].y == 1.0f);
    CHECK(d.root_motion.view() == StringView("hips"));
    REQUIRE(d.layers.size() == 2);
    CHECK(d.layers[0].weight_param == 2);
    CHECK(d.layers[1].additive);
    REQUIRE(d.ik.size() == 3);
    CHECK(d.ik[0].axis[2] == -1.0f);
    CHECK(d.ik[1].axis[2] == 1.0f);  // look_at's default axis
    CHECK(d.ik[2].kind == asset::AnimIKKind::Foot);

    // Written, read again: the same text and chunk; the chunk carries the extension.
    const asset::Guid guid = asset::parse_guid(kGraphGuid).value();
    cook::String a;
    cook::write_animgraph_document(d, guid, a);
    CHECK(std::strstr(a.c_str(), "\"version\": 2") != nullptr);
    cook::JsonDoc doc2;
    REQUIRE(doc2.parse(a.view(), err));
    cook::AnimGraphDocument back;
    REQUIRE(cook::read_animgraph_document(doc2.root(), "rig.mcanim", back, diags));
    cook::String b;
    cook::write_animgraph_document(back, guid, b);
    CHECK(std::string(b.c_str()) == std::string(a.c_str()));
    Array<byte> c1, c2;
    cook::build_animgraph_chunk(d, c1);
    cook::build_animgraph_chunk(back, c2);
    REQUIRE(c1.size() == c2.size());
    CHECK(std::memcmp(c1.data(), c2.data(), c1.size()) == 0);
    const Result<asset::AnimGraphView> v = asset::AnimGraphView::read(c1.span());
    REQUIRE(v);
    REQUIRE(v.value().extension);
    CHECK(v.value().root_bone() == StringView("hips"));
    CHECK(v.value().blend2d.size() == 3);
    CHECK(v.value().layers.size() == 2);
    CHECK(v.value().mask_bones.size() == 1);
    CHECK(v.value().ik.size() == 3);
    CHECK(v.value().states[0].blend_2d == 1);
    CHECK_FALSE(asset::AnimGraphView::read(c1.span().subspan(0, c1.size() - 4)));  // a cut chain

    // A graph without the extras is still version 1, with no extension.
    cook::AnimGraphDocument plain;
    plain.states.push_back(cook::AnimGraphDocState{});
    plain.states[0].name = "idle";
    plain.states[0].clip = "idle";
    CHECK_FALSE(plain.extended());
    cook::String pt;
    cook::write_animgraph_document(plain, guid, pt);
    CHECK(std::strstr(pt.c_str(), "\"version\": 1") != nullptr);
    Array<byte> pc;
    cook::build_animgraph_chunk(plain, pc);
    CHECK(pc.size() == sizeof(asset::AnimGraphHeader) + sizeof(asset::AnimGraphState));
    REQUIRE(asset::AnimGraphView::read(pc.span()));
    CHECK(asset::AnimGraphView::read(pc.span()).value().extension == nullptr);

    // Refusals.
    const char* bad[] = {
        R"("params": ["x", "y"], "states": [ { "name": "m", "blend2d": { "x": "x", "y": "y", "points": [
            { "clip": "a", "at": [0, 0] }, { "clip": "b", "at": [1, 0] } ] } } ])",
        R"("params": ["x"], "states": [ { "name": "m", "blend2d": { "x": "x", "y": "x", "points": [
            { "clip": "a", "at": [0, 0] }, { "clip": "b", "at": [1, 0] }, { "clip": "c", "at": [0, 1] } ] } } ])",
        R"("states": [ { "name": "m", "clip": "a" } ], "layers": [ { "name": "l", "clip": "a", "param": "nope" } ])",
        R"("states": [ { "name": "m", "clip": "a" } ], "ik": [ { "name": "k", "type": "fk", "bone": "b" } ])",
        R"("states": [ { "name": "m", "clip": "a" } ], "ik": [ { "name": "k", "type": "look_at", "bone": "b", "pole": [0, 0, 1] } ])",
        R"("states": [ { "name": "m", "clip": "a" } ], "ik": [ { "name": "k", "type": "two_bone", "bone": "b", "reach": 1 } ])",
        R"("states": [ { "name": "m", "clip": "a" } ], "layers": [ { "name": "a", "clip": "a" }, { "name": "b", "clip": "a" },
            { "name": "c", "clip": "a" }, { "name": "d", "clip": "a" }, { "name": "e", "clip": "a" } ])",
        R"("states": [ { "name": "m", "clip": "a" } ], "layers": [ { "name": "a", "clip": "a", "weight": 2 } ])",
    };
    for (const char* body : bad) {
        const std::string t = graph(body);
        cook::JsonDoc bd;
        REQUIRE(bd.parse(StringView(t.c_str()), err));
        cook::AnimGraphDocument out;
        cook::DiagList bdiags;
        CHECK_FALSE(cook::read_animgraph_document(bd.root(), "bad.mcanim", out, bdiags));
        CHECK(bdiags.error_count() > 0);
    }
}

TEST_CASE("anim3d: a 2D blend space mixes the clips around the parameters' point") {
    Rig r("anim3d_blend2d", graph(R"("params": ["x", "y"],
        "states": [ { "name": "move", "blend2d": { "x": "x", "y": "y", "points": [
            { "clip": "idle", "at": [0, 0] }, { "clip": "pose_r", "at": [1, 0] }, { "clip": "pose_l", "at": [-1, 0] },
            { "clip": "pose_f", "at": [0, 1] } ] } } ])"));
    const Entity hips = r.marker("hips");
    r.run(1);
    check_near(r.pos(hips), {0, 1, 0});
    CHECK(r.anim().playing == StringId("idle").value);

    r.anim().params = {1, 0, 0, 0};
    r.run(1);
    check_near(r.pos(hips), {1, 1, 0});
    CHECK(r.anim().playing == StringId("pose_r").value);

    r.anim().params = {0.5f, 0, 0, 0};
    r.run(1);
    check_near(r.pos(hips), {0.5f, 1, 0});

    // Between three points: the gradient bands give idle, pose_r and pose_f a third each.
    r.anim().params = {0.5f, 0.5f, 0, 0};
    r.run(1);
    check_near(r.pos(hips), {1.0f / 3.0f, 1, 1.0f / 3.0f});

    r.anim().params = {-1, 0, 0, 0};
    r.run(1);
    check_near(r.pos(hips), {-1, 1, 0});
    CHECK(r.anim().playing == StringId("pose_l").value);
}

TEST_CASE("anim3d: layers override their mask's bones or add their clip's motion") {
    Rig r("anim3d_layers", graph(R"("params": ["w"],
        "states": [ { "name": "stand", "clip": "idle" } ],
        "layers": [ { "name": "lift", "clip": "lift", "mask": ["spine"], "param": "w" },
                    { "name": "bob", "clip": "bob", "additive": true, "weight": 0.5 } ])"));
    const Entity hips = r.marker("hips");
    const Entity nose = r.marker("nose");
    r.run(1);
    // w = 0: the override layer is off; the additive one has barely started.
    CHECK(r.pos(nose).y == doctest::Approx(2.0f).epsilon(0.01));
    CHECK(r.pos(nose).z == doctest::Approx(0.2f).epsilon(0.01));

    // w = 1: the head (under the spine) pitches 90 degrees, the nose points down; the hips are
    // outside the mask, so the lift clip's hips stay where the state puts them.
    r.anim().params.x = 1.0f;
    r.run(1);
    check_near(r.pos(nose) - vec3{0, 0, 0}, {0, r.pos(hips).y + 1.0f - 0.2f, 0}, 0.01f);
    CHECK(r.pos(hips).y < 1.1f);

    // Half weight: half the pitch.
    r.anim().params.x = 0.5f;
    r.run(1);
    const f32 s45 = std::sqrt(0.5f) * 0.2f;
    CHECK(r.pos(nose).z == doctest::Approx(s45).epsilon(0.01));
    CHECK(r.pos(nose).y == doctest::Approx(r.pos(hips).y + 1.0f - s45).epsilon(0.01));

    // The extract poses the mesh through the same path.
    {
        render::MeshStore store;
        const Array<byte> blob = rig_blob();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());
        Kit3DExtract ex;
        ex.init(store, nullptr);
        ex.bind_mesh(kRig, 1);
        r.w.spawn_with(LocalTransform{{0, 1, 5}, quat::identity(), {1, 1, 1}}, Camera3DComponent{});
        render::Camera3D cam;
        REQUIRE(ex.camera(r.w, 1.0f, cam));
        render::Scene3D scene(store, mem::heap());
        scene.begin(cam, {64, 64});
        ex.extract(r.w, scene, 1.0f);
        CHECK(ex.stats().animated == 1);
    }

    // The additive bob: hips rise by half of the clip's 0.5 m-per-second motion.
    const f32 lt = r.anim().layer_time.y;
    CHECK(lt > 0.0f);
    CHECK(r.anim().layer_time.x == doctest::Approx(lt));
    CHECK(r.pos(hips).y == doctest::Approx(1.0f + 0.5f * 0.5f * lt).epsilon(1e-3));
}

TEST_CASE("anim3d: root motion moves the entity, a character's move, and turns it") {
    const std::string walk = graph(R"("states": [ { "name": "walk", "clip": "walk" } ], "root_motion": "hips")");
    {
        Rig r("anim3d_root", walk);
        const Entity hips = r.marker("hips");
        r.run(1);  // enters the state
        r.run(60);
        // Two metres a second along +Z; the hips stay over the entity.
        CHECK(r.pos(r.e).z == doctest::Approx(2.0f).epsilon(0.02));
        CHECK(r.anim().root_velocity.z == doctest::Approx(2.0f).epsilon(1e-3));
        CHECK(r.pos(hips).z == doctest::Approx(r.pos(r.e).z).epsilon(1e-3));
        CHECK(r.pos(hips).x == doctest::Approx(0.0f).epsilon(1e-3));
        r.run(30);  // across the loop
        CHECK(r.pos(r.e).z == doctest::Approx(3.0f).epsilon(0.02));
        CHECK(r.pos(hips).z == doctest::Approx(r.pos(r.e).z).epsilon(1e-3));

        // A turned entity walks its own forward.
        r.w.try_get_mut<LocalTransform>(r.e)->rotation = yaw(90);
        const vec3 before = r.pos(r.e);
        r.run(30);
        CHECK(r.pos(r.e).x - before.x == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(r.pos(r.e).z - before.z == doctest::Approx(0.0f).epsilon(0.02));

        // root_motion 2 only reports; 0 leaves the motion in the pose.
        r.anim().root_motion = 2;
        const vec3 still = r.pos(r.e);
        r.run(10);
        check_near(r.pos(r.e), still);
        CHECK(r.anim().root_velocity.x == doctest::Approx(2.0f).epsilon(1e-3));
        r.anim().root_motion = 0;
        r.run(1);
        CHECK(r.anim().root_velocity.x == 0.0f);
    }
    {
        Rig r("anim3d_root_cc", walk);
        CharacterController3D cc;
        r.w.add(r.e, cc);
        r.run(31);
        CHECK(r.w.try_get<CharacterController3D>(r.e)->move.z == doctest::Approx(2.0f).epsilon(1e-3));
        CHECK(r.pos(r.e).z > 0.5f);  // the controller carried it
    }
    {
        Rig r("anim3d_root_turn", graph(R"("states": [ { "name": "turn", "clip": "turn" } ], "root_motion": "hips")"));
        const Entity nose = r.marker("nose");
        r.run(1);
        r.run(30);  // half a second: 45 degrees
        const quat q = r.w.try_get<LocalTransform>(r.e)->rotation;
        const vec3 fwd = math::rotate(q, {0, 0, 1});
        CHECK(std::atan2(fwd.x, fwd.z) * 180.0f / math::kPi == doctest::Approx(45.0f).epsilon(0.02));
        CHECK(r.anim().root_turn == doctest::Approx(math::kPi / 2.0f).epsilon(0.01));
        // The pose keeps the clip's first heading: the nose points along the entity's forward.
        const vec3 n = r.pos(nose) - r.pos(r.e);
        CHECK(std::atan2(n.x, n.z) * 180.0f / math::kPi == doctest::Approx(45.0f).epsilon(0.02));
    }
}

TEST_CASE("anim3d: two-bone IK reaches an IKTarget3D, bends toward the pole and fades by weight") {
    Rig r("anim3d_twobone", graph(R"("params": ["w"], "states": [ { "name": "stand", "clip": "idle" } ],
        "ik": [ { "name": "hand", "type": "two_bone", "bone": "hand_r", "pole": [0, 0, -1] },
                { "name": "head", "type": "look_at", "bone": "head", "axis": [0, 0, 1], "limit": 60, "param": "w" } ])"));
    const Entity hand = r.marker("hand_r");
    const Entity elbow = r.marker("elbow_r");
    const Entity nose = r.marker("nose");
    const vec3 shoulder{-0.3f, 1.9f, 0};
    IKTarget3D it;
    it.animator = r.e;
    it.chain = "hand";
    const vec3 goal{-0.3f, 1.5f, 0.5f};
    const Entity target = r.w.spawn_with(LocalTransform{goal, quat::identity(), {1, 1, 1}}, it);
    r.run(2);
    CHECK(r.w.try_get<IKTarget3D>(target)->found == 1);
    check_near(r.pos(hand), goal, 2e-3f);
    CHECK(math::length(r.pos(elbow) - shoulder) == doctest::Approx(0.4f).epsilon(1e-3));
    CHECK(math::length(r.pos(hand) - r.pos(elbow)) == doctest::Approx(0.4f).epsilon(1e-3));
    // The elbow sits on the pole's side of the shoulder-hand line.
    const vec3 k = math::normalize(goal - shoulder);
    const vec3 el = r.pos(elbow) - shoulder;
    const vec3 side = el - k * math::dot(el, k);
    CHECK(side.z < -0.05f);

    // Out of reach: the arm points at the goal, straight.
    r.w.try_get_mut<LocalTransform>(target)->position = {-0.3f, 1.9f, 2.0f};
    r.run(1);
    check_near(r.pos(hand), {-0.3f, 1.9f, 0.8f}, 5e-3f);

    // Half weight: the hand goes halfway from its animated place to a reachable goal.
    r.w.try_get_mut<LocalTransform>(target)->position = goal;
    r.w.try_get_mut<IKTarget3D>(target)->weight = 0.5f;
    r.run(1);
    check_near(r.pos(hand), (goal + vec3{-0.3f, 1.1f, 0}) * 0.5f, 2e-3f);

    // An unknown chain does not resolve.
    r.w.try_get_mut<IKTarget3D>(target)->chain = "nope";
    r.run(1);
    CHECK(r.w.try_get<IKTarget3D>(target)->found == 0);
    check_near(r.pos(hand), {-0.3f, 1.1f, 0});

    // Look-at, faded in by its parameter: within the limit the nose points at the goal, past it
    // the head stops at 60 degrees.
    IKTarget3D look;
    look.animator = r.e;
    look.chain = "head";
    const Entity seen = r.w.spawn_with(LocalTransform{{0.5f, 2.0f, 1.0f}, quat::identity(), {1, 1, 1}}, look);
    r.run(1);
    check_near(r.pos(nose), {0, 2, 0.2f});  // w = 0
    r.anim().params.x = 1.0f;
    r.run(1);
    const vec3 dir = math::normalize(vec3{0.5f, 0, 1.0f}) * 0.2f;
    check_near(r.pos(nose), vec3{0, 2, 0} + dir, 2e-3f);
    r.w.try_get_mut<LocalTransform>(seen)->position = {3.0f, 2.0f, 0.0f};
    r.run(1);
    const f32 s60 = std::sin(math::kPi / 3.0f) * 0.2f, c60 = std::cos(math::kPi / 3.0f) * 0.2f;
    check_near(r.pos(nose), {s60, 2, c60}, 2e-3f);
}

TEST_CASE("anim3d: foot IK plants the feet on a step up and lowers the pelvis for a step down") {
    const std::string feet = graph(R"("states": [ { "name": "stand", "clip": "idle" } ],
        "ik": [ { "name": "foot_l", "type": "foot", "bone": "foot_l", "pelvis": "hips", "reach": 0.5, "pole": [0, 0, 1] },
                { "name": "foot_r", "type": "foot", "bone": "foot_r", "pelvis": "hips", "reach": 0.5, "pole": [0, 0, 1] } ])");
    {
        Rig r("anim3d_feet_up", feet, true);
        BoxCollider3D step;  // a 0.2 m step under the left foot (x > 0)
        step.half = {0.5f, 0.1f, 0.5f};
        r.w.spawn_with(LocalTransform{{0.6f, 0.1f, 0}, quat::identity(), {1, 1, 1}}, step);
        const Entity fl = r.marker("foot_l"), fr = r.marker("foot_r"), hips = r.marker("hips"), knee = r.marker("knee_l");
        r.run(2);
        check_near(r.pos(fl), {0.2f, 0.2f, 0}, 3e-3f);
        check_near(r.pos(fr), {-0.2f, 0, 0}, 3e-3f);
        check_near(r.pos(hips), {0, 1, 0}, 1e-3f);
        CHECK(r.pos(knee).z > 0.05f);  // the knee bends forward, toward the pole
        CHECK(math::length(r.pos(fl) - r.pos(knee)) == doctest::Approx(0.5f).epsilon(1e-3));
    }
    {
        Rig r("anim3d_feet_down", feet, false);
        BoxCollider3D high, low;  // the right foot on y = 0, the left over a floor 0.2 m lower
        high.half = {0.5f, 0.5f, 0.5f};
        low.half = {0.5f, 0.5f, 0.5f};
        r.w.spawn_with(LocalTransform{{-0.5f, -0.5f, 0}, quat::identity(), {1, 1, 1}}, high);
        r.w.spawn_with(LocalTransform{{0.5f, -0.7f, 0}, quat::identity(), {1, 1, 1}}, low);
        const Entity fl = r.marker("foot_l"), fr = r.marker("foot_r"), hips = r.marker("hips");
        r.run(2);
        check_near(r.pos(hips), {0, 0.8f, 0}, 2e-3f);
        check_near(r.pos(fl), {0.2f, -0.2f, 0}, 3e-3f);
        check_near(r.pos(fr), {-0.2f, 0, 0}, 3e-3f);
    }
}

TEST_CASE("anim3d: root motion and IK replay to the same state hash") {
    const std::string g = graph(R"("states": [ { "name": "walk", "clip": "walk" } ], "root_motion": "hips",
        "ik": [ { "name": "foot_l", "type": "foot", "bone": "foot_l", "pelvis": "hips" },
                { "name": "hand", "type": "two_bone", "bone": "hand_r" } ])");
    auto run = [&](const char* name) {
        Rig r(name, g, true);
        IKTarget3D it;
        it.animator = r.e;
        it.chain = "hand";
        r.w.spawn_with(LocalTransform{{-0.3f, 1.5f, 0.5f}, quat::identity(), {1, 1, 1}}, it);
        r.marker("hand_r");
        CharacterController3D cc;
        r.w.add(r.e, cc);
        r.run(120);
        return r.w.state_hash();
    };
    CHECK(run("anim3d_hash_a") == run("anim3d_hash_b"));
}

// ---- M-MOVE3D 5: animation from movement ------------------------------------------------------------

namespace {

// Idle, walk, air and duck states driven only by the parameters Locomotion3D fills.
std::string locomotion_graph() {
    return graph(R"("params": ["speed", "grounded", "crouch", "move_y"], "start": "idle",
        "states": [ { "name": "idle", "clip": "idle" }, { "name": "walk", "clip": "bob" },
                    { "name": "air", "clip": "lift" }, { "name": "duck", "clip": "pose_f" } ],
        "transitions": [
            { "from": "*", "to": "air", "when": [ { "param": "grounded", "op": "<", "value": 0.5 } ], "fade": 0 },
            { "from": "*", "to": "duck", "when": [ { "param": "crouch", "op": ">", "value": 0.5 }, { "param": "grounded", "op": ">", "value": 0.5 } ], "fade": 0 },
            { "from": "air", "to": "idle", "when": [ { "param": "grounded", "op": ">", "value": 0.5 } ], "fade": 0 },
            { "from": "duck", "to": "idle", "when": [ { "param": "crouch", "op": "<", "value": 0.5 } ], "fade": 0 },
            { "from": "idle", "to": "walk", "when": [ { "param": "speed", "op": ">", "value": 0.5 } ], "fade": 0 },
            { "from": "walk", "to": "idle", "when": [ { "param": "speed", "op": "<", "value": 0.3 } ], "fade": 0 } ])");
}

}  // namespace

TEST_CASE("anim3d: Locomotion3D feeds a character's run, jump and crouch to the graph") {
    Rig r("anim3d_loco", locomotion_graph(), true);
    r.w.try_get_mut<LocalTransform>(r.e)->position = {0, 0.9f, 0};
    CharacterController3D cc;
    cc.crouch_half_height = 0.3f;
    r.w.add(r.e, cc);
    Locomotion3D l;
    l.stride_speed = 2.0f;
    r.w.add(r.e, l);
    auto loco = [&]() -> const Locomotion3D& { return *r.w.try_get<Locomotion3D>(r.e); };
    auto ctl = [&]() -> CharacterController3D& { return *r.w.try_get_mut<CharacterController3D>(r.e); };
    r.run(30);
    CHECK(loco().found == 4);
    CHECK(r.anim().params.y == 1.0f);  // grounded
    CHECK(r.anim().state.view() == "idle");
    CHECK(r.anim().speed == 1.0f);

    // Walking forward (-Z) at 3 m/s: speed and move_y follow within 0.1 s smoothing, the walk plays
    // 1.5x fast for a 2 m/s stride.
    ctl().move = {0, 0, -3};
    r.run(60);
    CHECK(r.anim().params.x == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(r.anim().params.w == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(r.anim().state.view() == "walk");
    CHECK(r.anim().speed == doctest::Approx(1.5f).epsilon(0.01));
    // Sideways: move_y falls to 0 while speed holds.
    ctl().move = {3, 0, 0};
    r.run(60);
    CHECK(r.anim().params.x == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(std::fabs(r.anim().params.w) < 0.05f);
    CHECK(loco().local_move.x == doctest::Approx(3.0f).epsilon(0.01));
    // Fast: the play rate clamps at max_rate.
    ctl().move = {0, 0, -6};
    r.run(60);
    CHECK(r.anim().speed == doctest::Approx(2.0f));

    // Stopped: idle at rate 1.
    ctl().move = {};
    r.run(60);
    CHECK(r.anim().state.view() == "idle");
    CHECK(r.anim().speed == 1.0f);

    // A jump: airborne with a rising vertical speed, back to idle on landing.
    ctl().jump = true;
    r.run(10);
    CHECK(r.anim().params.y == 0.0f);
    CHECK(loco().vertical_speed > 1.0f);
    CHECK(r.anim().state.view() == "air");
    r.run(90);
    CHECK(r.anim().params.y == 1.0f);
    CHECK(r.anim().state.view() == "idle");

    // Crouching ducks; standing up returns.
    ctl().crouch = true;
    r.run(5);
    CHECK(r.anim().params.z == 1.0f);
    CHECK(r.anim().state.view() == "duck");
    ctl().crouch = false;
    r.run(5);
    CHECK(r.anim().params.z == 0.0f);
    CHECK(r.anim().state.view() == "idle");
}

TEST_CASE("anim3d: Locomotion3D reads a moved entity's speed and drives another entity's Animator") {
    Rig r("anim3d_loco_moved", locomotion_graph());
    // The mover has no controller: its position's change is the motion. The model (r.e) is a
    // separate entity named in `animator`.
    Locomotion3D l;
    l.animator = r.e;
    l.smoothing = 0.0f;
    const Entity mover = r.w.spawn_with(LocalTransform{{}, quat::identity(), {1, 1, 1}}, l);
    r.run(2);
    for (u32 i = 0; i < 20; ++i) {
        r.w.try_get_mut<LocalTransform>(mover)->position.z -= 0.05f;  // 3 m/s forward
        r.run(1);
    }
    CHECK(r.anim().params.x == doctest::Approx(3.0f).epsilon(1e-3));
    CHECK(r.anim().params.w == doctest::Approx(3.0f).epsilon(1e-3));
    CHECK(r.anim().params.y == 1.0f);
    CHECK(r.anim().state.view() == "walk");
    CHECK(r.anim().speed == 1.0f);  // no stride_speed: the play rate is left alone
}

TEST_CASE("anim3d: Locomotion3D turn_rate turns a root to face its motion, at its rate, and only while it moves") {
    Rig r("anim3d_loco_turn", locomotion_graph());
    Locomotion3D l;
    l.animator = r.e;
    l.turn_rate = 3.0f;  // rad/s
    const Entity mover = r.w.spawn_with(LocalTransform{{}, quat::identity(), {1, 1, 1}}, l);
    const auto yaw = [&] {
        const vec3 f = math::rotate(r.w.try_get<LocalTransform>(mover)->rotation, vec3{0.0f, 0.0f, -1.0f});
        return std::atan2(-f.x, -f.z);
    };
    r.run(2);
    // Moving along +X (to its right): it turns towards yaw -pi/2, at most 3 rad/s.
    for (u32 i = 0; i < 10; ++i) {
        r.w.try_get_mut<LocalTransform>(mover)->position.x += 0.05f;
        r.run(1);
    }
    CHECK(yaw() == doctest::Approx(-3.0f * 9.0f / 60.0f).epsilon(0.05));  // 9 ticks of motion seen, still turning
    for (u32 i = 0; i < 60; ++i) {
        r.w.try_get_mut<LocalTransform>(mover)->position.x += 0.05f;
        r.run(1);
    }
    CHECK(yaw() == doctest::Approx(-math::kPi / 2.0f).epsilon(1e-3));  // facing +X
    // Standing still (below turn_min_speed), it keeps that facing.
    r.run(30);
    CHECK(yaw() == doctest::Approx(-math::kPi / 2.0f).epsilon(1e-3));
    // turn_rate 0 never turns.
    r.w.try_get_mut<Locomotion3D>(mover)->turn_rate = 0.0f;
    for (u32 i = 0; i < 30; ++i) {
        r.w.try_get_mut<LocalTransform>(mover)->position.z += 0.05f;
        r.run(1);
    }
    CHECK(yaw() == doctest::Approx(-math::kPi / 2.0f).epsilon(1e-3));
}

TEST_CASE("anim3d: foot IK plants both feet on a slope and a stair while the character walks") {
    const std::string feet = graph(R"("params": ["speed"], "states": [ { "name": "stand", "clip": "idle" } ],
        "ik": [ { "name": "foot_l", "type": "foot", "bone": "foot_l", "pelvis": "hips", "reach": 0.5, "pole": [0, 0, 1] },
                { "name": "foot_r", "type": "foot", "bone": "foot_r", "pelvis": "hips", "reach": 0.5, "pole": [0, 0, 1] } ])");
    {
        // A 10 degree slope rising towards +X: the left foot (x = 0.2) stands higher than the right.
        Rig r("anim3d_feet_slope", feet);
        const f32 rise = std::tan(10.0f * math::kPi / 180.0f);
        BoxCollider3D slope;
        slope.half = {5.0f, 0.5f, 5.0f};
        const quat tilt = math::quat_from_axis_angle({0, 0, 1}, 10.0f * math::kPi / 180.0f);
        r.w.spawn_with(LocalTransform{math::rotate(tilt, vec3{0, -0.5f, 0}), tilt, {1, 1, 1}}, slope);
        const Entity fl = r.marker("foot_l"), fr = r.marker("foot_r"), hips = r.marker("hips");
        r.run(2);
        CHECK(r.pos(fl).y == doctest::Approx(0.2f * rise).epsilon(0.01).scale(1.0));
        CHECK(r.pos(fr).y == doctest::Approx(-0.2f * rise).epsilon(0.01).scale(1.0));
        // The pelvis drops by the lower foot's depth so that leg still reaches.
        CHECK(r.pos(hips).y == doctest::Approx(1.0f - 0.2f * rise).epsilon(0.01).scale(1.0));
    }
    {
        // A character walking along a 0.15 m stair edge (the left foot on the step, the right on the
        // floor) keeps both feet on their surfaces every tick while Locomotion3D reports its speed.
        Rig r("anim3d_feet_stair", feet, true, true);
        BoxCollider3D step;
        step.half = {1.0f, 0.075f, 6.0f};
        r.w.spawn_with(LocalTransform{{1.1f, 0.075f, -4.0f}, quat::identity(), {1, 1, 1}}, step);
        // The controller is on a mover; the model hangs below it as a child, feet on the floor.
        CharacterController3D cc;
        cc.radius = 0.05f;  // thin enough to walk beside the step without climbing it
        cc.half_height = 0.85f;
        Locomotion3D l;
        l.animator = r.e;
        const Entity mover = r.w.spawn_with(LocalTransform{{0, 0.9f, 0}, quat::identity(), {1, 1, 1}}, cc, l);
        r.w.add(r.e, ChildOf{mover});
        r.w.try_get_mut<LocalTransform>(r.e)->position = {0, -0.9f, 0};
        ecs::propagate_transforms(r.w);
        const Entity fl = r.marker("foot_l"), fr = r.marker("foot_r");
        r.run(30);
        r.w.try_get_mut<CharacterController3D>(mover)->move = {0, 0, -2};
        f32 worst_l = 0.0f, worst_r = 0.0f;
        for (u32 i = 0; i < 90; ++i) {
            r.run(1);
            worst_l = math::max(worst_l, std::fabs(r.pos(fl).y - 0.15f));
            worst_r = math::max(worst_r, std::fabs(r.pos(fr).y));
        }
        CHECK(r.pos(mover).z < -2.5f);
        CHECK(r.anim().params.x == doctest::Approx(2.0f).epsilon(0.02));
        CHECK(worst_l < 0.01f);
        CHECK(worst_r < 0.01f);
    }
}
