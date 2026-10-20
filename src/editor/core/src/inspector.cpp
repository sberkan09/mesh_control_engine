#include "mce/editor/inspector.h"
#include "mce/asset/handle.h"
#include "mce/ecs/transform.h"
#include "mce/log.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace mce::editor {

namespace {

using K = refl::FieldKind;

template <class T>
T load(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
template <class T>
void store(u8* p, T v) {
    std::memcpy(p, &v, sizeof(T));
}

void int_limits(K kind, i64& lo, i64& hi) {
    constexpr i64 kMaxExact = i64(1) << 53;
    switch (kind) {
        case K::I8: lo = -128; hi = 127; return;
        case K::I16: lo = -32768; hi = 32767; return;
        case K::I32: lo = -2147483648LL; hi = 2147483647LL; return;
        case K::U8: lo = 0; hi = 255; return;
        case K::U16: lo = 0; hi = 65535; return;
        case K::U32: lo = 0; hi = 4294967295LL; return;
        case K::U64: lo = 0; hi = kMaxExact; return;
        default: lo = -kMaxExact; hi = kMaxExact; return;
    }
}

i64 load_int(K kind, const u8* p) {
    switch (kind) {
        case K::I8: return load<i8>(p);
        case K::I16: return load<i16>(p);
        case K::I32: return load<i32>(p);
        case K::I64: return load<i64>(p);
        case K::U8: return load<u8>(p);
        case K::U16: return load<u16>(p);
        case K::U32: return load<u32>(p);
        case K::U64: return static_cast<i64>(load<u64>(p));
        default: return 0;
    }
}

void store_int(K kind, u8* p, i64 v) {
    switch (kind) {
        case K::I8: store<i8>(p, static_cast<i8>(v)); return;
        case K::I16: store<i16>(p, static_cast<i16>(v)); return;
        case K::I32: store<i32>(p, static_cast<i32>(v)); return;
        case K::I64: store<i64>(p, v); return;
        case K::U8: store<u8>(p, static_cast<u8>(v)); return;
        case K::U16: store<u16>(p, static_cast<u16>(v)); return;
        case K::U32: store<u32>(p, static_cast<u32>(v)); return;
        case K::U64: store<u64>(p, static_cast<u64>(v)); return;
        default: return;
    }
}

i64 load_enum(u32 size, bool is_signed, const u8* p) {
    switch (size) {
        case 1: return is_signed ? load<i8>(p) : load<u8>(p);
        case 2: return is_signed ? load<i16>(p) : load<u16>(p);
        case 4: return is_signed ? load<i32>(p) : load<u32>(p);
        default: return load<i64>(p);
    }
}

void store_enum(u32 size, u8* p, i64 v) {
    switch (size) {
        case 1: store<u8>(p, static_cast<u8>(v)); return;
        case 2: store<u16>(p, static_cast<u16>(v)); return;
        case 4: store<u32>(p, static_cast<u32>(v)); return;
        default: store<i64>(p, v); return;
    }
}

const char* asset_type_name(StringId id) {
    static constexpr const char* kNames[] = {asset::Texture::name, asset::Mesh::name, asset::Material::name,
                                             asset::Shader::name,  asset::Sfx::name,  asset::Font::name,
                                             asset::Atlas::name,   asset::Scene::name, asset::InputMap::name,
                                             asset::Video::name, asset::BehaviorTree::name, asset::Script::name,
                                             asset::RoadNetwork::name, asset::ItemCatalogChunk::name};
    for (const char* n : kNames)
        if (StringId(n) == id) return n;
    return "";
}

struct FieldDrawer {
    DrawCtx& ui;

    // Returns true when the value at `p` changed.
    bool element(const refl::FieldRecord& f, const char* label, u8* p) {
        const bool ro = f.has(refl::FieldFlags::ReadOnly);
        const bool ranged = f.has(refl::FieldFlags::EditorRange);
        bool changed = false;
        switch (f.kind) {
            case K::Bool: {
                bool v = load<u8>(p) != 0;
                if (ro) {
                    ui.read_only(label, v ? "true" : "false");
                } else if (ui.checkbox(label, &v)) {
                    store<u8>(p, v ? 1 : 0);
                    changed = true;
                }
                break;
            }
            case K::I8: case K::I16: case K::I32: case K::I64: case K::U8: case K::U16: case K::U32: case K::U64: {
                i64 lo, hi;
                int_limits(f.kind, lo, hi);
                if (ranged) {
                    lo = static_cast<i64>(f.editor_min);
                    hi = static_cast<i64>(f.editor_max);
                }
                i64 v = load_int(f.kind, p);
                if (ro) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
                    ui.read_only(label, buf);
                } else if (ui.int_field(label, &v, lo, hi)) {
                    store_int(f.kind, p, v < lo ? lo : (v > hi ? hi : v));
                    changed = true;
                }
                break;
            }
            case K::F32:
            case K::F64: {
                f64 v = f.kind == K::F32 ? static_cast<f64>(load<f32>(p)) : load<f64>(p);
                if (ro) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%g", v);
                    ui.read_only(label, buf);
                } else if (ui.float_field(label, &v, ranged ? f.editor_min : 0.0, ranged ? f.editor_max : 0.0)) {
                    if (f.kind == K::F32) store<f32>(p, static_cast<f32>(v));
                    else store<f64>(p, v);
                    changed = true;
                }
                break;
            }
            case K::Vec2:
            case K::Vec3:
            case K::Vec4: {
                f32 v[4];
                const u32 n = f.size / 4;
                std::memcpy(v, p, f.size);
                if (ro) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), n == 2 ? "%g, %g" : (n == 3 ? "%g, %g, %g" : "%g, %g, %g, %g"),
                                  static_cast<f64>(v[0]), static_cast<f64>(v[1]), static_cast<f64>(v[2]), static_cast<f64>(v[3]));
                    ui.read_only(label, buf);
                } else if (ui.float_n(label, v, n, ranged ? f.editor_min : 0.0f, ranged ? f.editor_max : 0.0f)) {
                    std::memcpy(p, v, f.size);
                    changed = true;
                }
                break;
            }
            case K::Quat: {
                const math::quat q = load<math::quat>(p);
                math::vec3 e = quat_to_euler_deg(q);
                f32 v[3] = {e.x, e.y, e.z};
                if (!ro && ui.float_n(label, v, 3, 0.0f, 0.0f)) {
                    store<math::quat>(p, euler_deg_to_quat({v[0], v[1], v[2]}));
                    changed = true;
                } else if (ro) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "%g, %g, %g deg", static_cast<f64>(v[0]), static_cast<f64>(v[1]),
                                  static_cast<f64>(v[2]));
                    ui.read_only(label, buf);
                }
                break;
            }
            case K::Color: {
                f32 c[4];
                std::memcpy(c, p, 16);
                if (!ro && ui.color_field(label, c)) {
                    std::memcpy(p, c, 16);
                    changed = true;
                }
                break;
            }
            case K::String: {
                if (!f.string_ops) break;
                char buf[512];
                const StringView cur = f.string_ops->get(p);
                const usize cap = f.string_ops->capacity + 1 < sizeof(buf) ? f.string_ops->capacity + 1 : sizeof(buf);
                const usize n = cur.size() < cap - 1 ? cur.size() : cap - 1;
                std::memcpy(buf, cur.data(), n);
                buf[n] = 0;
                if (ro) {
                    ui.read_only(label, StringView(buf, n));
                } else if (ui.text_field(label, buf, cap)) {
                    f.string_ops->set(p, StringView(buf));
                    changed = true;
                }
                break;
            }
            case K::Enum: {
                const refl::EnumRecord* e = f.type_id.valid() ? refl::find_enum(f.type_id) : nullptr;
                i64 v = load_enum(f.size, e && e->is_signed, p);
                bool edited = false;
                if (e && !ro) {
                    edited = ui.enum_field(label, &v, *e);
                } else if (!ro) {
                    i64 lo, hi;
                    int_limits(f.size == 1 ? K::U8 : (f.size == 2 ? K::U16 : K::U32), lo, hi);
                    edited = ui.int_field(label, &v, lo, hi);
                } else {
                    const refl::EnumValue* ev = e ? e->find(v) : nullptr;
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
                    ui.read_only(label, ev ? StringView(ev->name_str) : StringView(buf));
                }
                if (edited) {
                    store_enum(f.size, p, v);
                    changed = true;
                }
                break;
            }
            case K::AssetRef: {
                asset::Guid g;
                std::memcpy(&g, p, sizeof(g));
                if (!ro && ui.asset_field(label, &g, asset_type_name(f.type_id))) {
                    std::memset(p, 0, f.size);  // a new GUID drops the stale handle
                    std::memcpy(p, &g, sizeof(g));
                    changed = true;
                }
                break;
            }
            case K::EntityRef: {
                Entity ref;
                std::memcpy(&ref, p, sizeof(ref));
                EntityId id = doc_entity_id(ref);
                if (!ro && ui.entity_field(label, &id)) {
                    store<Entity>(p, doc_entity_ref(id));
                    changed = true;
                }
                break;
            }
            case K::Struct: {
                const refl::TypeRecord* t = refl::find_type(f.type_id);
                if (t && ui.begin_group(label, true)) {
                    changed = fields(*t, p);
                    ui.end_group();
                }
                break;
            }
        }
        if (f.help && f.help[0]) ui.help(f.help);
        return changed;
    }

    // `overrides` (a prefab instance's component, top level only): marks each field overridden
    // or inherited before drawing it.
    bool fields(const refl::TypeRecord& type, u8* value, const u64* overrides = nullptr) {
        bool changed = false;
        for (usize fi = 0; fi < type.fields.size(); ++fi) {
            const refl::FieldRecord& f = type.fields[fi];
            if (f.has(refl::FieldFlags::Transient)) continue;  // runtime-only: not authored
            if (overrides) ui.override_mark(fi < 64 && ((*overrides >> fi) & 1u) != 0);
            if (f.count > 1) {
                if (ui.begin_group(f.name_str, false)) {
                    for (u32 i = 0; i < f.count; ++i) {
                        char label[32];
                        std::snprintf(label, sizeof(label), "[%u]", i);
                        changed |= element(f, label, value + f.offset + usize(i) * f.size);
                    }
                    ui.end_group();
                }
            } else {
                changed |= element(f, f.name_str, value + f.offset);
            }
        }
        return changed;
    }
};

}  // namespace

