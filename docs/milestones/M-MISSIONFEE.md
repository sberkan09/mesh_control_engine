# M-MISSIONFEE — failing a mission costs money

Status: step 1 done (2026-10-29). Pairs with [M-MISSIONPAY](M-MISSIONPAY.md) and [M-MISSIONRETRY](M-MISSIONRETRY.md): a retry is no longer free.

## Design

`MissionReward3D` grows `fail_fee` (i32, default 0; size 4 to 8, no new component). `kit_missions` calls `take_cash(actor, fail_fee)`
at all three `MissionFailed` places (time limit or death, wanted level, missing objective), so the wallet never drops below zero.

## Limits

No caption; the fee is taken once per failure (a retry that fails again pays again); a mission whose actor has no wallet charges nothing.
