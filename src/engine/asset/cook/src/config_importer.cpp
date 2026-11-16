// .mcconfig -> CHNK_RCFG: a project's remote config defaults (M-LIVEOPS step 3; mce/liveops/remote_config.h
// has the format). The chunk is the one liveops::decode_config reads, decoded again before it is stored.
//
// {
//   "format": "mce-config", "version": 1, "guid": "<uuid>",
//   "values": { "ad_every_levels": 3, "gem_bonus": 1.5, "winter_event": false, "motd": "Welcome",
//               "drop_rate": { "type": "float", "value": 1 } }
// }
//
// A value's type is its JSON's (true/false: bool, a whole number: int, another number: float, text:
// string) unless the { "type": "int|float|bool|string", "value": ... } form names it. Keys are 1-63
// letters, digits, '_', '-' and '.'; strings are at most 255 bytes.

#include <cmath>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/liveops/remote_config.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.config";
constexpr u32 kConfigFormatVersion = 1;
constexpr f64 kMaxExactInt = 9007199254740992.0;  // 2^53: larger whole numbers lose digits in JSON

void error(DiagList& diags, StringView file, JsonValue at, StringView msg) {
    detail::error_at(diags, Errc::ParseError, file, at, kStage, msg);
}

bool valid_key(StringView k) {
    if (k.empty() || k.size() > 63) return false;
    for (const char c : k) {
        const bool okc = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                         c == '-' || c == '.';
        if (!okc) return false;
    }
    return true;
}

// Fills `v` from `val` as `type` (or the JSON's own type when `type` is empty); false with an error.
bool read_value(JsonValue val, StringView type, liveops::ConfigValue& v, StringView file, DiagList& diags) {
    StringView t = type;
    if (t.empty()) {
        if (val.is_bool())
            t = "bool";
        else if (val.is_string())
            t = "string";
        else if (val.is_number()) {
            const f64 n = val.as_number();
            if (std::floor(n) == n && std::fabs(n) > kMaxExactInt) {
                error(diags, file, val,
                      "a whole number beyond +-2^53 loses digits; use { \"type\": \"float\", \"value\": ... } for a "
                      "float");
                return false;
            }
            t = val.is_integer() ? "int" : "float";
        } else {
            error(diags, file, val, "a value must be true/false, a number, text or {\"type\": ..., \"value\": ...}");
            return false;
        }
    }
    if (t == "bool") {
        if (!val.is_bool()) return error(diags, file, val, "this value is a bool: true or false"), false;
        v.type = liveops::ConfigType::Bool;
        v.i = val.as_bool() ? 1 : 0;
    } else if (t == "int") {
        if (!val.is_number() || !val.is_integer() || std::fabs(val.as_number()) > kMaxExactInt)
            return error(diags, file, val, "this value is an int: a whole number within +-2^53"), false;
        v.type = liveops::ConfigType::Int;
        v.i = static_cast<i64>(val.as_number());
    } else if (t == "float") {
        if (!val.is_number()) return error(diags, file, val, "this value is a float: a number"), false;
        v.type = liveops::ConfigType::Float;
        v.f = val.as_number();
    } else if (t == "string") {
        if (!val.is_string()) return error(diags, file, val, "this value is a string: text"), false;
        v.type = liveops::ConfigType::String;
        v.s = val.as_string();
        if (v.s.truncated()) return error(diags, file, val, "a string value is at most 255 bytes"), false;
    } else {
        return false;  // the caller reported the unknown type
    }
    return true;
}

class ConfigImporter final : public IImporter {
public:
    StringView id() const override { return "config"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::RemoteConfig; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcconfig"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "values"}, file, kStage, diags)) return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-config") {
            error(diags, file, fmt.valid() ? fmt : doc, "\"format\" must be \"mce-config\"");
            return false;
        }
        if (!detail::check_format_version(doc, kConfigFormatVersion, "remote config", file, kStage, diags)) return false;
        const JsonValue values = doc.get("values");
        if (values.valid() && !values.is_object()) {
            error(diags, file, values, "'values' must be an object of key: value");
            return false;
        }
        liveops::ConfigValues config;
        for (u32 i = 0; values.valid() && i < values.size(); ++i) {
            const StringView key = values.key_at(i);
            JsonValue val = values.at(i);
            if (!valid_key(key)) {
                error(diags, file, val, "a key is 1-63 letters, digits, '_', '-' and '.'");
                continue;
            }
            StringView type;
            if (val.is_object()) {
                if (!detail::check_keys(val, {"type", "value"}, file, kStage, diags)) continue;
                const JsonValue t = val.get("type");
                if (!t.is_string() || (t.as_string() != "int" && t.as_string() != "float" && t.as_string() != "bool" &&
                                       t.as_string() != "string")) {
                    error(diags, file, t.valid() ? t : val, "\"type\" must be int, float, bool or string");
                    continue;
                }
                type = t.as_string();
                val = val.get("value");
                if (!val.valid()) {
                    error(diags, file, values.at(i), "the { \"type\", \"value\" } form needs a \"value\"");
                    continue;
                }
            }
            liveops::ConfigValue v;
            v.key = key;
            if (!read_value(val, type, v, file, diags)) continue;
            if (!config.add(v)) error(diags, file, values.at(i), "two values share this key");
        }
        if (diags.error_count() != errors_before) return false;

        Array<byte> bytes;
        liveops::encode_config(config, bytes);
        liveops::ConfigValues check;
        if (Result<void> r = liveops::decode_config(bytes.span(), check); !r) {
            FixedString<512> why;
            format_error(r.error(), why);
            String msg("the remote config is not loadable: ");
            msg += why.view();
            error(diags, file, doc, msg);
            return false;
        }

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::RemoteConfig;
        d.version = asset::chunk_version(asset::REMOTE_CONFIG_VERSION, 0);
        d.guid = in.guid;
        out.add(d, bytes.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_config_importer() { return mem::make_unique<ConfigImporter>(mem::heap()); }

}  // namespace mce::cook
