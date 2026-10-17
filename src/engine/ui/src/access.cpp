#include "mce/ui/access.h"

#include <cmath>
#include <cstdio>

namespace mce::ui {
namespace {

// The tab bar a tab's button sits in: the Tabs widget's first child.
bool is_tab_button(const Document& doc, WidgetId w, u32& index, WidgetId& tabs) {
    const WidgetId bar = doc.parent(w);
    tabs = bar.is_null() ? WidgetId{} : doc.parent(bar);
    if (tabs.is_null() || doc.kind(tabs) != WidgetKind::Tabs || doc.first_child(tabs) != bar) return false;
    index = 0;
    for (WidgetId c = doc.first_child(bar); !c.is_null() && c != w; c = doc.next_sibling(c)) ++index;
    return true;
}

AccessRole role_of(const Document& doc, WidgetId w) {
    switch (doc.kind(w)) {
        case WidgetKind::Text: return AccessRole::Text;
        case WidgetKind::Image:
        case WidgetKind::Minimap: return AccessRole::Image;
        case WidgetKind::Button: {
            u32 i = 0;
            WidgetId tabs;
            return is_tab_button(doc, w, i, tabs) ? AccessRole::Tab : AccessRole::Button;
        }
        case WidgetKind::Toggle: return AccessRole::Switch;
        case WidgetKind::Slider: return AccessRole::Slider;
        case WidgetKind::Scroll: return AccessRole::List;
        case WidgetKind::Console: return AccessRole::TextInput;
        case WidgetKind::Panel:
        case WidgetKind::Stack:
        case WidgetKind::Grid:
        case WidgetKind::Tabs: break;
    }
    return AccessRole::Group;
}

// A control with no text of its own is named by the Text just before it in its parent.
StringView label_before(const Document& doc, WidgetId w) {
    const WidgetId parent = doc.parent(w);
    if (parent.is_null()) return {};
    WidgetId prev;
    for (WidgetId c = doc.first_child(parent); !c.is_null() && c != w; c = doc.next_sibling(c)) prev = c;
    return !prev.is_null() && doc.kind(prev) == WidgetKind::Text ? doc.text(prev) : StringView();
}

// A row of controls led by a label ("Jump" | Space | Pad South | Toggle, the remap list): each
// control's own text is said after the row's label, so a reader hears which action a key belongs to.
StringView row_label(const Document& doc, WidgetId w) {
    const WidgetId parent = doc.parent(w);
    if (parent.is_null() || doc.kind(parent) != WidgetKind::Stack || doc.stack(parent).axis != Axis::Horizontal) return {};
    const WidgetId first = doc.first_child(parent);
    return !first.is_null() && first != w && doc.kind(first) == WidgetKind::Text ? doc.text(first) : StringView();
}

void value_of(const Document& doc, WidgetId w, AccessRole role, FixedString<32>& out) {
    out.clear();
    char buf[32];
    if (role == AccessRole::Switch) {
        out.assign(doc.value(w) >= 0.5f ? "on" : "off");
    } else if (role == AccessRole::Slider) {
        f32 lo = 0, hi = 1, step = 0;
        doc.range(w, lo, hi, step);
        const f32 v = doc.value(w);
        if (lo == 0.0f && hi == 1.0f) {
            std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(v * 100.0f)));
        } else if (step >= 1.0f && std::floor(step) == step) {
            std::snprintf(buf, sizeof(buf), "%ld", std::lround(v));
        } else {
            std::snprintf(buf, sizeof(buf), "%.3g", static_cast<double>(v));
        }
        out.assign(buf);
    } else if (role == AccessRole::Tab) {
        u32 i = 0;
        WidgetId tabs;
        if (is_tab_button(doc, w, i, tabs) && doc.selected_tab(tabs) == i) out.assign("selected");
    }
}

