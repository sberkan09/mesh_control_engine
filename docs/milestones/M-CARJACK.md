# M-CARJACK — stealing a car is a crime

Status: step 1 done (2026-10-28). Small gameplay milestone for the GTA-like slice, joining M-OPENWORLD C1 (boarding cars)
and the wanted level (C3).

## Why

A player can board any car with a seat, and it costs nothing. In the slice the loop is: take a car, get seen, get chased.

## Design

`CarOwner3D { heat (8), stolen }` (size 8) on a vehicle that belongs to someone. The first time a rider with a `Wanted3D`
boards it (`kit3d_seats`), `report_crime` adds `heat` to the rider and `stolen` is set, so one car counts once however
often it is boarded again. A car without `CarOwner3D` (the player's own, a police car) costs nothing. The crime does not
need a police witness: the owner is assumed to call it in. A rider without `Wanted3D` is unaffected.

## Built (step 1)

`CarOwner3D` in `wanted3d.h/.cpp` (registered by `install_wanted3d`), the check in `seats3d.cpp` where `VehicleEntered3D`
is pushed (so no new event reading and no install wiring). Test: an owned car adds 8 heat once, an unowned car and a rider
without `Wanted3D` add none, and boarding the same car again adds nothing.

## Out of scope

Owners that fight back or flee, witnesses deciding the heat, car alarms, hot-wiring time, a stolen car being reported
to the police as a description.
