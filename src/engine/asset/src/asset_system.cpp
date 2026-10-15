#include "mce/asset/asset_system.h"

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

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
        {ParticlePreset::name, ParticlePreset::chunk}, {Sequence::name, Sequence::chunk}, {Timeline::name, Timeline::chunk},
        {SpriteSheet::name, SpriteSheet::chunk}, {AnimGraph::name, AnimGraph::chunk},
        {UiDocument::name, UiDocument::chunk}, {UiTheme::name, UiTheme::chunk},
        {Terrain::name, Terrain::chunk},
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

constexpr usize kTypeSlots = AssetSystem::kPlaceholderSlots;

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
        case ChunkType::ParticlePreset: return PARTICLE_VERSION;
        case ChunkType::Sequence: return SEQUENCE_VERSION;
        case ChunkType::Timeline: return TIMELINE_VERSION;
        case ChunkType::Shader: return SHADER_VERSION;
        case ChunkType::Mesh: return MESH_VERSION;
        case ChunkType::ModInfo: return MOD_INFO_VERSION;
        case ChunkType::StoreCatalog: return STORE_CATALOG_VERSION;
        case ChunkType::RemoteConfig: return REMOTE_CONFIG_VERSION;
        case ChunkType::GameServicesCatalog: return GAME_SERVICES_CATALOG_VERSION;
        case ChunkType::StringTable: return STRING_TABLE_VERSION;
        case ChunkType::SpriteSheet: return SPRITESHEET_VERSION;
        case ChunkType::AnimGraph: return ANIMGRAPH_VERSION;
        case ChunkType::UiDocument: return UI_DOCUMENT_VERSION;
        case ChunkType::UiTheme: return UI_THEME_VERSION;
        case ChunkType::Terrain: return TERRAIN_VERSION;
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

// The loader thread: reads, hashes and CRC-checks container files, nothing else. It never
// touches the asset tables; the main thread commits its results in poll_async().
struct AssetSystem::AsyncLoader {
    struct Job {
        u32 id = 0;
        u64 content_hash = 0;
        bool verify = true;
        FixedString<1024> file;
    };
    struct Done {
        u32 id = 0;
        Array<byte> bytes;
        Errc error = Errc::Ok;
    };

    std::mutex mutex;
    std::condition_variable wake;  // jobs queued or stop
    std::condition_variable idle;  // a job finished
    Array<Job> jobs;
    usize head = 0;
    Array<Done> done;
    u32 busy = 0;
    bool stop = false;
    std::thread thread;

    AsyncLoader() : thread([this] { run(); }) {}
    ~AsyncLoader() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        thread.join();
    }

    void push(const Job& job) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            jobs.push_back(job);
        }
        wake.notify_one();
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stop || head < jobs.size(); });
                if (stop) return;
                job = jobs[head++];
                if (head == jobs.size()) {
                    jobs.clear();
                    head = 0;
                }
                ++busy;
            }
            Done d;
            d.id = job.id;
            if (Result<void> r = read_file(job.file.view(), d.bytes); !r) {
                d.error = r.error().code;
            } else if (xxh64(d.bytes.data(), d.bytes.size()) != job.content_hash) {
                d.error = Errc::CrcMismatch;
            } else if (auto v = ContainerView::open(d.bytes.span(), job.file.view(), OpenOptions{job.verify}); !v) {
                d.error = v.error().code;
            }
            if (d.error != Errc::Ok) d.bytes = Array<byte>();
            {
                std::lock_guard<std::mutex> lock(mutex);
                done.push_back(std::move(d));
                --busy;
            }
            idle.notify_all();
        }
    }
};

AssetSystem::~AssetSystem() { loader_.reset(); }

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
        if (const AssetKey* k = by_guid_.find(e.guid())) {
            // Earlier roots win (a mod over the game), but only with the same type: an override
            // that changes it would break every reference the later root's content makes, so the
            // later root keeps that GUID instead, while nothing has loaded it yet.
            Row& prev = rows_[*k];
            bool requested = false;  // an async read in flight already names this row's file
            for (const AsyncRequest& a : async_) requested = requested || a.key == *k;
            if (prev.type == e.type || prev.state != State::Unloaded || prev.refcount != 0 || requested) continue;
            MCE_LOG_WARN("asset {}: {} is a {} in {}, but {} here; the override is ignored", asset::to_string(e.guid()).view(),
                         StringView(strings_.data() + prev.path_offset, prev.path_size), chunk_type_name(prev.type),
                         mounts_[prev.mount].root.view(), chunk_type_name(e.type));
            prev.content_hash = e.content_hash;
            prev.type = e.type;
            prev.mount = mount_index;
            const StringView path = manifest.path(i);
            prev.path_offset = static_cast<u32>(strings_.size());
            prev.path_size = static_cast<u32>(path.size());
            for (char c : path) strings_.push_back(c);
            continue;
        }
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

