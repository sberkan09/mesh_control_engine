// .mcscene -> CHNK_SCENE and .mcprefab -> CHNK_PREFAB: the columnar transform of the editor's
// canonical scene / prefab documents (assets.md §6, FR-ENT-060/070). Every field name and type comes from mce_refl — there is no
// field list anywhere in this file, so a new reflected component needs zero cook changes
// (FR-DX-020).
//
// {
//   "format": "mce-scene", "version": 1, "guid": "<uuid>", "name": "level_01",
//   "settings": { ... },                       // editor/world settings, not cooked at M2
//   "entities": [
//     { "name": "Player", "components": {
//         "LocalTransform": { "position": [0, 2, 0] },
//         "Sprite": { "atlas": "guid:<uuid>", "frame": 0 },
//         "Follow": { "target": "$1" } } }     // entity refs: "$<index>" into this array
//   ]
// }
//
// Value spellings by FieldKind: Bool true/false; integers and floats as numbers (integers must be
// integral and in range); String as a string within capacity; Enum as an enumerator name (or an
// integer for un-reflected enums); Vec2/3/4/Quat/Color as number arrays; AssetRef as
// "guid:<uuid>" or null; EntityRef as "$<index>" or null; Struct as a nested object; fixed
// arrays as JSON arrays of exactly `count` elements. Omitted fields keep the C++ default member
// values; Transient fields must not appear. The cook accepts only the current "version" —
// migration happens at editor load (assets.md §6).
//
// Prefabs (v0.5): a .mcprefab has the same shape with "format": "mce-prefab" and no "settings";
// its entity 0 is the root. In either document an entity may instead be a prefab instance,
//   { "name": "Grunt A", "prefab": "guid:<uuid>", "components": { "LocalTransform": { ... } } }
// which the cook flattens: the prefab's entities (nested instances already expanded) take its
// place, and "components" overrides the root's fields (adding components it lacks). The runtime
// never sees an instance — only rows (no runtime inheritance, ecs-gameplay.md §2.11).

#include <algorithm>
#include <cstring>

#include "importer_common.h"
#include "mce/cook/scene_text.h"
#include "mce/asset/formats.h"
#include "mce/asset/handle.h"
#include "mce/asset/xxhash.h"
#include "mce/refl/refl.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.scene";
constexpr u32 kNoEntity = kNoSceneEntity;

bool asset_type_for(StringId type_id, ChunkType& out, const char*& name) {
    static constexpr const char* kNames[] = {asset::Texture::name, asset::Mesh::name,     asset::Material::name,
                                             asset::Shader::name,  asset::Sfx::name,      asset::Font::name,
                                             asset::Atlas::name,   asset::Scene::name,    asset::InputMap::name,
                                             asset::Prefab::name,  asset::Tilemap::name,  asset::ParticlePreset::name,
                                             asset::Sequence::name, asset::SpriteSheet::name,
                                             asset::AnimGraph::name, asset::UiDocument::name, asset::UiTheme::name,
                                             asset::Timeline::name, asset::Terrain::name, asset::Video::name,
                                             asset::NavMesh::name, asset::Skeleton2D::name};
    for (const char* n : kNames) {
        if (StringId(n) == type_id) {
            name = n;
            return asset::chunk_type_for_asset_name(n, out);
        }
    }
    return false;
}

struct PendingFixup {
    u32 column;  // builder index (remapped after sorting)
    u32 row;
    u32 offset;
    Guid guid;   // asset fixups
    u64 type_id;
};

struct ColumnBuilder {
    const refl::TypeRecord* type = nullptr;
    Array<u32> rows;
    Array<u8> data;
};

// Walks a document against reflection metadata, writing rows and collecting references.
class Walker {
public:
    Walker(StringView file, DiagList& diags) : file_(file), diags_(diags) {}

    // The document being walked: diagnostics name `file`; "$<i>" resolves through `map` (the
    // document's local entity index -> flattened entity index).
    void set_context(StringView file, const Array<u32>* map) {
        file_ = file;
        map_ = map;
    }

    Array<PendingFixup> asset_fixups;
    Array<PendingFixup> entity_fixups;
    Array<DepRef> deps;

