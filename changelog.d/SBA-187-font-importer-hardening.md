- The font importer refuses damaged fonts instead of crashing, M-FUZZ step 3b (SBA-187): `.ttf` files are checked
  (table directory, metrics, the cmap subtable stb_truetype reads, glyph data, kern) before stb_truetype sees them, a
  glyph with an inverted or oversized box is an error, and a glyph whose outline rises above its box is skipped with a
  warning. OpenType fonts with CFF outlines are refused with "not a TrueType/OpenType font"; use plain TrueType outlines.
