#pragma once
// Console variables and console functions (core.md §2.4, §3.6) — the single configuration
// surface of the engine. Registered by static registrar objects into an intrusive list (no heap,
// no init-order games). Main thread only.
//
//   MCE_CVAR(cv_time_scale, "time_scale", 1.0f, "Sim speed multiplier", mce::CVarFlags::Cheat);
//   MCE_CONFUNC(cf_quit, "quit", my_quit_fn, "Quit the application");
//
// Command line: "-name value" sets a convar ("-flag" alone sets a bool to true);
// "+func a b" calls a confunc with the following non-flag tokens. "-help" (also --help, -h, -?)
// asks for the listing every app prints with console::print_help before it exits.

#include <cstdio>

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/string_id.h"

namespace mce {

namespace CVarFlags {
enum : u32 {
    None = 0,
    Cheat = 1u << 0,
    Saved = 1u << 1,
    ReadOnly = 1u << 2,
    RequiresRestart = 1u << 3,
};
}

enum class CVarType : u8 { Bool, Int, Float, String };

class ConVar {
public:
    using ChangeFn = void (*)(ConVar& var);

    ConVar(const char* name, bool def, const char* help, u32 flags = CVarFlags::None);
    ConVar(const char* name, i32 def, const char* help, u32 flags = CVarFlags::None);
    ConVar(const char* name, f32 def, const char* help, u32 flags = CVarFlags::None);
    ConVar(const char* name, const char* def, const char* help, u32 flags = CVarFlags::None);
    ~ConVar();
    ConVar(const ConVar&) = delete;
    ConVar& operator=(const ConVar&) = delete;

    bool get_bool() const;
    i32 get_int() const;
    f32 get_float() const;
    StringView get_string() const { return str_.view(); }

    // Typed setters convert across numeric types; they respect ReadOnly unless force is set.
    bool set_bool(bool v, bool force = false);
    bool set_int(i32 v, bool force = false);
    bool set_float(f32 v, bool force = false);
    bool set_string(StringView v, bool force = false);
    // Parses according to type(). Returns false on parse failure or ReadOnly.
    bool set_from_string(StringView text, bool force = false);
    void to_string(FixedString<128>& out) const;
    void reset();

    const char* name() const { return name_; }
    const char* help() const { return help_; }
    StringId id() const { return id_; }
    u32 flags() const { return flags_; }
    CVarType type() const { return type_; }
    void set_on_change(ChangeFn fn) { on_change_ = fn; }

    static ConVar* find(StringId id);
    // All registered convars sorted by name (console listing, settings save).
    static void collect(Array<ConVar*>& out);

private:
    void link();
    void changed();

    const char* name_;
    const char* help_;
    StringId id_;
    u32 flags_;
    CVarType type_;
    union {
        bool b;
        i32 i;
        f32 f;
    } value_{}, default_{};
    FixedString<1024> str_;  // room for a file path (-module, -project); longer values are refused
    FixedString<128> default_str_;
    ChangeFn on_change_ = nullptr;
    ConVar* next_ = nullptr;
};

class ConFunc {
public:
    using Fn = void (*)(Span<const StringView> args);

    ConFunc(const char* name, Fn fn, const char* help);
    ~ConFunc();
    ConFunc(const ConFunc&) = delete;
    ConFunc& operator=(const ConFunc&) = delete;

    void call(Span<const StringView> args) const { fn_(args); }
    const char* name() const { return name_; }
    const char* help() const { return help_; }
    StringId id() const { return id_; }

    static ConFunc* find(StringId id);
    static void collect(Array<ConFunc*>& out);

private:
    const char* name_;
    const char* help_;
    StringId id_;
    Fn fn_;
    ConFunc* next_ = nullptr;
};

namespace console {
// Executes one console line: "name" prints, "name value" sets a convar, "func args" calls.
// Returns false when the name is unknown or the value fails to parse.
bool execute(StringView line);
// Applies argv per the command-line rules above. Returns the number of unknown entries.
// With `defer_unknown`, unknown `-name [value]` entries are kept quietly instead of warned about,
// for apply_deferred_command_line() once later code (a game module's MCE_CVARs) has registered.
u32 parse_command_line(int argc, const char* const* argv, bool defer_unknown = false);
// Applies the entries a defer_unknown parse kept, warning about the ones still unknown, and
// forgets them. Returns the number still unknown or rejected. argv must still be alive.
u32 apply_deferred_command_line();
// True after parse_command_line saw -help (the `help` convar): the app prints print_help and
// exits 0 instead of running.
bool help_requested();
// The command-line reference: `usage` (one or more lines naming the app's own flags; may be null)
// followed by every registered convar (name, type, default, help, flags) and every confunc, sorted
// by name, to `out` (stdout when null). Convars are the whole command line, so this is complete.
void print_help(const char* app_name, const char* usage, std::FILE* out = nullptr);
// The cheat lock (shipped builds): while cheats are not allowed, execute() and the command line
// refuse to set a Cheat convar, and locking resets every Cheat convar to its default. Code may
// still set them (a game's own slow-motion). Allowed by default (development builds).
void set_cheats_allowed(bool allowed);
bool cheats_allowed();
}  // namespace console

}  // namespace mce

#define MCE_CVAR(ident, name, def, help, flags) ::mce::ConVar ident(name, def, help, flags)
#define MCE_CONFUNC(ident, name, fn, help) ::mce::ConFunc ident(name, fn, help)
