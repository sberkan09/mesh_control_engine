// Asset thumbnail tool (M-THUMBS step 2): asset_thumbnail.
//
// One asset (by 'path' or 'guid') comes back as a size x size PNG; a 'folder' comes back as a contact sheet of
// its assets in catalog order. Both use editor::render_thumbnail, a CPU drawing of the asset's own bytes (no
// GPU, no cook needed for textures and audio; meshes need a cook). Read-only.

#include <cmath>

#include "agent_image.h"
#include "mce/editor/asset_thumb.h"
#include "mce/editor/project.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {
namespace {

constexpr u64 kMaxTiles = 64;
constexpr u32 kGap = 2;

void add_png(ToolCall& c, const cook::Image& img) {
    if (!c.content) return;
    ToolContent item;
    item.mime = cook::String("image/png");
    if (png_base64(img, item.base64)) c.content->push_back(static_cast<ToolContent&&>(item));
}

bool asset_thumbnail_tool(ToolCall& c) {
    if (!need_project(c)) return false;
    StringView path, guid_text, type;
    if (!arg_string(c, "path", path, false) || !arg_string(c, "guid", guid_text, false) || !arg_string(c, "type", type, false)) return false;
    const cook::JsonValue folder_arg = arg(c, "folder");
    const bool sheet = folder_arg.valid();
    if (sheet == (!path.empty() || !guid_text.empty())) return fail(c, "give one of 'path' or 'guid' (one asset), or 'folder' (a contact sheet of the assets under it, \"\" for all)");
    u64 size = sheet ? 64 : 128;
    if (!arg_uint(c, "size", size, editor::kThumbMin, editor::kThumbMax, false)) return false;
    editor::EditorProject& project = c.server.session().project();

    if (!sheet) {
        const editor::ProjectAsset* a = nullptr;
        if (!asset_tool_arg(c, a)) return false;
        cook::Image img;
        const bool real = editor::render_thumbnail(project, *a, static_cast<u32>(size), img);
        add_png(c, img);
        c.out.begin_object().key("path").str(a->path.view()).key("type").str(editor::asset_type_name(a->type));
        c.out.key("size").num_u(img.width).key("real").boolean(real).end_object();
        return true;
    }

    if (!folder_arg.is_string()) return arg_missing(c, "folder", "a path prefix under assets/ (\"\" for every asset)");
    const StringView folder = folder_arg.as_string();
    u64 limit = kMaxTiles;
    if (!arg_uint(c, "limit", limit, 1, kMaxTiles, false)) return false;
    Array<const editor::ProjectAsset*> picked;
    bool truncated = false;
    for (const editor::ProjectAsset& a : project.assets()) {
        if (!folder.empty() && a.path.view().substr(0, folder.size()) != folder) continue;
        if (!type.empty() && type != editor::asset_type_name(a.type)) continue;
        if (picked.size() >= limit) {
            truncated = true;
            break;
        }
        picked.push_back(&a);
    }
    if (picked.empty()) return fail(c, "no registered asset under that folder (asset_list lists them)");
    const u32 n = static_cast<u32>(picked.size());
    u32 cols = static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(n))));
    const u32 rows = (n + cols - 1) / cols;
    const u32 s = static_cast<u32>(size);
    cook::Image sheet_img = make_image(kGap + cols * (s + kGap), kGap + rows * (s + kGap), 0x202020FFu);
    c.out.begin_object().key("tile").num_u(s).key("columns").num_u(cols).key("rows").num_u(rows);
    c.out.key("truncated").boolean(truncated).key("assets").begin_array();
    for (u32 i = 0; i < n; ++i) {
        const editor::ProjectAsset& a = *picked[i];
        cook::Image tile;
        const bool real = editor::render_thumbnail(project, a, s, tile);
        const u32 col = i % cols, row = i / cols;
        blit(sheet_img, tile, static_cast<i32>(kGap + col * (s + kGap)), static_cast<i32>(kGap + row * (s + kGap)));
        c.out.begin_object().key("index").num_u(i).key("column").num_u(col).key("row").num_u(row);
        c.out.key("path").str(a.path.view()).key("type").str(editor::asset_type_name(a.type)).key("real").boolean(real).end_object();
    }
    c.out.end_array().end_object();
    add_png(c, sheet_img);
    return true;
}

const ToolDef kThumbTools[] = {
    {"asset_thumbnail",
     "A picture of an asset, drawn on the CPU from its own bytes (no GPU, no cook for textures and audio): a texture "
     "scaled to fit over a checker, an audio clip as its waveform, a mesh as a flat-shaded three-quarter view (cook "
     "first), a material as a lit sphere (base colour, metallic, roughness, emissive), any other asset a plain tile coloured by type. Give 'path' or 'guid' for one PNG of 'size' x 'size' "
     "(16..512, default 128), or 'folder' (a path prefix under assets/, \"\" for all) for a contact sheet PNG of the "
     "assets under it in catalog order, row by row (tile 'size' default 64, 'limit' up to 64 tiles, optional 'type' "
     "such as Texture or Mesh); the JSON lists each tile's index, column, row and whether it shows the asset itself "
     "('real') or the plain tile. Read-only.",
     R"j({"type":"object","properties":{"path":{"type":"string"},"guid":{"type":"string"},"folder":{"type":"string"},)j"
     R"j("type":{"type":"string"},"size":{"type":"integer","minimum":16,"maximum":512},)j"
     R"j("limit":{"type":"integer","minimum":1,"maximum":64}}})j",
     asset_thumbnail_tool},
};

}  // namespace

Span<const ToolDef> thumb_tool_table() { return Span<const ToolDef>(kThumbTools, sizeof(kThumbTools) / sizeof(kThumbTools[0])); }

}  // namespace mce::mcp
