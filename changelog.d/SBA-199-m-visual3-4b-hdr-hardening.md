- HDR/EXR cook hardening, M-VISUAL3 step 4b (SBA-199): a `.hdr` or `.exr` over 64 megapixels (8192 x 8192) is refused from its
  header with a cook error that says to downscale the source, before any pixel is decoded; truncated `.hdr` files are
  refused instead of padded; the decoder's buffer is used in place (no second full-size copy).
