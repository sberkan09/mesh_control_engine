// The editor's reflection probe bake (M-VISUAL 2) on a real GL driver: capture through the preview,
// .mcenv + sidecar under assets/probes/, the component pointed at the cooked bake (undoable), the
// GUID kept on re-bake, and the preview loading the bake instead of capturing. The GI volume bake
// (M-VISUAL 6) the same way under assets/gi/.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "cook_test_util.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/preview_renderer.h"
#include "mce/kit/kit3d.h"
#include "mce/memory/frame_slots.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/rhi/default_device.h"
#include "mce/rhi/render_device.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;
namespace fs = std::filesystem;

namespace {

constexpr const char* kFloorGuid = "292d58ee-2d67-59bc-a7ea-e3195335febe";  // projects/Room3D models
constexpr const char* kWallGuid = "a984dc99-37b2-51d0-ac6a-0b43a1cd7ed9";

std::string probe_project() {
    const std::string root = fresh_dir("probe_bake");
    const std::string models = std::string(MCE_SOURCE_DIR) + "/projects/Room3D/assets/models/";
    fs::create_directories(root + "/assets/models");
    for (const char* f : {"floor.glb", "floor.glb.mceasset", "wall.glb", "wall.glb.mceasset"})
        fs::copy_file(models + f, root + "/assets/models/" + f);
    fs::copy(std::string(MCE_SOURCE_DIR) + "/projects/Room3D/assets/textures", root + "/assets/textures");
    return root;
}

EntityId mesh_entity(EditorScene& s, const char* name, const char* guid, math::vec3 pos) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = pos;
    s.set(id, t);
    s.add_component(id, refl::type_of<kit::MeshRef>());
    kit::MeshRef m = *s.get<kit::MeshRef>(id);
    m.mesh.guid = asset::parse_guid(guid).value();
    s.set(id, m);
    return id;
}

}  // namespace

TEST_CASE("editor probe bake: .mcenv + sidecar, component pointed at the cooked bake, preview loads it") {
    REQUIRE(platform::init().has_value());
    platform::WindowDesc wd;
    wd.title = "test_probe_bake_gl";
    wd.width = 64;
    wd.height = 64;
    wd.resizable = false;
    auto w = platform::Window::create(wd);
    REQUIRE(w.has_value());
    mem::UniquePtr<platform::Window> window = std::move(w).value();
    auto d = rhi::create_default_device(*window);
    REQUIRE(d.has_value());
    mem::UniquePtr<rhi::RenderDevice> device = std::move(d).value();
    u64 frame = 0;
    mem::FrameSlots slots(mem::heap());
    {
        PreviewRenderer preview;
        REQUIRE(preview.init(*device).has_value());
        EditorSession session;
        session.set_preview(&preview);
        const std::string root = probe_project();
        REQUIRE(session.open_project(root).has_value());
        EditorScene& s = session.scene();
        mesh_entity(s, "Floor", kFloorGuid, {0, 0, 0});
        mesh_entity(s, "Wall", kWallGuid, {0, 0, -4});
        const EntityId env = s.create_entity("Environment", false);
        s.add_component(env, refl::type_of<kit::Environment3D>());
        kit::Environment3D e = *s.get<kit::Environment3D>(env);
        s.set(env, e);
        const EntityId probe = s.create_entity("Probe");
        LocalTransform pt;
        pt.position = {0, 1, 0};
        s.set(probe, pt);
        s.add_component(probe, refl::type_of<kit::ReflectionProbe3D>());

        // Without IBL the bake refuses, naming the switch.
        slots.begin_frame(frame);
        device->begin_frame(frame++);
        Result<u32> off = session.bake_reflection_probes();
        device->end_frame();
        slots.end_frame(nullptr);
        REQUIRE_FALSE(off);
        CHECK(std::string(off.error().path.c_str()).find("sky_ibl") != std::string::npos);

        e.sky_ibl = 1;
        s.set(env, e);
        slots.begin_frame(frame);
        device->begin_frame(frame++);
        Result<u32> baked = session.bake_reflection_probes();
        device->end_frame();
        slots.end_frame(nullptr);
        if (!baked) MESSAGE(baked.error().stage, " ", baked.error().path.c_str());
        REQUIRE(baked);
        CHECK(baked.value() == 1);
        const asset::Guid guid = s.get<kit::ReflectionProbe3D>(probe)->baked.guid;
        REQUIRE(guid != asset::Guid{});
        const std::string rel = "probes/untitled/probe_" + std::to_string(probe) + ".mcenv";
        const std::string file = root + "/assets/" + rel;
        REQUIRE(fs::exists(file));
        REQUIRE(fs::exists(file + ".mceasset"));
        CHECK(fs::file_size(file) == 24 + usize(render::Forward3D::kIblEnvWidth) * (render::Forward3D::kIblEnvWidth / 2) * 4 *
                                              render::Forward3D::kProbeLayers);
        CHECK(session.project().mounted()->find(guid) != asset::kInvalidAssetKey);
        // Not black: the default sky shows in the sharp layer.
        const std::string bytes = read_file(file);
        usize lit = 0;
        for (usize i = 24; i + 3 < bytes.size(); i += 4) lit += (u8(bytes[i]) | u8(bytes[i + 1]) | u8(bytes[i + 2])) != 0;
        CHECK(lit > bytes.size() / 8);

        // One undo step; a re-bake keeps the file's GUID.
        CHECK(s.undo());
        CHECK(s.get<kit::ReflectionProbe3D>(probe)->baked.guid == asset::Guid{});
        slots.begin_frame(frame);
        device->begin_frame(frame++);
        REQUIRE(session.bake_reflection_probes());
        device->end_frame();
        slots.end_frame(nullptr);
        CHECK(s.get<kit::ReflectionProbe3D>(probe)->baked.guid == guid);

        // The preview now loads the bake instead of capturing.
        rhi::TextureDesc td;
        td.size = {64, 64};
        td.usage = rhi::TextureUsage::SampledColorTarget;
        const rhi::TextureHandle target = device->create_texture(td);
        render::Camera3D cam;
        cam.position = {0, 2, 6};
        slots.begin_frame(frame);
        device->begin_frame(frame++);
        {
            rhi::CommandBuffer cmd;
            CHECK(preview.record_3d(cmd, s.world(), target, td.size, cam, {}, math::Color{0, 0, 0, 1}));
            device->submit(cmd);
        }
        device->end_frame();
        slots.end_frame(nullptr);
        const render::Forward3DStats& st = preview.renderer().forward_3d().last_stats();
        CHECK(st.probes == 1);
        CHECK(st.probes_loaded == 1);
        CHECK(st.probes_captured == 0);
        device->destroy_texture(target);
        CHECK(device->validation_error_count() == 0);
        session.set_preview(nullptr);
    }
    device.reset();
    window.reset();
    platform::shutdown();
}

