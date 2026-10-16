// M-ANIM2D 1: 2D skeletons. The pose math with exact numbers (parent chains, inherited rotation,
// non-uniform scale), the .mcskel2d cook and its errors, the CHNK_SKEL2D reader's rejects, the
// slots as the sprite extract places them, Bone2DFollow, and determinism: two runs hash the same,
// a save restores the pose, and the scene text round-trips the per-bone offsets.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/cook/json.h"
#include "mce/cook/scene_text.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/skeleton2d.h"
#include "mce/kit/stock.h"
#include "mce/scene/scene.h"

using namespace mce;
using namespace cook_test;
using asset::Skeleton2DBone;
using asset::Skeleton2DSlot;
using kit::Bone2DPose;
using kit::Bone2DTransform;
using kit::Skeleton2D;

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kDt = 1.0f / 60.0f;
constexpr const char* kHero = "5c2d0000-0000-4000-8000-000000000001";
constexpr const char* kScene = "5c2d0000-0000-4000-8000-000000000002";
constexpr const char* kArmTex = "5c2d0000-0000-4000-8000-0000000000a1";

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

// A CHNK_SKEL2D payload built by hand (the reader tests and the pure-math tests skip the cook).
struct Builder {
    std::vector<Skeleton2DBone> bones;
    std::vector<Skeleton2DSlot> slots;
    std::string pool;

    u32 str(const char* s) {
        const u32 at = static_cast<u32>(pool.size());
        pool += s;
        pool.push_back('\0');
        return at;
    }
    u16 bone(const char* name, u16 parent, f32 x, f32 y, f32 rot = 0.0f, f32 sx = 1.0f, f32 sy = 1.0f) {
        Skeleton2DBone b{};
        b.name = str(name);
        b.parent = parent;
        b.x = x;
        b.y = y;
        b.rotation = rot;
        b.scale_x = sx;
        b.scale_y = sy;
        bones.push_back(b);
        return static_cast<u16>(bones.size() - 1);
    }
    Skeleton2DSlot& slot(const char* name, u16 bone) {
        Skeleton2DSlot s{};
        s.name = str(name);
        s.bone = bone;
        s.rect[2] = s.rect[3] = 1.0f;
        s.size[0] = s.size[1] = 1.0f;
        s.pivot[0] = s.pivot[1] = 0.5f;
        for (f32& t : s.tint) t = 1.0f;
        slots.push_back(s);
        return slots.back();
    }
    Array<byte> payload() const {
        asset::Skeleton2DHeader h{};
        std::memcpy(h.name, "test", 4);
        h.bone_count = static_cast<u32>(bones.size());
        h.slot_count = static_cast<u32>(slots.size());
        h.string_bytes = static_cast<u32>(pool.size());
        Array<byte> out;
        auto append = [&](const void* p, usize n) {
            const usize at = out.size();
            out.resize(at + n, byte{0});
            if (n) std::memcpy(out.data() + at, p, n);
        };
        append(&h, sizeof(h));
        append(bones.data(), bones.size() * sizeof(Skeleton2DBone));
        append(slots.data(), slots.size() * sizeof(Skeleton2DSlot));
        append(pool.data(), pool.size());
        return out;
    }
};

constexpr u16 kRoot = asset::kSkeleton2DNoParent;

// `n` more rest-pose entries of Skeleton2D.pose (a scene lists all 64).
std::string rest_pose(u32 n) {
    std::string s;
    for (u32 i = 0; i < n; ++i) s += ", {}";
    return s;
}

