#include "mce/mcp/values.h"

#include <cstring>

#include "mce/cook/scene_text.h"
#include "mce/editor/project.h"

namespace mce::mcp {

namespace {

using editor::SceneDocument;

// EntityRef encodings while a value is being read (cook::read_scene_component writes the index in
// the first u32 and zeroes the rest, so generation 0 marks a field the reader wrote):
//   {id, 1}               untouched document reference
//   {kNoSceneEntity, 0}   null (untouched nulls are re-spelled this way first, so {0, 0} can only
//                         be an explicit "$0", which names no entity)
//   {id, 0}               "$<id>" just read: checked against the document
void encode_nulls(const refl::TypeRecord& type, void* value) {
    editor::visit_entity_refs(type, value, [](Entity* f, void*) {
        if (f->is_null()) *f = Entity{cook::kNoSceneEntity, 0};
    }, nullptr);
}

struct DecodeCtx {
    const SceneDocument* doc;
    u32 bad = 0;  // first unknown id + 1
};

bool decode_refs(const refl::TypeRecord& type, void* value, const SceneDocument& doc, cook::String& err) {
    DecodeCtx ctx{&doc};
    editor::visit_entity_refs(type, value, [](Entity* f, void* user) {
        auto* c = static_cast<DecodeCtx*>(user);
        if (f->generation != 0) return;
        if (f->index == cook::kNoSceneEntity) {
            *f = Entity::null;
            return;
        }
        if (f->index == editor::kNoEntity || !c->doc->find(f->index)) {
            if (c->bad == 0) c->bad = f->index + 1;
            *f = Entity::null;
            return;
        }
        *f = editor::doc_entity_ref(f->index);
    }, &ctx);
    if (ctx.bad == 0) return true;
    cook::appendf(err, "entity reference \"$%u\": no entity with that id in the scene", ctx.bad - 1);
    return false;
}

// Bound for "$<n>" while reading: one past the largest id in the document.
u32 id_bound(const SceneDocument& doc) {
    u32 bound = 1;
    for (const editor::DocEntity& e : doc.entities())
        if (e.id + 1 > bound) bound = e.id + 1;
    return bound;
}

// The reader's first error, with its "(at <path>)" spelled the way the caller addressed the value.
bool first_error(const cook::DiagList& diags, StringView at, cook::String& err) {
    for (const cook::Diagnostic& d : diags.all()) {
        if (d.severity != cook::Severity::Error) continue;
        StringView msg = d.message.view();
        // The reader speaks of file indices; the tools address entities by id.
        constexpr StringView kIndexMsgs[] = {"expected \"$<entity index>\" or null", "entity reference out of range"};
        cook::String reworded;
        for (StringView m : kIndexMsgs) {
            if (msg.substr(0, m.size()) != m) continue;
            reworded += "expected \"$<entity id>\" of an entity in the scene, or null";
            reworded += msg.substr(m.size());
            msg = reworded.view();
        }
        const StringView prefix = "(at value";
        const usize pos = msg.find(prefix);
        if (pos == StringView::npos) {
            err += msg;
            return false;
        }
        err += msg.substr(0, pos);
        err += "(at ";
        StringView rest = msg.substr(pos + prefix.size());
        if (!at.empty()) {
            err += at;
            if (rest.substr(0, 2) == ".v") rest.remove_prefix(2);
        } else if (!rest.empty() && rest[0] == '.') {
            rest.remove_prefix(1);
        }
        err += rest;
        return false;
    }
    return true;
}

// Fields flagged ReadOnly must come out of an edit unchanged (the inspector never writes them).
bool read_only_changed(const refl::TypeRecord& type, const u8* before, const u8* after, cook::String& err) {
    for (const refl::FieldRecord& f : type.fields) {
        const usize bytes = usize(f.size) * f.count;
        if (f.has(refl::FieldFlags::ReadOnly)) {
            if (std::memcmp(before + f.offset, after + f.offset, bytes) != 0) {
                err += "field '";
                err += f.name_str;
                err += "' of ";
                err += type.name;
                err += " is read-only";
                return true;
            }
        } else if (f.kind == refl::FieldKind::Struct) {
            if (const refl::TypeRecord* t = refl::find_type(f.type_id)) {
                for (u32 i = 0; i < f.count; ++i)
                    if (read_only_changed(*t, before + f.offset + i * f.size, after + f.offset + i * f.size, err)) return true;
            }
        }
    }
    return false;
}

// An entity reference may name the entity ("Player") as well as "$<id>": names are respelled as
// ids before the scene reader sees the value. `changed` says whether anything was respelled.
bool names_to_ids(const refl::FieldRecord* fields, usize nfields, cook::JsonValue v, const SceneDocument& doc, JsonWriter& w,
                  bool& changed, cook::String& err);

bool ref_to_id(cook::JsonValue v, const SceneDocument& doc, JsonWriter& w, bool& changed, cook::String& err) {
    if (!v.is_string() || v.as_string().empty() || v.as_string()[0] == '$') {
        w.copy(v);
        return true;
    }
    const editor::DocEntity* found = nullptr;
    for (const editor::DocEntity& e : doc.entities()) {
        if (e.name.view() != v.as_string()) continue;
        if (found) {
            err += "entity reference \"";
            err += v.as_string();
            err += "\": two entities have that name; give \"$<id>\"";
            return false;
        }
        found = &e;
    }
    if (!found) {
        err += "entity reference \"";
        err += v.as_string();
        err += "\": no entity with that name in the scene (an entity reference is \"$<id>\", a name, or null)";
        return false;
    }
    cook::String id;
    cook::appendf(id, "$%u", found->id);
    w.str(id.view());
    changed = true;
    return true;
}

bool field_to_ids(const refl::FieldRecord& f, cook::JsonValue v, const SceneDocument& doc, JsonWriter& w, bool& changed,
                  cook::String& err) {
    const refl::TypeRecord* nested = f.kind == refl::FieldKind::Struct ? refl::find_type(f.type_id) : nullptr;
    auto one = [&](cook::JsonValue x) {
        if (f.kind == refl::FieldKind::EntityRef) return ref_to_id(x, doc, w, changed, err);
        if (nested && x.is_object()) return names_to_ids(nested->fields.data(), nested->fields.size(), x, doc, w, changed, err);
        w.copy(x);
        return true;
    };
    if (f.count > 1 && v.is_array() && (f.kind == refl::FieldKind::EntityRef || nested)) {
        w.begin_array();
        for (u32 i = 0; i < v.size(); ++i)
            if (!one(v.at(i))) return false;
        w.end_array();
        return true;
    }
    return one(v);
}

bool names_to_ids(const refl::FieldRecord* fields, usize nfields, cook::JsonValue v, const SceneDocument& doc, JsonWriter& w,
                  bool& changed, cook::String& err) {
    w.begin_object();
    for (u32 i = 0; i < v.size(); ++i) {
        const StringView key = v.key_at(i);
        w.key(key);
        const refl::FieldRecord* f = nullptr;
        for (usize k = 0; k < nfields && !f; ++k)
            if (StringView(fields[k].name_str) == key) f = &fields[k];
        if (!f) {
            w.copy(v.at(i));  // the reader reports the unknown field
            continue;
        }
        if (!field_to_ids(*f, v.at(i), doc, w, changed, err)) return false;
    }
    w.end_object();
    return true;
}

// `at`: the property path of a single-leaf read ("" for an object of fields).
bool read_into(const refl::TypeRecord& type, cook::JsonValue obj, void* value, const SceneDocument& doc, StringView at,
               cook::String& err) {
    cook::String respelled;
    JsonWriter w(respelled);
    bool changed = false;
    if (obj.is_object() && !names_to_ids(type.fields.data(), type.fields.size(), obj, doc, w, changed, err)) return false;
    cook::JsonDoc named;
    if (changed) {
        cook::JsonError jerr;
        if (!named.parse(respelled.view(), jerr)) {
            err += "value is not valid JSON";
            return false;
        }
        obj = named.root();
    }
    encode_nulls(type, value);
    cook::DiagList diags;
    const bool ok = cook::read_scene_component(type, obj, value, id_bound(doc), "value", "value", diags);
    // Decode even on failure so no half-encoded reference survives in the caller's copy.
    const bool refs_ok = decode_refs(type, value, doc, err);
    if (!ok) {
        err.clear();
        return first_error(diags, at, err);
    }
    return refs_ok;
}

// The asset type name of an AssetRef field ("Texture", ...): its type_id is StringId(T::name).
const char* asset_type_of(StringId id) {
    for (u16 c = 1; c <= u16(asset::ChunkType::StringTable); ++c) {
        const char* name = editor::asset_type_name(static_cast<asset::ChunkType>(c));
        if (*name && StringId(name) == id) return name;
    }
    return "";
}

void describe_fields(JsonWriter& w, const refl::TypeRecord& type, u32 depth) {
    w.key("fields").begin_array();
    for (const refl::FieldRecord& f : type.fields) {
        w.begin_object();
        w.key("name").str(f.name_str);
        w.key("kind").str(refl::field_kind_name(f.kind));
        if (f.count > 1) w.key("count").num_u(f.count);
        if (f.kind == refl::FieldKind::Enum) {
            if (const refl::EnumRecord* e = refl::find_enum(f.type_id)) {
                w.key("type").str(e->name);
                w.key("values").begin_array();
                for (const refl::EnumValue& v : e->values) w.str(v.name_str);
                w.end_array();
            }
        } else if (f.kind == refl::FieldKind::AssetRef || f.kind == refl::FieldKind::Struct) {
            const refl::TypeRecord* t = f.kind == refl::FieldKind::Struct ? refl::find_type(f.type_id) : nullptr;
            if (t) {
                w.key("type").str(t->name);
                if (depth < 8) describe_fields(w, *t, depth + 1);
            } else if (const char* asset_type = asset_type_of(f.type_id); *asset_type) {
                w.key("type").str(asset_type);
            }
        } else if (f.kind == refl::FieldKind::String && f.string_ops) {
            w.key("capacity").num_u(f.string_ops->capacity);
        }
        if (f.has(refl::FieldFlags::EditorRange)) {
            w.key("min").num_f(f.editor_min);
            w.key("max").num_f(f.editor_max);
        }
        if (f.has(refl::FieldFlags::ReadOnly)) w.key("read_only").boolean(true);
        if (f.has(refl::FieldFlags::Transient)) w.key("transient").boolean(true);
        if (f.help && *f.help) w.key("help").str(f.help);
        w.end_object();
    }
    w.end_array();
}

}  // namespace

void write_component(const refl::TypeRecord& type, const void* value, cook::String& out) {
    cook::write_scene_component(type, value, out,
                                [](const void* field, void*) {
                                    Entity ref;
                                    std::memcpy(&ref, field, sizeof(ref));
                                    const editor::EntityId id = editor::doc_entity_id(ref);
                                    return id == editor::kNoEntity ? cook::kNoSceneEntity : id;
                                },
                                nullptr);
}

bool apply_patch(const refl::TypeRecord& type, cook::JsonValue patch, void* value, const SceneDocument& doc,
                 cook::String& err) {
    if (!patch.is_object()) {
        err += "expected an object of fields";
        return false;
    }
    Array<u64> before;
    before.resize(type.size / 8 + 1, 0);
    std::memcpy(before.data(), value, type.size);
    if (!read_into(type, patch, value, doc, {}, err)) return false;
    return !read_only_changed(type, reinterpret_cast<const u8*>(before.data()), static_cast<const u8*>(value), err);
}

bool apply_path(const refl::TypeRecord& type, StringView path, cook::JsonValue v, void* value, const SceneDocument& doc,
                cook::String& err) {
    Result<refl::PropertyRef> r = refl::resolve_path(type, path);
    if (!r) {
        err += r.error().code == Errc::NotFound ? "no such property '" : "malformed property path '";
        err += path;
        err += "' on ";
        err += type.name;
        return false;
    }
    const refl::PropertyRef ref = r.value();
    if (ref.field->has(refl::FieldFlags::Transient)) {
        err += "property '";
        err += path;
        err += "' is transient (never serialized)";
        return false;
    }
    // The leaf as a one-field struct at its own offset: the scene reader then does the type checks
    // (kinds, ranges, enumerators, array lengths, "$<id>", "guid:<uuid>") exactly as for a file.
    refl::FieldRecord leaf = *ref.field;
    leaf.name = StringId("v");
    leaf.name_str = "v";
    leaf.kind = ref.kind;
    leaf.offset = 0;
    leaf.size = ref.size;
    leaf.count = ref.count;
    leaf.type_id = ref.type_id;
    leaf.flags &= ~u32(refl::FieldFlags::ReadOnly);  // checked on the whole value below
    refl::TypeRecord wrapper;
    wrapper.name = type.name;
    wrapper.size = ref.size * ref.count;
    wrapper.fields = Span<const refl::FieldRecord>(&leaf, 1);

    cook::String text("{\"v\":");
    cook::write_json(v, text, true);
    text += '}';
    cook::JsonDoc json;
    cook::JsonError jerr;
    if (!json.parse(text.view(), jerr)) {
        err += "value is not valid JSON";
        return false;
    }
    Array<u64> before;
    before.resize(type.size / 8 + 1, 0);
    std::memcpy(before.data(), value, type.size);
    if (!read_into(wrapper, json.root(), static_cast<u8*>(value) + ref.offset, doc, path, err)) return false;
    return !read_only_changed(type, reinterpret_cast<const u8*>(before.data()), static_cast<const u8*>(value), err);
}

void describe_type(JsonWriter& w, const refl::TypeRecord& type) {
    w.begin_object();
    w.key("name").str(type.name);
    w.key("size").num_u(type.size);
    describe_fields(w, type, 0);
    w.end_object();
}

}  // namespace mce::mcp
