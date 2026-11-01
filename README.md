# Launcher for Shield/Project-BO4 Client

The launcher runs on Windows and on Linux from the same page, and manages mods from the launcher
instead of the in-game manager.

One page for both systems: `source/launcher/ui/` (HTML, CSS, JS) is what the launcher shows, and the
C++ behind it answers the same JSON messages everywhere. Windows hosts that page in WebView2 (the
upstream design, no extra runtime to ship), Linux hosts it in WebKitGTK.

## Install

Download the archive for your system, then extract it in the `Call of Duty Black Ops 4` directory
(wherever the game is installed).

- Windows: `Project_BO4_Launcher_Windows.zip`, run `Shield_Launcher.exe`.
  Needs the Microsoft Edge WebView2 Runtime, which Windows 10 and 11 already have.
- Linux: `Project_BO4_Launcher_Linux.tar.gz`, run `./Shield_Launcher`.
  Needs `webkit2gtk-4.1` and `gtk3` (Arch: `webkit2gtk-4.1`; Debian/Ubuntu: `libwebkit2gtk-4.1-0`).
  The game itself runs through Proton or Wine, see below.

Play Client starts the modded game, Play Vanilla starts the game without the client, Dedicated
starts a server that reads its setup from `project-bo4.json`.

## Mods

The Mods button lists what sits in `project-bo4/mods` and the curated catalog, and installs,
enables, disables or deletes from there.

A mod is a **folder** inside `project-bo4/mods` that carries a `metadata.json` and the compiled
scripts (`compiled.gscc`, `*.gsic`, `*.csic`, `*.luac`). Loose files dropped in `mods/` are never
loaded by the game; the launcher lists them as ignored instead of pretending they work. Disabled
mods go to `project-bo4/mods_disabled`.

Downloads go through `curl` (present on Windows 10+ and on every desktop Linux), so no TLS backend
is compiled into the launcher.

## Linux: Proton and prefixes

The Linux settings (the penguin button) hold the runtime and the Wine prefix the game runs in:

- Backend `umu-launcher` starts a Proton build through `umu-run`, `Wine` starts the binary directly.
- The runtime list is scanned from Steam (`steamapps/common`, `compatibilitytools.d`), Heroic and
  umu. A prefix that already holds Battle.net is picked automatically.
- The prefix folder is used as it is when it holds a prefix, and gets created (`wineboot -u`) when
  the folder is empty.

`WINEDLLOVERRIDES=XInput9_1_0.dll=n,b` is set on every launch, so the client dll next to
`BlackOps4.exe` wins over Wine's builtin. The game language picker is hidden on Linux: that setting
lives in Battle.net's registry, inside a prefix the launcher does not write to.

## Build

Windows (MSVC, WebView2, no Qt):

```
tools\premake5.exe vs2022
msbuild build\Shield_launcher.sln /m /p:Configuration=Release /p:Platform=x64
```

Linux (CMake, GTK3 + WebKitGTK, no Qt):

```
./build-linux.sh release
```

Both builds put the launcher at the game root; the release workflow builds and packages the two
archives for every push.

## Layout

```
<game>/Shield_Launcher               launcher binary
<game>/project-bo4/launcher/         images, LPC, Sounds, solo.zip, mp.zip, exe.zip, configs
<game>/project-bo4/mods/             one folder per installed mod
<game>/project-bo4/mods_disabled/    mods turned off, the game does not read them
<game>/project-bo4.json              client settings, name, dedicated server section
<game>/launcher-config.json          launcher settings: volume, close on play, Wine runtime
```

The launcher works from the game root and from `project-bo4/launcher`; `BlackOps4.exe` is what tells
it where the game is.

For help and troubleshooting, join the [discord server](https://discord.gg/AXECAzJJGU).
[Shield Documentation](https://shield-client.gitbook.io/shield-documentation).
