# Marble Garden: art summary

Agent: **art**. My work covered everything the player sees and hears.

## Tasks
- #7 Warm afternoon look: done
- #8 Audio: done
- #9 Fonts and UI style: done
- #13 Polish pass: done

## Made
- **Textures** (gen_texture, under `assets/textures/`): grass, stone, hedge, wood, water and an equirect afternoon sky.
- **Materials** (`assets/materials/*.mcmat`): grass, stone, hedge, wood, water (blend), marble (deep blue, glossy) and gem (ruby, emissive). They are applied to every MeshRef in the garden and to the title, win and lose plinths.
- **Environment3D** (garden): warm low sun at [1, .8, .55] and intensity 3.2, the sky texture, sky IBL, light warm fog, exposure .65, 3 cascades and soft bloom.
- **Models**: `models/gem.glb` is now a hexagonal crystal made with gen_mesh. I overwrote the file, so its GUID did not change.
- **Audio** (`assets/audio/`, all generated): music loop (D pentatonic, 84 bpm, no drums), gem chime, splash, ui blip, win jingle and lose tone.
- **Audio wiring**:
  - Garden: the Music loop, the Chime and Splash SfxTriggers (GardenGame fires them) and a round-start blip.
  - Title, win and lose: a quieter music loop.
  - Win plays a jingle and lose plays a tone, both on Start.
  - A headless run showed the chime firing once for each gem.
- **Fonts and UI**: Crimson Pro Bold and Regular (OFL, from the library) and a style guide in the #9 notes. The HUD uses a cream panel sprite with dark brown text.
- **Credits**: gen_* and library_add wrote all the rows. I removed a stale template row after deleting the unused template textures and `path.mcmat`.
- **Checks**: project_check passes with no warnings, and test_run passes 10/10.

## Friction filed (3)
1. gen_sfx: the tool schema does not list the accepted `params.wave` values.
2. There is no particle system for 3D scenes (`.mcpart` is 2D only), so gem pickups have no sparkles.
3. project_check's contrast check measures HUD text against the 2D camera's clear colour, not against the panel sprite behind it. To work around this, I set the HUD camera's clear_color to the panel colour.
