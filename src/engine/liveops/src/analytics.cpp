#include "mce/liveops/analytics.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "mce/containers/array.h"
#include "mce/log.h"

namespace mce::liveops::analytics {
namespace {

struct State {
    std::mutex mutex;
    Array<Event> queue;  // oldest first
    Stats stats;
    IBackend* backend = nullptr;
    Consent told = Consent::Unknown;  // what the backend last heard
    FixedString<41> last_rejected;    // logged once per name in a row
};

State& state() {
    static State s;
    return s;
}

bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }

u64 now_ms() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void reject(StringView name, StringView why, StringView key = {}) {
    State& s = state();
    ++s.stats.rejected;
    if (s.last_rejected == name) return;
    s.last_rejected.assign(name);
    if (key.empty())
        MCE_LOG_WARN("analytics: event '{}' rejected: {}", name, why);
    else
        MCE_LOG_WARN("analytics: event '{}' rejected: param '{}' {}", name, key, why);
}

// JSON string body (the quotes are the caller's).
void put_escaped(std::FILE* f, StringView s) {
    for (const char c : s) {
        switch (c) {
            case '"':
                std::fputs("\\\"", f);
                break;
            case '\\':
                std::fputs("\\\\", f);
                break;
            case '\n':
                std::fputs("\\n", f);
                break;
            case '\r':
                std::fputs("\\r", f);
                break;
            case '\t':
                std::fputs("\\t", f);
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    std::fprintf(f, "\\u%04x", static_cast<unsigned>(c));
                else
                    std::fputc(c, f);
        }
    }
}

}  // namespace

bool valid_name(StringView name) {
    if (name.empty() || name.size() > 40 || !letter(name[0])) return false;
    for (const char c : name)
        if (!letter(c) && !digit(c) && c != '_') return false;
    return !name.starts_with("firebase_") && !name.starts_with("google_") && !name.starts_with("ga_");
}

Param* Params::next(StringView key) {
    if (bad_) return nullptr;
    bool dup = false;
    for (u32 i = 0; i < count_; ++i) dup = dup || items_[i].key == key;
    if (count_ == kMaxParams || !valid_name(key) || dup) {
        bad_ = true;
        bad_key_.assign(key);
        return nullptr;
    }
    Param* p = &items_[count_++];
    *p = Param{};
    p->key.assign(key);
    return p;
}

Params& Params::add(StringView key, i64 v) {
    if (Param* p = next(key)) {
        p->kind = Param::Kind::Int;
        p->i = v;
    }
    return *this;
}

Params& Params::add(StringView key, f64 v) {
    if (!std::isfinite(v)) {
        if (!bad_) bad_key_.assign(key);
        bad_ = true;
        return *this;
    }
    if (Param* p = next(key)) {
        p->kind = Param::Kind::Float;
        p->f = v;
    }
    return *this;
}

Params& Params::add(StringView key, StringView v) {
    if (v.size() > 100) {
        if (!bad_) bad_key_.assign(key);
        bad_ = true;
        return *this;
    }
    if (Param* p = next(key)) {
        p->kind = Param::Kind::String;
        p->s.assign(v);
    }
    return *this;
}

bool event(StringView name, const Params& params) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!valid_name(name)) {
        reject(name, "is not a valid event name (1-40 letters, digits, '_', starting with a letter)");
        return false;
    }
    if (params.bad()) {
        reject(name, "is invalid, repeated, too long or one too many (25 at most)", params.bad_key());
        return false;
    }
    if (consent() == Consent::Denied) {
        ++s.stats.dropped;
        return true;
    }
    if (s.queue.size() == kMaxQueued) {
        s.queue.erase(0);  // the oldest go first
        ++s.stats.dropped;
    }
    Event e;
    e.name.assign(name);
    e.params = params;
    e.time_ms = now_ms();
    s.queue.push_back(e);
    return true;
}

void set_backend(IBackend* b) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.backend = b;
    s.told = Consent::Unknown;
    if (b) b->set_consent(consent());
    s.told = consent();
}

IBackend* backend() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.backend;
}

void update() {
    State& s = state();
    Array<Event> batch;
    IBackend* b = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const Consent c = consent();
        b = s.backend;
        if (b && c != s.told) {
            b->set_consent(c);
            s.told = c;
        }
        if (c == Consent::Unknown || s.queue.empty()) return;
        if (c == Consent::Denied || !b) {
            s.stats.dropped += s.queue.size();
            s.queue.clear();
            return;
        }
        batch = std::move(s.queue);
        s.queue.clear();
        s.stats.sent += batch.size();
    }
    for (const Event& e : batch) b->send(e);  // outside the lock: a backend may log or take its time
    b->flush();
}

Stats stats() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Stats out = s.stats;
    out.queued = static_cast<u32>(s.queue.size());
    return out;
}

void reset() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.queue.clear();
    s.stats = {};
    s.last_rejected.clear();
}

// ---- FileBackend ---------------------------------------------------------------------------------

FileBackend::FileBackend(StringView path) : path_(path) {}

FileBackend::~FileBackend() {
    if (file_) std::fclose(static_cast<std::FILE*>(file_));
}

void FileBackend::send(const Event& e) {
    if (failed_) return;
    if (!file_) {
        file_ = std::fopen(path_.c_str(), "ab");
        if (!file_) {
            failed_ = true;
            MCE_LOG_WARN("analytics: cannot open {}; events are not written", path_.view());
            return;
        }
    }
    std::FILE* f = static_cast<std::FILE*>(file_);
    std::fprintf(f, "{\"t\":%llu,\"name\":\"", static_cast<unsigned long long>(e.time_ms));
    put_escaped(f, e.name.view());
    std::fputs("\",\"params\":{", f);
    bool first = true;
    for (const Param& p : e.params.params()) {
        std::fputs(first ? "\"" : ",\"", f);
        first = false;
        put_escaped(f, p.key.view());
        std::fputs("\":", f);
        switch (p.kind) {
            case Param::Kind::Int:
                std::fprintf(f, "%lld", static_cast<long long>(p.i));
                break;
            case Param::Kind::Float:
                std::fprintf(f, "%.17g", p.f);
                break;
            case Param::Kind::String:
                std::fputc('"', f);
                put_escaped(f, p.s.view());
                std::fputc('"', f);
                break;
        }
    }
    std::fputs("}}\n", f);
}

void FileBackend::flush() {
    if (file_) std::fflush(static_cast<std::FILE*>(file_));
}

}  // namespace mce::liveops::analytics
