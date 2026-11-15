# Mod Loader for GTA: Liberty City Stories PC

This directory works just like the Mod Loader in [willzmkv/gta-vcs-recomp](https://github.com/willzmkv/gta-vcs-recomp) and traditional PC GTA mod loaders:

## How to use
1. Create a subfolder inside `modloader/` for your mod (e.g. `modloader/HD_Cars/` or `modloader/Realistic_Audio/`).
2. Place the modified files inside that folder, keeping the game's original folder paths:
   - `Textures_Shared/<texture>.png` (or `.dds`)
   - `Data/WEAPON.DAT`
   - `AUDIO/<sound>.mp3`
   - `Models/<model>.dff`
3. The game will automatically load files from `modloader/` with priority over base game files.

## Priority
When multiple mods contain the same file, priority is determined by folder alphabetical order or via a local `modloader.ini` configuration.
