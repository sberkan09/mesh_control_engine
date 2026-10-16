// mce_script_compiler (M-SCRIPT 4): see names_check.h.

#include "names_check.h"

#include <cstdio>
#include <cstring>

#include "Luau/Ast.h"
#include "mce/refl/type.h"

namespace mce::script::detail {

namespace {

// Base-library names the sandbox removes (script.cpp) and what to use instead.
struct Removed {
    const char* name;
    const char* instead;
};
constexpr Removed kRemoved[] = {
    {"print", "mce.log"},
    {"collectgarbage", "nothing: the VM's memory is the engine's"},
    {"gcinfo", "nothing: the VM's memory is the engine's"},
    {"getfenv", "plain globals"},
    {"setfenv", "plain globals"},
    {"os", "mce.tick() and mce.dt for time"},
    {"io", "components: scripts do not touch files"},
    {"debug", "mce.log"},
    {"coroutine", "components: scripts keep no state between ticks"},
    {"require", "one script per file (shared code arrives with modules later)"},
};

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Edit distance, case-insensitive, capped: names are short.
u32 distance(const char* a, const char* b) {
    const usize n = std::strlen(a);
    const usize m = std::strlen(b);
    if (n > 63 || m > 63) return 99;
    u32 row[64];
    for (usize j = 0; j <= m; ++j) row[j] = static_cast<u32>(j);
    for (usize i = 1; i <= n; ++i) {
        u32 diag = row[0];
        row[0] = static_cast<u32>(i);
        for (usize j = 1; j <= m; ++j) {
            const u32 up = row[j];
            const u32 cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0u : 1u;
            u32 best = diag + cost;
            if (up + 1 < best) best = up + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            row[j] = best;
            diag = up;
        }
    }
    return row[m];
}

// " (did you mean 'X'?)" for the closest candidate within reach, else "".
template <class Candidates>
void suggest(const char* name, const Candidates& each, char* out, usize cap) {
    const char* best = nullptr;
    u32 best_d = 99;
    each([&](const char* c) {
        const u32 d = distance(name, c);
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    });
    const usize len = std::strlen(name);
    const u32 reach = len <= 4 ? 1u : len <= 8 ? 2u : 3u;
    if (best && best_d <= reach) std::snprintf(out, cap, " (did you mean '%s'?)", best);
    else if (cap) out[0] = 0;
}

// A string literal's text (Luau's AstArray<char> is not NUL-terminated), or null.
struct Literal {
    FixedString<128> text;
    const char* c_str() const { return text.c_str(); }
};
const char* string_of(const Luau::AstExpr* e, Literal& out) {
    const auto* s = e ? e->as<Luau::AstExprConstantString>() : nullptr;
    if (!s) return nullptr;
    out.text.assign(StringView(s->value.data, s->value.size));
    return out.c_str();
}
bool is_string(const Luau::AstExpr* e) { return e && e->is<Luau::AstExprConstantString>(); }

class Checker : public Luau::AstVisitor {
public:
    explicit Checker(Array<NameIssue>& out) : out_(out) {}

    bool visit(Luau::AstExprGlobal* node) override {
        for (const Removed& r : kRemoved)
            if (std::strcmp(node->name.value, r.name) == 0) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "'%s' is not available in scripts; use %s", r.name, r.instead);
                add(node->location, msg);
            }
        return true;
    }

    bool visit(Luau::AstExprIndexName* node) override {
        const auto* g = node->expr->as<Luau::AstExprGlobal>();
        if (g && std::strcmp(g->name.value, "math") == 0 &&
            (std::strcmp(node->index.value, "random") == 0 || std::strcmp(node->index.value, "randomseed") == 0))
            add(node->location, "'math.random' is not available in scripts; use mce.random(), which replays the same");
        return true;
    }

