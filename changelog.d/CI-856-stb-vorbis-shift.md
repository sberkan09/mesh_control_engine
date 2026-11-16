- Build: the stb_vorbis translation unit is compiled with `-fno-sanitize=shift`. stb shifts a packet's end marker
  (-1) left while reading a truncated Ogg header, which UBSan reports in C; the ASan stage's chunk fuzz reached it
  once new sound clips joined its corpus. AddressSanitizer still covers the decoder.
