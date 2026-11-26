# Reverse engineering and source provenance

The supplied `NOGGIN2.EXE` is a DOS/4GW 32-bit Linear Executable inside an
MS-DOS MZ wrapper. Its SHA-256 is
`23d1c01d68d0df2193f8a13514764baa421a1def251bbfc31c8ca9b5b5ca6db6`.
REA's `open_binary` rejected this target as `target_unavailable` with the
message `unsupported LE executable in MZ image`. REA did not decompile or run
the game, and no gameplay claim in this project is presented as REA-observed
execution. Converting the wrapper to a 16-bit DOS target would not preserve
the protected-mode program.

A [preserved Bloodlust Software source release](https://github.com/trapexit/bloodlust-software-src)
contains a byte-identical copy of the supplied game executable at
`BLS-archive-199X/source/bldlust/nog/noggin.exe`. Its Git blob ID is
`64b7d8d49e7865aa9b2e0699fd1790405bc69cdb`. The supplied `NOGSETUP.EXE`
similarly matches the archive's `SETUP.EXE` (Git blob
`a0a13c9dfc4674dedaea093f556a7aa177e84c76`). The archive includes the
Watcom-era C/C++ source, linker map, data files, and an MIT license from
Bloodlust Software. This source is stronger evidence for game rules than
attempting to analyze the DOS stub.

The [source-matched 3DO port](https://github.com/trapexit/3do-bls-nogginknockers2)
already separated the game's 100 Hz rules, animation data, screen compositor,
and `.VOL` parser from DOS hardware. This native desktop rewrite reuses its
portable GPL-2.0 modules and extracted data, then supplies SDL2 display,
input, and audio adapters. The new port should be compared against the
original DOS release in a running emulator for frame-by-frame or timing claims;
static source correspondence alone does not establish perfect runtime parity.
