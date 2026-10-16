// The 2D skeleton tools (M-ANIM2D step 5) in process against a copy of the scene_demo project:
// skel2d_create / add_bone / add_slot / add_ik / key keep a .mcskel2d the real importer cooks (bad
// input comes back with its file:line message and writes nothing), and skel2d_preview returns the
// bone transforms of the runtime's pose functions plus a line drawing.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "mce/editor/skeleton2d_editor.h"
#include "mce/kit/skeleton2d.h"
#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

constexpr const char* kArmTex = "5c2d0000-0000-4000-8000-0000000000a1";

std::string skel_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    write_file(root + "/assets/arm.png", make_png(2, 2, std::vector<u8>(2 * 2 * 4, 255)));
    write_file(root + "/assets/arm.png.mceasset", sidecar(kArmTex, "texture", R"({ "srgb": true, "mips": false })"));
    return root;
}

const cook::JsonValue bone_named(const cook::JsonValue bones, const char* name) {
    for (u32 i = 0; i < bones.size(); ++i)
        if (str(bones.at(i).get("name")) == name) return bones.at(i);
    return cook::JsonValue();
}

}  // namespace

TEST_CASE("skel2d tools: build a rig and an animation; the file stays valid at every step") {
    const std::string root = skel_project("mcp_skel2d_edit");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();

    auto made = c.ok("skel2d_create", R"({"path":"rigs/hero.mcskel2d","name":"hero"})");
    CHECK(str(made.v().get("guid")).rfind("guid:", 0) == 0);
    CHECK(made.v().get("bones").as_number() == 1);
    const std::string file = root + "/assets/rigs/hero.mcskel2d";
    REQUIRE(std::filesystem::exists(file));
    CHECK(c.fails("skel2d_create", R"({"path":"rigs/hero.mcskel2d"})").find("already exists") != std::string::npos);
    CHECK(c.fails("skel2d_create", R"({"path":"rigs/x.json"})").find("'path'") != std::string::npos);
    CHECK(c.fails("skel2d_create", R"({"path":"../x.mcskel2d"})").find("'path'") != std::string::npos);

    c.ok("skel2d_add_bone",
         R"({"skeleton":"rigs/hero.mcskel2d","name":"upper","parent":"root","x":1,"y":2,"rotation":90,"length":1})");
    auto lower =
        c.ok("skel2d_add_bone",
             R"({"skeleton":"rigs/hero.mcskel2d","name":"lower","parent":"upper","x":1,"length":1,"scale":[2,1]})");
    CHECK(lower.v().get("bones").as_number() == 3);
    c.ok("skel2d_add_bone", R"({"skeleton":"rigs/hero.mcskel2d","name":"aim","parent":"root","x":1.5,"y":1})");

    // Errors come from the importer, with the file position, and nothing is written.
    const std::string before = read_file(file);
    const std::string e1 =
        c.fails("skel2d_add_bone", R"({"skeleton":"rigs/hero.mcskel2d","name":"hand","parent":"nobody"})");
    CHECK(e1.find("rigs/hero.mcskel2d:") != std::string::npos);
    CHECK(e1.find("no bone has that name") != std::string::npos);
    CHECK(c.fails("skel2d_add_bone", R"({"skeleton":"rigs/hero.mcskel2d","name":"upper","parent":"root"})")
              .find("share this name") != std::string::npos);
    CHECK(c.fails("skel2d_add_bone", R"({"skeleton":"rigs/hero.mcskel2d","parent":"root"})").find("'name'") !=
          std::string::npos);
    CHECK(c.fails("skel2d_add_bone", R"({"skeleton":"rigs/hero.mcskel2d","name":"b","x":"far"})").find("'x'") !=
          std::string::npos);
    CHECK(c.fails("skel2d_add_bone", R"({"skeleton":"rigs/none.mcskel2d","name":"b"})").find("no such asset") !=
          std::string::npos);
    CHECK(c.fails("skel2d_add_bone", R"({"skeleton":"scenes/demo.mcscene","name":"b"})").find("not a 2D skeleton") !=
          std::string::npos);
    CHECK(read_file(file) == before);

    // Slots: a textured quad, a solid quad, and an attachment list.
    auto slot = c.ok("skel2d_add_slot",
                     std::string(R"({"skeleton":"rigs/hero.mcskel2d","name":"arm","bone":"upper","texture":"guid:)") +
                         kArmTex + R"(","size":[1,0.25],"pivot":[0,0.5],"tint":[1,0.5,0.5,1]})");
    CHECK(slot.v().get("slots").as_number() == 1);
    c.ok("skel2d_add_slot", R"({"skeleton":"rigs/hero.mcskel2d","name":"box","bone":"lower","size":[0.2,0.2]})");
    c.ok("skel2d_add_slot", R"({"skeleton":"rigs/hero.mcskel2d","name":"hat","bone":"upper","attachments":[
        {"name":"plain","type":"quad","size":[0.3,0.3]},
        {"name":"fancy","type":"mesh","vertices":[{"uv":[0,0],"x":0,"y":0},{"uv":[1,0],"x":0.3,"y":0},{"uv":[0,1],"x":0,"y":0.3}],"triangles":[0,1,2]}],
        "attachment":"fancy"})");
    CHECK(
        c.fails("skel2d_add_slot", R"({"skeleton":"rigs/hero.mcskel2d","name":"bad","bone":"ghost"})").find("ghost") !=
        std::string::npos);
    CHECK(
        c.fails(
             "skel2d_add_slot",
             R"({"skeleton":"rigs/hero.mcskel2d","name":"bad","bone":"root","texture":"guid:5c2d0000-0000-4000-8000-0000000000ff"})")
            .find("rigs/hero.mcskel2d:") != std::string::npos);
    CHECK(c.fails("skel2d_add_slot", R"({"skeleton":"rigs/hero.mcskel2d","name":"bad","bone":"root","attachments":[]})")
              .find("'attachments'") != std::string::npos);

    auto ik = c.ok(
        "skel2d_add_ik",
        R"({"skeleton":"rigs/hero.mcskel2d","name":"reach","bones":["upper","lower"],"target":"aim","bend":"negative","mix":0.5})");
    CHECK(ik.v().get("ik").as_number() == 1);
    CHECK(c.fails("skel2d_add_ik", R"({"skeleton":"rigs/hero.mcskel2d","name":"bad","bones":["root","lower"]})")
              .find("rigs/hero.mcskel2d:") != std::string::npos);
    CHECK(c.fails("skel2d_add_ik", R"({"skeleton":"rigs/hero.mcskel2d","name":"bad"})").find("'bones'") !=
          std::string::npos);

    // Keys: every channel, a batch, a curve, the same time replacing a key.
    c.ok(
        "skel2d_key",
        R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"rotate","target":"upper","t":1,"value":60,"curve":[0.25,0,0.75,1],"duration":2})");
    c.ok("skel2d_key",
         R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"rotate","target":"upper","keys":[
        {"t":0,"value":0},{"t":2,"value":-30,"curve":"stepped"}]})");
    c.ok(
        "skel2d_key",
        R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"translate","target":"root","t":1,"x":0.5,"y":-0.25})");
    c.ok("skel2d_key",
         R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"scale","target":"lower","t":1,"x":1.5})");
    c.ok(
        "skel2d_key",
        R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"color","target":"box","t":0.5,"color":[1,0,0,0.5]})");
    c.ok(
        "skel2d_key",
        R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"attachment","target":"hat","t":1,"attachment":"plain"})");
    auto keyed = c.ok(
        "skel2d_key",
        R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"ik_mix","target":"reach","t":1,"value":1,"loop":false})");
    CHECK(keyed.v().get("animations").as_number() == 1);
    c.ok(
        "skel2d_key",
        R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"rotate","target":"upper","t":1,"value":45})");

    const std::string text = read_file(file);
    cook::JsonDoc doc;
    cook::JsonError jerr;
    REQUIRE(doc.parse(sv(text), jerr));
    const cook::JsonValue anim = doc.root().get("animations").at(0);
    CHECK(str(anim.get("name")) == "wave");
    CHECK(anim.get("duration").as_number() == 2);
    CHECK_FALSE(anim.get("loop").as_bool());
    const cook::JsonValue rot = anim.get("bones").get("upper").get("rotate");
    REQUIRE(rot.size() == 3);
    CHECK(rot.at(0).get("t").as_number() == 0);
    CHECK(rot.at(1).get("value").as_number() == 45);  // replaced at the same time
    CHECK(str(rot.at(2).get("curve")) == "stepped");
    CHECK(anim.get("bones").get("scale").valid() == false);
    CHECK(anim.get("bones").get("lower").get("scale").at(0).get("y").as_number() == 1);  // the default factor

    // Key errors.
    CHECK(
        c.fails(
             "skel2d_key",
             R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"rotate","target":"ghost","t":0,"value":1})")
            .find("wave") != std::string::npos);
    CHECK(c.fails("skel2d_key",
                  R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"spin","target":"upper","t":0})")
              .find("'channel'") != std::string::npos);
    CHECK(
        c.fails(
             "skel2d_key",
             R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"rotate","target":"upper","t":-1,"value":1})")
            .find("'t'") != std::string::npos);
    CHECK(
        c.fails(
             "skel2d_key",
             R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"rotate","target":"upper","keys":[{"t":0}]})")
            .find("keys[0]") != std::string::npos);
    CHECK(
        c.fails(
             "skel2d_key",
             R"({"skeleton":"rigs/hero.mcskel2d","animation":"wave","channel":"rotate","target":"upper","t":0,"value":1,"curve":[2,0,0,1]})")
            .find("cx1") != std::string::npos);
    CHECK(read_file(file) == text);

    // asset_read still sees it as a Skeleton2D asset.
    auto read = c.ok("asset_read", R"({"path":"rigs/hero.mcskel2d"})");
    CHECK(str(read.v().get("type")) == "Skeleton2D");
}

