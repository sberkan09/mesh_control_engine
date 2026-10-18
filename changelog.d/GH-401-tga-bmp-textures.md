- TGA and BMP textures (M-IMPORTMEDIA step 1, GH-401): `.tga` (uncompressed and RLE, 8/16/24/32 bit) and `.bmp` files cook like PNG
  with the same sidecar settings (`srgb`, `mips`, `filter`, `max_size`, `compress`). Every 8-bit image's header is checked
  (at most 16384 a side) before the decoder allocates its pixels. User guide 5 lists the formats.
