#include "mce/convar.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>

#include "mce/log.h"

namespace mce {
namespace {

// Zero-initialized before any dynamic initializer runs, so registrars may link in any order.
constinit ConVar* g_cvar_head = nullptr;
constinit ConFunc* g_cfunc_head = nullptr;

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

ConVar::~ConVar() {
    for (ConVar** p = &g_cvar_head; *p; p = &(*p)->next_) {
        if (*p == this) {
            *p = next_;
            break;
        }
    }
}

void ConVar::link() {
    MCE_ASSERT_MSG(find(id_) == nullptr, "duplicate convar name");
    next_ = g_cvar_head;
    g_cvar_head = this;
}

void ConVar::changed() {
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

void ConVar::reset() {
    value_ = default_;
    str_ = default_str_;
    changed();
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
}

// ------------------------------------------------------------------ ConFunc

ConFunc::ConFunc(const char* name, Fn fn, const char* help) : name_(name), help_(help), id_(name), fn_(fn) {
    MCE_ASSERT_MSG(find(id_) == nullptr, "duplicate confunc name");
    next_ = g_cfunc_head;
    g_cfunc_head = this;
}

ConFunc::~ConFunc() {
    for (ConFunc** p = &g_cfunc_head; *p; p = &(*p)->next_) {
        if (*p == this) {
            *p = next_;
            break;
        }
    }
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
}

// ------------------------------------------------------------------ console

namespace console {

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

u32 parse_command_line(int argc, const char* const* argv) {
    u32 unknown = 0;
    int i = 1;
    while (i < argc) {
        const StringView tok(argv[i]);
        if (tok.size() > 1 && tok[0] == '-') {
            ConVar* v = ConVar::find(StringId(tok.substr(tok[1] == '-' ? 2 : 1)));
            const bool has_value = i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1][0] != '+';
            if (!v) {
                ++unknown;
                MCE_LOG_WARN("command line: unknown convar '{}'", tok);
                i += has_value ? 2 : 1;
                continue;
            }
            if (has_value) {
                if (!v->set_from_string(argv[i + 1], /*force=*/true)) {
                    MCE_LOG_WARN("command line: bad value '{}' for '{}'", argv[i + 1], v->name());
                    ++unknown;
                }
                i += 2;
            } else {
                if (v->type() != CVarType::Bool || !v->set_bool(true, /*force=*/true)) ++unknown;
                i += 1;
            }
        } else if (tok.size() > 1 && tok[0] == '+') {
            constexpr usize kMaxArgs = 15;
            StringView args[kMaxArgs];
            usize n = 0;
            int j = i + 1;
            while (j < argc && argv[j][0] != '-' && argv[j][0] != '+') {
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

}  // namespace console
}  // namespace mce
