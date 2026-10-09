// mce_ui on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe), ui.md §6 "panel + button + text renders
// through SpriteBatch, ≤ 2 MDI groups": a menu drawn with the built-in skin into an offscreen
// target, probed by pixel, with the GL debug layer counting validation errors.

#include <doctest/doctest.h>

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
