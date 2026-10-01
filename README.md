# TSRealDriver.dll

Independent ETS2/ATS driver-walking plug-in for the TSMS ecosystem.

## TS Real Drive 0.5.4 — Grounded Walker Pose

This build replaces free-camera flying movement with an independent walker position.

- F10 resolves the live truck/door position from telemetry before walk mode can start.
- After the debug camera is selected, TS Real Drive writes the walker's SCS sector/local world position directly into the debug-camera pose.
- A stale debug-camera position is never accepted as the initial walk position; if live truck placement or the pose write is unavailable, entry aborts instead of sending the camera to an old city.
- W/A/S/D update the walker horizontally; they no longer drive Numpad 8/2/4/6 free-camera axes.
- Shift multiplies only the internal walker speed and therefore returns immediately to the exact walking speed when released.
- Mouse wheel changes an internal speed scale and is blocked from changing the game's free-camera speed while walking.
- The first ground plane is derived from the truck door/head position and configured eye height, so looking up/down no longer turns forward movement into flying.
- Ctrl changes walker eye height and Space uses walker gravity/jump velocity instead of Numpad vertical camera movement.
- Full road/terrain raycast following and step/collision resolution remain separate compatibility work; 0.5.4 intentionally prioritizes deterministic truck-door spawn and non-flying movement.

### Runtime

- `F10` leave / return to truck.
- Exit is blocked when telemetry says the truck is moving or the parking brake is not set.
- `W A S D` walk on the horizontal walker plane.
- `Shift` sprint; releasing it returns to base walking speed.
- Mouse wheel changes the walker speed scale; middle click resets it.
- `Ctrl` crouch and `Space` jump.
- `R` resets eye height.
- Walking keys remain isolated from normal truck controls while walk mode is active.
- Native game mouse look remains available while the walker position is controlled independently.

### TSMS telemetry integration

TSRealDriver reads the same public `Local\SCSTelemetry` rev12 map already used by TSMS Telemetry Lab.

It uses:

- truck speed;
- parking brake;
- engine/electrical state;
- truck world position and heading;
- cabin/head/hook local positions;
- fuel / AdBlue state;
- truck model id;
- trailer attached state and trailer world position.

If the shared map is unavailable, walking still has a camera-only fallback, but contextual interactions are reduced.

### Contextual `F` interaction

When telemetry and the estimated walker position are available, `F` is contextual instead of always returning to the cab.

Supported contexts:

- enter cab near the driver-door point;
- TS Fleet fuel-card interaction near the predicted pump point;
- nozzle/fueling near the truck tank;
- receipt at the pump;
- trailer coupling/uncoupling workflow near the fifth wheel.

### Fuel roleplay

Normal use no longer requires `F7`.

Expected flow:

1. stop the truck;
2. set parking brake;
3. turn engine off;
4. leave the truck;
5. walk to the pump point;
6. `[F] Pay with TS Fleet card`;
7. walk to the tank;
8. hold `F` — TSRealDriver holds the game's Enter key while filling;
9. return to the pump and take the receipt.

The tank position can be learned per truck model and is stored locally in `TSRealDriver.tanks.ini`.

`F7` remains as a manual fallback for diagnostics.

### Trailer roleplay

Near the truck's fifth-wheel point:

**Uncoupling**

1. lower landing gear;
2. disconnect air/electrical;
3. release fifth wheel / send the game's trailer attach-detach key.

**Coupling**

1. lock fifth wheel;
2. connect air/electrical;
3. raise landing gear / complete using the game's attach-detach key.

The game attach/detach key defaults to `T` and is configurable.

### Settings panel

`Ctrl+F10` opens the TSRealDriver panel with tabs:

- Movement
- Walk Style
- Camera
- Flashlight
- Sound
- Shadow
- Fuel
- Trailer
- Keys

The panel supports sliders, switches, presets and editable hotkeys. Saving hot-reloads `TSRealDriver.ini`.

### NativeGameBridge

The DLL includes its own native interoperability layer:

- identifies `eurotrucks2.exe` / `amtrucks.exe`;
- fingerprints the target executable's `.text` section;
- exposes an internal wildcard pattern scanner;
- records build identity in `TSRealDriver.log`.

No reference plug-in licence/token code is included.

## Requirement

Enable the game's developer camera:

```
uset g_developer "1"
```

The console is optional but useful while testing.

## Package

Copy these files into:

```
<ETS2 or ATS>\bin\win_x64\plugins\
```

Package files:

```
TSRealDriver.dll
TSRealDriver.ini
TSRealDriverConfig.exe
README.md
```

Runtime-created files:

```
TSRealDriver.log
TSRealDriver.tanks.ini
TSRealDriver.audio\
```

## What still requires real-game validation

This repository can compile automatically, but the development environment cannot launch your local ETS2/ATS installation. The following therefore must be validated in-game before calling the build 1.0:

- exact driver-door spawn position for different truck cabins;
- estimated walker position after large mouse turns;
- pump/tank geometry for different truck models;
- fifth-wheel interaction distance;
- free-camera speed restoration;
- Smart App Control behavior on the unsigned DLL;
- world collision/terrain following and true 3D flashlight/light-source integration.

The build is intentionally packaged as a **Closed Test Build**, not falsely labeled as a verified 1.0.
