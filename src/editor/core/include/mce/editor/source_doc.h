#pragma once
// SourceDoc — one JSON source asset as text (M-SOURCEEDIT): the Source panel's model. The text is edited in memory,
// check() cooks it the way the project cook would (nothing is written) and keeps what the cook says as problems with
// line and column, and save() goes through the cook-first write (validate_and_save_source), so a document the cook
// refuses is not written. Headless, tested in CI; the Source panel draws it.

#include "mce/cook/string.h"
#include "mce/editor/project.h"

namespace mce::editor {

struct SourceProblem {
    u32 line = 0;  // 1-based; 0 = the whole file
    u32 col = 0;
    cook::String message;
};

class SourceDoc {
public:
    // Reads the asset's file. False with `err` set when it cannot be read.
    bool open(const EditorProject& project, const ProjectAsset& asset, cook::String& err);
    bool is_open() const { return open_; }
    asset::Guid guid() const { return guid_; }
    StringView path() const { return path_.view(); }
    StringView text() const { return text_.view(); }
    // Replaces the text; the problems are stale until the next check().
    void set_text(StringView text);
    // The text differs from the file as last read or saved.
    bool dirty() const { return text_.view() != saved_.view(); }

    // Cooks the text in memory. True when the cook accepts it; the problems say why not.
    bool check(const EditorProject& project);
    Span<const SourceProblem> problems() const { return Span<const SourceProblem>(problems_.data(), problems_.size()); }
    // True once the current text was checked (set_text clears it).
    bool checked() const { return checked_; }

    // Cooks, then writes atomically; the document stays open on the new asset entry. False with `err` set (nothing was
    // written). Rescans the catalog, so project asset pointers are invalid afterwards.
    bool save(EditorProject& project, cook::String& err);
    // Reads the file again, dropping the edits.
    bool revert(const EditorProject& project, cook::String& err);

private:
    const ProjectAsset* find(const EditorProject& project) const;
    // A file that names its GUID in its text (every native kind) must still name it: dropping it deletes the asset.
    bool keeps_guid() const;

    bool open_ = false;
    bool checked_ = false;
    asset::Guid guid_;
    cook::String path_;
    cook::String text_;
    cook::String saved_;
    Array<SourceProblem> problems_;
};

// Whether the asset is a text source the Source panel can edit: a JSON kind of the asset kind table (mce/editor/asset_kinds.h).
bool source_editable(const ProjectAsset& asset);

}  // namespace mce::editor
