// M-ANIM2D 3: attachments, meshes, skinning, slot and deform keys. Weighted and unweighted vertex
// positions with exact numbers, deform key interpolation and blending, slot color and attachment
// keys through the animator (cross-fade: the attachment switches, the color blends), the cook of
// attachments and its errors, the reader's rejects, the drawn output of skeleton2d_draws, and
// determinism with a save in the middle of a cross-fade.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/skeleton2d.h"
#include "mce/scene/scene.h"

using namespace mce;
using namespace cook_test;
using kit::Bone2DTransform;
using kit::Skeleton2D;
using kit::Skeleton2DAnimator;
using kit::Skeleton2DSlots;

namespace {

constexpr f32 kDt = 0.125f;  // a power of two: playheads add up exactly
constexpr const char* kRig = "5c2d0000-0000-4000-8000-000000000201";
constexpr const char* kScene = "5c2d0000-0000-4000-8000-000000000202";
constexpr u32 kBody = 0, kArm = 1, kHat = 2;  // slots
constexpr u32 kQuad = 0, kMesh = 1;           // the body slot's attachments
constexpr u32 kArmMesh = 2;                   // the arm mesh's index in the attachment array

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

// The rig: bone a at (1, 0) turned 90 degrees, bone b at (0, 2) scaled 2.
//   body (on root): a quad "quad" (2 x 1 m) and a weighted mesh "mesh", shown first:
//     v0: a (1, 0) w 1                      -> (1, 1)
//     v1: a (1, 0) w 0.5, b (1, 1) w 0.5    -> 0.5 (1, 1) + 0.5 (2, 4) = (1.5, 2.5)
//     v2: b (0, 0.5) w 3, root (0, 0) w 1   -> normalized 0.75 / 0.25: 0.75 (0, 3) = (0, 2.25)
//   arm (on b, tint 1 0.5 1 1): an unweighted mesh (0, 0), (1, 0), (0, 1) -> (0, 2), (2, 2), (0, 4)
//   hat (on root): its own quad (the step-1 form)
// Animations:
//   blink (loops, 1 s): body color 1 -> 0 (all channels), body attachment mesh then quad at 0.5,
//                       arm attachment arm then none at 0.75, arm deform 0 -> (0.5, 0, 0, 0, 0, -1)
//   still (loops, 1 s): body color (0, 0, 1, 1), body attachment quad
//   squash (1 s): arm deform 0 -> v2.y + 1 on the bezier [1/3, 1, 2/3, 1]
const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("skeleton2d_mesh_project");
        write_file(r + "/assets/rig.mcskel2d", std::string(R"({ "format": "mce-skeleton2d", "version": 1,
            "guid": ")") + kRig + R"(", "name": "rig",
            "bones": [ { "name": "root" }, { "name": "a", "parent": "root", "x": 1, "rotation": 90 },
                       { "name": "b", "parent": "root", "y": 2, "scale": 2 } ],
            "slots": [
              { "name": "body", "bone": "root", "attachment": "mesh", "attachments": [
                  { "name": "quad", "size": [2, 1] },
                  { "name": "mesh", "type": "mesh", "vertices": [
                      { "uv": [0, 0], "weights": [ { "bone": "a", "x": 1, "y": 0, "weight": 1 } ] },
                      { "uv": [1, 0], "weights": [ { "bone": "a", "x": 1, "weight": 0.5 },
                                                   { "bone": "b", "x": 1, "y": 1, "weight": 0.5 } ] },
                      { "uv": [0, 1], "weights": [ { "bone": "b", "y": 0.5, "weight": 3 }, { "bone": "root", "weight": 1 } ] } ],
                    "triangles": [0, 1, 2] } ] },
              { "name": "arm", "bone": "b", "tint": [1, 0.5, 1, 1], "attachments": [
                  { "name": "arm", "type": "mesh",
                    "vertices": [ { "uv": [0, 0] }, { "uv": [1, 0], "x": 1 }, { "uv": [0, 1], "y": 1 } ],
                    "triangles": [0, 1, 2] } ] },
              { "name": "hat", "bone": "root", "size": [1, 1], "offset": [0, 3] } ],
            "animations": [
              { "name": "blink", "duration": 1,
                "slots": { "body": { "color": [ { "t": 0, "color": [1, 1, 1, 1] }, { "t": 1, "color": [0, 0, 0, 0] } ],
                                     "attachment": [ { "t": 0, "name": "mesh" }, { "t": 0.5, "name": "quad" } ] },
                           "arm": { "attachment": [ { "t": 0, "name": "arm" }, { "t": 0.75, "name": null } ] } },
                "deform": { "arm": { "arm": [ { "t": 0 }, { "t": 1, "offsets": [0.5, 0, 0, 0, 0, -1] } ] } } },
              { "name": "still", "duration": 1,
                "slots": { "body": { "color": [ { "t": 0, "color": [0, 0, 1, 1] } ],
                                     "attachment": [ { "t": 0, "name": "quad" } ] } } },
              { "name": "squash", "deform": { "arm": { "arm": [ { "t": 0, "curve": [0.3333333, 1, 0.6666667, 1] },
                                                              { "t": 1, "offsets": [0, 0, 0, 0, 0, 1] } ] } } } ] })");
        write_file(r + "/assets/stage.mcscene", std::string(R"({ "format": "mce-scene", "version": 1,
            "guid": ")") + kScene + R"(", "name": "stage", "entities": [
            { "name": "Rig", "components": {
                "SaveKey": { "key": 1 },
                "LocalTransform": { "position": [10, 0, 0] },
                "Skeleton2D": { "skeleton": "guid:)" +
                                                    kRig + R"(" },
                "Skeleton2DAnimator": { "clip": "blink", "fade": 0.5 } } } ] })");
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
    Array<Entity> e;
    u64 ticks = 0;

    Stage() {
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        world.register_components<LocalTransform, SaveKey>();
        kit::install_skeleton2d(world, &assets);
        REQUIRE(scene::load(world, assets, asset::parse_guid(kScene).value(), &e));
        REQUIRE(e.size() == 1);
    }
    Skeleton2D& rig() { return *world.try_get_mut<Skeleton2D>(e[0]); }
    Skeleton2DAnimator& anim() { return *world.try_get_mut<Skeleton2DAnimator>(e[0]); }
    const Skeleton2DSlots* slots() { return world.try_get<Skeleton2DSlots>(e[0]); }
    const kit::Skeleton2DRegistry::Def& def() {
        const kit::Skeleton2DRegistry::Def* d =
            world.try_resource<kit::Skeleton2DRegistry>()->resolve(rig().skeleton.guid);
        REQUIRE(d);
        return *d;
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
    // The arm mesh's deform offsets as the extract samples them now.
    Array<f32> deform() {
        kit::Skeleton2DDeform d;
        kit::Skeleton2DBlend b;
        b.deform = &d;
        kit::skeleton2d_animator_sample(def(), anim(), nullptr, b);
        Array<f32> out;
        if (const f32* v = d.of(kArmMesh))
            for (u32 i = 0; i < 6; ++i) out.push_back(v[i]);
        return out;
    }
};

