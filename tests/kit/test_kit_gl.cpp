// The starter kit's render extract on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe): authored
// components -> SpriteExtract -> SpriteBatch -> pixels. Parallax layers (FR-R2D-050) at their
// authored factors, endless backgrounds, and particles drawn through the batch.

#include <cstdio>

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/core.h"
#include "mce/ecs/transform.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/particles.h"
#include "mce/kit/render_textures.h"
#include "mce/kit/sprite_extract.h"
#include "mce/kit/views.h"
#include "mce/loc/loc.h"
#include "mce/log.h"
#include "mce/memory/frame_slots.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/asset_upload.h"
#include "mce/render/renderer.h"
#include "mce/render/scene3d.h"
#include "mce/rhi/default_device.h"
#include "mesh_fixture.h"

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

TEST_CASE("#176: a texture imported with \"filter\": \"linear\" draws its kit sprite bilinearly") {
    using namespace cook_test;
    const std::string root = fresh_dir("kit_filter");
    const std::vector<u8> black_white = {0, 0, 0, 255, 255, 255, 255, 255};  // 2x1: black | white
    write_file(root + "/assets/soft.png", make_png(2, 1, black_white));
    write_file(root + "/assets/soft.png.mceasset",
               sidecar("1176000a-0000-4000-8000-000000000001", "texture", R"({ "mips": false, "filter": "linear" })"));
    write_file(root + "/assets/hard.png", make_png(2, 1, black_white));
    write_file(root + "/assets/hard.png.mceasset", sidecar("1176000a-0000-4000-8000-000000000002", "texture", R"({ "mips": false })"));
    write_file(root + "/assets/bad.png", make_png(2, 1, black_white));
    write_file(root + "/assets/bad.png.mceasset", sidecar("1176000a-0000-4000-8000-000000000003", "texture", R"({ "filter": "bilinear" })"));
    cook::CookService svc;
    CHECK_FALSE(svc.cook(StringView(root.c_str()), cook::CookOptions{}).ok());  // "bilinear" is rejected
    std::remove((root + "/assets/bad.png.mceasset").c_str());
    std::remove((root + "/assets/bad.png").c_str());
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
    kit::Sprite soft;
    soft.texture.guid = asset::parse_guid("1176000a-0000-4000-8000-000000000001").value();
    soft.size = {4, 1};
    f.world.spawn_with(LocalTransform{{0, 1, 0}, math::quat::identity(), {1, 1, 1}}, soft);
    kit::Sprite hard = soft;
    hard.texture.guid = asset::parse_guid("1176000a-0000-4000-8000-000000000002").value();
    f.world.spawn_with(LocalTransform{{0, -1, 0}, math::quat::identity(), {1, 1, 1}}, hard);

    f.render();
    // A quarter of the way in, linear sampling is between the two texels; point sampling is black.
    CHECK(f.at_view({-0.5f, 1}).r > 60);
    CHECK(f.at_view({-0.5f, -1}).r < 10);
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

TEST_CASE("M-DATA-REACH 8, M-UI-DATA 4: a TextLabel wraps, shows its loc_key's string and draws its look") {
    using namespace cook_test;
    const std::string root = fresh_dir("kit_text");
    std::FILE* ttf = std::fopen(MCE_TEST_FONT, "rb");
    REQUIRE(ttf);
    std::string font_bytes;
    char buf[4096];
    for (usize n; (n = std::fread(buf, 1, sizeof buf, ttf)) > 0;) font_bytes.append(buf, n);
    std::fclose(ttf);
    write_file(root + "/assets/ui.ttf", font_bytes);
    write_file(root + "/assets/ui.ttf.mceasset",
               sidecar("1176000a-0000-4000-8000-000000000011", "font", R"({ "size": 26, "ranges": [[32, 126]], "padding": 1 })"));
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

    // The lit rows and columns of the frame.
    const auto extent = [&](u32& rows, u32& cols) {
        rows = cols = 0;
        for (u32 y = 0; y < kH; ++y) {
            bool lit = false;
            for (u32 x = 0; x < kW && !lit; ++x) lit = f.pixels[(usize(y) * kW + x) * 4] > 128;
            rows += lit ? 1 : 0;
        }
        for (u32 x = 0; x < kW; ++x) {
            bool lit = false;
            for (u32 y = 0; y < kH && !lit; ++y) lit = f.pixels[(usize(y) * kW + x) * 4] > 128;
            cols += lit ? 1 : 0;
        }
    };
    kit::TextLabel l;
    l.font.guid = asset::parse_guid("1176000a-0000-4000-8000-000000000011").value();
    l.text = "MMMM MMMM";
    l.color = kWhite;
    l.height = 1.0f;
    const Entity e = f.world.spawn_with(LocalTransform{}, l);
    f.render();
    u32 rows_one = 0, cols_one = 0;
    extent(rows_one, cols_one);
    REQUIRE(rows_one > 0);

    f.world.try_get_mut<kit::TextLabel>(e)->wrap_width = 3.0f;  // narrower than the line: two lines
    f.render();
    u32 rows_two = 0, cols_two = 0;
    extent(rows_two, cols_two);
    CHECK(rows_two > rows_one * 3 / 2);
    CHECK(cols_two < cols_one);

    // A loc_key with a string table: the table's text; without one, `text`.
    f.world.try_get_mut<kit::TextLabel>(e)->wrap_width = 0.0f;
    f.world.try_get_mut<kit::TextLabel>(e)->loc_key = "title";
    loc::StringTable table;
    table.set_locale("en");
    REQUIRE(table.add("title", "MM"));
    loc::Localization l10n;
    l10n.add_table(static_cast<loc::StringTable&&>(table));
    f.world.set_resource(static_cast<loc::Localization&&>(l10n));
    f.render();
    u32 rows_key = 0, cols_key = 0;
    extent(rows_key, cols_key);
    CHECK(cols_key * 3 < cols_one);

    // The look (M-UI-DATA 4): letter spacing widens the line, a red outline rings the glyphs, a
    // shadow down-right reaches rows and columns the text alone does not.
    kit::TextLabel& tl = *f.world.try_get_mut<kit::TextLabel>(e);
    tl.loc_key = "";
    tl.text = "MMMM";
    f.render();
    u32 rows_plain = 0, cols_plain = 0;
    extent(rows_plain, cols_plain);
    const auto reddish = [&] {
        u32 n = 0;
        for (usize i = 0; i < usize(kW) * kH; ++i)
            n += f.pixels[i * 4] > 128 && f.pixels[i * 4 + 1] < 64 ? 1 : 0;
        return n;
    };
    CHECK(reddish() == 0);

    // Leftmost to rightmost lit column (letter spacing adds gaps, not lit columns).
    const auto span_x = [&] {
        u32 lo = kW, hi = 0;
        for (u32 x = 0; x < kW; ++x)
            for (u32 y = 0; y < kH; ++y)
                if (f.pixels[(usize(y) * kW + x) * 4] > 128) {
                    lo = x < lo ? x : lo;
                    hi = x > hi ? x : hi;
                }
        return hi >= lo ? hi - lo + 1 : 0u;
    };
    const u32 span_plain = span_x();
    f.world.try_get_mut<kit::TextLabel>(e)->letter_spacing = 0.5f;
    f.render();
    u32 rows_spaced = 0, cols_spaced = 0;
    extent(rows_spaced, cols_spaced);
    CHECK(span_x() > span_plain);
    CHECK(rows_spaced == rows_plain);

    f.world.try_get_mut<kit::TextLabel>(e)->letter_spacing = 0.0f;
    f.world.try_get_mut<kit::TextLabel>(e)->outline_width = 0.08f;
    f.world.try_get_mut<kit::TextLabel>(e)->outline_color = math::Color{1, 0, 0, 1};
    f.render();
    CHECK(reddish() > 0);
    u32 rows_ring = 0, cols_ring = 0;
    extent(rows_ring, cols_ring);
    CHECK(rows_ring > rows_plain);
    CHECK(cols_ring > cols_plain);

    f.world.try_get_mut<kit::TextLabel>(e)->outline_width = 0.0f;
    f.world.try_get_mut<kit::TextLabel>(e)->shadow_offset = math::vec2{0.2f, -0.2f};
    f.world.try_get_mut<kit::TextLabel>(e)->shadow_color = math::Color{1, 1, 1, 1};  // bright, so `extent` sees it
    f.render();
    u32 rows_shadow = 0, cols_shadow = 0;
    extent(rows_shadow, cols_shadow);
    CHECK(rows_shadow > rows_plain);
    CHECK(cols_shadow > cols_plain);
    f.extract.reset(nullptr);
}

namespace {

// M-VIEWS 3: the frame as the player draws it with views: the textures synced, then every view.
struct ViewFrame {
    Fixture& f;
    kit::RenderTextures rts;
    Array<kit::ViewEntry> views;
    mem::FrameSlots slots{mem::heap()};  // record_views builds its batches in the frame arena
    explicit ViewFrame(Fixture& fx) : f(fx) {
        rts.init(f.renderer);
        f.extract.set_render_textures(&rts);
    }
    ~ViewFrame() {
        f.extract.set_render_textures(nullptr);
        rts.shutdown();
    }
    void render(f32 dt = 1.0f / 60.0f) {
        rts.sync(f.world, dt);
        slots.begin_frame(f.frame);
        f.device->begin_frame(f.frame++);
        REQUIRE(kit::collect_views(f.world, {kW, kH}, views, &kit::render_texture_size, &rts));
        kit::ViewRecordContext vc;
        vc.renderer = &f.renderer;
        vc.extract2d = &f.extract;
        vc.render_textures = &rts;
        vc.target = f.target;
        vc.target_size = {kW, kH};
        rhi::CommandBuffer cmd;
        stats = kit::record_views(cmd, f.world, views.span(), vc);
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, f.pixels.span()).has_value());
        f.device->end_frame();
        slots.end_frame(nullptr);
    }
    kit::ViewRecordStats stats;
};

}  // namespace