    bool visit(Luau::AstExprCall* node) override {
        const auto* idx = node->func->as<Luau::AstExprIndexName>();
        const auto* g = idx ? idx->expr->as<Luau::AstExprGlobal>() : nullptr;
        if (!g || std::strcmp(g->name.value, "mce") != 0) return true;
        const char* fn = idx->index.value;
        const auto& a = node->args;
        auto arg = [&](usize i) -> Luau::AstExpr* { return i < a.size ? a.data[i] : nullptr; };
        if (std::strcmp(fn, "field") == 0) {
            field_pair(arg(0), arg(1));
        } else if (std::strcmp(fn, "get") == 0) {
            for (usize i = 1; i < a.size;) {  // handles and ("Comp", "field") pairs, in any mix
                if (is_string(a.data[i])) {
                    field_pair(a.data[i], arg(i + 1));
                    i += 2;
                } else {
                    ++i;
                }
            }
        } else if (std::strcmp(fn, "set") == 0) {
            if (is_string(arg(1))) field_pair(arg(1), arg(2));
        } else if (std::strcmp(fn, "each") == 0) {
            for (usize i = 0; i + 1 < a.size; ++i) component(a.data[i]);
        } else if (std::strcmp(fn, "has") == 0 || std::strcmp(fn, "remove") == 0) {
            component(arg(1));
        } else if (std::strcmp(fn, "add") == 0) {
            if (const refl::TypeRecord* t = component(arg(1))) fields_of(*t, arg(2));
        } else if (std::strcmp(fn, "spawn") == 0) {
            if (const auto* tbl = arg(0) ? arg(0)->as<Luau::AstExprTable>() : nullptr)
                for (const Luau::AstExprTable::Item& item : tbl->items)
                    if (item.kind == Luau::AstExprTable::Item::Kind::Record)
                        if (const refl::TypeRecord* t = component(item.key)) fields_of(*t, item.value);
        } else if (std::strcmp(fn, "events") == 0) {
            event(arg(0));
        } else if (std::strcmp(fn, "emit") == 0) {
            if (const refl::TypeRecord* t = event(arg(0))) fields_of(*t, arg(1));
        }
        return true;
    }

private:
    void add(const Luau::Location& at, const char* msg) {
        NameIssue i;
        i.line = at.begin.line + 1;
        i.col = at.begin.column + 1;
        i.message.assign(StringView(msg));
        out_.push_back(i);
    }

    const refl::TypeRecord* named(Luau::AstExpr* e, u32 flag, const char* what) {
        Literal lit;
        const char* name = string_of(e, lit);
        if (!name) return nullptr;  // not a literal: the run-time check reports it
        const refl::TypeRecord* t = refl::find_type(StringId(name));
        if (t && (t->flags & flag) != 0) return t;
        char hint[96];
        suggest(
            name,
            [&](auto&& fn) {
                for (const refl::TypeRecord* c : refl::all())
                    if (c && (c->flags & flag) != 0) fn(c->name);
            },
            hint, sizeof(hint));
        char msg[256];
        std::snprintf(msg, sizeof(msg), "unknown %s '%s'%s", what, name, hint);
        add(e->location, msg);
        return nullptr;
    }

    const refl::TypeRecord* component(Luau::AstExpr* e) { return named(e, refl::TypeFlags::Component, "component"); }
    const refl::TypeRecord* event(Luau::AstExpr* e) { return named(e, refl::TypeFlags::Event, "event"); }

    void field(const refl::TypeRecord& t, Luau::AstExpr* e, const char* name) {
        if (t.find_field(StringId(name))) return;
        char hint[96];
        suggest(
            name,
            [&](auto&& fn) {
                for (const refl::FieldRecord& f : t.fields) fn(f.name_str);
            },
            hint, sizeof(hint));
        char msg[256];
        std::snprintf(msg, sizeof(msg), "'%s' has no field '%s'%s", t.name, name, hint);
        add(e->location, msg);
    }

    void field_pair(Luau::AstExpr* comp, Luau::AstExpr* f) {
        const refl::TypeRecord* t = component(comp);
        Literal lit;
        const char* name = t ? string_of(f, lit) : nullptr;
        if (name) field(*t, f, name);
    }

    // A table literal's named keys against the type's fields.
    void fields_of(const refl::TypeRecord& t, Luau::AstExpr* e) {
        const auto* tbl = e ? e->as<Luau::AstExprTable>() : nullptr;
        if (!tbl) return;
        for (const Luau::AstExprTable::Item& item : tbl->items)
            if (item.kind == Luau::AstExprTable::Item::Kind::Record)
                if (Literal lit; const char* name = string_of(item.key, lit)) field(t, item.key, name);
    }

    Array<NameIssue>& out_;
};

}  // namespace

void check_names(Luau::AstStatBlock* root, Array<NameIssue>& out) {
    Checker c(out);
    root->visit(&c);
}

}  // namespace mce::script::detail
