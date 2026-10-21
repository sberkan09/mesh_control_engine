#pragma once
// Game Center / Play Games / Steam behind game services (M-LIVEOPS step 2, M-STEAM step 2): PlatformGameServices is the
// IGameServicesProvider for a phone's own game services. It speaks to them through
// IPlatformGamesBackend, the platform's calls with nothing engine-specific in them: the player
// implements it over mce/platform/game_services.h, tests over a fake.
//
// What it adds on top of the platform:
// - the store ids: the catalog's "ios", "android" or "steam" id for each achievement and leaderboard
//   (Steam falls back to the catalog id);
// - versioned cloud saves. Game Center saved games and Play Games snapshots keep one blob per name and
//   no version, so the blob carries one (encode_cloud_blob): cloud_save reads the blob, answers
//   Conflict when another device moved it past base_version, and otherwise writes base_version + 1.
//   The read and the write are two calls, so two devices saving in the same moment can both win; the
//   later write is what the cloud keeps, and the other device sees a conflict on its next load.
//
// liveops never sees the platform's SDKs (the store wall): everything below is plain data.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/liveops/game_services.h"

namespace mce::liveops {

// A cloud blob: "MCGS", format 1, the version, then the game's save bytes.
void encode_cloud_blob(u64 version, Span<const byte> bytes, Array<byte>& out);
// False when `blob` is not one (another app's data, or damaged); `bytes` points into `blob`.
bool decode_cloud_blob(Span<const byte> blob, u64& version, Span<const byte>& bytes);

enum class BlobRead : u8 { Ok, Missing, Failed };

// Which of the catalog's store ids a platform knows.
enum class StoreIds : u8 { Ios, Android, Steam };

// What the platform answers, from any thread, once per call of IPlatformGamesBackend. `tag` is the
// achievement, leaderboard or cloud save index the call carried.
class PlatformGamesSink {
public:
    virtual void platform_signed_in(bool ok) = 0;
    virtual void platform_achievement(u16 tag, bool ok) = 0;
    virtual void platform_score(u16 tag, bool ok) = 0;
    virtual void platform_blob_read(u16 tag, BlobRead r, Span<const byte> blob) = 0;
    virtual void platform_blob_written(u16 tag, bool ok) = 0;

protected:
    ~PlatformGamesSink() = default;
};

// The phone's game services as the platform has them. Main thread.
class IPlatformGamesBackend {
public:
    virtual ~IPlatformGamesBackend() = default;
    virtual const char* name() const = 0;  // "game_center", "play_games", "steam"
    virtual StoreIds store_ids() const = 0;
    // False: the platform cannot start (no services here); every call then fails.
    virtual bool start(PlatformGamesSink& sink) = 0;
    virtual void stop() {}
    virtual void sign_in(bool interactive) = 0;
    // `steps` of `total` reached (one-shot achievements: total 1).
    virtual void report_achievement(StringView id, u32 steps, u32 total, u16 tag) = 0;
    virtual void submit_score(StringView id, i64 score, u16 tag) = 0;
    virtual void read_blob(StringView name, u16 tag) = 0;
    virtual void write_blob(StringView name, Span<const byte> blob, u16 tag) = 0;
    virtual void show_achievements() {}
    virtual void show_leaderboard(StringView /*id*/) {}
};

class PlatformGameServices final : public IGameServicesProvider, public PlatformGamesSink {
public:
    explicit PlatformGameServices(IPlatformGamesBackend& backend, mem::IAllocator& a = mem::heap());
    const char* name() const override { return backend_.name(); }

    void start(const ServicesCatalog& catalog, GameServicesSink& sink) override;
    void stop() override;
    void sign_in(bool interactive) override;
    void report_achievement(u16 achievement, u32 steps) override;
    void submit_score(u16 leaderboard, i64 score) override;
    void cloud_save(u16 slot, u64 base_version, Span<const byte> bytes) override;
    void cloud_load(u16 slot) override;
    void show_achievements() override;
    void show_leaderboard(u16 leaderboard) override;

    void platform_signed_in(bool ok) override;
    void platform_achievement(u16 tag, bool ok) override;
    void platform_score(u16 tag, bool ok) override;
    void platform_blob_read(u16 tag, BlobRead r, Span<const byte> blob) override;
    void platform_blob_written(u16 tag, bool ok) override;

private:
    // A cloud save's call in flight (GameServicesService sends one per slot at a time).
    struct Slot {
        enum class Op : u8 { None, Save, Load };
        Op op = Op::None;
        u64 base = 0;      // Save: the version this device builds on
        Array<byte> blob;  // Save: what is written once the read says base is current
    };

    template <class Info>
    StringView store_id(const Info& e) const;

    IPlatformGamesBackend& backend_;
    mem::IAllocator& alloc_;
    const ServicesCatalog* catalog_ = nullptr;
    GameServicesSink* sink_ = nullptr;
    bool started_ = false;
    Array<Slot> slots_;
};

}  // namespace mce::liveops