// Readers say names as written; menus written in capitals ("MUSIC") would be spelled out or shouted,
// so an all-caps name is read in sentence case.
void reader_case(FixedString<128>& name) {
    bool lower = false, letters = false;
    for (char c : name.view()) {
        lower = lower || (c >= 'a' && c <= 'z');
        letters = letters || (c >= 'A' && c <= 'Z');
    }
    if (lower || !letters) return;
    FixedString<128> out;
    bool start = true;
    for (char c : name.view()) {
        char o = c;
        if (c >= 'A' && c <= 'Z' && !start) o = static_cast<char>(c - 'A' + 'a');
        if (c != ' ') start = false;
        if (c == '.' || c == '!' || c == '?' || c == ':') start = true;
        out.append(StringView(&o, 1));
    }
    name = out;
}

}  // namespace

const char* access_role_name(AccessRole r) {
    switch (r) {
        case AccessRole::Group: return "group";
        case AccessRole::Text: return "text";
        case AccessRole::Image: return "image";
        case AccessRole::Button: return "button";
        case AccessRole::Switch: return "switch";
        case AccessRole::Slider: return "slider";
        case AccessRole::Tab: return "tab";
        case AccessRole::List: return "list";
        case AccessRole::TextInput: return "text field";
    }
    return "group";
}

AccessNode access_node(const Document& doc, WidgetId w) {
    AccessNode n;
    n.widget = w;
    if (!doc.valid(w)) return n;
    n.role = role_of(doc, w);
    StringView name = doc.text(w);
    if (name.empty() && (n.role == AccessRole::Slider || n.role == AccessRole::Switch || n.role == AccessRole::TextInput))
        name = label_before(doc, w);
    n.name.assign(name);
    reader_case(n.name);
    if (!name.empty() && n.role != AccessRole::Text && n.role != AccessRole::Group) {
        if (const StringView row = row_label(doc, w); !row.empty() && row != name) {
            FixedString<128> own(n.name.view());
            n.name.assign(row);
            reader_case(n.name);
            n.name.append(StringView(", "));
            n.name.append(own.view());
        }
    }
    value_of(doc, w, n.role, n.value);
    n.rect = doc.rect(w);
    n.focused = doc.focused() == w;
    n.enabled = doc.interactable(w);
    return n;
}

void access_line(const AccessNode& n, FixedString<192>& out) {
    out.clear();
    if (n.role == AccessRole::Group || (n.name.empty() && n.value.empty())) return;
    if (!n.name.empty()) {
        out.append(n.name.view());
        out.append(", ");
    }
    out.append(access_role_name(n.role));
    if (!n.value.empty()) {
        out.append(", ");
        out.append(n.value.view());
    }
    if (!n.enabled) out.append(", dimmed");
}

namespace {
// Depth-first in document order; a hidden widget hides its subtree.
void add_subtree(const Document& doc, WidgetId w, i32 parent, Array<AccessNode>& out) {
    if (!doc.visible_self(w)) return;
    AccessNode n = access_node(doc, w);
    n.parent = parent;
    const i32 index = static_cast<i32>(out.size());
    out.push_back(n);
    for (WidgetId c = doc.first_child(w); !c.is_null(); c = doc.next_sibling(c)) add_subtree(doc, c, index, out);
}
}  // namespace

void build_access_tree(const Document& doc, Array<AccessNode>& out) {
    out.clear();
    for (WidgetId c = doc.first_child(doc.root()); !c.is_null(); c = doc.next_sibling(c)) add_subtree(doc, c, -1, out);
}

bool AccessAnnouncer::next(const Document* doc, FixedString<192>& line) {
    line.clear();
    if (doc != doc_) {
        doc_ = doc;
        focus_ = WidgetId{};
        last_.clear();
    }
    if (!doc) return false;
    const WidgetId f = doc->focused();
    if (f.is_null() || !doc->valid(f)) {
        focus_ = WidgetId{};
        last_.clear();
        return false;
    }
    FixedString<192> now;
    access_line(access_node(*doc, f), now);
    if (f == focus_ && now.view() == last_.view()) return false;
    const bool moved = f != focus_;
    focus_ = f;
    // A value change on the same widget speaks only the value ("50%"), as readers do for sliders.
    if (!moved) {
        const AccessNode n = access_node(*doc, f);
        line.assign(n.value.empty() ? now.view() : n.value.view());
    } else {
        line = now;
    }
    last_ = now;
    return !line.empty();
}

void AccessAnnouncer::reset() {
    doc_ = nullptr;
    focus_ = WidgetId{};
    last_.clear();
}

}  // namespace mce::ui
