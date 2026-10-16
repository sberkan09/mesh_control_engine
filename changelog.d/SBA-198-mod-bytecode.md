- Security: mod packs can no longer carry Luau bytecode. Script entries of a pack mounted with `allow_code = false` (the player
  mounts mods that way) are dropped with a warning, so hostile bytecode never reaches `luau_load`.
