# Terrain Explorer smoke test

## Prerequisites

* Build the Rea front end with SDL support (`cmake -S . -B build -DSDL=ON && cmake --build build --target rea`).
* Ensure the working directory is the repository root so relative asset paths resolve correctly.

## Steps

1. Launch the demo:
   ```
   ./build/bin/rea Examples/rea/sdl/landscape
   ```
2. Confirm the HUD shows entries for **FAST DRAW**, **BIOME TOUR**, **SCREENSHOT**, and **FRAME**. The tour/screenshot lines should read `N/A` when the relevant helpers are unavailable.
3. Press **B** to start the biome tour. Observe the camera follow a smooth spline between at least three distinct vistas while palette/lighting presets change. The HUD should report the tour as `ON` during playback.
4. While the tour is running, move the mouse or tap a movement key to interrupt it. Verify that a console message reports the tour stopping due to manual input and control immediately returns to the player.
5. Press **K** to queue a PNG capture. After the next frame the console should log the destination path (for example `terrain_explorer_0001.png`). Check the working directory to ensure the file exists and updates on subsequent captures.
6. Toggle **F**, **C**, **V**, and **L** to confirm that fast draw and the weather toggles still function while the new features are present. No crashes or rendering corruption should occur.
7. Quit with **Q** or **Esc** and rerun the demo with a different numeric seed to ensure regeneration continues to work while the tour remains available.

## Expected results

* The biome tour runs hands-off until interrupted or the loop completes.
* Manual input always cancels the tour immediately.
* PNG captures succeed when SDL_image is available; otherwise the HUD labels the feature as unavailable and the console reports why.
* Existing controls (movement, regeneration, HUD toggle, fast draw) behave as before.
