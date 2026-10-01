# TSRealDriver native interoperability map

This document records the **game subsystems** that TSRealDriver needs to interoperate with. It is based on observable behaviour, public SCS telemetry concepts, the game's own controls, and static capability analysis of the reference plug-in supplied for comparison.

No licence/token mechanism, private server protocol, embedded artwork, sound assets, proprietary addresses, or copied machine-code signatures are used here.

## Reference capability map

The supplied reference plug-in exports the normal SCS telemetry entry points:

- `scs_telemetry_init`
- `scs_telemetry_shutdown`

Its observable/static capability map points at these categories:

1. **Game camera system**
   - camera manager
   - debug/free camera
   - vehicle interior/cabin camera
   - developer-camera state
   - free-camera speed

2. **Input**
   - keyboard polling
   - raw mouse input
   - input suppression/remapping while walking
   - synthetic game-key activation for contextual actions

3. **World/physics**
   - ground queries
   - collision/raycast/sweep style queries
   - nearby static and moving objects
   - map/prop focus around the walker

4. **Rendering**
   - D3D11
   - Present-time overlay rendering
   - prompts/menu/shadow
   - flashlight support

5. **Lighting**
   - game light-source integration
   - beam direction/range/angle/colour

6. **Truck/trailer state**
   - truck world placement
   - speed / engine / parking brake
   - fuel / AdBlue
   - trailer connected state and world placement
   - trailer coupling routines, legs and cable state

7. **Fuel interaction**
   - pump/tank proximity
   - card/nozzle/receipt state machine
   - holding the game's Activate key during filling
   - remembered tank position per truck model

## TSRealDriver architecture

TSRealDriver will implement its own adapters:

- `NativeGameBridge` — executable/build identification and safe module-section scanner
- `CameraBridge` — our own resolver/controller for the game's camera subsystem
- `PhysicsBridge` — our own ground/collision query adapter
- `RenderBridge` — TSRealDriver overlays, prompts, settings and shadow
- `LightBridge` — TSRealDriver flashlight integration
- `TruckStateBridge` — SCS/TSMS truck and trailer state
- `FuelInteraction` — TS Fleet card/nozzle/receipt flow
- `TrailerInteraction` — coupling/uncoupling workflow

The current `NativeGameBridge` deliberately contains **no copied reference offsets or signatures**. It fingerprints the target game's executable and provides a generic wildcard scanner so compatibility data discovered from our own ETS2/ATS testing can be added per build.
