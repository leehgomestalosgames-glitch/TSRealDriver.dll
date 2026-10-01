# TSRealDriver.dll

Clean-room ETS2/ATS walking/driver-interaction plugin.

## Goal

TSRealDriver is an independent plugin inspired by the *type of experience* offered by walking plugins: leave the cab, move with familiar keys, interact around the truck, configure behaviour in-game, and later add fuel-stop roleplay.

This repository does **not** contain TM Real Walk code, assets, licence/token logic, or copied proprietary resources.

## Current milestone

**v0.1 bootstrap**

- Windows x64 DLL named `TSRealDriver.dll`
- SCS telemetry-plugin entry points
- `F10` debug-camera bridge
- WASD/Q/E remap while walk mode is active
- `F` returns to the cab in the bootstrap bridge
- `Ctrl+F10` opens a native configuration editor
- hot-reloadable `TSRealDriver.ini`
- local logging
- GitHub Actions Windows build

Advanced systems (ground following, collision, gravity/jump, flashlight, shadow, on-screen prompts, fuel-card/nozzle/receipt flow, polished in-game settings panel) are represented in the config/roadmap and will be implemented independently in later milestones.

## Game requirement

The bootstrap walking bridge uses the game's developer/free camera. In the game profile's `config.cfg`, enable:

```
uset g_developer "1"
```

The game console is optional for TSRealDriver itself, but useful for debugging.

## Install

1. Build/download the Windows x64 artifact.
2. Copy `TSRealDriver.dll` and `TSRealDriver.ini` to:
   - ETS2: `bin/win_x64/plugins/`
   - ATS: `bin/win_x64/plugins/`
3. Start the game and load a profile.
4. Press `F10` to toggle the bootstrap walking bridge.
5. Press `Ctrl+F10` to edit configuration.

## Safety/design rule

We do not bypass or remove another developer's licence system. TSRealDriver is built as a separate implementation with its own branding and code.
