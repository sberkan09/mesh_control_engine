#pragma once
// ClipBank: the assets -> audio seam (audio-input-ui §4.3 "scene load: register_clip per referenced
// clip (assets resolves GUID -> chunk -> payload)"). Maps a cooked Sfx asset's GUID to a
// registered ClipHandle, holding one asset retain per clip so the payload the mixer reads stays
// resident for the clip's life.
//
// Main thread only. Call sync() once per frame, before the sim ticks: a hot-reloaded asset is
// re-registered and its old clip released while the retired blob is still alive (the asset
// system keeps it for MCE_FRAMES_IN_FLIGHT end_frame()s; the mixer drops the old clip on its next
// callback).

#include "mce/asset/asset_system.h"
#include "mce/audio/audio.h"

namespace mce::audio {

// Cooked CHNK_SFX payload -> mixer clip description (points into `payload`, no copy). A looping
// clip ends at its loop_end and wraps to loop_start; the cook guarantees loop_end > loop_start.
Result<ClipPayload> clip_payload_from_sfx(Span<const byte> payload);

class ClipBank {
public:
    ClipBank(asset::AssetSystem& assets, AudioDevice& device, mem::IAllocator& a = mem::heap());
    ~ClipBank();
    ClipBank(const ClipBank&) = delete;
    ClipBank& operator=(const ClipBank&) = delete;

    // Loads and registers the clip on first use; later calls are a table lookup. A GUID that is
    // not mounted, not an Sfx, or fails to register yields a null handle and one error, and is
    // not retried until release() (FR-DX-050: silence, never a crash). `stream` registers it as a
    // streamed clip (AudioDevice::register_stream: music, long ambiences), falling back to an
    // in-memory clip when every stream ring is taken; it applies when the GUID is first acquired.
    ClipHandle acquire(asset::Guid guid, bool stream = false);
    // The registered clip, or null when the GUID was never acquired (no load).
    ClipHandle find(asset::Guid guid) const;
    // Releases the clip and the asset retain.
    void release(asset::Guid guid);
    // Releases every clip and waits for the mixer to drop them before the retains go, so the
    // pack can unmount right after (AudioDevice::finish_releases).
    void release_all();

    // Picks up hot reloads: every clip whose asset generation moved is released and registered
    // again from the new payload. Handles held elsewhere go stale (silent no-ops); the bridge
    // restarts looping sources on the new clip.
    void sync();

    u32 size() const { return static_cast<u32>(entries_.size()); }

private:
    struct Entry {
        asset::AssetRef<asset::Sfx> ref;  // null handle = failed (not retried)
        u32 generation = 0;
        ClipHandle clip;
        bool stream = false;
    };
    ClipHandle register_entry(Entry& e);
    void drop(Entry& e);

    asset::AssetSystem* assets_;
    AudioDevice* device_;
    HashMap<asset::Guid, Entry> entries_;
};

}  // namespace mce::audio
