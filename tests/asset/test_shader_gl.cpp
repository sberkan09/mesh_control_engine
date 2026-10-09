// A cooked shader on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe): every variant's composed
// source compiles and links, and the defines reach the pixels.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/rhi/default_device.h"

using namespace mce;
using namespace cook_test;

TEST_CASE("shader cook: composed variants compile on the GL driver and their defines reach the pixels") {
    const std::string root = fresh_dir("shader_gl");
    write_file(root + "/assets/fullscreen.vert.glsl", R"(#version 450 core
out gl_PerVertex { vec4 gl_Position; };
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)");
    write_file(root + "/assets/fullscreen.vert.glsl.mceasset", sidecar("5d000000-0000-4000-8000-000000000031", "shader"));
    write_file(root + "/assets/fill.frag.glsl", R"(#version 450 core
// @variant RED
// @variant HALF
layout(location = 0) out vec4 o_color;
layout(std140, binding = 1) uniform PassU { vec4 u_tint; };
void main() {
    vec4 c = RED == 1 ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 0.0, 1.0, 1.0);
#if HALF
    c.rgb *= 0.5;
#endif
    o_color = c * u_tint;
}
)");
    write_file(root + "/assets/fill.frag.glsl.mceasset", sidecar("5d000000-0000-4000-8000-000000000032", "shader"));
    cook::CookService svc;
    REQUIRE(svc.cook(StringView(root.c_str()), cook::CookOptions{}).ok());
    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    auto view = [&](const char* guid) {
        auto h = assets.load_sync<asset::Shader>(assets.find(asset::parse_guid(guid).value()));
        REQUIRE(h);
        auto v = asset::ShaderView::read(assets.resolve(h.value()));
        REQUIRE(v);
        return v.value();
    };
    const asset::ShaderView vs = view("5d000000-0000-4000-8000-000000000031");
    const asset::ShaderView fs = view("5d000000-0000-4000-8000-000000000032");

    REQUIRE(platform::init().has_value());
    {
        platform::WindowDesc wd;
        wd.title = "test_shader_gl";
        wd.width = 64;
        wd.height = 64;
        wd.resizable = false;
        wd.high_dpi = false;
        auto window = platform::Window::create(wd);
        REQUIRE(window.has_value());
        auto device = rhi::create_default_device(*window.value());
        REQUIRE(device.has_value());
        rhi::RenderDevice& dev = *device.value();
        rhi::TextureDesc td;
        td.size = {8, 8};
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.debug_name = "test.target";
        const rhi::TextureHandle target = dev.create_texture(td);

        Array<char> vtext;
        vs.compose(0, vtext);
        struct Expect {
            u32 mask;
            u8 r, b;
        };
        const Expect cases[] = {{0, 0, 255}, {1, 255, 0}, {2, 0, 128}, {3, 128, 0}};
        u64 frame = 0;
        for (const Expect& e : cases) {
            Array<char> ftext;
            fs.compose(e.mask, ftext);
            rhi::GraphicsPipelineDesc pd;
            pd.vertex_shader = {StringView(vtext.data(), vtext.size()), "fullscreen.vert"};
            pd.fragment_shader = {StringView(ftext.data(), ftext.size()), "fill.frag"};
            pd.state.depth = {};
            pd.state.raster.cull = rhi::CullMode::None;
            pd.debug_name = "test.fill";
            auto pipe = dev.create_pipeline(pd);
            REQUIRE(pipe.has_value());

            dev.begin_frame(frame++);
            rhi::PassAttachment color;
            color.texture = target;
            rhi::RenderPassDesc pass;
            pass.colors = Span<const rhi::PassAttachment>(&color, 1);
            rhi::CommandBuffer cmd;
            cmd.begin_pass(pass);
            cmd.bind_pipeline(pipe.value());
            const math::vec4 tint{1, 1, 1, 1};
            cmd.set_uniforms(rhi::bank::kPass, Span<const byte>(reinterpret_cast<const byte*>(&tint), sizeof(tint)));
            cmd.draw_non_indexed(3);
            cmd.end_pass();
            dev.submit(cmd);
            Array<u8> px;
            px.resize(8 * 8 * 4);
            REQUIRE(dev.read_pixels(target, rhi::Rect2D{0, 0, 8, 8}, px.span()).has_value());
            dev.end_frame();
            INFO("variant mask " << e.mask);
            CHECK(px[4 * 36 + 0] >= e.r - 2);
            CHECK(px[4 * 36 + 0] <= e.r + 2);
            CHECK(px[4 * 36 + 2] >= e.b - 2);
            CHECK(px[4 * 36 + 2] <= e.b + 2);
            dev.destroy_pipeline(pipe.value());
        }
        dev.destroy_texture(target);
        CHECK(dev.validation_error_count() == 0);
    }
    platform::shutdown();
}
