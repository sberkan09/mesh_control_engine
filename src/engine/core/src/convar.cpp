#include "mce/convar.h"

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mce/log.h"
#include "mce/web.h"

namespace mce {
namespace {

// Zero-initialized before any dynamic initializer runs, so registrars may link in any order.
constinit ConVar* g_cvar_head = nullptr;
constinit ConFunc* g_cfunc_head = nullptr;

// After a stable sort by name of a newest-first list: keeps the first (newest) of each name.
template <class T>
void drop_shadowed(Array<T*>& out) {
    usize w = 0;
    for (usize i = 0; i < out.size(); ++i)
        if (w == 0 || StringView(out[w - 1]->name()) != StringView(out[i]->name())) out[w++] = out[i];
    while (out.size() > w) out.pop_back();
}

bool parse_bool(StringView s, bool& out) {
    if (s == "1" || s == "true" || s == "on" || s == "yes") {
        out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "off" || s == "no") {
        out = false;
        return true;
    }
    return false;
}

bool parse_int(StringView s, i32& out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool parse_float(StringView s, f32& out) {
    // std::from_chars for floats is not universally available; use strtof on a NUL-terminated copy.
    FixedString<64> tmp(s);
    if (tmp.truncated() || tmp.empty()) return false;
    char* end = nullptr;
    out = std::strtof(tmp.c_str(), &end);
    return end == tmp.c_str() + tmp.size();
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

}  // namespace

// ------------------------------------------------------------------ ConVar

ConVar::ConVar(const char* name, bool def, const char* help, u32 flags)
    : name_(name), help_(help), id_(name), flags_(flags), type_(CVarType::Bool) {
    value_.b = default_.b = def;
    link();
}
ConVar::ConVar(const char* name, i32 def, const char* help, u32 flags)
    : name_(name), help_(help), id_(name), flags_(flags), type_(CVarType::Int) {
    value_.i = default_.i = def;
    link();
}
ConVar::ConVar(const char* name, f32 def, const char* help, u32 flags)
    : name_(name), help_(help), id_(name), flags_(flags), type_(CVarType::Float) {
    value_.f = default_.f = def;
    link();
}
ConVar::ConVar(const char* name, const char* def, const char* help, u32 flags)
    : name_(name), help_(help), id_(name), flags_(flags), type_(CVarType::String), str_(def), default_str_(def) {
    link();
}

ConVar::~ConVar() { unlink(); }

void ConVar::unlink() {
    for (ConVar** p = &g_cvar_head; *p; p = &(*p)->next_) {
        if (*p == this) {
            *p = next_;
            break;
        }
    }
    next_ = nullptr;
}

void ConVar::link() {
    // A name already taken is shadowed, not refused: two copies of one game module are live at a
    // rebuild (the old one until its World is gone), and find() and collect() see the newest.
    next_ = g_cvar_head;
    g_cvar_head = this;
}

namespace {
u64 g_saved_generation = 0;
}

void ConVar::changed() {
    if (flags_ & CVarFlags::Saved) ++g_saved_generation;
    if (on_change_) on_change_(*this);
}

bool ConVar::get_bool() const {
    switch (type_) {
        case CVarType::Bool: return value_.b;
        case CVarType::Int: return value_.i != 0;
        case CVarType::Float: return value_.f != 0.0f;
        case CVarType::String: {
            bool b = false;
            return parse_bool(str_.view(), b) && b;
        }
    }
    return false;
}

i32 ConVar::get_int() const {
    switch (type_) {
        case CVarType::Bool: return value_.b ? 1 : 0;
        case CVarType::Int: return value_.i;
        case CVarType::Float: return static_cast<i32>(value_.f);
        case CVarType::String: {
            i32 i = 0;
            return parse_int(str_.view(), i) ? i : 0;
        }
    }
    return 0;
}

f32 ConVar::get_float() const {
    switch (type_) {
        case CVarType::Bool: return value_.b ? 1.0f : 0.0f;
        case CVarType::Int: return static_cast<f32>(value_.i);
        case CVarType::Float: return value_.f;
        case CVarType::String: {
            f32 f = 0.0f;
            return parse_float(str_.view(), f) ? f : 0.0f;
        }
    }
    return 0.0f;
}

bool ConVar::set_bool(bool v, bool force) {
    if ((flags_ & CVarFlags::ReadOnly) && !force) return false;
    switch (type_) {
        case CVarType::Bool: value_.b = v; break;
        case CVarType::Int: value_.i = v ? 1 : 0; break;
        case CVarType::Float: value_.f = v ? 1.0f : 0.0f; break;
        case CVarType::String: str_.assign(v ? "1" : "0"); break;
    }
    changed();
    return true;
}

bool ConVar::set_int(i32 v, bool force) {
    if ((flags_ & CVarFlags::ReadOnly) && !force) return false;
    switch (type_) {
        case CVarType::Bool: value_.b = v != 0; break;
        case CVarType::Int: value_.i = v; break;
        case CVarType::Float: value_.f = static_cast<f32>(v); break;
        case CVarType::String: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", v);
            str_.assign(buf);
            break;
        }
    }
    changed();
    return true;
}

bool ConVar::set_float(f32 v, bool force) {
    if ((flags_ & CVarFlags::ReadOnly) && !force) return false;
    switch (type_) {
        case CVarType::Bool: value_.b = v != 0.0f; break;
        case CVarType::Int: value_.i = static_cast<i32>(v); break;
        case CVarType::Float: value_.f = v; break;
        case CVarType::String: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
            str_.assign(buf);
            break;
        }
    }
    changed();
    return true;
}

bool ConVar::set_string(StringView v, bool force) {
    if (type_ != CVarType::String) return set_from_string(v, force);
    if ((flags_ & CVarFlags::ReadOnly) && !force) return false;
    if (v.size() > str_.capacity()) return false;  // never keep a silently cut path
    str_.assign(v);
    changed();
    return true;
}

bool ConVar::set_from_string(StringView text, bool force) {
    switch (type_) {
        case CVarType::Bool: {
            bool b;
            return parse_bool(text, b) && set_bool(b, force);
        }
        case CVarType::Int: {
            i32 i;
            return parse_int(text, i) && set_int(i, force);
        }
        case CVarType::Float: {
            f32 f;
            return parse_float(text, f) && set_float(f, force);
        }
        case CVarType::String: return set_string(text, force);
    }
    return false;
}

void ConVar::to_string(FixedString<128>& out) const {
    char buf[64];
    switch (type_) {
        case CVarType::Bool: out.assign(value_.b ? "true" : "false"); return;
        case CVarType::Int: std::snprintf(buf, sizeof(buf), "%d", value_.i); break;
        case CVarType::Float: std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(value_.f)); break;
        case CVarType::String: out.assign(str_.view()); return;
    }
    out.assign(buf);
}

