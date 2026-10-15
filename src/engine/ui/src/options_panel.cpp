#include "mce/ui/options_panel.h"

#include <cstdio>

#include "mce/convar.h"
#include "mce/input/binding_names.h"

namespace mce::ui {

namespace {

MCE_CVAR(cv_look_sensitivity, "input.look_sensitivity", 1.0f, "Scales the Look action (mouse, stick, touch); 1 = as bound",
         CVarFlags::Saved);
MCE_CVAR(cv_toggle_actions, "input.toggle_actions", "",
         "Hold-to-toggle: comma-separated Button actions a press latches on and the next press releases",
         CVarFlags::Saved);

constexpr u32 kMaxButtonsPerRow = 4;

void append_u32(FixedString<64>& s, u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", v);
    s.append(buf);
}
constexpr f32 kLabelWidth = 190.0f;
constexpr f32 kButtonWidth = 150.0f;

WidgetId find_named(const Document& d, WidgetId w, StringView name) {
    if (d.name(w) == name) return w;
    for (WidgetId c = d.first_child(w); d.valid(c); c = d.next_sibling(c))
        if (const WidgetId f = find_named(d, c, name); d.valid(f)) return f;
    return {};
}

bool rebindable(input::Device d) {
    return d == input::Device::Keyboard || d == input::Device::MouseButton || d == input::Device::GamepadButton ||
           d == input::Device::GamepadAxis;
}

u8 accept_for(input::Device d) {
    return d == input::Device::GamepadButton || d == input::Device::GamepadAxis
               ? u8(input::RebindAccept::Gamepad)
               : u8(input::RebindAccept::Keyboard | input::RebindAccept::Mouse);
}

// Direction slot of a binding on an axis action: 0 right / +, 1 left / -, 2 up, 3 down.
u32 direction(const input::BindingDef& b) {
    const bool neg = (b.scale < 0.f) != ((b.flags & input::BindingFlag::Invert) != 0);
    return b.axis == 0 ? (neg ? 1u : 0u) : (neg ? 3u : 2u);
}

}  // namespace

bool OptionsPanel::attach(Document& doc, input::InputSystem& input) {
    detach();
    const WidgetId list = find_named(doc, doc.root(), "remap_list");
    if (!doc.valid(list)) return false;
    doc_ = &doc;
    input_ = &input;
    list_ = list;
    static constexpr const char* kPresets[3] = {"preset_both", "preset_left", "preset_right"};
    for (u32 i = 0; i < 3; ++i) preset_[i] = find_named(doc, doc.root(), kPresets[i]);
    reset_ = find_named(doc, doc.root(), "reset_bindings");
    build();
    return true;
}

void OptionsPanel::detach() {
    if (input_ && capturing_ != kNone) input_->cancel_rebind();
    doc_ = nullptr;
    input_ = nullptr;
    slots_.clear();
    toggles_.clear();
    rows_widgets_.clear();
    rows_ = 0;
    capturing_ = kNone;
}

void OptionsPanel::build() {
    for (WidgetId w : rows_widgets_) doc_->remove(w);
    rows_widgets_.clear();
    slots_.clear();
    toggles_.clear();
    rows_ = 0;
    const input::InputMapSet& set = input_->maps();
    StackLayout row_layout;
    row_layout.axis = Axis::Horizontal;
    row_layout.spacing = 6.0f;
    FixedString<64> label, name;
    for (const input::ActionMapDef& m : set.maps) {
        if (m.name == MCE_SID("UI")) continue;  // menu navigation: not the player's to remap
        for (u32 ai = 0; ai < m.action_count; ++ai) {
            const usize action_index = m.first_action + ai;
            const input::ActionDef& a = set.actions[action_index];
            const StringView action_label = set.label(action_index);
            const bool button = a.type == input::ActionType::Button;
            const u32 groups = button ? 1u : a.type == input::ActionType::Axis1D ? 2u : 4u;
            for (u32 g = 0; g < groups; ++g) {
                u32 count = 0;
                for (u32 bi = 0; bi < a.binding_count; ++bi) {
                    const input::BindingDef& b = set.bindings[a.first_binding + bi];
                    if (rebindable(b.device) && (button || direction(b) == g)) ++count;
                }
                if (count == 0) continue;
                label.assign(action_label.empty() ? StringView("Action") : action_label);
                if (!button) {
                    static constexpr const char* kDir2[4] = {" Right", " Left", " Up", " Down"};
                    static constexpr const char* kDir1[2] = {" +", " -"};
                    label.append(groups == 4 ? kDir2[g] : kDir1[g]);
                }
                name.assign("row_");
                append_u32(name, rows_);
                Builder row = Builder(*doc_, list_).stack(name.view(), row_layout);
                rows_widgets_.push_back(row.id());
                ++rows_;
                name.assign("label_");
                append_u32(name, rows_ - 1);
                row.text(name.view(), label.view()).at({0, 0}, {kLabelWidth, 0});
                u32 placed = 0;
                for (u32 bi = 0; bi < a.binding_count && placed < kMaxButtonsPerRow; ++bi) {
                    const input::BindingDef& b = set.bindings[a.first_binding + bi];
                    if (!rebindable(b.device) || (!button && direction(b) != g)) continue;
                    name.assign("bind_");
                    append_u32(name, static_cast<u32>(slots_.size()));
                    Slot s;
                    s.button = row.button(name.view(), "").at({0, 0}, {kButtonWidth, 0}).id();
                    s.map = m.name;
                    s.action = a.name;
                    s.index = bi;
                    s.accept = accept_for(b.device);
                    slots_.push_back(s);
                    ++placed;
                }
                if (button && !action_label.empty()) {
                    name.assign("toggle_");
                    append_u32(name, static_cast<u32>(toggles_.size()));
                    ToggleRow t;
                    t.toggle = row.toggle(name.view(), "Toggle", input_->is_toggle(a.name)).id();
                    t.action = a.name;
                    toggles_.push_back(t);
                }
            }
        }
    }
    refresh_labels();
}

void OptionsPanel::refresh_labels() {
    const input::InputMapSet& set = input_->maps();
    input::BindingName text;
    for (const Slot& s : slots_) {
        const input::ActionMapDef* m = set.find_map(s.map);
        if (!m) continue;
        for (u32 ai = 0; ai < m->action_count; ++ai) {
            const input::ActionDef& a = set.actions[m->first_action + ai];
            if (a.name != s.action || s.index >= a.binding_count) continue;
            input::binding_name(set.bindings[a.first_binding + s.index], text);
            doc_->set_text(s.button, text.view());
        }
    }
    for (const ToggleRow& t : toggles_) doc_->set_value(t.toggle, input_->is_toggle(t.action) ? 1.0f : 0.0f);
    built_generation_ = input_->bindings_generation();
}

void OptionsPanel::update() {
    if (!doc_) return;
    if (capturing_ != kNone && input_->rebind_state() != input::RebindState::Waiting) {
        capturing_ = kNone;
        refresh_labels();
    }
    if (capturing_ == kNone && built_generation_ != input_->bindings_generation()) refresh_labels();
    for (const UiEvent& e : doc_->events()) {
        if (e.kind == UiEvent::ValueChanged) {
            for (const ToggleRow& t : toggles_) {
                if (!(t.toggle == e.widget)) continue;
                const input::InputMapSet& set = input_->maps();
                for (usize i = 0; i < set.actions.size(); ++i)
                    if (set.actions[i].name == t.action && !set.label(i).empty()) set_toggle_option(set.label(i), e.value > 0.5f);
                apply_input_options(*input_);
            }
            continue;
        }
        if (e.kind != UiEvent::Confirmed) continue;
        for (u32 i = 0; i < 3; ++i)
            if (doc_->valid(preset_[i]) && preset_[i] == e.widget) {
                input_->cancel_rebind();
                capturing_ = kNone;
                input_->apply_hand_preset(static_cast<input::Hand>(i));
                refresh_labels();
            }
        if (doc_->valid(reset_) && reset_ == e.widget) {
            input_->cancel_rebind();
            capturing_ = kNone;
            input_->reset_bindings();
            cv_toggle_actions.set_string("");
            apply_input_options(*input_);
            refresh_labels();
        }
        for (u32 i = 0; i < slots_.size(); ++i) {
            const Slot& s = slots_[i];
            if (!(s.button == e.widget)) continue;
            if (capturing_ != kNone) {
                input_->cancel_rebind();
                refresh_labels();
            }
            if (input_->begin_rebind(s.map, s.action, s.index, 5.0f, s.accept)) {
                capturing_ = i;
                doc_->set_text(s.button, s.accept == input::RebindAccept::Gamepad ? "Press a button" : "Press a key");
            }
        }
    }
}

WidgetId OptionsPanel::binding_button(StringId map, StringId action, u32 index) const {
    for (const Slot& s : slots_)
        if (s.map == map && s.action == action && s.index == index) return s.button;
    return {};
}

WidgetId OptionsPanel::toggle_switch(StringId action) const {
    for (const ToggleRow& t : toggles_)
        if (t.action == action) return t.toggle;
    return {};
}

void apply_input_options(input::InputSystem& input) {
    static const input::InputSystem* applied_to = nullptr;
    static FixedString<1024> applied;
    input.set_look_sensitivity(cv_look_sensitivity.get_float());
    const StringView list = cv_toggle_actions.get_string();
    if (applied_to == &input && applied.view() == list) return;
    applied_to = &input;
    applied.assign(list);
    input.clear_toggles();
    usize at = 0;
    while (at <= list.size()) {
        usize end = list.find(',', at);
        if (end == StringView::npos) end = list.size();
        StringView item = list.substr(at, end - at);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (!item.empty()) input.set_toggle(StringId(item), true);
        at = end + 1;
    }
}

void set_toggle_option(StringView action, bool on) {
    FixedString<1024> out;
    const StringView list = cv_toggle_actions.get_string();
    usize at = 0;
    while (at <= list.size()) {
        usize end = list.find(',', at);
        if (end == StringView::npos) end = list.size();
        const StringView item = list.substr(at, end - at);
        if (!item.empty() && item != action) {
            if (!out.empty()) out.append(",");
            out.append(item);
        }
        at = end + 1;
    }
    if (on) {
        if (!out.empty()) out.append(",");
        out.append(action);
    }
    cv_toggle_actions.set_string(out.view());
}

}  // namespace mce::ui
