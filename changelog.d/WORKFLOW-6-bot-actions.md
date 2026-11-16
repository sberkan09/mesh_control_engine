- Test bots that act, M-WORKFLOW step 6c (#391): `playtest` / `.mctest` bots take `hold` (actions held the whole run, a
  crouch walk), `hold_when` (`[{action, entity, field, value}]`, held while a component field check is true: crouch while
  `Perceiver.sees` is 1), `stop_when` (stand still while a field check is true) and a `walk_to` waypoint
  `{at, wait, press: [actions]}` that presses once on arrival. A stealth test no longer needs a custom C++ system writing
  `InputState`. User guide 14 describes them.
