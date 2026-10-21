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

namespace {

// Rewrites `old_text` (a JSON object) with the owned members replaced in place, the rest kept in order; an owned
// member the file lacks is appended only when it is not the default.
bool rewrite_members(StringView old_text, const Array<Owned>& owned, const char* what, cook::String& out, cook::String& err) {
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(old_text, je) || !doc.root().is_object()) {
        err += "the ";
        err += what;
        err += " file is not a JSON object";
        return false;
    }
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

}  // namespace

bool material_form_to_text(StringView old_text, const MaterialForm& form, cook::String& out, cook::String& err) {
    Array<Owned> owned;
    owned_members(form, owned);
    return rewrite_members(old_text, owned, "material", out, err);
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

bool check_source(const EditorProject& project, const ProjectAsset& asset, StringView text, cook::DiagList& diags, cook::String& err) {
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
    Array<byte> cooked;
    bool ok;
    if (importer->native()) {
        ok = cook::cook_document(registry, *importer, asset.guid, rel, text, cooked, diags);
    } else {
        // An interchange source (a strings .csv): cook the text with the settings of its sidecar, as the project cook does.
        cook::String side;
        cook::JsonDoc sidecar;
        cook::JsonError je;
        cook::ImportInput in;
        cook::String side_path = project.asset_path(rel);
        side_path += ".mceasset";
        if (cook::fs::read_text(side_path.view(), side) && sidecar.parse(side.view(), je))
            in.settings = sidecar.root().get("settings");
        cook::String disp = "assets/";
        disp += rel;
        in.guid = asset.guid;
        in.display_path = disp.view();
        in.source_path = rel;
        in.source = Span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size());
        asset::ContainerWriter writer;
        ok = importer->cook(in, writer, diags);
        if (ok) writer.finish(cooked);
    }
    if (!ok) err += "the cook refuses this document";
    return ok;
}

