/* dr_mp3 and dr_flac implementations for the cook (M-IMPORTMEDIA 2; compiled without the engine warning policy:
   third-party). Memory input only: no stdio, no Ogg-contained FLAC. The decoders are used through audio_decode.cpp. */
#define DR_MP3_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
