// The Skeleton panel's editor (ROADMAP M-ANIM2D step 5), headless: the document edits that the MCP
// skel2d_* tools share, the real importer as the validator (bad edits are refused with its
// file:line message and change nothing), undo and redo landing on disk, a reopened file giving the
// same document, and the pose preview equal to the runtime's pose functions.

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/skeleton2d_editor.h"
#include "mce/kit/skeleton2d.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;
namespace sk = mce::editor::skel2d;

namespace {

constexpr const char* kArmTex = "5c2d0000-0000-4000-8000-0000000000a1";
constexpr f32 kDeg = 3.14159265358979f / 180.0f;

StringView sv(const std::string& s) { return StringView(s.data(), s.size()); }
std::string str(const cook::String& s) { return std::string(s.view().data(), s.view().size()); }

std::string file_text(const std::string& path) {
    cook::String s;
    REQUIRE(cook::fs::read_text(sv(path), s));
    return str(s);
}

std::string skel_project(const char* name) {
    const std::string r = fresh_dir(name);
    write_file(r + "/assets/arm.png", make_png(2, 2, std::vector<u8>(2 * 2 * 4, 255)));
    write_file(r + "/assets/arm.png.mceasset", sidecar(kArmTex, "texture", R"({ "srgb": true, "mips": false })"));
    return r;
}

sk::BoneSpec bone(const char* name, const char* parent, f64 x, f64 y, f64 rot = 0, f64 length = 0) {
    sk::BoneSpec b;
    b.name = cook::String(name);
    b.parent = cook::String(parent);
    b.x = x;
    b.y = y;
    b.rotation = rot;
    b.length = length;
    return b;
}

sk::KeySpec rotate_key(const char* anim, const char* bone_name, f64 t, f64 deg) {
    sk::KeySpec k;
    k.animation = cook::String(anim);
    k.channel = sk::Channel::Rotate;
    k.target = cook::String(bone_name);
    k.t = t;
    k.value = deg;
    return k;
}

// A rig: root, upper (1 m, along x), lower (1 m), a target bone; a textured slot; a two-bone IK.
void build_arm(Skeleton2DEditor& ed) {
    REQUIRE(ed.add_bone(bone("upper", "root", 0, 0, 0, 1)));
    REQUIRE(ed.add_bone(bone("lower", "upper", 1, 0, 0, 1)));
    REQUIRE(ed.add_bone(bone("aim", "root", 1, 1)));
    sk::SlotSpec s;
    s.name = cook::String("arm");
    s.bone = cook::String("upper");
    s.texture = cook::String(std::string("guid:") + kArmTex);
    s.size[0] = 1.0;
    s.size[1] = 0.25;
    REQUIRE(ed.add_slot(s));
    sk::IkSpec ik;
    ik.name = cook::String("reach");
    ik.bones.push_back(cook::String("upper"));
    ik.bones.push_back(cook::String("lower"));
    ik.target = cook::String("aim");
    REQUIRE(ed.add_ik(ik));
}

}  // namespace

