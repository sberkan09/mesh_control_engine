# M-OPENWORLD-ART — trees and buildings that stop looking painted (open-world sample)

**Status:** step 1 (free path) done in the cloud, 2026-10-31; screenshots below. The paid generated-model trial (Tripo / Runpod) is prepared but
**not started**: it waits for Sami's budget approval. Source: `assessments/gta-gorsel-kalite-3d-uretim-arastirmasi-2026-10-31.md` (project files).
Lighting and sky are a separate step (thread "GTA sahnesinde ışık ve gökyüzü özelliklerini aç"); this one only touches models and their materials.

## What was wrong

- **Buildings** were Kenney models stretched to the lot's box (`w/2, h/(top+1), d/2`): the windows of a 14 m wide, 60 m tall tower were
  smeared, and the whole facade was one flat palette colour (no normal map, no roughness, no lit windows).
- **Trees and bushes** were flat-shaded icosphere blobs with one colour and no texture: the "paper cut-out" look.
- Street props (lamp post, hydrant, bin, bench) were flat-shaded, so round things looked faceted.

## What was built (`samples/09_openworld`, data and content only)

| Piece | How |
|---|---|
| Buildings | `tools/gen_buildings.py` writes one small `.glb` per lot (`assets/models/buildings/lot_*.glb`, 3-12 KB, listed by `gen_assets.py`). The walls are the lot's exact size, the UVs count bays (about 3 m, snapped per wall) and floors (about 3.2 m, snapped per building), so a window is the same size on every building and never cut at a corner. A shop band (4.5 m) at the bottom, a cornice, a parapet, rooftop boxes, and above 40 m a setback tier with a mast. |
| Facade textures | Five styles (`glass_tower`, `concrete_office`, `brick`, `stucco`, `modern_white`), two shop fronts, trim and roof, each with colour, normal map, ORM (occlusion, roughness, metallic) and, for windows, an emissive map with about 16% of the windows lit (warm or cool). 512 px, tiling, generated and seeded (1.4 MB), original to this repo (CC0). |
| Trees and bushes | `blender/gen_models.py`: crowns of about 190 alpha-masked leaf cards (`Textures/leaf_cluster.png`), soft outward crown normals so a crown shades as one volume, a darker inner layer; smooth tapered bark tubes with a bark colour and normal map; the pine is five tiers of boughs (`Textures/pine_bough.png`). 300-450 triangles per tree. The `.glb` references its textures as external files, the way the Kenney models do. |
| Street props | Smooth-shaded where round (edges over 35 degrees stay hard). |

Cars and characters are still Kenney models (see below).

## Decisions and limits

- Buildings are generated, not AI-made: modular, with real window scale and a collider that matches. The research found generated meshes unsuitable
  for buildings (one lump, no bays, no interiors).
- The lit-window emissive is a constant (strength 0.5, so at noon only a faint glow): at noon the lit windows still glow a little. A day-cycle driven strength is the lighting thread's call.
- `kenney-city-kit-commercial` and `-suburban` are no longer used by the city scene (cars, characters and the sidecars of their models remain).
- Still open: cars (one lump each in a Kenney palette, no wheel nodes to replace), characters (blocky), road signs, fences, parked-car variety.
- Paid trial (Tripo vs open source on Runpod) for one car, one building and three props: not started, needs a budget approval (estimate in the research file).
