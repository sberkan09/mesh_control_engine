/* stb implementations for the cook (compiled without the engine warning policy: third-party). */
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
/* Radiance .hdr (M-VISUAL3 4) is read as floats by stbi_loadf; PNG/JPEG still only go through the 8-bit loader. */
#define STBI_ONLY_HDR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
