# M-GRASSVAR — lawn with dry patches and worn spots

Owner: rutin 2 (M-VISUAL), SBA-236 (data only). Follow-up of [M-ROADWEAR](M-ROADWEAR.md).

**Problem.** The big lawns beside the avenues were one flat dark green with only faint tone noise.

**Change.** `samples/09_openworld/tools/gen_textures.py` `grass()`: slightly brighter base, sun-dried yellow patches and a few
bare soil spots. Only `grass_color.jpg` changes (normal map and other textures are byte-identical). No code or shader change.

**Check.** Junction frame after: `/mnt/project-files/notes/grass-after.png` (right-hand lawn).