bool draw_fields(DrawCtx& ui, const refl::TypeRecord& type, void* value) {
    FieldDrawer d{ui};
    return d.fields(type, static_cast<u8*>(value));
}

void addable_components(const DocEntity& e, Array<const refl::TypeRecord*>& out) {
    out.clear();
    for (const refl::TypeRecord* t : refl::all())  // sorted by name
        if (t->is_component() && !e.find(*t)) out.push_back(t);
}

bool draw_inspector(DrawCtx& ui, EditorScene& scene, EntityId id) {
    const DocEntity* e = scene.doc().find(id);
    if (!e) return false;
    bool changed = false;

    char name[65];
    const usize n = e->name.view().size() < 64 ? e->name.view().size() : 64;
    std::memcpy(name, e->name.view().data(), n);
    name[n] = 0;
    if (ui.text_field("Name", name, sizeof(name))) changed |= scene.rename_entity(id, name);
    const bool instance = !e->prefab.is_null();
    if (instance) {
        ui.read_only("Prefab", asset::to_string(e->prefab).view());
        ui.help("Prefab instance: the cook expands the prefab here. Fields you edit override the prefab root; "
                "the rest follow the prefab.");
        // The root's components this instance does not carry: inherited as they are.
        Array<const DocComponent*> base;
        scene.prefab_base_components(id, base);
        FixedString<256> inherited;
        for (const DocComponent* b : base) {
            if (e->find(*b->type)) continue;
            if (!inherited.empty()) inherited.append(", ");
            inherited.append(b->type->name);
        }
        if (!inherited.empty()) ui.read_only("Inherited", inherited.view());
    }

    // Draw over a copy of each component; commit the copy when a widget changed it. The entity
    // pointer is re-fetched after every commit (a commit may reallocate document storage).
    const usize count = e->components.size();
    for (usize i = 0; i < count; ++i) {
        e = scene.doc().find(id);
        if (!e || i >= e->components.size()) break;
        const DocComponent& c = e->components[i];
        const refl::TypeRecord& type = *c.type;
        Array<u64> copy;
        copy.resize(c.storage.size());
        std::memcpy(copy.data(), c.storage.data(), c.storage.size() * sizeof(u64));
        bool remove = false;
        const bool open = ui.begin_component(type.name, &remove);
        bool edited = false;
        DrawCtx::OverrideAction action = DrawCtx::OverrideAction::None;
        if (open) {
            const u64 overrides = c.overrides;
            if (instance && &type != &refl::type_of<LocalTransform>())
                action = ui.override_actions(overrides != 0 || !scene.prefab_base(id, type),
                                             (overrides & ~entity_ref_fields_mask(type)) != 0);
            FieldDrawer d{ui};
            edited = d.fields(type, reinterpret_cast<u8*>(copy.data()), instance ? &overrides : nullptr);
            ui.end_component();
        }
        if (edited) changed |= scene.set_component(id, type, copy.data());
        if (action == DrawCtx::OverrideAction::Revert) {
            changed |= scene.revert_override(id, type);
            break;  // the component may be gone; the next frame draws the rest
        }
        if (action == DrawCtx::OverrideAction::Apply) {
            if (Result<void> r = scene.apply_override(id, type); !r) {
                FixedString<512> msg;
                format_error(r.error(), msg);
                MCE_LOG_WARN("inspector: apply to prefab failed: {}", msg.c_str());
            } else {
                changed = true;
            }
            break;
        }
        if (remove) {
            changed |= scene.remove_component(id, type);
            break;  // indices shifted; the next frame draws the rest
        }
    }

    e = scene.doc().find(id);
    if (e) {
        Array<const refl::TypeRecord*> addable;
        addable_components(*e, addable);
        if (const refl::TypeRecord* t = ui.add_component_menu(addable.span())) changed |= scene.add_component(id, *t);
    }
    return changed;
}

