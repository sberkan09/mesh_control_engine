- Halo sample and docs, M-HALO step 54d (closes the milestone): `samples/12_weather` carries a default `Halo` on its Weather
  entity (the entity count stays 14) and its script logs `halo:` (strength and `weather.halo_level()`) every 150 ticks next
  to `cirrus:`, pinned in `weather_test.cmake`; no other sample moves. The user guide's sky section (8.5) now says what the
  halo is, its defaults, that the Low tier turns it off, that it needs cirrus, the sun disk and a sun above about 5.7
  degrees (the moon's ring at night) and that it has no shadow and no IBL; the samples table (18) points at the sample. The
  M-HALO record and ROADMAP row are done, `Halo` left the sample-coverage allowlist and `capabilities.md` is regenerated.
