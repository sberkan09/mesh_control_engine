# M-VISUAL7: open-world lots, plots and trees (milestone record)

Added 2026-11-28, owner: the M-VISUAL routine 2 thread, from a fresh look at the day frame `notes/audit-day-2.png` (150 frames of `scenes/city.mcscene` on llvmpipe, after M-VISUAL4..6) and the earlier audit `assessments/acik-dunya-gorsel-denetimi-2026-11-26.md`, whose six items are all done (M-VISUAL4). No side-by-side with another game was made. Each step is measured on a fresh frame before and after, and a step that leaves the frame unchanged is recorded as tried, not shipped. Content-only steps first; render-core work waits for review.

| # | Step | State |
|---|---|---|
| 1 | Building lots meet the sidewalk as bare lawn: the buildings stand straight on grass with no forecourt, path or edging. Add a paved forecourt strip along each building front and a short path to the sidewalk (thin `paving` slabs) | planned |
| 2 | Lot edges have no hedge, fence or wall against the sidewalk, so the lawn runs into the paving. A low hedge or fence row along the lot edge, built from the existing `bush` prop or a thin slab | planned |
| 3 | Street trees share one trunk colour and one leaf mass (M-VISUAL4 6 varied the tone only). Vary the trunk and crown scale per tree | planned |
| 4 | Shopfront glass with a room behind each pane: needs a non-square interior cell grid in `MCE_GLSL_INTERIOR` and a new material parameter (see M-VISUAL6 step 1); render core, review before merge, deferred | deferred |