    // Fills `dst` (one row of `type`, default-constructed by the caller) from `obj`.
    void write_struct(const refl::TypeRecord& type, JsonValue obj, u8* dst, u32 row_offset, u32 column, u32 row, String& path) {
        if (!obj.is_object()) {
            err(obj, path, "expected an object");
            return;
        }
        for (u32 i = 0; i < obj.size(); ++i) {
            const StringView key = obj.key_at(i);
            const usize mark = path.size();
            path += '.';
            path += key;
            const refl::FieldRecord* f = type.find_field(StringId(key));
            if (!f) {
                String msg("unknown field '");
                msg += key;
                msg += "' on ";
                msg += type.name;
                err(obj.at(i), path, msg);
            } else if (f->has(refl::FieldFlags::Transient)) {
                err(obj.at(i), path, "transient field is never serialized");
            } else {
                write_field(*f, obj.at(i), dst, row_offset, column, row, path);
            }
            truncate(path, mark);
        }
    }

private:
    static void truncate(String& s, usize n) {
        String t(s.view().substr(0, n));
        s = t;
    }

    void err(JsonValue at, StringView path, StringView msg) {
        String m(msg);
        if (!path.empty()) {
            m += " (at ";
            m += path;
            m += ')';
        }
        detail::error_at(diags_, Errc::ParseError, file_, at, kStage, m);
    }

    void write_field(const refl::FieldRecord& f, JsonValue v, u8* dst, u32 row_offset, u32 column, u32 row, String& path) {
        if (f.count > 1) {
            if (!v.is_array() || v.size() != f.count) {
                String msg;
                appendf(msg, "expected an array of %u elements", f.count);
                err(v, path, msg);
                return;
            }
            for (u32 i = 0; i < f.count; ++i) {
                const usize mark = path.size();
                appendf(path, "[%u]", i);
                write_element(f, v.at(i), dst + f.offset + i * f.size, row_offset + f.offset + i * f.size, column, row, path);
                truncate(path, mark);
            }
            return;
        }
        write_element(f, v, dst + f.offset, row_offset + f.offset, column, row, path);
    }

    template <class T>
    void put(u8* at, T value) {
        std::memcpy(at, &value, sizeof(T));
    }

    bool integer_in(JsonValue v, f64 lo, f64 hi, StringView path) {
        if (!v.is_integer() || v.as_number() < lo || v.as_number() > hi) {
            err(v, path, "expected an integer in range for the field type");
            return false;
        }
        return true;
    }

