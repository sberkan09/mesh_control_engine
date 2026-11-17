# M-TAILLIGHT — Brake lights on every car (milestone record)

**Status:** done (2026-11-17), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-OPENWORLD-GAPS ("lights
animation"); M-OWHORN listed "no brake or tail lights". No Linear issues (free limit); this record is the tracker.

**What.** Kit `BrakeLight3D` (12 bytes: `braking`, `idle`, engine-written `lit`) in `mce/kit/vehicle_extras.h`, handled by
`kit_vehicle_extras`. It goes with a `PointLight3D` on a lamp entity that is a `ChildOf` the car. The car counts as braking when
its `brake` or `handbrake` is above 0.05, when the player drives it with the brake trigger or handbrake key held, or when the
throttle pushes against the way it rolls (the same rule `kit3d_vehicles` feeds the wheels). The lamp then shows `braking`,
otherwise `idle`; `idle` 0 switches the lamp off, so a car that never brakes costs no light.

**Sample.** `car_visuals` in `samples/09_openworld/gen_assets.py` appends a "Tail lamp" child (red `PointLight3D`, radius 4.5 m,
`braking` 7) to every car: the car to take, parked cars, police and traffic. Idle is off, so only braking cars add a light.

**Tests.** `tests/kit/test_vehicle_extras.cpp`: lit while `brake` is set, back to `idle`, off at idle 0.
`tests/mcp/test_mcp_openworld.cpp`: at least five `BrakeLight3D` in the city.

**Not done.** White reverse lamps and a glowing lens mesh (the lamp lights the road, the body stays unlit); a door that swings.
