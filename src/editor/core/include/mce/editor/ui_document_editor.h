#pragma once
// UI document authoring in the editor (ROADMAP M-UI-DATA step 6): what the UI Document panel drives.
//
//   UiDocumentEditor  one open .mcui document: its widget tree, the selected widget, edits and undo.
//                     Every edit goes through the cook's own writer and reader, as the MCP ui_* tools
//                     do: the edited DocumentData is written canonically (cook::write_ui_document),
//                     read back whole (cook::read_ui_document) and kept only when it reads, so a bad
//                     edit is refused with the cook's message. save() writes that canonical text and
//                     hot-reloads it, so the file the panel leaves is the one ui_create and the
//                     ui_widget_* tools write, and reading it gives the same document.
//   UiDocumentPreview the document drawn alone at a screen size (phone or desktop), as the player
//                     draws it: the theme applied, the theme's and the document's fonts, the built-in
//                     skin underneath.
// Headless: the editor and its edits are tested in CI; the panel draws them.

#include "mce/cook/diagnostics.h"
#include "mce/cook/string.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/inspector.h"
#include "mce/rhi/command_buffer.h"
#include "mce/ui/document_data.h"
#include "mce/ui/ui.h"

namespace mce::editor {

class PreviewRenderer;

// The screen sizes the preview offers: logical pixels (points), drawn at UI scale 1.
struct UiPreviewSize {
    const char* name;
    rhi::Extent2D size;
};
Span<const UiPreviewSize> ui_preview_sizes();

class UiDocumentEditor {
public:
    static constexpr usize kMaxUndo = 128;

    // Opens the UI document `guid` of the session's project (re-reading it when it is already open
    // and nothing is unsaved). False with last_error() set when it is not a .mcui or does not read.
    bool open(EditorSession& session, asset::Guid guid);
    void close();
    bool is_open() const { return !guid_.is_null(); }
    asset::Guid guid() const { return guid_; }
    StringView path() const { return rel_.view(); }  // under assets/
    const ui::DocumentData& data() const { return data_; }
    // Bumped by every change of data(): the preview rebuilds when it moves.
    u64 revision() const { return revision_; }

    // Creates a new, empty document at `rel_path` (a .mcui under assets/) with a fresh GUID, cooks
    // it and opens it. Refuses an existing file.
    Result<asset::Guid> create(EditorSession& session, StringView rel_path, bool modal = true);

    // Re-reads the file when it changed on disk (an MCP tool, a text editor) and nothing is unsaved
    // here. True when it did.
    bool sync(EditorSession& session);

    // ---- selection ----
    u32 selected() const { return selected_; }
    void select(u32 widget) { selected_ = widget < data_.widgets.size() ? widget : ui::kNoWidgetIndex; }
    // "menu/play" for widget `i`.
    cook::String widget_path(u32 i) const;
    // The first index after `i`'s subtree (children follow their parent in document order).
    u32 subtree_end(u32 i) const;
    bool is_inside(u32 widget, u32 ancestor) const;  // `widget` is `ancestor` or below it

    // ---- edits ----
    // Each keeps data() only when the result reads back through the cook; false with
    // last_error() set otherwise (nothing changes). Edits mark the document unsaved; a run of
    // edits until the next save() is one undo step.
    bool apply(const ui::DocumentData& next);
    // A new widget of `kind` as the last child of `parent` (kNoWidgetIndex: the root), with its
    // kind's look and a free name ("button", "button2", ...); selects it.
    bool add_widget(u32 parent, ui::WidgetKind kind);
    // Removes `widget` and its children; nav edges and the default focus that named them are cleared.
    bool remove_widget(u32 widget);
    // Moves `widget` (with its children) one place up (-1) or down (+1) among its siblings.
    bool move_widget(u32 widget, i32 dir);
    // Changes a widget's kind: it takes the new kind's look and range (builder_defaults).
    bool set_kind(u32 widget, ui::WidgetKind kind);

    bool dirty() const { return dirty_; }
    // Writes the canonical text and hot-reloads it when there are unsaved edits.
    Result<void> save(EditorSession& session);
    bool undo(EditorSession& session);
    bool redo(EditorSession& session);
    bool can_undo() const { return !undo_.empty() || dirty_; }
    bool can_redo() const { return !redo_.empty(); }

    StringView last_error() const { return error_.view(); }

    // The canonical .mcui text of data().
    cook::String text() const;

private:
    bool read_text(StringView text, ui::DocumentData& out);
    void set_data(const ui::DocumentData& d);
    // `order`: old widget indices in their new order (removed ones left out).
    static void reorder(const ui::DocumentData& in, Span<const u32> order, ui::DocumentData& out);

    asset::Guid guid_;
    cook::String rel_;
    cook::String disk_;  // the file as last read or written
    ui::DocumentData data_;
    u32 selected_ = ui::kNoWidgetIndex;
    u64 revision_ = 0;
    bool dirty_ = false;
    cook::String error_;
    Array<cook::String> undo_, redo_;  // canonical texts
};

// The selected widget's fields (or, with none selected, the document's: modal, font, theme, default
// focus) through the inspector's widget seam. `state` is the style state shown; with `every_state`
// a style field changed in it is changed in every state. True when an edit was applied.
bool draw_ui_inspector(DrawCtx& ui, UiDocumentEditor& ed, u32& state, bool& every_state);

class UiDocumentPreview {
public:
    UiDocumentPreview() = default;
    UiDocumentPreview(const UiDocumentPreview&) = delete;
    UiDocumentPreview& operator=(const UiDocumentPreview&) = delete;
    ~UiDocumentPreview() { reset(); }

    // Draws `ed`'s document into `target` (`size` pixels at `ui_scale`). It is rebuilt when the
    // document changes, when the preview's content is remounted and when `content` moves (the
    // session's hot_reload_count(): a theme or a font re-cooked). Pointer: hover and press in
    // target pixels (negative: none).
    void record(rhi::CommandBuffer& cmd, PreviewRenderer& preview, const UiDocumentEditor& ed, u64 content,
                rhi::TextureHandle target, rhi::Extent2D size, f32 ui_scale, math::vec2 pointer = {-1, -1},
                bool pointer_down = false);
    void reset();

private:
    bool rebuild(PreviewRenderer& preview, const UiDocumentEditor& ed);
    bool stale(PreviewRenderer& preview, const UiDocumentEditor& ed, u64 content) const;

    ui::Document* doc_ = nullptr;
    asset::Guid guid_;
    u64 revision_ = ~u64(0);
    u64 content_ = ~u64(0), generation_ = ~u64(0);
    bool was_down_ = false;
};

}  // namespace mce::editor
