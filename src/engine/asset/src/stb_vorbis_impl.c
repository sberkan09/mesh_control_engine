/* stb_vorbis's implementation for mce_asset (asset/vorbis.h, ADR-0044): memory streams only,
   pull API only. Third-party code: compiled without the engine warning policy. */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"  // NOLINT(bugprone-suspicious-include): single-file library, this unit is its implementation