const Bone2DTransform kAt = kit::transform_2d({10, 0, 0}, rotation_z(0.0f), {1, 1, 1});

// The payload of the cooked rig, copied so tests can break it.
Array<byte> rig_payload() {
    Stage s;
    const asset::AssetKey k = s.assets.find(asset::parse_guid(kRig).value());
    REQUIRE(k != asset::kInvalidAssetKey);
    auto h = s.assets.load_sync<asset::Skeleton2D>(k);
    REQUIRE(h);
    Array<byte> out;
    for (byte b : s.assets.resolve(h.value())) out.push_back(b);
    s.assets.release(asset::AnyHandle(h.value()));
    return out;
}

}  // namespace

TEST_CASE("skeleton2d mesh: attachments, meshes and slot keys cook into CHNK_SKEL2D minor 2") {
    const Array<byte> bytes = rig_payload();
    auto view = asset::Skeleton2DView::read(bytes.span());
    REQUIRE(view);
    const asset::Skeleton2DView& v = view.value();
    CHECK(v.header->flags == asset::Skeleton2DHeaderFlags::Attachments);
    REQUIRE(v.slots.size() == 3);
    CHECK(v.find_slot("hat") == static_cast<i32>(kHat));
    const asset::Skeleton2DSlot& body = v.slots[kBody];
    CHECK(body.attachment_count == 2);
    CHECK(body.first_attachment == 0);
    CHECK(body.attachment == kMesh);             // "attachment": "mesh"
    CHECK(v.slots[kArm].attachment == 0);        // the first listed by default
    CHECK(v.slots[kHat].attachment_count == 0);  // its own quad, as before
    REQUIRE(v.attachments.size() == 3);
    CHECK(v.string(v.attachments[kQuad].name) == StringView("quad"));
    CHECK(v.attachments[kQuad].kind == asset::Skeleton2DAttachmentKind::Quad);
    CHECK(v.attachments[kQuad].size[0] == 2.0f);
    const asset::Skeleton2DAttachment& mesh = v.attachments[kMesh];
    CHECK(mesh.kind == asset::Skeleton2DAttachmentKind::Mesh);
    CHECK(mesh.vertex_count == 3);
    CHECK(mesh.index_count == 3);
    CHECK(v.attachments[kArmMesh].slot == kArm);
    REQUIRE(v.vertices.size() == 6);
    REQUIRE(v.weights.size() == 5);
    // The third vertex's weights 3 and 1 are normalized to 0.75 and 0.25.
    const asset::Skeleton2DVertex& v2 = v.vertices[2];
    CHECK(v2.weight_count == 2);
    CHECK(v.weights[v2.first_weight].weight == 0.75f);
    CHECK(v.weights[v2.first_weight + 1].weight == 0.25f);
    CHECK(v.weights[v2.first_weight + 1].bone == 0);
    CHECK(v.vertices[4].weight_count == 0);  // the arm's (1, 0)
    CHECK(v.vertices[4].x == 1.0f);
    CHECK(v.vertices[5].v == 1.0f);
    REQUIRE(v.animations.size() == 3);
    CHECK(v.animations[2].duration == 1.0f);  // squash: the last deform key
    const asset::Skeleton2DAnimation& blink = v.animations[0];
    REQUIRE(blink.timeline_count == 4);  // body color, body attachment, arm attachment, arm deform
    const asset::Skeleton2DTimeline& color = v.timelines[blink.first_timeline];
    CHECK(color.channel == asset::Skeleton2DChannel::Color);
    CHECK(color.bone == kBody);
    CHECK(v.values[v.slot_keys[color.first_key + 1].value + 3] == 0.0f);
    const asset::Skeleton2DTimeline& arm = v.timelines[blink.first_timeline + 2];
    CHECK(arm.channel == asset::Skeleton2DChannel::Attachment);
    CHECK(v.slot_keys[arm.first_key + 1].value == asset::kSkeleton2DNoAttachmentKey);
    CHECK(v.slot_keys[arm.first_key + 1].curve == asset::Skeleton2DCurve::Stepped);
    const asset::Skeleton2DTimeline& deform = v.timelines[blink.first_timeline + 3];
    CHECK(deform.channel == asset::Skeleton2DChannel::Deform);
    CHECK(deform.attachment == kArmMesh);
    CHECK(v.values[v.slot_keys[deform.first_key].value] == 0.0f);  // absent offsets: zeros
    CHECK(v.values[v.slot_keys[deform.first_key + 1].value + 5] == -1.0f);
}

