// The starter kit's render extract on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe): authored
// components -> SpriteExtract -> SpriteBatch -> pixels. Parallax layers (FR-R2D-050) at their
// authored factors, endless backgrounds, and particles drawn through the batch.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/core.h"
#include "mce/ecs/transform.h"
#include "mce/kit/particles.h"
#include "mce/kit/sprite_extract.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/asset_upload.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"

using namespace mce;
using math::vec2;

namespace {

constexpr u32 kW = 160, kH = 96;
constexpr f32 kPpm = 16.0f;  // 160 × 96 px = a 10 m × 6 m view

struct Rgba {
    u8 r, g, b, a;
};

struct Fixture {
    Fixture() {
        REQUIRE(platform::init().has_value());
        platform::WindowDesc wd;
        wd.title = "test_kit_gl";
        wd.width = 64;
        wd.height = 64;
        wd.resizable = false;
        wd.high_dpi = false;
        auto w = platform::Window::create(wd);
        REQUIRE(w.has_value());
        window = std::move(w).value();
        auto d = rhi::create_default_device(*window);
        REQUIRE(d.has_value());
        device = std::move(d).value();
        REQUIRE(renderer.init(*device).has_value());
        REQUIRE(extract.init(renderer, nullptr).has_value());
        rhi::TextureDesc td;
        td.size = {kW, kH};
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.debug_name = "test.target";
        target = device->create_texture(td);
        pixels.resize(usize(kW) * kH * 4);
    }
    ~Fixture() {
        device->destroy_texture(target);
        extract.shutdown();
        renderer.shutdown();
        device.reset();
        window.reset();
        platform::shutdown();
    }

    Entity camera(vec2 at) {
        kit::Camera2DComponent c;
        c.pixels_per_meter = kPpm;
        return world.spawn_with(LocalTransform{{at.x, at.y, 0}, math::quat::identity(), {1, 1, 1}}, c);
    }
    Entity sprite(vec2 at, math::Color tint, vec2 size = {1, 1}, i16 layer = 0) {
        kit::Sprite s;
        s.tint = tint;
        s.size = size;
        s.layer = layer;
        return world.spawn_with(LocalTransform{{at.x, at.y, 0}, math::quat::identity(), {1, 1, 1}}, s);
    }

    void render() {
        device->begin_frame(frame++);
        render::Camera2D cam;
        f32 ppm = 48.0f;
        kit::primary_camera(world, cam, ppm);
        render::SpriteBatch batch(renderer.sprite_sets(), mem::heap());
        batch.begin(render::compute_view_2d(cam, {kW, kH}, ppm));
        extract.extract(world, batch);
        batch.build();
        stats = batch.stats();
        lights = batch.lights();
        rhi::PassAttachment color;
        color.texture = target;
        color.clear.color[3] = 1.0f;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        rhi::CommandBuffer cmd;
        cmd.begin_pass(pass);
        renderer.record_2d(cmd, batch);
        cmd.end_pass();
        device->submit(cmd);
        REQUIRE(device->read_pixels(target, rhi::Rect2D{0, 0, kW, kH}, pixels.span()).has_value());
        device->end_frame();
    }

    // Pixel at a position relative to the view center, in metres.
    Rgba at_view(vec2 p) const {
        const u32 x = static_cast<u32>(p.x * kPpm + kW / 2.0f);
        const u32 y = static_cast<u32>(kH / 2.0f - p.y * kPpm);  // read_pixels is top-row first
        const usize i = (usize(y) * kW + x) * 4;
        return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
    }

    mem::UniquePtr<platform::Window> window;
    mem::UniquePtr<rhi::RenderDevice> device;
    render::Renderer renderer;
    kit::SpriteExtract extract;
    World world;
    rhi::TextureHandle target;
    Array<u8> pixels;
    render::BatchStats stats;
    render::Lights2D lights;
    u64 frame = 0;
};

constexpr math::Color kRed{1, 0, 0, 1}, kGreen{0, 1, 0, 1}, kBlue{0, 0, 1, 1}, kWhite{1, 1, 1, 1};

bool is(Rgba p, u8 r, u8 g, u8 b) {
    auto near = [](u8 x, u8 y) { return (x > y ? x - y : y - x) <= 3; };
    return near(p.r, r) && near(p.g, g) && near(p.b, b);
}

}  // namespace

