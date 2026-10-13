// ui_gallery — mce_ui end to end (ROADMAP M8, docs/architecture/ui.md): a title menu, an options
// page whose sliders and toggles are bound two-way to convars (one of them is the UI scale itself),
// a 3×3 level grid navigated geometrically, and the shipped developer console (ui::DevConsole). Everything draws through one
// SpriteBatch in PassId::Screen2D with the built-in skin; there is no ImGui in this executable.
//
//   ui_gallery                                 arrows / Enter / Esc, or the mouse; F1 or ` toggles the console
//   ui_gallery -page options -frames 30 -capture out.ppm
//                                              start on a page (title|options|levels; console = title + the console), write the last frame

#include <cstdio>

#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"
#include "mce/ui/ui.h"

namespace {

using namespace mce;
using namespace mce::math;
using namespace mce::render;

MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_page, "page", "title", "First page: title | options | levels | console", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);
// The options page's convars (a game's real settings would be audio.* / r.*).
MCE_CVAR(cv_ui_scale, "ui.scale", 1.0f, "UI scale multiplier (FR-R2D-080)", CVarFlags::Saved);
MCE_CVAR(cv_volume, "gallery.volume", 0.8f, "Master volume (demo)", CVarFlags::Saved);
MCE_CVAR(cv_music, "gallery.music", 0.5f, "Music volume (demo)", CVarFlags::Saved);
MCE_CVAR(cv_subtitles, "gallery.subtitles", true, "Show subtitles (demo)", CVarFlags::Saved);
MCE_CVAR(cv_shake, "gallery.screen_shake", false, "Screen shake (demo)", CVarFlags::Saved);

enum class Page : u8 { Title, Options, Levels };

class GalleryApp final : public app::Application, public app::IPlatformHooks {
public:
    GalleryApp(platform::Window& window, rhi::RenderDevice& device) : window_(window), device_(device) {}

    ResultV on_init(app::Context&) override {
        MCE_TRY(renderer_.init(device_));
        MCE_TRY(ui::create_builtin_skin(renderer_, skin_));
        build_title();
        build_options();
        build_levels();
        dev_.init(skin_.skin);
        const StringView start = cv_page.get_string();
        open(start == "options" ? Page::Options : start == "levels" ? Page::Levels : Page::Title);
        if (start == "console") {
            MCE_LOG_INFO("ui_gallery: try 'gallery.volume 0.3', then look at the options page");
            set_console(true);
        }
        window_.set_event_handler(&GalleryApp::on_event, this);
        return ok();
    }

    void on_tick(const app::TickContext&) override {}

