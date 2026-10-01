# TSRealDriver.dll

Independent ETS2/ATS driver-walking plug-in built from scratch.

## Version 0.2

This build now contains real runtime behavior inside `TSRealDriver.dll`, not only configuration placeholders.

Implemented in the DLL:

- `F10` toggles walking mode.
- Walking mode requests the game's developer/free camera with the top-row `0` key.
- Automatic small exit offset toward the driver's door.
- `W A S D` drive the game free-camera movement controls.
- `Shift` increases/decreases free-camera movement speed while held.
- `Space` performs a short up/hang/down jump cycle.
- `Ctrl` lowers eye height while held and raises it again on release.
- `Q / E` lower/raise the eye manually.
- `R` performs an eye-height reset pulse.
- `F` returns to the cab when normal walk interaction is active.
- `F8` toggles building/ghost-walk status.
- `~` pauses/resumes TSRealDriver walking input for console use.
- Right mouse button toggles the TSRealDriver flashlight overlay.
- `G` cycles flashlight beam size.
- `Ctrl+F10` opens `TSRealDriverConfig.exe`.
- `TSRealDriver.ini` is hot-reloaded when saved.
- On-screen TSRealDriver prompts/status overlay.
- `F7` arms the current fuel-roleplay flow:
  1. TS Fleet card
  2. nozzle
  3. hold `F` to hold the game's Enter key while fueling
  4. receipt
- Local `TSRealDriver.log` runtime log.

## Important game setting

The walking bridge uses the developer/free camera already present in ETS2/ATS.

In the game's `config.cfg`:

```
uset g_developer "1"
```

The console can also be enabled for debugging, but TSRealDriver specifically depends on the developer camera.

## Install

Copy these files from the Windows x64 build artifact into the game's x64 plug-ins folder:

```
TSRealDriver.dll
TSRealDriver.ini
TSRealDriverConfig.exe
```

Typical folder:

```
<game>\bin\win_x64\plugins\
```

## Configuration

Run `TSRealDriverConfig.exe` or press `Ctrl+F10` while the plug-in is loaded.

The current editor is functional but intentionally simple. The next UI pass will replace it with the dark tabbed TSRealDriver panel based on the reference layout supplied during development.

## Clean-room boundary

TSRealDriver does not contain TM Real Walk source code, licence/token logic, private server endpoints, copied proprietary images, copied sounds, or copied menu artwork. The controls and user-facing interaction categories are being implemented independently with TSRealDriver code and branding.

## Current limitations

The current walking engine is built on the game's developer/free camera rather than a game-memory camera controller. Because of that, exact ground collision, truck-door position, world-space hand models, true in-world flashlight lighting, and per-truck fuel-tank detection still require a deeper game-integration layer and testing against ETS2/ATS builds.