void AssetSystem::fail_load(Row& row, AssetKey key, Errc code) {
    row.state = State::Failed;
    row.owned.clear();
    row.payload = {};
    post(AssetEvent{AssetEvent::Kind::LoadFailed, row.guid, key, row.generation, row.generation, code});
    MCE_LOG_WARN("asset load failed: {} ({})", StringView(strings_.data() + row.path_offset, row.path_size), errc_name(code));
}

void AssetSystem::become_resident(Row& row, AssetKey key) {
    row.state = State::Resident;
    row.generation = next_generation(row.generation);
    row.stale_logged = false;
    if (auto u = upload(row, key); !u)
        MCE_LOG_WARN("gpu upload failed: {} ({})", StringView(strings_.data() + row.path_offset, row.path_size),
                     errc_name(u.error().code));
}

Result<AnyHandle> AssetSystem::load_any(AssetKey key, ChunkType type) {
    if (key >= rows_.size()) return Error(Errc::NotFound, "asset.load", {}, key);
    Row& row = rows_[key];
    const StringView path(strings_.data() + row.path_offset, row.path_size);
    if (row.type != type) return Error(Errc::InvalidArgument, "asset.load.type", path, static_cast<u32>(row.type));

    if (row.state != State::Resident) {
        auto r = make_resident(row, key);
        if (!r) {
            fail_load(row, key, r.error().code);
            return r.error();
        }
        become_resident(row, key);
    }
    ++row.refcount;
    MCE_ASSERT_MSG(row.refcount != 0, "asset refcount overflow");
    AnyHandle h;
    h.index = key;
    h.generation = row.generation;
    h.type = row.type;
    return h;
}

namespace {

// cooked/<aa>/<guid-hex>/<content-hash>.mce under a dev mount (assets.md §3.1)
void container_file(StringView root, Guid guid, u64 content_hash, FixedString<1024>& out) {
    const FixedString<40> hex = to_hex(guid);
    char name[96];  // sized for FixedString<40>'s worst case, not just a 32-char GUID
    std::snprintf(name, sizeof(name), "/%.2s/%s/%016llx.mce", hex.c_str(), hex.c_str(),
                  static_cast<unsigned long long>(content_hash));
    out.assign(root);
    out.append(name);
}

}  // namespace

Result<void> AssetSystem::make_resident(Row& row, AssetKey key) {
    (void)key;
    const Mount& m = mounts_[row.mount];
    const StringView path(strings_.data() + row.path_offset, row.path_size);
    if (m.is_pack) return bind_payload(row, m.view, path);
    FixedString<1024> file;
    container_file(m.root.view(), row.guid, row.content_hash, file);
    MCE_TRY(read_file(file.view(), row.owned));
    if (xxh64(row.owned.data(), row.owned.size()) != row.content_hash) return Error(Errc::CrcMismatch, "asset.load.content_hash", path);
    MCE_TRY_ASSIGN(const ContainerView view, ContainerView::open(row.owned.span(), path, OpenOptions{m.verify}));
    return bind_payload(row, view, path);
}

