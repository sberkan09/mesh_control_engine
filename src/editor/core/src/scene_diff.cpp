#include "mce/editor/scene_diff.h"

#include <cstdio>

namespace mce::editor {

namespace {

bool names_differ(const FixedString<64>& x, const FixedString<64>& y) { return x.view() != y.view(); }

void diff_components(const DocEntity& a, const DocEntity& b, Array<ComponentChange>& out) {
    for (const DocComponent& cb : b.components) {
        const DocComponent* ca = a.find(*cb.type);
        if (ca == nullptr) {
            out.push_back({cb.type, ComponentOp::Added, 0});
            continue;
        }
        const u64 fields = changed_fields_mask(*cb.type, ca->data(), cb.data()) | (ca->overrides ^ cb.overrides);
        if (fields != 0) out.push_back({cb.type, ComponentOp::Changed, fields});
    }
    for (const DocComponent& ca : a.components)
        if (b.find(*ca.type) == nullptr) out.push_back({ca.type, ComponentOp::Removed, 0});
}

void append_number(cook::String& out, u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(v));
    out.append(StringView(buf));
}

void append_name_and_id(cook::String& out, const EntityDiff& e) {
    out.append(e.name.view());
    out.append(StringView(" (id "));
    append_number(out, e.id);
    out.append(')');
}

}  // namespace

SceneDiff diff_scenes(const SceneDocument& a, const SceneDocument& b) {
    SceneDiff diff;
    diff.header_changed = !a.same_header(b);

    Array<EntityId> order_a, order_b;  // the ids both documents have, in each one's order
    for (const DocEntity& eb : b.entities()) {
        const DocEntity* ea = a.find(eb.id);
        EntityDiff d;
        d.id = eb.id;
        d.name = eb.name;
        if (ea == nullptr) {
            d.kind = DiffKind::Added;
            diff.entities.push_back(d);
            continue;
        }
        order_b.push_back(eb.id);
        d.kind = DiffKind::Changed;
        if (names_differ(ea->name, eb.name)) d.old_name = ea->name;
        d.prefab_changed = !(ea->prefab == eb.prefab);
        diff_components(*ea, eb, d.components);
        if (!d.old_name.view().empty() || d.prefab_changed || !d.components.empty()) diff.entities.push_back(d);
    }
    for (const DocEntity& ea : a.entities()) {
        if (b.find(ea.id) != nullptr) {
            order_a.push_back(ea.id);
            continue;
        }
        EntityDiff d;
        d.id = ea.id;
        d.kind = DiffKind::Removed;
        d.name = ea.name;
        diff.entities.push_back(d);
    }
    for (usize i = 0; i < order_a.size() && !diff.reordered; ++i) diff.reordered = order_a[i] != order_b[i];
    return diff;
}

cook::String format_diff(const SceneDiff& diff) {
    cook::String out;
    auto line = [&out]() {
        if (!out.empty()) out.append('\n');
    };
    if (diff.header_changed) {
        line();
        out.append(StringView("~ scene header (guid, name or settings)"));
    }
    if (diff.reordered) {
        line();
        out.append(StringView("~ entity order"));
    }
    for (const EntityDiff& e : diff.entities) {
        line();
        out.append(e.kind == DiffKind::Added ? '+' : e.kind == DiffKind::Removed ? '-' : '~');
        out.append(' ');
        append_name_and_id(out, e);
        if (e.kind != DiffKind::Changed) continue;
        bool first = true;
        auto sep = [&]() {
            out.append(first ? StringView(": ") : StringView("; "));
            first = false;
        };
        if (!e.old_name.view().empty()) {
            sep();
            out.append(StringView("renamed from \""));
            out.append(e.old_name.view());
            out.append('"');
        }
        if (e.prefab_changed) {
            sep();
            out.append(StringView("prefab link"));
        }
        for (const ComponentChange& c : e.components) {
            sep();
            if (c.op == ComponentOp::Added) out.append('+');
            if (c.op == ComponentOp::Removed) out.append('-');
            if (c.op != ComponentOp::Changed) {
                out.append(StringView(c.type->name));
                continue;
            }
            bool first_field = true;
            for (usize i = 0; i < c.type->fields.size() && i < 64; ++i) {
                if (((c.fields >> i) & 1u) == 0) continue;
                if (!first_field) out.append(StringView(", "));
                first_field = false;
                out.append(StringView(c.type->name));
                out.append('.');
                out.append(StringView(c.type->fields[i].name_str));
            }
        }
    }
    return out;
}

}  // namespace mce::editor