TEST_CASE("skeleton2d mesh: weighted vertices sum their bones, unweighted ones ride the slot bone") {
    Stage s;
    const kit::Skeleton2DRegistry::Def& def = s.def();
    // A slot without listed attachments gets its own quad, appended after the chunk's three.
    REQUIRE(def.attachments.size() == 4);
    CHECK(def.slots[kHat].first_attachment == 3);
    CHECK(def.find_attachment(kHat, "hat") == 0);
    CHECK(def.find_attachment(kBody, "quad") == 0);
    CHECK(def.find_attachment(kBody, "nope") == -1);
    CHECK(def.slot_timelines);

    Skeleton2D sk;
    kit::Skeleton2DDraws d;
    kit::skeleton2d_draws(def, sk, nullptr, nullptr, kAt, d);
    // The setup: body shows its mesh, arm its mesh, hat its quad. Slot order across both lists.
    REQUIRE(d.meshes.size() == 2);
    REQUIRE(d.quads.size() == 1);
    CHECK(d.meshes[0].slot == kBody);
    CHECK(d.meshes[1].slot == kArm);
    CHECK(d.quads[0].slot == kHat);
    CHECK(d.quads[0].position.x == 10.0f);
    CHECK(d.quads[0].position.y == 3.0f);
    REQUIRE(d.vertices.size() == 6);
    const kit::Skeleton2DMeshVertex* v = d.vertices.data();
    // Bone a turns 90 degrees: cos is 4e-8 off zero in f32, hence Approx on its terms.
    CHECK(v[0].position.x == doctest::Approx(11.0f).epsilon(1e-6));
    CHECK(v[0].position.y == 1.0f);
    CHECK(v[1].position.x == doctest::Approx(11.5f).epsilon(1e-6));
    CHECK(v[1].position.y == 2.5f);
    CHECK(v[2].position.x == 10.0f);  // b and root only: exact
    CHECK(v[2].position.y == 2.25f);
    CHECK(v[1].uv.x == 1.0f);
    CHECK(v[2].uv.y == 1.0f);
    // The arm: unweighted, in bone b's space (at (0, 2), scale 2).
    CHECK(v[3].position.x == 10.0f);
    CHECK(v[3].position.y == 2.0f);
    CHECK(v[4].position.x == 12.0f);
    CHECK(v[4].position.y == 2.0f);
    CHECK(v[5].position.x == 10.0f);
    CHECK(v[5].position.y == 4.0f);
    CHECK(d.meshes[1].first_vertex == 3);
    CHECK(d.meshes[1].first_index == 3);
    CHECK(d.indices.size() == 6);
    CHECK(d.indices[5] == 2);           // relative to the mesh's first vertex
    CHECK(d.meshes[1].tint.g == 0.5f);  // the arm slot's tint
    CHECK(d.meshes[1].rotation == 0.0f);
    CHECK_FALSE(d.meshes[1].mirrored);

    // Pose offsets move the bones: b one metre right moves the arm and the body's b-weighted parts.
    sk.pose[2].x = 1.0f;
    d.clear();
    kit::skeleton2d_draws(def, sk, nullptr, nullptr, kAt, d);
    CHECK(d.vertices[1].position.x == doctest::Approx(12.0f).epsilon(1e-6));  // half on b: + 0.5
    CHECK(d.vertices[2].position.x == 10.75f);                                // 0.75 on b
    CHECK(d.vertices[3].position.x == 11.0f);
    // flip_x mirrors the skeleton: x mirrors about the entity, the meshes are mirrored.
    sk.pose[2].x = 0.0f;
    sk.flip_x = true;
    d.clear();
    kit::skeleton2d_draws(def, sk, nullptr, nullptr, kAt, d);
    CHECK(d.vertices[4].position.x == 8.0f);
    CHECK(d.vertices[2].position.y == 2.25f);
    CHECK(d.meshes[1].mirrored);
    // The step-1 entry point keeps returning the quads only.
    Array<kit::Skeleton2DQuad> quads;
    kit::skeleton2d_quads(def, sk, kAt, quads);
    REQUIRE(quads.size() == 1);
    CHECK(quads[0].slot == kHat);
}

