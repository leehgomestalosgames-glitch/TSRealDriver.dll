# TSRealDriver.dll

Independent ETS2/ATS driver-walking plug-in for the TSMS ecosystem.

## TSRealDriver 0.4.2 — Camera Stabilization

This build addresses the first recorded in-game ATS walk test.

- Forces the normal interior camera before entering developer/free camera.
- Uses **Numpad 0** to activate the developer/free camera.
- Corrects free-camera reverse to **Numpad 5**.
- Controls `g_flyspeed` while the screen is faded so the timed door offset no longer launches the camera several metres away at the game's default fly speed.
- Uses a controlled spawn speed, then switches to a walking-speed free camera and restores the configured free-camera speed when returning to the cab.
- Reduces the generated footstep level further while the audio replacement work continues.
- Keeps the 44.1 kHz regenerated interaction audio from 0.4.1.

This build is intended to make the driver spawn just outside the cab instead of appearing high above, below, or far away from the truck.

### Runtime

- `F10` leave / return to truck.
- Exit is blocked when TSMS telemetry says the truck is still moving or the parking brake is not set.
- Uses the ETS2/ATS developer/free camera as the current movement backend.
- `W A S D` -> free-camera forward/back/left/right using Numpad 8/2/4/6.
- Walking keys are isolated from normal truck controls while walk mode is active.
- `Shift` run-speed boost.
- Mouse wheel changes walking/free-camera speed; middle click resets the manual speed offset.
- `Ctrl` crouch.
- `Space` jump.
- `Q / E` lower/raise eye height.
- `R` eye-height reset.
- `F8` building/ghost-walk state.
- `~` pauses the walking input bridge while using the game console.
- Head bob / side sway, footsteps, running breath, door sound and fade transition.
- Right mouse button flashlight overlay and `G` beam-size cycle.
- Walking shadow fallback overlay.
- Low-level mouse tracking maintains an estimated walker heading/position for contextual interactions.

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