    void write_element(const refl::FieldRecord& f, JsonValue v, u8* at, u32 row_offset, u32 column, u32 row, String& path) {
        using K = refl::FieldKind;
        switch (f.kind) {
            case K::Bool:
                if (!v.is_bool()) return err(v, path, "expected true or false");
                put<u8>(at, v.as_bool() ? 1 : 0);
                return;
            case K::I8: if (integer_in(v, -128, 127, path)) put<i8>(at, static_cast<i8>(v.as_number())); return;
            case K::I16: if (integer_in(v, -32768, 32767, path)) put<i16>(at, static_cast<i16>(v.as_number())); return;
            case K::I32: if (integer_in(v, -2147483648.0, 2147483647.0, path)) put<i32>(at, static_cast<i32>(v.as_number())); return;
            case K::I64: if (integer_in(v, -9007199254740992.0, 9007199254740992.0, path)) put<i64>(at, static_cast<i64>(v.as_number())); return;
            case K::U8: if (integer_in(v, 0, 255, path)) put<u8>(at, static_cast<u8>(v.as_number())); return;
            case K::U16: if (integer_in(v, 0, 65535, path)) put<u16>(at, static_cast<u16>(v.as_number())); return;
            case K::U32: if (integer_in(v, 0, 4294967295.0, path)) put<u32>(at, static_cast<u32>(v.as_number())); return;
            case K::U64: if (integer_in(v, 0, 9007199254740992.0, path)) put<u64>(at, static_cast<u64>(v.as_number())); return;
            case K::F32:
                if (!v.is_number()) return err(v, path, "expected a number");
                put<f32>(at, static_cast<f32>(v.as_number()));
                return;
            case K::F64:
                if (!v.is_number()) return err(v, path, "expected a number");
                put<f64>(at, v.as_number());
                return;
            case K::String: {
                if (!v.is_string()) return err(v, path, "expected a string");
                if (!f.string_ops || v.as_string().size() > f.string_ops->capacity) return err(v, path, "string longer than the field's capacity");
                f.string_ops->set(at, v.as_string());
                return;
            }
            case K::Enum: return write_enum(f, v, at, path);
            case K::Vec2:
            case K::Vec3:
            case K::Vec4:
            case K::Quat:
            case K::Color: {
                const u32 n = f.size / 4;
                if (!v.is_array() || v.size() != n) {
                    String msg;
                    appendf(msg, "expected an array of %u numbers", n);
                    return err(v, path, msg);
                }
                for (u32 i = 0; i < n; ++i) {
                    if (!v.at(i).is_number()) return err(v.at(i), path, "expected a number");
                    put<f32>(at + 4 * i, static_cast<f32>(v.at(i).as_number()));
                }
                return;
            }
            case K::AssetRef: {
                std::memset(at, 0, f.size);
                if (v.is_null()) return;
                Guid g;
                if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) return err(v, path, "expected \"guid:<uuid>\" or null");
                ChunkType expected = ChunkType::Manifest;
                const char* type_name = "";
                if (!asset_type_for(f.type_id, expected, type_name)) return err(v, path, "field has an unknown asset type");
                std::memcpy(at, &g, sizeof(Guid));  // AssetRef<T> = {Guid, AssetHandle<T>}; the handle stays null
                asset_fixups.push_back(PendingFixup{column, row, row_offset, g, f.type_id.value});
                DepRef d;
                d.guid = g;
                d.expected = expected;
                d.line = v.line();
                d.col = v.col();
                d.where.assign(path.view().substr(0, 127));
                d.link = expected == ChunkType::Scene;  // the GUID is all a scene keeps of another scene
                deps.push_back(d);
                return;
            }
            case K::EntityRef: {
                std::memset(at, 0, f.size);
                u32 index = kNoEntity;
                if (!v.is_null()) {
                    StringView s = v.is_string() ? v.as_string() : StringView();
                    if (s.size() < 2 || s[0] != '$') return err(v, path, "expected \"$<entity index>\" or null");
                    u64 n = 0;
                    for (char c : s.substr(1)) {
                        if (c < '0' || c > '9' || n > map_->size()) return err(v, path, "expected \"$<entity index>\" or null");
                        n = n * 10 + static_cast<u64>(c - '0');
                    }
                    if (n >= map_->size()) return err(v, path, "entity reference out of range");
                    index = (*map_)[static_cast<usize>(n)];
                }
                put<u32>(at, index);
                entity_fixups.push_back(PendingFixup{column, row, row_offset, {}, 0});
                return;
            }
            case K::Struct: {
                const refl::TypeRecord* t = refl::find_type(f.type_id);
                if (!t) return err(v, path, "nested struct type is not registered");
                write_struct(*t, v, at, row_offset, column, row, path);
                return;
            }
        }
    }

    void write_enum(const refl::FieldRecord& f, JsonValue v, u8* at, StringView path) {
        const refl::EnumRecord* e = f.type_id.valid() ? refl::find_enum(f.type_id) : nullptr;
        i64 value = 0;
        if (v.is_string()) {
            const refl::EnumValue* ev = e ? e->find(StringId(v.as_string())) : nullptr;
            if (!ev) return err(v, path, "unknown enumerator");
            value = ev->value;
        } else if (v.is_integer()) {
            value = static_cast<i64>(v.as_number());
            if (e && !e->find(value)) return err(v, path, "value is not an enumerator");
        } else {
            return err(v, path, "expected an enumerator name");
        }
        switch (f.size) {
            case 1: put<u8>(at, static_cast<u8>(value)); break;
            case 2: put<u16>(at, static_cast<u16>(value)); break;
            case 4: put<u32>(at, static_cast<u32>(value)); break;
            case 8: put<i64>(at, value); break;
            default: err(v, path, "unsupported enum size");
        }
    }

    StringView file_;
    DiagList& diags_;
    const Array<u32>* map_ = nullptr;
};

struct SceneBuild {
    Array<ColumnBuilder> columns;
    Array<String> names;  // by flattened entity index
};

constexpr u32 kMaxPrefabDepth = 16;