TEST_CASE("skeleton2d editor: create, edit, save; the file cooks and reopens to the same document") {
    const std::string root = skel_project("skel_ed_roundtrip");
    EditorSession session;
    REQUIRE(session.open_project(sv(root)));
    Skeleton2DEditor ed;
    Result<asset::Guid> made = ed.create(session, "rigs/hero.mcskel2d");
    REQUIRE(made);
    CHECK(ed.is_open());
    REQUIRE(ed.info().bones.size() == 1);
    CHECK(ed.info().bones[0].name.view() == "root");
    CHECK(ed.info().title.view() == "hero");
    const std::string file = root + "/assets/rigs/hero.mcskel2d";
    CHECK(file_text(file).find("mce-skeleton2d") != std::string::npos);
    CHECK(ed.create(session, "rigs/hero.mcskel2d").error().code == Errc::AlreadyExists);

    build_arm(ed);
    REQUIRE(ed.key(rotate_key("wave", "upper", 0, 0)));
    REQUIRE(ed.key(rotate_key("wave", "upper", 1, 90)));
    REQUIRE(ed.key(rotate_key("wave", "upper", 0.5, 20)));  // merged by time
    sk::KeySpec tr;
    tr.animation = cook::String("wave");
    tr.channel = sk::Channel::Translate;
    tr.target = cook::String("root");
    tr.t = 1;
    tr.x = 2;
    REQUIRE(ed.key(tr));
    sk::KeySpec color;
    color.animation = cook::String("wave");
    color.channel = sk::Channel::SlotColor;
    color.target = cook::String("arm");
    color.t = 0.5;
    color.color[3] = 0.5;
    REQUIRE(ed.key(color));
    sk::KeySpec mix;
    mix.animation = cook::String("wave");
    mix.channel = sk::Channel::IkMix;
    mix.target = cook::String("reach");
    mix.t = 1;
    mix.value = 0.25;
    mix.has_duration = true;
    mix.duration = 2;
    REQUIRE(ed.key(mix));
    CHECK(ed.dirty());
    REQUIRE(ed.save(session));
    CHECK_FALSE(ed.dirty());

    // The real importer cooks the saved file; the definition carries what was added.
    const kit::Skeleton2DRegistry::Def* def = ed.def();
    REQUIRE(def != nullptr);
    CHECK(def->bones.size() == 4);
    CHECK(def->slots.size() == 1);
    CHECK(def->ik.size() == 1);
    REQUIRE(def->animations.size() == 1);
    CHECK(def->animations[0].duration == doctest::Approx(2.0f));
    CHECK(def->bones[2].rotation == doctest::Approx(0.0f));
    CHECK(ed.info().animations.size() == 1);

    // A second editor on the same file reads the same text.
    Skeleton2DEditor again;
    REQUIRE(again.open(session, made.value()));
    CHECK(str(cook::String(again.text())) == str(cook::String(ed.text())));
    CHECK(again.info().bones.size() == 4);
    CHECK(again.info().ik.size() == 1);
    CHECK(again.info().animations[0].name.view() == "wave");

    // The key list is sorted: three rotate keys at 0, 0.5, 1.
    cook::JsonDoc doc;
    cook::JsonError err;
    const std::string text = file_text(file);
    REQUIRE(doc.parse(sv(text), err));
    const cook::JsonValue keys = doc.root().get("animations").at(0).get("bones").get("upper").get("rotate");
    REQUIRE(keys.size() == 3);
    CHECK(keys.at(1).get("t").as_number() == 0.5);
    CHECK(keys.at(1).get("value").as_number() == 20.0);
    // The same time replaces the key.
    REQUIRE(ed.key(rotate_key("wave", "upper", 0.5, 30)));
    CHECK(ed.info().animations.size() == 1);
    CHECK(str(cook::String(ed.text())).find("\"value\": 30") != std::string::npos);
    CHECK(str(cook::String(ed.text())).find("\"value\": 20") == std::string::npos);
}

TEST_CASE("skeleton2d editor: the importer refuses bad edits with file:line text and nothing changes") {
    const std::string root = skel_project("skel_ed_errors");
    EditorSession session;
    REQUIRE(session.open_project(sv(root)));
    Skeleton2DEditor ed;
    REQUIRE(ed.create(session, "hero.mcskel2d"));
    const std::string before = str(cook::String(ed.text()));
    const u64 rev = ed.revision();

    CHECK_FALSE(ed.add_bone(bone("arm", "nobody", 0, 0)));
    CHECK(str(cook::String(ed.last_error())).find("hero.mcskel2d:") != std::string::npos);
    CHECK(str(cook::String(ed.last_error())).find("no bone has that name") != std::string::npos);
    CHECK_FALSE(ed.add_bone(bone("root", "", 0, 0)));  // a duplicate name
    CHECK(str(cook::String(ed.last_error())).find("share this name") != std::string::npos);

    sk::SlotSpec slot;
    slot.name = cook::String("body");
    slot.bone = cook::String("ghost");
    CHECK_FALSE(ed.add_slot(slot));
    slot.bone = cook::String("root");
    slot.texture = cook::String("guid:5c2d0000-0000-4000-8000-0000000000ff");  // not in the project
    CHECK_FALSE(ed.add_slot(slot));

    sk::IkSpec ik;
    ik.name = cook::String("bad");
    ik.bones.push_back(cook::String("root"));
    ik.bones.push_back(cook::String("root"));
    CHECK_FALSE(ed.add_ik(ik));

    CHECK_FALSE(ed.key(rotate_key("walk", "ghost", 0, 10)));
    CHECK(str(cook::String(ed.last_error())).find("walk") != std::string::npos);
    CHECK_FALSE(ed.key(rotate_key("", "root", 0, 10)));
    sk::SlotSpec nameless;
    CHECK_FALSE(ed.add_slot(nameless));
    CHECK_FALSE(ed.set_bone_rest(9, 0, 0, 0, 1, 1));

    CHECK(str(cook::String(ed.text())) == before);
    CHECK(ed.revision() == rev);
    CHECK_FALSE(ed.dirty());
    CHECK_FALSE(ed.apply_text("{ not json"));
    CHECK_FALSE(ed.apply_text("{\"format\": \"mce-skeleton2d\", \"version\": 1, \"bones\": []}"));
    CHECK(str(cook::String(ed.text())) == before);

    cook::String copy(before.c_str()), why;
    CHECK_FALSE(sk::set_bone_rest(copy, "ghost", 0, 0, 0, 1, 1, why));
}

