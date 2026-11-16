- Security: Luau bytecode is trusted only from the game's own pack. `MountOptions::allow_code` defaults to false (Script entries
  are dropped at mount with a warning); the player's game pack and the project's own cook ask for true, mods, base packs and
  fetched config do not.