    void on_render(const app::RenderContext& rc) override {
        device_.begin_frame(rc.frame_index);
        const rhi::Extent2D bb = device_.backbuffer_size();

        // Input for this frame -> the open page.
        input::InputState in;
        in.move = held_;
        in.confirm_pressed = confirm_;
        in.cancel_pressed = cancel_;
        ui::UpdateContext ctx;
        ctx.viewport = bb;
        ctx.ui_scale = ui::compute_ui_scale(bb, window_.display_scale(), cv_ui_scale.get_float());
        ctx.actions = &in;
        ctx.pointer = pointer_;
        ctx.pointer_down = pointer_down_;
        ctx.pointer_pressed = pointer_pressed_;
        ctx.pointer_released = pointer_released_;
        ctx.text_input = StringView(typed_.c_str(), typed_.size());
        ctx.key_backspace = backspace_;
        ctx.key_enter = enter_;
        ctx.key_up = key_up_;
        ctx.key_down = key_down_;
        ctx.dt = 1.0 / 60.0;
        confirm_ = cancel_ = pointer_pressed_ = pointer_released_ = backspace_ = enter_ = key_up_ = key_down_ = false;
        typed_.clear();

        const f64 t0 = platform::now_seconds();
        // While the console is open it owns the input; the page below still updates (and draws).
        const bool console = dev_.is_open();
        ui::UpdateContext page_ctx = ctx;
        if (console) {
            page_ctx.actions = nullptr;
            page_ctx.pointer = vec2(-1, -1);
            page_ctx.pointer_pressed = page_ctx.pointer_released = page_ctx.pointer_down = false;
        }
        ui::Document& doc = page_doc(page_);
        doc.update(page_ctx);
        handle(doc);
        ui::Document& shown = page_doc(page_);  // handle() may have switched pages
        if (&shown != &doc) shown.update(page_ctx);
        dev_.update(ctx);
        if (console && !dev_.is_open()) window_.set_text_input(false);  // closed with Esc
        hud_text();
        hud_.update(ctx);
        const f64 t1 = platform::now_seconds();

        SpriteBatch batch(renderer_.sprite_sets(), mem::frame());
        batch.begin(compute_screen_view(bb), PassId::Screen2D);
        shown.submit(batch, 100);
        hud_.submit(batch, 110);
        dev_.submit(batch, 200);
        batch.build();
        ui_ms_ = static_cast<f32>((platform::now_seconds() - t0) * 1000.0);
        update_ms_ = static_cast<f32>((t1 - t0) * 1000.0);
        groups_ = batch.stats().groups;
        quads_ = batch.stats().quads;

        rhi::PassAttachment color;
        color.clear.color[0] = 0.07f;
        color.clear.color[1] = 0.08f;
        color.clear.color[2] = 0.11f;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "ui_gallery";
        rhi::CommandBuffer cmd(mem::frame());
        cmd.begin_pass(pass);
        renderer_.record_2d(cmd, batch);
        cmd.end_pass();
        device_.submit(cmd);

        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last && !cv_capture.get_string().empty()) capture(cv_capture.get_string().data(), bb);
        device_.end_frame();
        if (last) quit(0);
    }

    void on_shutdown() override {
        std::printf("ui_gallery: last frame %u quads in %u MDI groups; ui update %.3f ms, update+submit+build %.3f ms\n", quads_,
                    groups_, static_cast<f64>(update_ms_), static_cast<f64>(ui_ms_));
        renderer_.shutdown();
    }

    bool pump_events() override { return platform::pump_events(); }
    void present(bool vsync) override { device_.present(vsync); }

