#include "cook_db.h"

#include <cstdio>
#include <cstring>

#include "mce/cook/fs.h"
#include "mce/asset/file_io.h"

namespace mce::cook {

namespace {

constexpr u32 kDbMagic = 0x4244434D;  // "MCDB"
constexpr u32 kDbVersion = 1;

struct FileHeader {
    u32 magic;
    u32 version;
};

struct RecordHead {  // fixed part of a record body
    u64 guid_hi, guid_lo;
    u64 source_hash, settings_hash, importer_id;
    u32 importer_version, tool_version;
    u64 output_hash;
    u16 type, reserved;
    u32 path_size;
    u32 dep_count;
    u32 reserved2;
};
static_assert(sizeof(RecordHead) == 72);

void put(Array<byte>& out, const void* p, usize n) {
    const usize at = out.size();
    out.resize(at + n);
    if (n) std::memcpy(out.data() + at, p, n);
}

void serialize(const DbRecord& r, Array<byte>& out) {
    Array<byte> body;
    RecordHead h{};
    h.guid_hi = r.guid.hi;
    h.guid_lo = r.guid.lo;
    h.source_hash = r.source_hash;
    h.settings_hash = r.settings_hash;
    h.importer_id = r.importer_id;
    h.importer_version = r.importer_version;
    h.tool_version = r.tool_version;
    h.output_hash = r.output_hash;
    h.type = static_cast<u16>(r.type);
    h.path_size = static_cast<u32>(r.source_path.size());
    h.dep_count = static_cast<u32>(r.deps.size());
    put(body, &h, sizeof(h));
    put(body, r.source_path.c_str(), r.source_path.size());
    for (const DbDep& d : r.deps) {
        const u64 v[3] = {d.guid.hi, d.guid.lo, d.output_hash};
        put(body, v, sizeof(v));
    }
    const u32 size = static_cast<u32>(body.size());
    const u32 crc = crc32(body.data(), body.size());
    put(out, &size, 4);
    put(out, &crc, 4);
    put(out, body.data(), body.size());
}

}  // namespace

void CookDb::load(StringView path) {
    records_.clear();
    index_.clear();
    journal_records_ = 0;
    needs_rewrite_ = false;
    Array<byte> bytes;
    if (!asset::read_file(path, bytes)) return;
    const u8* p = reinterpret_cast<const u8*>(bytes.data());
    const usize n = bytes.size();
    FileHeader fh{};
    if (n < sizeof(fh)) {
        needs_rewrite_ = true;
        return;
    }
    std::memcpy(&fh, p, sizeof(fh));
    if (fh.magic != kDbMagic || fh.version != kDbVersion) {
        needs_rewrite_ = true;
        return;
    }
    usize pos = sizeof(fh);
    while (pos < n) {
        u32 size = 0, crc = 0;
        if (n - pos < 8) break;
        std::memcpy(&size, p + pos, 4);
        std::memcpy(&crc, p + pos + 4, 4);
        if (size < sizeof(RecordHead) || size > n - pos - 8 || crc32(p + pos + 8, size) != crc) break;
        const u8* body = p + pos + 8;
        RecordHead h;
        std::memcpy(&h, body, sizeof(h));
        if (static_cast<u64>(sizeof(h)) + h.path_size + static_cast<u64>(h.dep_count) * 24 != size) break;
        DbRecord r;
        r.guid = asset::Guid{h.guid_hi, h.guid_lo};
        r.source_hash = h.source_hash;
        r.settings_hash = h.settings_hash;
        r.importer_id = h.importer_id;
        r.importer_version = h.importer_version;
        r.tool_version = h.tool_version;
        r.output_hash = h.output_hash;
        r.type = static_cast<asset::ChunkType>(h.type);
        r.source_path = String(StringView(reinterpret_cast<const char*>(body + sizeof(h)), h.path_size));
        for (u32 i = 0; i < h.dep_count; ++i) {
            u64 v[3];
            std::memcpy(v, body + sizeof(h) + h.path_size + i * 24, sizeof(v));
            r.deps.push_back(DbDep{asset::Guid{v[0], v[1]}, v[2]});
        }
        if (u32* at = index_.find(r.guid)) {
            records_[*at] = std::move(r);
        } else {
            index_.insert_or_assign(r.guid, static_cast<u32>(records_.size()));
            records_.push_back(std::move(r));
        }
        ++journal_records_;
        pos += 8 + size;
    }
    if (pos != n) needs_rewrite_ = true;  // corrupt or truncated tail: drop it on the next commit
}

const DbRecord* CookDb::find(asset::Guid g) const {
    const u32* at = index_.find(g);
    return at ? &records_[*at] : nullptr;
}

bool CookDb::commit(StringView path, Span<const DbRecord> fresh) {
    for (const DbRecord& r : fresh) {
        if (u32* at = index_.find(r.guid)) {
            records_[*at] = r;
        } else {
            index_.insert_or_assign(r.guid, static_cast<u32>(records_.size()));
            records_.push_back(r);
        }
    }
    journal_records_ += static_cast<u32>(fresh.size());
    const bool rewrite = needs_rewrite_ || !fs::exists(path) || journal_records_ > 2 * records_.size() + 64;
    if (rewrite) {
        Array<byte> out;
        const FileHeader fh{kDbMagic, kDbVersion};
        put(out, &fh, sizeof(fh));
        for (const DbRecord& r : records_) serialize(r, out);
        if (!fs::write_file_atomic(path, out.span())) return false;
        journal_records_ = static_cast<u32>(records_.size());
        needs_rewrite_ = false;
        return true;
    }
    if (fresh.empty()) return true;
    Array<byte> out;
    for (const DbRecord& r : fresh) serialize(r, out);
    String p(path);
    std::FILE* f = std::fopen(p.c_str(), "ab");
    if (!f) return false;
    const bool okw = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return okw;
}

}  // namespace mce::cook
