# M-WEATHERNIGHT — rain at night no longer lights the sky up

Owner: rutin 2 (M-VISUAL), SBA-234 (bug fix). Follow-up of [M-WEATHER](M-WEATHER.md).

**Bug.** Each weather kind pulls the fog colour toward a fixed daylight grey (`fog_to`, rain 0.5 at `fog_mix` 0.6). At night the
base fog is almost black, so rain (and snow, overcast, storm) turned a night sky and street into a bright grey. Found while
looking for puddle reflections in `samples/09_openworld`: rain at 21:30 looked like an overcast afternoon.

**Fix.** `kind_target` (`weather.cpp`) scales `fog_to` so its luma is never more than 1.25 times the luma of the base fog (the
colour DayCycle or the author wrote). A day keeps the colour it always had, because a daylight fog is bright enough that the
scale stays 1 (the existing rain formula test still holds). Render-side and hash-side nothing else changes.

**Test.** `weather_env: rain at night does not light the fog up`. Before and after on llvmpipe: `/mnt/project-files/notes/` has
`wet-night-fixed.png` (rain at 21:30 at a junction: dark sky, wet road, lamp sheen on the asphalt).
