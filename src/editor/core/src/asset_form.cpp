#include "mce/editor/asset_form.h"

#include <cmath>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"

namespace mce::editor {
namespace {

constexpr const char* kSlotKeys[MaterialForm::kSlots] = {"base_color", "normal", "metallic_roughness", "emissive", "height"};
constexpr const char* kAlphaModes[] = {"opaque", "mask", "blend"};

void put_number(cook::String& out, f32 v) { cook::appendf(out, "%.6g", static_cast<double>(v)); }

void put_array(cook::String& out, const f32* v, u32 n) {
    out += '[';
    for (u32 i = 0; i < n; ++i) {
        if (i) out += ", ";
        put_number(out, v[i]);
    }
    out += ']';
}

bool same(const f32* a, const f32* b, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

// One member the form owns: its JSON text, and whether it is the default (so a file that lacks it stays without it).
struct Owned {
    const char* key;
    cook::String text;
    bool is_default;
};

void owned_members(const MaterialForm& f, Array<Owned>& out) {
    const MaterialForm d;
    auto add = [&](const char* key, cook::String text, bool is_default) { out.push_back(Owned{key, static_cast<cook::String&&>(text), is_default}); };
    auto num = [&](const char* key, f32 v, f32 def) {
        cook::String t;
        put_number(t, v);
        add(key, static_cast<cook::String&&>(t), v == def);
    };
    auto arr = [&](const char* key, const f32* v, const f32* def, u32 n) {
        cook::String t;
        put_array(t, v, n);
        add(key, static_cast<cook::String&&>(t), same(v, def, n));
    };
    arr("base_color", f.base_color, d.base_color, 4);
    num("metallic", f.metallic, d.metallic);
    num("roughness", f.roughness, d.roughness);
    arr("emissive", f.emissive, d.emissive, 3);
    arr("uv_scale", f.uv_scale, d.uv_scale, 2);
    {
        cook::String t;
        cook::append_json_string(t, kAlphaModes[f.alpha_mode < 0 || f.alpha_mode > 2 ? 0 : f.alpha_mode]);
        add("alpha_mode", static_cast<cook::String&&>(t), f.alpha_mode == d.alpha_mode);
    }
    num("alpha_cutoff", f.alpha_cutoff, d.alpha_cutoff);
    add("double_sided", cook::String(f.double_sided ? "true" : "false"), f.double_sided == d.double_sided);
    add("unlit", cook::String(f.unlit ? "true" : "false"), f.unlit == d.unlit);
    num("clearcoat", f.clearcoat, d.clearcoat);
    num("clearcoat_roughness", f.clearcoat_roughness, d.clearcoat_roughness);
    num("transmission", f.transmission, d.transmission);
    num("ior", f.ior, d.ior);
    num("height_scale", f.height_scale, d.height_scale);
    cook::String tex("{");
    bool any = false;
    for (u32 i = 0; i < MaterialForm::kSlots; ++i) {
        if (f.texture[i].empty()) continue;
        tex += any ? ", \"" : " \"";
        any = true;
        tex += kSlotKeys[i];
        tex += "\": ";
        cook::append_json_string(tex, f.texture[i].view());
    }
    tex += any ? " }" : "}";
    add("textures", static_cast<cook::String&&>(tex), !any);
}

}  // namespace

const char* MaterialForm::slot_name(u32 slot) { return slot < kSlots ? kSlotKeys[slot] : ""; }

MaterialSwatch MaterialForm::swatch() const {
    MaterialSwatch s;
    for (u32 i = 0; i < 4; ++i) s.base[i] = base_color[i];
    for (u32 i = 0; i < 3; ++i) s.emissive[i] = emissive[i];
    s.metallic = metallic;
    s.roughness = roughness;
    s.blend = alpha_mode == 2;
    s.unlit = unlit;
    return s;
}

bool material_form_from_text(StringView text, MaterialForm& out, cook::String& err) {
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(text, je) || !doc.root().is_object()) {
        err += "the material file is not a JSON object";
        return false;
    }
    const cook::JsonValue root = doc.root();
    const cook::JsonValue fmt = root.get("format");
    if (!fmt.valid() || !fmt.is_string() || fmt.as_string() != "mce-material") {
        err += "the file is not an mce-material document";
        return false;
    }
    MaterialForm f;
    auto number = [&](const char* key, f32& v) {
        const cook::JsonValue n = root.get(key);
        if (n.valid() && n.is_number()) v = static_cast<f32>(n.as_number());
    };
    auto vec = [&](const char* key, f32* v, u32 n) {
        const cook::JsonValue a = root.get(key);
        if (!a.valid() || !a.is_array() || a.size() != n) return;
        for (u32 i = 0; i < n; ++i)
            if (a.at(i).is_number()) v[i] = static_cast<f32>(a.at(i).as_number());
    };
    auto flag = [&](const char* key, bool& v) {
        const cook::JsonValue b = root.get(key);
        if (b.valid() && b.is_bool()) v = b.as_bool();
    };
    vec("base_color", f.base_color, 4);
    number("metallic", f.metallic);
    number("roughness", f.roughness);
    vec("emissive", f.emissive, 3);
    vec("uv_scale", f.uv_scale, 2);
    number("alpha_cutoff", f.alpha_cutoff);
    flag("double_sided", f.double_sided);
    flag("unlit", f.unlit);
    number("clearcoat", f.clearcoat);
    number("clearcoat_roughness", f.clearcoat_roughness);
    number("transmission", f.transmission);
    number("ior", f.ior);
    number("height_scale", f.height_scale);
    if (const cook::JsonValue m = root.get("alpha_mode"); m.valid() && m.is_string())
        for (int i = 0; i < 3; ++i)
            if (m.as_string() == kAlphaModes[i]) f.alpha_mode = i;
    if (const cook::JsonValue t = root.get("textures"); t.valid() && t.is_object())
        for (u32 i = 0; i < MaterialForm::kSlots; ++i)
            if (const cook::JsonValue s = t.get(kSlotKeys[i]); s.valid() && s.is_string()) f.texture[i] = cook::String(s.as_string());
    out = static_cast<MaterialForm&&>(f);
    return true;
}

bool material_form_to_text(StringView old_text, const MaterialForm& form, cook::String& out, cook::String& err) {
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(old_text, je) || !doc.root().is_object()) {
        err += "the material file is not a JSON object";
        return false;
    }
    Array<Owned> owned;
    owned_members(form, owned);
    const cook::JsonValue root = doc.root();
    cook::String text("{\n");
    bool first = true;
    auto member = [&](StringView key) {
        text += first ? "  " : ",\n  ";
        first = false;
        cook::append_json_string(text, key);
        text += ": ";
    };
    for (u32 i = 0; i < root.size(); ++i) {
        const StringView key = root.key_at(i);
        const Owned* mine = nullptr;
        for (const Owned& o : owned)
            if (key == o.key) mine = &o;
        member(key);
        if (mine) text += mine->text.view();
        else cook::write_json(root.at(i), text, true);
    }
    for (const Owned& o : owned) {
        if (root.get(o.key).valid() || o.is_default) continue;
        member(o.key);
        text += o.text.view();
    }
    text += "\n}\n";
    out = static_cast<cook::String&&>(text);
    return true;
}