TEST_CASE("M-VIEWS 3: a camera draws into a RenderTexture that a sprite elsewhere shows") {
    Fixture f;
    ViewFrame vf(f);
    const math::Color gray{0.5f, 0.5f, 0.5f, 1.0f};
    // The window's camera at the origin; the texture's camera 20 m away, both clearing to gray.
    const Entity win = f.camera({0, 0});
    f.world.try_get_mut<kit::Camera2DComponent>(win)->clear_color = gray;
    kit::RenderTexture rt;
    rt.width = rt.height = 32;  // 2 m x 2 m at 16 px per metre
    const Entity tex = f.world.spawn_with(rt);
    kit::Camera2DComponent cc;
    cc.pixels_per_meter = kPpm;
    cc.primary = false;
    cc.clear_color = gray;
    kit::CameraView cv;
    cv.target = tex;
    f.world.spawn_with(LocalTransform{{20, 0, 0}, math::quat::identity(), {1, 1, 1}}, cc, cv);
    f.sprite({20, 0}, kRed);                            // the middle half of the texture
    const Entity screen = f.sprite({0, 0}, kWhite, {4, 4});  // the texture, twice its size
    f.world.add(screen, kit::ShowRenderTexture{tex});

    vf.render();
    CHECK(vf.stats.views_texture == 1);
    CHECK(vf.rts.live() == 1);
    CHECK(is(f.at_view({0, 0}), 255, 0, 0));  // the red sprite, seen through the texture
    // The texture's clear shows as the window's own clear does (the linear texture's encode).
    const Rgba in_tex = f.at_view({1.5f, 1.5f}), window = f.at_view({4.0f, 2.5f});
    INFO("texture clear " << int(in_tex.r) << " window clear " << int(window.r));
    CHECK(is(in_tex, window.r, window.g, window.b));
    CHECK_FALSE(is(window, 255, 255, 255));
    CHECK(f.device->validation_error_count() == 0);

    // A surface inside the texture's own view never samples it: it shows its own (green) look, and
    // upright (the texture's top-right corner is the screen's top-right).
    const Entity mirror = f.sprite({20.75f, 0.75f}, kGreen, {0.5f, 0.5f});
    f.world.add(mirror, kit::ShowRenderTexture{tex});
    vf.render();
    vf.render();
    const Rgba m = f.at_view({1.5f, 1.5f});
    INFO("mirror " << int(m.r) << " " << int(m.g) << " " << int(m.b));
    CHECK(is(m, 0, 255, 0));

    // A new size re-makes the texture; the picture stays where it was.
    const rhi::TextureHandle before = vf.rts.find(tex)->texture;
    f.world.try_get_mut<kit::RenderTexture>(tex)->width = 64;
    f.world.try_get_mut<kit::RenderTexture>(tex)->height = 64;
    vf.render();
    CHECK(vf.rts.find(tex)->texture != before);
    CHECK(vf.rts.find(tex)->size.width == 64);
    CHECK(is(f.at_view({0, 0}), 255, 0, 0));

    // update_hz: 10 redraws a second at 20 frames a second is every second frame.
    f.world.try_get_mut<kit::RenderTexture>(tex)->update_hz = 10.0f;
    u32 drawn = 0;
    for (int i = 0; i < 10; ++i) {
        vf.render(0.05f);
        drawn += vf.stats.views_texture;
    }
    CHECK(drawn == 5);
    CHECK(is(f.at_view({0, 0}), 255, 0, 0));  // a frame that skipped the redraw keeps the picture

    // The RenderTexture gone: its texture freed, the screen shows its own white.
    f.world.destroy(tex);
    vf.render();
    CHECK(vf.rts.live() == 0);
    CHECK(is(f.at_view({0, 0}), 255, 255, 255));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("M-VIEWS 5: two 2D cameras in a split screen each keep their own cell and clear color") {
    Fixture f;
    ViewFrame vf(f);
    auto cam = [&](f32 x, math::Color clear, u32 split) {
        kit::Camera2DComponent c;
        c.pixels_per_meter = kPpm;
        c.primary = false;
        c.pixel_snap = false;
        c.clear_color = clear;
        kit::CameraView cv;
        cv.split = split;
        f.world.spawn_with(LocalTransform{{x, 0, 0}, math::quat::identity(), {1, 1, 1}}, c, cv);
    };
    cam(0.0f, math::Color{0.0f, 0.0f, 0.5f, 1.0f}, 1);
    cam(50.0f, math::Color{0.5f, 0.0f, 0.0f, 1.0f}, 2);
    f.world.spawn_with(kit::SplitScreen{kit::SplitLayout::Columns, 0});
    f.sprite({0, 0}, kRed);
    f.sprite({50, 0}, kGreen);
    vf.render();
    auto px = [&](u32 x, u32 y) {
        const usize i = (usize(y) * kW + x) * 4;
        return Rgba{f.pixels[i], f.pixels[i + 1], f.pixels[i + 2], f.pixels[i + 3]};
    };
    CHECK(is(px(40, 48), 255, 0, 0));   // player 1's cell: the red sprite (a later cell's clear did not wipe it)
    CHECK(is(px(120, 48), 0, 255, 0));  // player 2's cell: the green sprite
    const Rgba a = px(2, 2), b = px(157, 2);
    INFO("cell clears " << int(a.r) << "," << int(a.g) << "," << int(a.b) << " and " << int(b.r) << "," << int(b.g) << "," << int(b.b));
    CHECK((a.b > a.r && b.r > b.b));  // each cell shows its own camera's background
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("M-VISUAL3 2: a material with a height map gets one [normal, height] array; a mismatch draws without parallax") {
    using namespace cook_test;
    const std::string root = fresh_dir("kit_parallax");
    const std::vector<u8> grey(4 * 4 * 4, 128), small(2 * 2 * 4, 128);
    for (const char* n : {"n", "h", "s"}) {
        const std::string name = std::string(root + "/assets/") + n + ".png";
        write_file(name, make_png(n[0] == 's' ? 2 : 4, n[0] == 's' ? 2 : 4, n[0] == 's' ? small : grey));
    }
    const char* guid_n = "7a270000-0000-4000-8000-000000000001";
    const char* guid_h = "7a270000-0000-4000-8000-000000000002";
    const char* guid_s = "7a270000-0000-4000-8000-000000000003";
    const char* opts = R"({ "srgb": false, "mips": false })";
    write_file(root + "/assets/n.png.mceasset", sidecar(guid_n, "texture", opts));
    write_file(root + "/assets/h.png.mceasset", sidecar(guid_h, "texture", opts));
    write_file(root + "/assets/s.png.mceasset", sidecar(guid_s, "texture", opts));
    const char* guid_c = "7a270000-0000-4000-8000-000000000004";  // an sRGB height map
    write_file(root + "/assets/c.png", make_png(4, 4, grey));
    write_file(root + "/assets/c.png.mceasset", sidecar(guid_c, "texture", R"({ "srgb": true, "mips": false })"));
    auto mat = [&](const char* guid, const std::string& textures) {
        write_file(std::string(root + "/assets/") + guid + ".mcmat",
                   std::string(R"({ "format": "mce-material", "version": 1, "guid": ")") + guid + R"(", "height_scale": 0.05, "textures": )" + textures + " }");
    };
    const char* good = "7a270000-0000-4000-8000-000000000010";
    const char* no_normal = "7a270000-0000-4000-8000-000000000011";
    const char* mismatch = "7a270000-0000-4000-8000-000000000012";
    mat(good, std::string(R"({ "normal": "guid:)") + guid_n + R"(", "height": "guid:)" + guid_h + R"(" })");
    mat(no_normal, std::string(R"({ "height": "guid:)") + guid_h + R"(" })");
    mat(mismatch, std::string(R"({ "normal": "guid:)") + guid_n + R"(", "height": "guid:)" + guid_s + R"(" })");
    const char* srgb_with_normal = "7a270000-0000-4000-8000-000000000013";
    const char* srgb_alone = "7a270000-0000-4000-8000-000000000014";
    mat(srgb_with_normal, std::string(R"({ "normal": "guid:)") + guid_n + R"(", "height": "guid:)" + guid_c + R"(" })");
    mat(srgb_alone, std::string(R"({ "height": "guid:)") + guid_c + R"(" })");
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

    mesh_fixture::Builder box;
    box.add_node(-1);
    mesh_fixture::Box b;
    b.half = {0.5f, 0.5f, 0.5f};
    box.add_box(b);
    const Array<byte> blob = box.build();
    REQUIRE(f.renderer.meshes().upload(1, 1, blob.span()).has_value());
    const asset::Guid kBox{0x3d00000000000011ull, 0x8000000000000011ull};

    World world;
    kit::install_kit3d(world, nullptr);
    kit::Kit3DExtract ex3;
    ex3.init(f.renderer.meshes(), &assets);
    ex3.set_texture_store(&f.renderer.textures());
    ex3.bind_mesh(kBox, 1);
    REQUIRE(ex3.preload().has_value());
    world.spawn_with(LocalTransform{}, kit::Camera3DComponent{});
    const char* guids[] = {good, no_normal, mismatch, srgb_with_normal, srgb_alone};
    for (const char* g : guids) {
        kit::MeshRef m;
        m.mesh.guid = kBox;
        m.material.guid = asset::parse_guid(g).value();
        world.spawn_with(LocalTransform{{0, 0, -5}, math::quat::identity(), {1, 1, 1}}, m);
    }
    world.tick(1.0f / 60.0f, 0);
    render::Camera3D cam;
    REQUIRE(ex3.camera(world, 1.0f, cam));
    render::Scene3D scene(f.renderer.meshes(), mem::heap());
    scene.begin(cam, {64, 64});
    ex3.extract(world, scene, 1.0f);
    REQUIRE(scene.instances().size() == 5);
    const render::MaterialOverride *a = scene.instances()[0].material, *n = scene.instances()[1].material, *x = scene.instances()[2].material;
    REQUIRE((a && n && x));
    CHECK(a->parallax);
    CHECK(a->parallax_normal);
    CHECK(a->ext.parallax());
    CHECK(a->normal_key >= render::TextureStore::kFirstLocalKey);
    CHECK(f.renderer.textures().contains(a->normal_key));
    CHECK(n->parallax);
    CHECK_FALSE(n->parallax_normal);
    CHECK(n->normal_key != a->normal_key);
    // 2x2 height against a 4x4 normal map: no parallax, the plain normal map stays.
    CHECK_FALSE(x->parallax);
    CHECK(x->ext.height_scale == 0.0f);
    CHECK(x->normal_key < render::TextureStore::kFirstLocalKey);
    // An sRGB height map is refused, with or without a normal map (alone it would set the array's format).
    for (u32 i : {3u, 4u}) {
        const render::MaterialOverride* c = scene.instances()[i].material;
        REQUIRE(c != nullptr);
        CHECK_FALSE(c->parallax);
        CHECK(c->ext.height_scale == 0.0f);
        CHECK((c->normal_key == ~0u || c->normal_key < render::TextureStore::kFirstLocalKey));  // no array key
    }
    ex3.shutdown();
    f.extract.reset(nullptr);
}

TEST_CASE("M-CONTENT 1b: BC cooks - a BC5 normal + BC4 height make a parallax array; a BC4 base colour warns") {
    using namespace cook_test;
    const std::string root = fresh_dir("kit_parallax_bc");
    const std::vector<u8> grey(8 * 8 * 4, 128);
    const char* guid_n = "7a270000-0000-4000-8000-000000000101";
    const char* guid_h = "7a270000-0000-4000-8000-000000000102";
    const struct {
        const char* name;
        const char* guid;
        const char* compress;
    } tex[] = {{"n", guid_n, "bc5"}, {"h", guid_h, "bc4"}};
    for (const auto& t : tex) {
        write_file(std::string(root + "/assets/") + t.name + ".png", make_png(8, 8, grey));
        write_file(std::string(root + "/assets/") + t.name + ".png.mceasset",
                   sidecar(t.guid, "texture", (std::string(R"({ "srgb": false, "mips": false, "compress": ")") + t.compress + R"(" })").c_str()));
    }
    auto mat = [&](const char* guid, const std::string& textures, const char* extra = R"("height_scale": 0.05,)") {
        write_file(std::string(root + "/assets/") + guid + ".mcmat",
                   std::string(R"({ "format": "mce-material", "version": 1, "guid": ")") + guid + R"(", )" + extra + R"( "textures": )" + textures + " }");
    };
    const char* parallax = "7a270000-0000-4000-8000-000000000110";
    const char* bc4_albedo = "7a270000-0000-4000-8000-000000000111";
    mat(parallax, std::string(R"({ "normal": "guid:)") + guid_n + R"(", "height": "guid:)" + guid_h + R"(" })");
    mat(bc4_albedo, std::string(R"({ "base_color": "guid:)") + guid_h + R"(" })", "");
    cook::CookService svc;
    cook::CookOptions opts;
    opts.textures = cook::TextureCompression::Bc;
    REQUIRE(svc.cook(StringView(root.c_str()), opts).ok());

    Fixture f;
    if (!f.device->caps().texture_bc) {
        MESSAGE("skipped: this device cannot sample BC (texture_bc false)");
        return;
    }
    asset::AssetSystem assets;
    render::AssetUploadSink sink(f.renderer, assets);
    assets.set_upload_sink(&sink);
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    f.extract.reset(&assets);

    mesh_fixture::Builder box;
    box.add_node(-1);
    mesh_fixture::Box b;
    b.half = {0.5f, 0.5f, 0.5f};
    box.add_box(b);
    const Array<byte> blob = box.build();
    REQUIRE(f.renderer.meshes().upload(1, 1, blob.span()).has_value());
    const asset::Guid kBox{0x3d00000000000011ull, 0x8000000000000011ull};

    World world;
    kit::install_kit3d(world, nullptr);
    kit::Kit3DExtract ex3;
    ex3.init(f.renderer.meshes(), &assets);
    ex3.set_texture_store(&f.renderer.textures());
    ex3.bind_mesh(kBox, 1);
    REQUIRE(ex3.preload().has_value());
    world.spawn_with(LocalTransform{}, kit::Camera3DComponent{});
    for (const char* g : {parallax, bc4_albedo}) {
        kit::MeshRef m;
        m.mesh.guid = kBox;
        m.material.guid = asset::parse_guid(g).value();
        world.spawn_with(LocalTransform{{0, 0, -5}, math::quat::identity(), {1, 1, 1}}, m);
    }
    world.tick(1.0f / 60.0f, 0);
    render::Camera3D cam;
    REQUIRE(ex3.camera(world, 1.0f, cam));
    render::Scene3D scene(f.renderer.meshes(), mem::heap());
    scene.begin(cam, {64, 64});
    log::set_level(log::Level::Info);
    const u64 seq = log::console_last_seq();
    ex3.extract(world, scene, 1.0f);
    log::ConsoleLine lines[64];
    const usize nlines = log::read_console(seq, Span<log::ConsoleLine>(lines, 64));
    log::set_level(log::Level::Warn);
    REQUIRE(scene.instances().size() == 2);

    // BC5 normal + BC4 height: one BC5 array [normal, height], parallax on.
    const render::MaterialOverride* a = scene.instances()[0].material;
    REQUIRE(a != nullptr);
    CHECK(a->parallax);
    CHECK(a->parallax_normal);
    CHECK(a->normal_key >= render::TextureStore::kFirstLocalKey);
    CHECK(f.renderer.textures().contains(a->normal_key));

    // A BC4 texture in the base-colour slot is warned about (once per material), and the BC5-only slot raises nothing.
    usize bc4_warnings = 0;
    for (usize i = 0; i < nlines; ++i)
        if (StringView(lines[i].text.c_str()).find("cooked as BC4") != StringView::npos) ++bc4_warnings;
    CHECK(bc4_warnings == 1);
    ex3.shutdown();
    f.extract.reset(nullptr);
}
