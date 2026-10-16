// mce_script_compiler (M-SCRIPT 3): see state_check.h.

#include "state_check.h"

#include <cstdio>
#include <cstring>

#include "Luau/Ast.h"

namespace mce::script::detail {

namespace {

// Calls that change their first argument (and table.move its fifth).
bool mutates_argument(const Luau::AstExprCall* call) {
    const Luau::AstExpr* fn = call->func;
    if (const auto* g = fn->as<Luau::AstExprGlobal>())
        return std::strcmp(g->name.value, "rawset") == 0 || std::strcmp(g->name.value, "setmetatable") == 0;
    const auto* idx = fn->as<Luau::AstExprIndexName>();
    if (!idx) return false;
    const auto* lib = idx->expr->as<Luau::AstExprGlobal>();
    if (!lib) return false;
    const char* name = idx->index.value;
    if (std::strcmp(lib->name.value, "table") == 0) {
        for (const char* m : {"insert", "remove", "clear", "sort", "move"})
            if (std::strcmp(name, m) == 0) return true;
        return false;
    }
    if (std::strcmp(lib->name.value, "buffer") == 0)
        return std::strncmp(name, "write", 5) == 0 || std::strcmp(name, "fill") == 0 || std::strcmp(name, "copy") == 0;
    return false;
}

class Checker : public Luau::AstVisitor {
public:
    explicit Checker(StateIssue& issue) : issue_(issue) {}
    bool failed() const { return failed_; }

    bool visit(Luau::AstExprFunction* node) override {
        ++depth_;
        node->body->visit(this);
        --depth_;
        return false;
    }

    bool visit(Luau::AstStatLocal* node) override {
        for (usize i = 0; i < node->vars.size && i < node->values.size; ++i) note_object_local(node->vars.data[i], node->values.data[i]);
        return true;
    }

    bool visit(Luau::AstStatLocalFunction* node) override {
        if (object_count_ < kMaxObjectLocals) object_locals_[object_count_++] = node->name;
        return true;
    }

    bool visit(Luau::AstStatAssign* node) override {
        for (usize i = 0; i < node->vars.size; ++i) check_write(node->vars.data[i]);
        return true;
    }

    bool visit(Luau::AstStatCompoundAssign* node) override {
        check_write(node->var);
        return true;
    }

    bool visit(Luau::AstStatFunction* node) override {
        if (!node->name->is<Luau::AstExprGlobal>()) check_write(node->name);
        return true;
    }

    bool visit(Luau::AstExprCall* node) override {
        if (depth_ > 0 && mutates_argument(node)) {
            if (node->args.size > 0) check_root(node->args.data[0], node->location, "changes");
            if (node->args.size > 4) check_root(node->args.data[4], node->location, "changes");
        }
        return true;
    }

    bool visit(Luau::AstExprTable* node) override {
        for (const Luau::AstExprTable::Item& item : node->items)
            if (item.kind == Luau::AstExprTable::Item::Kind::General && item.key && object_like(item.key)) object_key(item.key->location);
        return true;
    }

private:
    static constexpr usize kMaxObjectLocals = 256;

    // A local bound to a table constructor or a function: its uses as a key are object keys.
    void note_object_local(Luau::AstLocal* local, Luau::AstExpr* value) {
        if (!value->is<Luau::AstExprTable>() && !value->is<Luau::AstExprFunction>()) return;
        if (object_count_ < kMaxObjectLocals) object_locals_[object_count_++] = local;
    }

    bool object_like(Luau::AstExpr* e) const {
        while (auto* g = e->as<Luau::AstExprGroup>()) e = g->expr;
        if (e->is<Luau::AstExprTable>() || e->is<Luau::AstExprFunction>()) return true;
        if (const auto* l = e->as<Luau::AstExprLocal>())
            for (usize i = 0; i < object_count_; ++i)
                if (object_locals_[i] == l->local) return true;
        return false;
    }

    void check_write(Luau::AstExpr* target) {
        if (auto* ix = target->as<Luau::AstExprIndexExpr>(); ix && object_like(ix->index)) object_key(ix->index->location);
        if (depth_ == 0) return;  // the top level runs once, when the script loads
        check_root(target, target->location, nullptr);
    }

    // `e` is written (or changed through a call): fails when its root is a top-level local.
    void check_root(Luau::AstExpr* e, const Luau::Location& at, const char* verb) {
        bool through_table = verb != nullptr;
        for (;;) {
            if (auto* g = e->as<Luau::AstExprGroup>()) e = g->expr;
            else if (auto* n = e->as<Luau::AstExprIndexName>()) e = n->expr, through_table = true;
            else if (auto* x = e->as<Luau::AstExprIndexExpr>()) e = x->expr, through_table = true;
            else break;
        }
        const auto* l = e->as<Luau::AstExprLocal>();
        if (!l || l->local->functionDepth != 0) return;
        char msg[256];
        if (through_table)
            std::snprintf(msg, sizeof(msg),
                          "a function %s the table in top-level local '%s', which would keep state between ticks; keep it "
                          "in a component",
                          verb ? verb : "writes into", l->local->name.value);
        else
            std::snprintf(msg, sizeof(msg),
                          "a function assigns top-level local '%s', which would keep state between ticks; keep it in a "
                          "component",
                          l->local->name.value);
        fail(at, msg);
    }

    void object_key(const Luau::Location& at) {
        fail(at,
             "a table or function used as a table key: iteration order over such keys differs between runs; key by "
             "entity, number or string");
    }

    void fail(const Luau::Location& at, const char* msg) {
        if (failed_) return;
        failed_ = true;
        issue_.line = at.begin.line + 1;
        issue_.col = at.begin.column + 1;
        issue_.message.assign(StringView(msg));
    }

    StateIssue& issue_;
    bool failed_ = false;
    u32 depth_ = 0;
    Luau::AstLocal* object_locals_[kMaxObjectLocals] = {};
    usize object_count_ = 0;
};

}  // namespace

bool check_state(Luau::AstStatBlock* root, StateIssue& issue) {
    Checker c(issue);
    root->visit(&c);
    return !c.failed();
}

}  // namespace mce::script::detail