Result<void> AssetSystem::bind_payload(Row& row, ContainerView view, StringView path) {
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

// ---- async loading -----------------------------------------------------------------------------

AssetSystem::AsyncRequest* AssetSystem::find_async(AsyncTicket t) {
    for (AsyncRequest& r : async_)
        if (r.id == t.id && t.valid()) return &r;
    return nullptr;
}

const AssetSystem::AsyncRequest* AssetSystem::find_async(AsyncTicket t) const {
    return const_cast<AssetSystem*>(this)->find_async(t);
}

AsyncTicket AssetSystem::request_async(AssetKey key, ChunkType type) {
    AsyncRequest req;
    req.id = next_async_id_++;
    if (next_async_id_ == 0) next_async_id_ = 1;
    req.key = key;
    req.type = type;
    auto settle = [&](Result<AnyHandle> r) {
        if (r) {
            req.status = AsyncStatus::Ready;
            req.handle = r.value();
        } else {
            req.status = AsyncStatus::Failed;
            req.error = r.error().code;
        }
    };
    if (key >= rows_.size() || rows_[key].type != type || rows_[key].state == State::Resident || mounts_[rows_[key].mount].is_pack) {
        // Nothing to read off the main thread: resident already, a pack's bytes, or an error.
        settle(load_any(key, type));
    } else {
        const Row& row = rows_[key];
        const Mount& m = mounts_[row.mount];
        req.content_hash = row.content_hash;
        AsyncLoader::Job job;
        job.id = req.id;
        job.content_hash = row.content_hash;
        job.verify = m.verify;
        container_file(m.root.view(), row.guid, row.content_hash, job.file);
        if (!loader_) loader_ = mem::make_unique<AsyncLoader>(*alloc_);
        loader_->push(job);
    }
    async_.push_back(req);
    return AsyncTicket{req.id};
}

AsyncStatus AssetSystem::async_status(AsyncTicket t) const {
    const AsyncRequest* r = find_async(t);
    return r ? r->status : AsyncStatus::Unknown;
}

Result<AnyHandle> AssetSystem::take_async(AsyncTicket t) {
    AsyncRequest* r = find_async(t);
    if (!r || r->status == AsyncStatus::Pending) return Error(Errc::InvalidArgument, "asset.take_async");
    const AsyncRequest req = *r;
    async_.erase_swap(static_cast<usize>(r - async_.data()));
    if (req.status == AsyncStatus::Failed)
        return Error(req.error, "asset.load_async", req.key < rows_.size() ? info(req.key).path : StringView());
    return req.handle;
}

void AssetSystem::cancel_async(AsyncTicket t) {
    AsyncRequest* r = find_async(t);
    if (!r) return;
    if (r->status == AsyncStatus::Ready) release(r->handle);
    async_.erase_swap(static_cast<usize>(r - async_.data()));  // a read in flight is discarded by poll_async
}

u32 AssetSystem::async_pending() const {
    u32 n = 0;
    for (const AsyncRequest& r : async_) n += r.status == AsyncStatus::Pending ? 1u : 0u;
    return n;
}

void AssetSystem::poll_async() {
    if (!loader_) return;
    Array<AsyncLoader::Done> done;
    {
        std::lock_guard<std::mutex> lock(loader_->mutex);
        done = std::move(loader_->done);
        loader_->done = Array<AsyncLoader::Done>();
    }
    for (AsyncLoader::Done& d : done) {
        AsyncRequest* req = find_async(AsyncTicket{d.id});
        if (!req || req->status != AsyncStatus::Pending) continue;  // cancelled meanwhile
        commit_read(*req, d.bytes, d.error);
    }
}

void AssetSystem::wait_async() {
    if (!loader_) return;
    {
        std::unique_lock<std::mutex> lock(loader_->mutex);
        loader_->idle.wait(lock, [this] { return loader_->head == loader_->jobs.size() && loader_->busy == 0; });
    }
    poll_async();
}

void AssetSystem::commit_read(AsyncRequest& req, Array<byte>& bytes, Errc read_error) {
    Row& row = rows_[req.key];
    auto settle = [&](Result<AnyHandle> r) {
        if (r) {
            req.status = AsyncStatus::Ready;
            req.handle = r.value();
        } else {
            req.status = AsyncStatus::Failed;
            req.error = r.error().code;
        }
    };
    // A load_sync got there first, or a reload re-cooked the asset while the old file was read:
    // the synchronous path does the right thing (a refcount bump, or a read of the current file).
    if (row.state == State::Resident || row.content_hash != req.content_hash) {
        settle(load_any(req.key, req.type));
        return;
    }
    if (read_error != Errc::Ok) {
        fail_load(row, req.key, read_error);
        req.status = AsyncStatus::Failed;
        req.error = read_error;
        return;
    }
    const StringView path(strings_.data() + row.path_offset, row.path_size);
    row.owned = std::move(bytes);
    Result<void> bound = [&]() -> Result<void> {
        // The loader verified the payload CRC already.
        MCE_TRY_ASSIGN(const ContainerView view, ContainerView::open(row.owned.span(), path, OpenOptions{false}));
        return bind_payload(row, view, path);
    }();
    if (!bound) {
        fail_load(row, req.key, bound.error().code);
        req.status = AsyncStatus::Failed;
        req.error = bound.error().code;
        return;
    }
    become_resident(row, req.key);
    ++row.refcount;
    AnyHandle h;
    h.index = req.key;
    h.generation = row.generation;
    h.type = row.type;
    req.status = AsyncStatus::Ready;
    req.handle = h;
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
    poll_async();
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