bool ConVar::is_default() const {
    switch (type_) {
        case CVarType::Bool: return value_.b == default_.b;
        case CVarType::Int: return value_.i == default_.i;
        case CVarType::Float: return value_.f == default_.f;
        case CVarType::String: return str_.view() == default_str_.view();
    }
    return true;
}

void ConVar::reset() {
    value_ = default_;
    str_.assign(default_str_.view());
    changed();
}

bool ConVar::set_default(StringView text) {
    const bool keep = from_command_line_ || !is_default();  // a value someone chose stays; only the default moves
    switch (type_) {
        case CVarType::Bool: {
            bool b;
            if (!parse_bool(text, b)) return false;
            default_.b = b;
            if (!keep) value_.b = b;
            break;
        }
        case CVarType::Int: {
            i32 i;
            if (!parse_int(text, i)) return false;
            default_.i = i;
            if (!keep) value_.i = i;
            break;
        }
        case CVarType::Float: {
            f32 f;
            if (!parse_float(text, f)) return false;
            default_.f = f;
            if (!keep) value_.f = f;
            break;
        }
        case CVarType::String:
            if (text.size() > default_str_.capacity() || text.size() > str_.capacity()) return false;
            default_str_.assign(text);
            if (!keep) str_.assign(text);
            break;
    }
    if (!keep) changed();
    return true;
}