private:
    // ---- pages -------------------------------------------------------------------------------

    static ui::Builder heading(ui::Builder& root, const char* text) {
        ui::Anchors a;
        a.min = a.max = vec2(0.5f, 0.0f);
        a.pivot = vec2(0.5f, 0.0f);
        a.offset_min = a.offset_max = vec2(0, 60);
        ui::Builder h = root.text("heading", text);
        h.anchors(a).text_size(26.0f);
        return h;
    }

    void build_title() {
        ui::Builder root(title_doc_, title_doc_.root());
        title_doc_.set_skin(skin_.skin);
        root.panel("bg").fill().tint({0.05f, 0.05f, 0.08f, 1.0f});
        heading(root, "MESH CONTROL ENGINE");
        ui::Builder col = root.stack("menu", ui::StackLayout{ui::Axis::Vertical, 10.0f, 12.0f}).at({0.5f, 0.55f}, {280, 0});
        ui::Style backdrop;
        backdrop.tint = Color{0.1f, 0.1f, 0.14f, 0.9f};
        col.style(backdrop);
        btn_play_ = col.button("play", "PLAY").default_focus().id();
        btn_options_ = col.button("options", "OPTIONS").id();
        btn_console_ = col.button("console", "CONSOLE").id();
        btn_quit_ = col.button("quit", "QUIT").id();
        root.text("hint", "ARROWS + ENTER, OR THE MOUSE.  ESC GOES BACK.  F1: CONSOLE").at({0.5f, 0.94f}, {0, 0});
    }

    void build_options() {
        options_doc_.set_skin(skin_.skin);
        ui::Builder root(options_doc_, options_doc_.root());
        root.panel("bg").fill().tint({0.05f, 0.06f, 0.08f, 1.0f});
        heading(root, "OPTIONS");
        ui::Builder card = root.panel("card").at({0.5f, 0.5f}, {520, 330});
        card.tint({0.1f, 0.11f, 0.15f, 0.95f});
        ui::Builder labels = card.stack("labels", ui::StackLayout{ui::Axis::Vertical, 18.0f, 0.0f, ui::Align::Start});
        ui::Anchors la;
        la.min = la.max = vec2(0, 0);
        la.pivot = vec2(0, 0);
        la.offset_min = la.offset_max = vec2(28, 34);
        labels.anchors(la);
        const char* names[] = {"UI SCALE", "MASTER VOLUME", "MUSIC VOLUME"};
        for (const char* n : names) labels.text(n, n).at({0, 0}, {0, 20});
        ui::Builder ctl = card.stack("controls", ui::StackLayout{ui::Axis::Vertical, 14.0f, 0.0f});
        ui::Anchors ca;
        ca.min = ca.max = vec2(1, 0);
        ca.pivot = vec2(1, 0);
        ca.offset_min = vec2(-268, 30);
        ca.offset_max = vec2(-28, 30);
        ctl.anchors(ca);
        opt_scale_ = ctl.slider("scale", 0.75f, 2.0f, 0.25f).bind_convar("ui.scale").id();
        ctl.slider("volume", 0.0f, 1.0f, 0.1f).bind_convar("gallery.volume");
        ctl.slider("music", 0.0f, 1.0f, 0.1f).bind_convar("gallery.music");
        ctl.toggle("subtitles", "SUBTITLES").bind_convar("gallery.subtitles");
        ctl.toggle("shake", "SCREEN SHAKE").bind_convar("gallery.screen_shake");
        opt_back_ = ctl.button("back", "BACK").id();
        root.text("hint", "LEFT / RIGHT CHANGES A SLIDER.  VALUES ARE CONVARS: TRY 'gallery.volume 0.3' IN THE CONSOLE")
            .at({0.5f, 0.94f}, {0, 0});
    }

    void build_levels() {
        levels_doc_.set_skin(skin_.skin);
        ui::Builder root(levels_doc_, levels_doc_.root());
        root.panel("bg").fill().tint({0.06f, 0.05f, 0.07f, 1.0f});
        heading(root, "SELECT A LEVEL");
        ui::Builder grid = root.grid("grid", ui::GridLayout{3, {150, 90}, {16, 16}}).at({0.5f, 0.52f}, {0, 0});
        const char* names[] = {"1 EMBERS", "2 SHAFT", "3 DASH", "4 CRUMBLE", "5 ALL", "6 COURT", "7 VAULT", "8 SPIRE", "9 CORE"};
        for (u32 i = 0; i < 9; ++i) {
            ui::Builder b = grid.button(names[i], names[i]);
            if (i >= 6) levels_doc_.set_interactable(b.id(), false);  // locked
        }
        root.text("hint", "GEOMETRIC NAVIGATION: NO EDGES AUTHORED.  LOCKED LEVELS ARE SKIPPED.").at({0.5f, 0.94f}, {0, 0});
    }

    void set_console(bool on) {
        dev_.set_open(on);
        window_.set_text_input(on);
    }

    ui::Document& page_doc(Page p) {
        switch (p) {
            case Page::Options: return options_doc_;
            case Page::Levels: return levels_doc_;
            case Page::Title: break;
        }
        return title_doc_;
    }

    void open(Page p) {
        page_ = p;
    }

    void handle(ui::Document& doc) {
        for (const ui::UiEvent& e : doc.events()) {
            if (e.kind == ui::UiEvent::Cancelled) {
                open(Page::Title);
                return;
            }
            if (e.kind != ui::UiEvent::Confirmed) continue;
            if (e.widget == btn_play_) open(Page::Levels);
            else if (e.widget == btn_options_) open(Page::Options);
            else if (e.widget == btn_console_) set_console(true);
            else if (e.widget == btn_quit_) quit(0);
            else if (e.widget == opt_back_) open(Page::Title);
            else if (&doc == &levels_doc_) {
                MCE_LOG_INFO("ui_gallery: level picked: {}", doc.text(e.widget));
            }
        }
    }

    void hud_text() {
        if (hud_line_.is_null()) {
            hud_.set_skin(skin_.skin);
            hud_.set_modal(false);
            ui::Anchors a;
            a.min = a.max = vec2(1, 1);
            a.pivot = vec2(1, 1);
            a.offset_min = a.offset_max = vec2(-10, -8);
            hud_line_ = ui::Builder(hud_, hud_.root()).text("stats", "").anchors(a).id();
        }
        char line[128];
        std::snprintf(line, sizeof(line), "UI %.3f MS  QUADS %u  MDI %u  SCALE %.2f", static_cast<f64>(ui_ms_), quads_, groups_,
                      static_cast<f64>(cv_ui_scale.get_float()));
        hud_.set_text(hud_line_, line);
    }

    // ---- platform ----------------------------------------------------------------------------

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_.read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("ui_gallery: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("ui_gallery: wrote {}", path);
    }

    static void on_event(const platform::Event& e, void* user) {
        auto* self = static_cast<GalleryApp*>(user);
        using platform::Key;
        switch (e.type) {
            case platform::EventType::KeyDown:
            case platform::EventType::KeyUp: {
                const bool down = e.type == platform::EventType::KeyDown;
                const bool fresh = down && !(e.flags & platform::EventFlag::Repeat);
                const auto key = static_cast<Key>(e.code);
                const bool console = self->dev_.is_open();
                if (key == Key::Left) self->held_.x = down ? -1.0f : (self->held_.x < 0 ? 0.0f : self->held_.x);
                if (key == Key::Right) self->held_.x = down ? 1.0f : (self->held_.x > 0 ? 0.0f : self->held_.x);
                if (!console && key == Key::Up) self->held_.y = down ? 1.0f : (self->held_.y > 0 ? 0.0f : self->held_.y);
                if (!console && key == Key::Down) self->held_.y = down ? -1.0f : (self->held_.y < 0 ? 0.0f : self->held_.y);
                if (!down) break;
                if (console && key == Key::Up) self->key_up_ = true;
                if (console && key == Key::Down) self->key_down_ = true;
                if (key == Key::Backspace) self->backspace_ = true;
                if (key == Key::Return) (console ? self->enter_ : self->confirm_) = fresh || console;
                if (fresh && key == Key::Escape) self->cancel_ = true;
                if (fresh && (key == Key::F1 || key == Key::Grave)) self->set_console(!console);
                break;
            }
            case platform::EventType::TextInput: {
                if (e.code < 128 && e.code != '`') {
                    const char c = static_cast<char>(e.code);
                    self->typed_.append(StringView(&c, 1));
                }
                break;
            }
            case platform::EventType::MouseMove: self->pointer_ = vec2(e.x, e.y); break;
            case platform::EventType::MouseButtonDown:
                if (e.code == static_cast<u32>(platform::MouseButton::Left)) {
                    self->pointer_ = vec2(e.x, e.y);
                    self->pointer_down_ = self->pointer_pressed_ = true;
                }
                break;
            case platform::EventType::MouseButtonUp:
                if (e.code == static_cast<u32>(platform::MouseButton::Left)) {
                    self->pointer_down_ = false;
                    self->pointer_released_ = true;
                }
                break;
            default: break;
        }
    }

    platform::Window& window_;
    rhi::RenderDevice& device_;
    Renderer renderer_;
    ui::BuiltinSkin skin_;
    ui::Document title_doc_, options_doc_, levels_doc_, hud_;
    ui::DevConsole dev_;
    ui::WidgetId btn_play_, btn_options_, btn_console_, btn_quit_, opt_scale_, opt_back_, hud_line_;
    Page page_ = Page::Title;
    vec2 held_, pointer_{-1, -1};
    bool confirm_ = false, cancel_ = false, pointer_down_ = false, pointer_pressed_ = false, pointer_released_ = false;
    bool backspace_ = false, enter_ = false, key_up_ = false, key_down_ = false;
    FixedString<64> typed_;
    f32 ui_ms_ = 0.0f, update_ms_ = 0.0f;
    u32 groups_ = 0, quads_ = 0;
};

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("ui_gallery", text.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "ui_gallery";
    log::init(log_desc);
    console::parse_command_line(argc, argv);

    int rc = 1;
    if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        platform::WindowDesc wd;
        wd.title = "MCE - ui_gallery";
        wd.width = 1280;
        wd.height = 720;
        auto window = platform::Window::create(wd);
        if (!window) {
            rc = fail("window creation failed", window.error());
        } else {
            rhi::DeviceDesc dd;
            dd.debug_layer = cv_gl_debug.get_bool();
            auto device = rhi::create_default_device(*window.value(), dd);
            if (!device) {
                rc = fail("render device creation failed", device.error());
            } else {
                GalleryApp app(*window.value(), *device.value());
                app::AppDesc desc;
                desc.name = "ui_gallery";
                desc.platform = &app;
                desc.fences = device.value().get();
                app::RunStats stats;
                rc = app::run(app, desc, &stats);
                const u32 gl_errors = device.value()->validation_error_count();
                std::printf("ui_gallery: %llu frames, %u GL errors\n", static_cast<unsigned long long>(stats.frames), gl_errors);
                if (rc == 0 && gl_errors != 0) rc = 2;
            }
        }
    }
    platform::shutdown();
    log::shutdown();
    return rc;
}
