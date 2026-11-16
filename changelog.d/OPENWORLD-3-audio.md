- Sound for the open-world sample, M-OPENWORLD E3: `kit/world_audio.h` adds `EngineAudio` (an engine whose pitch and gain follow a `Vehicle3D`'s
  speed), `WeaponAudio` (a shot, or a sound set, per `WeaponFired3D`), `FootstepAudio` (a step every stride while a `CharacterController3D`
  is grounded and on foot) and `SirenAudio` (a looping source that follows the `Wanted3D` level), all installed by `install_stock`.
  `samples/09_openworld` uses them with CC0 sounds from OpenGameArt (`assets/audio/SOURCES.md`, `tools/fetch_audio.py`): engines on the
  parked and traffic cars, gun shots, footsteps for the player and police, sirens, a city bed, a master limiter and a pause snapshot. User guide 10.9.
