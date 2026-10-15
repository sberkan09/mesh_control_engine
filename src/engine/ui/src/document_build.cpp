// DocumentData <-> ui::Document (M-UI-DATA step 1; mce/ui/document_data.h).

#include "mce/ui/document_data.h"
#include "mce/ui/ui.h"

namespace mce::ui {

namespace {

constexpr const char* kStage = "ui.document";

}  // namespace

Result<void> build_document(const DocumentData& d, Document& doc) {
    Array<WidgetId> ids;
    ids.reserve(d.widgets.size());
    for (u32 i = 0; i < d.widgets.size(); ++i) {
        const WidgetData& w = d.widgets[i];
        if (w.parent != kNoWidgetIndex && w.parent >= i) return Error(Errc::OutOfBounds, kStage, w.name.view(), i);
        const WidgetId parent = w.parent == kNoWidgetIndex ? doc.root() : ids[w.parent];
        const WidgetId id = doc.add(parent, w.kind, w.name.view());
        if (!doc.valid(id)) return Error(Errc::InvalidArgument, kStage, w.name.view(), i);
        ids.push_back(id);
        doc.set_anchors(id, w.anchors);
        for (u32 s = 0; s < kWidgetStateCount; ++s) doc.set_style(id, static_cast<WidgetState>(s), w.styles[s]);
        doc.set_text(id, w.text.view());
        doc.set_range(id, w.lo, w.hi, w.step);
        if (w.kind != WidgetKind::Tabs) doc.set_value(id, w.value);  // a tabs widget's value is its page, below
        doc.set_visible(id, w.visible);
        doc.set_interactable(id, w.interactable);
        doc.set_blocks_pointer(id, w.blocks_pointer);
        doc.set_stack(id, w.stack);
        doc.set_grid(id, w.grid);
    }
    // What refers to other widgets or reads their state, once they all exist.
    for (u32 i = 0; i < d.widgets.size(); ++i) {
        const WidgetData& w = d.widgets[i];
        NavEdges nav;
        WidgetId* edges[NavEdgeCount] = {&nav.up, &nav.down, &nav.left, &nav.right};
        for (u32 e = 0; e < NavEdgeCount; ++e) {
            if (w.nav[e] == kNoWidgetIndex) continue;
            if (w.nav[e] >= ids.size()) return Error(Errc::OutOfBounds, kStage, w.name.view(), i);
            *edges[e] = ids[w.nav[e]];
        }
        doc.set_nav(ids[i], nav);
        if (w.kind == WidgetKind::Tabs) doc.select_tab(ids[i], static_cast<u32>(w.value));
        if (!w.convar.empty()) doc.bind_convar(ids[i], w.convar.view());
    }
    doc.set_modal(d.modal);
    if (d.default_focus != kNoWidgetIndex) {
        if (d.default_focus >= ids.size()) return Error(Errc::OutOfBounds, kStage);
        doc.set_default_focus(ids[d.default_focus]);
    }
    return ok();
}

void extract_document(const Document& doc, DocumentData& out) {
    out.widgets.clear();
    out.modal = doc.modal();
    out.default_focus = kNoWidgetIndex;
    // Depth-first, children in sibling order: the order build_document adds them in.
    Array<WidgetId> ids;
    Array<WidgetId> stack;
    Array<u32> parents;
    auto push_children = [&](WidgetId w, u32 index) {
        const usize mark = stack.size();
        for (WidgetId c = doc.first_child(w); doc.valid(c); c = doc.next_sibling(c)) {
            stack.push_back(c);
            parents.push_back(index);
        }
        // Reverse the run just pushed so the first child pops first.
        for (usize a = mark, b = stack.size(); a + 1 < b; ++a, --b) {
            const WidgetId t = stack[a];
            stack[a] = stack[b - 1];
            stack[b - 1] = t;
            const u32 p = parents[a];
            parents[a] = parents[b - 1];
            parents[b - 1] = p;
        }
    };
    push_children(doc.root(), kNoWidgetIndex);
    while (!stack.empty()) {
        const WidgetId w = stack.back();
        const u32 parent = parents.back();
        stack.pop_back();
        parents.pop_back();
        const u32 index = static_cast<u32>(ids.size());
        ids.push_back(w);
        WidgetData d;
        d.name.assign(doc.name(w));
        d.kind = doc.kind(w);
        d.parent = parent;
        d.anchors = doc.anchors(w);
        for (u32 s = 0; s < kWidgetStateCount; ++s) d.styles[s] = doc.style(w, static_cast<WidgetState>(s));
        d.text.assign(doc.text(w));
        d.value = doc.value(w);
        doc.range(w, d.lo, d.hi, d.step);
        d.visible = doc.visible_self(w);
        d.interactable = doc.interactable(w);
        d.blocks_pointer = doc.blocks_pointer(w);
        d.stack = doc.stack(w);
        d.grid = doc.grid(w);
        d.convar.assign(doc.convar(w));
        out.widgets.push_back(d);
        push_children(w, index);
    }
    auto index_of = [&](WidgetId w) {
        if (!doc.valid(w)) return kNoWidgetIndex;
        for (u32 i = 0; i < ids.size(); ++i)
            if (ids[i] == w) return i;
        return kNoWidgetIndex;
    };
    for (u32 i = 0; i < ids.size(); ++i) {
        const NavEdges nav = doc.nav(ids[i]);
        const WidgetId edges[NavEdgeCount] = {nav.up, nav.down, nav.left, nav.right};
        for (u32 e = 0; e < NavEdgeCount; ++e) out.widgets[i].nav[e] = index_of(edges[e]);
    }
    out.default_focus = index_of(doc.default_focus());
}

}  // namespace mce::ui
