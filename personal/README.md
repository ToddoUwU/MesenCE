# PersonalUpdates notes (ToddoUwU/MesenCE)

Fork of [nesdev-org/MesenCE](https://github.com/nesdev-org/MesenCE) (the community continuation of SourMesen/Mesen2,
which is archived at 2.1.1). `upstream` is read-only (push URL disabled); local work lives on `PersonalUpdates`,
branched from the `2.2.1` release tag on 2026-11-09.

## Why a source build

The official Linux release (built on Ubuntu 22.04 with `STATICLINK=true`) carries its own GCC 12 libstdc++ inside
`MesenCore.so`. On this machine (Ubuntu 24.04, GCC 14 libstdc++, which .NET loads first) the two copies disagree about
locale facets and Mesen aborts while loading the core: `terminate called after throwing an instance of 'std::bad_cast'`
(from a static `std::regex` in `Base6502Assembler.cpp`). Building here with `STATICLINK=false` fixes it.

## Patches on PersonalUpdates

- `Linux/LinuxGameController.cpp`: only real controllers get pad numbers: udev's verdict (`ID_INPUT_JOYSTICK` in
  `/run/udev/data`, what SDL and Steam use; it includes a blacklist of fake joysticks) plus at least one joystick button;
  without udev, a short version of udev's rule. Before, any device with an `ABS_X` axis counted, so Sunshine's virtual
  absolute mice, pen tablet and touchscreen and the motherboard's ASRock LED controller (Select/Start buttons, 8 axes)
  took pad slots ahead of the controller in hand.
- `Linux/LinuxKeyManager.cpp`: pads are numbered in connection order (kernel `inputN` number) instead of directory
  order; "PadN Back" resolves to Select, so Mesen's default Xbox layout binds NES/SNES Select on Linux; button names win
  over the analog-axis names that reuse them, so the default Turbo A/B (Y/X) bindings work.
- `UI/Windows/MainWindow.axaml.cs`: Ctrl+C and SIGTERM close Mesen like its close button, so battery saves and
  settings are written (SIGTERM used to end the process at once; Ctrl+C raced the shutdown). A second signal still ends it.
- `UI/global.json`: `rollForward: latestMajor`, so the installed .NET 10 SDK builds the net8.0 project.
- `UI/Directory.Build.props`: NativeAOT builds pin the .NET 8.0.33 runtime + ILCompiler (Brotli CVE fix; the .NET 10
  SDK otherwise picks 8.0.32, which NuGet's audit rejects).

## Scripts

- `personal/mesen-update` (linked as `~/.local/bin/mesen-update`): `sync` (merge the newest upstream release tag,
  abort on conflict, push), `build` (NativeAOT + clang thin LTO, memory-capped), `test` (headless `--testRunner`, 120
  frames), `install` (copy the binary into each Quiver game folder listed in `APPS`). No argument = build, test, install.
- `personal/couch-input.py SETTINGS_JSON [--window X,Y,W,H]`: one-player couch setup for a portable Mesen folder (P1
  on Pad1 or Pad2 plus keyboard; View+Menu = Exit; input kept when unfocused; optional window placement). Run with
  Mesen closed. Kirby uses `--window 3200,104,1280,1232` (centred on the streamed ASUS, DP-1, at X11 x=2560).

Quiver game: `~/.local/share/QuiverLauncher/Apps/KirbysAdventure-MesenCE` (portable Mesen: `settings.json`, `Saves/`,
`SaveStates/` live there; `play-kirby` is the selected executable; the ROM stays on MediaStorageTwo).
