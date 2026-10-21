# Screens that stay right

You want to know when a change breaks how a screen looks, without a person looking.

1. **Get it right once.** `render_frame {"tick": 30}` (and with `input` to reach a moment, e.g.
   `"1-90 Move=1,0"`) until the screen looks as intended. A phone layout:
   `"safe_area": {"top": 48, "bottom": 34}`.
2. **Bless it.** `image_diff {"golden": "goldens/title.png", "render": {"tick": 30}}`: with no golden
   yet, the frame becomes the golden. Do the same for each level's first frame.
3. **Check after every change.** The same call returns `score` (0 is identical), `changed_fraction`
   and `pass` (`score <= max_score`, default 0.01), with a heat map image of what moved. A failing
   score on a screen you did not mean to change is a regression; on one you did, look at the frame
   and pass `update: true` to bless it again.
4. **Several moments at once.** `render_sheet {"ticks": [1, 60, 120, 240], "cameras": [{}, {"center": [12, 3], "zoom": 0.5}]}`
   gives one image to review.

Frames come from the real player (offscreen software rendering without a display), so the same
arguments give the same pixels on the same machine. Keep goldens in the project (they are PNG files
under any folder you choose) and commit them with the scenes.
