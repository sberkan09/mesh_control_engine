#pragma once
// Runtime asset facade (assets.md §7): mount cooked trees, sync load, refcount, deferred unload,
// generational handles, hot-reload commit, per-type placeholders.
//
// Main thread only: the main thread is the sole mutator of the tables, so the read path takes
// no locks (assets.md §8.1). Async loading (v0.5) adds a loader thread that only builds
// ready-to-commit entries.

#include "mce/asset/formats.h"
#include "mce/asset/gpu_upload_sink.h"
#include "mce/asset/handle.h"
#include "mce/containers/hash_map.h"

namespace mce::asset {

struct AssetEvent {
    enum class Kind : u8 {
        Reloaded,      // commit_reload swapped in a new generation
        Unloading,     // posted before the blob and GPU objects are destroyed
        UnloadFailed,  // refcount resurrected between release and reap; asset stays resident
        LoadFailed,    // load_sync / commit_reload failed (error says why)
    };
    Kind kind = Kind::LoadFailed;
    Guid guid;
    AssetKey key = kInvalidAssetKey;
    u32 old_generation = 0;
    u32 new_generation = 0;
    Errc error = Errc::Ok;
};

struct AssetInfo {
    Guid guid;
    ChunkType type = ChunkType::Manifest;
    u64 content_hash = 0;
    StringView path;  // source path relative to assets/ (diagnostics only)
};

struct MountOptions {
    // Dev mounts verify every container's payload CRC on load. A shipped pack is verified once at
    // mount and trusted afterwards (assets.md §3, the < 1 s boot budget).
    bool verify_payload_crc = true;
};

class AssetSystem {
public:
    explicit AssetSystem(mem::IAllocator& a = mem::heap());
    ~AssetSystem();
    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;

    // Each root is either a dev cooked directory (holding manifest.mce) or a pack file
    // (assets.mce). Earlier roots win when two roots carry the same GUID.
    Result<void> mount(Span<const StringView> roots, MountOptions opt = {});

    u32 asset_count() const { return static_cast<u32>(rows_.size()); }
    AssetKey find(Guid guid) const;
    AssetInfo info(AssetKey key) const;

    // Loads (or bumps the refcount of an already resident asset) and returns a handle holding one
    // reference: pair every successful load_sync with a release.
    template <class T>
    Result<AssetHandle<T>> load_sync(AssetKey key) {
        MCE_TRY_ASSIGN(AnyHandle h, load_any(key, T::chunk));
        return AssetHandle<T>{h.index, h.generation};
    }
    template <class T>
    Result<AssetHandle<T>> load_sync(Guid guid) {
        return load_sync<T>(find(guid));
    }

    void retain(AnyHandle h);
    // Decrements; at zero the asset is queued and reaped MCE_FRAMES_IN_FLIGHT end_frame()s later.
    void release(AnyHandle h);
    u32 refcount(AssetKey key) const;
    bool is_alive(AnyHandle h) const;
    u32 generation(AssetKey key) const;

    // Payload of the asset's primary chunk. A null/stale handle resolves to the type's
    // placeholder and logs once per asset generation — never a crash (FR-DX-050).
    template <class T>
    Span<const byte> resolve(AssetHandle<T> h) const {
        return resolve_any(AnyHandle(h));
    }
    // Component-side resolve: re-stamps a handle left stale by a reload to the live generation
    // (the swap is one frame wide and gameplay code does nothing — assets.md §7.5).
    template <class T>
    Span<const byte> resolve(AssetRef<T>& ref) const {
        AnyHandle h(ref.handle);
        refresh(h, ref.guid);
        ref.handle = AssetHandle<T>{h.index, h.generation};
        return resolve_any(h);
    }

    template <class T>
    Span<const byte> placeholder() const {
        return placeholder_for(T::chunk);
    }
    Span<const byte> placeholder_for(ChunkType type) const;

    // Hot reload commit (editor only, main thread, frame start): swaps in a freshly cooked
    // container for `guid` and bumps the generation. The old blob stays alive for
    // MCE_FRAMES_IN_FLIGHT frames. All-or-nothing: on error the old generation keeps serving.
    Result<void> commit_reload(Guid guid, Span<const byte> container_bytes);

    // Frame end: reaps queued unloads and retired reload blobs whose frame slot came round, and
    // flips the event buffer.
    void end_frame();

    // Events posted during the previous frame (double-buffered).
    Span<const AssetEvent> events() const { return events_[read_buf_].span(); }

    void set_upload_sink(IGpuUploadSink* sink) { sink_ = sink; }

private:
    enum class State : u8 { Unloaded, Resident, Failed };

    struct Row {
        Guid guid;
        u64 content_hash = 0;
        ChunkType type = ChunkType::Manifest;
        u32 mount = 0;
        u32 path_offset = 0;
        u32 path_size = 0;
        u32 generation = 0;
        u32 refcount = 0;
        State state = State::Unloaded;
        mutable bool stale_logged = false;
        Array<byte> owned;          // dev mounts: the container file's bytes
        Span<const byte> payload;   // primary chunk payload (into `owned` or the pack)
    };
    struct Mount {
        FixedString<512> root;
        bool is_pack = false;
        bool verify = true;
        Array<byte> bytes;  // pack: whole file; dev: manifest.mce
        ContainerView view;  // over `bytes` (heap storage: survives moves of the Mount)
    };
    struct Pending {
        AssetKey key;
        u32 generation;
        u32 frames_left;
    };
    struct Retired {
        AssetKey key;
        u32 generation;
        ChunkType type;
        u32 frames_left;
        Array<byte> bytes;
    };

    Result<void> mount_one(StringView root, MountOptions opt);
    Result<AnyHandle> load_any(AssetKey key, ChunkType type);
    Result<void> make_resident(Row& row, AssetKey key);
    Result<void> upload(const Row& row, AssetKey key);
    Span<const byte> resolve_any(AnyHandle h) const;
    void refresh(AnyHandle& h, Guid guid) const;
    void post(const AssetEvent& e) { events_[write_buf_].push_back(e); }
    void destroy_gpu(AssetKey key, u32 generation, ChunkType type);
    static u32 next_generation(u32 g) { return g + 1 == 0 ? 1 : g + 1; }

    mem::IAllocator* alloc_;
    Array<Mount> mounts_;
    Array<Row> rows_;
    Array<char> strings_;
    HashMap<Guid, AssetKey> by_guid_;
    Array<Pending> pending_unload_;
    Array<Retired> retired_;
    Array<AssetEvent> events_[2];
    u32 read_buf_ = 0;
    u32 write_buf_ = 1;
    IGpuUploadSink* sink_ = nullptr;
    Array<byte> placeholders_[16];  // indexed by ChunkType
};

}  // namespace mce::asset
