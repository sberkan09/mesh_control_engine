#pragma once
// Platform game services (M-LIVEOPS step 2, issue #148): achievements, leaderboards and cloud
// saves through one engine API over Game Center / iCloud and Play Games Services, with a local
// fake (LocalGameServices) on desktop and in CI.
//
// The catalog: one `.mcservices` file in the project's assets, cooked to a GameServicesCatalog
// chunk (the runtime never parses text, FR-AST-010). Game code names everything by "id"; the store
// ids stay in the file.
//
//   { "format": "mce-services", "version": 1, "guid": "<uuid>",
//     "achievements": [ { "id": "first_win", "ios": "com.studio.game.first_win", "android": "CgkI..." },
//                       { "id": "win_10", "steps": 10, "ios": "...", "android": "..." } ],
//     "leaderboards": [ { "id": "best_time", "order": "lower_is_better", "ios": "...", "android": "..." } ],
//     "cloud_saves": [ "progress" ] }
//
// "steps" > 1 makes an incremental achievement (unlocked when its progress reaches steps). A
// leaderboard's "order" is higher_is_better (the default) or lower_is_better. "cloud_saves" names
// the saves (save::write names) that may be synced to the player's cloud.
//
// Game code (FixedUpdate systems):
//
//   if (liveops::GameServices* gs = f.try_resource<liveops::GameServices>()) {
//       if (won) { gs->unlock("first_win"); gs->add_progress("win_10", 1); gs->submit_score("best_time", ms); }
//       for (const liveops::GameServicesEvent& e : f.events().read<liveops::GameServicesEvent>())
//           if (e.kind == liveops::GameServicesEventKind::CloudLoaded) reload_progress();
//   }
//
// Offline first: unlock, add_progress and submit_score take effect on this device at once (unlocked(),
// progress(), best_score()), are saved (the "game_services" save) and wait in an outbox until the
// player is signed in and the service answers; a failure is retried later, and the outbox survives
// relaunches. Sign-in is silent at start (no UI); sign_in() is for a game's own "Sign in" button.
//
// Cloud saves are versioned blobs over the save system (FR-RUN-065). cloud_save uploads the local
// save on top of the version this device last synced; if another device uploaded since, that is a
// conflict. cloud_load downloads: a newer cloud version replaces the local save when the local one
// has not changed since the last sync; when both changed it is a conflict, settled by
// GameServicesOptions::conflict (by default the game decides: a CloudConflict event, then
// resolve_conflict). The game reloads its save on CloudLoaded.
//
// Determinism (FR-RUN-040): providers answer on any thread; answers enter the sim only at the start
// of a tick as GameServicesEvent (the game_services_pump system, first in FixedUpdate_Pre). A
// Recorder captures the local state when recording starts and every event with its tick; a Replayer
// plays them back with no provider and no file writes, so replays hash the same.
//
// Game services are the player's own platform account, not analytics: they do not wait for the
// consent gate (consent.h). The provider decides what the platform shows (its sign-in banner).

#include <atomic>
#include <mutex>

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/fixed_string.h"
#include "mce/result.h"