// What a document is: the envelope keys and format string it must carry.
struct DocKind {
    const char* format;
    std::initializer_list<StringView> keys;
};
const DocKind kSceneDoc{"mce-scene", {"format", "version", "guid", "name", "settings", "entities"}};
const DocKind kPrefabDoc{"mce-prefab", {"format", "version", "guid", "name", "entities"}};

// Validates a document envelope. Returns the entities array (possibly invalid = none) or false.
bool check_envelope(JsonValue doc, const DocKind& kind, StringView file, DiagList& diags, JsonValue& ents) {
    if (!detail::check_keys(doc, kind.keys, file, kStage, diags)) return false;
    const JsonValue fmt = doc.get("format");
    if (!fmt.is_string() || fmt.as_string() != kind.format) {
        String msg;
        appendf(msg, "\"format\" must be \"%s\"", kind.format);
        detail::error_at(diags, Errc::ParseError, file, fmt.valid() ? fmt : doc, kStage, msg);
        return false;
    }
    const JsonValue ver = doc.get("version");
    if (!ver.is_integer() || ver.as_number() != kSceneFormatVersion) {
        String msg;
        appendf(msg, "%s version not accepted by this cook (expects %u): open and re-save in the editor first", kind.format,
                kSceneFormatVersion);
        detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
        return false;
    }
    ents = doc.get("entities");
    if (ents.valid() && !ents.is_array()) {
        detail::error_at(diags, Errc::ParseError, file, ents, kStage, "'entities' must be an array");
        return false;
    }
    return true;
}

// Expands a scene/prefab document into flattened entities (FR-ENT-070): an entity spelled
// { "prefab": "guid:<uuid>", "name": ..., "components": {...} } is replaced by the prefab's whole
// entity list (its entity 0 = the instance root), recursively, and its "components" are applied
// to that root as field-level overrides on top of the prefab's values. "$<i>" references always
// resolve within the document that spells them. In scan mode (dependency scan) instances are not
// expanded: each becomes one placeholder entity and a dependency on the prefab.
class Flattener {
public:
    Flattener(const ImportInput& in, DiagList& diags, Walker& w, SceneBuild& b, bool scan)
        : in_(in), diags_(diags), walker_(w), build_(b), scan_(scan) {}

    Array<DepRef> prefab_deps;

    // Flattened entity count of `ents` (a document's entities array). False on error (reported).
    bool count(JsonValue ents, StringView file, u32 depth, u32& out) {
        out = 0;
        const u32 n = ents.valid() ? ents.size() : 0;
        for (u32 i = 0; i < n; ++i) {
            const JsonValue ent = ents.at(i);
            if (!ent.is_object() || !ent.get("prefab").valid() || scan_) {
                ++out;
                continue;
            }
            JsonValue pents;
            StringView pfile;
            asset::Guid g;
            if (!resolve(ent.get("prefab"), file, depth, pents, pfile, g)) return false;
            stack_.push_back(g);
            u32 sub = 0;
            const bool ok = count(pents, pfile, depth + 1, sub);
            stack_.pop_back();
            if (!ok) return false;
            out += sub;
        }
        return true;
    }

