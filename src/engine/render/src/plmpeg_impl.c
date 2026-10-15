// pl_mpeg (ADR-0043): the MPEG-1 video / MP2 audio decoder, compiled once here with warnings off.
// render/src/video.cpp is its only caller (the pl_mpeg wall).
#include <stddef.h>  /* size_t: PLM_NO_STDIO leaves out the stdio.h that would bring it */
#include <stdlib.h>
#include <string.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
