#pragma once
// Runtime asset facade (assets.md §7): mount cooked trees, sync load, refcount, deferred unload,
// generational handles, hot-reload commit, per-type placeholders.
//
// Main thread only: the main thread is the sole mutator of the tables, so the read path takes
// no locks (assets.md §8.1). Async loading (FR-ENT-120, request_async) adds a loader thread that
// only reads and hashes container files; the main thread commits them in poll_async().

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

// An async load request (request_async). Zero is never a live ticket.
struct AsyncTicket {
    u32 id = 0;
    constexpr bool valid() const { return id != 0; }
    friend constexpr bool operator==(AsyncTicket a, AsyncTicket b) { return a.id == b.id; }
};

enum class AsyncStatus : u8 {
    Pending,  // queued or being read on the loader thread
    Ready,    // committed: take_async hands back the handle
    Failed,   // take_async returns the error
    Unknown,  // not a live ticket (never issued, taken or cancelled)
};

struct MountOptions {
    // Dev mounts verify every container's payload CRC on load. A shipped pack is verified once at
    // mount and trusted afterwards (assets.md §3, the < 1 s boot budget).
    bool verify_payload_crc = true;
};

class AssetSystem {
public:
    // Placeholder payloads, one per ChunkType value (room for the appended types).
    static constexpr usize kPlaceholderSlots = 32;

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

    // ---- async loading (FR-ENT-120) ----------------------------------------------------------
    // Queues a load that does not block the main thread. A dev mount's container file is read and
    // hashed on the loader thread; an asset already resident, or one in a pack (its bytes were read
    // at mount), is ready at once. Finished reads are committed on the main thread by poll_async()
    // (end_frame() calls it too); the commit makes the asset resident exactly as load_sync would.
    // Pair every ticket with take_async or cancel_async.
    AsyncTicket request_async(AssetKey key, ChunkType type);
    template <class T>
    AsyncTicket request_async(AssetKey key) {
        return request_async(key, T::chunk);
    }
    AsyncStatus async_status(AsyncTicket t) const;
    // Ready: the handle, holding one reference (pair it with a release). Failed: the load error.
    // Either way the ticket is spent. Pending/Unknown: Errc::InvalidArgument, ticket untouched.
    Result<AnyHandle> take_async(AsyncTicket t);
    // Drops an unwanted request; a read in flight finishes and is discarded, a ready one releases
    // the reference it held. Unknown tickets are ignored.
    void cancel_async(AsyncTicket t);
    // Commits the loader thread's finished reads (main thread).
    void poll_async();
    // Blocks until every queued read has finished, then commits them (tests, loading screens).
    void wait_async();
    u32 async_pending() const;

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

    struct AsyncLoader;  // the loader thread and its queues (asset_system.cpp)
    struct AsyncRequest {
        u32 id = 0;
        AssetKey key = kInvalidAssetKey;
        ChunkType type = ChunkType::Manifest;
        AsyncStatus status = AsyncStatus::Pending;
        u64 content_hash = 0;  // of the file being read (a reload in between makes it stale)
        AnyHandle handle;
        Errc error = Errc::Ok;
    };

    Result<void> mount_one(StringView root, MountOptions opt);
    Result<void> bind_payload(Row& row, ContainerView view, StringView path);
    void fail_load(Row& row, AssetKey key, Errc code);
    void become_resident(Row& row, AssetKey key);
    void commit_read(AsyncRequest& req, Array<byte>& bytes, Errc read_error);
    AsyncRequest* find_async(AsyncTicket t);
    const AsyncRequest* find_async(AsyncTicket t) const;
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
    Array<byte> placeholders_[kPlaceholderSlots];  // indexed by ChunkType
    Array<AsyncRequest> async_;
    u32 next_async_id_ = 1;
    mem::UniquePtr<AsyncLoader> loader_;  // started by the first dev-mount request
};

}  // namespace mce::asset
