#pragma once
// The stock options screen's controls page (M-ACCESS step 3, FR-INP-040): a remap list built from
// the input map, live rebinding through InputSystem's capture, hold-to-toggle per button action and
// one-hand presets. Everything else on the screen (sensitivity, text scale, colorblind filter,
// volumes) is plain convar binding in the `.mcui` document; this class fills and drives only the
// widgets it finds by name anywhere in the document:
//
//   remap_list      a Stack or Scroll: one row per action (per direction for axis actions), with a
//                   button per keyboard/mouse and gamepad binding; confirming one captures the next
//                   press; Button actions get a "Toggle" switch (hold-to-toggle)
//   preset_both     buttons: the cooked layout, the left-hand and the right-hand keyboard layouts
//   preset_left
//   preset_right
//   reset_bindings  back to the cooked map and no toggles
//
// Maps named "UI" (menu navigation) are not listed. Settings persist through the host: bindings by
// InputSystem::bindings_generation, toggles and sensitivity through the Saved convars
// input.toggle_actions and input.look_sensitivity (apply_input_options).

#include "mce/input/input_system.h"
#include "mce/ui/ui.h"

namespace mce::ui {

class OptionsPanel {
public:
    // Fills `remap_list` and remembers the preset buttons. False when the document has no
    // remap_list (the panel then does nothing). Call again after the input map is reloaded.
    bool attach(Document& doc, input::InputSystem& input);
    void detach();
    bool attached() const { return doc_ != nullptr; }
    // Per frame, after doc.update(): the document's events and the capture's progress.
    void update();

    u32 row_count() const { return rows_; }
    // The binding button for (action, index), or null: tests and hosts drive it like a player.
    WidgetId binding_button(StringId map, StringId action, u32 index) const;
    WidgetId toggle_switch(StringId action) const;
    bool capturing() const { return capturing_ != kNone; }

private:
    static constexpr u32 kNone = ~0u;
    struct Slot {
        WidgetId button;
        StringId map, action;
        u32 index = 0;
        u8 accept = 0;
    };
    struct ToggleRow {
        WidgetId toggle;
        StringId action;
    };
    void build();
    void refresh_labels();

    Document* doc_ = nullptr;
    input::InputSystem* input_ = nullptr;
    WidgetId list_, preset_[3], reset_;
    Array<Slot> slots_;
    Array<ToggleRow> toggles_;
    Array<WidgetId> rows_widgets_;
    u32 rows_ = 0;
    u32 capturing_ = kNone;  // index into slots_
    u64 built_generation_ = 0;
};

// Reads the Saved convars input.look_sensitivity and input.toggle_actions (comma-separated action
// names) into `input`; cheap when nothing changed. Hosts call it once per frame.
void apply_input_options(input::InputSystem& input);
// Sets or clears `action` in input.toggle_actions (what the remap list's switches do).
void set_toggle_option(StringView action, bool on);

}  // namespace mce::ui
