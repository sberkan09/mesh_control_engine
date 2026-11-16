- Content: kenney.nl now hands out a short-lived signed link per download, so the six Kenney packs in
  `content/ledger.json` are `manual`: download the zip from the pack's page and pass it with
  `mce content add <pack> <project> --file <zip>` (the pinned SHA-256 still checks it). `mce content` now refuses a
  download that comes back as a web page and says so, instead of reporting a hash mismatch. Also fixes a dangling
  `string_view` in the scene lint test that AddressSanitizer caught (the integration run had been red since #781).