    // Walks `ents` into flattened entities [base, base + count).
    void walk(JsonValue ents, StringView file, u32 base, u32 depth) {
        const u32 n = ents.valid() ? ents.size() : 0;
        Array<u32> map;
        map.resize(n);
        u32 cursor = base;
        Array<u32> sizes;
        sizes.resize(n, 1);
        for (u32 i = 0; i < n; ++i) {
            map[i] = cursor;
            const JsonValue ent = ents.at(i);
            if (!scan_ && ent.is_object() && ent.get("prefab").valid()) {
                JsonValue pents;
                StringView pfile;
                asset::Guid g;
                if (resolve(ent.get("prefab"), file, depth, pents, pfile, g)) {
                    stack_.push_back(g);
                    (void)count(pents, pfile, depth + 1, sizes[i]);
                    stack_.pop_back();
                }
            }
            cursor += sizes[i];
        }
        for (u32 i = 0; i < n; ++i) {
            const JsonValue ent = ents.at(i);
            String path;
            appendf(path, "entities[%u]", i);
            if (!ent.is_object()) {
                detail::error_at(diags_, Errc::ParseError, file, ent, kStage, "entity must be an object");
                continue;
            }
            const JsonValue prefab = ent.get("prefab");
            if (prefab.valid()) {
                // "id" is the editor's persistent entity id (SceneDocument); the cook addresses entities by position.
                if (!detail::check_keys(ent, {"id", "name", "prefab", "components"}, file, kStage, diags_)) continue;
                if (scan_) {
                    asset::Guid g;
                    if (prefab.is_string() && parse_guid_ref(prefab.as_string(), g)) {
                        DepRef d;
                        d.guid = g;
                        d.expected = ChunkType::Prefab;
                        d.line = prefab.line();
                        d.col = prefab.col();
                        d.where.assign(path.view().substr(0, 127));
                        prefab_deps.push_back(d);
                    }
                } else {
                    JsonValue pents;
                    StringView pfile;
                    asset::Guid g;
                    if (!resolve(prefab, file, depth, pents, pfile, g)) continue;
                    stack_.push_back(g);
                    walk(pents, pfile, map[i], depth + 1);
                    stack_.pop_back();
                }
            } else if (!detail::check_keys(ent, {"id", "name", "components"}, file, kStage, diags_)) {
                continue;
            }
            const JsonValue name = ent.get("name");
            if (name.valid() && !name.is_string()) detail::error_at(diags_, Errc::ParseError, file, name, kStage, "'name' must be a string");
            if (name.is_string() || !prefab.valid()) build_.names[map[i]] = String(name.is_string() ? name.as_string() : StringView());
            apply_components(ent.get("components"), map[i], file, map, path);
        }
    }

private:
    // "prefab": "guid:<uuid>" -> the prefab document's entities. Reports and returns false on any
    // problem (bad spelling, unknown GUID, not a prefab, cycle, nesting too deep).
    bool resolve(JsonValue ref, StringView file, u32 depth, JsonValue& ents, StringView& pfile, asset::Guid& g) {
        if (!ref.is_string() || !parse_guid_ref(ref.as_string(), g)) {
            detail::error_at(diags_, Errc::ParseError, file, ref, kStage, "'prefab' must be \"guid:<uuid>\"");
            return false;
        }
        JsonValue pdoc;
        StringView rel;
        ChunkType t = ChunkType::Manifest;
        if (!in_.lookup || !in_.lookup->type_of(g, t) || t != ChunkType::Prefab ||
            !in_.lookup->native_document(g, pdoc, rel)) {
            detail::error_at(diags_, Errc::MissingRef, file, ref, kStage, "'prefab' does not name a .mcprefab asset of this project");
            return false;
        }
        if (depth + 1 >= kMaxPrefabDepth || stack_.contains(g)) {
            detail::error_at(diags_, Errc::DependencyCycle, file, ref, kStage, "prefab nesting cycle (or deeper than 16 levels)");
            return false;
        }
        // Display path storage must outlive the walk: keep one String per resolved file.
        String disp("assets/");
        disp += rel;
        usize k = 0;
        while (k < files_.size() && files_[k].view() != disp.view()) ++k;
        if (k == files_.size()) files_.push_back(std::move(disp));
        pfile = files_[k].view();
        DiagList scratch;
        if (!check_envelope(pdoc, kPrefabDoc, pfile, envelope_checked(g) ? scratch : diags_, ents)) return false;
        if (!ents.valid() || ents.size() == 0) {
            if (!envelope_checked(g)) detail::error_at(diags_, Errc::InvalidArgument, pfile, pdoc, kStage, "a prefab needs at least one entity (entity 0 is its root)");
            return false;
        }
        checked_.push_back(g);
        return true;
    }

    bool envelope_checked(asset::Guid g) const { return checked_.contains(g); }