TEST_CASE("skeleton2d editor: undo and redo land on disk, a run of edits is one step") {
    const std::string root = skel_project("skel_ed_undo");
    EditorSession session;
    REQUIRE(session.open_project(sv(root)));
    Skeleton2DEditor ed;
    REQUIRE(ed.create(session, "hero.mcskel2d"));
    const std::string file = root + "/assets/hero.mcskel2d";
    const std::string base = file_text(file);
    REQUIRE(ed.add_bone(bone("a", "root", 1, 0)));
    REQUIRE(ed.add_bone(bone("b", "a", 1, 0)));
    REQUIRE(ed.set_bone_rest(1, 2, 0.5, 15, 1, 1));
    CHECK(ed.info().bones[1].x == 2.0);
    CHECK(ed.info().bones[1].rotation == 15.0);
    REQUIRE(ed.save(session));
    REQUIRE(ed.add_bone(bone("c", "b", 1, 0)));
    CHECK(ed.can_undo());
    REQUIRE(ed.undo(session));  // the unsaved bone goes
    CHECK(ed.info().bones.size() == 3);
    CHECK(file_text(file) == str(cook::String(ed.text())));
    REQUIRE(ed.redo(session));
    CHECK(ed.info().bones.size() == 4);
    REQUIRE(ed.undo(session));
    REQUIRE(ed.undo(session));  // back before "a", "b" and the rest pose
    CHECK(file_text(file) == base);
    CHECK(ed.info().bones.size() == 1);
}

