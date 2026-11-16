- Script state check defaults, M-SCRIPT3 step 2b: `-script.state_check` (-1 auto, 0, 1; auto is off only in a shipped game's window), `mce_player -headless` exits 3
  when a script failed, and Luau bytecode carries local and upvalue names (`debugLevel 2`, script importer version 3), so the error says `upvalue 'n'`.
