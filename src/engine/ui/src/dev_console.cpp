#include "mce/convar.h"
#include "mce/log.h"
#include "mce/ui/ui.h"

namespace mce::ui {
namespace {

constexpr Color kEcho{0.55f, 0.75f, 1.0f, 1};
constexpr Color kInfo{0.85f, 0.85f, 0.85f, 1};
constexpr Color kWarn{1.0f, 0.82f, 0.35f, 1};
constexpr Color kError{1.0f, 0.42f, 0.36f, 1};

Color level_color(log::Level l) {
    switch (l) {
        case log::Level::Warn: return kWarn;
        case log::Level::Error:
        case log::Level::Critical: return kError;
        case log::Level::Trace:
        case log::Level::Debug: return Color{0.6f, 0.6f, 0.62f, 1};
        default: break;
    }
    return kInfo;
}

}  // namespace

DevConsole::DevConsole(mem::IAllocator& a) : doc_(a) {}

void DevConsole::init(const Skin& skin, f32 height_fraction) {
    doc_.set_skin(skin);
    Anchors top;
    top.min = vec2(0, 0);
    top.max = vec2(1, height_fraction);
    console_ = Builder(doc_, doc_.root()).console("console").anchors(top).id();
    doc_.set_visible(console_, false);
    doc_.console_push_line(console_, "console: 'help' lists commands, Up/Down recall, Esc closes.", kWarn);
    seq_ = 0;  // the whole log ring: startup warnings are what a console is opened for
}

void DevConsole::set_open(bool open) {
    open_ = open;
    doc_.set_visible(console_, open);
}

void DevConsole::update(const UpdateContext& ctx) {
    log::ConsoleLine lines[32];
    for (;;) {
        const usize n = log::read_console(seq_, Span<log::ConsoleLine>(lines, 32));
        for (usize i = 0; i < n; ++i) {
            doc_.console_push_line(console_, lines[i].text.view(), level_color(lines[i].level));
            seq_ = lines[i].seq;
        }
        if (n < 32) break;
    }
    if (!open_) return;
    doc_.update(ctx);
    for (const UiEvent& e : doc_.events()) {
        if (e.kind == UiEvent::Cancelled) set_open(false);
        if (e.kind == UiEvent::Submitted) run(doc_.console_submitted());
    }
}

void DevConsole::submit(render::SpriteBatch& batch, i16 sort_layer) const {
    if (open_) doc_.submit(batch, sort_layer);
}

void DevConsole::run(StringView line) {
    FixedString<192> echo("> ");
    echo.append(line);
    doc_.console_push_line(console_, echo.view(), kEcho);
    if (line == "clear") {
        doc_.console_clear(console_);
        return;
    }
    if (line == "help") {
        Array<ConVar*> vars;
        ConVar::collect(vars);
        for (const ConVar* v : vars) {
            if (v->flags() & CVarFlags::Cheat) continue;
            FixedString<128> value;
            v->to_string(value);
            FixedString<192> out(v->name());
            out.append(" = ");
            out.append(value.view());
            out.append("   ");
            out.append(v->help());
            doc_.console_push_line(console_, out.view());
        }
        Array<ConFunc*> funcs;
        ConFunc::collect(funcs);
        for (const ConFunc* f : funcs) {
            FixedString<192> out(f->name());
            out.append("(...)   ");
            out.append(f->help());
            doc_.console_push_line(console_, out.view());
        }
        return;
    }
    if (!console::execute(line)) {
        FixedString<192> out("unknown command or bad value: ");
        out.append(line);
        doc_.console_push_line(console_, out.view(), kError);
    }
}

}  // namespace mce::ui