bool validate_and_save_source(EditorProject& project, const ProjectAsset& asset, StringView text, cook::String& err) {
    const StringView rel = asset.path.view();
    cook::DiagList diags;
    cook::String why;
    if (!check_source(project, asset, text, diags, why)) {
        if (diags.all().empty()) {
            err += why;
            return false;
        }
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

// ---- ParticleForm --------------------------------------------------------------------------------------------

ParticleSwatch ParticleForm::swatch() const {
    constexpr f32 kDeg = 0.017453292519943295f;
    ParticleSwatch p;
    p.rate = rate;
    p.burst = burst;
    p.max_particles = max_particles;
    p.duration = duration;
    for (u32 i = 0; i < 2; ++i) {
        p.lifetime[i] = lifetime[i];
        p.speed[i] = speed[i];
        p.gravity[i] = gravity[i];
        p.size[i] = size[i];
        p.area[i] = area[i];
    }
    p.direction = direction * kDeg;
    p.spread = spread * kDeg;
    p.drag = drag;
    for (u32 i = 0; i < 4; ++i) {
        p.color_start[i] = color_start[i];
        p.color_end[i] = color_end[i];
    }
    p.additive = additive;
    return p;
}

bool particle_form_from_text(StringView text, ParticleForm& out, cook::String& err) {
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(text, je) || !doc.root().is_object()) {
        err += "the particle file is not a JSON object";
        return false;
    }
    const cook::JsonValue root = doc.root();
    const cook::JsonValue fmt = root.get("format");
    if (!fmt.valid() || !fmt.is_string() || fmt.as_string() != "mce-particles") {
        err += "the file is not an mce-particles document";
        return false;
    }
    ParticleForm f;
    auto number = [&](const char* key, f32& v) {
        const cook::JsonValue n = root.get(key);
        if (n.valid() && n.is_number()) v = static_cast<f32>(n.as_number());
    };
    auto whole = [&](const char* key, u32& v) {
        const cook::JsonValue n = root.get(key);
        if (n.valid() && n.is_number() && n.as_number() >= 0) v = static_cast<u32>(n.as_number());
    };
    auto vec = [&](const char* key, f32* v, u32 n) {
        const cook::JsonValue a = root.get(key);
        if (!a.valid() || !a.is_array() || a.size() != n) return;
        for (u32 i = 0; i < n; ++i)
            if (a.at(i).is_number()) v[i] = static_cast<f32>(a.at(i).as_number());
    };
    number("rate", f.rate);
    whole("burst", f.burst);
    whole("max_particles", f.max_particles);
    number("duration", f.duration);
    vec("lifetime", f.lifetime, 2);
    vec("speed", f.speed, 2);
    number("direction", f.direction);
    number("spread", f.spread);
    vec("gravity", f.gravity, 2);
    number("drag", f.drag);
    vec("size", f.size, 2);
    vec("spin", f.spin, 2);
    vec("area", f.area, 2);
    vec("color_start", f.color_start, 4);
    vec("color_end", f.color_end, 4);
    if (const cook::JsonValue b = root.get("blend"); b.valid() && b.is_string()) f.additive = b.as_string() == "additive";
    if (const cook::JsonValue t = root.get("texture"); t.valid() && t.is_string()) f.texture = cook::String(t.as_string());
    out = static_cast<ParticleForm&&>(f);
    return true;
}

bool particle_form_to_text(StringView old_text, const ParticleForm& form, cook::String& out, cook::String& err) {
    const ParticleForm d;
    Array<Owned> owned;
    auto add = [&](const char* key, cook::String text, bool is_default) { owned.push_back(Owned{key, static_cast<cook::String&&>(text), is_default}); };
    auto num = [&](const char* key, f32 v, f32 def) {
        cook::String t;
        put_number(t, v);
        add(key, static_cast<cook::String&&>(t), v == def);
    };
    auto count = [&](const char* key, u32 v, u32 def) {
        cook::String t;
        cook::appendf(t, "%u", v);
        add(key, static_cast<cook::String&&>(t), v == def);
    };
    auto arr = [&](const char* key, const f32* v, const f32* def, u32 n) {
        cook::String t;
        put_array(t, v, n);
        add(key, static_cast<cook::String&&>(t), same(v, def, n));
    };
    if (form.texture.empty()) {
        add("texture", cook::String("null"), true);
    } else {
        cook::String t;
        cook::append_json_string(t, form.texture.view());
        add("texture", static_cast<cook::String&&>(t), false);
    }
    num("rate", form.rate, d.rate);
    count("burst", form.burst, d.burst);
    count("max_particles", form.max_particles, d.max_particles);
    num("duration", form.duration, d.duration);
    arr("lifetime", form.lifetime, d.lifetime, 2);
    arr("speed", form.speed, d.speed, 2);
    num("direction", form.direction, d.direction);
    num("spread", form.spread, d.spread);
    arr("gravity", form.gravity, d.gravity, 2);
    num("drag", form.drag, d.drag);
    arr("size", form.size, d.size, 2);
    arr("spin", form.spin, d.spin, 2);
    arr("area", form.area, d.area, 2);
    arr("color_start", form.color_start, d.color_start, 4);
    arr("color_end", form.color_end, d.color_end, 4);
    {
        cook::String t;
        cook::append_json_string(t, form.additive ? "additive" : "alpha");
        add("blend", static_cast<cook::String&&>(t), form.additive == d.additive);
    }
    return rewrite_members(old_text, owned, "particle", out, err);
}

bool load_particle_form(const EditorProject& project, const ProjectAsset& asset, ParticleForm& out, cook::String& err) {
    if (asset.type != asset::ChunkType::ParticlePreset) {
        err += "the asset is not a particle preset";
        return false;
    }
    cook::String text;
    if (!cook::fs::read_text(project.asset_path(asset.path.view()).view(), text)) {
        err += "could not read the particle file";
        return false;
    }
    return particle_form_from_text(text.view(), out, err);
}

bool save_particle_form(EditorProject& project, const ProjectAsset& asset, const ParticleForm& form, cook::String& err) {
    if (asset.type != asset::ChunkType::ParticlePreset) {
        err += "the asset is not a particle preset";
        return false;
    }
    cook::String old_text;
    if (!cook::fs::read_text(project.asset_path(asset.path.view()).view(), old_text)) {
        err += "could not read the particle file";
        return false;
    }
    cook::String text;
    if (!particle_form_to_text(old_text.view(), form, text, err)) return false;
    return validate_and_save_source(project, asset, text.view(), err);
}

// ---- SequenceForm --------------------------------------------------------------------------------------------

namespace {

constexpr const char* kStepKeys[SequenceStepForm::kKinds] = {"wait", "wait_event", "move_to", "anim", "sound",
                                                             "stop_sound", "emit", "call", "spawn", "destroy"};

bool step_has_name(int kind) {
    return kind == SequenceStepForm::WaitEvent || kind == SequenceStepForm::Anim || kind == SequenceStepForm::Emit ||
           kind == SequenceStepForm::Call;
}

void put_position(cook::String& out, const SequenceStepForm& s) { put_array(out, s.pos, s.has_z ? 3 : 2); }

// One step as the object the cook reads: the action key first, then the extras that differ from their defaults.
void put_step(cook::String& out, const SequenceStepForm& s) {
    const SequenceStepForm d;
    out += "{";
    cook::append_json_string(out, kStepKeys[s.kind]);
    out += ": ";
    if (s.kind == SequenceStepForm::Wait) put_number(out, s.seconds);
    else if (step_has_name(s.kind) || s.kind == SequenceStepForm::Sound || s.kind == SequenceStepForm::Spawn)
        cook::append_json_string(out, s.text.view());
    else if (s.kind == SequenceStepForm::MoveTo) put_position(out, s);
    else out += "true";  // stop_sound, destroy
    auto extra_number = [&](const char* key, f32 v, f32 def) {
        if (v == def) return;
        out += ", ";
        cook::append_json_string(out, key);
        out += ": ";
        put_number(out, v);
    };
    auto extra_relative = [&] {
        if (s.relative) out += ", \"relative\": true";
    };
    if (s.kind == SequenceStepForm::MoveTo) {
        extra_number("speed", s.speed, d.speed);
        extra_number("tolerance", s.tolerance, d.tolerance);
        extra_relative();
    } else if (s.kind == SequenceStepForm::Sound) {
        extra_number("gain_db", s.gain_db, d.gain_db);
    } else if (s.kind == SequenceStepForm::Spawn) {
        if (s.pos[0] != 0 || s.pos[1] != 0 || s.pos[2] != 0 || s.has_z) {
            out += ", \"at\": ";
            put_position(out, s);
        }
        extra_relative();
    }
    out += "}";
}

bool read_position(const cook::JsonValue& v, SequenceStepForm& s) {
    if (!v.valid() || !v.is_array() || v.size() < 2 || v.size() > 3) return false;
    for (u32 i = 0; i < v.size(); ++i) {
        if (!v.at(i).is_number()) return false;
        s.pos[i] = static_cast<f32>(v.at(i).as_number());
    }
    s.has_z = v.size() == 3;
    return true;
}

}  // namespace

const char* SequenceStepForm::kind_name(int kind) { return kind >= 0 && kind < kKinds ? kStepKeys[kind] : ""; }

cook::String SequenceForm::describe(const SequenceStepForm& s) {
    cook::String t(SequenceStepForm::kind_name(s.kind));
    switch (s.kind) {
    case SequenceStepForm::Wait: cook::appendf(t, " %g s", static_cast<double>(s.seconds)); break;
    case SequenceStepForm::MoveTo:
        cook::appendf(t, " [%g, %g]%s at %g m/s", static_cast<double>(s.pos[0]), static_cast<double>(s.pos[1]),
                      s.relative ? " (relative)" : "", static_cast<double>(s.speed));
        break;
    case SequenceStepForm::Spawn:
        cook::appendf(t, " %s at [%g, %g]", s.text.c_str(), static_cast<double>(s.pos[0]), static_cast<double>(s.pos[1]));
        break;
    case SequenceStepForm::StopSound:
    case SequenceStepForm::Destroy: break;
    default:
        t += ' ';
        t += s.text.view();
        break;
    }
    return t;
}

bool sequence_form_from_text(StringView text, SequenceForm& out, cook::String& err) {
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(text, je) || !doc.root().is_object()) {
        err += "the sequence file is not a JSON object";
        return false;
    }
    const cook::JsonValue root = doc.root();
    const cook::JsonValue fmt = root.get("format");
    if (!fmt.valid() || !fmt.is_string() || fmt.as_string() != "mce-sequence") {
        err += "the file is not an mce-sequence document";
        return false;
    }
    SequenceForm f;
    if (const cook::JsonValue n = root.get("name"); n.valid() && n.is_string()) f.name = cook::String(n.as_string());
    if (const cook::JsonValue l = root.get("loop"); l.valid() && l.is_bool()) f.loop = l.as_bool();
    const cook::JsonValue steps = root.get("steps");
    if (steps.valid() && steps.is_array()) {
        for (u32 i = 0; i < steps.size(); ++i) {
            const cook::JsonValue step = steps.at(i);
            if (!step.is_object()) {
                cook::appendf(err, "step %u is not an object", i + 1);
                return false;
            }
            SequenceStepForm s;
            bool found = false;
            for (int k = 0; k < SequenceStepForm::kKinds && !found; ++k) {
                const cook::JsonValue v = step.get(kStepKeys[k]);
                if (!v.valid()) continue;
                found = true;
                s.kind = k;
                if (k == SequenceStepForm::Wait && v.is_number()) s.seconds = static_cast<f32>(v.as_number());
                else if ((step_has_name(k) || k == SequenceStepForm::Sound || k == SequenceStepForm::Spawn) && v.is_string())
                    s.text = cook::String(v.as_string());
                else if (k == SequenceStepForm::MoveTo) read_position(v, s);
            }
            if (!found) {
                cook::appendf(err, "step %u names no known action", i + 1);
                return false;
            }
            auto number = [&](const char* key, f32& v) {
                const cook::JsonValue n = step.get(key);
                if (n.valid() && n.is_number()) v = static_cast<f32>(n.as_number());
            };
            number("speed", s.speed);
            number("tolerance", s.tolerance);
            number("gain_db", s.gain_db);
            if (const cook::JsonValue r = step.get("relative"); r.valid() && r.is_bool()) s.relative = r.as_bool();
            if (s.kind == SequenceStepForm::Spawn) read_position(step.get("at"), s);
            f.steps.push_back(static_cast<SequenceStepForm&&>(s));
        }
    }
    out = static_cast<SequenceForm&&>(f);
    return true;
}

