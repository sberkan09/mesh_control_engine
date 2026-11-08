# M-HARDEN5 — Silent `FixedString` truncation in the caller-text paths M-HARDEN4 did not reach (milestone record)

**Status:** step 1 done 2026-11-08, step 2 in progress. **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-11-08 because the Linear Backlog holds only device-bound work (SBA-112, SBA-58) and M-HARDEN4 is done. No Linear issues. Does not touch the render, rhi, kit (GTA and weather), physics3d or openworld modules that other threads own.

**Why.** `FixedString::assign` truncates silently by design (`truncated()` reports it). M-HARDEN4 step 4 found two real bugs of that shape within four clusters: `scene::request_load` loaded the slot a cut name matched (#1168), and the items cooker cut a stat name past 31 bytes (#1176). A caller that never reads `truncated()` is the pattern; this milestone finds the rest and fixes each with a regression test.

**Scope.** Device-free and GPU-free. A fix to core production code is its own small PR with a regression test that fails without it.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Survey: `.assign(` calls in `src/engine/{net,platform,input,asset,ui,liveops,ecs,scene,script,audio}` against `truncated()` checks | done 2026-11-08: one check per 13 to 38 assigns in net, platform, input and ui; none in audio and script. Candidates, in the order to test: `RtcHost::start` stores `desc.ice_server` in a `FixedString<256>` (`net/src/rtc.cpp`) and cuts a longer TURN URL silently, so the link then fails against the wrong server; `InputScript::add` refuses an action over 31 bytes (`input/src/script.cpp`) but no test sits on the 31/32 boundary; platform and ui paths still to read |
| 2 | One cluster per PR: a test at N and N+1 for each candidate, and a fix where the cut changes meaning | in progress: input script action name at 31/32 bytes in `test_script.cpp`. `RtcHost::start` ICE server: judged not worth a fix, the value is a `stun:host:port` URL that is far below 255 bytes in practice and a fix would add a failure path to a void start; platform and ui paths still to read |

**Gates:** each step's tests run in the module's own test binary, so `--task` runs them.
