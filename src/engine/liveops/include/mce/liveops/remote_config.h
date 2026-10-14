#pragma once
// Remote config (M-LIVEOPS step 3): values a game reads by key, with defaults cooked from the
// project and newer values fetched in the background, applied only at a defined point.
//
// Defaults: one `.mcconfig` file in the project's assets, cooked to a RemoteConfig chunk (the
// runtime never parses text, FR-AST-010):
//
//   { "format": "mce-config", "version": 1, "guid": "<uuid>",
//     "values": { "ad_every_levels": 3, "gem_bonus": 1.5, "winter_event": false, "motd": "Welcome",
//                 "drop_rate": { "type": "float", "value": 1 } } }
//
// A value's type comes from its JSON (true/false: bool, a whole number: int, any other number:
// float, text: string) or from the { "type", "value" } form. The defaults are the schema: a fetched
// value for a key the defaults do not have, or of another type, is ignored.
//
// Game code (systems):
//
//   const liveops::RemoteConfig& rc = f.resource<liveops::RemoteConfig>();
//   const i64 every = rc.get<i64>("ad_every_levels", 3);   // the code's own default if the key is missing
//   if (rc.fetched_waiting() && at_main_menu) f.resource<liveops::RemoteConfig>().apply_fetched();
//
// When values change: a fetch never changes what the game reads mid-run. Fetched values are saved
// (the "remote_config" save) and applied at the next launch, or earlier when the game calls
// apply_fetched() at a moment of its choosing; that applies at the start of the next tick, never in
// the middle of one. A Recorder captures the values in force when recording starts and every apply
// with its tick; a Replayer plays them back with no fetcher, so replays hash the same.
//
// Fetching: an IConfigFetcher (a vendor's config service, or FileConfigFetcher, which reads a cooked
// pack holding a .mcconfig: the static-file backend) runs when the service starts and when the game
// calls fetch(). A fetcher that talks to a server waits for the player's consent (consent.h).

#include <atomic>
#include <mutex>
#include <type_traits>

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/fixed_string.h"
#include "mce/result.h"

namespace mce::liveops {

enum class ConfigType : u8 { Int, Float, Bool, String };
const char* config_type_name(ConfigType t);  // "int", "float", "bool", "string"

struct ConfigValue {
    FixedString<64> key;
    ConfigType type = ConfigType::Int;
    i64 i = 0;  // Int, and Bool (0 or 1)
    f64 f = 0.0;
    FixedString<256> s;
};

class ConfigValues {
public:
    explicit ConfigValues(mem::IAllocator& a = mem::heap()) : values_(a) {}
    Span<const ConfigValue> values() const { return values_.span(); }
    const ConfigValue* find(StringView key) const;
    // False when the key is empty, longer than 63 bytes, already present, or the string longer than 255.
    bool add(const ConfigValue& v);
    void clear() { values_.clear(); }
    usize size() const { return values_.size(); }

private:
    Array<ConfigValue> values_;
};

// The RemoteConfig chunk (defaults, the saved cache and recordings use it too).
void encode_config(const ConfigValues& v, Array<byte>& out);
Result<void> decode_config(Span<const byte> payload, ConfigValues& out);
u64 hash_config(const ConfigValues& v);

// ---- fetching -----------------------------------------------------------------------------------

class ConfigFetchSink {
public:
    // Any thread, exactly once per fetch.
    virtual void fetched(const ConfigValues& v) = 0;
    virtual void fetch_failed() = 0;

protected:
    ~ConfigFetchSink() = default;
};

class IConfigFetcher {
public:
    virtual ~IConfigFetcher() = default;
    virtual const char* name() const = 0;
    // A fetcher that sends anything from the device (a vendor's service) waits for Consent::Granted.
    virtual bool needs_consent() const { return true; }
    // Main thread; answers through the sink now or later.
    virtual void fetch(ConfigFetchSink& sink) = 0;
    virtual void update() {}
};

// Reads a cooked pack (or cooked/ directory) holding a .mcconfig and answers with its values: the
// static-file backend, and the stand-in for a server in tests. Reads only a local path; sends nothing.
class FileConfigFetcher final : public IConfigFetcher {
public:
    explicit FileConfigFetcher(StringView pack_path) : path_(pack_path) {}
    const char* name() const override { return "file"; }
    bool needs_consent() const override { return false; }
    void fetch(ConfigFetchSink& sink) override;

private:
    FixedString<512> path_;
};

// ---- record / replay ----------------------------------------------------------------------------

class ConfigRecorder {
public:
    explicit ConfigRecorder(mem::IAllocator& a = mem::heap()) : bytes_(a) {}
    Span<const byte> bytes() const { return bytes_.span(); }

private:
    friend class RemoteConfigService;
    Array<byte> bytes_;
};

class ConfigReplayer {
public:
    explicit ConfigReplayer(Span<const byte> stream) : stream_(stream) {}  // must outlive the replayer
    bool done() const { return pos_ >= stream_.size(); }
    bool corrupt() const { return corrupt_; }

private:
    friend class RemoteConfigService;
    Span<const byte> stream_;
    usize pos_ = 0;
    bool corrupt_ = false;
};

// ---- the game's side: a world resource ----------------------------------------------------------

class RemoteConfigService;

class RemoteConfig {
public:
    explicit RemoteConfig(RemoteConfigService* s = nullptr) : s_(s) {}