namespace mce::liveops {

inline constexpr u16 kNoService = 0xFFFF;

// ---- the catalog ----------------------------------------------------------------------------------

struct AchievementInfo {
    FixedString<64> id;
    FixedString<128> ios_id;
    FixedString<128> android_id;
    u32 steps = 1;  // > 1: incremental
};

struct LeaderboardInfo {
    FixedString<64> id;
    FixedString<128> ios_id;
    FixedString<128> android_id;
    bool lower_is_better = false;  // a time; otherwise a score
};

class ServicesCatalog {
public:
    explicit ServicesCatalog(mem::IAllocator& a = mem::heap()) : achievements_(a), leaderboards_(a), cloud_(a) {}
    Span<const AchievementInfo> achievements() const { return achievements_.span(); }
    Span<const LeaderboardInfo> leaderboards() const { return leaderboards_.span(); }
    Span<const FixedString<64>> cloud_saves() const { return cloud_.span(); }
    u16 find_achievement(StringView id) const;  // kNoService when absent
    u16 find_leaderboard(StringView id) const;
    u16 find_cloud_save(StringView name) const;
    // False when the id is empty or already used, or steps is 0.
    bool add_achievement(const AchievementInfo& a);
    bool add_leaderboard(const LeaderboardInfo& l);
    bool add_cloud_save(StringView name);
    void clear();

private:
    Array<AchievementInfo> achievements_;
    Array<LeaderboardInfo> leaderboards_;
    Array<FixedString<64>> cloud_;
};

// The GameServicesCatalog chunk: written by the cook, read by the player (bounds-checked; empty or
// duplicate ids rejected).
void encode_services_catalog(const ServicesCatalog& c, Array<byte>& out);
Result<void> decode_services_catalog(Span<const byte> payload, ServicesCatalog& out);

// ---- what enters the sim --------------------------------------------------------------------------

enum class GameServicesEventKind : u8 {
    SignedIn,         // the outbox starts draining
    SignedOut,        // a sign-in failed or the player signed out; everything waits
    AchievementSent,  // index: achievement; value: the steps the platform now has
    ScoreSent,        // index: leaderboard; value: the score
    CloudSaved,       // index: cloud save; value: the cloud version now
    CloudLoaded,      // the local save was replaced by the cloud's (value: version): reload it
    CloudCurrent,     // cloud_load found nothing newer than this device's save
    CloudConflict,    // both changed (value: the cloud version): resolve_conflict(slot, keep_local)
    Failed,           // a cloud_save / cloud_load did not reach the cloud (offline); nothing changed
};
const char* game_services_event_name(GameServicesEventKind k);  // "CloudLoaded"

struct GameServicesEvent {
    GameServicesEventKind kind = GameServicesEventKind::Failed;
    u8 pad_ = 0;
    u16 index = kNoService;  // achievement, leaderboard or cloud save index (catalog order)
    u32 request = 0;         // the id cloud_save / cloud_load / sign_in returned; 0 for the outbox
    i64 value = 0;
};

// ---- the game's side: a world resource ------------------------------------------------------------

class GameServicesService;

class GameServices {
public:
    explicit GameServices(GameServicesService* s = nullptr) : s_(s) {}
    bool available() const;  // false: no provider here (everything stays on this device)
    bool signed_in() const;  // as of the last SignedIn / SignedOut event
    u32 sign_in();           // an interactive sign-in (a "Sign in" button); answered by SignedIn / SignedOut
    const ServicesCatalog& catalog() const;

    // Achievements. False: unknown id, or nothing changes (already unlocked).
    bool unlock(StringView achievement);
    bool add_progress(StringView achievement, u32 steps);  // incremental; caps at the achievement's steps
    bool unlocked(StringView achievement) const;
    u32 progress(StringView achievement) const;  // steps reached on this device

    // Leaderboards. False: unknown id. best_score is this device's best, `def` before any.
    bool submit_score(StringView leaderboard, i64 score);
    i64 best_score(StringView leaderboard, i64 def) const;

    // Cloud saves (names from the catalog's cloud_saves). 0: unknown name or no provider.
    u32 cloud_save(StringView save_name);
    u32 cloud_load(StringView save_name);
    // After CloudConflict: keep this device's save (it is uploaded over the cloud's) or the cloud's
    // (it replaces the local save; CloudLoaded follows). False when no conflict waits.
    bool resolve_conflict(StringView save_name, bool keep_local);