TEST_CASE("skeleton2d editor: the pose preview equals the runtime's pose functions") {
    const std::string root = skel_project("skel_ed_pose");
    EditorSession session;
    REQUIRE(session.open_project(sv(root)));
    Skeleton2DEditor ed;
    REQUIRE(ed.create(session, "hero.mcskel2d"));
    build_arm(ed);
    REQUIRE(ed.set_bone_rest(1, 0, 0, 30, 1, 1));  // upper rests at 30 degrees
    REQUIRE(ed.key(rotate_key("wave", "upper", 0, 0)));
    REQUIRE(ed.key(rotate_key("wave", "upper", 1, 90)));
    sk::KeySpec tr;
    tr.animation = cook::String("wave");
    tr.channel = sk::Channel::Translate;
    tr.target = cook::String("root");
    tr.t = 1;
    tr.x = 2;
    tr.y = -1;
    REQUIRE(ed.key(tr));
    sk::KeySpec stepped = rotate_key("wave", "lower", 0, 0);
    stepped.curve = cook::String("stepped");
    REQUIRE(ed.key(stepped));
    REQUIRE(ed.key(rotate_key("wave", "lower", 1, 40)));

    const kit::Skeleton2DRegistry::Def* def = ed.def();
    REQUIRE(def != nullptr);
    // No animation: the rest pose, then the constraint (it aims the arm at the "aim" bone).
    ed.set_animation("");
    kit::Skeleton2DIK rest_ik;
    kit::skeleton2d_ik_setup(*def, rest_ik);
    kit::Bone2DTransform want[4];
    kit::skeleton2d_solve_pose(*def, {}, &rest_ik, nullptr, Span<kit::Bone2DTransform>(want, 4));
    Span<const kit::Bone2DTransform> got = ed.pose();
    REQUIRE(got.size() == 4);
    for (usize i = 0; i < 4; ++i) CHECK(got[i] == want[i]);

    // A skeleton without IK: the rest pose is exactly skeleton2d_pose_offsets of the rest poses.
    Skeleton2DEditor plain_ed;
    REQUIRE(plain_ed.create(session, "plain.mcskel2d"));
    REQUIRE(plain_ed.add_bone(bone("a", "root", 0.5, 0.25, 33, 1)));
    REQUIRE(plain_ed.add_bone(bone("b", "a", 1, 0, -20, 1)));
    const kit::Skeleton2DRegistry::Def* pd = plain_ed.def();
    REQUIRE(pd != nullptr);
    kit::Bone2DTransform plain_want[3];
    kit::skeleton2d_pose_offsets(pd->bones.span(), {}, Span<kit::Bone2DTransform>(plain_want, 3));
    Span<const kit::Bone2DTransform> plain_got = plain_ed.pose();
    REQUIRE(plain_got.size() == 3);
    for (usize i = 0; i < 3; ++i) CHECK(plain_got[i] == plain_want[i]);
    CHECK(plain_got[1].x == doctest::Approx(0.5f));

    // An animation at 0.5 s: linear rotation 45 on upper, the translation held (one key), lower
    // stepped (still 0).
    ed.set_animation("wave");
    ed.set_time(0.5f);
    got = ed.pose();
    REQUIRE(got.size() == 4);
    kit::Bone2DPose offsets[4];
    offsets[1].rotation = 45.0f * kDeg;
    offsets[2].rotation = 0.0f;
    // Translate has one key at t = 1: held for every earlier time.
    offsets[0].x = 2.0f;
    offsets[0].y = -1.0f;
    kit::Bone2DTransform plain[4];
    kit::skeleton2d_pose_offsets(def->bones.span(), Span<const kit::Bone2DPose>(offsets, 4),
                                 Span<kit::Bone2DTransform>(plain, 4));
    // The IK constraint aims the arm at the aim bone, so the arm bones differ from `plain`, but the
    // untouched bones (root, aim) must match it exactly.
    CHECK(got[0] == plain[0]);
    CHECK(got[3] == plain[3]);
    // And the whole pose equals the runtime solve over the same offsets.
    kit::Skeleton2DIK ik;
    kit::skeleton2d_ik_setup(*def, ik);
    kit::Bone2DTransform solved[4];
    kit::skeleton2d_solve_pose(*def, Span<const kit::Bone2DPose>(offsets, 4), &ik, nullptr,
                               Span<kit::Bone2DTransform>(solved, 4));
    for (usize i = 0; i < 4; ++i) CHECK(got[i] == solved[i]);

    // An unknown animation reports and leaves the pose empty.
    Array<kit::Bone2DTransform> world;
    cook::String why;
    CHECK_FALSE(skeleton2d_pose(*def, "missing", 0.0f, {}, world, why));
    CHECK(str(why).find("missing") != std::string::npos);

    // An IK target in the skeleton's space: with mix 1 the tip of the arm lands on it.
    Skeleton2DIKTarget t;
    t.constraint = cook::String("reach");
    t.position = {1.2f, 0.8f};
    REQUIRE(skeleton2d_pose(*def, "", 0.0f, Span<const Skeleton2DIKTarget>(&t, 1), world, why));
    const math::vec2 tip = world[2].apply({1.0f, 0.0f});
    CHECK(tip.x == doctest::Approx(1.2f).epsilon(1e-4));
    CHECK(tip.y == doctest::Approx(0.8f).epsilon(1e-4));
    t.constraint = cook::String("nope");
    CHECK_FALSE(skeleton2d_pose(*def, "", 0.0f, Span<const Skeleton2DIKTarget>(&t, 1), world, why));

    // The bone lines: two per bone, the selected one brighter, from the origin to the tip.
    Array<OverlayLine> lines;
    kit::Bone2DTransform entity = kit::Bone2DTransform::trs({10.0f, 0.0f}, 0.0f, {1.0f, 1.0f});
    skeleton2d_bone_lines(*def, got, entity, 1, lines);
    REQUIRE(lines.size() == 8);
    const math::vec2 o = entity.apply(got[1].apply({0.0f, 0.0f}));
    CHECK(lines[2].a.x == doctest::Approx(o.x));
    CHECK(lines[2].width_px > lines[0].width_px);
}

TEST_CASE("skeleton2d editor: sync picks up a change made on disk, and not over unsaved edits") {
    const std::string root = skel_project("skel_ed_sync");
    EditorSession session;
    REQUIRE(session.open_project(sv(root)));
    Skeleton2DEditor ed;
    REQUIRE(ed.create(session, "hero.mcskel2d"));
    // Another writer (an MCP tool) adds a bone.
    cook::String text(ed.text()), why;
    REQUIRE(sk::add_bone(text, bone("tail", "root", -1, 0), why));
    REQUIRE(session.write_asset("hero.mcskel2d", text.view()));
    CHECK_FALSE(ed.sync(session) == false);
    CHECK(ed.info().bones.size() == 2);
    CHECK_FALSE(ed.sync(session));  // nothing new
    REQUIRE(ed.add_bone(bone("ear", "root", 0, 1)));
    CHECK(ed.dirty());
    CHECK_FALSE(ed.sync(session));  // unsaved edits are kept
    CHECK(ed.info().bones.size() == 3);
}

