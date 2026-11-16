- Luau scripts: hidden state is caught at run time, M-SCRIPT3 step 2a. After a script's top level runs, every table its
  `update()` and globals reach is frozen (a write fails at its line, also through an alias, a helper's parameter, a global
  table or a `newproxy` metatable), and the upvalues and buffers it reaches are compared after each `update()`: a change
  disables the script that tick with an error naming what changed. `ScriptBudget::check_state` (default on) switches it.
