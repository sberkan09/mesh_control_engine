- Fuzz seeds for the new texture and audio formats (M-IMPORTMEDIA step 3, GH-403): `test_fuzz_importers` mutates built TGA (raw,
  RLE, 32-bit) and BMP files and the committed MP3 and FLAC tone clips (header-byte sweep, short prefixes, byte mutants). No
  finding. ADR-0049 (dr_libs) is Accepted; M-IMPORTMEDIA is closed.
