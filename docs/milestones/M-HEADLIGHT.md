# M-HEADLIGHT — reflective paint under headlights

Owner: rutin 2 (M-VISUAL). Step 3 of [M-PAINTWEAR](M-PAINTWEAR.md).

**Problem.** Road markings look the same at night as by day. Real lane paint contains glass beads and throws light back toward a car's headlights.

**Plan.**
1. A retro-reflection term for materials flagged reflective: brighter when the light and the eye direction agree (headlight on a vehicle, camera behind it). Needs the coordinator: new material field and shader term.
2. The two marking materials set the flag; chipped (alpha-cut) areas already show plain asphalt, so they stay dark.
3. Vehicle headlights in the open-world sample as spot lights for the check.

**Check.** Night junction frame with and without the term.
