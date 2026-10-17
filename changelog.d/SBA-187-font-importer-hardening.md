- The font importer refuses damaged fonts instead of crashing, M-FUZZ step 3b (SBA-187): `.ttf`/`.otf` files are checked
  (table directory, metrics, the cmap subtable stb_truetype reads, glyph data, kern) before stb_truetype sees them, and
  a glyph that would scale past 1024 px is an error. OpenType fonts with CFF outlines and TrueType fonts with scaled or
  rotated composite glyphs are now refused with "not a TrueType/OpenType font"; use a font with plain TrueType outlines.
