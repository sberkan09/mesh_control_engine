#include "mce/ecs/save.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "mce/asset/xxhash.h"

namespace mce::save {

namespace {

constexpr const char* kStageWrite = "save.write";
constexpr const char* kStageRead = "save.read";
constexpr const char* kStageReplay = "save.replay";

struct SaveHeader {  // 64 bytes, little-endian
    u32 magic;
    u16 format_version;
    u16 reserved0;
    u32 module_version;
    u32 record_count;
    u64 scene_hi;
    u64 scene_lo;
    u64 tick;
    f64 playtime;
    u64 body_size;
    u64 body_hash;  // xxh64 of the body
};
static_assert(sizeof(SaveHeader) == 64);

struct ReplayHeader {  // 80 bytes
    u32 magic;
    u16 format_version;
    u16 reserved0;
    u32 module_version;
    u32 reserved1;
    u64 scene_hi;
    u64 scene_lo;
    u64 start_tick;
    u64 tick_count;
    u64 start_hash;
    u64 final_hash;
    u64 stream_size;
    u64 stream_hash;
};
static_assert(sizeof(ReplayHeader) == 80);

template <class T>
void put(Array<byte>& out, const T& v) {
    const usize at = out.size();
    out.resize_uninitialized(at + sizeof(T));
    std::memcpy(out.data() + at, &v, sizeof(T));
}

void put_bytes(Array<byte>& out, const void* p, usize n) {
    const usize at = out.size();
    out.resize_uninitialized(at + n);
    if (n) std::memcpy(out.data() + at, p, n);
}

class Reader {
public:
    explicit Reader(Span<const byte> b) : b_(b) {}
    template <class T>
    bool get(T& v) {
        if (b_.size() - pos_ < sizeof(T)) return false;
        std::memcpy(&v, b_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    bool bytes(usize n, const byte*& p) {
        if (b_.size() - pos_ < n) return false;
        p = b_.data() + pos_;
        pos_ += n;
        return true;
    }
    bool done() const { return pos_ == b_.size(); }

private:
    Span<const byte> b_;
    usize pos_ = 0;
};

// Byte size of a field's whole storage (fixed arrays included).
u32 field_bytes(const refl::FieldRecord& f) { return f.size * f.count; }

struct Field {
    u64 name;
    u8 kind;
    u32 size;
    const byte* data;
};

FixedString<512>& root_storage() {
    static FixedString<512> r;  // main-thread configuration, set once at startup
    return r;
}

namespace stdfs = std::filesystem;
stdfs::path to_path(StringView s) { return stdfs::path(s.begin(), s.end()); }

}  // namespace

// ============================================================================ payloads

void write_save(World& w, const SaveInfo& info, Array<byte>& out) {
    Array<byte> body;
    u32 records = 0;
    const ecs::ComponentId key_id = w.component_id<SaveKey>();
    w.query<const SaveKey&>().each([&](Entity e, const SaveKey& k) {
        ++records;
        put(body, k.key);
        const usize count_at = body.size();
        put(body, u32(0));
        u32 comps = 0;
        for (usize ci = 0; ci < w.component_count(); ++ci) {
            const auto c = static_cast<ecs::ComponentId>(ci);
            if (c == key_id) continue;
            const ecs::ComponentInfo& info_c = w.component(c);
            const void* row = w.get_raw(e, c);
            if (!row || info_c.refl_id == 0) continue;
            const refl::TypeRecord* t = refl::find_type(StringId(info_c.refl_id));
            if (!t) continue;
            u32 nfields = 0;
            for (const refl::FieldRecord& f : t->fields) nfields += f.has(refl::FieldFlags::Save) ? 1 : 0;
            if (nfields == 0) continue;
            ++comps;
            put(body, info_c.refl_id);
            put(body, nfields);
            for (const refl::FieldRecord& f : t->fields) {
                if (!f.has(refl::FieldFlags::Save)) continue;
                put(body, f.name.value);
                put(body, static_cast<u8>(f.kind));
                put(body, field_bytes(f));
                put_bytes(body, static_cast<const byte*>(row) + f.offset, field_bytes(f));
            }
        }
        std::memcpy(body.data() + count_at, &comps, sizeof(comps));
    });
    SaveHeader h{};
    h.magic = kSaveMagic;
    h.format_version = kSaveFormatVersion;
    h.module_version = info.module_version;
    h.record_count = records;
    h.scene_hi = info.scene.hi;
    h.scene_lo = info.scene.lo;
    h.tick = info.tick;
    h.playtime = info.playtime;
    h.body_size = body.size();
    h.body_hash = asset::xxh64(body.data(), body.size());
    out.clear();
    put(out, h);
    put_bytes(out, body.data(), body.size());
}

Result<LoadStats> read_save(World& w, Span<const byte> payload, SaveInfo* info, u32 module_version) {
    MCE_ASSERT_MSG(!w.in_phase(), "read_save runs outside phases");
    if (payload.size() < sizeof(u32)) return Error(Errc::Truncated, kStageRead);
    SaveHeader h;
    u32 magic;
    std::memcpy(&magic, payload.data(), sizeof(magic));
    if (magic != kSaveMagic) return Error(Errc::BadMagic, kStageRead);
    if (payload.size() < sizeof(SaveHeader)) return Error(Errc::Truncated, kStageRead);
    std::memcpy(&h, payload.data(), sizeof(h));
    if (h.format_version > kSaveFormatVersion) return Error(Errc::VersionTooNew, kStageRead, {}, h.format_version);
    if (payload.size() - sizeof(SaveHeader) != h.body_size) return Error(Errc::Truncated, kStageRead);
    const Span<const byte> body(payload.data() + sizeof(SaveHeader), static_cast<usize>(h.body_size));
    if (asset::xxh64(body.data(), body.size()) != h.body_hash) return Error(Errc::CrcMismatch, kStageRead);

    // Pass 1: parse everything (no world mutation until the whole body is known good).
    struct Comp {
        u64 type;
        u32 first_field;
        u32 field_count;
    };
    struct Record {
        u32 key;
        u32 first_comp;
        u32 comp_count;
    };
    Array<Record> records;
    Array<Comp> comps;
    Array<Field> fields;
    Reader r(body);
    for (u32 i = 0; i < h.record_count; ++i) {
        Record rec{};
        if (!r.get(rec.key) || !r.get(rec.comp_count)) return Error(Errc::Truncated, kStageRead, {}, i);
        rec.first_comp = static_cast<u32>(comps.size());
        for (u32 c = 0; c < rec.comp_count; ++c) {
            Comp cp{};
            if (!r.get(cp.type) || !r.get(cp.field_count)) return Error(Errc::Truncated, kStageRead, {}, i);
            cp.first_field = static_cast<u32>(fields.size());
            for (u32 f = 0; f < cp.field_count; ++f) {
                Field fd{};
                if (!r.get(fd.name) || !r.get(fd.kind) || !r.get(fd.size) || !r.bytes(fd.size, fd.data))
                    return Error(Errc::Truncated, kStageRead, {}, i);
                fields.push_back(fd);
            }
            comps.push_back(cp);
        }
        records.push_back(rec);
    }
    if (!r.done()) return Error(Errc::Truncated, kStageRead);

    LoadStats st;
    st.module_version_mismatch = h.module_version != module_version;
    if (info) {
        info->module_version = h.module_version;
        info->scene = asset::Guid{h.scene_hi, h.scene_lo};
        info->tick = h.tick;
        info->playtime = h.playtime;
    }

    // Pass 2: apply on top of the live world.
    HashMap<u32, Entity> by_key;
    w.query<const SaveKey&>().each([&](Entity e, const SaveKey& k) { by_key.insert_or_assign(k.key, e); });
    for (const Record& rec : records) {
        Entity e;
        if (const Entity* found = by_key.find(rec.key)) {
            e = *found;
        } else {
            e = w.spawn_with(SaveKey{rec.key});
            by_key.insert_or_assign(rec.key, e);
            ++st.spawned;
        }
        ++st.entities;
        for (u32 ci = 0; ci < rec.comp_count; ++ci) {
            const Comp& cp = comps[rec.first_comp + ci];
            const refl::TypeRecord* t = refl::find_type(StringId(cp.type));
            const ecs::ComponentId c = t ? w.find_component_by_refl(cp.type) : ecs::kInvalidComponent;
            if (c == ecs::kInvalidComponent || w.component(c).size != t->size) {
                ++st.unknown_components;
                continue;
            }
            if (!w.get_raw(e, c)) {
                // Missing on the live entity: add it with its C++ defaults, then write fields.
                Array<byte> def;
                def.resize(t->size, byte{0});
                if (t->construct_default) t->construct_default(def.data());
                w.set_raw(e, c, def.data());
            }
            byte* row = static_cast<byte*>(w.get_raw_mut(e, c));
            for (u32 fi = 0; fi < cp.field_count; ++fi) {
                const Field& fd = fields[cp.first_field + fi];
                const refl::FieldRecord* f = t->find_field(StringId(fd.name));
                if (!f) {
                    ++st.unknown_fields;
                    continue;
                }
                if (static_cast<u8>(f->kind) != fd.kind || field_bytes(*f) != fd.size) {
                    ++st.mismatched_fields;
                    continue;
                }
                std::memcpy(row + f->offset, fd.data, fd.size);
                ++st.fields;
            }
            w.touch(e, c);
        }
    }
    return st;
}

void write_replay(const ReplayInfo& info, Span<const byte> input_stream, Array<byte>& out) {
    ReplayHeader h{};
    h.magic = kReplayMagic;
    h.format_version = kReplayFormatVersion;
    h.module_version = info.module_version;
    h.scene_hi = info.scene.hi;
    h.scene_lo = info.scene.lo;
    h.start_tick = info.start_tick;
    h.tick_count = info.tick_count;
    h.start_hash = info.start_hash;
    h.final_hash = info.final_hash;
    h.stream_size = input_stream.size();
    h.stream_hash = asset::xxh64(input_stream.data(), input_stream.size());
    out.clear();
    put(out, h);
    put_bytes(out, input_stream.data(), input_stream.size());
}

Result<Span<const byte>> read_replay(Span<const byte> payload, ReplayInfo& info) {
    if (payload.size() < sizeof(u32)) return Error(Errc::Truncated, kStageReplay);
    ReplayHeader h;
    u32 magic;
    std::memcpy(&magic, payload.data(), sizeof(magic));
    if (magic != kReplayMagic) return Error(Errc::BadMagic, kStageReplay);
    if (payload.size() < sizeof(ReplayHeader)) return Error(Errc::Truncated, kStageReplay);
    std::memcpy(&h, payload.data(), sizeof(h));
    if (h.format_version > kReplayFormatVersion) return Error(Errc::VersionTooNew, kStageReplay, {}, h.format_version);
    if (payload.size() - sizeof(ReplayHeader) != h.stream_size) return Error(Errc::Truncated, kStageReplay);
    const Span<const byte> stream(payload.data() + sizeof(ReplayHeader), static_cast<usize>(h.stream_size));
    if (asset::xxh64(stream.data(), stream.size()) != h.stream_hash) return Error(Errc::CrcMismatch, kStageReplay);
    info.module_version = h.module_version;
    info.scene = asset::Guid{h.scene_hi, h.scene_lo};
    info.start_tick = h.start_tick;
    info.tick_count = h.tick_count;
    info.start_hash = h.start_hash;
    info.final_hash = h.final_hash;
    return stream;
}

// ============================================================================ files

void set_root(StringView dir) { root_storage().assign(dir); }

FixedString<512> default_root(StringView game) {
    FixedString<512> r;
#if defined(_WIN32)
    const char* base = std::getenv("LOCALAPPDATA");
    r.assign(base ? base : ".");
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg) {
        r.assign(xdg);
    } else {
        r.assign(home ? home : ".");
        r.append("/.local/share");
    }
#endif
    r.append("/MCE/");
    r.append(game);
    r.append("/saves");
    return r;
}

FixedString<512> root() {
    if (root_storage().empty()) root_storage() = default_root("MCE");
    return root_storage();
}

FixedString<512> path_of(StringView name) {
    FixedString<512> p = root();
    p.append("/");
    p.append(name);
    p.append(".mcesave");
    return p;
}

bool exists(StringView name) {
    std::error_code ec;
    return stdfs::exists(to_path(path_of(name).view()), ec);
}

Result<void> write(StringView name, Span<const byte> payload) {
    const FixedString<512> p = path_of(name);
    if (p.truncated()) return Error(Errc::InvalidArgument, kStageWrite, name);
    const stdfs::path target = to_path(p.view());
    std::error_code ec;
    stdfs::create_directories(target.parent_path(), ec);
    if (ec) return Error(Errc::IoError, kStageWrite, p.view(), static_cast<u32>(ec.value()));

    stdfs::path tmp = target;
    tmp += ".tmp";
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return Error(Errc::AccessDenied, kStageWrite, p.view());
    const bool wrote = payload.empty() || std::fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    const bool flushed = std::fflush(f) == 0;
    std::fclose(f);
    if (!wrote || !flushed) {
        stdfs::remove(tmp, ec);
        return Error(Errc::IoError, kStageWrite, p.view());
    }
    // Rotate the previous save to .bak, then move the new one in (rename never overwrites on
    // every platform, so both steps clear their destination first).
    stdfs::path bak = target;
    bak += ".bak";
    if (stdfs::exists(target, ec)) {
        stdfs::remove(bak, ec);
        stdfs::rename(target, bak, ec);
        if (ec) return Error(Errc::IoError, kStageWrite, p.view(), static_cast<u32>(ec.value()));
    }
    stdfs::rename(tmp, target, ec);
    if (ec) return Error(Errc::IoError, kStageWrite, p.view(), static_cast<u32>(ec.value()));
    return ok();
}

Result<void> read(StringView name, Array<byte>& out) {
    const FixedString<512> p = path_of(name);
    if (p.truncated()) return Error(Errc::InvalidArgument, kStageRead, name);
    std::FILE* f = std::fopen(p.c_str(), "rb");
    FixedString<512> used = p;
    if (!f) {
        used.append(".bak");
        f = std::fopen(used.c_str(), "rb");
        if (!f) return Error(Errc::FileNotFound, kStageRead, p.view());
    }
    out.clear();
    bool good = std::fseek(f, 0, SEEK_END) == 0;
    const long size = good ? std::ftell(f) : -1;
    good = good && size >= 0 && std::fseek(f, 0, SEEK_SET) == 0;
    if (good) {
        out.resize_uninitialized(static_cast<usize>(size));
        good = size == 0 || std::fread(out.data(), 1, static_cast<usize>(size), f) == static_cast<usize>(size);
    }
    std::fclose(f);
    if (!good) {
        out.clear();
        return Error(Errc::IoError, kStageRead, used.view());
    }
    return ok();
}

Result<void> save_game(World& w, StringView name, const SaveInfo& info) {
    Array<byte> bytes;
    write_save(w, info, bytes);
    return write(name, bytes.span());
}

Result<LoadStats> load_game(World& w, StringView name, SaveInfo* info, u32 module_version) {
    Array<byte> bytes;
    if (auto r = read(name, bytes); !r) return r.error();
    auto st = read_save(w, bytes.span(), info, module_version);
    if (!st) {
        Error e = st.error();
        e.path.assign(path_of(name).view());
        return e;
    }
    return st;
}

}  // namespace mce::save
