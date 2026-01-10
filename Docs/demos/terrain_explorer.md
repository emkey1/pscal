# Terrain Explorer Demo

The terrain explorer showcases the procedural landscape renderer that ships with the Rea front end. It relies on the SDL/OpenGL helpers and optional landscape built-ins to stream vertex data and shading parameters to the GPU.

## Build instructions

1. Configure the project with SDL support enabled:
   ```
   cmake -S . -B build -DSDL=ON
   ```
   If the configure step reports that SDL support was disabled, install the
   SDL2/SDL3 and SDL_image development headers for your platform and rerun the
   command so that the screenshot helper can be compiled.
2. Build the Rea front end and bundled extensions:
   ```
   cmake --build build --target rea
   ```

The SDL runtime links against SDL_image to provide PNG export support. When the library is missing the demo still runs, but the screenshot helper is reported as unavailable.

## Running the explorer

After building, launch the demo from the repository root:

```
./build/bin/rea Examples/rea/sdl/landscape
```

The script prints the active controls on startup and opens a 1024×768 window. Regenerate terrain with different seeds by passing an integer argument on the command line (for example `./build/bin/rea Examples/rea/sdl/landscape 42`).

## Controls

* **W/S** or **Up/Down** – move forward/backward
* **A/D** or **Left/Right** – strafe/turn
* **Mouse** – look around
* **N/P** – cycle to the next/previous seed
* **R** – randomise the seed using the current tick counter
* **F** – toggle the accelerated draw path (if the extended built-ins are present)
* **T/Y** – decrease/increase tessellation step
* **J/U** – decrease/increase elevation scale
* **C/V/L** – toggle clouds, water, and lens flare
* **H** – toggle the HUD overlay
* **B** – start or stop the scripted biome tour
* **K** – capture a PNG screenshot of the current frame
* **Q** or **Esc** – exit the demo

## Scripted biome tour

Press **B** to trigger the narrated fly-through. The camera follows a Catmull–Rom spline that visits curated viewpoints for the temperate, desert, arctic, and twilight palettes. Palette and lighting presets are applied automatically when the corresponding extended built-ins are available. Any manual input (mouse or movement keys) immediately cancels the tour so you can resume exploring from the current position.

## Screenshot capture

While the HUD is visible, press **K** to queue a PNG capture. The demo writes sequential files named `terrain_explorer_0001.png`, `terrain_explorer_0002.png`, and so on to the working directory after the current frame is rendered. The feature requires the `GLSaveFramebufferPng` helper; when the helper is absent the HUD labels the option as unavailable and the key press logs a short explanation.

## Troubleshooting

* Exported PNGs may appear flipped vertically if the runtime was built without OpenGL double buffering. Pass `false` as the optional second parameter to `GLSaveFramebufferPng` in your own scripts to disable the default vertical flip.
* Environment variables such as `REA_LANDSCAPE_FORCE_FAST_DRAW` and `REA_LANDSCAPE_DISABLE_EXT` continue to work with the tour and screenshot helpers. When fast draw is disabled the tour still executes, but terrain regeneration during palette transitions can take longer on slower CPUs.
