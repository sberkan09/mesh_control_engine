#include "mce/editor/scene_document.h"

#include <cstring>

#include "mce/cook/cook.h"
#include "mce/cook/json.h"
#include "mce/cook/scene_text.h"

namespace mce::editor {

namespace {

constexpr const char* kStage = "editor.scene";

bool type_less(const DocComponent& a, const DocComponent& b) { return StringView(a.type->name) < StringView(b.type->name); }

void visit_struct(const refl::TypeRecord& type, u8* value, void (*fn)(Entity*, void*), void* user) {
    for (const refl::FieldRecord& f : type.fields) {
        for (u32 i = 0; i < f.count; ++i) {
            u8* p = value + f.offset + usize(i) * f.size;
            if (f.kind == refl::FieldKind::EntityRef && f.size == sizeof(Entity)) {
                fn(reinterpret_cast<Entity*>(p), user);
            } else if (f.kind == refl::FieldKind::Struct) {
                if (const refl::TypeRecord* t = refl::find_type(f.type_id)) visit_struct(*t, p, fn, user);
            }
        }
    }
}

void error(cook::DiagList& diags, StringView file, cook::JsonValue at, StringView msg) {
    diags.error(Errc::ParseError, file, at.valid() ? at.line() : 0, at.valid() ? at.col() : 0, kStage, msg);
}

}  // namespace

const DocComponent* DocEntity::find(const refl::TypeRecord& t) const {
    for (const DocComponent& c : components)
        if (c.type == &t) return &c;
    return nullptr;
}

DocComponent* DocEntity::find(const refl::TypeRecord& t) {
    return const_cast<DocComponent*>(static_cast<const DocEntity*>(this)->find(t));
}

void visit_entity_refs(const refl::TypeRecord& type, void* value, void (*fn)(Entity* field, void* user), void* user) {
    visit_struct(type, static_cast<u8*>(value), fn, user);
}

SceneDocument::SceneDocument() { reset("untitled"); }

u64 all_fields_mask(const refl::TypeRecord& type) {
    const usize n = type.fields.size() < 64 ? type.fields.size() : 64;
    return n == 64 ? ~u64(0) : (u64(1) << n) - 1;
}

u64 changed_fields_mask(const refl::TypeRecord& type, const void* a, const void* b) {
    const auto* pa = static_cast<const u8*>(a);
    const auto* pb = static_cast<const u8*>(b);
    u64 mask = 0;
    for (usize i = 0; i < type.fields.size() && i < 64; ++i) {
        const refl::FieldRecord& f = type.fields[i];
        if (f.has(refl::FieldFlags::Transient)) continue;
        if (std::memcmp(pa + f.offset, pb + f.offset, usize(f.size) * f.count) != 0) mask |= u64(1) << i;
    }
    return mask;
}

void copy_fields(const refl::TypeRecord& type, u64 mask, const void* src, void* dst) {
    for (usize i = 0; i < type.fields.size() && i < 64; ++i) {
        if (((mask >> i) & 1u) == 0) continue;
        const refl::FieldRecord& f = type.fields[i];
        std::memcpy(static_cast<u8*>(dst) + f.offset, static_cast<const u8*>(src) + f.offset, usize(f.size) * f.count);
    }
}

namespace {
bool holds_entity_ref(const refl::FieldRecord& f) {
    if (f.kind == refl::FieldKind::EntityRef) return true;
    if (f.kind != refl::FieldKind::Struct) return false;
    const refl::TypeRecord* t = refl::find_type(f.type_id);
    if (!t) return false;
    for (const refl::FieldRecord& sub : t->fields)
        if (holds_entity_ref(sub)) return true;
    return false;
}
}  // namespace

u64 entity_ref_fields_mask(const refl::TypeRecord& type) {
    u64 mask = 0;
    for (usize i = 0; i < type.fields.size() && i < 64; ++i)
        if (holds_entity_ref(type.fields[i])) mask |= u64(1) << i;
    return mask;
}

void SceneDocument::reset(StringView scene_name) {
    kind_ = DocKind::Scene;
    guid_ = cook::generate_guid();
    name_.assign(scene_name);
    entities_.clear();
    settings_.clear();
    next_id_ = 1;
}

const DocEntity* SceneDocument::find(EntityId id) const {
    const i32 i = index_of(id);
    return i < 0 ? nullptr : &entities_[static_cast<usize>(i)];
}

DocEntity* SceneDocument::find(EntityId id) { return const_cast<DocEntity*>(static_cast<const SceneDocument*>(this)->find(id)); }

i32 SceneDocument::index_of(EntityId id) const {
    if (id == kNoEntity) return -1;
    for (usize i = 0; i < entities_.size(); ++i)
        if (entities_[i].id == id) return static_cast<i32>(i);
    return -1;
}

EntityId SceneDocument::insert(StringView name, i32 at, EntityId id) {
    if (id == kNoEntity || find(id)) id = next_id_;
    if (id >= next_id_) next_id_ = id + 1;
    DocEntity e;
    e.id = id;
    e.name.assign(name);
    const usize pos = at < 0 || static_cast<usize>(at) > entities_.size() ? entities_.size() : static_cast<usize>(at);
    entities_.insert(pos, std::move(e));
    return id;
}

bool SceneDocument::erase(EntityId id) {
    const i32 i = index_of(id);
    if (i < 0) return false;
    entities_.erase(static_cast<usize>(i));
    return true;
}

DocComponent* SceneDocument::add_component(EntityId id, const refl::TypeRecord& type) {
    DocEntity* e = find(id);
    if (!e) return nullptr;
    if (DocComponent* c = e->find(type)) return c;
    DocComponent c;
    c.type = &type;
    c.storage.resize(type.size / 8 + 1, 0);
    if (type.construct_default) type.construct_default(c.data());
    usize pos = 0;
    while (pos < e->components.size() && type_less(e->components[pos], c)) ++pos;
    e->components.insert(pos, std::move(c));
    return &e->components[pos];
}

bool SceneDocument::remove_component(EntityId id, const refl::TypeRecord& type) {
    DocEntity* e = find(id);
    if (!e) return false;
    for (usize i = 0; i < e->components.size(); ++i) {
        if (e->components[i].type == &type) {
            e->components.erase(i);
            return true;
        }
    }
    return false;
}

void SceneDocument::insert_entity(DocEntity&& e, usize at) {
    if (e.id >= next_id_) next_id_ = e.id + 1;
    entities_.insert(at > entities_.size() ? entities_.size() : at, std::move(e));
}

bool SceneDocument::same_header(const SceneDocument& other) const {
    return kind_ == other.kind_ && guid_ == other.guid_ && name_.view() == other.name_.view() && settings_ == other.settings_;
}

bool SceneDocument::move(EntityId id, usize index) {
    const i32 from = index_of(id);
    if (from < 0) return false;
    if (index >= entities_.size()) index = entities_.size() - 1;
    DocEntity e = std::move(entities_[static_cast<usize>(from)]);
    entities_.erase(static_cast<usize>(from));
    entities_.insert(index, std::move(e));
    return true;
}

bool SceneDocument::parse(StringView text, StringView file, cook::DiagList& diags) {
    entities_.clear();
    settings_.clear();
    next_id_ = 1;
    const u32 errors_before = diags.error_count();

    cook::JsonDoc json;
    cook::JsonError jerr;
    if (!json.parse(text, jerr)) {
        diags.error(Errc::ParseError, file, jerr.line, jerr.col, kStage, jerr.message.view());
        return false;
    }
    const cook::JsonValue root = json.root();
    if (!root.is_object()) {
        error(diags, file, root, "a scene must be a JSON object");
        return false;
    }
    const cook::JsonValue fmt = root.get("format");
    if (!fmt.is_string() || (fmt.as_string() != "mce-scene" && fmt.as_string() != "mce-prefab")) {
        error(diags, file, fmt.valid() ? fmt : root, "\"format\" must be \"mce-scene\" or \"mce-prefab\"");
        return false;
    }
    kind_ = fmt.as_string() == "mce-prefab" ? DocKind::Prefab : DocKind::Scene;
    const cook::JsonValue ver = root.get("version");
    if (!ver.is_integer() || ver.as_number() < 1) {
        error(diags, file, ver.valid() ? ver : root, "\"version\" must be a positive integer");
        return false;
    }
    if (ver.as_number() > kFormatVersion) {
        cook::String msg;
        cook::appendf(msg, "scene version %u is newer than this editor understands (%u): update the editor",
                      static_cast<u32>(ver.as_number()), kFormatVersion);
        diags.error(Errc::VersionTooNew, file, ver.line(), ver.col(), kStage, msg);
        return false;
    }
    // Older versions migrate here (assets.md §6: migration happens at editor load): component
    // renames are looked up per record below. Version 1 is the first format, so the table is empty.
    const u32 file_version = static_cast<u32>(ver.as_number());

    const cook::JsonValue g = root.get("guid");
    Result<asset::Guid> parsed = g.is_string() ? asset::parse_guid(g.as_string()) : Result<asset::Guid>(Error(Errc::ParseError, kStage));
    if (!parsed) {
        error(diags, file, g.valid() ? g : root, "\"guid\" must be a UUID string");
        return false;
    }
    guid_ = parsed.value();
    const cook::JsonValue n = root.get("name");
    name_.assign(n.is_string() ? n.as_string() : StringView("untitled"));
    if (const cook::JsonValue s = root.get("settings"); s.valid()) cook::write_json(s, settings_, true);

    const cook::JsonValue ents = root.get("entities");
    if (ents.valid() && !ents.is_array()) {
        error(diags, file, ents, "'entities' must be an array");
        return false;
    }
    const u32 count = ents.valid() ? ents.size() : 0;
    // "$<index>" references are by file position. Ids come from the file's "id" keys (a save
    // writes them, so they survive a reopen); an entity without one, or with a duplicate, gets the
    // next free id, which for a file with no ids at all is index + 1.
    Array<EntityId> ids;
    for (u32 i = 0; i < count; ++i) {
        const cook::JsonValue ent = ents.at(i);
        const cook::JsonValue en = ent.is_object() ? ent.get("name") : cook::JsonValue();
        const cook::JsonValue eid = ent.is_object() ? ent.get("id") : cook::JsonValue();
        EntityId want = kNoEntity;
        if (eid.valid()) {
            if (!eid.is_integer() || eid.as_number() < 1.0 || eid.as_number() > 4294967294.0)
                error(diags, file, eid, "'id' must be an integer >= 1");
            else want = static_cast<EntityId>(eid.as_number());
        }
        ids.push_back(insert(en.is_string() ? en.as_string() : StringView(), -1, want));
        if (!ent.is_object()) {
            error(diags, file, ent, "entity must be an object");
            continue;
        }
        const EntityId id = ids[i];
        if (const cook::JsonValue pf = ent.get("prefab"); pf.valid()) {
            const StringView ref = pf.is_string() ? pf.as_string() : StringView();
            Result<asset::Guid> pg = ref.size() > 5 && ref.substr(0, 5) == "guid:" ? asset::parse_guid(ref.substr(5))
                                                                                 : Result<asset::Guid>(Error(Errc::ParseError, kStage, ref));
            if (pg) find(id)->prefab = pg.value();
            else error(diags, file, pf, "expected \"guid:<uuid>\" for 'prefab'");
        }
        const cook::JsonValue comps = ent.get("components");
        if (!comps.valid()) continue;
        if (!comps.is_object()) {
            error(diags, file, comps, "'components' must be an object");
            continue;
        }
        for (u32 c = 0; c < comps.size(); ++c) {
            StringView tname = comps.key_at(c);
            for (usize r = 0; r < rename_count_; ++r)
                if (file_version <= renames_[r].up_to_version && tname == StringView(renames_[r].old_name)) {
                    tname = StringView(renames_[r].new_name);
                    break;
                }
            const refl::TypeRecord* t = refl::find_type(StringId(tname));
            if (!t || !t->is_component()) {
                cook::String msg("unknown component type '");
                msg += tname;
                msg += "' (not registered with mce_refl in the editor)";
                msg += cook::kStaleModuleHint;
                diags.error(Errc::NotFound, file, comps.at(c).line(), comps.at(c).col(), kStage, msg);
                continue;
            }
            DocComponent* dc = add_component(id, *t);
            // Omitted references are null, not "$0": pre-fill them with the no-entity index the
            // reader writes for an explicit null (both read back as Entity::null below).
            visit_entity_refs(*t, dc->data(), [](Entity* f, void*) {
                f->index = cook::kNoSceneEntity;  // the reader's layout: the index in the first u32
                f->generation = 0;
            }, nullptr);
            cook::String path;
            cook::appendf(path, "entities[%u].", i);
            path += tname;
            // Renamed fields: the object is rebuilt under the new keys and read from that (a
            // diagnostic inside a migrated record then points into the rebuilt text).
            cook::JsonDoc migrated;
            cook::JsonValue body = comps.at(c);
            if (field_rename_count_ != 0 && body.is_object()) {
                cook::String text("{");
                bool any = false;
                for (u32 k = 0; k < body.size(); ++k) {
                    StringView key = body.key_at(k);
                    for (usize r = 0; r < field_rename_count_; ++r) {
                        const FieldRename& fr = field_renames_[r];
                        if (file_version <= fr.up_to_version && tname == StringView(fr.component) && key == StringView(fr.old_field)) {
                            key = StringView(fr.new_field);
                            any = true;
                            break;
                        }
                    }
                    if (k != 0) text += ", ";
                    text += "\"";
                    text += key;
                    text += "\": ";
                    cook::write_json(body.at(k), text, true);
                }
                text += "}";
                cook::JsonError merr;
                if (any && migrated.parse(text.view(), merr)) body = migrated.root();
            }
            cook::read_scene_component(*t, body, dc->data(), count, file, path, diags);
            // An instance overrides exactly the fields its file names.
            if (!find(id)->prefab.is_null()) {
                const cook::JsonValue obj = body;
                for (u32 k = 0; obj.is_object() && k < obj.size(); ++k)
                    for (usize fi = 0; fi < t->fields.size() && fi < 64; ++fi)
                        if (obj.key_at(k) == StringView(t->fields[fi].name_str)) dc->overrides |= u64(1) << fi;
            }
        }
    }
    // Scene-local indices (first u32 of each EntityRef, CHNK_SCENE layout) -> the ids of the
    // entities at those file positions.
    for (DocEntity& e : entities_) {
        for (DocComponent& c : e.components) {
            visit_entity_refs(*c.type, c.data(), [](Entity* f, void* user) {
                u32 index = 0;
                std::memcpy(&index, f, sizeof(index));
                const Array<EntityId>& by_index = *static_cast<const Array<EntityId>*>(user);
                *f = index == cook::kNoSceneEntity || index >= by_index.size() ? Entity::null : doc_entity_ref(by_index[index]);
            }, &ids);
        }
    }
    if (diags.error_count() != errors_before) {
        entities_.clear();
        next_id_ = 1;
        return false;
    }
    return true;
}

void SceneDocument::serialize(cook::String& out) const {
    out += kind_ == DocKind::Prefab ? "{\n  \"format\": \"mce-prefab\",\n" : "{\n  \"format\": \"mce-scene\",\n";
    cook::appendf(out, "  \"version\": %u,\n", kFormatVersion);
    out += "  \"guid\": \"";
    out += asset::to_string(guid_).view();
    out += "\",\n  \"name\": ";
    cook::append_json_string(out, name_.view());
    out += ",\n";
    if (!settings_.empty() && kind_ == DocKind::Scene) {
        out += "  \"settings\": ";
        out += settings_.view();
        out += ",\n";
    }
    out += "  \"entities\": [";
    struct Ctx {
        const SceneDocument* doc;
    } ctx{this};
    for (usize i = 0; i < entities_.size(); ++i) {
        const DocEntity& e = entities_[i];
        out += i ? ",\n    {\n      \"id\": " : "\n    {\n      \"id\": ";
        cook::appendf(out, "%u,\n      \"name\": ", e.id);
        cook::append_json_string(out, e.name.view());
        if (!e.prefab.is_null()) {
            out += ",\n      \"prefab\": \"guid:";
            out += asset::to_string(e.prefab).view();
            out += "\"";
        }
        out += ",\n      \"components\": {";
        for (usize c = 0; c < e.components.size(); ++c) {
            out += c ? ",\n        " : "\n        ";
            cook::append_json_string(out, e.components[c].type->name);
            out += ": ";
            const cook::SceneEntityIndexFn index_of_ref = [](const void* field, void* user) {
                Entity ref;
                std::memcpy(&ref, field, sizeof(ref));
                const i32 idx = static_cast<const Ctx*>(user)->doc->index_of(doc_entity_id(ref));
                return idx < 0 ? cook::kNoSceneEntity : static_cast<u32>(idx);
            };
            const DocComponent& dc = e.components[c];
            if (e.prefab.is_null())
                cook::write_scene_component(*dc.type, dc.data(), out, index_of_ref, &ctx);
            else  // an instance: its overrides, nothing else (the rest is the prefab's)
                cook::write_scene_component_fields(*dc.type, dc.data(), dc.overrides, out, index_of_ref, &ctx);
        }
        out += e.components.empty() ? "}\n    }" : "\n      }\n    }";
    }
    out += entities_.empty() ? "]\n}\n" : "\n  ]\n}\n";
}

}  // namespace mce::editor