    // The value in force, or `def` when the key is missing or of another type.
    i64 get_int(StringView key, i64 def) const;
    f64 get_float(StringView key, f64 def) const;
    bool get_bool(StringView key, bool def) const;
    StringView get_string(StringView key, StringView def) const;  // valid until the next apply
    template <class T>
    T get(StringView key, T def) const;
    StringView get(StringView key, const char* def) const { return get_string(key, def); }

    Span<const ConfigValue> values() const;
    // Applies counted since launch (0: the launch values).
    u32 version() const;
    // Newer values were fetched and are waiting: the next launch uses them, apply_fetched() sooner.
    bool fetched_waiting() const;
    // Applies the fetched values at the start of the next tick.
    void apply_fetched();
    // Asks the fetcher again (a game's "check for updates"); it answers whenever it likes.
    void fetch();

private:
    RemoteConfigService* s_;
};

template <class T>
T RemoteConfig::get(StringView key, T def) const {
    if constexpr (std::is_same_v<T, bool>)
        return get_bool(key, def);
    else if constexpr (std::is_floating_point_v<T>)
        return static_cast<T>(get_float(key, static_cast<f64>(def)));
    else if constexpr (std::is_integral_v<T>)
        return static_cast<T>(get_int(key, static_cast<i64>(def)));
    else
        return get_string(key, def);
}

// ---- the host's side ------------------------------------------------------------------------------

struct RemoteConfigOptions {
    FixedString<64> cache_save =
        "remote_config";  // save::write name; empty: no cache (every launch starts from the defaults)
    bool fetch_on_start = true;
};

class RemoteConfigService final : public ConfigFetchSink {
public:
    RemoteConfigService() = default;
    RemoteConfigService(const RemoteConfigService&) = delete;
    RemoteConfigService& operator=(const RemoteConfigService&) = delete;

    // The cooked defaults; the cached values from the last fetch are applied on top. Null fetcher: none.
    void init(const ConfigValues& defaults, IConfigFetcher* fetcher, const RemoteConfigOptions& opt = {});
    // Sets the RemoteConfig resource and adds the remote_config_apply system (first in FixedUpdate_Pre).
    void install(World& world);
    // Main thread, once per host tick: starts a waiting fetch once consent allows, saves what arrived.
    void update();

    void fetch();
    void apply_fetched() { apply_requested_ = true; }
    bool fetched_waiting() const { return fetched_waiting_; }

    const ConfigValues& active() const { return active_; }
    u32 version() const { return version_; }

    // Recording starts with the values in force; replaying takes them and every apply from the
    // stream, and ignores the fetcher (nothing is fetched or saved).
    void set_recorder(ConfigRecorder* r);
    void set_replayer(ConfigReplayer* r);

    // ConfigFetchSink (any thread).
    void fetched(const ConfigValues& v) override;
    void fetch_failed() override;

    // The remote_config_apply system.
    void apply(u64 tick);

private:
    // `active_` = defaults with every key of `over` that matches one in name and type.
    void overlay(const ConfigValues& over, ConfigValues& out) const;
    void record_active(u64 tick);

    ConfigValues defaults_;
    ConfigValues active_;
    ConfigValues fetched_;  // main thread copy of the last fetch
    IConfigFetcher* fetcher_ = nullptr;
    RemoteConfigOptions opt_;
    u32 version_ = 0;
    bool fetch_wanted_ = false;
    bool fetching_ = false;
    bool fetched_waiting_ = false;
    bool apply_requested_ = false;

    std::mutex inbox_mutex_;
    ConfigValues inbox_;
    std::atomic<u8> inbox_state_{0};  // 0 none, 1 values, 2 failed

    ConfigRecorder* recorder_ = nullptr;
    ConfigReplayer* replayer_ = nullptr;
};

}  // namespace mce::liveops
