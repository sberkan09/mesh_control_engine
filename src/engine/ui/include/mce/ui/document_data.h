#pragma once
// mce_ui: a UI document as data (M-UI-DATA step 1). A `.mcui` asset (JSON source, cooked by the
// `ui` importer to CHNK_UIDOC) holds one widget tree: the same values ui::Document holds, flat and
// in document order, with anchors, styles and layouts as the reflected Ui* types (mce/ui/types.h).
//
//   cook:    .mcui JSON -> DocumentData -> encode_ui_document -> CHNK_UIDOC
//   runtime: CHNK_UIDOC -> decode_ui_document -> DocumentData -> build_document -> ui::Document
//   tools:   ui::Document -> extract_document -> DocumentData (-> cook::write_ui_document, JSON)
//
// encode/decode use nothing but this header, so the cook links them without the renderer;
// build_document / extract_document are in a separate TU (document_build.cpp).
//
// Chunk layout v3 (v2: action / bind / format strings; v3: the theme and each widget's font role)
// (little-endian, POD records): UiDocHeader | UiWidgetRecord[widget_count] | string blob (names,
// texts, convars, actions, bindings).
//
// Themes (M-UI-DATA step 3): a `.mcuitheme` asset (cooked by the `uitheme` importer to
// CHNK_UITHEME) restyles every document that uses it. apply_theme gives each widget style field the
// document leaves at its kind's default look (builder_defaults) the theme's value instead; fields a
// document sets keep their value. default_theme() is the engine's own look.

#include "mce/asset/guid.h"
#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/result.h"
#include "mce/ui/types.h"

namespace mce::ui {

class Document;

inline constexpr u32 kNoWidgetIndex = ~0u;
inline constexpr u32 kWidgetStateCount = static_cast<u32>(WidgetState::Count);
inline constexpr u32 kMaxDocumentWidgets = 4096;

enum NavEdge : u32 { NavUp, NavDown, NavLeft, NavRight, NavEdgeCount };

// One widget, as Document holds it. Everything is explicit: build_document sets every field, so a
// widget's look never depends on the Builder defaults of its kind.
struct WidgetData {
    FixedString<32> name;  // unique among its siblings; no '/'
    WidgetKind kind = WidgetKind::Panel;
    u32 parent = kNoWidgetIndex;  // an earlier widget; kNoWidgetIndex = the document's root
    Anchors anchors;
    Style styles[kWidgetStateCount];
    FixedString<256> text;
    f32 value = 0.0f, lo = 0.0f, hi = 1.0f, step = 0.1f;
    bool visible = true, interactable = true, blocks_pointer = false;
    StackLayout stack;
    GridLayout grid;
    u32 nav[NavEdgeCount] = {kNoWidgetIndex, kNoWidgetIndex, kNoWidgetIndex, kNoWidgetIndex};  // widget indices
    FixedString<64> convar;  // bound with Document::bind_convar; empty = none
    // Gameplay talks to a document through data (M-UI-DATA step 2). `action`: the input action a
    // Confirmed / ValueChanged event raises in the player (InputState, so replays record it).
    // `bind`: "<Component>/<field>" the host binds read-only with Document::bind_property, shown
    // with `format` (printf, one double; empty = "%g").
    FixedString<32> action;
    FixedString<64> bind;
    FixedString<32> format;
    FontRole font_role = FontRole::Auto;  // which of the skin's fonts the text uses
};

struct DocumentData {
    explicit DocumentData(mem::IAllocator& a = mem::heap()) : widgets(a) {}

    // The skin by asset reference: a Font and a SpriteSheet whose region `white_region` is a white
    // texel block (flat quads). Null GUIDs: the engine's built-in skin.
    asset::Guid font;
    asset::Guid sprites;
    u32 white_region = 0;
    asset::Guid theme;  // a UiTheme; null: the default look (a scene's UiScreen may name one instead)
    bool modal = true;  // Document::set_modal
    u32 default_focus = kNoWidgetIndex;
    Array<WidgetData> widgets;  // depth-first document order: parents before children

    // "buttons/resume" (relative to the root, as Document::find), or kNoWidgetIndex.
    u32 find(StringView path) const;
    void clear();
};

// The look and values Builder gives a new widget of `kind` (every state's style, a toggle's
// range): what a .mcui widget starts from unless it says "kind_defaults": false. Interactive
// kinds get the default look (apply_default_style).
void builder_defaults(WidgetKind kind, WidgetData& out);
// Builder::modal's veil over a Panel: dim, anchored to the whole screen, blocks the pointer,
// hidden until Document::open_modal.
void modal_defaults(WidgetData& out);

// A property binding's format is data, so it is checked before printf sees it: literal text, "%%",
// and exactly one floating conversion (flags "-+ #0", a width and a precision of digits, then one of
// f F e E g G). Empty is valid ("%g").
bool valid_number_format(StringView format);

// The CHNK_UIDOC payload. decode checks bounds, enumerators, the tree order, unique sibling names
// and the nav / default-focus indices, so a chunk build_document would misread never loads.
void encode_ui_document(const DocumentData& d, Array<byte>& out);
Result<void> decode_ui_document(Span<const byte> payload, DocumentData& out);

// Adds the widgets under the root of `doc` (a fresh document) and applies the document-level values
// (modal, default focus). The skin is the caller's: it resolves `font` and `sprites`.
// Unknown or cheat convars are logged by bind_convar and left unbound.
Result<void> build_document(const DocumentData& d, Document& doc);
// The document's tree (the root's subtree, root excluded) back into data: what an editor saves.
// `font`, `sprites`, `white_region` and `theme` are left as they are in `out`.
void extract_document(const Document& doc, DocumentData& out);

// ------------------------------------------------------------------ themes

inline constexpr u32 kThemeFontCount = 3;  // body, title, button: FontRole - 1

struct ThemeData {
    Palette palette;
    asset::Guid fonts[kThemeFontCount];  // per role; null: the document's font (or the built-in one)
    asset::Guid sprites;                 // a SpriteSheet skin; null: the document's
    u32 white_region = 0;
    Style styles[kWidgetKindCount][kWidgetStateCount];  // each kind's default look under this theme

    asset::Guid font(FontRole role) const { return role == FontRole::Auto ? asset::Guid{} : fonts[static_cast<u32>(role) - 1]; }
};

// Every kind's look made of `palette`'s colors (what a theme with no "styles" overrides gets).
void theme_from_palette(const Palette& palette, ThemeData& out);
// The engine's own look: Palette's defaults, no fonts, no sprites. builder_defaults reads it.
const ThemeData& default_theme();
// Each widget style field of `d` that equals its kind's default look takes `theme`'s value.
void apply_theme(const ThemeData& theme, DocumentData& d);
// The font role a widget's text uses: `role`, or for Auto the kind's (Button for buttons, toggles,
// sliders; Body otherwise).
FontRole resolve_font_role(WidgetKind kind, FontRole role);

// The CHNK_UITHEME payload (UI_THEME_VERSION); decode checks bounds and enumerators.
void encode_ui_theme(const ThemeData& t, Array<byte>& out);
Result<void> decode_ui_theme(Span<const byte> payload, ThemeData& out);

}  // namespace mce::ui