    // Writes (or overrides) components of flattened entity `flat`.
    void apply_components(JsonValue comps, u32 flat, StringView file, const Array<u32>& map, String& path) {
        if (!comps.valid()) return;
        if (!comps.is_object()) {
            detail::error_at(diags_, Errc::ParseError, file, comps, kStage, "'components' must be an object");
            return;
        }
        walker_.set_context(file, &map);
        for (u32 c = 0; c < comps.size(); ++c) {
            const StringView tname = comps.key_at(c);
            const refl::TypeRecord* t = refl::find_type(StringId(tname));
            if (!t || !t->is_component()) {
                String msg("unknown component type '");
                msg += tname;
                msg += t ? "' (reflected, but not an MCE_COMPONENT)" : "' (not registered with mce_refl in this cook)";
                detail::error_at(diags_, Errc::NotFound, file, comps.at(c), kStage, msg);
                continue;
            }
            u32 col = 0;
            while (col < build_.columns.size() && build_.columns[col].type != t) ++col;
            if (col == build_.columns.size()) {
                build_.columns.push_back(ColumnBuilder{});
                build_.columns.back().type = t;
            }
            ColumnBuilder& cb = build_.columns[col];
            u32 row = 0;
            while (row < cb.rows.size() && cb.rows[row] != flat) ++row;
            if (row == cb.rows.size()) {  // new row: C++ defaults, then the document's fields
                cb.rows.push_back(flat);
                const usize at = cb.data.size();
                cb.data.resize(at + t->size, 0);
                if (t->construct_default) t->construct_default(cb.data.data() + at);
            }
            const usize mark = path.size();
            path += '.';
            path += tname;
            walker_.write_struct(*t, comps.at(c), cb.data.data() + usize(row) * t->size, 0, col, row, path);
            path = String(path.view().substr(0, mark));
        }
    }

    const ImportInput& in_;
    DiagList& diags_;
    Walker& walker_;
    SceneBuild& build_;
    bool scan_;
    Array<asset::Guid> stack_;    // prefabs being expanded (cycle guard)
    Array<asset::Guid> checked_;  // prefab envelopes already validated (report once)
    Array<String> files_;
};

// Walks a whole document into `out`. Returns false on a structural error (already reported).
bool build_document(const ImportInput& in, const DocKind& kind, DiagList& diags, SceneBuild& out, Walker& walker, bool scan,
                    Array<DepRef>* prefab_deps) {
    JsonValue ents;
    if (!check_envelope(in.settings, kind, in.display_path, diags, ents)) return false;
    Flattener fl(in, diags, walker, out, scan);
    u32 total = 0;
    if (!fl.count(ents, in.display_path, 0, total)) return false;
    if (&kind == &kPrefabDoc && total == 0) {
        detail::error_at(diags, Errc::InvalidArgument, in.display_path, in.settings, kStage, "a prefab needs at least one entity (entity 0 is its root)");
        return false;
    }
    out.names.resize(total);
    fl.walk(ents, in.display_path, 0, 0);
    if (prefab_deps)
        for (const DepRef& d : fl.prefab_deps) prefab_deps->push_back(d);
    return true;
}

class SceneImporter final : public IImporter {
public:
    SceneImporter(const DocKind& kind, ChunkType type, u8 major, StringView id, StringView ext)
        : kind_(kind), type_(type), major_(major), id_(id), ext_(ext) {}

