#include "mce/asset/asset_system.h"

#include <cstdio>
#include <cstring>

#include "mce/asset/file_io.h"
#include "mce/asset/xxhash.h"
#include "mce/log.h"

namespace mce::asset {

bool chunk_type_for_asset_name(StringView name, ChunkType& out) {
    struct Pair {
        const char* name;
        ChunkType type;
    };
    static constexpr Pair kTable[] = {
        {Texture::name, Texture::chunk}, {Mesh::name, Mesh::chunk}, {Material::name, Material::chunk},
        {Shader::name, Shader::chunk},   {Sfx::name, Sfx::chunk},   {Font::name, Font::chunk},
        {Atlas::name, Atlas::chunk},     {Scene::name, Scene::chunk}, {InputMap::name, InputMap::chunk},
        {Prefab::name, Prefab::chunk},   {Tilemap::name, Tilemap::chunk},
    };
    for (const Pair& p : kTable) {
        if (name == p.name) {
            out = p.type;
            return true;
        }
    }
    return false;
}

namespace {

constexpr usize kTypeSlots = 16;

usize slot(ChunkType t) {
    const usize i = static_cast<usize>(t);
    MCE_ASSERT_MSG(i < kTypeSlots, "chunk type out of placeholder range");
    return i;
}

// Schema major this build reads, per chunk type (0 = no reader yet: any version accepted, the
// payload is handed through opaquely — e.g. meshes until the glTF importer lands).
u8 supported_major(ChunkType t) {
    switch (t) {
        case ChunkType::Manifest: return MANIFEST_VERSION;
        case ChunkType::Texture: return TEXTURE_VERSION;
        case ChunkType::Sfx: return SFX_VERSION;
        case ChunkType::Font: return FONT_VERSION;
        case ChunkType::Material: return MATERIAL_VERSION;
        case ChunkType::Scene: return SCENE_VERSION;
        case ChunkType::Prefab: return PREFAB_VERSION;
        case ChunkType::Tilemap: return TILEMAP_VERSION;
        case ChunkType::Mesh: return MESH_VERSION;
        default: return 0;
    }
}

void build_placeholders(Array<byte>* out) {
    {  // magenta/black 8x8 checker, one mip
        constexpr u32 N = 8;
        u8 px[N * N * 4];
        for (u32 y = 0; y < N; ++y) {
            for (u32 x = 0; x < N; ++x) {
                const bool on = ((x / 2) + (y / 2)) % 2 == 0;
                u8* p = px + (y * N + x) * 4;
                p[0] = on ? 255 : 0;
                p[1] = 0;
                p[2] = on ? 255 : 0;
                p[3] = 255;
            }
        }
        const TextureMipData mip{N, N, Span<const byte>(reinterpret_cast<const byte*>(px), sizeof(px))};
        write_texture(TextureFormat::RGBA8, 1, 0, Span<const TextureMipData>(&mip, 1), out[slot(ChunkType::Texture)]);
    }
    {  // one second of mono silence at 48 kHz
        Array<f32> silence;
        silence.resize(48000, 0.0f);
        SfxHeader h{};
        h.sample_rate = 48000;
        h.channels = 1;
        h.frame_count = 48000;
        h.gain = 1.0f;
        write_sfx(h, silence.span(), out[slot(ChunkType::Sfx)]);
    }
    {  // fallback material: unlit magenta
        MaterialChunk m{};
        m.base_color[0] = 1.0f;
        m.base_color[2] = 1.0f;
        m.base_color[3] = 1.0f;
        m.roughness = 1.0f;
        m.flags = MaterialFlags::Unlit;
        Array<byte>& b = out[slot(ChunkType::Material)];
        b.resize(sizeof(m));
        std::memcpy(b.data(), &m, sizeof(m));
    }
    {  // empty font: no glyphs, 1x1 blank atlas
        FontHeader h{};
        h.pixel_height = 16.0f;
        h.atlas_width = 1;
        h.atlas_height = 1;
        const byte blank{0};
        write_font(h, {}, {}, Span<const byte>(&blank, 1), out[slot(ChunkType::Font)]);
    }
    {  // empty scene
        SceneHeader h{};
        h.columns_offset = h.asset_refs_offset = h.asset_fixups_offset = h.entity_fixups_offset = h.names_offset = sizeof(h);
        Array<byte>& b = out[slot(ChunkType::Scene)];
        b.resize(sizeof(h));
        std::memcpy(b.data(), &h, sizeof(h));
    }
    out[slot(ChunkType::Prefab)] = out[slot(ChunkType::Scene)];  // empty prefab: same layout
    {  // empty mesh: no geometry, no nodes (draws nothing)
        write_mesh(MeshData{}, out[slot(ChunkType::Mesh)]);
    }
    // Shader/...: empty payload until their formats land.
}

}  // namespace

AssetSystem::AssetSystem(mem::IAllocator& a)
    : alloc_(&a), mounts_(a), rows_(a), strings_(a), by_guid_(a), pending_unload_(a), retired_(a) {
    build_placeholders(placeholders_);
}

AssetSystem::~AssetSystem() = default;

Result<void> AssetSystem::mount(Span<const StringView> roots, MountOptions opt) {
    for (StringView r : roots) MCE_TRY(mount_one(r, opt));
    return ok();
}

Result<void> AssetSystem::mount_one(StringView root, MountOptions opt) {
    Mount m;
    m.root.assign(root);
    m.verify = opt.verify_payload_crc;
    if (m.root.truncated()) return Error(Errc::InvalidArgument, "asset.mount", root);

    // A path ending in ".mce" is a pack; anything else is a dev cooked directory.
    m.is_pack = root.size() >= 4 && root.substr(root.size() - 4) == ".mce";
    FixedString<1024> manifest_path(root);
    if (!m.is_pack) manifest_path.append("/manifest.mce");
    MCE_TRY(read_file(manifest_path.view(), m.bytes));

    // A pack verifies its payload CRC once here and is trusted afterwards.
    MCE_TRY_ASSIGN(m.view, ContainerView::open(m.bytes.span(), manifest_path.view(), OpenOptions{true}));
    const u32 mi = m.view.find(ChunkType::Manifest);
    if (mi == m.view.chunk_count()) return Error(Errc::MissingRef, "asset.mount.manifest", manifest_path.view());
    MCE_TRY(ContainerView::check_version(m.view.entry(mi), MANIFEST_VERSION, manifest_path.view()));
    MCE_TRY_ASSIGN(ManifestView manifest, ManifestView::read(m.view.payload(mi)));

    const u32 mount_index = static_cast<u32>(mounts_.size());
    for (u32 i = 0; i < manifest.size(); ++i) {
        const ManifestEntry& e = manifest.entry(i);
        if (by_guid_.contains(e.guid())) continue;  // earlier roots win
        Row row;
        row.guid = e.guid();
        row.content_hash = e.content_hash;
        row.type = e.type;
        row.mount = mount_index;
        const StringView path = manifest.path(i);
        row.path_offset = static_cast<u32>(strings_.size());
        row.path_size = static_cast<u32>(path.size());
        for (char c : path) strings_.push_back(c);
        by_guid_.insert_or_assign(row.guid, static_cast<AssetKey>(rows_.size()));
        rows_.push_back(std::move(row));
    }
    mounts_.push_back(std::move(m));
    return ok();
}

AssetKey AssetSystem::find(Guid guid) const {
    const AssetKey* k = by_guid_.find(guid);
    return k ? *k : kInvalidAssetKey;
}

AssetInfo AssetSystem::info(AssetKey key) const {
    MCE_ASSERT_MSG(key < rows_.size(), "AssetSystem::info: bad key");
    const Row& r = rows_[key];
    return AssetInfo{r.guid, r.type, r.content_hash, StringView(strings_.data() + r.path_offset, r.path_size)};
}

Result<AnyHandle> AssetSystem::load_any(AssetKey key, ChunkType type) {
    if (key >= rows_.size()) return Error(Errc::NotFound, "asset.load", {}, key);
    Row& row = rows_[key];
    const StringView path(strings_.data() + row.path_offset, row.path_size);
    if (row.type != type) return Error(Errc::InvalidArgument, "asset.load.type", path, static_cast<u32>(row.type));

    if (row.state != State::Resident) {
        auto r = make_resident(row, key);
        if (!r) {
            row.state = State::Failed;
            row.owned.clear();
            row.payload = {};
            post(AssetEvent{AssetEvent::Kind::LoadFailed, row.guid, key, row.generation, row.generation, r.error().code});
            MCE_LOG_WARN("asset load failed: {} ({})", path, errc_name(r.error().code));
            return r.error();
        }
        row.state = State::Resident;
        row.generation = next_generation(row.generation);
        row.stale_logged = false;
        if (auto u = upload(row, key); !u) MCE_LOG_WARN("gpu upload failed: {} ({})", path, errc_name(u.error().code));
    }
    ++row.refcount;
    MCE_ASSERT_MSG(row.refcount != 0, "asset refcount overflow");
    AnyHandle h;
    h.index = key;
    h.generation = row.generation;
    h.type = row.type;
    return h;
}

Result<void> AssetSystem::make_resident(Row& row, AssetKey key) {
    (void)key;
    const Mount& m = mounts_[row.mount];
    const StringView path(strings_.data() + row.path_offset, row.path_size);
    ContainerView view;
    if (m.is_pack) {
        view = m.view;
    } else {
        // cooked/<aa>/<guid-hex>/<content-hash>.mce (assets.md §3.1)
        const FixedString<40> hex = to_hex(row.guid);
        char name[96];  // sized for FixedString<40>'s worst case, not just a 32-char GUID
        std::snprintf(name, sizeof(name), "/%.2s/%s/%016llx.mce", hex.c_str(), hex.c_str(),
                      static_cast<unsigned long long>(row.content_hash));
        FixedString<1024> file(m.root.view());
        file.append(name);
        MCE_TRY(read_file(file.view(), row.owned));
        if (xxh64(row.owned.data(), row.owned.size()) != row.content_hash) return Error(Errc::CrcMismatch, "asset.load.content_hash", path);
        MCE_TRY_ASSIGN(view, ContainerView::open(row.owned.span(), path, OpenOptions{m.verify}));
    }
    const u32 ci = view.find(row.type, row.guid);
    if (ci == view.chunk_count()) return Error(Errc::MissingRef, "asset.load.chunk", path);
    if (const u8 major = supported_major(row.type); major != 0) MCE_TRY(ContainerView::check_version(view.entry(ci), major, path));
    row.payload = view.payload(ci);
    return ok();
}

Result<void> AssetSystem::upload(const Row& row, AssetKey key) {
    if (!sink_) return ok();
    switch (row.type) {
        case ChunkType::Texture: {
            MCE_TRY_ASSIGN(TextureView tv, TextureView::read(row.payload));
            return sink_->upload_texture(key, row.payload, tv.header->flags);
        }
        case ChunkType::Mesh: return sink_->upload_mesh(key, row.payload);
        case ChunkType::Material: return sink_->update_material(key, row.payload);
        default: return ok();
    }
}

void AssetSystem::destroy_gpu(AssetKey key, u32 generation, ChunkType type) {
    if (!sink_) return;
    if (type != ChunkType::Texture && type != ChunkType::Mesh && type != ChunkType::Material) return;
    const RetiredAsset r{key, generation, type};
    sink_->destroy_gpu_objects(Span<const RetiredAsset>(&r, 1));
}

void AssetSystem::retain(AnyHandle h) {
    MCE_ASSERT_MSG(h.index < rows_.size() && rows_[h.index].state == State::Resident, "retain on a non-resident asset");
    ++rows_[h.index].refcount;
    MCE_ASSERT_MSG(rows_[h.index].refcount != 0, "asset refcount overflow");
}

void AssetSystem::release(AnyHandle h) {
    MCE_ASSERT_MSG(h.index < rows_.size(), "release: bad handle");
    Row& row = rows_[h.index];
    MCE_ASSERT_MSG(row.state == State::Resident && row.refcount > 0, "release without a matching load/retain");
    if (--row.refcount == 0) pending_unload_.push_back(Pending{h.index, row.generation, MCE_FRAMES_IN_FLIGHT});
}

u32 AssetSystem::refcount(AssetKey key) const { return key < rows_.size() ? rows_[key].refcount : 0; }

u32 AssetSystem::generation(AssetKey key) const { return key < rows_.size() ? rows_[key].generation : 0; }

bool AssetSystem::is_alive(AnyHandle h) const {
    if (h.generation == 0 || h.index >= rows_.size()) return false;
    const Row& r = rows_[h.index];
    return r.state == State::Resident && r.generation == h.generation && r.type == h.type;
}

Span<const byte> AssetSystem::placeholder_for(ChunkType type) const { return placeholders_[slot(type)].span(); }

Span<const byte> AssetSystem::resolve_any(AnyHandle h) const {
    if (is_alive(h)) return rows_[h.index].payload;
    if (h.generation != 0 && h.index < rows_.size()) {
        const Row& r = rows_[h.index];
        if (!r.stale_logged) {
            r.stale_logged = true;
            MCE_LOG_WARN("stale asset handle -> placeholder: {} (gen {} vs live {})",
                         StringView(strings_.data() + r.path_offset, r.path_size), h.generation, r.generation);
        }
    }
    return placeholder_for(h.type);
}

void AssetSystem::refresh(AnyHandle& h, Guid guid) const {
    if (is_alive(h)) return;
    AssetKey key = h.index;
    if (h.generation == 0 || key >= rows_.size() || rows_[key].guid != guid) key = find(guid);
    if (key == kInvalidAssetKey) return;
    const Row& r = rows_[key];
    if (r.state == State::Resident && r.type == h.type) {
        h.index = key;
        h.generation = r.generation;
    }
}

Result<void> AssetSystem::commit_reload(Guid guid, Span<const byte> container_bytes) {
    const AssetKey key = find(guid);
    if (key == kInvalidAssetKey) return Error(Errc::NotFound, "asset.reload", to_string(guid).view());
    Row& row = rows_[key];
    const StringView path(strings_.data() + row.path_offset, row.path_size);

    Array<byte> fresh(*alloc_);
    fresh.resize_uninitialized(container_bytes.size());
    if (!container_bytes.empty()) std::memcpy(fresh.data(), container_bytes.data(), container_bytes.size());
    auto fail = [&](const Error& e) -> Result<void> {
        post(AssetEvent{AssetEvent::Kind::LoadFailed, guid, key, row.generation, row.generation, e.code});
        return e;
    };
    auto view = ContainerView::open(fresh.span(), path);
    if (!view) return fail(view.error());
    const u32 ci = view.value().find(row.type, guid);
    if (ci == view.value().chunk_count()) return fail(Error(Errc::MissingRef, "asset.reload.chunk", path));
    if (const u8 major = supported_major(row.type); major != 0) {
        if (auto v = ContainerView::check_version(view.value().entry(ci), major, path); !v) return fail(v.error());
    }
    const u64 new_hash = xxh64(fresh.data(), fresh.size());

    if (row.state != State::Resident) {
        // Not loaded: the next load picks the new cooked file up by its content hash.
        if (mounts_[row.mount].is_pack) return fail(Error(Errc::Unsupported, "asset.reload.pack", path));
        row.content_hash = new_hash;
        return ok();
    }

    const u32 old_gen = row.generation;
    retired_.push_back(Retired{key, old_gen, row.type, MCE_FRAMES_IN_FLIGHT, std::move(row.owned)});
    row.owned = std::move(fresh);
    row.payload = view.value().payload(ci);
    row.content_hash = new_hash;
    row.generation = next_generation(old_gen);
    row.stale_logged = false;
    if (auto u = upload(row, key); !u) MCE_LOG_WARN("gpu upload failed on reload: {} ({})", path, errc_name(u.error().code));
    post(AssetEvent{AssetEvent::Kind::Reloaded, guid, key, old_gen, row.generation, Errc::Ok});
    return ok();
}

void AssetSystem::end_frame() {
    for (usize i = 0; i < pending_unload_.size();) {
        Pending& p = pending_unload_[i];
        if (--p.frames_left != 0) {
            ++i;
            continue;
        }
        Row& row = rows_[p.key];
        if (row.state == State::Resident && row.generation == p.generation && row.refcount == 0) {
            post(AssetEvent{AssetEvent::Kind::Unloading, row.guid, p.key, row.generation, row.generation, Errc::Ok});
            destroy_gpu(p.key, row.generation, row.type);
            row.owned = Array<byte>(*alloc_);
            row.payload = {};
            row.state = State::Unloaded;
            row.generation = next_generation(row.generation);  // stragglers go stale, never alias
        } else if (row.refcount > 0) {
            post(AssetEvent{AssetEvent::Kind::UnloadFailed, row.guid, p.key, row.generation, row.generation, Errc::Ok});
            MCE_LOG_INFO("asset unload skipped, re-acquired: {}", StringView(strings_.data() + row.path_offset, row.path_size));
        }
        pending_unload_.erase_swap(i);
    }
    for (usize i = 0; i < retired_.size();) {
        Retired& r = retired_[i];
        if (--r.frames_left != 0) {
            ++i;
            continue;
        }
        destroy_gpu(r.key, r.generation, r.type);
        retired_.erase_swap(i);
    }
    read_buf_ = write_buf_;
    write_buf_ ^= 1u;
    events_[write_buf_].clear();
}

}  // namespace mce::asset
