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
// Chunk layout v1 (little-endian, POD records): UiDocHeader | UiWidgetRecord[widget_count] |
// string blob (names, texts, convars).

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
};

struct DocumentData {
    explicit DocumentData(mem::IAllocator& a = mem::heap()) : widgets(a) {}

    // The skin by asset reference: a Font and a SpriteSheet whose region `white_region` is a white
    // texel block (flat quads). Null GUIDs: the engine's built-in skin.
    asset::Guid font;
    asset::Guid sprites;
    u32 white_region = 0;
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

// The CHNK_UIDOC payload. decode checks bounds, enumerators, the tree order, unique sibling names
// and the nav / default-focus indices, so a chunk build_document would misread never loads.
void encode_ui_document(const DocumentData& d, Array<byte>& out);
Result<void> decode_ui_document(Span<const byte> payload, DocumentData& out);

// Adds the widgets under the root of `doc` (a fresh document) and applies the document-level values
// (modal, default focus). The skin is the caller's: it resolves `font` and `sprites`.
// Unknown or cheat convars are logged by bind_convar and left unbound.
Result<void> build_document(const DocumentData& d, Document& doc);
// The document's tree (the root's subtree, root excluded) back into data: what an editor saves.
// `font`, `sprites` and `white_region` are left as they are in `out`.
void extract_document(const Document& doc, DocumentData& out);

}  // namespace mce::ui
