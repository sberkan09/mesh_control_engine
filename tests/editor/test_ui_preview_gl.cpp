// The UI Document panel's preview (ROADMAP M-UI-DATA step 6) on a real GL driver: the document drawn
// alone at a phone and a desktop size, its theme applied, and redrawn after an edit.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/cook/cook.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/preview_renderer.h"
#include "mce/editor/ui_document_editor.h"
#include "mce/memory/frame_slots.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/rhi/default_device.h"
#include "mce/rhi/render_device.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;

namespace {

struct Px {
    u8 r, g, b, a;
};

}  // namespace

TEST_CASE("UI document preview: a phone and a desktop frame, the theme applied, edits redrawn") {
    REQUIRE(platform::init().has_value());
    platform::WindowDesc wd;
    wd.title = "test_ui_preview_gl";
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
        const std::string root = fresh_dir("ui_preview_gl");
        std::filesystem::create_directories(root + "/assets/ui");
        REQUIRE(session.open_project(root).has_value());

        UiDocumentEditor ed;
        REQUIRE(ed.create(session, "ui/menu.mcui").has_value());
        REQUIRE(ed.add_widget(ui::kNoWidgetIndex, ui::WidgetKind::Button));
        REQUIRE(ed.save(session).has_value());

        UiDocumentPreview ui_preview;
        // Draws at `size` and reads the frame back (top row first).
        auto draw = [&](rhi::Extent2D size) {
            rhi::TextureDesc td;
            td.size = size;
            td.format = rhi::TextureFormat::RGBA8_UNorm;
            td.usage = rhi::TextureUsage::SampledColorTarget;
            const rhi::TextureHandle target = device->create_texture(td);
            slots.begin_frame(frame);
            device->begin_frame(frame++);
            {
                rhi::CommandBuffer cmd;
                ui_preview.record(cmd, preview, ed, session.hot_reload_count(), target, size, 1.0f);
                device->submit(cmd);
            }
            device->end_frame();
            slots.end_frame(nullptr);
            std::vector<Px> px(usize(size.width) * size.height);
            REQUIRE(device
                        ->read_pixels(target, rhi::Rect2D{0, 0, size.width, size.height},
                                      Span<u8>(reinterpret_cast<u8*>(px.data()), px.size() * 4))
                        .has_value());
            device->destroy_texture(target);
            return px;
        };
        // The button is 200 x 48 in the middle: a point inside it, left of its label.
        auto button = [](const std::vector<Px>& px, rhi::Extent2D size) {
            return px[usize(size.height / 2) * size.width + size.width / 2 - 80];
        };
        const rhi::Extent2D phone = ui_preview_sizes()[0].size, desktop = ui_preview_sizes()[3].size;
        CHECK(phone.width < phone.height);
        CHECK(desktop.width > desktop.height);

        const std::vector<Px> plain_phone = draw(phone);
        const Px corner = plain_phone[0];
        const Px plain = button(plain_phone, phone);
        CHECK((plain.r != corner.r || plain.g != corner.g || plain.b != corner.b));  // the button is drawn
        const Px plain_desk = button(draw(desktop), desktop);
        CHECK(plain_desk.r == plain.r);  // the same widget, centred on either screen
        CHECK(plain_desk.b == plain.b);

        // A red theme: the same document, its button red (it has the focus: a modal document's first button).
        const asset::Guid theme = cook::generate_guid();
        const std::string text =
            std::string(R"({"format":"mce-uitheme","version":1,"guid":")") + asset::to_string(theme).c_str() +
            R"(","palette":{"button":[1,0,0,1],"button_hover":[1,0,0,1],"button_focused":[1,0,0,1]}})";
        REQUIRE(session.write_asset("ui/red.mcuitheme", StringView(text.data(), text.size())).has_value());
        ui::DocumentData next = ed.data();
        next.theme = theme;
        REQUIRE(ed.apply(next));
        REQUIRE(ed.save(session).has_value());
        const Px red = button(draw(phone), phone);
        CHECK(red.r > 200);
        CHECK(red.g < 60);
        CHECK(red.b < 60);

        // An edit shows on the next frame: the button moves to the top left.
        next = ed.data();
        next.widgets[0].anchors.min = next.widgets[0].anchors.max = math::vec2(0.0f, 0.0f);
        next.widgets[0].anchors.offset_min = math::vec2(0.0f, 0.0f);
        next.widgets[0].anchors.offset_max = math::vec2(200.0f, 48.0f);
        REQUIRE(ed.apply(next));
        const std::vector<Px> moved = draw(phone);
        const Px centre = button(moved, phone), top_left = moved[usize(10) * phone.width + 10];
        CHECK(centre.r == corner.r);
        CHECK(top_left.r > 200);
        CHECK(top_left.g < 60);

        ui_preview.reset();
        CHECK(device->validation_error_count() == 0);
        session.set_preview(nullptr);
    }
    device.reset();
    window.reset();
    platform::shutdown();
}