ConVar* ConVar::find(StringId id) {
    for (ConVar* v = g_cvar_head; v; v = v->next_)
        if (v->id_ == id) return v;
    return nullptr;
}

void ConVar::collect(Array<ConVar*>& out) {
    out.clear();
    for (ConVar* v = g_cvar_head; v; v = v->next_) out.push_back(v);
    // Insertion sort by name: tiny N, no <algorithm> dependency.
    for (usize i = 1; i < out.size(); ++i) {
        ConVar* key = out[i];
        usize j = i;
        while (j > 0 && StringView(out[j - 1]->name_) > StringView(key->name_)) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = key;
    }
    drop_shadowed(out);
}

// ------------------------------------------------------------------ ConFunc

ConFunc::ConFunc(const char* name, Fn fn, const char* help) : name_(name), help_(help), id_(name), fn_(fn) {
    next_ = g_cfunc_head;  // a taken name is shadowed, as ConVar::link

    g_cfunc_head = this;
}

ConFunc::~ConFunc() { unlink(); }

void ConFunc::unlink() {
    for (ConFunc** p = &g_cfunc_head; *p; p = &(*p)->next_) {
        if (*p == this) {
            *p = next_;
            break;
        }
    }
    next_ = nullptr;
}

ConFunc* ConFunc::find(StringId id) {
    for (ConFunc* f = g_cfunc_head; f; f = f->next_)
        if (f->id_ == id) return f;
    return nullptr;
}

void ConFunc::collect(Array<ConFunc*>& out) {
    out.clear();
    for (ConFunc* f = g_cfunc_head; f; f = f->next_) out.push_back(f);
    for (usize i = 1; i < out.size(); ++i) {
        ConFunc* key = out[i];
        usize j = i;
        while (j > 0 && StringView(out[j - 1]->name_) > StringView(key->name_)) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = key;
    }
    drop_shadowed(out);
}

// ------------------------------------------------------------------ console