// The project the cooked tests share: a three-bone hero with two slots (the arm's texture is a
// real PNG, so the dependency resolves) and a scene with the hero and a sword following its hand.
const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("skeleton2d_project");
        write_file(r + "/assets/arm.png", make_png(2, 2, std::vector<u8>(2 * 2 * 4, 255)));
        write_file(r + "/assets/arm.png.mceasset", sidecar(kArmTex, "texture", R"({ "srgb": true, "mips": false })"));
        write_file(r + "/assets/hero.mcskel2d", std::string(R"({ "format": "mce-skeleton2d", "version": 1,
            "guid": ")") + kHero + R"(", "name": "hero",
            "bones": [
              { "name": "root" },
              { "name": "torso", "parent": "root", "y": 0.5, "length": 0.6 },
              { "name": "hand", "parent": "torso", "x": 0.25, "y": 0.5, "rotation": 90, "scale": [2, 1] } ],
            "slots": [
              { "name": "body", "bone": "torso", "size": [0.4, 0.7], "pivot": [0.5, 0], "tint": [1, 0.5, 0.5, 1] },
              { "name": "arm", "bone": "hand", "texture": "guid:)" +
                                                    kArmTex + R"(", "rect": [0, 0, 0.5, 0.5],
                "size": [0.1, 0.3], "offset": [0.1, 0], "rotation": -90 } ] })");
        write_file(r + "/assets/stage.mcscene", std::string(R"({ "format": "mce-scene", "version": 1,
            "guid": ")") + kScene + R"(", "name": "stage", "entities": [
            { "name": "Hero", "components": {
                "SaveKey": { "key": 1 },
                "LocalTransform": { "position": [3, 1, 0] },
                "Skeleton2D": { "skeleton": "guid:)" +
                                                    kHero + R"(", "layer": 2, "order": 5,
                                "pose": [ {}, { "rotation": 0.5 })" +
                                                    rest_pose(62) + R"( ] } } },
            { "name": "Sword", "components": {
                "LocalTransform": { "position": [0, 0, 7] },
                "Bone2DFollow": { "skeleton": "$0", "bone": "hand", "offset": [0.2, 0] } } } ] })");
        cook::CookService svc;
        const cook::CookReport rep = svc.cook(StringView(r.c_str()), cook::CookOptions{});
        INFO(diag_text(rep));
        REQUIRE(rep.ok());
        return r;
    }();
    return root;
}

struct Stage {
    asset::AssetSystem assets;
    World world;
    Array<Entity> e;  // hero, sword
    u64 ticks = 0;

    Stage() {
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        world.register_components<LocalTransform, SaveKey>();
        kit::install_skeleton2d(world, &assets);
        REQUIRE(scene::load(world, assets, asset::parse_guid(kScene).value(), &e));
        REQUIRE(e.size() == 2);
    }
    Skeleton2D& hero() { return *world.try_get_mut<Skeleton2D>(e[0]); }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
};

void wave(Frame& f) {
    f.query<Skeleton2D>().each([&](Entity, Skeleton2D& sk) { sk.pose[2].rotation += 0.05f; });
}

}  // namespace

TEST_CASE("skeleton2d: world pose of a parent chain, inherited rotation and non-uniform scale") {
    SUBCASE("translation chains add") {
        Builder b;
        b.bone("root", kRoot, 1, 0);
        b.bone("mid", 0, 0, 2);
        b.bone("tip", 1, 3, 0);
        Bone2DTransform w[3];
        kit::skeleton2d_pose_offsets(Span<const Skeleton2DBone>(b.bones.data(), 3), {}, Span<Bone2DTransform>(w, 3));
        CHECK(w[0].x == 1.0f);
        CHECK(w[0].y == 0.0f);
        CHECK(w[1].x == 1.0f);
        CHECK(w[1].y == 2.0f);
        CHECK(w[2].x == 4.0f);
        CHECK(w[2].y == 2.0f);
        CHECK(w[2].a == 1.0f);
        CHECK(w[2].d == 1.0f);
    }
    SUBCASE("rotation inherits") {
        Builder b;
        b.bone("root", kRoot, 0, 0, kPi / 2);
        b.bone("arm", 0, 2, 0, kPi / 2);
        b.bone("hand", 1, 1, 0);
        Bone2DTransform w[3];
        kit::skeleton2d_pose_offsets(Span<const Skeleton2DBone>(b.bones.data(), 3), {}, Span<Bone2DTransform>(w, 3));
        CHECK(w[1].x == doctest::Approx(0.0f));
        CHECK(w[1].y == doctest::Approx(2.0f));
        CHECK(w[1].a == doctest::Approx(-1.0f));  // the arm points along -x: 180 degrees in all
        CHECK(w[1].b == doctest::Approx(0.0f).epsilon(1e-6));
        CHECK(w[2].x == doctest::Approx(-1.0f));
        CHECK(w[2].y == doctest::Approx(2.0f));
    }
    SUBCASE("non-uniform scale scales the children's offsets and axes") {
        Builder b;
        b.bone("root", kRoot, 0, 0, 0, 2, 1);
        b.bone("child", 0, 1, 1);
        b.bone("turned", 0, 0, 0, kPi / 2);
        Bone2DTransform w[3];
        kit::skeleton2d_pose_offsets(Span<const Skeleton2DBone>(b.bones.data(), 3), {}, Span<Bone2DTransform>(w, 3));
        CHECK(w[1].x == 2.0f);
        CHECK(w[1].y == 1.0f);
        CHECK(w[1].a == 2.0f);
        CHECK(w[1].d == 1.0f);
        // A turned child of a stretched parent: its +x axis is (0, 1), its +y axis (-2, 0).
        CHECK(w[2].a == doctest::Approx(0.0f));
        CHECK(w[2].b == doctest::Approx(1.0f));
        CHECK(w[2].c == doctest::Approx(-2.0f));
        CHECK(w[2].d == doctest::Approx(0.0f));
    }
    SUBCASE("offsets add to the rest pose; scales multiply") {
        Builder b;
        b.bone("root", kRoot, 1, 2, 0.5f, 2, 3);
        Bone2DPose off;
        off.x = 1;
        off.y = -1;
        off.rotation = 0.25f;
        off.scale_x = 0.5f;
        off.scale_y = 2;
        const Bone2DPose p = kit::bone2d_offset(b.bones[0], off);
        CHECK(p.x == 2.0f);
        CHECK(p.y == 1.0f);
        CHECK(p.rotation == 0.75f);
        CHECK(p.scale_x == 1.0f);
        CHECK(p.scale_y == 6.0f);
    }
}

