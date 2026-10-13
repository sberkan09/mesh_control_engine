// mce_ui on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe), ui.md §6 "panel + button + text renders
// through SpriteBatch, ≤ 2 MDI groups": a menu drawn with the built-in skin into an offscreen
// target, probed by pixel, with the GL debug layer counting validation errors.

#include <doctest/doctest.h>

#include <cstdio>

#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"
#include "mce/ui/ui.h"

using namespace mce;
using math::vec2;

namespace {

constexpr u32 kW = 320, kH = 180;

struct Rgba {
    u8 r, g, b, a;
};

}  // namespace

TEST_CASE("ui_gl: a menu renders through SpriteBatch in two MDI groups, text on its buttons") {
    REQUIRE(platform::init().has_value());
    platform::WindowDesc wd;
    wd.title = "test_ui_gl";
    wd.width = 64;
    wd.height = 64;
    wd.resizable = false;
    wd.high_dpi = false;
    auto w = platform::Window::create(wd);
    REQUIRE(w.has_value());
    mem::UniquePtr<platform::Window> window = std::move(w).value();
    auto d = rhi::create_default_device(*window);
    REQUIRE(d.has_value());
    mem::UniquePtr<rhi::RenderDevice> device = std::move(d).value();
    {
        render::Renderer renderer;
        REQUIRE(renderer.init(*device).has_value());
        ui::BuiltinSkin skin;
        REQUIRE(ui::create_builtin_skin(renderer, skin).has_value());

        ui::Document doc;
        doc.set_skin(skin.skin);
        ui::Builder b(doc, doc.root());
        b.panel("bg").fill().tint({0.0f, 0.0f, 0.5f, 1.0f});
        ui::Builder col = b.stack("buttons").at({0.5f, 0.5f}, {160, 0});
        const ui::WidgetId resume = col.button("resume", "RESUME").id();
        const ui::WidgetId quit = col.button("quit", "QUIT").id();
        ui::UpdateContext ctx;
        ctx.viewport = {kW, kH};
        doc.update(ctx);
        REQUIRE(doc.focused() == resume);

        rhi::TextureDesc td;
        td.size = {kW, kH};
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.debug_name = "test.ui";
        const rhi::TextureHandle target = device->create_texture(td);

        device->begin_frame(0);
        render::SpriteBatch batch(renderer.sprite_sets(), mem::heap());
        batch.begin(render::compute_screen_view({kW, kH}), render::PassId::Screen2D);
        doc.submit(batch);
        batch.build();
        CHECK(batch.stats().groups == 2);
        rhi::PassAttachment color;
        color.texture = target;
        color.clear.color[3] = 1.0f;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "test.ui";
        rhi::CommandBuffer cmd;
        cmd.begin_pass(pass);
        renderer.record_2d(cmd, batch);
        cmd.end_pass();
        device->submit(cmd);
        Array<u8> px;
        px.resize(usize(kW) * kH * 4);
        REQUIRE(device->read_pixels(target, rhi::Rect2D{0, 0, kW, kH}, px.span()).has_value());
        device->end_frame();
        CHECK(device->last_frame_stats().indirect_draws == 2);
        CHECK(device->validation_error_count() == 0);

        auto at = [&](vec2 p) {
            const usize i = (usize(p.y) * kW + usize(p.x)) * 4;
            return Rgba{px[i], px[i + 1], px[i + 2], px[i + 3]};
        };
        // The background panel: blue, no red.
        const Rgba bg = at({4, 4});
        CHECK(bg.b > 100);
        CHECK(bg.r < 10);
        // The focused button is warm (its focus tint), the other one grey-blue.
        const ui::Rect rr = doc.rect(resume), rq = doc.rect(quit);
        const Rgba focus_px = at(vec2(rr.min.x + 3, rr.min.y + 3));
        const Rgba plain_px = at(vec2(rq.min.x + 3, rq.min.y + 3));
        CHECK(focus_px.r > focus_px.b);
        CHECK(plain_px.b >= plain_px.r);
        // Text: the label's row has bright pixels the button's corner does not.
        u32 bright = 0;
        for (f32 x = rr.min.x; x < rr.max.x; x += 1.0f)
            for (f32 y = rr.min.y; y < rr.max.y; y += 1.0f) {
                const Rgba p = at(vec2(x, y));
                bright += p.r > 200 && p.g > 160 ? 1u : 0u;
            }
        CHECK(bright > 40);

        device->destroy_texture(target);
        renderer.shutdown();
    }
    device.reset();
    window.reset();
    platform::shutdown();
}

// M-MOBILE step 11b: one layout on three screens (a 16:9 desktop, a 20:9 phone with a notch and a
// home indicator, a 4:3 tablet), each at the UI scale its display asks for. The frames are written
// to tests/ui/tmp/ui_layout_<w>x<h>.ppm for a look; the test checks what a look would: the backdrop
// fills the screen (under the notch too), everything else stays in the safe area, the level grid
// fits the width without overlaps, the 9-slice frame keeps its border, and the open modal is on top.
namespace {

struct Screen {
    u32 w, h;
    f32 display_scale;
    ui::SafeInsets safe;
};

void write_ppm(const char* path, u32 w, u32 h, const Array<u8>& px) {
    std::FILE* f = std::fopen(path, "wb");
    REQUIRE(f);
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
    std::fclose(f);
}

bool inside(const ui::Rect& in, const ui::Rect& out) {
    return in.min.x >= out.min.x && in.min.y >= out.min.y && in.max.x <= out.max.x && in.max.y <= out.max.y;
}

}  // namespace