bool load_material_form(const EditorProject& project, const ProjectAsset& asset, MaterialForm& out, cook::String& err) {
    if (asset.type != asset::ChunkType::Material) {
        err += "the asset is not a material";
        return false;
    }
    cook::String text;
    if (!cook::fs::read_text(project.asset_path(asset.path.view()).view(), text)) {
        err += "could not read the material file";
        return false;
    }
    return material_form_from_text(text.view(), out, err);
}

bool validate_and_save_source(EditorProject& project, const ProjectAsset& asset, StringView text, cook::String& err) {
    const StringView rel = asset.path.view();
    const usize dot = rel.rfind('.');
    const cook::IImporter* importer = dot == StringView::npos ? nullptr : project.importers().for_extension(rel.substr(dot));
    if (!importer) {
        err += "no importer reads that file type";
        return false;
    }
    cook::AssetRegistry registry;
    cook::DiagList scan;
    registry.scan(project.root(), project.importers(), false, scan);
    cook::DiagList diags;
    Array<byte> cooked;
    if (!cook::cook_document(registry, *importer, asset.guid, rel, text, cooked, diags)) {
        err += "the cook refuses this document (nothing was written):";
        usize n = 0;
        for (const cook::Diagnostic& d : diags.all()) {
            if (d.severity != cook::Severity::Error || n++ == 5) continue;
            err += '\n';
            cook::format_diagnostic(d, err);
        }
        return false;
    }
    if (!cook::fs::write_file_atomic(project.asset_path(rel).view(), Span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size()))) {
        err += "could not write the file";
        return false;
    }
    project.rescan();
    return true;
}

bool save_material_form(EditorProject& project, const ProjectAsset& asset, const MaterialForm& form, cook::String& err) {
    if (asset.type != asset::ChunkType::Material) {
        err += "the asset is not a material";
        return false;
    }
    cook::String old_text;
    if (!cook::fs::read_text(project.asset_path(asset.path.view()).view(), old_text)) {
        err += "could not read the material file";
        return false;
    }
    cook::String text;
    if (!material_form_to_text(old_text.view(), form, text, err)) return false;
    return validate_and_save_source(project, asset, text.view(), err);
}

}  // namespace mce::editor
