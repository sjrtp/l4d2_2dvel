# L4D2 2D Velocity Speed Proxy

A small plugin (DLL) for Left 4 Dead 2, built for local/listen server use in TAS/TSA or even in RTA speedrunning. It shows the player's 2D speed (ground speed, ignoring vertical movement) through a material proxy called PlayerSpeed, giving a clean bhop-style speed number instead of raw 3D velocity. Not intended for multiplayer or dedicated servers.

## What it does

- Reads the local player's velocity directly from game memory.
- Shows only horizontal (2D) speed, which is what bhop players care about.
- Switches to full 3D speed automatically when the player is on a ladder, or when boosting/launched at steep angles (e.g. -78 degree throwable/launcher boosts) where the jump starts near-zero horizontal velocity.
- Smooths out small jitters so the number on screen does not flicker.
- Plugs into the game's material system so any material using `$speed` can display this value.
- Works during demo playback. (Even for demos recorded on official servers without the plugin loaded.)

The plugin loads as a **server plugin**. Once loaded, it replaces the game's material proxy factory with its own. Whenever the game asks for a proxy named `PlayerSpeed`, the plugin hands back its own code instead, which calculates and updates the speed value every frame.

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the full breakdown with diagrams.

## Project structure

```
l4d2_2dvel/
├── l4d2_2dvel.slnx            # Visual Studio solution file
├── hl2sdk/                    # Left 4 Dead 2 SDK goes here (see Installation)
├── dllmain.cpp                # Main plugin code
├── framework.h                # Windows header setup (used by the precompiled header)
├── pch.h / pch.cpp            # Precompiled header (auto-created by Visual Studio)
├── l4d2_2dvel.vcxproj         # Visual Studio project file
├── l4d2_2dvel.vcxproj.filters
├── ARCHITECTURE.md
├── README.md
└── .gitignore
```

> The `hl2sdk/` folder is kept empty in this repository on purpose. It is a large third-party SDK maintained by AlliedModders, not part of this project, so it is not bundled here. Cloning it yourself (see Installation below) also means you always get the latest version of the `l4d2` branch, instead of a copy that goes stale over time.
>
> A `Debug/`, `Release/`, or `Win32/` folder will appear once you build the project. That is build output, it is ignored by `.gitignore`, and Visual Studio regenerates it automatically.

## Requirements

| Requirement | Notes |
|---|---|
| Windows 10 or 11 | The plugin only builds and runs on Windows |
| Visual Studio 2026 (or 2022) | With the "Desktop development with C++" workload installed |
| C++20 support | Already set in the project, comes with modern Visual Studio |
| Git | To download the SDK |
| Left 4 Dead 2 SDK (hl2sdk) | Provides the game headers and libraries, see below |
| Left 4 Dead 2 (the game) | To actually test and run the plugin |
| A speedometer material/HUD add-on | Must define a `$speed` proxy variable on its material — this plugin only feeds the number, it does not draw anything on screen |

## Installation

### 1. Clone this repository

```bash
git clone https://github.com/sjrtp/l4d2_2dvel.git
cd l4d2_2dvel
```

### 2. Download the Left 4 Dead 2 SDK (hl2sdk)

Clone it straight into the `hl2sdk` folder, right next to `dllmain.cpp`:

```bash
git clone --branch l4d2 --single-branch https://github.com/alliedmodders/hl2sdk.git hl2sdk
```

This downloads the `l4d2` branch of AlliedModders' `hl2sdk` repository, the branch made specifically for Left 4 Dead 2.

### 3. Match the folder path

Open `l4d2_2dvel.vcxproj` in a text editor (or in Visual Studio, under Project Properties → C/C++ → General → Additional Include Directories) and make sure the include/library paths point at wherever you placed the `hl2sdk` folder. By default, the project expects the SDK next to the project files:

```
l4d2_2dvel/
└── hl2sdk/
    ├── public/
    ├── common/
    ├── game/
    ├── mathlib/
    └── lib/
```

If the paths in the `.vcxproj` are still hardcoded to a personal `C:\Users\...` folder, replace them with a relative path like `$(SolutionDir)hl2sdk\public` so the project builds correctly on any machine.

### 4. If you want to modify the plugin, build it yourself

1. Open `l4d2_2dvel.slnx` in Visual Studio.
2. After modifying, pick a build type: **Debug** or **Release**, and platform **Win32** (some Visual Studio versions label this dropdown **x86** instead, same thing). Left 4 Dead 2 is a 32-bit game, so the DLL must be built for Win32/x86; an x64 build will not load into the game.
3. Build the project (`Ctrl+Shift+B`).
4. The finished DLL will appear in the output folder (e.g. `Win32\Release\` or `Release\`, depending on your Visual Studio version).

### 5. Install into the game

1. Copy the built DLL into your Left 4 Dead 2 `addons` folder, for example:
   ```
   left4dead2\addons\l4d2_2dvel.dll
   ```
2. Create a plugin manifest file named `l4d2_2dvel.vdf` in the same `addons` folder, with this content:
   ```
   "Plugin"
   {
       "file" "addons/l4d2_2dvel"
   }
   ```
   This tells the engine to automatically load the DLL on startup. Note there is no `.dll` extension in the `"file"` value, the engine appends it automatically.
3. Start (or restart) the server. The plugin loads on its own, no extra command needed.

If you would rather load it manually instead of using the `.vdf` manifest, you can skip step 2 and use the engine's `plugin_load addons/l4d2_2dvel` console command instead.

### 6. Enable the `-insecure` launch option

Left 4 Dead 2 blocks unsigned server plugins like this one while VAC (Valve Anti-Cheat) is active, so you need to disable VAC for the session:

1. Open Steam.
2. Go to **Library**.
3. Right-click **Left 4 Dead 2** → **Properties**.
4. In the **Launch Options** field, add:
   ```
   -insecure
   ```
5. Close the properties window and launch the game.

Without this, the game will refuse to load the plugin.

# **WARNING**:
Do not UNLOAD THEN LOAD/RELOAD the plugin while the game is running, as this will cause a crash. (Planning to fix that soon)

## Dependencies at a glance

- `tier0.lib`, `tier1.lib`, `mathlib.lib`, `vstdlib.lib` come from the `hl2sdk`
- `d3d9.lib` comes with Windows / DirectX SDK tools already bundled in Visual Studio
- All required headers (`eiface.h`, `cdll_int.h`, `imaterialsystem.h`, etc.) come from `hl2sdk`

Nothing else is required beyond Visual Studio and the SDK.



➡[Download v1.0 Release](https://github.com/sjrtp/l4d2_2dvel/releases/tag/v1.0)
