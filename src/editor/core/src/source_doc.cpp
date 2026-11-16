#include "mce/editor/source_doc.h"

#include "mce/cook/diagnostics.h"
#include "mce/cook/fs.h"
#include "mce/editor/asset_form.h"
#include "mce/editor/asset_kinds.h"

namespace mce::editor {

const ProjectAsset* SourceDoc::find(const EditorProject& project) const {
    for (const ProjectAsset& a : project.assets())
        if (a.path.view() == path_.view()) return &a;
    return nullptr;
}

bool SourceDoc::open(const EditorProject& project, const ProjectAsset& asset, cook::String& err) {
    cook::String text;
    if (!cook::fs::read_text(project.asset_path(asset.path.view()).view(), text)) {
        err += "could not read the file";
        return false;
    }
    guid_ = asset.guid;
    path_ = asset.path;
    text_ = text;
    saved_ = text;
    problems_.clear();
    checked_ = false;
    open_ = true;
    return true;
}

void SourceDoc::set_text(StringView text) {
    if (text_.view() == text) return;
    text_ = cook::String(text);
    checked_ = false;
}

bool SourceDoc::keeps_guid() const {
    const asset::GuidString g = asset::to_string(guid_);
    return saved_.view().find(StringView(g.c_str())) == StringView::npos || text_.view().find(StringView(g.c_str())) != StringView::npos;
}

bool SourceDoc::check(const EditorProject& project) {
    problems_.clear();
    checked_ = true;
    if (!keeps_guid()) {
        // A document that names its own GUID is no asset without it: every reference to it would break.
        problems_.push_back(SourceProblem{0, 0, cook::String("the text no longer carries this asset's GUID; keep the \"guid\" member")});
        return false;
    }
    const ProjectAsset* asset = find(project);
    if (!asset) {
        problems_.push_back(SourceProblem{0, 0, cook::String("the asset is no longer in the project")});
        return false;
    }
    cook::DiagList diags;
    cook::String why;
    if (check_source(project, *asset, text_.view(), diags, why)) return true;
    for (const cook::Diagnostic& d : diags.all()) {
        if (d.severity != cook::Severity::Error) continue;
        problems_.push_back(SourceProblem{d.line, d.col, d.message});
    }
    if (problems_.empty()) problems_.push_back(SourceProblem{0, 0, why});
    return false;
}

bool SourceDoc::save(EditorProject& project, cook::String& err) {
    const ProjectAsset* asset = find(project);
    if (!asset) {
        err += "the asset is no longer in the project";
        return false;
    }
    if (!keeps_guid()) {
        err += "the text no longer carries this asset's GUID; keep the \"guid\" member (nothing was written)";
        return false;
    }
    const cook::String text = text_;
    if (!validate_and_save_source(project, *asset, text.view(), err)) return false;
    saved_ = text;
    problems_.clear();
    checked_ = true;
    return true;
}

bool SourceDoc::revert(const EditorProject& project, cook::String& err) {
    const ProjectAsset* asset = find(project);
    if (!asset) {
        err += "the asset is no longer in the project";
        return false;
    }
    return open(project, *asset, err);
}

bool source_editable(const ProjectAsset& asset) {
    const AssetKind* kind = asset_kind_for_extension(asset.path.view());
    return kind != nullptr;
}

}  // namespace mce::editor