math::vec3 quat_to_euler_deg(math::quat q) {
    // Z * Y * X (yaw-pitch-roll) extraction.
    const f32 sinr = 2.0f * (q.w * q.x + q.y * q.z);
    const f32 cosr = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const f32 x = std::atan2(sinr, cosr);
    f32 sinp = 2.0f * (q.w * q.y - q.z * q.x);
    sinp = sinp > 1.0f ? 1.0f : (sinp < -1.0f ? -1.0f : sinp);
    const f32 y = std::asin(sinp);
    const f32 siny = 2.0f * (q.w * q.z + q.x * q.y);
    const f32 cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const f32 z = std::atan2(siny, cosy);
    constexpr f32 kDeg = 180.0f / math::kPi;
    return {x * kDeg, y * kDeg, z * kDeg};
}

math::quat euler_deg_to_quat(math::vec3 deg) {
    constexpr f32 kRad = math::kPi / 180.0f;
    const math::quat qx = math::quat_from_axis_angle({1, 0, 0}, deg.x * kRad);
    const math::quat qy = math::quat_from_axis_angle({0, 1, 0}, deg.y * kRad);
    const math::quat qz = math::quat_from_axis_angle({0, 0, 1}, deg.z * kRad);
    return math::normalize(qz * qy * qx);
}

}  // namespace mce::editor
