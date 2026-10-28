# M-MISSIONPAY — missions pay cash

Status: step 1 done (2026-10-28). Small gameplay milestone joining M-MISSION and M-CASH: a finished job pays.

## Why

Money from kills (M-CASH) is one source; the GTA loop's other one is missions. Missions had no reward at all.

## Design

`MissionReward3D { cash (100) }` (size 4) on the Mission entity. When the last objective is done and the mission goes
`Complete`, `kit_missions` credits `Mission.actor`'s `Wallet3D` through `give_cash` (so `CashChanged3D` is pushed).
No reward component, no wallet on the actor, a failed mission, or a mission with nothing to do: nothing is paid. The
component is registered by `install_wanted3d`; worlds that never install it see no change, and `missions.cpp` only reads
it with `try_get`, so there is no new install wiring and no change to scenes without it.

## Built (step 1)

`MissionReward3D` in `wanted3d.h/.cpp`; `pay_reward` in `missions.cpp`. Test: a completed mission pays 250, an actor
without a wallet is paid nothing, a failed mission pays nothing.

## Out of scope

Item rewards, a "+$N" caption (the game's, on `CashChanged3D`), rewards on single objectives.