TEST_CASE("skeleton2d: slots become quads in slot order at entity * flip * bone * slot") {
    Builder b;
    b.bone("root", kRoot, 0, 0);
    const u16 body = b.bone("body", 0, 0, 1);
    const u16 arm = b.bone("arm", body, 0.5f, 0, kPi / 2);
    {
        Skeleton2DSlot& s = b.slot("body", body);
        s.size[0] = 1;
        s.size[1] = 2;
        s.pivot[1] = 0;
        s.tint[1] = 0.5f;
    }
    {
        Skeleton2DSlot& s = b.slot("arm", arm);
        s.texture[0] = 7;
        s.texture[1] = 9;
        s.rect[2] = s.rect[3] = 0.5f;
        s.size[0] = 0.25f;
        s.size[1] = 1;
        s.offset[0] = 0.5f;
    }
    const Array<byte> bytes = b.payload();
    kit::Skeleton2DRegistry reg;
    const asset::Guid guid{1, 2};
    REQUIRE(reg.define(guid, bytes.span()));
    const kit::Skeleton2DRegistry::Def* def = reg.resolve(guid);
    REQUIRE(def);
    CHECK(def->find_bone("arm") == 2);

    Skeleton2D sk;
    sk.skeleton.guid = guid;
    sk.tint = {0.5f, 1, 1, 1};
    const Bone2DTransform at = kit::transform_2d({10, 0, 0}, rotation_z(0.0f), {1, 1, 1});
    Array<kit::Skeleton2DQuad> q;
    kit::skeleton2d_quads(*def, sk, at, q);
    REQUIRE(q.size() == 2);
    CHECK(q[0].slot == 0);  // slot order is draw order
    CHECK(q[0].texture == asset::Guid{});
    CHECK(q[0].position.x == 10.0f);
    CHECK(q[0].position.y == 1.0f);
    CHECK(q[0].rotation == 0.0f);
    CHECK(q[0].scale.x == 1.0f);
    CHECK(q[0].scale.y == 2.0f);
    CHECK(q[0].pivot.y == 0.0f);
    CHECK(q[0].tint.r == 0.5f);  // slot tint x component tint
    CHECK(q[0].tint.g == 0.5f);
    CHECK(q[1].slot == 1);
    CHECK(q[1].texture == asset::Guid{7, 9});
    CHECK(q[1].rect.z == 0.5f);
    CHECK(q[1].position.x == doctest::Approx(10.5f));
    CHECK(q[1].position.y == doctest::Approx(1.5f));  // the arm turns 90 degrees: its +x is world +y
    CHECK(q[1].rotation == doctest::Approx(kPi / 2));
    CHECK(q[1].scale.x == doctest::Approx(0.25f));
    CHECK(q[1].scale.y == doctest::Approx(1.0f));

    // flip_x mirrors about the entity's origin: x positions mirror, the mirrored quad's y size is negative.
    sk.flip_x = true;
    q.clear();
    kit::skeleton2d_quads(*def, sk, at, q);
    REQUIRE(q.size() == 2);
    CHECK(q[0].position.x == 10.0f);
    CHECK(q[0].scale.x == 1.0f);
    CHECK(q[0].scale.y == -2.0f);
    CHECK(q[1].position.x == doctest::Approx(9.5f));
    CHECK(q[1].position.y == doctest::Approx(1.5f));
    CHECK(q[1].scale.y == doctest::Approx(-1.0f));

    // A pose offset moves the slots; an invisible skeleton draws nothing.
    sk.flip_x = false;
    sk.pose[1].y = 1.0f;
    q.clear();
    kit::skeleton2d_quads(*def, sk, at, q);
    CHECK(q[0].position.y == 2.0f);
    CHECK(q[1].position.y == doctest::Approx(2.5f));
    sk.visible = false;
    q.clear();
    kit::skeleton2d_quads(*def, sk, at, q);
    CHECK(q.empty());
}