TEST_CASE("FR-R2D-050: three parallax layers scroll at their authored factors") {
    Fixture f;
    const Entity cam = f.camera({0, 0});
    // Far (0.25), mid (0.5) and near (1) layers, each a 1 m square authored at x = 0.
    const Entity far = f.sprite({0, 2}, kRed);
    f.world.add(far, kit::Parallax{{0.25f, 0.25f}});
    const Entity mid = f.sprite({0, 0}, kGreen);
    f.world.add(mid, kit::Parallax{{0.5f, 0.5f}});
    f.sprite({0, -2}, kBlue);  // no Parallax: the world layer

    f.render();
    CHECK(is(f.at_view({0, 2}), 255, 0, 0));
    CHECK(is(f.at_view({0, 0}), 0, 255, 0));
    CHECK(is(f.at_view({0, -2}), 0, 0, 255));

    // Camera 4 m right: the world layer moves 4 m left on screen, mid 2 m, far 1 m.
    f.world.set(cam, LocalTransform{{4, 0, 0}, math::quat::identity(), {1, 1, 1}});
    f.render();
    CHECK(is(f.at_view({-1, 2}), 255, 0, 0));
    CHECK(is(f.at_view({0, 2}), 0, 0, 0));
    CHECK(is(f.at_view({-2, 0}), 0, 255, 0));
    CHECK(is(f.at_view({-4, -2}), 0, 0, 255));

    // Vertical factors are independent; 0 is fixed to the camera.
    f.world.set(far, kit::Parallax{{0.0f, 1.0f}});
    f.world.set(cam, LocalTransform{{3, 1, 0}, math::quat::identity(), {1, 1, 1}});
    f.render();
    CHECK(is(f.at_view({0, 1}), 255, 0, 0));  // x pinned to the camera, y scrolls with the world
}

TEST_CASE("FR-R2D-050: repeat_x tiles a background across the whole view at any camera position") {
    Fixture f;
    const Entity cam = f.camera({0, 0});
    const Entity bg = f.sprite({0, 0}, kWhite, {2, 1});
    f.world.add(bg, kit::Parallax{{0.5f, 1.0f}, true});
    for (f32 x : {0.0f, 7.3f, -31.0f, 250.0f}) {
        f.world.set(cam, LocalTransform{{x, 0, 0}, math::quat::identity(), {1, 1, 1}});
        f.render();
        for (f32 sx = -4.9f; sx < 5.0f; sx += 0.35f) CHECK(is(f.at_view({sx, 0}), 255, 255, 255));
        CHECK(is(f.at_view({0, 1.5f}), 0, 0, 0));
        CHECK(f.stats.quads <= 7);  // only the copies the view needs
    }
}

TEST_CASE("FR-R2D-070: particles reach the screen through the kit extract") {
    Fixture f;
    f.camera({0, 0});
    kit::install_particles(f.world, nullptr);
    asset::ParticlePresetChunk p{};
    p.burst = 1;
    p.max_particles = 8;
    p.lifetime[0] = p.lifetime[1] = 10.0f;
    p.size_start = p.size_end = 1.0f;
    p.color_start[0] = p.color_end[0] = 1.0f;
    p.color_start[3] = p.color_end[3] = 1.0f;
    p.flags = asset::ParticleFlags::Additive;
    const asset::Guid preset{1, 2};
    f.world.resource<kit::ParticleSystem>().set_preset(preset, p);
    kit::ParticleEmitter em;
    em.preset.guid = preset;
    f.world.spawn_with(LocalTransform{{2, 1, 0}, math::quat::identity(), {1, 1, 1}}, em);
    f.sprite({2, 1}, math::Color{0, 0, 1, 1}, {1, 1}, -1);  // under it: additive red over blue
    f.world.resource<kit::ParticleSystem>().advance(f.world, 1.0f / 60.0f);
    f.render();
    CHECK(is(f.at_view({2, 1}), 255, 0, 255));
    CHECK(is(f.at_view({0, 0}), 0, 0, 0));
}

