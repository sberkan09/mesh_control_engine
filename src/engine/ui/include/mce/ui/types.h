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
inline constexpr u32 kWidgetKindCount = static_cast<u32>(WidgetKind::Tabs) + 1;

// Which of the skin's fonts a widget's text uses (M-UI-DATA step 3: a theme has a font per role).
// Auto: Button for buttons, toggles and sliders, Body for everything else.
enum class FontRole : u8 { Auto, Body, Title, Button };
using UiFontRole = FontRole;
MCE_ENUM(UiFontRole, Auto, Body, Title, Button)
// Where a Text widget's lines sit across its rect (M-UI-DATA2 1); buttons always center theirs.
enum class TextAlign : u8 { Left, Center, Right };
using UiTextAlign = TextAlign;
MCE_ENUM(UiTextAlign, Left, Center, Right)

// Style fields a widget keeps over a theme even when they equal the kind's default look
// (M-UI-DATA2 2): apply_theme gives a field still at the default the theme's value, so a bit here
// is how a document says "the kind's default, not the theme's". Each bit covers that field in every
// state. `.mcui` spells them as "own_style": ["tint", ...].
enum StyleOwn : u8 {
    OwnTint = 1u << 0,
    OwnTextColor = 1u << 1,
    OwnAccent = 1u << 2,
    OwnSprite = 1u << 3,  // sprite_region, slice and draw_quad
    OwnTextSize = 1u << 4,
    OwnLetterSpacing = 1u << 5,
    OwnTextOutline = 1u << 6,  // text_outline and its color
    OwnTextShadow = 1u << 7,   // text_shadow and its color
};
inline constexpr u32 kStyleOwnCount = 8;
inline constexpr const char* kStyleOwnNames[kStyleOwnCount] = {"tint",         "text_color",     "accent",      "sprite",
                                                               "text_size",    "letter_spacing", "text_outline", "text_shadow"};

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
    // The text's look (M-UI-DATA step 4), in pixels at UI scale 1 like text_size.
    f32 letter_spacing = 0.0f;                      // extra space between letters (may be negative)
    Color text_outline_color{0, 0, 0, 1};
    f32 text_outline = 0.0f;                        // outline thickness; 0 = none
    Color text_shadow_color{0, 0, 0, 0.6f};
    vec2 text_shadow{};                             // drop shadow offset, +y down; (0, 0) = none
};
using UiStyle = Style;
MCE_REFL_STRUCT(UiStyle, tint, text_color, accent, (sprite_region, Help{"into the skin's sprite set; 4294967295 = flat"}),
                (text_size, Range{0, 256}, Help{"pixels; 0 = the font's baked height"}), draw_quad, slice,
                (letter_spacing, Range{-16, 64}, Help{"extra pixels between letters"}), text_outline_color,
                (text_outline, Range{0, 16}, Help{"outline thickness in pixels; 0 = none"}), text_shadow_color,
                (text_shadow, Help{"drop shadow offset in pixels, +y down; [0, 0] = none"}))

// A theme's palette (M-UI-DATA step 3): the colors the widget kinds' default looks are made of.
// These defaults are the engine's own look (ui::Builder, apply_default_style); sRGB like Style.
struct Palette {
    Color panel{0.06f, 0.06f, 0.08f, 0.85f};            // Panel backgrounds
    Color button{0.16f, 0.17f, 0.21f, 0.94f};           // buttons, toggles, slider tracks
    Color button_hover{0.24f, 0.25f, 0.31f, 0.96f};
    Color button_pressed{0.1f, 0.1f, 0.13f, 0.96f};
    Color button_focused{0.34f, 0.25f, 0.12f, 0.96f};
    Color button_disabled{0.12f, 0.12f, 0.14f, 0.6f};
    Color label{1, 1, 1, 1};                            // Text widgets
    Color text{0.88f, 0.88f, 0.9f, 1};                  // on buttons, toggles and sliders
    Color text_focused{1.0f, 0.88f, 0.55f, 1};
    Color text_disabled{0.5f, 0.5f, 0.52f, 1};
    Color accent{0.95f, 0.75f, 0.35f, 1};               // slider fill, toggle check, selected tab
    Color console{0.02f, 0.02f, 0.03f, 0.88f};
    Color console_text{0.9f, 0.9f, 0.9f, 1};
};
using UiPalette = Palette;
MCE_REFL_STRUCT(UiPalette, (panel, Help{"Panel backgrounds"}), (button, Help{"buttons, toggles and slider tracks"}), button_hover,
                button_pressed, button_focused, button_disabled, (label, Help{"Text widgets"}),
                (text, Help{"text on buttons, toggles and sliders"}), text_focused, text_disabled,
                (accent, Help{"slider fill, toggle check, selected tab"}), console, console_text)

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