TEST_CASE("skeleton2d mesh: Skeleton2DSlots picks attachments and colors; none or alpha 0 hides") {
    Stage s;
    const kit::Skeleton2DRegistry::Def& def = s.def();
    Skeleton2DSlots slots;
    kit::skeleton2d_slots_setup(def, slots);
    CHECK(slots.slot_count == 3);
    CHECK(slots.attachment[kBody] == kMesh);
    CHECK(slots.color[kArm].g == 0.5f);
    CHECK(slots.color[kHat] == math::Color::white());
    slots.attachment[kBody] = kQuad;
    slots.attachment[kArm] = asset::kSkeleton2DNoAttachment;
    slots.color[kBody] = {0.5f, 1, 1, 1};
    Skeleton2D sk;
    sk.tint = {1, 1, 0.5f, 1};
    kit::Skeleton2DDraws d;
    kit::skeleton2d_draws(def, sk, &slots, nullptr, kAt, d);
    CHECK(d.meshes.empty());
    REQUIRE(d.quads.size() == 2);
    CHECK(d.quads[0].slot == kBody);
    CHECK(d.quads[0].scale.x == 2.0f);  // the body's quad attachment
    CHECK(d.quads[0].tint.r == 0.5f);   // slot color x component tint
    CHECK(d.quads[0].tint.b == 0.5f);
    slots.color[kHat].a = 0.0f;  // alpha 0 hides
    d.clear();
    kit::skeleton2d_draws(def, sk, &slots, nullptr, kAt, d);
    REQUIRE(d.quads.size() == 1);
    CHECK(d.quads[0].slot == kBody);
    // A component not set up yet (slot_count 0) draws the setup.
    Skeleton2DSlots fresh;
    d.clear();
    kit::skeleton2d_draws(def, sk, &fresh, nullptr, kAt, d);
    CHECK(d.meshes.size() == 2);
    CHECK(d.quads.size() == 1);
}

TEST_CASE("skeleton2d mesh: deform keys interpolate, blend by weight and move the vertices") {
    Stage s;
    const kit::Skeleton2DRegistry::Def& def = s.def();
    kit::Skeleton2DDeform deform;
    {
        kit::Skeleton2DBlend b;
        b.deform = &deform;
        b.add(def, 0, 0.5f, 1.0f);  // blink at 0.5: half of (0.5, 0, 0, 0, 0, -1)
    }
    const f32* o = deform.of(kArmMesh);
    REQUIRE(o);
    CHECK(deform.of(kMesh) == nullptr);  // not keyed
    CHECK(o[0] == 0.25f);
    CHECK(o[1] == 0.0f);
    CHECK(o[5] == -0.5f);
    // In bone b's space (scale 2): v0 (0.25, 0) lands at x + 0.5, v2 (0, 1 - 0.5) at y 3.
    Skeleton2D sk;
    kit::Skeleton2DDraws d;
    kit::skeleton2d_draws(def, sk, nullptr, &deform, kAt, d);
    REQUIRE(d.vertices.size() == 6);
    CHECK(d.vertices[3].position.x == 10.5f);
    CHECK(d.vertices[5].position.y == 3.0f);
    CHECK(d.vertices[4].position.x == 12.0f);  // offset 0

    // Cross-fade weights: blink at half weight against a clip that does not key the mesh (zero).
    deform.clear();
    {
        kit::Skeleton2DBlend b;
        b.deform = &deform;
        b.add(def, 0, 0.5f, 0.5f);
        b.add(def, 1, 0.5f, 0.5f);
    }
    CHECK(deform.of(kArmMesh)[0] == 0.125f);
    CHECK(deform.of(kArmMesh)[5] == -0.25f);
    // A bezier deform key: x(s) = s for cx 1/3, 2/3, so u = 0.5 eases to 0.875.
    deform.clear();
    {
        kit::Skeleton2DBlend b;
        b.deform = &deform;
        b.add(def, 2, 0.5f, 1.0f);
    }
    CHECK(deform.of(kArmMesh)[5] == doctest::Approx(0.875f).epsilon(1e-5));
    CHECK(deform.of(kArmMesh)[4] == 0.0f);
}

