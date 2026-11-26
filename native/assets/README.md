# Native game assets

Regenerate these files from the original 31 `.VOL` files in the project root:

```sh
python3 native/tools/extract_assets.py
```

This uses the GPLv2 `nog2vol` parser in `native/tools/nog2vol`. Its detailed,
lossless record extraction is in `extracted/`, including PNG previews and
16 kHz mono WAVs for the DOS PCM samples. `TITLESND.VOL` supplies title
speech. The preserved OPL music streams are in `NOGTITLE.VOL` and
`NOGGINBG.VOL`, with patches from `TIMBRES.VOL`.

The desktop renderer reads `image_index.tsv`. Each tab-separated line has
`kind`, `bank`, `index`, and a path relative to this directory. The first
three fields are the numeric `NkCompositorImageRef` members. Every referenced
`.rgba` file starts with two little-endian 32-bit integers, `width` and
`height`, then `width * height` tightly packed RGBA8888 pixels in row order.
Transparency is represented by the alpha byte. There is no row padding.

The renderer reads `palette_index.tsv` using the same four-column layout,
keyed by numeric `NkCompositorPaletteRef` fields. Each listed file contains
exactly 256 RGBA8888 entries (1024 bytes), with no header. VGA palette
components are expanded from 6 bits using `(value << 2) | (value >> 4)`;
alpha is 255. Scene palettes use kind 0, banks 0 title, 1 gameplay, and 2
selection, all with index 0. Cinema palettes use kind 1 and banks 0 through 9
for logo, credits, and endings 1 through 8. Their index is the active
background image index; the source palette is appended to that background's
image data. Index 65535 is the base cinema palette used before a background
appears. The 3DO-specific port-credit palette (kind 2) has no original DOS
asset and is absent.

`manifest.json` lists SHA-256 hashes of the 31 source `.VOL` files, plus all
extracted images and WAVs with their original roles, byte offsets, palette
sources, dimensions, transparency flags, and compositor mappings, including
palette references. Drawing
positions and orientation come from the
game's compositor commands. The generated game layer 1 composite stacks
`NOGGINBG` layers 1a and 1b into a 640 × 200 image for compositor reference
`kind=2, bank=1, index=4`.

`music/title-midi.wav` and `music/match-cmf.wav` were rendered from the
original OPL streams using the [3DO port's music tools](https://github.com/trapexit/3do-bls-nogginknockers2)
at commit `8fc7f106d5b8f272291eb949d441e858c035b360`. To regenerate them,
from a checkout of that repository run the following commands, replacing
`<game-directory>` with this project's absolute path:

```sh
python3 tools/music/extract_music.py <game-directory> build/music/source
python3 -c 'import tools.convert as c; print(c.build_music_renderer("clang", "clang++"))'
build/tools/render_music build/music/source/title-midi.bin build/music/source/timbres.vol <game-directory>/native/assets/music/title-midi.wav
build/tools/render_music build/music/source/match-cmf.bin build/music/source/timbres.vol <game-directory>/native/assets/music/match-cmf.wav
```

The music renderer uses the upstream `ymfm` code and writes 49,715 Hz WAVs;
SDL converts them to the output device rate at load time.

The `.VOL` files and their extracted artwork/audio remain copyrighted by
their original creators. This directory is generated from the user's game
copy; the `nog2vol` code has its separate GPLv2 notice.
