- Cirrus sample and docs, M-CIRRUS step 53d (closes the milestone): `samples/12_weather` carries a default `Cirrus` on its
  Weather entity (the entity count stays 14) and its script logs `cirrus:` (strength, `weather.cirrus_coverage()` and the
  plane height) every 150 ticks, pinned in `weather_test.cmake`; no other sample moves. The user guide's sky section (8.5)
  now says what the layer is, its defaults, that the Low tier turns it off and why it has no cloud shadow and is not in the
  IBL; 27.3 and the samples table (18) point at the sample. The M-CIRRUS record and ROADMAP row are done.