TEST_CASE("editor GI bake: .mcenv + sidecar, component pointed at the cooked bake, preview loads it") {
    REQUIRE(platform::init().has_value());
    platform::WindowDesc wd;
    wd.title = "test_gi_bake_gl";
    wd.width = 64;
    wd.height = 64;
    wd.resizable = false;
    auto w = platform::Window::create(wd);
    REQUIRE(w.has_value());
    mem::UniquePtr<platform::Window> window = std::move(w).value();
    auto d = rhi::create_default_device(*window);
    REQUIRE(d.has_value());
    mem::UniquePtr<rhi::RenderDevice> device = std::move(d).value();
    u64 frame = 0;
    mem::FrameSlots slots(mem::heap());
    {
        PreviewRenderer preview;
        REQUIRE(preview.init(*device).has_value());
        EditorSession session;
        session.set_preview(&preview);
        const std::string root = probe_project();
        REQUIRE(session.open_project(root).has_value());
        EditorScene& s = session.scene();
        mesh_entity(s, "Floor", kFloorGuid, {0, 0, 0});
        mesh_entity(s, "Wall", kWallGuid, {0, 0, -4});
        const auto bake = [&]() {
            slots.begin_frame(frame);
            device->begin_frame(frame++);
            Result<void> r = session.bake_gi_volume();
            device->end_frame();
            slots.end_frame(nullptr);
            return r;
        };

        // Without a volume the bake refuses.
        Result<void> none = bake();
        REQUIRE_FALSE(none);
        CHECK(std::string(none.error().path.c_str()).find("GiVolume3D") != std::string::npos);

        const EntityId vol = s.create_entity("GI");
        LocalTransform vt;
        vt.position = {0, 1.2f, 0};
        s.set(vol, vt);
        s.add_component(vol, refl::type_of<kit::GiVolume3D>());
        kit::GiVolume3D g = *s.get<kit::GiVolume3D>(vol);
        g.half_extents = {3, 1, 3};
        g.probes_x = 4;
        g.probes_y = 2;
        g.probes_z = 4;
        s.set(vol, g);
        Result<void> baked = bake();
        if (!baked) MESSAGE(baked.error().stage, " ", baked.error().path.c_str());
        REQUIRE(baked);
        const asset::Guid guid = s.get<kit::GiVolume3D>(vol)->baked.guid;
        REQUIRE(guid != asset::Guid{});
        const std::string file = root + "/assets/gi/untitled/gi_" + std::to_string(vol) + ".mcenv";
        REQUIRE(fs::exists(file));
        REQUIRE(fs::exists(file + ".mceasset"));
        CHECK(fs::file_size(file) == 24 + usize(16) * 8 * 4);  // 4 texels per probe x 4, rows y + 2 z
        CHECK(session.project().mounted()->find(guid) != asset::kInvalidAssetKey);
        // Lit: the default sky and sun reach every probe of an open room.
        const std::string bytes = read_file(file);
        usize lit = 0;
        for (usize i = 24; i + 3 < bytes.size(); i += 4) lit += (u8(bytes[i]) | u8(bytes[i + 1]) | u8(bytes[i + 2])) != 0;
        CHECK(lit == (bytes.size() - 24) / 4);

        // One undo step; a re-bake keeps the file's GUID.
        CHECK(s.undo());
        CHECK(s.get<kit::GiVolume3D>(vol)->baked.guid == asset::Guid{});
        REQUIRE(bake());
        CHECK(s.get<kit::GiVolume3D>(vol)->baked.guid == guid);

        // The preview now starts from the bake instead of filling.
        rhi::TextureDesc td;
        td.size = {64, 64};
        td.usage = rhi::TextureUsage::SampledColorTarget;
        const rhi::TextureHandle target = device->create_texture(td);
        render::Camera3D cam;
        cam.position = {0, 2, 6};
        slots.begin_frame(frame);
        device->begin_frame(frame++);
        {
            rhi::CommandBuffer cmd;
            CHECK(preview.record_3d(cmd, s.world(), target, td.size, cam, {}, math::Color{0, 0, 0, 1}));
            device->submit(cmd);
        }
        device->end_frame();
        slots.end_frame(nullptr);
        const render::Forward3DStats& st = preview.renderer().forward_3d().last_stats();
        CHECK(st.gi);
        CHECK(st.gi_loaded);
        CHECK_FALSE(st.gi_filled);
        device->destroy_texture(target);
        CHECK(device->validation_error_count() == 0);
        session.set_preview(nullptr);
    }
    device.reset();
    window.reset();
    platform::shutdown();
}
