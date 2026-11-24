# Building AO2DC-Recomp from your own dump (Windows 11)

This repository and its releases contain **no game code or data**. The game's PowerPC code is
recompiled into `ao2dc_AO4_Main.dll` on your own PC, from your own legally owned and dumped copy
of *Army of Two: The Devil's Cartel* (Xbox 360). `build_from_dump.bat` does the whole build.

## Prerequisites

Install these once (from an ordinary, non-admin Command Prompt; `winget` comes with Windows 11):

| Tool | Install |
|---|---|
| Visual Studio 2022 Build Tools, workload **Desktop development with C++** (includes the Windows SDK, the linker, CMake tools and Ninja) | `winget install Microsoft.VisualStudio.2022.BuildTools`, then in the Visual Studio Installer tick **Desktop development with C++** |
| LLVM / Clang (the compiler this project uses) | `winget install LLVM.LLVM` |
| CMake 3.25 or newer | `winget install Kitware.CMake` (or use the one Visual Studio installs) |
| Git | `winget install Git.Git` |

You also need:

- A 64-bit CPU with x86-64-v2 support (any CPU from about 2009 on) and a Direct3D 12 GPU.
- **About 16 GB of free RAM** for the code generation step, and about 15 GB of free disk space.
- About 30–90 minutes for the first build, depending on your PC.

## Getting the game files