TEST_CASE("skeleton2d: the reader rejects bad payloads") {
    Builder b;
    b.bone("root", kRoot, 0, 0);
    b.bone("a", 0, 1, 0);
    b.bone("b", 1, 1, 0);
    b.slot("s", 2);
    {
        const Array<byte> ok = b.payload();
        auto v = asset::Skeleton2DView::read(ok.span());
        REQUIRE(v);
        CHECK(v.value().name() == StringView("test"));
        CHECK(v.value().find_bone("b") == 2);
        CHECK(v.value().string(v.value().slots[0].name) == StringView("s"));
        // Truncated: every byte short of the declared size fails.
        CHECK_FALSE(asset::Skeleton2DView::read(ok.span().subspan(0, ok.size() - 1)));
        CHECK_FALSE(asset::Skeleton2DView::read(ok.span().subspan(0, 10)));
    }
    auto rejects = [&](auto&& mutate) {
        Builder c = b;
        mutate(c);
        const Array<byte> bytes = c.payload();
        return !asset::Skeleton2DView::read(bytes.span());
    };
    CHECK(rejects([](Builder& c) { c.bones[1].parent = 2; }));  // parent after child
    CHECK(rejects([](Builder& c) { c.bones[1].parent = 1; }));  // its own parent
    CHECK(rejects([](Builder& c) { c.bones[0].parent = 0; }));  // the first bone is a root
    CHECK(rejects([](Builder& c) { c.slots[0].bone = 3; }));    // no such bone
    CHECK(rejects([](Builder& c) { c.slots[0].name = 999; }));  // string out of range
    CHECK(rejects([](Builder& c) { c.bones[2].x = std::nanf(""); }));
    CHECK(rejects([](Builder& c) { c.slots[0].tint[3] = INFINITY; }));
    CHECK(rejects([](Builder& c) { c.slots[0].kind = static_cast<asset::Skeleton2DSlotKind>(1); }));
    CHECK(rejects([](Builder& c) {
        for (u32 i = 0; i < 64; ++i) c.bone("extra", 0, 0, 0);  // 67 bones
    }));
    CHECK(rejects([](Builder& c) { c.pool.back() = 'x'; }));  // the pool must end in NUL
}

