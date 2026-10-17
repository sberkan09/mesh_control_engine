// .mcdefaults -> CHNK_CVARS: a project's own convar defaults (M-WORKFLOW 7). The chunk is the "name value" lines
// console::set_defaults reads, so a shipped pack carries them to every platform.
//
// {
//   "format": "mce-defaults", "version": 1, "guid": "<uuid>",
//   "convars": { "ui.reference_height": 720, "access.captions": true, "audio.music_db": -6 }
// }
//
// A value is true/false, a number or text. Names are 1-63 letters, digits, '_', '-' and '.'. Whether a convar exists is
// the player's to say (a game module's convars are not known to the cook): an unknown name is warned about there.

#include <cmath>
#include <cstdio>

#include "importer_common.h"
#include "mce/asset/formats.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.defaults";
constexpr u32 kDefaultsFormatVersion = 1;
constexpr usize kMaxValueBytes = 900;

void error(DiagList& diags, StringView file, JsonValue at, StringView msg) {
    detail::error_at(diags, Errc::ParseError, file, at, kStage, msg);
}

bool valid_name(StringView k) {
    if (k.empty() || k.size() > 63) return false;
    for (const char c : k) {
        const bool okc = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                         c == '-' || c == '.';
        if (!okc) return false;
    }
    return true;
}

class ConvarDefaultsImporter final : public IImporter {
public:
    StringView id() const override { return "convar_defaults"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::ConvarDefaults; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcdefaults"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "convars"}, file, kStage, diags)) return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-defaults") {
            error(diags, file, fmt.valid() ? fmt : doc, "\"format\" must be \"mce-defaults\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kDefaultsFormatVersion) {
            String msg;
            appendf(msg, "unsupported convar defaults version (this cook reads version %u)", kDefaultsFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }
        const JsonValue convars = doc.get("convars");
        if (convars.valid() && !convars.is_object()) {
            error(diags, file, convars, "'convars' must be an object of name: value");
            return false;
        }
        String text;
        for (u32 i = 0; convars.valid() && i < convars.size(); ++i) {
            const StringView name = convars.key_at(i);
            const JsonValue val = convars.at(i);
            if (!valid_name(name)) {
                error(diags, file, val, "a convar name is 1-63 letters, digits, '_', '-' and '.'");
                continue;
            }
            for (u32 j = 0; j < i; ++j)
                if (convars.key_at(j) == name) {
                    error(diags, file, val, "two values share this convar name");
                    break;
                }
            String line;
            line += name;
            line += " ";
            if (val.is_bool()) {
                line += val.as_bool() ? "true" : "false";
            } else if (val.is_number()) {
                char buf[40];
                const f64 n = val.as_number();
                if (!std::isfinite(n)) {
                    error(diags, file, val, "a number must be finite");
                    continue;
                }
                if (val.is_integer() && std::fabs(n) < 2147483648.0) std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(n));
                else std::snprintf(buf, sizeof(buf), "%.9g", n);
                line += buf;
            } else if (val.is_string()) {
                const StringView s = val.as_string();
                bool clean = s.size() <= kMaxValueBytes;
                for (const char c : s) clean = clean && static_cast<unsigned char>(c) >= 32;
                if (!clean) {
                    error(diags, file, val, "a text value is at most 900 bytes, on one line");
                    continue;
                }
                line += s;
            } else {
                error(diags, file, val, "a value must be true/false, a number or text");
                continue;
            }
            line += "\n";
            text += line.view();
        }
        if (diags.error_count() != errors_before) return false;

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::ConvarDefaults;
        d.version = asset::chunk_version(asset::CONVAR_DEFAULTS_VERSION, 0);
        d.guid = in.guid;
        out.add(d, Span<const byte>(reinterpret_cast<const byte*>(text.c_str()), text.size()));
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_convar_defaults_importer() { return mem::make_unique<ConvarDefaultsImporter>(mem::heap()); }

}  // namespace mce::cook