TEST_CASE("skeleton2d mesh: color and attachment keys drive Skeleton2DSlots through the animator") {
    Stage s;
    CHECK(s.slots() == nullptr);
    s.run(1);  // the starting tick: blink at 0; the animator adds the slots component
    REQUIRE(s.slots());
    CHECK(s.slots()->slot_count == 3);
    CHECK(s.slots()->attachment[kBody] == kMesh);
    CHECK(s.slots()->color[kBody] == math::Color::white());
    CHECK(s.slots()->color[kArm].g == 0.5f);  // not keyed: the setup tint
    s.run(2);                                 // 0.25
    CHECK(s.anim().time == 0.25f);
    CHECK(s.slots()->color[kBody] == math::Color{0.75f, 0.75f, 0.75f, 0.75f});
    CHECK(s.slots()->attachment[kBody] == kMesh);
    const Array<f32> d = s.deform();
    REQUIRE(d.size() == 6);
    CHECK(d[0] == 0.125f);
    CHECK(d[5] == -0.25f);
    s.run(2);  // 0.5: stepped to the quad
    CHECK(s.slots()->attachment[kBody] == kQuad);
    CHECK(s.slots()->color[kBody].a == 0.5f);
    CHECK(s.slots()->attachment[kArm] == 0);
    s.run(2);  // 0.75: the arm hides
    CHECK(s.slots()->attachment[kArm] == asset::kSkeleton2DNoAttachment);
    CHECK(s.slots()->attachment[kHat] == 0);  // never keyed
    // Gameplay owns slots no clip keys.
    s.world.try_get_mut<Skeleton2DSlots>(s.e[0])->color[kHat] = {0, 1, 0, 1};
    s.run(1);
    CHECK(s.slots()->color[kHat] == math::Color{0, 1, 0, 1});
    s.run(1);  // 1.0 wraps to 0: back to the mesh and white, the arm shows again
    CHECK(s.anim().time == 0.0f);
    CHECK(s.slots()->attachment[kBody] == kMesh);
    CHECK(s.slots()->attachment[kArm] == 0);
    CHECK(s.slots()->color[kBody] == math::Color::white());
}

TEST_CASE("skeleton2d mesh: in a cross-fade the attachment switches and the color blends") {
    Stage s;
    s.run(3);                 // blink at 0.25
    s.anim().clip = "still";  // fade 0.5
    s.run(1);                 // the switching tick: still has weight 0, blink holds at 0.25
    CHECK(s.anim().blend == 0.0f);
    CHECK(s.slots()->attachment[kBody] == kMesh);
    CHECK(s.slots()->color[kBody] == math::Color{0.75f, 0.75f, 0.75f, 0.75f});
    s.run(1);  // blend 0.25: still at 0.25, blink at 0.375 with 0.75
    CHECK(s.anim().blend == 0.25f);
    CHECK(s.anim().previous_time == 0.375f);
    // still keys the body's attachment and now has weight: it switches at once.
    CHECK(s.slots()->attachment[kBody] == kQuad);
    // Only blink keys the arm's attachment: it keeps blink's.
    CHECK(s.slots()->attachment[kArm] == 0);
    // Color: 0.25 (0, 0, 1, 1) + 0.75 (0.625, 0.625, 0.625, 0.625).
    CHECK(s.slots()->color[kBody] == math::Color{0.46875f, 0.46875f, 0.71875f, 0.71875f});
    // Deform: blink's offsets at 0.375 times 0.75; still adds zero.
    const Array<f32> d = s.deform();
    REQUIRE(d.size() == 6);
    CHECK(d[0] == 0.140625f);
    CHECK(d[5] == -0.28125f);
    s.run(2);  // the fade ends: still alone
    CHECK(s.anim().blend == 0.75f);
    s.run(2);
    CHECK(s.anim().blend == 1.0f);
    CHECK(s.slots()->color[kBody] == math::Color{0, 0, 1, 1});
    CHECK(s.deform().empty());  // nothing keys the mesh: no offsets
}

