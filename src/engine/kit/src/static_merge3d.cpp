// StaticMerge3D (M-VISUAL3 5b): the extract's side of the runtime merge. render::build_merged_mesh() does the
// geometry; this file picks the children that qualify, sorts them into cells, uploads the merged meshes under
// local keys, remembers which children they replace and re-merges when the children change.

#include <cmath>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/render_textures.h"
#include "mce/kit/static_merge3d.h"
#include "mce/log.h"
#include "mce/render/static_merge.h"

namespace mce::kit {

using namespace mce::math;

namespace {

// The entity's pose: WorldTransform when the hierarchy wrote one, else LocalTransform (as kit3d_extract.cpp reads it).
bool pose_of(World& world, Entity e, vec3& pos, quat& rot, vec3& scale) {
    if (const WorldTransform* w = world.try_get<WorldTransform>(e)) {
        pos = w->position;
        rot = w->rotation;
        scale = w->scale;
        return true;
    }
    if (const LocalTransform* l = world.try_get<LocalTransform>(e)) {
        pos = l->position;
        rot = l->rotation;
        scale = l->scale;
        return true;
    }
    return false;
}

struct Hasher {
    u64 h = 1469598103934665603ull;
    void mix(const void* p, usize n) {
        for (usize i = 0; i < n; ++i) h = (h ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
    }
    template <class T>
    void mix(const T& v) {
        mix(&v, sizeof(T));
    }
};

// One child that merges.
struct Candidate {
    Entity entity;
    mat4 world;
    const MeshRef* ref = nullptr;
    const render::MaterialOverride* material = nullptr;
    asset::MeshView view;
    vec3 center;  // the mesh's bounds centre in the world: picks the cell
};

// Children sharing a cell and a shadow flag: one merged mesh.
struct Bucket {
    i32 cell[3] = {0, 0, 0};
    bool cast = true;
    Array<u32> candidates;
};

}  // namespace

struct Kit3DExtract::MergeState {
    struct Cell {
        u32 key = ~0u;  // mesh-store key of the merged mesh
        vec3 center;
        f32 radius = 0.0f;
        bool cast = true;
        u8 lod = 0;  // the level drawn last frame (hysteresis)
        Array<u32> children;  // entity indices of the MeshRefs it replaces
    };
    struct Group {
        Entity entity;
        u64 hash = 0;
        bool built = false;
        u32 stamp = 0;  // the update that last saw the component
        Array<Cell> cells;
    };
    Array<mem::UniquePtr<Group>> groups;
    u32 stamp = 0;
    Array<Candidate> candidates;  // scratch
    Array<Bucket> buckets;
    Array<render::MergeSource> sources;
    Array<byte> blob;
    Array<Entity> stack;
};

Kit3DExtract::Kit3DExtract() = default;

Kit3DExtract::~Kit3DExtract() { shutdown(); }

void Kit3DExtract::set_static_merge(bool on) {
    static_merge_ = on;
    if (!on) release_static_merges();
}

void Kit3DExtract::release_static_merges() {
    if (merge_) {
        if (writable_)
            for (auto& g : merge_->groups)
                for (const MergeState::Cell& c : g->cells)
                    if (c.key != ~0u) writable_->release(c.key);
        merge_.reset();
    }
    merged_draws_.clear();
    merged_stamp_.clear();
    merge_stamp_ = 0;
}

// The cooked mesh `guid` (loaded already by mesh_key) as a view over the asset system's bytes; false when the mesh is
// bound by hand (bind_mesh) or does not read.
bool Kit3DExtract::read_mesh_view(asset::Guid guid, asset::MeshView& out) {
    if (!assets_) return false;
    for (const MeshEntry& m : mesh_entries_) {
        if (!(m.guid == guid)) continue;
        if (m.handle.generation == 0) return false;
        const Result<asset::MeshView> v =
            asset::MeshView::read(assets_->resolve(asset::AssetHandle<asset::Mesh>{m.handle.index, m.handle.generation}));
        if (!v) return false;
        out = v.value();
        return true;
    }
    return false;
}

void Kit3DExtract::update_static_merges(World& world) {
    merged_draws_.clear();
    if (!static_merge_ || !writable_ || !assets_) {
        if (merge_) release_static_merges();
        return;
    }
    if (!merge_) {
        usize components = 0;
        world.query<const StaticMerge3D&>().each([&](const StaticMerge3D&) { ++components; });
        if (components == 0) {
            merge_stamp_ = 0;
            return;
        }
        merge_ = mem::make_unique<MergeState>(mem::heap());
    }
    MergeState& st = *merge_;
    ++st.stamp;
    if (st.stamp == 0) st.stamp = 1;
    merge_stamp_ = merge_stamp_ + 1 == 0 ? 1 : merge_stamp_ + 1;
    const bool hierarchy = ecs::hierarchy_installed(world);

    render::TextureKeyResolver resolver;
    resolver.ctx = this;
    resolver.fn = [](void* ctx, asset::Guid g) -> u32 { return static_cast<Kit3DExtract*>(ctx)->texture_key(g); };

    auto claimed = [&](u32 index) { return index < merged_stamp_.size() && merged_stamp_[index] == merge_stamp_; };

    world.query<const StaticMerge3D&>().each([&](Entity root, const StaticMerge3D& m) {
        if (!m.enabled) return;
        MergeState::Group* group = nullptr;
        for (auto& g : st.groups)
            if (g->entity == root) group = g.get();
        if (!group) {
            st.groups.push_back(mem::make_unique<MergeState::Group>(mem::heap()));
            group = st.groups.back().get();
            group->entity = root;
        }
        group->stamp = st.stamp;

        // ---- the children that qualify, in hierarchy order -------------------------------------------------------
        st.candidates.clear();
        Hasher hash;
        hash.mix(m.cell_size);
        hash.mix(m.cast_shadows);
        hash.mix(m.version);
        if (hierarchy) {
            st.stack.clear();
            const Span<const Entity> top = ecs::children_of(world, root);
            for (usize i = top.size(); i-- > 0;) st.stack.push_back(top[i]);
            while (!st.stack.empty()) {
                const Entity d = st.stack.back();
                st.stack.pop_back();
                const Span<const Entity> kids = ecs::children_of(world, d);
                for (usize i = kids.size(); i-- > 0;) st.stack.push_back(kids[i]);

                const MeshRef* r = world.try_get<MeshRef>(d);
                if (!r || !r->visible || r->interpolate != 0 || world.try_get<Animator>(d)) continue;
                if (claimed(d.index) || (render_textures_ && world.try_get<ShowRenderTexture>(d))) continue;
                vec3 pos, scale;
                quat rot;
                if (!pose_of(world, d, pos, rot, scale)) continue;
                const u32 key = mesh_key(r->mesh.guid);
                const render::GpuMesh* mesh = key != ~0u && meshes_ ? meshes_->get(key) : nullptr;
                if (!mesh || mesh->skinned()) continue;
                Candidate c;
                if (!read_mesh_view(r->mesh.guid, c.view) || c.view.skinned()) continue;
                // Opaque or masked only: blended and transmissive draws are sorted per draw, behind the opaques.
                c.material = r->material.guid != asset::Guid{} ? material(r->material.guid) : nullptr;
                bool opaque = true;
                if (c.material) {
                    opaque = !(c.material->params.flags & asset::MaterialFlags::AlphaBlend) && c.material->ext.transmission <= 0.0f &&
                             !c.material->parallax;
                } else {
                    for (u32 s = 0; s < c.view.submeshes.size() && opaque; ++s)
                        opaque = !(c.view.submeshes[s].material.flags & asset::MaterialFlags::AlphaBlend) &&
                                 c.view.material_ext(s).transmission <= 0.0f;
                }
                if (!opaque) continue;
                c.entity = d;
                c.ref = r;
                c.world = trs(pos, rot, scale);
                c.center = transform_point(c.world, mesh->bounds.center());
                hash.mix(d);
                hash.mix(r->mesh.guid);
                hash.mix(r->material.guid);
                hash.mix(c.world);
                hash.mix(r->tint);
                hash.mix(r->emissive_tint);
                hash.mix(r->cast_shadows);
                hash.mix(mesh->generation);
                st.candidates.push_back(c);
            }
        }

        // ---- merge, unless the children are the ones merged last time ---------------------------------------------
        const u64 h = hash.h;
        if (!group->built || group->hash != h) {
            for (const MergeState::Cell& c : group->cells)
                if (c.key != ~0u) writable_->release(c.key);
            group->cells.clear();
            group->hash = h;
            group->built = true;
            st.buckets.clear();
            const f32 inv = 1.0f / max(m.cell_size, 0.5f);
            for (u32 i = 0; i < st.candidates.size(); ++i) {
                const Candidate& c = st.candidates[i];
                Bucket key;
                for (int k = 0; k < 3; ++k) key.cell[k] = static_cast<i32>(std::floor(clamp((&c.center.x)[k] * inv, -1.0e6f, 1.0e6f)));
                key.cast = m.cast_shadows != 0 && c.ref->cast_shadows != 0;
                Bucket* b = nullptr;
                for (Bucket& o : st.buckets)
                    if (o.cell[0] == key.cell[0] && o.cell[1] == key.cell[1] && o.cell[2] == key.cell[2] && o.cast == key.cast) b = &o;
                if (!b) {
                    st.buckets.push_back(Bucket{});
                    b = &st.buckets.back();
                    for (int k = 0; k < 3; ++k) b->cell[k] = key.cell[k];
                    b->cast = key.cast;
                }
                b->candidates.push_back(i);
            }
            for (const Bucket& b : st.buckets) {
                st.sources.clear();
                for (u32 i : b.candidates) {
                    const Candidate& c = st.candidates[i];
                    render::MergeSource s;
                    s.mesh = &c.view;
                    s.world = c.world;
                    s.tint = c.ref->tint;
                    s.emissive_tint = c.ref->emissive_tint;
                    if (c.material) {
                        s.material = &c.material->params;
                        s.material_ext = &c.material->ext;
                    }
                    st.sources.push_back(s);
                }
                render::MergeStats stats;
                if (!render::build_merged_mesh(st.sources.span(), st.blob, &stats) || stats.skipped != 0) {
                    MCE_LOG_WARN("kit3d: StaticMerge3D on entity {}: a cell of {} children did not merge; they draw on their own",
                                 root.index, b.candidates.size());
                    continue;
                }
                const u32 key = writable_->allocate_key();
                if (!writable_->upload(key, 1, st.blob.span(), resolver)) {
                    MCE_LOG_WARN("kit3d: StaticMerge3D on entity {}: the merged mesh did not upload; its children draw on their own",
                                 root.index);
                    continue;
                }
                const render::GpuMesh* mesh = writable_->get(key);
                MergeState::Cell cell;
                cell.key = key;
                cell.center = mesh->bounds.center();
                cell.radius = length(mesh->bounds.extents());
                cell.cast = b.cast;
                for (u32 i : b.candidates) cell.children.push_back(st.candidates[i].entity.index);
                group->cells.push_back(static_cast<MergeState::Cell&&>(cell));
            }
        }

        // ---- the children it replaces, and what to draw in their place --------------------------------------------
        u32 children = 0;
        for (MergeState::Cell& c : group->cells) {
            for (u32 index : c.children) {
                if (merged_stamp_.size() <= index) merged_stamp_.resize(index + 1, 0);
                merged_stamp_[index] = merge_stamp_;
            }
            children += static_cast<u32>(c.children.size());
            MergedDraw d;
            d.key = c.key;
            d.center = c.center;
            d.radius = c.radius;
            d.cast_shadows = c.cast;
            const render::GpuMesh* mesh = meshes_->get(c.key);
            d.lod_state = mesh && mesh->lod_levels > 1 ? &c.lod : nullptr;
            merged_draws_.push_back(d);
        }
        if (!group->cells.empty()) ++stats_.merged_groups;
        stats_.merged_children += children;
        stats_.merged_cells += static_cast<u32>(group->cells.size());
    });

    // Groups whose component is gone or disabled: release their meshes, the children draw on their own again.
    for (usize i = st.groups.size(); i-- > 0;) {
        if (st.groups[i]->stamp == st.stamp) continue;
        for (const MergeState::Cell& c : st.groups[i]->cells)
            if (c.key != ~0u) writable_->release(c.key);
        st.groups.erase(i);
    }
}

}  // namespace mce::kit
