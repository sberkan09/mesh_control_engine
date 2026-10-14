#include "mce/liveops/game_services.h"

#include <cstring>

#include "mce/ecs/save.h"
#include "mce/hash.h"
#include "mce/log.h"

namespace mce::liveops {

namespace {

constexpr u32 kRecordMagic = 0x5347434D;  // "MCGS"
constexpr u32 kStateMagic = 0x5447434D;   // "MCGT"
constexpr u16 kFormatVersion = 1;

constexpr u8 kWantSave = 1;
constexpr u8 kWantLoad = 2;
constexpr u8 kWantSaveOverConflict = 3;

struct PumpRef {
    GameServicesService* service = nullptr;
};

void pump_system(Frame& f) {
    if (GameServicesService* s = f.resource<PumpRef>().service) s->pump(f.world(), f.tick);
}

template <class T>
void put(Array<byte>& out, T v) {
    const usize at = out.size();
    out.resize_uninitialized(at + sizeof(T));
    std::memcpy(out.data() + at, &v, sizeof(T));
}

void put_str(Array<byte>& out, StringView s) {
    put<u8>(out, static_cast<u8>(s.size()));
    for (char c : s) out.push_back(static_cast<byte>(c));
}

struct Reader {
    Span<const byte> in;
    usize pos = 0;
    bool bad = false;

