#pragma once
// mce_ui's authoring values (ui.h has the widget tree that uses them): widget kinds and states,
// anchors, styles and container layouts. Light on purpose (math and mce_refl only) so the editor
// and the MCP server reflect them without the renderer: they are registered under Ui* names,
// because reflected type names are global and "Style" or "Axis" may well be a game's too.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::ui {

using math::Color;
using math::vec2;

enum class WidgetKind : u8 { Panel, Image, Text, Button, Toggle, Slider, Stack, Grid, Console, Scroll, Tabs };
using UiWidgetKind = WidgetKind;
MCE_ENUM(UiWidgetKind, Panel, Image, Text, Button, Toggle, Slider, Stack, Grid, Console, Scroll, Tabs)
enum class WidgetState : u8 { Normal, Hover, Pressed, Focused, Disabled, Count };
using UiWidgetState = WidgetState;
MCE_ENUM(UiWidgetState, Normal, Hover, Pressed, Focused, Disabled, Count)

// What a widget's anchors are fractions of: its parent's rect (the default; for the root's
// children that is the safe area), the safe area (the screen minus the notch and the home
// indicator) or the whole screen (a backdrop that bleeds under the notch).
enum class AnchorTo : u8 { Parent, SafeArea, Screen };
using UiAnchorTo = AnchorTo;
MCE_ENUM(UiAnchorTo, Parent, SafeArea, Screen)

struct Anchors {                                    // uGUI RectTransform, parent-relative
    vec2 min{0, 0}, max{0, 0};                      // 0..1 of the parent rect
    vec2 offset_min{0, 0}, offset_max{0, 0};        // pixels at UI scale 1
    vec2 pivot{0.5f, 0.5f};                         // size-change origin (point anchors)
    AnchorTo to = AnchorTo::Parent;
};
using UiAnchors = Anchors;
MCE_REFL_STRUCT(UiAnchors, (min, Help{"0..1 of the rect anchored to"}), max, (offset_min, Help{"pixels at UI scale 1"}),
                offset_max, (pivot, Help{"where a point anchor grows from"}), to)

// 9-slice insets in texels of the sprite region: the corners keep their size (times the UI
// scale), the edges stretch along one axis and the middle along both. All zero: one stretched quad.
struct Slice {
    f32 left = 0, top = 0, right = 0, bottom = 0;
    bool empty() const { return left <= 0 && top <= 0 && right <= 0 && bottom <= 0; }
};
using UiSlice = Slice;
MCE_REFL_STRUCT(UiSlice, left, top, right, bottom)

struct Style {  // one per WidgetState; colors are sRGB (as a design tool shows them), alpha linear
    Color tint{1, 1, 1, 1};                         // the widget's quad (Panel / Button / track)
    Color text_color{1, 1, 1, 1};
    Color accent{0.95f, 0.75f, 0.35f, 1};           // slider fill + handle, toggle check, caret, selected tab
    u32 sprite_region = ~0u;                        // into the skin's sprite set; ~0 = the white region (flat)
    f32 text_size = 0.0f;                           // pixels; 0 = the font's baked height
    bool draw_quad = true;                          // false: text only (labels)
    Slice slice;                                    // 9-slice insets of sprite_region
};
using UiStyle = Style;
MCE_REFL_STRUCT(UiStyle, tint, text_color, accent, (sprite_region, Help{"into the skin's sprite set; 4294967295 = flat"}),
                (text_size, Range{0, 256}, Help{"pixels; 0 = the font's baked height"}), draw_quad, slice)

enum class Axis : u8 { Vertical, Horizontal };
using UiAxis = Axis;
MCE_ENUM(UiAxis, Vertical, Horizontal)
enum class Align : u8 { Start, Center, End, Stretch };
using UiAlign = Align;
MCE_ENUM(UiAlign, Start, Center, End, Stretch)
// A stack, and the content of a scroll view (which is a stack that may overflow its rect).
struct StackLayout {
    Axis axis = Axis::Vertical;
    f32 spacing = 8.0f;
    f32 padding = 0.0f;
    Align align = Align::Stretch;  // cross axis
};
using UiStackLayout = StackLayout;
MCE_REFL_STRUCT(UiStackLayout, axis, (spacing, Range{0, 512}), (padding, Range{0, 512}), align)
struct GridLayout {
    u32 columns = 3;  // 0 = as many cells as fit the width (a level grid on any aspect ratio)
    vec2 cell{64, 64};
    vec2 spacing{8, 8};
};
using UiGridLayout = GridLayout;
MCE_REFL_STRUCT(UiGridLayout, (columns, Help{"0 = as many as fit the width"}), cell, spacing)

}  // namespace mce::ui