    u32 pending() const;  // achievements and scores waiting in the outbox
    // The platform's own screens (no-ops on the fake).
    void show_achievements();
    void show_leaderboard(StringView leaderboard);

private:
    GameServicesService* s_;
};

// ---- the platform's side: providers -----------------------------------------------------------------

enum class CloudResult : u8 { Ok, Conflict, Failed };

// What a provider reports, from any thread, exactly once per call below (sign_in: once per call).
class GameServicesSink {
public:
    virtual void signed_in(bool ok) = 0;
    virtual void achievement_sent(u16 achievement, bool ok) = 0;
    virtual void score_sent(u16 leaderboard, bool ok) = 0;
    // Ok: `version` is the cloud's version now. Conflict: the cloud is at `version`, not base_version.
    virtual void cloud_saved(u16 slot, CloudResult r, u64 version) = 0;
    // Ok: the cloud's blob and version (version 0: nothing in the cloud yet).
    virtual void cloud_loaded(u16 slot, CloudResult r, u64 version, Span<const byte> bytes) = 0;

protected:
    ~GameServicesSink() = default;
};

// Game Center / Play Games, or the fake. Called on the main thread; one call in flight per kind.
class IGameServicesProvider {
public:
    virtual ~IGameServicesProvider() = default;
    virtual const char* name() const = 0;
    virtual void start(const ServicesCatalog& catalog, GameServicesSink& sink) = 0;
    virtual void stop() {}
    virtual void update() {}
    virtual void sign_in(bool interactive) = 0;
    // `steps`: the total reached (one-shot achievements: their steps, i.e. unlocked).
    virtual void report_achievement(u16 achievement, u32 steps) = 0;
    virtual void submit_score(u16 leaderboard, i64 score) = 0;
    // Stores `bytes` as base_version + 1 when the cloud is still at base_version; Conflict otherwise.
    virtual void cloud_save(u16 slot, u64 base_version, Span<const byte> bytes) = 0;
    virtual void cloud_load(u16 slot) = 0;
    virtual void show_achievements() {}
    virtual void show_leaderboard(u16 /*leaderboard*/) {}
};

// The local fake (desktop, CI): the "platform" is a folder. Accepted achievements and scores are
// appended to <dir>/game_services.jsonl (one JSON object per line); cloud saves are
// <dir>/cloud/<name>.blob (a version, then the bytes). Offline: every call fails, sign-in included.
class LocalGameServices final : public IGameServicesProvider {
public:
    explicit LocalGameServices(StringView dir) : dir_(dir) {}
    const char* name() const override { return "local"; }
    void set_offline(bool offline) { offline_ = offline; }
    void start(const ServicesCatalog& catalog, GameServicesSink& sink) override;
    void sign_in(bool interactive) override;
    void report_achievement(u16 achievement, u32 steps) override;
    void submit_score(u16 leaderboard, i64 score) override;
    void cloud_save(u16 slot, u64 base_version, Span<const byte> bytes) override;
    void cloud_load(u16 slot) override;

private:
    void log_line(StringView json);
    FixedString<512> blob_path(u16 slot) const;

    FixedString<512> dir_;
    const ServicesCatalog* catalog_ = nullptr;
    GameServicesSink* sink_ = nullptr;
    bool offline_ = false;
};

// ---- record / replay ----------------------------------------------------------------------------------

class GameServicesRecorder {
public:
    explicit GameServicesRecorder(mem::IAllocator& a = mem::heap()) : bytes_(a) {}
    Span<const byte> bytes() const { return bytes_.span(); }

private:
    friend class GameServicesService;
    Array<byte> bytes_;
};

class GameServicesReplayer {
public:
    explicit GameServicesReplayer(Span<const byte> stream) : stream_(stream) {}  // must outlive the replayer
    bool done() const { return pos_ >= stream_.size(); }
    bool corrupt() const { return corrupt_; }

private:
    friend class GameServicesService;
    Span<const byte> stream_;
    usize pos_ = 0;
    bool corrupt_ = false;
};

// ---- the host's side ------------------------------------------------------------------------------------

enum class ConflictPolicy : u8 {
    AskGame,       // a CloudConflict event; nothing changes until resolve_conflict
    PreferCloud,   // the cloud's save replaces the local one (CloudLoaded)
    PreferDevice,  // this device's save is uploaded over the cloud's (CloudSaved)
};

struct GameServicesOptions {
    FixedString<64> state_save = "game_services";  // save::write name; empty: nothing persisted
    u32 retry_updates = 600;                       // host ticks between retries after a failure or sign-out
    ConflictPolicy conflict = ConflictPolicy::AskGame;
};

class GameServicesService final : public GameServicesSink {
public:
    explicit GameServicesService(mem::IAllocator& a = mem::heap());
    GameServicesService(const GameServicesService&) = delete;
    GameServicesService& operator=(const GameServicesService&) = delete;

