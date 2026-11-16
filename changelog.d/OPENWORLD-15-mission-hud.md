- Mission HUD, M-OPENWORLD-GAPS A1/A3: `samples/09_openworld` shows the active mission (name, stops reached, a time bar and clock,
  PASSED and FAILED banners), one `mission_<id>.mcui` per `Mission` entity through `UiScreen`. `mce_ui`: a bound Panel or Image
  is visible while the field is within its `range` `[lo, hi]` (was `>= lo`), and a bound reflected enum reads as its number.