1. Dump your disc with ABGX360, or unpack your own ISO with
   [extract-xiso](https://github.com/XboxDev/extract-xiso).
2. Copy the **whole extracted disc** into the `assets\` folder of this repository, so that you get:

   ```
   assets\default.xex         (some tools name it Default.xex - that's fine)
   assets\AO4.Main.xex
   assets\AO4.Splash.xex
   assets\Engine.BuildInfo.xex
   assets\Data\
   assets\Movies\
   assets\Content\ ...        (plus $SystemUpdate\, NxeArt and the other disc files)
   ```

The build only reads these files; nothing in `assets\` is changed. Keep a backup of your dump
anyway.

Supported release: Title ID `454109AB`, Media ID `60F8EF51`, version `0.0.0.11`, no title update
in the dump (Title Update 1 is added separately in `title_update\`; it is required, see below).

| File | SHA-256 |
|---|---|
| `default.xex` | `76de620af99c6a1f4c15972b726074e9c6219950842a50aecd45971dcf98bf38` |
| `AO4.Main.xex` | `bedc9c918716454fdf6573af154073052272597c8b43329ed24c62b62cc2c137` |
| `Engine.BuildInfo.xex` | `575af0d4d5ceb1e51c1a0fc5c918d35467d02b06fc11dc6fa765c7c5b5cb0e52` |

### Title Update 1 and DLC

Title Update 1 is **required**. DLC is optional. Copy them from your own console's storage (for example with a USB drive or FTP on a modded
console). They are not part of any release and are never committed.

- **Title Update 1 (required):** put `TU_12K22DB_000001C000000.000000000008B` (from
  `Content\0000000000000000\454109AB\000B0000\`) in `title_update\`. Only this update is
  supported (version 0.0.1.11, SHA-256
  `12a417208649711a53687ea33be0049409b965cae7cd82d9406ed2bd536f5675`). The build recompiles the
  updated code from it, so it must be in place **before building**. `build_from_dump.bat` and CMake
  stop with an error when `title_update\` is empty or holds another file. If you built an earlier
  release without the update, add it and rebuild (clean `out\build` and the
  `generated\default`, `generated\ao4_main` and `generated\engine_buildinfo` folders first; keep
  `generated\rexglue.cmake`).
- **DLC:** put the packages from `Content\0000000000000000\454109AB\00000002\` in `dlc\`:
  `8BBB0270061C248F30443C9885CB502300E2C03D45` (Hitmakers Pack) and
  `9DDFDADC8879D83BEE746E3C54945866E095576745` (Online Pass and Overkill Edition content). They
  are installed into `user\` when the game starts; no rebuild is needed. Avatar items
  (`00009000`) are not supported, and online features (including the online pass) do not work.
- **HD textures:** nothing to add. The disc dump already holds the HD pack
  (`assets\Content\0000000000000000\454109AB\00000002\454109AB00000000`, 1.4 GB). The game
  installs it into `user\` on the first start (about 1.4 GB of free space is needed there) and
  reuses that install afterwards; an incomplete install is repaired on the next start. If the
  package is missing from your dump the game runs without the HD textures and the log says so
  (look for "HD content:"). Do not also copy it into `dlc\`.

```
title_update\TU_12K22DB_000001C000000.000000000008B
dlc\8BBB0270061C248F30443C9885CB502300E2C03D45
dlc\9DDFDADC8879D83BEE746E3C54945866E095576745
```

## Building

1. Get the source **with the ReXGlue SDK**, either way:
   - download `Ao2TDC-win64.zip` from the GitHub Releases page and extract it. It
     already contains the SDK at the pinned commit. Extract it to a short path outside any other
     git repository, for example `C:\AO2DC-Recomp` (the full folder path must be at most 96
     characters, see Troubleshooting); or
   - clone with submodules:

     ```
     git clone --recursive https://github.com/BrewSparrow/AO2DC-Recomp.git
     ```

   GitHub's own "Source code (zip)" download does **not** include the SDK. Use the build kit or a
   clone instead.
2. Put your dump in `assets\` (above).
3. Double-click `build_from_dump.bat`, or run it from a Command Prompt in the repository folder.

The script:

1. checks that the required files are in `assets\`;
2. checks the SHA-256 of the three XEX files. On a mismatch it explains why and stops, without
   touching your files. It also checks that the title update in `title_update\` is present and is
   Title Update 1 (it stops otherwise), and counts the packages in `dlc\`;
3. finds Visual Studio (via `vswhere`), LLVM/Clang, CMake, Ninja and Git;
4. fetches the SDK submodule if it is missing;
5. configures the project (`cmake --preset win-amd64-release -DREXGLUE_ENABLE_FIDELITYFX=ON`, which also
   downloads the FidelityFX SDK for the optional FSR upscaler), which also applies the local SDK
   fixes in `thirdparty\patches\`, then runs the recompiler (`ao2dc_codegen`). This writes the
   generated C++ into `generated\`, which is never committed;
6. configures again (the module targets exist only after code generation) and compiles;
7. checks that `out\build\win-amd64-release\` holds `ArmyOfTwoTDC X360 Recomp.exe`,
   `ao2dc_AO4_Main.dll`, `ao2dc_Engine_BuildInfo.dll`, `rexruntime.dll` and `rexgpu-xenos.dll`.

Each stage prints a clear error if it fails. Running the script again later only rebuilds what
changed.

To run the same steps by hand, use an "x64 Native Tools Command Prompt for VS 2022" with
`C:\Program Files\LLVM\bin` first on `PATH`:

```
cmake --preset win-amd64-release -DREXGLUE_ENABLE_FIDELITYFX=ON
cmake --build out\build\win-amd64-release --target ao2dc_codegen
cmake --preset win-amd64-release -DREXGLUE_ENABLE_FIDELITYFX=ON
cmake --build out\build\win-amd64-release
```

Leave out `-DREXGLUE_ENABLE_FIDELITYFX=ON` (or use `=OFF`, or `set AO2DC_FSR=0` before the script) to
build without FSR support.

## Running

Run `out\build\win-amd64-release\ArmyOfTwoTDC X360 Recomp.exe`. It finds the project folder
(the one holding `ao2dc_manifest.toml`) and reads the game from `assets\`. Saves go to `user\`, the
shader cache to `cache\` and settings to `settings\hardware.toml`, all inside the project. Press
`F4` in game for settings. With Title Update 1 built in, the game mounts the package from
`title_update\` as `UPDATE:`; DLC from `dlc\` shows up in game after the first start.

The log is written to `logs\ui2.log` relative to the folder you start the game from, so start it
from the build folder:

```
cd /d out\build\win-amd64-release
"ArmyOfTwoTDC X360 Recomp.exe"
```

## Troubleshooting

- **Hash mismatch:** a different region or version, an installed title update, or a damaged
  dump. Re-dump or re-extract the disc. The title update goes in `title_update\`, never into
  `assets\`.
- **Title Update 1 is missing:** the build needs it. Copy
  `TU_12K22DB_000001C000000.000000000008B` into `title_update\` (see "Title Update 1 and DLC").
- **Unsupported title update:** only Title Update 1 is supported. Replace the file in
  `title_update\` with that exact package.
- **The game crashes in Chapter 2 or Chapter 35 ("Call to invalid or unregistered function"):**
  check the log for `Title update mounted` and `Module 'ao4.main.xex' registered 117503 functions`.
  A build without the update registers 117095 functions and has no crash guards.
- **"the project folder path is too long", or a FidelityFX shader step fails with exit code
  `0xc0000409`:** the FidelityFX shader compiler (built for FSR) crashes when the project folder
  path is long. Measured: it builds with a path up to 103 characters and crashes at 104;
  `build_from_dump.bat` stops at more than 96 characters. Move the whole folder to a short path such
  as `C:\AO2DC-Recomp` and run the script again. To build without FSR, run `set AO2DC_FSR=0` first
  (the path limit then does not apply).
- **Out of memory during code generation:** close other programs; about 16 GB of free RAM is
  needed.
- **"clang not found" or "Visual Studio not found":** install the prerequisites above, then open a
  new Command Prompt.
- **Failed to apply SDK patch:** the SDK submodule is not at the pinned commit. Run
  `git submodule update --init --recursive`.
