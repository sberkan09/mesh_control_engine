#pragma once
// Analytics events (M-LIVEOPS step 1): event(name, params) from anywhere in the game, delivered to
// one backend (a vendor, or FileBackend's local JSON lines) once consent allows.
//
//   analytics::event("level_end", analytics::Params().add("level", 3).add("time_s", 41.5).add("result", "win"));
//
// Names follow the strictest vendor's rules (Firebase Analytics), so an event valid here is valid
// everywhere: a name is 1-40 letters, digits and '_', starting with a letter, and not starting with
// "firebase_", "google_" or "ga_"; at most 25 params; a param key follows the same rules; a string
// value is at most 100 bytes. An invalid event is rejected (event() returns false, logged once per
// name) rather than truncated, so a typo shows up in development.
//
// Delivery: event() queues (any thread, bounded at kMaxQueued; the oldest go first when full) and
// update() hands the queue to the backend on the main thread: all of it while consent is Granted,
// none while Unknown, and Denied clears it. Events never touch the sim: a replay or a determinism
// run is not affected, and with no backend (the default) event() costs a validation and a drop.

#include "mce/fixed_string.h"
#include "mce/liveops/consent.h"
#include "mce/types.h"

namespace mce::liveops::analytics {

inline constexpr u32 kMaxParams = 25;
inline constexpr u32 kMaxQueued = 256;

struct Param {
    enum class Kind : u8 { Int, Float, String };
    FixedString<41> key;
    Kind kind = Kind::Int;
    i64 i = 0;
    f64 f = 0.0;
    FixedString<101> s;
};

class Params {
public:
    Params& add(StringView key, i64 v);
    Params& add(StringView key, i32 v) { return add(key, static_cast<i64>(v)); }
    Params& add(StringView key, u32 v) { return add(key, static_cast<i64>(v)); }
    Params& add(StringView key, f64 v);
    Params& add(StringView key, f32 v) { return add(key, static_cast<f64>(v)); }
    Params& add(StringView key, StringView v);
    Params& add(StringView key, const char* v) { return add(key, StringView(v)); }

    Span<const Param> params() const { return Span<const Param>(items_, count_); }
    // Something added did not fit or was invalid: the event will be rejected.
    bool bad() const { return bad_; }
    StringView bad_key() const { return bad_key_.view(); }

private:
    Param* next(StringView key);
    Param items_[kMaxParams];
    u32 count_ = 0;
    bool bad_ = false;
    FixedString<41> bad_key_;
};

struct Event {
    FixedString<41> name;
    Params params;
    u64 time_ms = 0;  // wall clock (Unix ms) when event() was called
};

// A vendor or a local sink. Called on the main thread from update().
class IBackend {
public:
    virtual ~IBackend() = default;
    virtual const char* name() const = 0;
    virtual void send(const Event& e) = 0;
    virtual void flush() {}
    // Consent changed (the vendor's own collection switch, e.g. Firebase's consent mode).
    virtual void set_consent(Consent) {}
};

// An event name or param key as the vendors accept it.
bool valid_name(StringView name);

// Queues an event; false when it is invalid (nothing is queued). Any thread.
bool event(StringView name, const Params& params = {});

// The backend events go to (null: dropped at update). Main thread; owned by the caller.
void set_backend(IBackend* backend);
IBackend* backend();

// Main thread, once per host tick: delivers or drops the queue as consent says.
void update();

struct Stats {
    u32 queued = 0;    // waiting for consent
    u64 sent = 0;      // handed to a backend
    u64 dropped = 0;   // consent denied, no backend, or the queue was full
    u64 rejected = 0;  // invalid name or params
};
Stats stats();
// Clears the queue and the counters (tests).
void reset();

// Appends one JSON object per line to a file: the local backend (desktop, CI, a game's own log).
//   {"t":1760412345123,"name":"level_end","params":{"level":3,"time_s":41.5,"result":"win"}}
class FileBackend final : public IBackend {
public:
    explicit FileBackend(StringView path);
    ~FileBackend() override;
    FileBackend(const FileBackend&) = delete;
    FileBackend& operator=(const FileBackend&) = delete;
    const char* name() const override { return "file"; }
    void send(const Event& e) override;
    void flush() override;
    StringView path() const { return path_.view(); }

private:
    FixedString<512> path_;
    void* file_ = nullptr;  // FILE*, opened (append) on the first event
    bool failed_ = false;
};

}  // namespace mce::liveops::analytics
