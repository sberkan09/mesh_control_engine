#pragma once
// The mce_ui tree for screen readers (M-ACCESS step 4): what each widget is (its role), what a
// reader calls it (its name), what it holds (its value) and where it is, and the line a reader
// speaks when focus moves or a focused value changes ("Music, slider, 40%", "Subtitles, switch,
// on"). The platform side (mce/platform/screen_reader.h) speaks the lines through VoiceOver,
// TalkBack or Narrator; the stock player connects the two (access.screen_reader).
//
// A widget's name is its own text; a slider or toggle without text takes the text of the label
// just before it in its parent (the `"MUSIC"` Text over the music slider in options.mcui).
// Panels, stacks and scroll views are groups: they carry no line of their own.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/ui/ui.h"

namespace mce::ui {

enum class AccessRole : u8 { Group, Text, Image, Button, Switch, Slider, Tab, List, TextInput };
const char* access_role_name(AccessRole r);  // what a reader says: "button", "switch", "slider", ...

struct AccessNode {
    WidgetId widget;
    i32 parent = -1;  // index into the tree, -1 for the root's children
    AccessRole role = AccessRole::Group;
    FixedString<128> name;
    FixedString<32> value;  // sliders: "40%" (a 0..1 range) or the number; switches: "on" / "off"
    Rect rect;
    bool focused = false;
    bool enabled = true;
};

// The visible widgets of a document in document order (hidden subtrees are left out).
void build_access_tree(const Document& doc, Array<AccessNode>& out);

// The node for one widget, and the line a reader speaks for it: "<name>, <role>[, <value>]"
// ("" for groups and widgets with no name and no value).
AccessNode access_node(const Document& doc, WidgetId w);
void access_line(const AccessNode& n, FixedString<192>& out);

// Watches a document's focus and the focused widget's value: next() hands back the line to speak
// when either changed since the last call (the first call speaks the focused widget, if any).
class AccessAnnouncer {
public:
    // `doc` may change between calls (another screen came up): a new document speaks its focus.
    bool next(const Document* doc, FixedString<192>& line);
    void reset();

private:
    const Document* doc_ = nullptr;
    WidgetId focus_;
    FixedString<192> last_;
};

}  // namespace mce::ui
