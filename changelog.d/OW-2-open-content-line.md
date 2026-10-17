- Open-licence content line, M-OPENWORLD D2 and D3: `content/ledger.json` lists pinned CC0 / CC-BY packs (Kenney city, car and
  character kits and sounds, ambientCG PBR sets, a Poly Haven sky, Quaternius animations); `mce content list|info|check|fetch|add|verify|credits`
  fetches a pack by SHA-256, cooks it into `assets/third_party/` with deterministic sidecars and materials, and keeps `CREDITS.md`
  and `third_party_content.json` current. `mce retarget <clips.glb> --onto <character.glb>` copies humanoid animation clips onto
  another skeleton (name-matched bones, world-space retarget, hip-height scaling). Guide chapters 5.3, 8.9, 16.7.