TEST_CASE("skeleton2d mesh: two runs hash the same, and a save mid-fade resumes identically") {
    auto script = [](Stage& s, u64 tick) {
        if (tick == 3) s.anim().clip = "still";
        if (tick == 9) s.anim().clip = "blink";
    };
    Stage a, b;
    for (u32 i = 0; i < 16; ++i) {
        script(a, a.ticks);
        script(b, b.ticks);
        a.run(1);
        b.run(1);
        REQUIRE(a.world.state_hash() == b.world.state_hash());
    }

    Stage c;
    for (u32 i = 0; i < 5; ++i) {
        script(c, c.ticks);
        c.run(1);
    }
    REQUIRE(c.anim().blend == 0.25f);  // mid-fade
    Array<byte> bytes;
    save::write_save(c.world, save::SaveInfo{}, bytes);
    Stage d;  // the scene as authored: no Skeleton2DSlots yet
    d.ticks = c.ticks;
    REQUIRE(save::read_save(d.world, bytes.span()));
    REQUIRE(d.slots());
    for (u32 i = 0; i < 3; ++i) {
        CHECK(d.slots()->attachment[i] == c.slots()->attachment[i]);
        CHECK(d.slots()->color[i] == c.slots()->color[i]);
    }
    CHECK(d.slots()->slot_count == 3);
    // The deform offsets come from the saved animator state: the same bits.
    const Array<f32> dc = c.deform(), dd = d.deform();
    REQUIRE(dc.size() == 6);
    REQUIRE(dd.size() == 6);
    for (u32 i = 0; i < 6; ++i) CHECK(std::bit_cast<u32>(dc[i]) == std::bit_cast<u32>(dd[i]));
    for (u32 i = 0; i < 3; ++i) {
        script(c, c.ticks);
        script(d, d.ticks);
        c.run(1);
        d.run(1);
        CHECK(d.world.state_hash() == c.world.state_hash());
    }
    CHECK(d.slots()->attachment[kBody] == c.slots()->attachment[kBody]);
    CHECK(d.slots()->color[kBody] == c.slots()->color[kBody]);
}

TEST_CASE("skeleton2d mesh: the reader rejects bad attachments, meshes and slot keys") {
    const Array<byte> good = rig_payload();
    REQUIRE(asset::Skeleton2DView::read(good.span()));
    CHECK_FALSE(asset::Skeleton2DView::read(good.span().subspan(0, good.size() - 1)));
    // Breaks a copy through the view's spans (they point into the copy).
    auto rejects = [&](auto&& mutate) {
        Array<byte> c;
        for (byte b : good) c.push_back(b);
        auto v = asset::Skeleton2DView::read(c.span());
        REQUIRE(v);
        mutate(v.value());
        return !asset::Skeleton2DView::read(c.span());
    };
    using V = asset::Skeleton2DView;
    auto mut = [](const auto& span, usize i) -> auto& {
        using T = std::remove_cvref_t<decltype(span[0])>;
        return const_cast<T&>(span[i]);
    };
    auto header = [](const V& v) -> asset::Skeleton2DHeader& {
        return const_cast<asset::Skeleton2DHeader&>(*v.header);
    };
    const u32 blink = 0;
    auto tl = [&](const V& v, u32 i) -> asset::Skeleton2DTimeline& {
        return mut(v.timelines, v.animations[blink].first_timeline + i);
    };
    CHECK(rejects([&](const V& v) { header(v).flags = 3; }));              // an unknown flag
    CHECK(rejects([&](const V& v) { mut(v.indices, 1) = 3; }));            // index past the mesh's vertices
    CHECK(rejects([&](const V& v) { mut(v.weights, 0).weight = 0.5f; }));  // weights sum to 0.5
    CHECK(rejects([&](const V& v) { mut(v.weights, 0).weight = std::nanf(""); }));
    CHECK(rejects([&](const V& v) { mut(v.weights, 0).bone = 3; }));            // no such bone
    CHECK(rejects([&](const V& v) { mut(v.vertices, 0).weight_count = 5; }));   // more than 4 weights
    CHECK(rejects([&](const V& v) { mut(v.vertices, 5).first_weight = 99; }));  // unweighted: range still checked
    CHECK(rejects([&](const V& v) {
        mut(v.vertices, 3).weight_count = 1;
        mut(v.vertices, 3).first_weight = 5;
    }));
    CHECK(rejects([&](const V& v) { mut(v.vertices, 0).u = INFINITY; }));
    CHECK(rejects([&](const V& v) { mut(v.attachments, kMesh).vertex_count = 2; }));  // fewer than 3
    CHECK(rejects([&](const V& v) { mut(v.attachments, kMesh).index_count = 2; }));   // not whole triangles
    CHECK(rejects([&](const V& v) { mut(v.attachments, kMesh).first_vertex = 5; }));  // past the vertices
    CHECK(
        rejects([&](const V& v) { mut(v.attachments, kMesh).kind = static_cast<asset::Skeleton2DAttachmentKind>(2); }));
    CHECK(rejects([&](const V& v) { mut(v.attachments, kQuad).slot = kArm; }));  // listed by another slot
    CHECK(rejects([&](const V& v) { mut(v.attachments, kQuad).size[0] = std::nanf(""); }));
    CHECK(rejects([&](const V& v) { mut(v.slots, kBody).attachment = 2; }));       // the body has 2
    CHECK(rejects([&](const V& v) { mut(v.slots, kHat).attachment = 1; }));        // its own quad is 0
    CHECK(rejects([&](const V& v) { mut(v.slots, kArm).first_attachment = 3; }));  // past the attachments
    CHECK(rejects([&](const V& v) { mut(v.slots, kBody).attachment_count = 65; }));
    CHECK(rejects([&](const V& v) { tl(v, 3).attachment = kQuad; }));  // deform on a quad
    CHECK(rejects([&](const V& v) { tl(v, 3).attachment = kMesh; }));  // a mesh of another slot
    CHECK(rejects([&](const V& v) { tl(v, 0).bone = 3; }));            // no such slot
    CHECK(rejects([&](const V& v) { tl(v, 0).attachment = 1; }));      // only deform names one
    CHECK(rejects([&](const V& v) { tl(v, 0).channel = static_cast<asset::Skeleton2DChannel>(6); }));
    CHECK(rejects([&](const V& v) { tl(v, 0).key_count = 99; }));                         // past the slot keys
    CHECK(rejects([&](const V& v) { mut(v.slot_keys, tl(v, 2).first_key).value = 1; }));  // the arm lists 1
    CHECK(rejects([&](const V& v) { mut(v.slot_keys, tl(v, 2).first_key).curve = asset::Skeleton2DCurve::Linear; }));
    CHECK(rejects([&](const V& v) { mut(v.slot_keys, tl(v, 0).first_key + 1).t = 0.0f; }));  // not increasing
    CHECK(rejects(
        [&](const V& v) { mut(v.slot_keys, tl(v, 0).first_key).value = static_cast<u32>(v.values.size()) - 3; }));
    CHECK(rejects(
        [&](const V& v) { mut(v.slot_keys, tl(v, 3).first_key + 1).value = static_cast<u32>(v.values.size()) - 5; }));
    CHECK(rejects([&](const V& v) { mut(v.values, 0) = std::nanf(""); }));
}

