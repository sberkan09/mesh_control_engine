# M-CARJACK — stealing a car is a crime

Status: steps 1 and 2 done (2026-10-28). Small gameplay milestone for the GTA-like slice, joining M-OPENWORLD C1 (boarding cars)
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

## Built (step 2): witnesses

`CarOwner3D.needs_witness` (default 0, size still 8). With 1 the theft counts only when `crime_seen` is true: the rider's
`Wanted3D.require_witness` is off, or a living `Police3D` unit within its `witness_range` has a clear line to the car.
An unseen theft still sets `stolen`, so the car is not counted again later. `crime_seen(Frame&, offender, at)` is a new
public function in `wanted3d.h` (it reuses the witness test the civilian crimes use). Test: with a unit in range the
theft counts, with none it does not, with `needs_witness` 0 it always counts.

## Out of scope

Owners that fight back or flee, car alarms, hot-wiring time, a stolen car being reported
to the police as a description.