namespace console {
namespace {
std::atomic<bool> g_cheats_allowed{true};
MCE_CVAR(cv_help, "help", false, "Print the command line reference (every console variable) and exit", CVarFlags::None);

const char* type_name(CVarType t) {
    switch (t) {
        case CVarType::Bool: return "bool";
        case CVarType::Int: return "int";
        case CVarType::Float: return "float";
        case CVarType::String: return "string";
    }
    return "?";
}

bool cheat_locked(const ConVar& v, const char* where) {
    if (!(v.flags() & CVarFlags::Cheat) || g_cheats_allowed.load()) return false;
    MCE_LOG_WARN("{}: '{}' is a cheat and cheats are locked in this build", where, v.name());
    return true;
}
}  // namespace

void set_cheats_allowed(bool allowed) {
    g_cheats_allowed.store(allowed);
    if (allowed) return;
    Array<ConVar*> vars;
    ConVar::collect(vars);
    for (ConVar* v : vars)
        if (v->flags() & CVarFlags::Cheat) v->reset();
}

bool cheats_allowed() { return g_cheats_allowed.load(); }

bool execute(StringView line) {
    constexpr usize kMaxTokens = 16;
    StringView tokens[kMaxTokens];
    usize n = 0;
    usize i = 0;
    while (i < line.size() && n < kMaxTokens) {
        while (i < line.size() && is_space(line[i])) ++i;
        if (i >= line.size()) break;
        usize start = i;
        if (line[i] == '"') {
            start = ++i;
            while (i < line.size() && line[i] != '"') ++i;
            tokens[n++] = line.substr(start, i - start);
            if (i < line.size()) ++i;
        } else {
            while (i < line.size() && !is_space(line[i])) ++i;
            tokens[n++] = line.substr(start, i - start);
        }
    }
    if (n == 0) return true;

    const StringId id(tokens[0]);
    if (ConVar* v = ConVar::find(id)) {
        if (n == 1) {
            FixedString<128> s;
            v->to_string(s);
            MCE_LOG_INFO("{} = {}  ({})", v->name(), s.c_str(), v->help());
            return true;
        }
        if (cheat_locked(*v, "console")) return false;
        if (!v->set_from_string(tokens[1])) {
            MCE_LOG_WARN("console: cannot set '{}' to '{}'", v->name(), tokens[1]);
            return false;
        }
        return true;
    }
    if (ConFunc* f = ConFunc::find(id)) {
        f->call(Span<const StringView>(tokens + 1, n - 1));
        return true;
    }
    MCE_LOG_WARN("console: unknown command '{}'", tokens[0]);
    return false;
}

bool help_requested() { return cv_help.get_bool(); }

void print_help(const char* app_name, const char* usage, std::FILE* out) {
    if (!out) out = stdout;
    if (usage && *usage) {
        std::fputs(usage, out);
        if (usage[std::strlen(usage) - 1] != '\n') std::fputc('\n', out);
    } else {
        std::fprintf(out, "usage: %s [-name value ...] [+func args ...]\n", app_name ? app_name : "app");
    }
    std::fputs("\nEvery option is a console variable: -name value sets it, -flag alone sets a bool to true,\n"
               "+func args calls a console function. Values: true/false, integers, floats, strings.\n\nOptions:\n",
               out);
    Array<ConVar*> vars;
    ConVar::collect(vars);
    for (const ConVar* v : vars) {
        FixedString<128> value;
        v->to_string(value);
        FixedString<160> head("  -");
        head.append(v->name());
        head.append(" <");
        head.append(type_name(v->type()));
        head.append(">");
        std::fprintf(out, "%-34s %s", head.c_str(), v->help() ? v->help() : "");
        if (v->type() == CVarType::String) std::fprintf(out, " [default: \"%s\"]", value.c_str());
        else std::fprintf(out, " [default: %s]", value.c_str());
        if (v->flags() & CVarFlags::Cheat) std::fputs(" (cheat)", out);
        if (v->flags() & CVarFlags::ReadOnly) std::fputs(" (read-only)", out);
        if (v->flags() & CVarFlags::RequiresRestart) std::fputs(" (restart)", out);
        std::fputc('\n', out);
    }
    Array<ConFunc*> funcs;
    ConFunc::collect(funcs);
    if (!funcs.empty()) std::fputs("\nConsole functions (+name args):\n", out);
    for (const ConFunc* f : funcs) {
        FixedString<160> head("  +");
        head.append(f->name());
        std::fprintf(out, "%-34s %s\n", head.c_str(), f->help() ? f->help() : "");
    }
    std::fflush(out);
}

namespace {
struct DeferredArg {
    const char* tok;
    const char* value;  // null: a bare flag
};
// Fixed storage: convar.cpp runs before any allocator policy and keeps no heap state.
constexpr u32 kMaxDeferredArgs = 64;
DeferredArg g_deferred[kMaxDeferredArgs];
u32 g_deferred_count = 0;

// One `-name [value]` entry for a known convar. False when it is refused or the value is bad.
bool apply_arg(ConVar& v, const char* value) {
    if (cheat_locked(v, "command line")) return false;
    if (value) {
        if (!v.set_from_string(value, /*force=*/true)) {
            MCE_LOG_WARN("command line: bad value '{}' for '{}'", value, v.name());
            return false;
        }
        v.mark_command_line();
        return true;
    }
    if (!(v.type() == CVarType::Bool && v.set_bool(true, /*force=*/true))) return false;
    v.mark_command_line();
    return true;
}

StringView arg_name(StringView tok) {
    StringView name = tok.substr(tok[1] == '-' ? 2 : 1);
    if (name == "h" || name == "?") name = "help";  // the usual spellings of -help
    return name;
}

// A value, not the next -name / +command: a token that starts with neither, or a negative
// number ("-3", "-0.5,4,10": a free camera left of the origin).
bool is_value(const char* a) {
    if (a[0] == '+') return false;
    if (a[0] != '-') return true;
    const char c = a[1] == '.' ? a[2] : a[1];
    return c >= '0' && c <= '9';
}
}  // namespace

u32 parse_command_line(int argc, const char* const* argv, bool defer_unknown) {
    u32 unknown = 0;
    int i = 1;
    while (i < argc) {
        const StringView tok(argv[i]);
        if (tok.size() > 1 && tok[0] == '-') {
            ConVar* v = ConVar::find(StringId(arg_name(tok)));
            const bool has_value = i + 1 < argc && is_value(argv[i + 1]);
            if (!v) {
                if (defer_unknown && g_deferred_count < kMaxDeferredArgs) {
                    g_deferred[g_deferred_count++] = {argv[i], has_value ? argv[i + 1] : nullptr};
                } else {
                    ++unknown;
                    MCE_LOG_WARN("command line: unknown convar '{}'", tok);
                }
                i += has_value ? 2 : 1;
                continue;
            }
            if (!apply_arg(*v, has_value ? argv[i + 1] : nullptr)) ++unknown;
            i += has_value ? 2 : 1;
        } else if (tok.size() > 1 && tok[0] == '+') {
            constexpr usize kMaxArgs = 15;
            StringView args[kMaxArgs];
            usize n = 0;
            int j = i + 1;
            while (j < argc && is_value(argv[j])) {
                if (n < kMaxArgs) args[n++] = argv[j];
                ++j;
            }
            if (ConFunc* f = ConFunc::find(StringId(tok.substr(1)))) {
                f->call(Span<const StringView>(args, n));
            } else {
                ++unknown;
                MCE_LOG_WARN("command line: unknown confunc '{}'", tok);
            }
            i = j;
        } else {
            ++unknown;
            MCE_LOG_WARN("command line: stray argument '{}'", tok);
            ++i;
        }
    }
    return unknown;
}

u32 apply_deferred_command_line() {
    u32 unknown = 0;
    for (u32 i = 0; i < g_deferred_count; ++i) {
        const DeferredArg& a = g_deferred[i];
        const StringView tok(a.tok);
        if (ConVar* v = ConVar::find(StringId(arg_name(tok)))) {
            if (!apply_arg(*v, a.value)) ++unknown;
        } else {
            ++unknown;
            MCE_LOG_WARN("command line: unknown convar '{}'", tok);
        }
    }
    g_deferred_count = 0;
    return unknown;
}

// ------------------------------------------------------------------ settings file

namespace {
constexpr const char* kStageSettings = "console.settings";

struct SettingLine {
    FixedString<64> name;
    FixedString<1024> value;
};
Array<SettingLine> g_setting_lines;  // the file's lines, rewritten by save_settings
struct SettingStart {
    StringId id;
    FixedString<128> value;  // to_string() as load_settings left it
};
Array<SettingStart> g_setting_start;

SettingLine* find_line(StringView name) {
    for (SettingLine& l : g_setting_lines)
        if (l.name.view() == name) return &l;
    return nullptr;
}

}  // namespace

Result<void> set_defaults(StringView path) {
    const FixedString<512> p(path);
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return ok();
    char line[1200];
    while (std::fgets(line, sizeof(line), f)) {
        StringView t(line);
        while (!t.empty() && is_space(t[t.size() - 1])) t = t.substr(0, t.size() - 1);
        usize at = 0;  // a hand-written file: indentation and aligned columns are fine
        while (at < t.size() && is_space(t[at])) ++at;
        if (at == t.size() || t[at] == '#') continue;
        usize end = at;
        while (end < t.size() && !is_space(t[end])) ++end;
        const StringView name = t.substr(at, end - at);
        usize vs = end;
        while (vs < t.size() && is_space(t[vs])) ++vs;
        const StringView value = t.substr(vs);
        ConVar* v = ConVar::find(StringId(name));
        if (!v) MCE_LOG_WARN("defaults: unknown convar '{}'", name);
        else if (cheat_locked(*v, "defaults")) continue;
        else if (v->flags() & CVarFlags::ReadOnly) MCE_LOG_WARN("defaults: '{}' is read-only", name);
        else if (!v->set_default(value)) MCE_LOG_WARN("defaults: '{}' rejects '{}'", name, value);
    }
    std::fclose(f);
    return ok();
}

Result<void> load_settings(StringView path) {
    const FixedString<512> p(path);
    if (std::FILE* f = std::fopen(p.c_str(), "rb")) {
        g_setting_lines.clear();
        char line[1200];
        while (std::fgets(line, sizeof(line), f)) {
            StringView t(line);
            while (!t.empty() && is_space(t[t.size() - 1])) t = t.substr(0, t.size() - 1);
            usize sp = 0;
            while (sp < t.size() && !is_space(t[sp])) ++sp;
            if (sp == 0 || t[0] == '#') continue;
            StringView value = sp < t.size() ? t.substr(sp + 1) : StringView();
            SettingLine l;
            l.name.assign(t.substr(0, sp));
            l.value.assign(value);
            if (SettingLine* old = find_line(l.name.view())) *old = l;
            else g_setting_lines.push_back(l);
        }
        std::fclose(f);
    }
    for (const SettingLine& l : g_setting_lines) {
        ConVar* v = ConVar::find(StringId(l.name.view()));
        if (!v || !(v->flags() & CVarFlags::Saved) || v->from_command_line() || !v->is_default()) continue;
        if (!v->set_from_string(l.value.view())) MCE_LOG_WARN("settings: '{}' rejects '{}'", l.name.view(), l.value.view());
    }
    Array<ConVar*> all;
    ConVar::collect(all);
    for (ConVar* v : all) {
        if (!(v->flags() & CVarFlags::Saved)) continue;
        SettingStart* start = nullptr;
        for (SettingStart& s : g_setting_start)
            if (s.id == v->id()) start = &s;
        if (start) continue;  // a later load (after a module) keeps the first snapshot
        SettingStart s;
        s.id = v->id();
        v->to_string(s.value);
        g_setting_start.push_back(s);
    }
    return ok();
}

Result<void> save_settings(StringView path) {
    Array<ConVar*> all;
    ConVar::collect(all);
    for (ConVar* v : all) {
        if (!(v->flags() & CVarFlags::Saved)) continue;
        FixedString<128> now;
        v->to_string(now);
        const SettingStart* start = nullptr;
        for (const SettingStart& s : g_setting_start)
            if (s.id == v->id()) start = &s;
        if (start && start->value.view() == now.view()) continue;  // unchanged this run (or set on the command line)
        SettingLine l;
        l.name.assign(v->name());
        l.value.assign(v->type() == CVarType::String ? v->get_string() : now.view());
        if (SettingLine* old = find_line(l.name.view())) *old = l;
        else g_setting_lines.push_back(l);
    }
    const FixedString<512> p(path);
    FixedString<520> tmp(path);
    tmp.append(".tmp");
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return Error(Errc::AccessDenied, kStageSettings, path);
    bool wrote = true;
    for (const SettingLine& l : g_setting_lines)
        wrote = wrote && std::fprintf(f, "%s %.*s\n", l.name.c_str(), static_cast<int>(l.value.size()), l.value.c_str()) > 0;
    wrote = std::fflush(f) == 0 && wrote;
    std::fclose(f);
    if (!wrote) {
        std::remove(tmp.c_str());
        return Error(Errc::IoError, kStageSettings, path);
    }
    std::remove(p.c_str());  // rename does not overwrite everywhere
    if (std::rename(tmp.c_str(), p.c_str()) != 0) return Error(Errc::IoError, kStageSettings, path);
    web::persist();  // a browser keeps it in IndexedDB (no-op elsewhere)
    return ok();
}

u64 saved_generation() { return g_saved_generation; }

}  // namespace console
}  // namespace mce
