- Video: a damaged `.mpg` no longer reads past the decoder's picture (heap-buffer-overflow found by M-FUZZ in
  pl_mpeg's macroblock copy, where a half-pel motion vector read one row or column past the bound that upstream
  checks). pl_mpeg is patched at fetch time (`cmake/patch_pl_mpeg.cmake`) until upstream has a fix; `test_render`
  keeps one such stream as a regression case.
