#include "importer_common.h"

namespace mce::cook {

void ImporterRegistry::add(mem::UniquePtr<IImporter> importer) {
    MCE_ASSERT_MSG(importer && !find(importer->id()), "importer missing or registered twice");
    importers_.push_back(std::move(importer));
}

const IImporter* ImporterRegistry::find(StringView id) const {
    for (const auto& i : importers_)
        if (i->id() == id) return i.get();
    return nullptr;
}

const IImporter* ImporterRegistry::for_extension(StringView lower_ext) const {
    for (const auto& i : importers_)
        if (i->handles_extension(lower_ext)) return i.get();
    return nullptr;
}

void ImporterRegistry::add_builtins(ImporterRegistry& reg) {
    reg.add(make_texture_importer());
    reg.add(make_wav_importer());
    reg.add(make_font_importer());
    reg.add(make_material_importer());
    reg.add(make_scene_importer());
    reg.add(make_gltf_importer());
    reg.add(make_prefab_importer());
    reg.add(make_tilemap_importer());
    reg.add(make_particle_importer());
    reg.add(make_sequence_importer());
    reg.add(make_shader_importer());
    reg.add(make_input_map_importer());
    reg.add(make_store_importer());
}

bool parse_guid_ref(StringView text, Guid& out) {
    constexpr StringView kPrefix = "guid:";
    if (text.substr(0, kPrefix.size()) != kPrefix) return false;
    auto g = asset::parse_guid(text.substr(kPrefix.size()));
    if (!g) return false;
    out = g.value();
    return true;
}

namespace detail {

void error_at(DiagList& diags, Errc code, StringView file, JsonValue at, const char* stage, StringView msg) {
    diags.error(code, file, at.valid() ? at.line() : 0, at.valid() ? at.col() : 0, stage, msg);
}

bool check_keys(JsonValue obj, std::initializer_list<StringView> allowed, StringView file, const char* stage, DiagList& diags) {
    if (!obj.valid()) return true;
    if (!obj.is_object()) {
        error_at(diags, Errc::ParseError, file, obj, stage, "expected an object");
        return false;
    }
    bool ok = true;
    for (u32 i = 0; i < obj.size(); ++i) {
        bool known = false;
        for (StringView a : allowed) known = known || a == obj.key_at(i);
        if (!known) {
            String msg("unknown key '");
            msg += obj.key_at(i);
            msg += "'";
            error_at(diags, Errc::ParseError, file, obj.at(i), stage, msg);
            ok = false;
        }
    }
    return ok;
}

bool read_bool(JsonValue obj, StringView key, bool def, bool& out, StringView file, const char* stage, DiagList& diags) {
    out = def;
    const JsonValue v = obj.get(key);
    if (!v.valid()) return true;
    if (!v.is_bool()) {
        String msg("'");
        msg += key;
        msg += "' must be true or false";
        error_at(diags, Errc::ParseError, file, v, stage, msg);
        return false;
    }
    out = v.as_bool();
    return true;
}

bool read_number(JsonValue obj, StringView key, f64 def, f64& out, StringView file, const char* stage, DiagList& diags) {
    out = def;
    const JsonValue v = obj.get(key);
    if (!v.valid()) return true;
    if (!v.is_number()) {
        String msg("'");
        msg += key;
        msg += "' must be a number";
        error_at(diags, Errc::ParseError, file, v, stage, msg);
        return false;
    }
    out = v.as_number();
    return true;
}

}  // namespace detail
}  // namespace mce::cook
