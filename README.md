# NukePlayer

The runtime host of [NukeEngine](https://github.com/Luastris/NukeEngine-Eco) — the exe a
shipped game runs on. No editor code, no ImGui. `File → Package Project` in the editor
renames it to `<GameName>.exe`, stamps the game's icon into its resources and lays out
the full `dist/` around it.

## Boot

1. Mount `content/game.nupak` (the packed game) + every mod enabled in
   `config/mods.json` (dependency-ordered; a mod with a missing requirement is skipped
   with a log).
2. Load modules from `modules/` per the game manifest; scripting backends load their
   assemblies/scripts from the pak.
3. Open the startup world (mod layers MERGE per-property) and run:
   `Time::NewFrame → World::Update → World::Render` each frame; cameras render to the
   backbuffer.

A **Release** player refuses to start without a pak (release builds never open raw
project trees); Debug builds can run the raw `project/` tree for development.

## Building

Part of the [NukeEngine-Eco](https://github.com/Luastris/NukeEngine-Eco) superbuild, or
standalone: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` +
`cmake --build build --config Debug` (needs `VCPKG_ROOT`; the engine must be built
first). Deploys next to the engine in `NukeEngine/x64/<Config>`.