    // Null provider: everything stays on this device. Loads the saved state (a missing or unreadable
    // save starts empty), starts the provider and signs in silently.
    void init(const ServicesCatalog& catalog, IGameServicesProvider* provider, const GameServicesOptions& opt = {});
    void shutdown();
    // Registers GameServicesEvent, sets the GameServices resource and adds game_services_pump (first in
    // FixedUpdate_Pre). Install before the game module.
    void install(World& world);
    // Main thread, once per host tick: drains the outbox, sends cloud requests, retries.
    void update();

    void set_recorder(GameServicesRecorder* r);
    void set_replayer(GameServicesReplayer* r);

    // GameServicesSink (any thread).
    void signed_in(bool ok) override;
    void achievement_sent(u16 achievement, bool ok) override;
    void score_sent(u16 leaderboard, bool ok) override;
    void cloud_saved(u16 slot, CloudResult r, u64 version) override;
    void cloud_loaded(u16 slot, CloudResult r, u64 version, Span<const byte> bytes) override;

    // Moves answers into the sim (the game_services_pump system).
    void pump(World& world, u64 tick);

    const ServicesCatalog& catalog() const { return catalog_; }
    u32 pending() const;
    bool is_signed_in() const { return signed_in_; }

private:
    friend class GameServices;

    // A provider's answer, waiting for the next tick.
    struct Answer {
        enum What : u8 { SignIn, Achievement, Score, CloudSaved, CloudLoaded, Resolved };
        What what = SignIn;
        bool ok = false;
        CloudResult result = CloudResult::Failed;
        u16 index = kNoService;
        u64 version = 0;
        Array<byte> bytes;
    };
    struct Slot {
        u64 synced_version = 0;  // the cloud version this device last uploaded or downloaded
        u64 synced_hash = 0;     // hash of those bytes
        u64 upload_hash = 0;     // hash of the bytes in flight
        u8 want = 0;             // 1 save, 2 load, 3 save over a conflict
        u32 request = 0;
        bool in_flight = false;
        bool conflict = false;
        u64 conflict_version = 0;
        Array<byte> conflict_bytes;
    };

    void post(const Answer& a);
    void emit(World& world, const GameServicesEvent& e);
    void apply_answer(World& world, const Answer& a, Array<GameServicesEvent>& out);
    void apply_cloud(u16 slot, u64 version, Span<const byte> bytes);
    bool local_changed(u16 slot, u64* hash_out) const;
    void persist();
    void load_state();
    void request_slot(u16 slot, u8 want, u32 request);
    u32 next_request() { return ++last_request_; }
    bool replaying() const { return replayer_ != nullptr; }

    mem::IAllocator& alloc_;
    ServicesCatalog catalog_;
    IGameServicesProvider* provider_ = nullptr;
    GameServicesOptions opt_;
    bool started_ = false;

    // Sim-side state (main thread).
    Array<u32> steps_;       // per achievement, reached on this device
    Array<u32> sent_steps_;  // per achievement, acknowledged by the platform
    Array<i64> best_;        // per leaderboard
    Array<u8> has_best_;
    Array<u8> score_waiting_;  // per leaderboard: best_ not sent yet
    Array<Slot> slots_;
    bool signed_in_ = false;
    u32 last_request_ = 0;

    // Outbox driving (main thread, never replayed).
    bool sign_in_in_flight_ = false;
    bool sign_in_wanted_ = false;
    bool interactive_ = false;
    u16 ach_in_flight_ = kNoService;
    u32 ach_in_flight_steps_ = 0;
    u16 lb_in_flight_ = kNoService;
    i64 lb_in_flight_score_ = 0;
    u32 wait_ = 0;  // host ticks until the next retry

    std::mutex inbox_mutex_;
    Array<Answer> inbox_;

    GameServicesRecorder* recorder_ = nullptr;
    GameServicesReplayer* replayer_ = nullptr;
};

}  // namespace mce::liveops