    StringView id() const override { return id_; }
    u32 version() const override { return 2; }  // 2: prefab instances flattened (v0.5)
    // Scene chunks store component rows in this build's layout: any reflected type change re-cooks.
    u64 schema_hash() const override {
        u64 h = kFnv64Offset;
        for (const refl::TypeRecord* t : refl::all()) {
            h = hash_combine(h, t->id.value);
            h = hash_combine(h, (u64(t->size) << 32) | t->align);
            for (const refl::FieldRecord& f : t->fields)
                h = hash_combine(hash_combine(h, f.name.value), (u64(f.offset) << 40) ^ (u64(f.size) << 16) ^ (u64(f.count) << 8) ^ u64(f.kind));
        }
        return h;
    }
    ChunkType type() const override { return type_; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ext_; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        (void)diags;  // structural errors are reported once, by cook()
        DiagList scratch;
        Walker w(in.display_path, scratch);
        SceneBuild b;
        build_document(in, kind_, scratch, b, w, true, &out);
        for (const DepRef& d : w.deps) out.push_back(d);
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        Walker w(file, diags);
        SceneBuild b;
        if (!build_document(in, kind_, diags, b, w, false, nullptr) || diags.error_count() != errors_before) return false;
        const u32 entity_count = static_cast<u32>(b.names.size());

        // Rows ascend by entity index (the reader's invariant): overrides and nested prefabs can
        // append a row for an earlier entity, so sort each column and remap fixup rows.
        Array<Array<u32>> row_remap;
        for (ColumnBuilder& cb : b.columns) {
            Array<u32> perm;
            for (u32 i = 0; i < cb.rows.size(); ++i) perm.push_back(i);
            std::sort(perm.begin(), perm.end(), [&](u32 x, u32 y) { return cb.rows[x] < cb.rows[y]; });
            Array<u32> rows;
            Array<u8> data;
            Array<u32> inv;
            inv.resize(perm.size());
            const u32 sz = cb.type->size;
            data.resize(cb.data.size());
            for (u32 i = 0; i < perm.size(); ++i) {
                rows.push_back(cb.rows[perm[i]]);
                if (sz) std::memcpy(data.data() + usize(i) * sz, cb.data.data() + usize(perm[i]) * sz, sz);
                inv[perm[i]] = i;
            }
            cb.rows = std::move(rows);
            cb.data = std::move(data);
            row_remap.push_back(std::move(inv));
        }
        for (PendingFixup& f : w.asset_fixups) f.row = row_remap[f.column][f.row];
        for (PendingFixup& f : w.entity_fixups) f.row = row_remap[f.column][f.row];

        // Columns sorted by type name (deterministic, independent of first-use order).
        Array<u32> order;
        for (u32 i = 0; i < b.columns.size(); ++i) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](u32 x, u32 y) { return StringView(b.columns[x].type->name) < StringView(b.columns[y].type->name); });
        Array<u32> remap;
        remap.resize(order.size());
        for (u32 i = 0; i < order.size(); ++i) remap[order[i]] = i;

        // A field written twice (prefab value, then an instance override) keeps its last fixup.
        auto keep_last = [](Array<PendingFixup>& fx) {
            Array<PendingFixup> outv;
            for (usize i = 0; i < fx.size(); ++i) {
                bool later = false;
                for (usize j = i + 1; j < fx.size() && !later; ++j)
                    later = fx[j].column == fx[i].column && fx[j].row == fx[i].row && fx[j].offset == fx[i].offset;
                if (!later) outv.push_back(fx[i]);
            }
            fx = std::move(outv);
        };
        keep_last(w.asset_fixups);
        keep_last(w.entity_fixups);

        // Unique asset refs, sorted by GUID; one GUID used as two asset types is an error.
        Array<asset::SceneAssetRef> refs;
        for (const PendingFixup& f : w.asset_fixups) refs.push_back(asset::SceneAssetRef{f.guid.hi, f.guid.lo, f.type_id});
        std::sort(refs.begin(), refs.end(), [](const asset::SceneAssetRef& x, const asset::SceneAssetRef& y) { return x.guid() < y.guid(); });
        usize n = 0;
        for (usize i = 0; i < refs.size(); ++i) {
            if (n > 0 && refs[n - 1].guid() == refs[i].guid()) {
                if (refs[n - 1].type_id != refs[i].type_id) {
                    String msg("asset ");
                    msg += asset::to_string(refs[i].guid()).view();
                    msg += " is referenced as two different asset types";
                    diags.error(Errc::InvalidArgument, file, 0, 0, kStage, msg);
                    return false;
                }
                continue;
            }
            refs[n++] = refs[i];
        }
        refs.resize(n);
        auto ref_index = [&](Guid g) {
            const asset::SceneAssetRef* it = std::lower_bound(refs.begin(), refs.end(), g, [](const asset::SceneAssetRef& r, Guid k) { return r.guid() < k; });
            return static_cast<u32>(it - refs.begin());
        };
        auto fixup_less = [](const asset::SceneFixup& x, const asset::SceneFixup& y) {
            if (x.column != y.column) return x.column < y.column;
            if (x.row != y.row) return x.row < y.row;
            return x.byte_offset < y.byte_offset;
        };
        Array<asset::SceneFixup> afix, efix;
        for (const PendingFixup& f : w.asset_fixups) afix.push_back(asset::SceneFixup{remap[f.column], f.row, f.offset, ref_index(f.guid)});
        for (const PendingFixup& f : w.entity_fixups) efix.push_back(asset::SceneFixup{remap[f.column], f.row, f.offset, 0});
        std::sort(afix.begin(), afix.end(), fixup_less);
        std::sort(efix.begin(), efix.end(), fixup_less);

        // Canonical source hash (assets.md §6 hash discipline).
        String canonical;
        write_json(doc, canonical, true);

        // ---- serialize
        Array<byte> payload;
        auto append = [&](const void* data, usize size, usize align) {
            const usize at = align_up(payload.size(), align < asset::CHUNK_ALIGN ? asset::CHUNK_ALIGN : align);
            payload.resize(at + size, byte{0});
            if (size) std::memcpy(payload.data() + at, data, size);
            return at;
        };
        asset::SceneHeader h{};
        h.format_version = kSceneFormatVersion;
        h.entity_count = entity_count;
        h.column_count = static_cast<u32>(order.size());
        h.asset_ref_count = static_cast<u32>(refs.size());
        h.asset_fixup_count = static_cast<u32>(afix.size());
        h.entity_fixup_count = static_cast<u32>(efix.size());
        h.source_hash = asset::xxh64(canonical.view());
        append(&h, sizeof(h), 16);
        Array<asset::SceneColumn> cols;
        cols.resize(order.size());
        h.columns_offset = append(cols.data(), cols.size() * sizeof(asset::SceneColumn), 16);
        h.asset_refs_offset = append(refs.data(), refs.size() * sizeof(asset::SceneAssetRef), 16);
        h.asset_fixups_offset = append(afix.data(), afix.size() * sizeof(asset::SceneFixup), 16);
        h.entity_fixups_offset = append(efix.data(), efix.size() * sizeof(asset::SceneFixup), 16);
        {
            Array<u32> table;
            Array<char> blob;
            for (const String& s : b.names) {
                table.push_back(static_cast<u32>(blob.size()));
                table.push_back(static_cast<u32>(s.size()));
                for (char c : s.view()) blob.push_back(c);
            }
            Array<byte> names;
            names.resize(table.size() * 4 + blob.size());
            if (!table.empty()) std::memcpy(names.data(), table.data(), table.size() * 4);
            if (!blob.empty()) std::memcpy(names.data() + table.size() * 4, blob.data(), blob.size());
            h.names_offset = append(names.data(), names.size(), 16);
            h.names_size = names.size();
        }
        for (u32 i = 0; i < order.size(); ++i) {
            const ColumnBuilder& cb = b.columns[order[i]];
            asset::SceneColumn& c = cols[i];
            c.type_id = StringId(cb.type->name).value;
            c.type_size = cb.type->size;
            c.type_align = cb.type->align;
            c.row_count = static_cast<u32>(cb.rows.size());
            c.rows_offset = append(cb.rows.data(), cb.rows.size() * 4, 16);
            c.data_offset = append(cb.data.data(), cb.data.size(), cb.type->align);
        }
        payload.resize(align_up(payload.size(), asset::CHUNK_ALIGN), byte{0});
        std::memcpy(payload.data(), &h, sizeof(h));
        if (!cols.empty()) std::memcpy(payload.data() + h.columns_offset, cols.data(), cols.size() * sizeof(asset::SceneColumn));

        asset::ContainerWriter::ChunkDesc d;
        d.type = type_;
        d.version = asset::chunk_version(major_, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }

private:
    const DocKind& kind_;
    ChunkType type_;
    u8 major_;
    StringView id_;
    StringView ext_;
};

}  // namespace

bool read_scene_component(const refl::TypeRecord& type, JsonValue obj, void* dst, u32 entity_count, StringView file,
                          StringView path, DiagList& diags) {
    const u32 errors_before = diags.error_count();
    Array<u32> identity;  // a lone document: local index == flattened index
    for (u32 i = 0; i < entity_count; ++i) identity.push_back(i);
    Walker w(file, diags);
    w.set_context(file, &identity);
    String p(path);
    w.write_struct(type, obj, static_cast<u8*>(dst), 0, 0, 0, p);
    return diags.error_count() == errors_before;
}

mem::UniquePtr<IImporter> make_scene_importer() {
    return mem::make_unique<SceneImporter>(mem::heap(), kSceneDoc, ChunkType::Scene, asset::SCENE_VERSION, "scene", ".mcscene");
}

mem::UniquePtr<IImporter> make_prefab_importer() {
    return mem::make_unique<SceneImporter>(mem::heap(), kPrefabDoc, ChunkType::Prefab, asset::PREFAB_VERSION, "prefab", ".mcprefab");
}

}  // namespace mce::cook
