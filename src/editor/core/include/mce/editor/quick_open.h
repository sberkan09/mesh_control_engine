#pragma once
// QuickOpen — one box to open anything (M-QUICKOPEN): a list of labelled items (assets, entities, panels, commands)
// and a fuzzy search over their labels. The caller fills it from what the editor already lists (the project's assets,
// the open scene's entities, the panels' titles, the menu commands) and keeps its own meaning for `id`; the search
// only ranks. Matching is case-insensitive and needs every query letter in order. A letter that starts a word (the
// label's first letter, or one after '/', '_', '-', ' ' or '.', or an upper-case letter after a lower-case one) or
// follows the previous match scores higher, and between equal scores the shorter label and then the earlier item win.
// Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/cook/string.h"

namespace mce::editor {

enum class QuickOpenKind : u8 { Asset, Entity, Panel, Command };

// "Asset", "Entity", "Panel", "Command".
const char* quick_open_kind_name(QuickOpenKind kind);

struct QuickOpenItem {
    QuickOpenKind kind = QuickOpenKind::Asset;
    cook::String label;  // matched and shown: an asset path, an entity name, a panel title, a command name
    u64 id = 0;          // the caller's handle for what to do on Enter
};

class QuickOpen {
public:
    void clear() { items_.clear(); }
    void add(QuickOpenKind kind, StringView label, u64 id = 0);
    const Array<QuickOpenItem>& items() const { return items_; }

    // The score of `label` for `query` (higher is better), or -1 when a query letter is missing. An empty query
    // matches everything with the same score.
    static i32 score(StringView query, StringView label);

    // Indices into items() of the best `limit` matches, best first. An empty query lists the first `limit` items.
    void search(StringView query, u32 limit, Array<u32>& out) const;

private:
    Array<QuickOpenItem> items_;
};

}  // namespace mce::editor
