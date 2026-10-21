#include "mce/editor/asset_ops.h"

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/editor/asset_form.h"

namespace mce::editor {
namespace {

constexpr u64 kMaxScanBytes = 8u << 20;  // references_to skips files larger than this

cook::String sidecar_of(const EditorProject& project, StringView rel) {
    cook::String p = project.asset_path(rel);
    p += ".mceasset";
    return p;
}

// `text` with every occurrence of `from` replaced by `to`.
cook::String replace_all(StringView text, StringView from, StringView to) {
    cook::String out;
    usize at = 0;
    while (true) {
        const usize hit = text.find(from, at);
        if (hit == StringView::npos) break;
        out += text.substr(at, hit - at);
        out += to;
        at = hit + from.size();
    }
    out += text.substr(at);
    return out;
}

// The starter document of `kind`: format, version and a fresh guid first, then the template's members, indented.
bool build_starter(const AssetKind& kind, asset::Guid guid, cook::String& out, cook::String& err) {
    cook::JsonDoc tmpl;
    cook::JsonError je;
    if (!tmpl.parse(kind.tmpl, je) || !tmpl.root().is_object()) {
        err += "the starter template of that kind is not an object";
        return false;
    }
    cook::String text("{\"format\":\"");
    text += kind.format;
    cook::appendf(text, "\",\"version\":%u,\"guid\":\"", kind.version);
    text += asset::to_string(guid).view();
    text += '"';
    cook::String body = kind.tmpl;
    const usize open = body.view().find('{');
    const usize close = body.view().rfind('}');
    const StringView inner = body.view().substr(open + 1, close - open - 1);
    bool has_members = false;
    for (const char c : inner)
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r') has_members = true;
    if (has_members) {
        text += ',';
        text += inner;
    }
    // A tilemap starts with every cell empty (the importer wants width x height tiles).
    if (StringView(kind.ext) == ".mctilemap" && !tmpl.root().get("tiles").valid()) {
        const cook::JsonValue w = tmpl.root().get("width"), h = tmpl.root().get("height");
        if (w.is_integer() && h.is_integer() && w.as_number() >= 1 && h.as_number() >= 1) {
            text += ",\"tiles\":[";
            const u64 n = static_cast<u64>(w.as_number()) * static_cast<u64>(h.as_number());
            for (u64 i = 0; i < n; ++i) text += i == 0 ? "0" : ",0";
            text += ']';
        }
    }
    text += '}';
    cook::JsonDoc doc;
    if (!doc.parse(text.view(), je)) {
        err += "could not build the starter document";
        return false;
    }
    cook::write_json(doc.root(), out, false);
    out += '\n';
    return true;
}

}  // namespace

bool asset_path_valid(StringView rel) {
    if (rel.empty() || rel.size() >= 400 || rel[0] == '/' || rel.find('\\') != StringView::npos ||
        rel.find(':') != StringView::npos)
        return false;
    usize at = 0;
    while (at <= rel.size()) {
        usize next = rel.find('/', at);
        if (next == StringView::npos) next = rel.size();
        const StringView seg = rel.substr(at, next - at);
        if (seg.empty() || seg == "." || seg == "..") return false;
        at = next + 1;
    }
    const usize dot = rel.rfind('.');
    const usize slash = rel.rfind('/');
    return dot != StringView::npos && dot + 1 < rel.size() && (slash == StringView::npos || dot > slash + 1);
}

bool create_asset(EditorProject& project, StringView rel, asset::Guid& out, cook::String& err) {
    if (!project.is_open()) {
        err += "open a project first";
        return false;
    }
    if (!asset_path_valid(rel)) {
        err += "the path must be like effects/dust.mcpart: forward slashes, no '..'";
        return false;
    }
    const AssetKind* kind = asset_kind_for_extension(rel);
    if (!kind) {
        err += "no template for that extension: ";
        for (const AssetKind& k : asset_kinds()) {
            err += k.ext;
            err += ' ';
        }
        return false;
    }
    if (cook::fs::exists(project.asset_path(rel).view())) {
        err += "a file already exists there";
        return false;
    }
    const asset::Guid guid = cook::generate_guid();
    cook::String text;
    if (!build_starter(*kind, guid, text, err)) return false;
    const cook::String abs = project.asset_path(rel);
    const usize slash = abs.view().rfind('/');
    if (slash != StringView::npos && !cook::fs::create_directories(abs.view().substr(0, slash))) {
        err += "could not create the folder";
        return false;
    }
    ProjectAsset fresh;
    fresh.guid = guid;
    fresh.path = cook::String(rel);
    if (!validate_and_save_source(project, fresh, text.view(), err)) return false;
    out = guid;
    return true;
}

bool duplicate_asset(EditorProject& project, asset::Guid source, StringView new_rel, asset::Guid& out, cook::String& err) {
    const ProjectAsset* a = project.find(source);
    if (!a) {
        err += "no such asset";
        return false;
    }
    const cook::String old_rel = a->path;  // the catalog is rescanned below
    const asset::ChunkType type = a->type;
    if (!asset_path_valid(new_rel)) {
        err += "the path must be like effects/dust2.mcpart: forward slashes, no '..'";
        return false;
    }
    if (!(cook::lower_extension(new_rel) == cook::lower_extension(old_rel.view()))) {
        err += "a copy keeps the extension of its source";
        return false;
    }
    if (cook::fs::exists(project.asset_path(new_rel).view())) {
        err += "a file already exists there";
        return false;
    }
    cook::String text;
    if (!cook::fs::read_text(project.asset_path(old_rel.view()).view(), text)) {
        err += "could not read the source file";
        return false;
    }
    const asset::Guid guid = cook::generate_guid();
    const asset::GuidString old_id = asset::to_string(source), new_id = asset::to_string(guid);
    const cook::String copy = replace_all(text.view(), old_id.view(), new_id.view());
    cook::String side;
    const bool has_sidecar = cook::fs::read_text(sidecar_of(project, old_rel.view()).view(), side);
    if (!has_sidecar) {
        // A native document: cooked in memory first, written only when the cook accepts it.
        ProjectAsset fresh;
        fresh.guid = guid;
        fresh.path = cook::String(new_rel);
        fresh.type = type;
        const cook::String abs = project.asset_path(new_rel);
        const usize slash = abs.view().rfind('/');
        if (slash != StringView::npos && !cook::fs::create_directories(abs.view().substr(0, slash))) {
            err += "could not create the folder";
            return false;
        }
        if (!validate_and_save_source(project, fresh, copy.view(), err)) return false;
        out = guid;
        return true;
    }
    // An interchange source: the bytes as they are, the sidecar under the new GUID.
    const cook::String abs = project.asset_path(new_rel);
    if (!cook::fs::write_file_atomic(abs.view(), Span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size()))) {
        err += "could not write the file";
        return false;
    }
    cook::String new_side = replace_all(side.view(), old_id.view(), new_id.view());
    if (!cook::fs::write_file_atomic(sidecar_of(project, new_rel).view(),
                                     Span<const byte>(reinterpret_cast<const byte*>(new_side.data()), new_side.size()))) {
        cook::fs::remove(abs.view());
        err += "could not write the sidecar";
        return false;
    }
    project.rescan();
    out = guid;
    return true;
}

bool delete_asset(EditorProject& project, asset::Guid guid, cook::String& err) {
    const ProjectAsset* a = project.find(guid);
    if (!a) {
        err += "no such asset";
        return false;
    }
    const cook::String rel = a->path;
    if (!cook::fs::remove(project.asset_path(rel.view()).view())) {
        err += "could not delete the file";
        return false;
    }
    cook::fs::remove(sidecar_of(project, rel.view()).view());
    project.rescan();
    return true;
}

void references_to(const EditorProject& project, asset::Guid guid, Array<cook::String>& out) {
    const asset::GuidString id = asset::to_string(guid);
    for (const ProjectAsset& a : project.assets()) {
        if (a.guid == guid) continue;
        const cook::String abs = project.asset_path(a.path.view());
        u64 size = 0;
        i64 mtime = 0;
        if (!cook::fs::stat(abs.view(), size, mtime) || size > kMaxScanBytes) continue;
        cook::String text;
        if (cook::fs::read_text(abs.view(), text) && text.view().find(id.view()) != StringView::npos) out.push_back(a.path);
    }
}

}  // namespace mce::editor