TEST_CASE("FR-R2D-090: Light2D entities and a cooked normal map light a kit sprite") {
    using namespace cook_test;
    const std::string root = fresh_dir("kit_lit");
    std::vector<u8> white(4 * 4 * 4, 255), tilted;
    for (u32 i = 0; i < 16; ++i) tilted.insert(tilted.end(), {238, 128, 191, 255});  // 60° toward +X
    write_file(root + "/assets/rock.png", make_png(4, 4, white));
    write_file(root + "/assets/rock.png.mceasset", sidecar("1170000a-0000-4000-8000-000000000001", "texture", R"({ "srgb": true, "mips": false })"));
    write_file(root + "/assets/rock_n.png", make_png(4, 4, tilted));
    write_file(root + "/assets/rock_n.png.mceasset", sidecar("1170000a-0000-4000-8000-000000000002", "texture", R"({ "srgb": false, "mips": false })"));
    cook::CookService svc;
    REQUIRE(svc.cook(StringView(root.c_str()), cook::CookOptions{}).ok());

    Fixture f;
    asset::AssetSystem assets;
    render::AssetUploadSink sink(f.renderer, assets);
    assets.set_upload_sink(&sink);
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    f.extract.reset(&assets);

    f.camera({0, 0});
    kit::Sprite rock;
    rock.texture.guid = asset::parse_guid("1170000a-0000-4000-8000-000000000001").value();
    rock.normal_map.guid = asset::parse_guid("1170000a-0000-4000-8000-000000000002").value();
    f.world.spawn_with(LocalTransform{{0, 0, 0}, math::quat::identity(), {1, 1, 1}}, rock);
    f.sprite({3, 0}, kWhite);  // unlit
    kit::Light2D amb;
    amb.kind = kit::Light2DKind::Ambient;
    amb.color = math::Color{0.05f, 0.05f, 0.05f, 1};
    f.world.spawn_with(LocalTransform{}, amb);
    kit::Light2D lamp;
    lamp.height = 0.5f;
    lamp.radius = 4.0f;
    const Entity light = f.world.spawn_with(LocalTransform{{1, 0, 0}, math::quat::identity(), {1, 1, 1}}, lamp);
    kit::Light2D far_lamp = lamp;
    f.world.spawn_with(LocalTransform{{100, 0, 0}, math::quat::identity(), {1, 1, 1}}, far_lamp);  // culled

    f.render();
    CHECK(f.lights.count == 1);
    CHECK(f.lights.ambient.r == doctest::Approx(0.05f));
    const u8 lit_right = f.at_view({0, 0}).r;
    CHECK(is(f.at_view({3, 0}), 255, 255, 255));  // the unlit sprite ignores the dim ambient

    f.world.set(light, LocalTransform{{-1, 0, 0}, math::quat::identity(), {1, 1, 1}});
    f.render();
    const u8 lit_left = f.at_view({0, 0}).r;
    CHECK(lit_right > lit_left + 60);  // the normal faces +X
    f.extract.reset(nullptr);
}

TEST_CASE("FR-R2D-100: the primary camera's Post2DComponent becomes renderer post settings") {
    Fixture f;
    const Entity cam = f.camera({0, 0});
    render::Post2D post;
    CHECK_FALSE(f.extract.post_2d(f.world, post));  // no component
    kit::Post2DComponent pc;
    f.world.add(cam, pc);
    CHECK_FALSE(f.extract.post_2d(f.world, post));  // every effect off
    pc.vignette = true;
    pc.vignette_intensity = 0.7f;
    pc.chromatic_aberration = true;
    pc.color_grade = true;  // no LUT texture: grading stays off
    f.world.set(cam, pc);
    REQUIRE(f.extract.post_2d(f.world, post));
    CHECK(post.vignette);
    CHECK(post.vignette_intensity == 0.7f);
    CHECK(post.chromatic_aberration);
    CHECK_FALSE(post.color_grade);
}
