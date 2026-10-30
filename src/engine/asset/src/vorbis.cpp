#include "mce/asset/vorbis.h"

#include <cstring>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"  // NOLINT(bugprone-suspicious-include): stb's single-file header part

namespace mce::asset {

namespace {
constexpr const char* kStage = "asset.vorbis";
constexpr usize kFirstArena = 128 * 1024;
constexpr usize kMaxArena = 16 * 1024 * 1024;
stb_vorbis* sv(void* v) { return static_cast<stb_vorbis*>(v); }

// stb_vorbis trusts the lengths in the comment header. A count of billions leaves it freeing a null `comment_list`, and a
// vendor or comment length of 0xFFFFFFFF makes `len + 1` wrap to 0, so it copies four billion bytes into an empty block (both
// found by the chunk fuzz test). A comment costs at least its 4-byte length field and no string can be longer than the stream,
// so a header whose vendor, count or comment lengths do not fit the data is refused here, before stb sees it.
bool comment_header_fits(Span<const byte> data) {
    static constexpr char kMagic[7] = {3, 'v', 'o', 'r', 'b', 'i', 's'};
    const usize n = data.size();
    for (usize i = 0; i + 11 <= n; ++i) {
        if (std::memcmp(data.data() + i, kMagic, sizeof(kMagic)) != 0) continue;
        auto u32_at = [&](usize at) { return static_cast<u32>(data[at]) | static_cast<u32>(data[at + 1]) << 8 | static_cast<u32>(data[at + 2]) << 16 | static_cast<u32>(data[at + 3]) << 24; };
        const usize vendor = u32_at(i + 7);
        if (vendor > n - (i + 11)) return false;
        usize at = i + 11 + vendor;
        if (at + 4 > n) return true;  // truncated: stb stops at the end of data on its own
        const usize count = u32_at(at);
        at += 4;
        if (count > (n - at) / 4 + 1) return false;
        for (usize k = 0; k < count && at + 4 <= n; ++k) {
            const usize len = u32_at(at);
            at += 4;
            if (len > n - at) return false;
            at += len;
        }
        return true;
    }
    return true;
}
}  // namespace

VorbisReader::VorbisReader(mem::IAllocator& a) : arena_(a) {}

VorbisReader::~VorbisReader() { close(); }

void VorbisReader::close() {
    if (v_) stb_vorbis_close(sv(v_));
    v_ = nullptr;
    channels_ = sample_rate_ = 0;
    frames_ = 0;
}

Result<void> VorbisReader::open(Span<const byte> data) {
    close();
    if (data.empty() || data.size() > 0x7FFFFFFFu) return Error(Errc::InvalidArgument, kStage, "empty or oversized stream");
    if (!comment_header_fits(data)) return Error(Errc::ImportFailed, kStage, "not an Ogg Vorbis stream");
    if (arena_.size() < kFirstArena) arena_.resize(kFirstArena);
    for (;;) {
        stb_vorbis_alloc alloc;
        alloc.alloc_buffer = reinterpret_cast<char*>(arena_.data());
        alloc.alloc_buffer_length_in_bytes = static_cast<int>(arena_.size());
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_memory(reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()), &err, &alloc);
        if (v) {
            const stb_vorbis_info info = stb_vorbis_get_info(v);
            v_ = v;
            channels_ = static_cast<u32>(info.channels);
            sample_rate_ = info.sample_rate;
            frames_ = stb_vorbis_stream_length_in_samples(v);
            return ok();
        }
        if (err != VORBIS_outofmem || arena_.size() >= kMaxArena) return Error(Errc::ImportFailed, kStage, "not an Ogg Vorbis stream", static_cast<u32>(err));
        arena_.resize(arena_.size() * 2);
    }
}

bool VorbisReader::seek(u64 frame) {
    if (!v_ || frame > frames_ || frame > 0xFFFFFFFFull) return false;
    return stb_vorbis_seek(sv(v_), static_cast<unsigned int>(frame)) != 0;
}

u32 VorbisReader::read(f32* out, u32 max_frames) {
    if (!v_ || max_frames == 0) return 0;
    const u64 floats = static_cast<u64>(max_frames) * channels_;
    const int n = stb_vorbis_get_samples_float_interleaved(sv(v_), static_cast<int>(channels_), out,
                                                           static_cast<int>(floats > 0x7FFFFFFF ? 0x7FFFFFFF : floats));
    return n > 0 ? static_cast<u32>(n) : 0;
}

Result<void> decode_vorbis(Span<const byte> data, Array<f32>& out, u32& channels, u32& sample_rate) {
    VorbisReader r;
    MCE_TRY(r.open(data));
    channels = r.channels();
    sample_rate = r.sample_rate();
    out.clear();
    out.resize(static_cast<usize>(r.frames() * r.channels()));
    u64 at = 0;
    while (at < r.frames()) {
        const u64 left = r.frames() - at;
        const u32 got = r.read(out.data() + at * channels, static_cast<u32>(left > 65536 ? 65536 : left));
        if (got == 0) break;
        at += got;
    }
    if (at != r.frames()) return Error(Errc::Truncated, kStage, "the stream ended early", static_cast<u32>(at));
    return ok();
}

}  // namespace mce::asset