    template <class T>
    T get() {
        T v{};
        if (bad || pos + sizeof(T) > in.size()) {
            bad = true;
            return v;
        }
        std::memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    StringView get_str() {
        const u8 n = get<u8>();
        if (bad || pos + n > in.size()) {
            bad = true;
            return {};
        }
        const StringView s(reinterpret_cast<const char*>(in.data() + pos), n);
        pos += n;
        return s;
    }
};

bool better(const LeaderboardInfo& l, i64 a, i64 b) { return l.lower_is_better ? a < b : a > b; }

}  // namespace

const char* game_services_event_name(GameServicesEventKind k) {
    switch (k) {
        case GameServicesEventKind::SignedIn:
            return "SignedIn";
        case GameServicesEventKind::SignedOut:
            return "SignedOut";
        case GameServicesEventKind::AchievementSent:
            return "AchievementSent";
        case GameServicesEventKind::ScoreSent:
            return "ScoreSent";
        case GameServicesEventKind::CloudSaved:
            return "CloudSaved";
        case GameServicesEventKind::CloudLoaded:
            return "CloudLoaded";
        case GameServicesEventKind::CloudCurrent:
            return "CloudCurrent";
        case GameServicesEventKind::CloudConflict:
            return "CloudConflict";
        case GameServicesEventKind::Failed:
            return "Failed";
    }
    return "?";
}

// ---- GameServices -------------------------------------------------------------------------------------

bool GameServices::available() const { return s_ && (s_->provider_ || s_->replaying()); }
bool GameServices::signed_in() const { return s_ && s_->signed_in_; }

u32 GameServices::sign_in() {
    if (!available()) return 0;
    const u32 id = s_->next_request();
    if (!s_->replaying()) {
        s_->sign_in_wanted_ = true;
        s_->interactive_ = true;
        s_->wait_ = 0;
    }
    return id;
}

const ServicesCatalog& GameServices::catalog() const {
    static const ServicesCatalog empty;
    return s_ ? s_->catalog_ : empty;
}

bool GameServices::unlock(StringView achievement) {
    if (!s_) return false;
    const u16 i = s_->catalog_.find_achievement(achievement);
    if (i == kNoService) return false;
    return add_progress(achievement, s_->catalog_.achievements()[i].steps);
}

bool GameServices::add_progress(StringView achievement, u32 steps) {
    if (!s_) return false;
    const u16 i = s_->catalog_.find_achievement(achievement);
    if (i == kNoService || steps == 0) return false;
    const u32 total = s_->catalog_.achievements()[i].steps;
    u32& reached = s_->steps_[i];
    if (reached >= total) return false;
    reached = steps >= total - reached ? total : reached + steps;
    if (!s_->replaying()) s_->persist();
    return true;
}

bool GameServices::unlocked(StringView achievement) const {
    if (!s_) return false;
    const u16 i = s_->catalog_.find_achievement(achievement);
    return i != kNoService && s_->steps_[i] >= s_->catalog_.achievements()[i].steps;
}

u32 GameServices::progress(StringView achievement) const {
    if (!s_) return 0;
    const u16 i = s_->catalog_.find_achievement(achievement);
    return i == kNoService ? 0 : s_->steps_[i];
}

bool GameServices::submit_score(StringView leaderboard, i64 score) {
    if (!s_) return false;
    const u16 i = s_->catalog_.find_leaderboard(leaderboard);
    if (i == kNoService) return false;
    if (!s_->has_best_[i] || better(s_->catalog_.leaderboards()[i], score, s_->best_[i])) {
        s_->best_[i] = score;
        s_->has_best_[i] = 1;
        s_->score_waiting_[i] = 1;
        if (!s_->replaying()) s_->persist();
    }
    return true;
}

i64 GameServices::best_score(StringView leaderboard, i64 def) const {
    if (!s_) return def;
    const u16 i = s_->catalog_.find_leaderboard(leaderboard);
    return i != kNoService && s_->has_best_[i] ? s_->best_[i] : def;
}

u32 GameServices::cloud_save(StringView save_name) {
    if (!available()) return 0;
    const u16 slot = s_->catalog_.find_cloud_save(save_name);
    if (slot == kNoService) return 0;
    const u32 id = s_->next_request();
    s_->request_slot(slot, kWantSave, id);
    return id;
}

u32 GameServices::cloud_load(StringView save_name) {
    if (!available()) return 0;
    const u16 slot = s_->catalog_.find_cloud_save(save_name);
    if (slot == kNoService) return 0;
    const u32 id = s_->next_request();
    s_->request_slot(slot, kWantLoad, id);
    return id;
}

bool GameServices::resolve_conflict(StringView save_name, bool keep_local) {
    if (!s_) return false;
    const u16 slot = s_->catalog_.find_cloud_save(save_name);
    if (slot == kNoService || !s_->slots_[slot].conflict) return false;
    GameServicesService::Slot& sl = s_->slots_[slot];
    sl.conflict = false;
    if (s_->replaying()) return true;
    if (keep_local) {
        s_->request_slot(slot, kWantSaveOverConflict, sl.request);
    } else {
        s_->apply_cloud(slot, sl.conflict_version, sl.conflict_bytes.span());
        GameServicesService::Answer a;
        a.what = GameServicesService::Answer::Resolved;
        a.index = slot;
        a.version = sl.conflict_version;
        s_->post(a);
    }
    sl.conflict_bytes.clear();
    return true;
}

u32 GameServices::pending() const { return s_ ? s_->pending() : 0; }

void GameServices::show_achievements() {
    if (available() && !s_->replaying()) s_->provider_->show_achievements();
}

void GameServices::show_leaderboard(StringView leaderboard) {
    if (!available() || s_->replaying()) return;
    const u16 i = s_->catalog_.find_leaderboard(leaderboard);
    if (i != kNoService) s_->provider_->show_leaderboard(i);
}

// ---- GameServicesService --------------------------------------------------------------------------------

GameServicesService::GameServicesService(mem::IAllocator& a)
    : alloc_(a),
      catalog_(a),
      steps_(a),
      sent_steps_(a),
      best_(a),
      has_best_(a),
      score_waiting_(a),
      slots_(a),
      inbox_(a) {}

void GameServicesService::init(const ServicesCatalog& catalog, IGameServicesProvider* provider,
                               const GameServicesOptions& opt) {
    shutdown();
    catalog_ = catalog;
    provider_ = provider;
    opt_ = opt;
    steps_.clear();
    sent_steps_.clear();
    best_.clear();
    has_best_.clear();
    score_waiting_.clear();
    slots_.clear();
    steps_.resize(catalog_.achievements().size(), 0u);
    sent_steps_.resize(catalog_.achievements().size(), 0u);
    best_.resize(catalog_.leaderboards().size(), i64(0));
    has_best_.resize(catalog_.leaderboards().size(), u8(0));
    score_waiting_.resize(catalog_.leaderboards().size(), u8(0));
    slots_.resize(catalog_.cloud_saves().size());
    signed_in_ = false;
    last_request_ = 0;
    ach_in_flight_ = kNoService;
    lb_in_flight_ = kNoService;
    sign_in_in_flight_ = false;
    sign_in_wanted_ = provider_ != nullptr;
    interactive_ = false;
    wait_ = 0;
    load_state();
    if (provider_) provider_->start(catalog_, *this);
    started_ = true;
    MCE_LOG_INFO("game_services: provider '{}', {} achievements, {} leaderboards, {} cloud saves, {} waiting",
                 provider_ ? provider_->name() : "none", catalog_.achievements().size(), catalog_.leaderboards().size(),
                 catalog_.cloud_saves().size(), pending());
}

void GameServicesService::shutdown() {
    if (!started_) return;
    if (provider_) provider_->stop();
    started_ = false;
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    inbox_.clear();
}

void GameServicesService::install(World& world) {
    world.register_event<GameServicesEvent>(Phase::FixedUpdate_Pre);
    world.set_resource(GameServices(this));
    world.set_resource(PumpRef{this});
    world.add_system(Phase::FixedUpdate_Pre, "game_services_pump", &pump_system);
}

u32 GameServicesService::pending() const {
    u32 n = 0;
    for (usize i = 0; i < steps_.size(); ++i) n += steps_[i] > sent_steps_[i] ? 1 : 0;
    for (const u8 w : score_waiting_) n += w;
    return n;
}

void GameServicesService::request_slot(u16 slot, u8 want, u32 request) {
    Slot& s = slots_[slot];
    s.request = request;
    if (replaying()) return;
    s.want = want;
}

void GameServicesService::update() {
    if (!started_ || replaying() || !provider_) return;
    provider_->update();
    if (wait_ > 0) --wait_;

    if (!signed_in_ && sign_in_wanted_ && !sign_in_in_flight_ && wait_ == 0) {
        sign_in_in_flight_ = true;
        sign_in_wanted_ = false;
        const bool interactive = interactive_;
        interactive_ = false;
        provider_->sign_in(interactive);
    }
    if (signed_in_ && wait_ == 0) {
        if (ach_in_flight_ == kNoService) {
            for (usize i = 0; i < steps_.size(); ++i)
                if (steps_[i] > sent_steps_[i]) {
                    ach_in_flight_ = static_cast<u16>(i);
                    ach_in_flight_steps_ = steps_[i];
                    provider_->report_achievement(ach_in_flight_, ach_in_flight_steps_);
                    break;
                }
        }
        if (lb_in_flight_ == kNoService) {
            for (usize i = 0; i < score_waiting_.size(); ++i)
                if (score_waiting_[i]) {
                    lb_in_flight_ = static_cast<u16>(i);
                    lb_in_flight_score_ = best_[i];
                    provider_->submit_score(lb_in_flight_, lb_in_flight_score_);
                    break;
                }
        }
    }
    for (usize i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        if (s.want == 0 || s.in_flight) continue;
        const u16 slot = static_cast<u16>(i);
        const u8 want = s.want;
        s.want = 0;
        s.in_flight = true;
        if (!signed_in_) {
            cloud_saved(slot, CloudResult::Failed, 0);  // nothing reaches the cloud signed out
            continue;
        }
        if (want == kWantLoad) {
            provider_->cloud_load(slot);
            continue;
        }
        Array<byte> bytes(alloc_);
        const StringView name = catalog_.cloud_saves()[i].view();
        if (!save::exists(name) || !save::read(name, bytes)) {
            MCE_LOG_WARN("game_services: the '{}' save is missing or unreadable; nothing to upload", name);
            cloud_saved(slot, CloudResult::Failed, 0);
            continue;
        }
        s.upload_hash = fnv1a64(bytes.data(), bytes.size());
        provider_->cloud_save(slot, want == kWantSaveOverConflict ? s.conflict_version : s.synced_version,
                              bytes.span());
    }
}

void GameServicesService::post(const Answer& a) {
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    inbox_.push_back(a);
}

void GameServicesService::signed_in(bool ok) {
    Answer a;
    a.what = Answer::SignIn;
    a.ok = ok;
    post(a);
}

void GameServicesService::achievement_sent(u16 achievement, bool ok) {
    Answer a;
    a.what = Answer::Achievement;
    a.ok = ok;
    a.index = achievement;
    post(a);
}

void GameServicesService::score_sent(u16 leaderboard, bool ok) {
    Answer a;
    a.what = Answer::Score;
    a.ok = ok;
    a.index = leaderboard;
    post(a);
}

void GameServicesService::cloud_saved(u16 slot, CloudResult r, u64 version) {
    Answer a;
    a.what = Answer::CloudSaved;
    a.result = r;
    a.index = slot;
    a.version = version;
    post(a);
}

void GameServicesService::cloud_loaded(u16 slot, CloudResult r, u64 version, Span<const byte> bytes) {
    Answer a;
    a.what = Answer::CloudLoaded;
    a.result = r;
    a.index = slot;
    a.version = version;
    for (const byte b : bytes) a.bytes.push_back(b);
    post(a);
}

bool GameServicesService::local_changed(u16 slot, u64* hash_out) const {
    const StringView name = catalog_.cloud_saves()[slot].view();
    if (!save::exists(name)) return false;
    Array<byte> bytes(alloc_);
    if (!save::read(name, bytes)) return false;
    const u64 h = fnv1a64(bytes.data(), bytes.size());
    if (hash_out) *hash_out = h;
    return slots_[slot].synced_version == 0 || h != slots_[slot].synced_hash;
}

void GameServicesService::apply_cloud(u16 slot, u64 version, Span<const byte> bytes) {
    const StringView name = catalog_.cloud_saves()[slot].view();
    if (Result<void> r = save::write(name, bytes); !r) {
        FixedString<512> why;
        format_error(r.error(), why);
        MCE_LOG_ERROR("game_services: the cloud's '{}' could not replace the local save: {}", name, why.view());
        return;
    }
    slots_[slot].synced_version = version;
    slots_[slot].synced_hash = fnv1a64(bytes.data(), bytes.size());
    persist();
}

// Driver side of an answer (in flight, retries, files); the events it yields carry the sim side.
void GameServicesService::apply_answer(World& /*world*/, const Answer& a, Array<GameServicesEvent>& out) {
    GameServicesEvent e;
    switch (a.what) {
        case Answer::SignIn:
            sign_in_in_flight_ = false;
            if (!a.ok) {
                sign_in_wanted_ = true;  // silently again after the wait
                wait_ = opt_.retry_updates;
            }
            e.kind = a.ok ? GameServicesEventKind::SignedIn : GameServicesEventKind::SignedOut;
            out.push_back(e);
            return;
        case Answer::Achievement:
            if (a.index != ach_in_flight_) return;
            ach_in_flight_ = kNoService;
            if (!a.ok) {
                wait_ = opt_.retry_updates;
                return;
            }
            e.kind = GameServicesEventKind::AchievementSent;
            e.index = a.index;
            e.value = ach_in_flight_steps_;
            out.push_back(e);
            return;
        case Answer::Score:
            if (a.index != lb_in_flight_) return;
            lb_in_flight_ = kNoService;
            if (!a.ok) {
                wait_ = opt_.retry_updates;
                return;
            }
            e.kind = GameServicesEventKind::ScoreSent;
            e.index = a.index;
            e.value = lb_in_flight_score_;
            out.push_back(e);
            return;
        default:
            break;
    }

    if (a.index >= slots_.size()) return;
    Slot& s = slots_[a.index];
    e.index = a.index;
    e.request = s.request;
    if (a.what == Answer::Resolved) {
        e.kind = GameServicesEventKind::CloudLoaded;
        e.value = static_cast<i64>(a.version);
        out.push_back(e);
        return;
    }
    s.in_flight = false;
    if (a.result == CloudResult::Failed) {
        e.kind = GameServicesEventKind::Failed;
        out.push_back(e);
        return;
    }
    if (a.what == Answer::CloudSaved) {
        if (a.result == CloudResult::Conflict) {
            s.want = kWantLoad;  // another device uploaded since: fetch it and settle the conflict
            return;
        }
        s.synced_version = a.version;
        s.synced_hash = s.upload_hash;
        persist();
        e.kind = GameServicesEventKind::CloudSaved;
        e.value = static_cast<i64>(a.version);
        out.push_back(e);
        return;
    }
    // CloudLoaded
    e.value = static_cast<i64>(a.version);
    if (a.version == 0 || a.version <= s.synced_version) {
        e.kind = GameServicesEventKind::CloudCurrent;
        out.push_back(e);
        return;
    }
    if (!local_changed(a.index, nullptr) || opt_.conflict == ConflictPolicy::PreferCloud) {
        apply_cloud(a.index, a.version, a.bytes.span());
        e.kind = GameServicesEventKind::CloudLoaded;
        out.push_back(e);
        return;
    }
    s.conflict_version = a.version;
    if (opt_.conflict == ConflictPolicy::PreferDevice) {
        s.want = kWantSaveOverConflict;
        return;
    }
    s.conflict_bytes = a.bytes;
    e.kind = GameServicesEventKind::CloudConflict;
    out.push_back(e);
}

// Sim side of an event (also while replaying), then into the world.
void GameServicesService::emit(World& world, const GameServicesEvent& e) {
    switch (e.kind) {
        case GameServicesEventKind::SignedIn:
            signed_in_ = true;
            break;
        case GameServicesEventKind::SignedOut:
            signed_in_ = false;
            break;
        case GameServicesEventKind::AchievementSent:
            if (e.index < sent_steps_.size() && static_cast<u32>(e.value) > sent_steps_[e.index])
                sent_steps_[e.index] = static_cast<u32>(e.value);
            break;
        case GameServicesEventKind::ScoreSent:
            if (e.index < best_.size() && best_[e.index] == e.value) score_waiting_[e.index] = 0;
            break;
        case GameServicesEventKind::CloudConflict:
            if (e.index < slots_.size()) slots_[e.index].conflict = true;
            break;
        case GameServicesEventKind::CloudLoaded:
        case GameServicesEventKind::CloudSaved:
            if (e.index < slots_.size()) slots_[e.index].conflict = false;
            break;
        default:
            break;
    }
    EventAccess(world).push(e);
}

void GameServicesService::pump(World& world, u64 tick) {
    Array<GameServicesEvent> events(alloc_);
    if (replayer_) {
        GameServicesReplayer& rp = *replayer_;
        while (!rp.corrupt_ && !rp.done()) {
            Reader r{rp.stream_, rp.pos_};
            const u64 at = r.get<u64>();
            if (r.bad) {
                rp.corrupt_ = true;
                break;
            }
            if (at > tick) break;
            const u32 count = r.get<u32>();
            for (u32 i = 0; i < count && !r.bad; ++i) {
                GameServicesEvent e;
                e.kind = static_cast<GameServicesEventKind>(r.get<u8>());
                e.index = r.get<u16>();
                e.request = r.get<u32>();
                e.value = r.get<i64>();
                if (static_cast<u8>(e.kind) > static_cast<u8>(GameServicesEventKind::Failed)) r.bad = true;
                events.push_back(e);
            }
            if (r.bad) {
                rp.corrupt_ = true;
                events.clear();
                break;
            }
            rp.pos_ = r.pos;
        }
        if (rp.corrupt_) MCE_LOG_ERROR("game_services: the replay stream is corrupt; no more game services events");
    } else {
        Array<Answer> in(alloc_);
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            std::swap(in, inbox_);
        }
        for (const Answer& a : in) apply_answer(world, a, events);
    }