TEST_CASE("ui_gl: one layout on a desktop, a notched phone and a tablet (layout screenshots)") {
    REQUIRE(platform::init().has_value());
    platform::WindowDesc wd;
    wd.title = "test_ui_gl";
    wd.width = 64;
    wd.height = 64;
    wd.resizable = false;
    wd.high_dpi = false;
    auto w = platform::Window::create(wd);
    REQUIRE(w.has_value());
    mem::UniquePtr<platform::Window> window = std::move(w).value();
    auto d = rhi::create_default_device(*window);
    REQUIRE(d.has_value());
    mem::UniquePtr<rhi::RenderDevice> device = std::move(d).value();
    {
        render::Renderer renderer;
        REQUIRE(renderer.init(*device).has_value());
        ui::BuiltinSkin builtin;
        REQUIRE(ui::create_builtin_skin(renderer, builtin).has_value());

        // A 16×16 skin texture: a white 4×4 block (flat quads) and a 12×12 frame with a 3-texel red
        // border around a white middle (the 9-slice art).
        u8 tex[16 * 16 * 4];
        for (u32 y = 0; y < 16; ++y)
            for (u32 x = 0; x < 16; ++x) {
                u8* p = &tex[(y * 16 + x) * 4];
                const bool frame = x >= 4 && y >= 4;
                const bool border = frame && (x < 7 || y < 7 || x >= 13 || y >= 13);
                p[0] = 255;
                p[1] = p[2] = border ? 0 : 255;
                p[3] = 255;
            }
        const u32 key = renderer.textures().allocate_key();
        const render::TextureMipUpload mip{16, 16, Span<const byte>(reinterpret_cast<const byte*>(tex), sizeof(tex))};
        render::TextureUpload up;
        up.mips = Span<const render::TextureMipUpload>(&mip, 1);
        up.debug_name = "test.ui_skin";
        REQUIRE(renderer.textures().upload(key, 1, up).has_value());
        const render::SpriteRegion regions[2] = {{0, 1.0f / 16, 1.0f / 16, 3.0f / 16, 3.0f / 16, 1.0f, 1.0f},
                                                 {0, 4.0f / 16, 4.0f / 16, 1.0f, 1.0f, 12.0f, 12.0f}};
        render::SpriteSetDesc sd;
        sd.texture_key = key;
        sd.regions = Span<const render::SpriteRegion>(regions, 2);
        sd.pixels_per_meter = 1.0f;
        sd.filter = rhi::Filter::Nearest;
        sd.debug_name = "test.ui_skin";
        ui::Skin skin;
        skin.set = renderer.create_sprite_set(sd);
        skin.white_region = 0;
        skin.font = builtin.skin.font;

        // The layout: a backdrop under everything (the whole screen), a framed title bar, tabs with a
        // scrolling, width-fitting level grid on the first page, and a quit modal on top.
        ui::Document doc;
        doc.set_skin(skin);
        ui::Builder b(doc, doc.root());
        const ui::WidgetId backdrop = b.panel("backdrop").to(ui::AnchorTo::Screen).fill().tint({0.0f, 0.0f, 0.6f, 1.0f}).id();
        ui::Builder title = b.image("title", 1).slice(ui::Slice{3, 3, 3, 3}).anchors(ui::Anchors{{0, 0}, {1, 0}, {16, 12}, {-16, 64}});
        ui::Style dark_text;
        dark_text.text_color = math::Color{0.1f, 0.1f, 0.1f, 1.0f};
        title.text("label", "SELECT A LEVEL").at({0.5f, 0.5f}, {0, 0}).style(dark_text);
        ui::Builder tabs = b.tabs("tabs").anchors(ui::Anchors{{0, 0}, {1, 1}, {16, 76}, {-16, -16}});
        ui::GridLayout g;
        g.columns = 0;
        g.cell = vec2(120, 64);
        g.spacing = vec2(12, 12);
        ui::Builder grid = tabs.tab("levels", "LEVELS").scroll("list").fill().grid("grid", g);
        ui::WidgetId cells[24];
        for (u32 i = 0; i < 24; ++i) cells[i] = grid.button("level", "LEVEL").id();
        tabs.tab("settings", "SETTINGS").toggle("music", "MUSIC", true).at({0.5f, 0.3f}, {0, 0});
        ui::Builder quit = b.modal("quit");
        ui::Builder box = quit.stack("box", ui::StackLayout{ui::Axis::Vertical, 8.0f, 12.0f}).at({0.5f, 0.5f}, {260, 0});
        ui::Style card;
        card.tint = math::Color{0.12f, 0.12f, 0.16f, 1.0f};
        box.style(card);
        box.text("ask", "QUIT THE GAME?");
        const ui::WidgetId yes = box.button("yes", "YES").id();
        box.button("no", "NO");

        const Screen screens[] = {
            {1280, 720, 1.0f, {}},
            {2400, 1080, 2.625f, {132, 0, 132, 63}},  // landscape phone: camera cut-out left and right, home indicator
            {2048, 1536, 2.0f, {0, 48, 0, 40}},
        };
        for (const Screen& s : screens) {
            CAPTURE(s.w);
            ui::UpdateContext ctx;
            ctx.viewport = {s.w, s.h};
            ctx.safe_insets = s.safe;
            ctx.ui_scale = ui::compute_ui_scale(ctx.viewport, s.display_scale);
            doc.close_modal(quit.id());
            doc.update(ctx);
            // Layout: the backdrop is the screen; the rest is inside the safe area; cells do not overlap.
            const ui::Rect safe{vec2(s.safe.left, s.safe.top), vec2(static_cast<f32>(s.w) - s.safe.right, static_cast<f32>(s.h) - s.safe.bottom)};
            CHECK(doc.rect(backdrop).min == vec2(0, 0));
            CHECK(doc.rect(backdrop).max == vec2(static_cast<f32>(s.w), static_cast<f32>(s.h)));
            CHECK(inside(doc.rect(title.id()), safe));
            CHECK(inside(doc.rect(tabs.id()), safe));
            const ui::Rect list = doc.rect(doc.find("tabs/pages/levels/list"));
            u32 per_row = 0;
            for (u32 i = 0; i < 24; ++i) per_row += doc.rect(cells[i]).min.y == doc.rect(cells[0]).min.y ? 1u : 0u;
            CHECK(per_row >= 2);
            CHECK(doc.rect(cells[per_row - 1]).max.x <= list.max.x);  // the row fits the width
            for (u32 i = 0; i + 1 < 24; ++i) {
                const ui::Rect a = doc.rect(cells[i]), c = doc.rect(cells[i + 1]);
                CHECK((a.max.x <= c.min.x || a.max.y <= c.min.y));
            }
            doc.open_modal(quit.id());
            doc.update(ctx);
            CHECK(doc.focused() == yes);

            rhi::TextureDesc td;
            td.size = {s.w, s.h};
            td.usage = rhi::TextureUsage::SampledColorTarget;
            td.debug_name = "test.ui_layout";
            const rhi::TextureHandle target = device->create_texture(td);
            device->begin_frame(0);
            render::SpriteBatch batch(renderer.sprite_sets(), mem::heap());
            batch.begin(render::compute_screen_view({s.w, s.h}), render::PassId::Screen2D);
            doc.submit(batch);
            batch.build();
            CHECK(batch.stats().groups == 4);  // skin + font, and again two layers up for the open modal
            rhi::PassAttachment color;
            color.texture = target;
            color.clear.color[3] = 1.0f;
            rhi::RenderPassDesc pass;
            pass.colors = Span<const rhi::PassAttachment>(&color, 1);
            pass.debug_name = "test.ui_layout";
            rhi::CommandBuffer cmd;
            cmd.begin_pass(pass);
            renderer.record_2d(cmd, batch);
            cmd.end_pass();
            device->submit(cmd);
            Array<u8> px;
            px.resize(usize(s.w) * s.h * 4);
            REQUIRE(device->read_pixels(target, rhi::Rect2D{0, 0, s.w, s.h}, px.span()).has_value());
            device->end_frame();
            CHECK(device->validation_error_count() == 0);
            char path[512];
            std::snprintf(path, sizeof(path), "%s/ui_layout_%ux%u.ppm", MCE_TEST_TMP_DIR, s.w, s.h);
            write_ppm(path, s.w, s.h, px);

            auto at = [&](f32 x, f32 y) {
                const usize i = (usize(y) * s.w + usize(x)) * 4;
                return Rgba{px[i], px[i + 1], px[i + 2], px[i + 3]};
            };
            // Under the notch: the backdrop's blue, dimmed by the modal veil (not the clear black).
            const Rgba corner = at(2, static_cast<f32>(s.h) / 2);
            CHECK(corner.b > 40);
            CHECK(corner.r < 10);
            // The 9-slice frame: its border is 3 texels × the UI scale, its middle white.
            const ui::Rect t = doc.rect(title.id());
            const f32 border = 3.0f * ctx.ui_scale;
            const Rgba edge = at(t.min.x + 1, t.center().y), mid = at(t.min.x + border + 2, t.center().y);
            CHECK(edge.r > 100);
            CHECK(edge.g < 10);
            CHECK(mid.g > 100);  // white (dimmed by the veil), not the border's red
            // The modal's YES button is drawn above the veil: its focus tint, not the dimmed backdrop.
            const Rgba y = at(doc.rect(yes).min.x + 3, doc.rect(yes).min.y + 3);
            CHECK(y.r > y.b);
            device->destroy_texture(target);
        }
        renderer.shutdown();
    }
    device.reset();
    window.reset();
    platform::shutdown();
}
