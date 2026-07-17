# L4D2 2D Velocity Speed Proxy

A small plugin (DLL) for Left 4 Dead 2, built for local/listen server use in TAS/TSA or even in RTA speedrunning. It reads the player's speed and feeds it into a material proxy value called `$speed`, matching `cl_showpos 1` by default, with settings a `.vmt` file can use to change how the number is read — so it works with other people's existing speedometer/velometer add-ons without editing their files. Not intended for multiplayer or dedicated servers.

## What it does

- Reads the local player's velocity directly from game memory.
- Feeds a live speed number into the game's material system through the `PlayerSpeed` proxy, written into `$speed` by default.
  - By default (`mode "3d"`), this is the raw, unsmoothed speed number, matching `cl_showpos 1` exactly.
  - With `mode "2d"` set in a material's `.vmt` file, the number becomes horizontal-only (2D) speed with smoothing applied, so it does not flicker on tiny jitters — meant for a clean bhop-style readout.
  - A material can also set `resultVar` (which shader variable to write into) and `scale` (multiply the number by this) in the same block, so this plugin works with other people's existing speedometer/velometer add-ons without editing their files.
- Switches to full 3D speed automatically when the player is on a ladder, or when boosting/launched at steep angles where the jump starts near-zero horizontal velocity — in `mode "2d"`.
- Works during demo playback. (Even for demos recorded on official servers without the plugin loaded.)

The plugin loads as a **server plugin**. Once loaded, it replaces the game's material proxy factory with its own. Whenever the game asks for a proxy called `PlayerSpeed`, the plugin hands back its own code instead, which calculates and updates the speed value every frame. Any other proxy name is passed straight through to the game's original factory, so nothing else breaks.

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the full breakdown with diagrams.

## Using `$speed` with other people's speedometer add-ons

Because a material can set its own `resultVar`, `scale`, and `mode` inside its `Proxies { PlayerSpeed { ... } } }` block, this plugin does not force every speedometer to use the same settings. Example:

```
Proxies {
    PlayerSpeed {
        resultVar "$speed"   // which shader var to write into (default: $speed)
        scale 1               // multiply the raw number by this (default: 1)
        mode "2d"// <------  
            //^ "3d" (Raw values just like what you see from VEL: in `cl_showpos 1`)
            //^ "2d" (horizontal-only, smoothed like what you see in hl2 TAS speedruns uses UPS instead of VEL)
    }
}
```

Most speedometer/velometer add-ons you'll find on the Workshop only use `$speed` with no `mode` set at all, so they load correctly out of the box, but they'll show raw 3D speed (matching `cl_showpos 1`), not a clean bhop-style number. If you want the smoothed, horizontal-only reading instead, you'll need to open that add-on's `.vmt` file yourself and add `mode "2d"` inside its `PlayerSpeed` block. Make sure this plugin is installed first (see [Download](#download) link below) before testing this.

A material can also declare `PlayerSpeed` more than once with a different `resultVar`/`mode` on each, to get both a raw number and a smoothed number at the same time on the same material.

## Load order matters

Materials only get hooked by this plugin the first time the game loads them. If a speedometer material was already loaded before this plugin started (for example, if you load the plugin while already in a map), that material keeps using whatever it was using before and will not pick up this plugin's values. To make sure every speedometer/HUD material gets hooked correctly, load this plugin (`plugin_load addons/l4d2_2dvel`, or through the `.vdf` manifest) **before** joining or loading a map — ideally right at the main menu with nothing connected.

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
| A speedometer material/HUD add-on | Must define a `$speed` (or whatever `resultVar` you choose) proxy variable on its material — this plugin only feeds the number, it does not draw anything on screen |

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

> **Note:** the `hl2sdk` `l4d2` branch's own `imaterialsystem.h` header labels itself as interface version `VMaterialSystem079`, but the actual running game exposes `VMaterialSystem080` — a different, incompatible layout. This plugin already works around that (see [`ARCHITECTURE.md`](ARCHITECTURE.md)), so you do not need to change anything, but keep this in mind if you add new calls into the material system interface yourself — test them carefully, since a wrong assumption here can cause a crash on the affected call.

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

If you would rather load it manually instead of using the `.vdf` manifest, you can skip step 2 and use the engine's `plugin_load addons/l4d2_2dvel` console command instead. If you load it manually, remember the load order note above — load it before joining a map.

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


## Known non-issue: exception message on `quit`

If you have a debugger attached (e.g. Visual Studio) and type `quit` in the console, you may see an exception message pointing at `tier0.dll`. This is a normal side effect of Windows unloading DLLs in an unpredictable order while the whole game process is shutting down, not something wrong with this plugin's code — the process still exits normally right after (exit code 0), and the message does not appear at all without a debugger attached. It is safe to ignore. Disconnecting from a map (instead of fully quitting) does not trigger this at all.


## Download
➡[Download v1.0 Release](https://github.com/sjrtp/l4d2_2dvel/releases/tag/v1.0)