TEST_CASE("skeleton2d editor: the viewport overlay places the previewed bones on the entity") {
    const std::string root = skel_project("skel_ed_overlay");
    EditorSession session;
    REQUIRE(session.open_project(sv(root)));
    Skeleton2DEditor ed;
    Result<asset::Guid> made = ed.create(session, "hero.mcskel2d");
    REQUIRE(made);
    REQUIRE(ed.add_bone(bone("arm", "root", 1, 0, 90, 0.5)));
    REQUIRE(ed.save(session));
    write_file(root + "/assets/stage.mcscene", std::string(R"({ "format": "mce-scene", "version": 1,
        "guid": "5c2d0000-0000-4000-8000-000000000002", "name": "stage", "entities": [
        { "name": "Hero", "components": {
            "LocalTransform": { "position": [3, 1, 0] },
            "Skeleton2D": { "skeleton": "guid:)") + str(cook::String(asset::to_string(made.value()).view())) +
                                                   R"(", "flip_x": true } } },
        { "name": "Other", "components": { "LocalTransform": { "position": [9, 9, 0] } } } ] })");
    session.project().rescan();
    REQUIRE(session.open_scene("stage.mcscene"));
    EntityId hero = kNoEntity, other = kNoEntity;
    for (const DocEntity& e : session.scene().doc().entities()) {
        if (e.name.view() == "Hero") hero = e.id;
        if (e.name.view() == "Other") other = e.id;
    }
    REQUIRE(hero != kNoEntity);
    Array<OverlayLine> lines;
    CHECK_FALSE(skeleton2d_entity_lines(ed, session.scene(), other, lines));
    CHECK(lines.empty());
    ed.select_bone(1);
    REQUIRE(skeleton2d_entity_lines(ed, session.scene(), hero, lines));
    REQUIRE(lines.size() == 4);
    // The root bone is a 0.1 m stub from the entity's origin; the flip mirrors x about it.
    CHECK(lines[0].a.x == doctest::Approx(3.0f));
    CHECK(lines[0].a.y == doctest::Approx(1.0f));
    CHECK(lines[0].b.x == doctest::Approx(2.9f));
    // The arm: origin one metre along x (mirrored), pointing up (90 degrees), 0.5 m long, selected.
    CHECK(lines[2].a.x == doctest::Approx(2.0f));
    CHECK(lines[2].a.y == doctest::Approx(1.0f));
    CHECK(lines[2].b.x == doctest::Approx(2.0f).epsilon(1e-5));
    CHECK(lines[2].b.y == doctest::Approx(1.5f));
    CHECK(lines[2].width_px > lines[0].width_px);
}

TEST_CASE("skeleton2d editor: new_text reads back, selection and preview time stay in range") {
    const Result<asset::Guid> parsed = asset::parse_guid(kArmTex);
    REQUIRE(parsed.has_value());
    const asset::Guid guid = parsed.value();
    const cook::String text = sk::new_text(guid, "hero", "root");
    CHECK(str(text).find("\"hero\"") != std::string::npos);
    cook::String err;
    cook::String edited(text.view());
    CHECK(sk::add_bone(edited, bone("arm", "root", 1, 0), err));  // the fresh text is a skeleton document
    CHECK(err.empty());

    const std::string root = skel_project("skel_ed_range");
    EditorSession session;
    REQUIRE(session.open_project(sv(root)));
    Skeleton2DEditor ed;
    REQUIRE(ed.create(session, "hero.mcskel2d"));
    build_arm(ed);

    ed.select_bone(2);
    CHECK(ed.selected_bone() == 2);
    ed.select_bone(99);  // past the last bone selects none
    CHECK(ed.selected_bone() == kNoSkeleton2DBone);

    ed.set_time(-3.0f);
    CHECK(ed.time() == 0.0f);

    CHECK(ed.animation_length() == 0.0f);  // no animation previewed
    REQUIRE(ed.key(rotate_key("wave", "upper", 0, 0)));
    REQUIRE(ed.key(rotate_key("wave", "upper", 2, 90)));
    ed.set_animation("wave");
    CHECK(ed.animation_length() == doctest::Approx(2.0f));
    ed.set_animation("nonesuch");
    CHECK(ed.animation_length() == 0.0f);  // an unknown animation has no length
}
