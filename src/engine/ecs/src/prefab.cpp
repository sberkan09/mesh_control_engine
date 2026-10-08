#include "mce/ecs/prefab.h"

#include "mce/ecs/hierarchy.h"
#include "mce/refl/property_path.h"

namespace mce::ecs {

namespace {

constexpr const char* kStage = "ecs.prefab.instantiate";
constexpr u32 kNoEntity = 0xFFFFFFFFu;

struct ColumnMap {
    ComponentId id = kInvalidComponent;
    const refl::TypeRecord* type = nullptr;
    u32 cursor = 0;  // next row to consume (rows ascend by entity index)
};

struct ResolvedOverride {
    const PrefabOverride* src = nullptr;
    ComponentId comp = kInvalidComponent;
    const refl::TypeRecord* type = nullptr;
    u32 offset = 0;
};

Result<ResolvedOverride> resolve_override(World& w, const PrefabOverride& o, u32 entity_count) {
    if (o.entity >= entity_count) return Error(Errc::InvalidArgument, kStage, o.path.view(), o.entity);
    const StringView path = o.path.view();
    const usize dot = path.find('.');
    if (dot == StringView::npos || dot == 0) return Error(Errc::InvalidArgument, kStage, path);
    const refl::TypeRecord* t = refl::find_type(StringId(path.substr(0, dot)));
    if (!t || !t->is_component()) return Error(Errc::NotFound, kStage, path);
    const ComponentId c = w.find_component_by_refl(t->id.value);
    if (c == kInvalidComponent) return Error(Errc::NotFound, kStage, path);
    auto ref = refl::resolve_path(*t, path.substr(dot + 1));
    if (!ref) return Error(ref.error().code, kStage, path);
    const refl::PropertyRef& r = ref.value();
    if (r.count != 1 || r.kind != o.kind || r.size != o.size || r.type_id != o.type_id || r.offset + r.size > t->size)
        return Error(Errc::InvalidArgument, kStage, path);
    ResolvedOverride out;
    out.src = &o;
    out.comp = c;
    out.type = t;
    out.offset = r.offset;
    return out;
}

}  // namespace

Result<u32> instantiate(World& w, Span<const byte> chunk_payload, const SpawnParams& p, Array<Entity>* out) {
    auto v = asset::SceneView::read(chunk_payload);
    if (!v) return v.error();
    return instantiate(w, v.value(), p, out);
}

Result<u32> instantiate(World& w, const asset::SceneView& tmpl, const SpawnParams& p, Array<Entity>* out) {
    const u32 n = tmpl.header ? tmpl.header->entity_count : 0;
    if (n == 0 || p.count == 0) return 0u;

    // 1. Validate everything before reserving a single handle.
    Array<ColumnMap> cols;
    for (u32 i = 0; i < tmpl.columns.size(); ++i) {
        const asset::SceneColumn& sc = tmpl.columns[i];
        ColumnMap m;
        m.id = w.find_component_by_refl(sc.type_id);
        if (m.id == kInvalidComponent) {
            const refl::TypeRecord* t = refl::find_type(StringId(sc.type_id));
            return Error(Errc::NotFound, kStage, t ? StringView(t->name) : StringView("<unregistered component>"), i);
        }
        m.type = refl::find_type(StringId(sc.type_id));
        if (w.component(m.id).size != sc.type_size) return Error(Errc::InvalidArgument, kStage, w.component(m.id).name, i);
        cols.push_back(m);
    }
    Array<ResolvedOverride> overrides;
    for (const PrefabOverride& o : p.overrides) {
        auto r = resolve_override(w, o, n);
        if (!r) return r.error();
        overrides.push_back(r.value());
    }
    // Template-local entity refs must point inside the template.
    for (const asset::SceneFixup& f : tmpl.entity_fixups) {
        u32 idx;
        std::memcpy(&idx, tmpl.data(f.column).data() + usize(f.row) * tmpl.columns[f.column].type_size + f.byte_offset, 4);
        if (idx != kNoEntity && idx >= n) return Error(Errc::InvalidArgument, kStage, "entity reference", idx);
    }
    const ComponentId child_of = p.parent.is_null() ? kInvalidComponent : w.component_id<ChildOf>();
    if (!p.parent.is_null() && !w.alive(p.parent)) return Error(Errc::InvalidArgument, kStage, "parent");
    // Asset refs resolve once per template (every replica shares the stamped value).
    constexpr usize kRefBytes = sizeof(asset::AssetRef<asset::Texture>);
    Array<u8> resolved;
    if (p.resolve_asset) {
        resolved.resize(tmpl.asset_refs.size() * kRefBytes, 0);
        for (u32 i = 0; i < tmpl.asset_refs.size(); ++i) {
            const asset::Guid g = tmpl.asset_refs[i].guid();
            std::memcpy(resolved.data() + i * kRefBytes, &g, sizeof(g));
            if (!p.resolve_asset(p.resolve_ctx, tmpl.asset_refs[i], resolved.data() + i * kRefBytes))
                return Error(Errc::MissingRef, kStage, asset::to_string(g).view(), i);
        }
    }
    // Fixups are sorted by (column, row, offset) by the cook: one cursor per column walks them in
    // step with the column's ascending rows.
    auto first_of = [&](Span<const asset::SceneFixup> fx, Array<u32>& start) {
        start.resize(cols.size() + 1);
        u32 k = 0;
        for (u32 c = 0; c <= cols.size(); ++c) {
            while (k < fx.size() && fx[k].column < c) ++k;
            start[c] = k;
        }
    };
    Array<u32> efix_start, afix_start, efix_cur, afix_cur;
    first_of(tmpl.entity_fixups, efix_start);
    first_of(tmpl.asset_fixups, afix_start);

    // Row-major scratch: per template entity, its rows patched for the current replica.
    // fixup lookups: (column,row) -> fixups, both sorted by (column,row,offset) by the cook.
    Array<u8> scratch;
    Array<RawComponent> comps;
    Array<u32> comp_offsets;
    Array<Entity> handles;
    handles.resize(n);
    u32 spawned = 0;
    const bool deferred = w.in_phase();

    for (u32 rep = 0; rep < p.count; ++rep) {
        for (u32 e = 0; e < n; ++e) handles[e] = w.reserve_entity();
        for (ColumnMap& m : cols) m.cursor = 0;
        efix_cur = efix_start;
        afix_cur = afix_start;

        for (u32 e = 0; e < n; ++e) {
            scratch.clear();
            comps.clear();
            comp_offsets.clear();
            bool has_child_of = false;
            // Template rows of entity e.
            for (u32 c = 0; c < cols.size(); ++c) {
                ColumnMap& m = cols[c];
                const Span<const u32> rows = tmpl.rows(c);
                if (m.cursor >= rows.size() || rows[m.cursor] != e) continue;
                const u32 row = m.cursor++;
                const u32 size = tmpl.columns[c].type_size;
                const usize at = align_up(scratch.size(), 16);
                scratch.resize(at + size, 0);
                std::memcpy(scratch.data() + at, tmpl.data(c).data() + usize(row) * size, size);
                // Entity refs: template index -> this replica's handle.
                for (; efix_cur[c] < efix_start[c + 1] && tmpl.entity_fixups[efix_cur[c]].row <= row; ++efix_cur[c]) {
                    const asset::SceneFixup& f = tmpl.entity_fixups[efix_cur[c]];
                    if (f.row != row) continue;
                    u32 idx;
                    std::memcpy(&idx, scratch.data() + at + f.byte_offset, 4);
                    const Entity h = idx == kNoEntity ? Entity::null : handles[idx];
                    std::memcpy(scratch.data() + at + f.byte_offset, &h, sizeof(Entity));
                }
                // Asset refs: the caller stamps runtime handles.
                for (; afix_cur[c] < afix_start[c + 1] && tmpl.asset_fixups[afix_cur[c]].row <= row; ++afix_cur[c]) {
                    const asset::SceneFixup& f = tmpl.asset_fixups[afix_cur[c]];
                    if (f.row == row && p.resolve_asset) {
                        const usize room = size - f.byte_offset;  // reader guarantees >= 16 (the Guid)
                        std::memcpy(scratch.data() + at + f.byte_offset, resolved.data() + f.index * kRefBytes,
                                    room < kRefBytes ? room : kRefBytes);
                    }
                }
                comps.push_back(RawComponent{m.id, nullptr});
                comp_offsets.push_back(static_cast<u32>(at));
                has_child_of = has_child_of || m.id == child_of;
            }
            // Overrides targeting e; components it lacks are added default-constructed.
            for (const ResolvedOverride& o : overrides) {
                if (o.src->entity != e) continue;
                usize k = 0;
                while (k < comps.size() && comps[k].id != o.comp) ++k;
                if (k == comps.size()) {
                    const usize at = align_up(scratch.size(), 16);
                    scratch.resize(at + o.type->size, 0);
                    if (o.type->construct_default) o.type->construct_default(scratch.data() + at);
                    comps.push_back(RawComponent{o.comp, nullptr});
                    comp_offsets.push_back(static_cast<u32>(at));
                    has_child_of = has_child_of || o.comp == child_of;
                }
                std::memcpy(scratch.data() + comp_offsets[k] + o.offset, o.src->value, o.src->size);
            }
            // Attach template roots under the requested parent.
            ChildOf attach{p.parent};
            if (child_of != kInvalidComponent && !has_child_of) {
                comps.push_back(RawComponent{child_of, &attach});
                comp_offsets.push_back(kNoEntity);
            }
            for (usize k = 0; k < comps.size(); ++k)
                if (comp_offsets[k] != kNoEntity) comps[k].data = scratch.data() + comp_offsets[k];
            if (deferred) w.commands().spawn_raw(handles[e], comps.span());
            else w.spawn_raw(handles[e], comps.span());
            if (out) out->push_back(handles[e]);
            ++spawned;
        }
    }
    return spawned;
}

}  // namespace mce::ecs
