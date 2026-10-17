- Kit: `UiFlow.keep_audio` (with `pause`) keeps the sound going under a pause or title screen: the audio bridge and
  `SfxTrigger` run on held ticks while the game stays held (a new `audio::PauseMix` resource carries the flag). Off by
  default, so existing scenes and replays are unchanged. (M-WORKFLOW 7, PR 6+7, #382)
