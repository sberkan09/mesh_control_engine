#include "mce/scene/scene.h"

#include <cstring>

#include "mce/ecs/hierarchy.h"
#include "mce/log.h"
#include "mce/refl/refl.h"

namespace mce::scene {

namespace {

constexpr const char* kStage = "scene.instantiate";

struct ColumnPlan {
    const refl::TypeRecord* type = nullptr;
    ecs::ComponentId id = ecs::kInvalidComponent;
};

}  // namespace

ecs::ComponentId component_for(World& world, const refl::TypeRecord& type) {
    const ecs::ComponentId found = world.find_component(type.type_hash);
    if (found != ecs::kInvalidComponent) return found;
    ecs::ComponentInfo info;
    info.type_hash = type.type_hash;
    info.name = StringView(type.name);
    info.is_tag = type.fields.empty() && type.size == 1;  // MCE_COMPONENT(Tag): an empty struct
    info.size = info.is_tag ? 0u : type.size;
    info.align = type.align;
    return world.register_component_info(info);
}

Result<void> instantiate(World& world, const asset::SceneView& scene, Array<Entity>* spawned) {
    const asset::SceneHeader& h = *scene.header;

    // Validate every column against this process's registry before touching the world.
    Array<ColumnPlan> plan;
    plan.resize(h.column_count);
    u32 max_size = 0;
    for (u32 c = 0; c < h.column_count; ++c) {
        const asset::SceneColumn& col = scene.columns[c];
        const refl::TypeRecord* t = refl::find_type(StringId(col.type_id));
        if (!t || !t->is_component()) {
            MCE_LOG_ERROR("scene: column {} names a component type that is not reflected in this process ({:016x})", c,
                          col.type_id);
            return Error(Errc::NotFound, kStage);
        }
        if (t->size != col.type_size || t->align != col.type_align || t->type_hash == 0) {
            MCE_LOG_ERROR("scene: component {} was cooked with size {} align {}, this build has {} / {}: re-cook",
                          t->name, col.type_size, col.type_align, t->size, t->align);
            return Error(Errc::InvalidArgument, kStage, StringView(t->name));
        }
        plan[c].type = t;
        if (t->size > max_size) max_size = t->size;
    }

    Array<Entity> local;
    Array<Entity>& out = spawned ? *spawned : local;
    out.clear();
    out.reserve(h.entity_count);
    for (u32 e = 0; e < h.entity_count; ++e) out.push_back(world.spawn());
    for (u32 c = 0; c < h.column_count; ++c) plan[c].id = component_for(world, *plan[c].type);

    // Rows are copied into an aligned scratch row so entity fixups can be applied before the
    // write; fixups are sorted by (column, row, offset).
    Array<u64> scratch;  // u64 storage: 8-byte alignment covers every component
    scratch.resize((max_size + 7) / 8 + 1);
    usize fix = 0;
    const Span<const asset::SceneFixup> efix = scene.entity_fixups;
    for (u32 c = 0; c < h.column_count; ++c) {
        const Span<const u32> rows = scene.rows(c);
        const Span<const byte> data = scene.data(c);
        const u32 size = scene.columns[c].type_size;
        for (u32 r = 0; r < rows.size(); ++r) {
            byte* row = reinterpret_cast<byte*>(scratch.data());
            std::memcpy(row, data.data() + usize(r) * size, size);
            while (fix < efix.size() && (efix[fix].column < c || (efix[fix].column == c && efix[fix].row < r))) ++fix;
            for (; fix < efix.size() && efix[fix].column == c && efix[fix].row == r; ++fix) {
                u32 index = 0;
                std::memcpy(&index, row + efix[fix].byte_offset, sizeof(index));
                const Entity target = index < out.size() ? out[index] : Entity::null;
                if (efix[fix].byte_offset + sizeof(Entity) <= size) std::memcpy(row + efix[fix].byte_offset, &target, sizeof(Entity));
            }
            world.set_raw(out[rows[r]], plan[c].id, row);
        }
    }
    // A scene with a ChildOf column gets the transform hierarchy: WorldTransforms are composed
    // now and kept by "transform_propagate" (ADR-0030 §5). Scenes without one are untouched.
    for (u32 c = 0; c < h.column_count; ++c) {
        if (plan[c].type != &refl::type_of<ChildOf>()) continue;
        ecs::install_hierarchy(world);
        ecs::propagate_transforms(world);
        break;
    }
    return ok();
}

Result<void> load(World& world, asset::AssetSystem& assets, asset::Guid guid, Array<Entity>* spawned) {
    const asset::AssetKey key = assets.find(guid);
    if (key == asset::kInvalidAssetKey) return Error(Errc::NotFound, kStage);
    MCE_TRY_ASSIGN(asset::AssetHandle<asset::Scene> handle, assets.load_sync<asset::Scene>(key));
    Result<asset::SceneView> view = asset::SceneView::read(assets.resolve(handle));
    Result<void> r = view ? instantiate(world, view.value(), spawned) : Result<void>(view.error());
    assets.release(asset::AnyHandle(handle));
    return r;
}

}  // namespace mce::scene