bool sequence_form_to_text(StringView old_text, const SequenceForm& form, cook::String& out, cook::String& err) {
    Array<Owned> owned;
    auto add = [&](const char* key, cook::String text, bool is_default) { owned.push_back(Owned{key, static_cast<cook::String&&>(text), is_default}); };
    {
        cook::String t;
        cook::append_json_string(t, form.name.view());
        add("name", static_cast<cook::String&&>(t), form.name.empty());
    }
    add("loop", cook::String(form.loop ? "true" : "false"), !form.loop);
    cook::String steps("[");
    for (u32 i = 0; i < form.steps.size(); ++i) {
        steps += i ? ",\n    " : "\n    ";
        put_step(steps, form.steps[i]);
    }
    steps += form.steps.empty() ? "]" : "\n  ]";
    add("steps", static_cast<cook::String&&>(steps), false);
    return rewrite_members(old_text, owned, "sequence", out, err);
}

bool load_sequence_form(const EditorProject& project, const ProjectAsset& asset, SequenceForm& out, cook::String& err) {
    if (asset.type != asset::ChunkType::Sequence) {
        err += "the asset is not a sequence";
        return false;
    }
    cook::String text;
    if (!cook::fs::read_text(project.asset_path(asset.path.view()).view(), text)) {
        err += "could not read the sequence file";
        return false;
    }
    return sequence_form_from_text(text.view(), out, err);
}

bool save_sequence_form(EditorProject& project, const ProjectAsset& asset, const SequenceForm& form, cook::String& err) {
    if (asset.type != asset::ChunkType::Sequence) {
        err += "the asset is not a sequence";
        return false;
    }
    cook::String old_text;
    if (!cook::fs::read_text(project.asset_path(asset.path.view()).view(), old_text)) {
        err += "could not read the sequence file";
        return false;
    }
    cook::String text;
    if (!sequence_form_to_text(old_text.view(), form, text, err)) return false;
    return validate_and_save_source(project, asset, text.view(), err);
}

}  // namespace mce::editor