TEST_CASE("skeleton2d: .mcskel2d cooks to CHNK_SKEL2D and a scene poses it") {
    Stage s;
    const asset::AssetKey key = s.assets.find(asset::parse_guid(kHero).value());
    REQUIRE(key != asset::kInvalidAssetKey);
    auto h = s.assets.load_sync<asset::Skeleton2D>(key);
    REQUIRE(h);
    auto view = asset::Skeleton2DView::read(s.assets.resolve(h.value()));
    REQUIRE(view);
    const asset::Skeleton2DView& v = view.value();
    CHECK(v.name() == StringView("hero"));
    REQUIRE(v.bones.size() == 3);
    CHECK(v.bones[0].parent == kRoot);
    CHECK(v.bones[2].parent == 1);
    CHECK(v.bones[1].length == doctest::Approx(0.6f));
    CHECK(v.bones[2].rotation == doctest::Approx(kPi / 2));  // degrees in the text, radians cooked
    CHECK(v.bones[2].scale_x == 2.0f);
    CHECK(v.bones[2].scale_y == 1.0f);
    REQUIRE(v.slots.size() == 2);
    CHECK(v.string(v.slots[0].name) == StringView("body"));
    CHECK(v.slots[0].texture_guid() == asset::Guid{});
    CHECK(v.slots[0].tint[1] == 0.5f);
    CHECK(v.slots[1].bone == 2);
    CHECK(v.slots[1].texture_guid() == asset::parse_guid(kArmTex).value());
    CHECK(v.slots[1].rotation == doctest::Approx(-kPi / 2));
    s.assets.release(asset::AnyHandle(h.value()));

    // The scene's per-bone offset loaded; the tick poses the bones and moves the sword.
    CHECK(s.hero().pose[1].rotation == 0.5f);
    CHECK(s.hero().layer == 2);
    CHECK(s.hero().bone_count == 0);
    s.run(1);
    CHECK(s.hero().bone_count == 3);
    // torso: (0, 0.5) turned 0.5 rad; hand: torso * (0.25, 0.5).
    const f32 c = std::cos(0.5f), sn = std::sin(0.5f);
    const Bone2DTransform& hand = s.hero().world[2];
    CHECK(hand.x == doctest::Approx(0.25f * c - 0.5f * sn));
    CHECK(hand.y == doctest::Approx(0.5f + 0.25f * sn + 0.5f * c));
    Bone2DTransform by_name;
    REQUIRE(kit::skeleton2d_bone(s.world, s.e[0], "hand", by_name));
    CHECK(by_name == hand);
    CHECK_FALSE(kit::skeleton2d_bone(s.world, s.e[0], "tail", by_name));

    // The sword sits at hero (3, 1) + hand * (0.2, 0): the hand is scaled 2 along its +x.
    const LocalTransform& sword = *s.world.try_get<LocalTransform>(s.e[1]);
    const math::vec2 tip = hand.apply({0.2f, 0.0f});
    CHECK(sword.position.x == doctest::Approx(3.0f + tip.x));
    CHECK(sword.position.y == doctest::Approx(1.0f + tip.y));
    CHECK(sword.position.z == 7.0f);  // z keeps its value
    CHECK(angle_z(sword.rotation) == doctest::Approx(0.5f + kPi / 2));
}

TEST_CASE("skeleton2d: a follower parented to the skeleton gets the bone in skeleton space") {
    Stage s;
    ecs::install_hierarchy(s.world);
    REQUIRE(ecs::set_parent(s.world, s.e[1], s.e[0]));
    s.run(1);
    const Bone2DTransform& hand = s.hero().world[2];
    const math::vec2 tip = hand.apply({0.2f, 0.0f});
    const LocalTransform& sword = *s.world.try_get<LocalTransform>(s.e[1]);
    CHECK(sword.position.x == doctest::Approx(tip.x));
    CHECK(sword.position.y == doctest::Approx(tip.y));
}

