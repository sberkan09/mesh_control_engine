// Ogg Vorbis encoding for "compress": "vorbis" clips (M-AUDIO2 step 1, ADR-0044): libvorbis in VBR
// mode, one logical stream with a fixed serial number, so the same samples and quality cook to the
// same bytes on a host. The runtime decodes with asset::VorbisReader (stb_vorbis).

#include <vorbis/vorbisenc.h>

#include <cstring>

#include "mce/cook/audio.h"

namespace mce::cook {

namespace {

constexpr int kSerial = 0x4D434531;  // "MCE1"
constexpr u32 kBlock = 1024;          // frames handed to the analyser at a time

void append(Array<byte>& out, const unsigned char* p, long n) {
    if (n <= 0) return;
    const usize at = out.size();
    out.resize(at + static_cast<usize>(n));
    std::memcpy(out.data() + at, p, static_cast<usize>(n));
}

void flush_pages(ogg_stream_state& os, Array<byte>& out, bool force) {
    ogg_page page;
    while (force ? ogg_stream_flush(&os, &page) : ogg_stream_pageout(&os, &page)) {
        append(out, page.header, page.header_len);
        append(out, page.body, page.body_len);
    }
}

}  // namespace

bool encode_vorbis(Span<const f32> samples, u32 channels, u32 sample_rate, f32 quality, Array<byte>& out, FixedString<128>& why) {
    out.clear();
    if (channels == 0 || channels > 2 || sample_rate == 0) {
        why = "vorbis: 1 or 2 channels at a known rate";
        return false;
    }
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, static_cast<long>(channels), static_cast<long>(sample_rate), quality) != 0) {
        vorbis_info_clear(&vi);
        why = "vorbis: the encoder refused this rate, channel count or quality";
        return false;
    }
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "mce_cook");
    vorbis_dsp_state vd;
    vorbis_block vb;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_state os;
    ogg_stream_init(&os, kSerial);

    ogg_packet header, header_comm, header_code;
    vorbis_analysis_headerout(&vd, &vc, &header, &header_comm, &header_code);
    ogg_stream_packetin(&os, &header);
    ogg_stream_packetin(&os, &header_comm);
    ogg_stream_packetin(&os, &header_code);
    flush_pages(os, out, true);  // the audio starts on a fresh page

    const u64 frames = samples.size() / channels;
    u64 at = 0;
    bool eos = false;
    while (!eos) {
        const u64 n = frames - at < kBlock ? frames - at : kBlock;
        if (n == 0) {
            vorbis_analysis_wrote(&vd, 0);  // end of stream
        } else {
            float** buf = vorbis_analysis_buffer(&vd, static_cast<int>(n));
            for (u64 i = 0; i < n; ++i)
                for (u32 c = 0; c < channels; ++c) buf[c][i] = samples[(at + i) * channels + c];
            vorbis_analysis_wrote(&vd, static_cast<int>(n));
            at += n;
        }
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            ogg_packet op;
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                flush_pages(os, out, false);
                if (op.e_o_s) eos = true;
            }
        }
    }
    flush_pages(os, out, true);

    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return true;
}

}  // namespace mce::cook
