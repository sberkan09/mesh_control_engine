- Assets: a project's `.mcdefaults` (convar name -> true/false, number or text) cooks to a new chunk type, `CHNK_CVARS`
  (`ConvarDefaults`), the "name value" lines `console::set_defaults` reads; `asset_create` knows the kind. Cook side
  only: the player applies it in the next PR, so existing packs and runs are unchanged. (M-WORKFLOW 7, PR 4b-1, #382)
