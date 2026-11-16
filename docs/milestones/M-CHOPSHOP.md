# M-CHOPSHOP — sell a stolen car

Status: step 1 done (2026-10-29). Closes the car-theft loop of [M-CARJACK](M-CARJACK.md): steal, drive to the scrapyard, get paid.

## Design

`ChopShop3D { radius (4 m), price (300), enabled }` (size 12) on a placed entity; event `CarSold3D { seller, shop, car, paid }`;
system `kit3d_chopshop` (after `kit3d_cashpickup`). A vehicle with `CarOwner3D.stolen` 1 and a `VehicleSeat3D` within `radius`
whose `driver` is alive and holds a `Wallet3D` is bought: `give_cash(driver, price)`, the car is destroyed through the frame's
commands and the event is pushed. `seats3d` already releases a rider whose vehicle is gone, so the driver is out next tick.

## Limits

Flat price (no damage or model value); a car stolen unseen (`needs_witness`) sells like any other; no drop-off animation or
caption; only cars with a driver are bought (an abandoned stolen car is not).
