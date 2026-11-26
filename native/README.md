# Noggin Knockers 2 native desktop port

This is a native SDL2 rewrite for macOS and Windows. The original DOS
executable is not used at runtime. The port keeps the source-matched 100 Hz
game rules, eight characters, abilities, one-player tournament, two-player
matches, options, title/select screens, and endings. It renders the original
`.VOL` artwork and plays audio extracted from the supplied game files.

## Releases

Ready-to-run archives are in `dist/`:

- `NogginKnockers2-macOS-arm64.zip` contains `Noggin Knockers 2.app`.
- `NogginKnockers2-Windows-x64.zip` contains `NogginKnockers2.exe`, `SDL2.dll`,
  assets, notices, and corresponding source.

The macOS app is ad hoc signed and is not Apple notarized. Its ZIP is the
deliverable to move to another Mac; keep the app bundle intact after
extraction. The Windows build was cross-compiled on macOS and should be
exercised on a Windows 10 or 11 machine before a wider release.

Keyboard and gamepad controls are in `packaging/README.txt`. The game also
prints them with `--help`.

The Options screen has an **HD Upscale** toggle. It starts in Classic mode
(off), which displays the original artwork. Turn it on to apply edge-aware
Scale4x upscaling to the game image. The setting is saved with the other
options. HD mode is most visible in a larger window or fullscreen.

## Build from source

On macOS, install CMake and SDL2, then from the project root:

```sh
cmake -S native -B /tmp/nk2-macos -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/nk2-macos -j 4
/tmp/nk2-macos/noggin2 --assets native/assets
```

For a Windows x64 cross-build with MinGW, obtain the official SDL2 MinGW
development package and set `SDL2_MINGW_ROOT` to its
`x86_64-w64-mingw32` directory:

```sh
cmake -S native -B /tmp/nk2-windows \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL2_MINGW_ROOT=/path/to/SDL2/x86_64-w64-mingw32
cmake --build /tmp/nk2-windows -j 4
```

To regenerate artwork and PCM samples from the original `.VOL` files:

```sh
python3 native/tools/extract_assets.py
```

The music rendering steps are documented in `assets/README.md`.

To make distributable bundles:

```sh
python3 native/package.py macos --binary /tmp/nk2-macos/noggin2
python3 native/package.py windows --binary /tmp/nk2-windows/noggin2.exe \
  --sdl2-runtime /path/to/SDL2/x86_64-w64-mingw32/bin/SDL2.dll
```

## Verification and provenance

The macOS build has a headless smoke run that checks intro skip audio,
responsive title and character selection, and two-player Enter input. It also
composes title, options, selection, and match frames, advances an active match,
then cancels back to the title:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  /tmp/nk2-macos/noggin2 --assets native/assets --smoke \
  --smoke-output /tmp/nk2-smoke
```

`core/` adapts portable modules from the
[GPL-2.0 3DO port](https://github.com/trapexit/3do-bls-nogginknockers2).
The original game source and data were released by Bloodlust Software under
[MIT terms](https://github.com/trapexit/bloodlust-software-src).
See `REA_FINDINGS.md` for the supplied binary's identity and the scope of
REA analysis.
