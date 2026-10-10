#include "mce/cook/mod_project.h"

#include "mce/asset/file_io.h"
#include "mce/asset/formats.h"
#include "mce/asset/mods.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.mod";

bool is_absolute(StringView p) { return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':')); }

}  // namespace

bool is_mod_project(StringView project_root) {
    const String p = path_join(project_root, "mod.json");
    return fs::exists(p.view()) && !fs::is_directory(p.view());
}

bool read_mod_project(StringView project_root, ModProject& out, DiagList& diags) {
    out = ModProject{};
    const String path = path_join(project_root, "mod.json");
    const u32 errors_before = diags.error_count();
    String text;
    if (!fs::read_text(path.view(), text)) {
        diags.error(Errc::FileNotFound, path.view(), 0, 0, kStage, "cannot read mod.json");
        return false;
    }
    JsonDoc doc;
    JsonError je;
    if (!doc.parse(text.view(), je)) {
        diags.error(Errc::ParseError, path.view(), je.line, je.col, kStage, je.message.view());
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.is_object()) {
        diags.error(Errc::ParseError, path.view(), root.line(), root.col(), kStage, "mod.json must be an object");
        return false;
    }
    auto fail = [&](JsonValue at, const char* msg) {
        diags.error(Errc::InvalidArgument, path.view(), at.valid() ? at.line() : root.line(), at.valid() ? at.col() : root.col(), kStage, msg);
    };
    const JsonValue format = root.get("format");
    if (!format.is_string() || format.as_string() != "mce-mod") fail(format, "\"format\" must be \"mce-mod\"");
    const JsonValue version = root.get("version");
    if (!version.is_number() || !version.is_integer() || version.as_number() != 1) fail(version, "\"version\" must be 1");

    auto text_field = [&](const char* key, String& dst, bool required) {
        const JsonValue v = root.get(key);
        if (!v.valid()) {
            if (required) {
                String msg;
                appendf(msg, "\"%s\" is required", key);
                diags.error(Errc::InvalidArgument, path.view(), root.line(), root.col(), kStage, msg.view());
            }
            return;
        }
        if (!v.is_string()) {
            String msg;
            appendf(msg, "\"%s\" must be a string", key);
            diags.error(Errc::InvalidArgument, path.view(), v.line(), v.col(), kStage, msg.view());
            return;
        }
        dst = String(v.as_string());
    };
    text_field("id", out.id, true);
    text_field("name", out.name, false);
    text_field("mod_version", out.version, false);
    text_field("game", out.game, false);
    text_field("description", out.description, false);
    if (!out.id.empty() && !asset::is_valid_mod_id(out.id.view()))
        fail(root.get("id"), "\"id\" must be 1..64 of [a-z0-9_.-], not starting with '.'");
    if (out.name.empty()) out.name = out.id;
    if (out.name.size() > 128 || out.version.size() > 32 || out.game.size() > 64 || out.description.size() > 4096)
        fail(root, "mod.json text too long (name 128, mod_version 32, game 64, description 4096 bytes)");

    for (u32 i = 0; i < root.size(); ++i) {
        const StringView k = root.key_at(i);
        if (k != "format" && k != "version" && k != "id" && k != "name" && k != "mod_version" && k != "game" && k != "description" &&
            k != "base") {
            String msg;
            appendf(msg, "unknown key \"%.*s\"", static_cast<int>(k.size()), k.data());
            diags.warning(path.view(), root.at(i).line(), root.at(i).col(), kStage, msg.view());
        }
    }

    auto add_base = [&](JsonValue v) {
        if (!v.is_string() || v.as_string().empty()) {
            fail(v, "\"base\" entries must be non-empty paths");
            return;
        }
        const StringView p = v.as_string();
        out.base.push_back(is_absolute(p) ? String(p) : path_join(project_root, p));
    };
    const JsonValue base = root.get("base");
    if (base.is_string()) add_base(base);
    else if (base.is_array())
        for (u32 i = 0; i < base.size(); ++i) add_base(base.at(i));
    else if (base.valid())
        fail(base, "\"base\" must be a path or an array of paths");
    return diags.error_count() == errors_before;
}

bool read_base_assets(Span<const String> bases, HashMap<asset::Guid, asset::ChunkType>& out, DiagList& diags) {
    bool ok = true;
    for (const String& b : bases) {
        const bool pack = b.size() >= 4 && b.view().substr(b.size() - 4) == ".mce";
        const String file = pack ? b : path_join(b.view(), "manifest.mce");
        Array<byte> bytes;
        Result<void> read = asset::read_file(file.view(), bytes);
        Result<asset::ContainerView> view = read ? asset::ContainerView::open(bytes.span(), file.view())
                                                 : Result<asset::ContainerView>(read.error());
        const u32 mi = view ? view.value().find(asset::ChunkType::Manifest) : 0;
        Result<asset::ManifestView> manifest = !view ? Result<asset::ManifestView>(view.error())
                                               : mi == view.value().chunk_count()
                                                   ? Result<asset::ManifestView>(Error(Errc::MissingRef, kStage, file.view()))
                                                   : asset::ManifestView::read(view.value().payload(mi));
        if (!manifest) {
            String msg;
            appendf(msg, "cannot read the base pack (%s): build the game first, or fix \"base\" in mod.json",
                    errc_name(manifest.error().code));
            diags.error(manifest.error().code, file.view(), 0, 0, kStage, msg.view());
            ok = false;
            continue;
        }
        for (u32 i = 0; i < manifest.value().size(); ++i) {
            const asset::ManifestEntry& e = manifest.value().entry(i);
            if (!out.contains(e.guid())) out.insert_or_assign(e.guid(), e.type);
        }
    }
    return ok;
}

}  // namespace mce::cook