TEST_CASE("skeleton2d mesh: a chunk without the attachments flag reads as before and has no attachment fields") {
    struct Parts {
        asset::Skeleton2DHeader h{};
        asset::Skeleton2DBone bone{};
        asset::Skeleton2DSlot slot{};
        char pool[8] = "root\0s";
    };
    Parts base;
    base.h.bone_count = 1;
    base.h.slot_count = 1;
    base.h.string_bytes = sizeof base.pool;
    base.bone.parent = asset::kSkeleton2DNoParent;
    base.bone.scale_x = base.bone.scale_y = 1.0f;
    base.slot.name = 5;
    base.slot.size[0] = base.slot.size[1] = 1.0f;
    auto reads = [](const Parts& p) {
        Array<byte> b;
        b.resize(sizeof p, byte{0});
        std::memcpy(b.data(), &p, sizeof p);
        return static_cast<bool>(asset::Skeleton2DView::read(b.span()));
    };
    static_assert(sizeof(Parts) == 64 + 48 + 96 + 8);
    CHECK(reads(base));
    Parts c = base;
    c.slot.attachment_count = 1;
    CHECK_FALSE(reads(c));
    c = base;
    c.slot.attachment = asset::kSkeleton2DNoAttachment;  // hiding needs minor 2
    CHECK_FALSE(reads(c));
    c = base;
    c.slot.first_attachment = 1;
    CHECK_FALSE(reads(c));
    // The runtime gives the slot its own quad as attachment 0, named after it.
    Array<byte> b;
    b.resize(sizeof base, byte{0});
    std::memcpy(b.data(), &base, sizeof base);
    kit::Skeleton2DRegistry reg;
    REQUIRE(reg.define(asset::Guid{3, 4}, b.span()));
    const kit::Skeleton2DRegistry::Def* def = reg.resolve(asset::Guid{3, 4});
    REQUIRE(def);
    REQUIRE(def->attachments.size() == 1);
    CHECK(def->find_attachment(0, "s") == 0);
    CHECK(def->slots[0].attachment_count == 1);
    CHECK_FALSE(def->slot_timelines);
}

