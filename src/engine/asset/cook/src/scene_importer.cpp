// .mcscene -> CHNK_SCENE: the columnar transform of the editor's canonical scene document
// (assets.md §6, FR-ENT-060). Every field name and type comes from mce_refl — there is no
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

#include <algorithm>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/asset/handle.h"
#include "mce/asset/xxhash.h"
#include "mce/refl/refl.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.scene";
constexpr u32 kSceneFormatVersion = 1;
constexpr u32 kNoEntity = 0xFFFFFFFFu;

bool asset_type_for(StringId type_id, ChunkType& out, const char*& name) {
    static constexpr const char* kNames[] = {asset::Texture::name, asset::Mesh::name, asset::Material::name,
                                             asset::Shader::name,  asset::Sfx::name,  asset::Font::name,
                                             asset::Atlas::name,   asset::Scene::name, asset::InputMap::name};
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
    Walker(StringView file, DiagList& diags, u32 entity_count) : file_(file), diags_(diags), entity_count_(entity_count) {}

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
                        if (c < '0' || c > '9' || n > entity_count_) return err(v, path, "expected \"$<entity index>\" or null");
                        n = n * 10 + static_cast<u64>(c - '0');
                    }
                    if (n >= entity_count_) return err(v, path, "entity reference out of range");
                    index = static_cast<u32>(n);
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
    u32 entity_count_;
};

struct SceneBuild {
    Array<ColumnBuilder> columns;
    Array<String> names;
    Walker* walker = nullptr;
};

// Validates the document envelope and walks every entity. Returns false on a structural error.
bool walk_scene(JsonValue doc, StringView file, DiagList& diags, SceneBuild& out, Walker& walker) {
    if (!detail::check_keys(doc, {"format", "version", "guid", "name", "settings", "entities"}, file, kStage, diags)) return false;
    const JsonValue fmt = doc.get("format");
    if (!fmt.is_string() || fmt.as_string() != "mce-scene") {
        detail::error_at(diags, Errc::ParseError, file, fmt.valid() ? fmt : doc, kStage, "\"format\" must be \"mce-scene\"");
        return false;
    }
    const JsonValue ver = doc.get("version");
    if (!ver.is_integer() || ver.as_number() != kSceneFormatVersion) {
        String msg;
        appendf(msg, "scene version not accepted by this cook (expects %u): open and re-save in the editor first", kSceneFormatVersion);
        detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
        return false;
    }
    const JsonValue ents = doc.get("entities");
    if (ents.valid() && !ents.is_array()) {
        detail::error_at(diags, Errc::ParseError, file, ents, kStage, "'entities' must be an array");
        return false;
    }
    const u32 count = ents.valid() ? ents.size() : 0;
    for (u32 e = 0; e < count; ++e) {
        const JsonValue ent = ents.at(e);
        String path;
        appendf(path, "entities[%u]", e);
        if (!ent.is_object()) {
            detail::error_at(diags, Errc::ParseError, file, ent, kStage, "entity must be an object");
            continue;
        }
        if (ent.get("prefab").valid()) {
            detail::error_at(diags, Errc::Unsupported, file, ent.get("prefab"), kStage, "prefab instances arrive at v0.5 (MVP copies prefab contents)");
            continue;
        }
        if (!detail::check_keys(ent, {"name", "components"}, file, kStage, diags)) continue;
        const JsonValue name = ent.get("name");
        if (name.valid() && !name.is_string()) detail::error_at(diags, Errc::ParseError, file, name, kStage, "'name' must be a string");
        out.names.push_back(String(name.is_string() ? name.as_string() : StringView()));

        const JsonValue comps = ent.get("components");
        if (!comps.valid()) continue;
        if (!comps.is_object()) {
            detail::error_at(diags, Errc::ParseError, file, comps, kStage, "'components' must be an object");
            continue;
        }
        for (u32 c = 0; c < comps.size(); ++c) {
            const StringView tname = comps.key_at(c);
            const refl::TypeRecord* t = refl::find_type(StringId(tname));
            if (!t || !t->is_component()) {
                String msg("unknown component type '");
                msg += tname;
                msg += t ? "' (reflected, but not an MCE_COMPONENT)" : "' (not registered with mce_refl in this cook)";
                detail::error_at(diags, Errc::NotFound, file, comps.at(c), kStage, msg);
                continue;
            }
            u32 col = 0;
            while (col < out.columns.size() && out.columns[col].type != t) ++col;
            if (col == out.columns.size()) {
                out.columns.push_back(ColumnBuilder{});
                out.columns.back().type = t;
            }
            ColumnBuilder& cb = out.columns[col];
            const u32 row = static_cast<u32>(cb.rows.size());
            cb.rows.push_back(e);
            const usize at = cb.data.size();
            cb.data.resize(at + t->size, 0);
            if (t->construct_default) t->construct_default(cb.data.data() + at);
            const usize mark = path.size();
            path += '.';
            path += tname;
            walker.write_struct(*t, comps.at(c), cb.data.data() + at, 0, col, row, path);
            path = String(path.view().substr(0, mark));
        }
    }
    return true;
}

class SceneImporter final : public IImporter {
public:
    StringView id() const override { return "scene"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Scene; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcscene"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        (void)diags;  // structural errors are reported once, by cook()
        DiagList scratch;
        const JsonValue ents = in.settings.get("entities");
        Walker w(in.display_path, scratch, ents.is_array() ? ents.size() : 0);
        SceneBuild b;
        walk_scene(in.settings, in.display_path, scratch, b, w);
        for (const DepRef& d : w.deps) out.push_back(d);
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        const JsonValue ents = doc.get("entities");
        Walker w(file, diags, ents.is_array() ? ents.size() : 0);
        SceneBuild b;
        if (!walk_scene(doc, file, diags, b, w) || diags.error_count() != errors_before) return false;
        const u32 entity_count = static_cast<u32>(b.names.size());

        // Columns sorted by type name (deterministic, independent of first-use order).
        Array<u32> order;
        for (u32 i = 0; i < b.columns.size(); ++i) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](u32 x, u32 y) { return StringView(b.columns[x].type->name) < StringView(b.columns[y].type->name); });
        Array<u32> remap;
        remap.resize(order.size());
        for (u32 i = 0; i < order.size(); ++i) remap[order[i]] = i;

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
        d.type = ChunkType::Scene;
        d.version = asset::chunk_version(asset::SCENE_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_scene_importer() { return mem::make_unique<SceneImporter>(mem::heap()); }

}  // namespace mce::cook
