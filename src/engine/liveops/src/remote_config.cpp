#include "mce/liveops/remote_config.h"

#include <cstring>

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/ecs/save.h"
#include "mce/hash.h"
#include "mce/liveops/consent.h"
#include "mce/log.h"

namespace mce::liveops {
namespace {

constexpr const char* kStage = "liveops.remote_config";

// Chunk layout v1 (little-endian): ConfigHeader | ConfigEntry[count] | string blob.
struct ConfigHeader {
    u32 count;
    u32 strings_size;
    u64 reserved;
};
static_assert(sizeof(ConfigHeader) == 16);

struct StrRef {
    u32 offset;
    u32 size;
};

struct ConfigEntry {
    StrRef key;
    StrRef str;  // String
    u8 type;     // ConfigType
    u8 reserved[7];
    u64 bits;  // Int / Bool: the i64; Float: the f64's bits
};
static_assert(sizeof(ConfigEntry) == 32);

constexpr u32 kRecordMagic = 0x47464352;  // "RCFG"
constexpr u16 kRecordVersion = 1;

StrRef add_string(Array<byte>& blob, StringView s) {
    const StrRef r{static_cast<u32>(blob.size()), static_cast<u32>(s.size())};
    for (char c : s) blob.push_back(static_cast<byte>(c));
    return r;
}

void append(Array<byte>& out, const void* p, usize n) {
    const usize at = out.size();
    out.resize_uninitialized(at + n);
    std::memcpy(out.data() + at, p, n);
}

bool read_string(const char* blob, u32 blob_size, StrRef r, StringView& out) {
    if (r.offset > blob_size || r.size > blob_size - r.offset) return false;
    out = StringView(blob + r.offset, r.size);
    return true;
}

template <class T>
void put(Array<byte>& out, T v) {
    append(out, &v, sizeof(T));
}

struct Reader {
    Span<const byte> in;
    usize pos = 0;
    bool bad = false;
    template <class T>
    T get() {
        T v{};
        if (bad || in.size() - pos < sizeof(T) || pos > in.size()) {
            bad = true;
            return v;
        }
        std::memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    Span<const byte> bytes(usize n) {
        if (bad || pos > in.size() || in.size() - pos < n) {
            bad = true;
            return {};
        }
        const Span<const byte> s(in.data() + pos, n);
        pos += n;
        return s;
    }
};

struct ServiceRef {
    RemoteConfigService* service = nullptr;
};

void apply_system(Frame& f) {
    if (RemoteConfigService* s = f.resource<ServiceRef>().service) s->apply(f.tick);
}

}  // namespace

const char* config_type_name(ConfigType t) {
    switch (t) {
        case ConfigType::Int:
            return "int";
        case ConfigType::Float:
            return "float";
        case ConfigType::Bool:
            return "bool";
        case ConfigType::String:
            return "string";
    }
    return "int";
}

const ConfigValue* ConfigValues::find(StringView key) const {
    for (const ConfigValue& v : values_)
        if (v.key.view() == key) return &v;
    return nullptr;
}

bool ConfigValues::add(const ConfigValue& v) {
    if (v.key.empty() || v.key.truncated() || v.s.truncated() || find(v.key.view())) return false;
    values_.push_back(v);
    return true;
}

void encode_config(const ConfigValues& v, Array<byte>& out) {
    out.clear();
    Array<byte> blob;
    Array<ConfigEntry> entries;
    for (const ConfigValue& c : v.values()) {
        ConfigEntry e{};
        e.key = add_string(blob, c.key.view());
        e.type = static_cast<u8>(c.type);
        if (c.type == ConfigType::String) e.str = add_string(blob, c.s.view());
        if (c.type == ConfigType::Float)
            std::memcpy(&e.bits, &c.f, sizeof(e.bits));
        else
            e.bits = static_cast<u64>(c.i);
        entries.push_back(e);
    }
    ConfigHeader h{};
    h.count = static_cast<u32>(entries.size());
    h.strings_size = static_cast<u32>(blob.size());
    append(out, &h, sizeof(h));
    if (!entries.empty()) append(out, entries.data(), entries.size() * sizeof(ConfigEntry));
    if (!blob.empty()) append(out, blob.data(), blob.size());
}

Result<void> decode_config(Span<const byte> payload, ConfigValues& out) {
    out.clear();
    if (payload.size() < sizeof(ConfigHeader)) return Error(Errc::Truncated, kStage);
    ConfigHeader h;
    std::memcpy(&h, payload.data(), sizeof(h));
    if (h.reserved != 0) return Error(Errc::ReservedNonZero, kStage);
    const u64 table = static_cast<u64>(h.count) * sizeof(ConfigEntry);
    if (sizeof(ConfigHeader) + table + h.strings_size != payload.size())
        return Error(Errc::OutOfBounds, kStage, "size");
    const char* blob = reinterpret_cast<const char*>(payload.data() + sizeof(ConfigHeader) + table);
    for (u32 i = 0; i < h.count; ++i) {
        ConfigEntry e;
        std::memcpy(&e, payload.data() + sizeof(ConfigHeader) + i * sizeof(ConfigEntry), sizeof(e));
        if (e.type > static_cast<u8>(ConfigType::String)) return Error(Errc::OutOfBounds, kStage, "type");
        StringView key, str;
        if (!read_string(blob, h.strings_size, e.key, key)) return Error(Errc::OutOfBounds, kStage, "key");
        ConfigValue v;
        v.key.assign(key);
        v.type = static_cast<ConfigType>(e.type);
        if (v.type == ConfigType::String) {
            if (!read_string(blob, h.strings_size, e.str, str)) return Error(Errc::OutOfBounds, kStage, "string");
            v.s.assign(str);
        } else if (v.type == ConfigType::Float) {
            std::memcpy(&v.f, &e.bits, sizeof(v.f));
        } else {
            v.i = static_cast<i64>(e.bits);
            if (v.type == ConfigType::Bool && v.i != 0 && v.i != 1) return Error(Errc::OutOfBounds, kStage, "bool");
        }
        if (!out.add(v)) return Error(Errc::OutOfBounds, kStage, "key", i);
    }
    return ok();
}

u64 hash_config(const ConfigValues& v) {
    Array<byte> bytes;
    encode_config(v, bytes);
    return fnv1a64(bytes.data(), bytes.size());
}

// ---- FileConfigFetcher ------------------------------------------------------------------------------

void FileConfigFetcher::fetch(ConfigFetchSink& sink) {
    asset::AssetSystem assets;
    const StringView roots[1] = {path_.view()};
    if (!assets.mount(Span<const StringView>(roots, 1))) {
        MCE_LOG_WARN("remote_config: cannot read {}", path_.view());
        return sink.fetch_failed();
    }
    for (asset::AssetKey k = 0; k < assets.asset_count(); ++k) {
        if (assets.info(k).type != asset::ChunkType::RemoteConfig) continue;
        Result<asset::AssetHandle<asset::RemoteConfigChunk>> h = assets.load_sync<asset::RemoteConfigChunk>(k);
        if (!h) break;
        ConfigValues values;
        const Result<void> r = decode_config(assets.resolve(h.value()), values);
        assets.release(asset::AnyHandle(h.value()));
        if (!r) break;
        return sink.fetched(values);
    }
    MCE_LOG_WARN("remote_config: {} holds no readable remote config", path_.view());
    sink.fetch_failed();
}

// ---- RemoteConfig (the resource) --------------------------------------------------------------------

namespace {
const ConfigValue* lookup(const RemoteConfigService* s, StringView key, ConfigType t) {
    if (!s) return nullptr;
    const ConfigValue* v = s->active().find(key);
    return v && v->type == t ? v : nullptr;
}
}  // namespace

i64 RemoteConfig::get_int(StringView key, i64 def) const {
    const ConfigValue* v = lookup(s_, key, ConfigType::Int);
    return v ? v->i : def;
}

f64 RemoteConfig::get_float(StringView key, f64 def) const {
    const ConfigValue* v = lookup(s_, key, ConfigType::Float);
    return v ? v->f : def;
}

bool RemoteConfig::get_bool(StringView key, bool def) const {
    const ConfigValue* v = lookup(s_, key, ConfigType::Bool);
    return v ? v->i != 0 : def;
}

StringView RemoteConfig::get_string(StringView key, StringView def) const {
    const ConfigValue* v = lookup(s_, key, ConfigType::String);
    return v ? v->s.view() : def;
}

Span<const ConfigValue> RemoteConfig::values() const { return s_ ? s_->active().values() : Span<const ConfigValue>(); }
u32 RemoteConfig::version() const { return s_ ? s_->version() : 0; }
bool RemoteConfig::fetched_waiting() const { return s_ && s_->fetched_waiting(); }
void RemoteConfig::apply_fetched() {
    if (s_) s_->apply_fetched();
}
void RemoteConfig::fetch() {
    if (s_) s_->fetch();
}

// ---- RemoteConfigService ----------------------------------------------------------------------------

void RemoteConfigService::overlay(const ConfigValues& over, ConfigValues& out) const {
    out.clear();
    for (const ConfigValue& d : defaults_.values()) {
        const ConfigValue* o = over.find(d.key.view());
        out.add(o && o->type == d.type ? *o : d);
    }
}

void RemoteConfigService::init(const ConfigValues& defaults, IConfigFetcher* fetcher, const RemoteConfigOptions& opt) {
    defaults_ = defaults;
    fetcher_ = fetcher;
    opt_ = opt;
    version_ = 0;
    fetched_waiting_ = false;
    apply_requested_ = false;
    fetching_ = false;
    inbox_state_.store(0, std::memory_order_release);
    active_ = defaults_;
    const char* source = "defaults";
    if (!opt_.cache_save.empty() && save::exists(opt_.cache_save.view())) {
        Array<byte> bytes;
        ConfigValues cached;
        if (save::read(opt_.cache_save.view(), bytes) && decode_config(bytes.span(), cached)) {
            overlay(cached, active_);
            source = "the last fetch";
        } else {
            MCE_LOG_WARN("remote_config: the cached values cannot be read; using the defaults");
        }
    }
    fetch_wanted_ = opt_.fetch_on_start && fetcher_ != nullptr;
    MCE_LOG_INFO("remote_config: {} values from {}, fetcher '{}'", active_.size(), source,
                 fetcher_ ? fetcher_->name() : "none");
}

void RemoteConfigService::install(World& world) {
    world.set_resource(RemoteConfig(this));
    world.set_resource(ServiceRef{this});
    world.add_system(Phase::FixedUpdate_Pre, "remote_config_apply", &apply_system);
}

void RemoteConfigService::fetch() {
    if (fetcher_ && !replayer_) fetch_wanted_ = true;
}

void RemoteConfigService::update() {
    if (replayer_ || !fetcher_) return;
    fetcher_->update();
    const u8 state = inbox_state_.exchange(0, std::memory_order_acq_rel);
    if (state != 0) {
        fetching_ = false;
        if (state == 1) {
            {
                std::lock_guard<std::mutex> lock(inbox_mutex_);
                overlay(inbox_, fetched_);
            }
            if (!opt_.cache_save.empty()) {
                Array<byte> bytes;
                encode_config(fetched_, bytes);
                if (!save::write(opt_.cache_save.view(), bytes.span()))
                    MCE_LOG_WARN("remote_config: the fetched values could not be saved for the next launch");
            }
            fetched_waiting_ = hash_config(fetched_) != hash_config(active_);
            MCE_LOG_INFO("remote_config: fetched {} values ({})", fetched_.size(),
                         fetched_waiting_ ? "waiting for the next launch or apply_fetched" : "no change");
        }
    }
    if (fetch_wanted_ && !fetching_ && (!fetcher_->needs_consent() || consent() == Consent::Granted)) {
        fetch_wanted_ = false;
        fetching_ = true;
        fetcher_->fetch(*this);
    }
}

void RemoteConfigService::fetched(const ConfigValues& v) {
    for (const ConfigValue& c : v.values()) {
        const ConfigValue* d = defaults_.find(c.key.view());
        if (!d || d->type != c.type)
            MCE_LOG_WARN("remote_config: fetched '{}' ({}) is not a default of that type; ignored", c.key.view(),
                         config_type_name(c.type));
    }
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_ = v;
    }
    inbox_state_.store(1, std::memory_order_release);
}

void RemoteConfigService::fetch_failed() {
    MCE_LOG_INFO("remote_config: fetch failed; keeping the values in force");
    inbox_state_.store(2, std::memory_order_release);
}

void RemoteConfigService::apply(u64 tick) {
    if (replayer_) {
        ConfigReplayer& rp = *replayer_;
        while (!rp.corrupt_ && !rp.done()) {
            Reader r{rp.stream_, rp.pos_};
            const u64 at = r.get<u64>();
            if (r.bad) {
                rp.corrupt_ = true;
                break;
            }
            if (at > tick) break;
            const u32 n = r.get<u32>();
            const Span<const byte> payload = r.bytes(n);
            ConfigValues v;
            if (r.bad || !decode_config(payload, v)) {
                rp.corrupt_ = true;
                break;
            }
            active_ = v;
            ++version_;
            rp.pos_ = r.pos;
        }
        return;
    }
    if (!apply_requested_) return;
    apply_requested_ = false;
    if (!fetched_waiting_) return;
    active_ = fetched_;
    fetched_waiting_ = false;
    ++version_;
    MCE_LOG_INFO("remote_config: fetched values applied at tick {}", tick);
    record_active(tick);
}

void RemoteConfigService::record_active(u64 tick) {
    if (!recorder_) return;
    Array<byte> payload;
    encode_config(active_, payload);
    put<u64>(recorder_->bytes_, tick);
    put<u32>(recorder_->bytes_, static_cast<u32>(payload.size()));
    append(recorder_->bytes_, payload.data(), payload.size());
}

void RemoteConfigService::set_recorder(ConfigRecorder* r) {
    recorder_ = r;
    if (!r) return;
    r->bytes_.clear();
    put<u32>(r->bytes_, kRecordMagic);
    put<u16>(r->bytes_, kRecordVersion);
    put<u16>(r->bytes_, 0);
    Array<byte> payload;
    encode_config(active_, payload);
    put<u32>(r->bytes_, static_cast<u32>(payload.size()));
    append(r->bytes_, payload.data(), payload.size());
}

void RemoteConfigService::set_replayer(ConfigReplayer* r) {
    replayer_ = r;
    if (!r) return;
    Reader rd{r->stream_, 0};
    const u32 magic = rd.get<u32>();
    const u16 version = rd.get<u16>();
    (void)rd.get<u16>();
    const u32 n = rd.get<u32>();
    const Span<const byte> payload = rd.bytes(n);
    ConfigValues v;
    if (rd.bad || magic != kRecordMagic || version != kRecordVersion || !decode_config(payload, v)) {
        MCE_LOG_ERROR("remote_config: the replay stream is not a remote config recording");
        r->corrupt_ = true;
        return;
    }
    active_ = v;  // the values the recording started with
    fetched_waiting_ = false;
    apply_requested_ = false;
    r->pos_ = rd.pos;
}

}  // namespace mce::liveops
