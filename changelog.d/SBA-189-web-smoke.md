- Web stage, SBA-189: the browser smoke gives the canvas screenshot 180 s (a 3D sample's frame takes about a minute
  under SwiftShader, so `04_diorama`, `04_cubes` and `05_ashen_court` timed out at 30 s), and `crash::add_context` /
  `remove_context` exist in a browser build, so a project that uses the script host links for the web again.

The web stage's Kindle replay baseline (`tests/web/baselines.json`) moves to `fb72acbe8baa8b11`: the old value predates the
Kindle link fix and the hash is the same drawn and headless on two runs.