TEST_CASE("skel2d tools: skel2d_preview returns the pose and a drawing") {
    const std::string root = skel_project("mcp_skel2d_preview");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    c.ok("skel2d_create", R"({"path":"rigs/hero.mcskel2d"})");
    c.ok("skel2d_add_bone",
         R"({"skeleton":"rigs/hero.mcskel2d","name":"upper","parent":"root","x":1,"y":2,"rotation":90,"length":1})");
    c.ok("skel2d_add_bone", R"({"skeleton":"rigs/hero.mcskel2d","name":"lower","parent":"upper","x":1,"length":1})");
    c.ok("skel2d_add_slot",
         R"({"skeleton":"rigs/hero.mcskel2d","name":"box","bone":"upper","size":[1,0.2],"pivot":[0,0.5]})");
    c.ok(
        "skel2d_key",
        R"({"skeleton":"rigs/hero.mcskel2d","animation":"turn","channel":"rotate","target":"upper","keys":[{"t":0,"value":0},{"t":1,"value":90}]})");
    c.ok(
        "skel2d_key",
        R"({"skeleton":"rigs/hero.mcskel2d","animation":"turn","channel":"translate","target":"root","keys":[{"t":0,"x":0},{"t":1,"x":4,"y":1}]})");

    // The rest pose: upper at (1, 2) turned 90 degrees, lower one metre along it.
    auto rest = c.ok("skel2d_preview", R"({"skeleton":"rigs/hero.mcskel2d"})");
    REQUIRE(c.server.session().project().is_open());
    const cook::JsonValue bones = rest.v().get("bones");
    REQUIRE(bones.size() == 3);
    const cook::JsonValue upper = bone_named(bones, "upper");
    CHECK(upper.get("x").as_number() == doctest::Approx(1.0));
    CHECK(upper.get("y").as_number() == doctest::Approx(2.0));
    CHECK(upper.get("rotation").as_number() == doctest::Approx(90.0));
    CHECK(str(upper.get("parent")) == "root");
    const cook::JsonValue lower = bone_named(bones, "lower");
    CHECK(lower.get("x").as_number() == doctest::Approx(1.0).epsilon(1e-5));
    CHECK(lower.get("y").as_number() == doctest::Approx(3.0));
    CHECK(lower.get("tip").at(1).as_number() == doctest::Approx(4.0));
    CHECK(rest.v().get("animation").is_null());
    // An image of the requested size, not blank.
    REQUIRE(rest.images.size() == 1);
    CHECK(rest.images[0].width == 480);
    bool varied = false;
    for (usize i = 4; i < rest.images[0].rgba.size(); i += 4)
        varied |= rest.images[0].rgba[i] != rest.images[0].rgba[0];
    CHECK(varied);
    auto small = c.ok("skel2d_preview", R"({"skeleton":"rigs/hero.mcskel2d","width":96,"height":64})");
    CHECK(small.images[0].width == 96);
    CHECK(small.images[0].height == 64);
    CHECK(c.ok("skel2d_preview", R"({"skeleton":"rigs/hero.mcskel2d","render":false})").images.empty());

    // The numbers equal the runtime's: the same offsets through skeleton2d_pose_offsets.
    auto half = c.ok("skel2d_preview", R"({"skeleton":"rigs/hero.mcskel2d","animation":"turn","time":0.5})");
    CHECK(half.v().get("duration").as_number() == doctest::Approx(1.0));
    CHECK(half.v().get("loop").as_bool());
    editor::EditorProject& project = c.server.session().project();
    cook::AssetRegistry registry;
    cook::DiagList diags;
    registry.scan(project.root(), project.importers(), false, diags);
    const editor::ProjectAsset* asset = nullptr;
    for (const editor::ProjectAsset& a : project.assets())
        if (a.path.view() == "rigs/hero.mcskel2d") asset = &a;
    REQUIRE(asset != nullptr);
    kit::Skeleton2DRegistry defs;
    REQUIRE(editor::cook_skeleton2d(project, registry, asset->guid, asset->path.view(),
                                    sv(read_file(root + "/assets/rigs/hero.mcskel2d")), defs, diags));
    const kit::Skeleton2DRegistry::Def* def = defs.resolve(asset->guid);
    REQUIRE(def != nullptr);
    kit::Bone2DPose offsets[3];
    offsets[0].x = 2.0f;
    offsets[0].y = 0.5f;
    offsets[1].rotation = 45.0f * 3.14159265358979f / 180.0f;
    kit::Bone2DTransform want[3];
    kit::skeleton2d_pose_offsets(def->bones.span(), Span<const kit::Bone2DPose>(offsets, 3),
                                 Span<kit::Bone2DTransform>(want, 3));
    const cook::JsonValue hb = half.v().get("bones");
    for (u32 i = 0; i < 3; ++i) {
        const cook::JsonValue w = hb.at(i).get("world");
        CHECK(w.get("a").as_number() == doctest::Approx(want[i].a).epsilon(1e-5));
        CHECK(w.get("b").as_number() == doctest::Approx(want[i].b).epsilon(1e-5));
        CHECK(w.get("c").as_number() == doctest::Approx(want[i].c).epsilon(1e-5));
        CHECK(w.get("d").as_number() == doctest::Approx(want[i].d).epsilon(1e-5));
        CHECK(w.get("x").as_number() == doctest::Approx(want[i].x).epsilon(1e-5));
        CHECK(w.get("y").as_number() == doctest::Approx(want[i].y).epsilon(1e-5));
    }
    // A looping clip wraps: 1.5 s is 0.5 s.
    auto wrapped =
        c.ok("skel2d_preview", R"({"skeleton":"rigs/hero.mcskel2d","animation":"turn","time":1.5,"render":false})");
    CHECK(bone_named(wrapped.v().get("bones"), "lower").get("x").as_number() ==
          doctest::Approx(bone_named(hb, "lower").get("x").as_number()));

    // IK targets aim a constraint.
    c.ok("skel2d_add_ik", R"({"skeleton":"rigs/hero.mcskel2d","name":"reach","bones":["upper","lower"]})");
    auto aimed = c.ok(
        "skel2d_preview",
        R"({"skeleton":"rigs/hero.mcskel2d","ik_targets":[{"constraint":"reach","x":1.5,"y":3.2}],"render":false})");
    const cook::JsonValue tip = bone_named(aimed.v().get("bones"), "lower").get("tip");
    CHECK(tip.at(0).as_number() == doctest::Approx(1.5).epsilon(1e-3));
    CHECK(tip.at(1).as_number() == doctest::Approx(3.2).epsilon(1e-3));

    CHECK(c.fails("skel2d_preview", R"({"skeleton":"rigs/hero.mcskel2d","animation":"nope"})").find("nope") !=
          std::string::npos);
    CHECK(c.fails("skel2d_preview", R"({"skeleton":"rigs/hero.mcskel2d","time":-1})").find("'time'") !=
          std::string::npos);
    CHECK(c.fails("skel2d_preview",
                  R"({"skeleton":"rigs/hero.mcskel2d","ik_targets":[{"constraint":"zzz","x":0,"y":0}]})")
              .find("zzz") != std::string::npos);
    CHECK(c.fails("skel2d_preview", R"({"skeleton":"rigs/hero.mcskel2d","width":8})").find("'width'") !=
          std::string::npos);
}