TEST_CASE("skeleton2d mesh: cook errors name the slot, the attachment and the vertex") {
    struct Case {
        const char* slots;
        const char* anims;
        const char* expect;
    };
    const char* mesh = R"("vertices": [ { "uv": [0, 0] }, { "uv": [1, 0], "x": 1 }, { "uv": [0, 1], "y": 1 } ])";
    const std::string m =
        std::string(R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "m", "type": "mesh", )") + mesh +
        R"(, "triangles": [0, 1, 2] }, { "name": "q" } ] } ])";
    const Case cases[] = {
        {R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "m", "type": "mesh", "vertices": [ { "uv": [0, 0] }, { "uv": [1, 0] }, { "uv": [0, 1] } ], "triangles": [0, 1, 3] } ] } ])",
         "[]", "slot 's', attachment 'm': triangle index 2 must be a vertex index below 3"},
        {R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "m", "type": "mesh", "vertices": [ { "uv": [0, 0] }, { "uv": [1, 0] } ], "triangles": [0, 1, 1] } ] } ])",
         "[]", "a mesh's 'vertices' must be an array of 3 to 1024 vertices"},
        {R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "m", "type": "mesh", "vertices": [ { "uv": [0, 0], "weights": [ { "bone": "leg", "weight": 1 } ] }, { "uv": [1, 0] }, { "uv": [0, 1] } ], "triangles": [0, 1, 2] } ] } ])",
         "[]", "slot 's', attachment 'm': vertex 0: a weight's 'bone' must name a bone"},
        {R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "m", "type": "mesh", "vertices": [ { "uv": [0, 0] }, { "uv": [1, 0], "weights": [ { "bone": "arm", "weight": 0 } ] }, { "uv": [0, 1] } ], "triangles": [0, 1, 2] } ] } ])",
         "[]", "vertex 1: a vertex's weights must not all be 0"},
        {R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "m", "type": "mesh", "vertices": [ { "uv": [0, 0], "x": 1, "weights": [ { "bone": "arm", "weight": 1 } ] }, { "uv": [1, 0] }, { "uv": [0, 1] } ], "triangles": [0, 1, 2] } ] } ])",
         "[]", "a weighted vertex has its positions in 'weights'"},
        {R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "m", "type": "mesh", "vertices": [ { "uv": [0, 0], "weights": [ { "bone": "arm", "weight": 1 }, { "bone": "arm", "weight": 1 }, { "bone": "arm", "weight": 1 }, { "bone": "arm", "weight": 1 }, { "bone": "arm", "weight": 1 } ] }, { "uv": [1, 0] }, { "uv": [0, 1] } ], "triangles": [0, 1, 2] } ] } ])",
         "[]", "'weights' must be an array of 1 to 4"},
        {R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "m", "type": "mesh", "vertices": [ { "x": 0 }, { "uv": [1, 0] }, { "uv": [0, 1] } ], "triangles": [0, 1, 2] } ] } ])",
         "[]", "vertex 0: a vertex needs its texture coordinates 'uv'"},
        {R"([ { "name": "s", "bone": "arm", "rect": [0, 0, 1, 1], "attachments": [ { "name": "q" } ] } ])", "[]",
         "slot 's' lists 'attachments': put 'rect' in an attachment"},
        {R"([ { "name": "s", "bone": "arm", "attachment": "z", "attachments": [ { "name": "q" } ] } ])", "[]",
         "'attachment' must name one of the slot's attachments, or be null"},
        {R"([ { "name": "s", "bone": "arm", "attachment": "q" } ])", "[]",
         "a slot without 'attachments' shows its own quad"},
        {R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "q" }, { "name": "q" } ] } ])", "[]",
         "slot 's', attachment 'q': two attachments of the slot share this name"},
        {R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "q", "type": "spline" } ] } ])", "[]",
         "slot 's', attachment 'q': 'type' must be \"quad\" or \"mesh\""},
        {R"([ { "name": "s", "bone": "arm", "attachments": [ { "name": "q", "vertices": [] } ] } ])", "[]", "vertices"},
        {"[]", R"([ { "name": "walk", "slots": { "hand": { "color": [ { "t": 0 } ] } } } ])",
         "animation 'walk', slot 'hand': no slot has that name"},
        {m.c_str(), R"([ { "name": "walk", "slots": { "s": { "attachment": [ { "t": 0, "name": "z" } ] } } } ])",
         "animation 'walk', slot 's': an attachment key's 'name' must name one of the slot's attachments, or be null"},
        {m.c_str(), R"([ { "name": "walk", "slots": { "s": { "color": [ { "t": 1 }, { "t": 0.5 } ] } } } ])",
         "animation 'walk', slot 's': color keys must be in increasing time order"},
        {m.c_str(), R"([ { "name": "walk", "slots": { "s": { "color": [ { "t": 0, "color": [1, 1, 1] } ] } } } ])",
         "'color' must be an array of 4 numbers"},
        {m.c_str(), R"([ { "name": "walk", "deform": { "s": { "q": [ { "t": 0 } ] } } } ])",
         "animation 'walk', slot 's', attachment 'q': deform keys need a mesh attachment of the slot"},
        {m.c_str(), R"([ { "name": "walk", "deform": { "s": { "m": [ { "t": 0, "offsets": [1, 2] } ] } } } ])",
         "animation 'walk', slot 's', attachment 'm': 'offsets' must be 6 numbers"},
        {m.c_str(), R"([ { "name": "walk", "deform": { "s": { "m": [] } } } ])",
         "'deform' must be an array of at least one key"},
    };
    u32 i = 0;
    for (const Case& c : cases) {
        const std::string root = fresh_dir("skeleton2d_mesh_errors_" + std::to_string(i++));
        write_file(
            root + "/assets/bad.mcskel2d",
            std::string(R"({ "format": "mce-skeleton2d", "version": 1, "guid": "5c2d0000-0000-4000-8000-0000000002f0",
    "bones": [ { "name": "root" }, { "name": "arm", "parent": "root" } ], "slots": )") +
                c.slots + R"(, "animations": )" + c.anims + " }");
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        const std::string text = diag_text(r);
        INFO(text);
        CHECK(text.find(c.expect) != std::string::npos);
        CHECK(text.find("bad.mcskel2d:") != std::string::npos);
    }
}
