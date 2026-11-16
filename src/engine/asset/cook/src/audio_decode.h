#pragma once
// MP3 and FLAC decoding for the cook's audio importer (M-IMPORTMEDIA step 2). Cook-private: the importer factories are
// declared here and not in importer.h (169 translation units); only wav_importer.cpp, importer_registry.cpp and the
// cook's tests include this.

#include "mce/cook/audio.h"
#include "mce/cook/importer.h"

namespace mce::cook {

// Decodes a whole MP3 / FLAC file into interleaved floats in [-1, 1]. The file is untrusted: both decoders run on a
// budgeted allocator (kAudioDecodeMemory) and stop at kAudioMaxFrames. False with `why` set on any problem, including a
// stream that is not mono or stereo, has no audio, or is cut off before its first frame decodes.
bool decode_mp3(Span<const byte> bytes, PcmClip& out, FixedString<128>& why);
bool decode_flac(Span<const byte> bytes, PcmClip& out, FixedString<128>& why);

inline constexpr u64 kAudioDecodeMemory = u64(256) << 20;  // outstanding bytes the decoders may hold
inline constexpr u64 kAudioMaxFrames = u64(48000) * 60 * 60;  // one hour at 48 kHz
inline constexpr u64 kAudioMaxSamples = u64(1) << 27;         // 512 MiB of floats, the importer's working-set cap

mem::UniquePtr<IImporter> make_mp3_importer();   // .mp3 -> CHNK_SFX (compress defaults to "vorbis")
mem::UniquePtr<IImporter> make_flac_importer();  // .flac -> CHNK_SFX (compress defaults to "pcm")

}  // namespace mce::cook