TEST_CASE("skeleton2d: cook errors name the bone and the file position") {
    struct Case {
        const char* body;
        const char* expect;
    };
    const Case cases[] = {
        {R"("bones": [ { "name": "root" }, { "name": "arm", "parent": "torso" }, { "name": "torso", "parent": "root" } ])",
         "bone 'arm' names parent 'torso', which comes later; list parents before their children"},
        {R"("bones": [ { "name": "root" }, { "name": "arm", "parent": "nope" } ])", "but no bone has that name"},
        {R"("bones": [ { "name": "root" }, { "name": "root" } ])", "two bones share this name"},
        {R"("bones": [ { "name": "root" } ], "slots": [ { "name": "s", "bone": "hip" } ])",
         "slot 's' names bone 'hip'"},
        {R"("bones": [ { "name": "root" } ], "slots": [ { "name": "s", "bone": "root" }, { "name": "s", "bone": "root" } ])",
         "two slots share this name"},
        {R"("bones": [ { "name": "root" } ], "slots": [ { "name": "s", "bone": "root", "size": [1] } ])",
         "'size' must be an array of 2 numbers"},
        {R"("bones": [ { "name": "root", "rotaton": 3 } ])", "rotaton"},
        {R"("bones": [])", "at least one bone"},
        {R"("bones": [ { "name": "root" } ], "slots": [ { "name": "s", "bone": "root", "texture": "guid:5c2d0000-0000-4000-8000-0000000000ff" } ])",
         "slots[0].texture"},
    };
    u32 i = 0;
    for (const Case& c : cases) {
        const std::string root = fresh_dir("skeleton2d_errors_" + std::to_string(i++));
        write_file(root + "/assets/bad.mcskel2d",
                   std::string(
                       R"({ "format": "mce-skeleton2d", "version": 1, "guid": "5c2d0000-0000-4000-8000-0000000000f0",
    )") + c.body + " }");
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        const std::string text = diag_text(r);
        INFO(text);
        CHECK(text.find(c.expect) != std::string::npos);
        CHECK(text.find("bad.mcskel2d:") != std::string::npos);
    }

    // 65 bones: one more than the component holds.
    std::string bones = R"("bones": [ { "name": "b0" })";
    for (u32 b = 1; b <= 64; ++b) bones += R"(, { "name": "b)" + std::to_string(b) + R"(", "parent": "b0" })";
    bones += " ]";
    const std::string root = fresh_dir("skeleton2d_errors_many");
    write_file(
        root + "/assets/bad.mcskel2d",
        std::string(R"({ "format": "mce-skeleton2d", "version": 1, "guid": "5c2d0000-0000-4000-8000-0000000000f1", )") +
            bones + " }");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    CHECK_FALSE(r.ok());
    CHECK(diag_text(r).find("at most 64 bones") != std::string::npos);
}

TEST_CASE("skeleton2d: two runs hash the same every tick, and a save restores the pose") {
    auto setup = [](Stage& s) { s.world.add_system(Phase::FixedUpdate_Pre, "test_wave", &wave); };
    Stage a, b;
    setup(a);
    setup(b);
    for (u32 i = 0; i < 60; ++i) {
        a.run(1);
        b.run(1);
        REQUIRE(a.world.state_hash() == b.world.state_hash());
    }
    CHECK(a.hero().pose[2].rotation == doctest::Approx(3.0f));

    Stage c;
    setup(c);
    c.run(30);
    Array<byte> bytes;
    save::write_save(c.world, save::SaveInfo{}, bytes);
    Stage d;  // the scene as authored
    setup(d);
    d.ticks = c.ticks;
    REQUIRE(save::read_save(d.world, bytes.span()));
    CHECK(d.hero().pose[2].rotation == c.hero().pose[2].rotation);
    CHECK(d.hero().pose[1].rotation == 0.5f);
    c.run(1);
    d.run(1);
    for (u32 i = 0; i < 3; ++i) CHECK(d.hero().world[i] == c.hero().world[i]);
    CHECK(d.world.state_hash() == c.world.state_hash());
}

TEST_CASE("skeleton2d: the scene text keeps the per-bone offsets and drops the derived pose") {
    const refl::TypeRecord* type = refl::find_type(StringId("Skeleton2D"));
    REQUIRE(type);
    Skeleton2D sk;
    sk.layer = 3;
    sk.pose[2].rotation = 0.25f;
    sk.pose[2].scale_y = 2.0f;
    sk.world[0].x = 9.0f;  // Transient: never written
    cook::String text;
    cook::write_scene_component(*type, &sk, text, [](const void*, void*) { return cook::kNoSceneEntity; }, nullptr);
    const std::string s(text.view().data(), text.view().size());
    INFO(s);
    CHECK(s.find("\"pose\"") != std::string::npos);
    CHECK(s.find("\"world\"") == std::string::npos);
    CHECK(s.find("\"bone_count\"") == std::string::npos);

    cook::JsonDoc doc;
    cook::JsonError err;
    REQUIRE(doc.parse(text.view(), err));
    Skeleton2D back;
    cook::DiagList diags;
    REQUIRE(cook::read_scene_component(*type, doc.root(), &back, 0, "t.mcscene", "Skeleton2D", diags));
    CHECK(back.layer == 3);
    CHECK(back.pose[2] == sk.pose[2]);
    CHECK(back.pose[1] == Bone2DPose{});
    CHECK(back.world[0].x == 0.0f);
}

TEST_CASE("skeleton2d: install_stock installs the skeleton system") {
    World w;
    kit::install_stock(w, nullptr);
    CHECK(w.try_resource<kit::Skeleton2DRegistry>() != nullptr);
}
