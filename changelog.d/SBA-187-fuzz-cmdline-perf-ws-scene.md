- Fuzzing covers the command line and console, `.mcperf` captures, the WebSocket transport and scene instantiate, M-FUZZ
  step 3c (SBA-187): `test_fuzz_misc`, `test_fuzz_websocket` (raw TCP peers on loopback) and `test_fuzz_scene` found no
  crash. The importer fuzz now runs `.mpg` files by default, since the MPEG decoder overflow is fixed.