    if (recorder_ && !replayer_ && !events.empty()) {
        Array<byte>& out = recorder_->bytes_;
        put<u64>(out, tick);
        put<u32>(out, static_cast<u32>(events.size()));
        for (const GameServicesEvent& e : events) {
            put<u8>(out, static_cast<u8>(e.kind));
            put<u16>(out, e.index);
            put<u32>(out, e.request);
            put<i64>(out, e.value);
        }
    }
    bool changed = false;
    for (const GameServicesEvent& e : events) {
        emit(world, e);
        changed |= e.kind == GameServicesEventKind::AchievementSent || e.kind == GameServicesEventKind::ScoreSent;
    }
    if (changed && !replaying()) persist();
}

// ---- record / replay ----------------------------------------------------------------------------------

void GameServicesService::set_recorder(GameServicesRecorder* r) {
    recorder_ = r;
    if (!r) return;
    Array<byte>& out = r->bytes_;
    out.clear();
    put<u32>(out, kRecordMagic);
    put<u16>(out, kFormatVersion);
    put<u16>(out, static_cast<u16>(steps_.size()));
    put<u16>(out, static_cast<u16>(best_.size()));
    put<u16>(out, static_cast<u16>(slots_.size()));
    put<u8>(out, signed_in_ ? 1 : 0);
    put<u32>(out, last_request_);
    for (usize i = 0; i < steps_.size(); ++i) {
        put<u32>(out, steps_[i]);
        put<u32>(out, sent_steps_[i]);
    }
    for (usize i = 0; i < best_.size(); ++i) {
        put<i64>(out, best_[i]);
        put<u8>(out, has_best_[i]);
        put<u8>(out, score_waiting_[i]);
    }
    for (const Slot& s : slots_) put<u8>(out, s.conflict ? 1 : 0);
}

void GameServicesService::set_replayer(GameServicesReplayer* r) {
    replayer_ = r;
    if (!r) return;
    Reader rd{r->stream_, 0};
    const u32 magic = rd.get<u32>();
    const u16 version = rd.get<u16>();
    const u16 na = rd.get<u16>();
    const u16 nl = rd.get<u16>();
    const u16 ns = rd.get<u16>();
    if (rd.bad || magic != kRecordMagic || version != kFormatVersion || na != steps_.size() || nl != best_.size() ||
        ns != slots_.size()) {
        MCE_LOG_ERROR(
            "game_services: the replay stream does not match this catalog; no game services events will replay");
        r->corrupt_ = true;
        return;
    }
    signed_in_ = rd.get<u8>() != 0;
    last_request_ = rd.get<u32>();
    for (usize i = 0; i < steps_.size(); ++i) {
        steps_[i] = rd.get<u32>();
        sent_steps_[i] = rd.get<u32>();
    }
    for (usize i = 0; i < best_.size(); ++i) {
        best_[i] = rd.get<i64>();
        has_best_[i] = rd.get<u8>();
        score_waiting_[i] = rd.get<u8>();
    }
    for (Slot& s : slots_) {
        s.conflict = rd.get<u8>() != 0;
        s.want = 0;
        s.in_flight = false;
    }
    r->pos_ = rd.pos;
    r->corrupt_ = rd.bad;
}

// ---- the saved state ----------------------------------------------------------------------------------

void GameServicesService::persist() {
    if (opt_.state_save.empty()) return;
    Array<byte> out(alloc_);
    put<u32>(out, kStateMagic);
    put<u16>(out, kFormatVersion);
    put<u16>(out, static_cast<u16>(steps_.size()));
    for (usize i = 0; i < steps_.size(); ++i) {
        put_str(out, catalog_.achievements()[i].id.view());
        put<u32>(out, steps_[i]);
        put<u32>(out, sent_steps_[i]);
    }
    put<u16>(out, static_cast<u16>(best_.size()));
    for (usize i = 0; i < best_.size(); ++i) {
        put_str(out, catalog_.leaderboards()[i].id.view());
        put<i64>(out, best_[i]);
        put<u8>(out, has_best_[i]);
        put<u8>(out, score_waiting_[i]);
    }
    put<u16>(out, static_cast<u16>(slots_.size()));
    for (usize i = 0; i < slots_.size(); ++i) {
        put_str(out, catalog_.cloud_saves()[i].view());
        put<u64>(out, slots_[i].synced_version);
        put<u64>(out, slots_[i].synced_hash);
    }
    if (Result<void> r = save::write(opt_.state_save.view(), out.span()); !r) {
        FixedString<512> why;
        format_error(r.error(), why);
        MCE_LOG_ERROR("game_services: the state was not saved: {}", why.view());
    }
}

void GameServicesService::load_state() {
    if (opt_.state_save.empty() || !save::exists(opt_.state_save.view())) return;
    Array<byte> bytes(alloc_);
    if (!save::read(opt_.state_save.view(), bytes)) {
        MCE_LOG_WARN("game_services: the saved state cannot be read; starting empty");
        return;
    }
    Reader r{bytes.span()};
    const u32 magic = r.get<u32>();
    const u16 version = r.get<u16>();
    if (r.bad || magic != kStateMagic || version > kFormatVersion) {
        MCE_LOG_WARN("game_services: the saved state is not one this build reads; starting empty");
        return;
    }
    const u16 na = r.get<u16>();
    for (u16 k = 0; k < na && !r.bad; ++k) {
        const StringView id = r.get_str();
        const u32 steps = r.get<u32>();
        const u32 sent = r.get<u32>();
        const u16 i = catalog_.find_achievement(id);
        if (r.bad || i == kNoService) continue;  // an achievement the catalog dropped
        const u32 total = catalog_.achievements()[i].steps;
        steps_[i] = steps < total ? steps : total;
        sent_steps_[i] = sent < steps_[i] ? sent : steps_[i];
    }
    const u16 nl = r.get<u16>();
    for (u16 k = 0; k < nl && !r.bad; ++k) {
        const StringView id = r.get_str();
        const i64 best = r.get<i64>();
        const u8 has = r.get<u8>();
        const u8 waiting = r.get<u8>();
        const u16 i = catalog_.find_leaderboard(id);
        if (r.bad || i == kNoService) continue;
        best_[i] = best;
        has_best_[i] = has ? 1 : 0;
        score_waiting_[i] = has && waiting ? 1 : 0;
    }
    const u16 ns = r.get<u16>();
    for (u16 k = 0; k < ns && !r.bad; ++k) {
        const StringView name = r.get_str();
        const u64 v = r.get<u64>();
        const u64 h = r.get<u64>();
        const u16 i = catalog_.find_cloud_save(name);
        if (r.bad || i == kNoService) continue;
        slots_[i].synced_version = v;
        slots_[i].synced_hash = h;
    }
    if (r.bad) MCE_LOG_WARN("game_services: the saved state is cut short; kept what was read");
}

}  // namespace mce::liveops
