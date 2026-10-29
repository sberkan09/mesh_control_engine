# M-OPENWORLD-LOOK — real surfaces, trees and street furniture for the open-world sample (E7)

**Status:** done (2026-10-29), the "Roads, ground, kerbs and trees are tinted boxes" item of [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) B, plus the two
small items left by [M-OPENWORLD-DAYNIGHT](M-OPENWORLD-DAYNIGHT.md) (car headlights, thunder sound). Data and content only: no engine code, the
render core (`scene3d`, `Kit3DExtract`) is untouched.

## What was built (`samples/09_openworld`)

| Piece | How |
|---|---|
| Ground, roads, sidewalks, junctions, parks | Textured slabs: `materials/{asphalt,paving,grass,concrete}.mcmat` (colour and normal maps from `tools/gen_textures.py`, 512 px, tileable, seeded) on box meshes whose UVs are in metres, so a tile covers 4 m (asphalt), 2.4 m (paving) or 8 m (grass) whatever the slab size. Meshes are written by `gen_assets.py` (`models/surface/`, deduplicated by size) |
| Road paint | One mesh per road class: edge lines, dashed lane or centre lines, a double yellow centre line on the avenues, a crosswalk and a stop line before it at both ends (the arriving lane only, traffic keeps right). A 3 cm slab in `materials/marking_{white,yellow}.mcmat`, so it does not fight the road in the depth buffer |
| Trees, lamp posts, benches, hydrants, bins | Blender (`blender/gen_models.py`, headless `bpy`; `pip install bpy`): `assets/models/props/*.glb`, plain colour materials, one metre per unit, origin at the base, a few hundred triangles each. Street trees stand at the outer edge of the sidewalks (no collider, so the crowd is not blocked), park trees have a trunk collider, every lot has a tree in a free corner |
| Street lamps | The Blender pole (with an arm) replaces the box; lamps and props now stand on the corner pad outside the junction asphalt (they stood in the avenue junctions before) |
| Headlights | `SpotLight3D` on the nose of every car (traffic, the car to take, police) with a `TimedLight3D`, on with the street lamps |
| Thunder | `ThunderDelay.clip` (CC0 `thunder.ogg`, rubberduck, `assets/audio/SOURCES.md`): each lightning flash is followed by the clap after distance/343 s |

Buildings, cars and characters stay the Kenney models.

## Decisions and findings

- **Textures are generated here, not downloaded.** ambientCG (pinned in `content/ledger.json`) answered the sandbox with 403, and Kenney's roads kit is a manual
  download, so the surfaces are made by a small seeded numpy script (original, CC0). They are plain: tinted noise, paving joints, a few cracks, grass streaks.
  Real PBR sets (`mce content add ambientcg-asphalt031` and friends) can replace the files without touching the scene.
- **Paint layout** assumes the roads' `drive: right`: local +x traffic on the +z side; z-roads are the same mesh turned -90 degrees.
- **Colour.** The grass is deliberately darker and less saturated than the old tint (it read as pastel in the captures); the sidewalk is a warm grey, not near white.
- **Not done:** kerb stones as their own mesh (the sidewalk is one slab with the paving on its sides too), traffic light heads that change colour, moving wheels,
  a sky HDRI (the DayCycle/atmosphere preset needs a render-path change and stays an open item of E6).
- **Not measured:** frame time on a real GPU. The city has about 1.7k scene entities, roughly 190k triangles in view on the start corner, and one spot light per car.

## Verification

- Screenshots on llvmpipe at the start corner, along the avenue, in a park block and at 21:30 (lamps, headlight pool): textures, paint, crosswalk, trees.
- `openworld_sample_player` (replay hash) and `test_mcp_openworld` pass; `--premerge` clean.
