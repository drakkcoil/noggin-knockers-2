# Noggin Knockers 2 portable core

These game rules, animation tables, scene data, and presentation command
builders come from [trapexit's 3DO port](https://github.com/trapexit/3do-bls-nogginknockers2)
at commit `8fc7f106d5b8f272291eb949d441e858c035b360`. They preserve the
original game's 100 Hz logic. The only host adaptation here is `types.h`,
which supplies fixed-width C types in place of the 3DO Portfolio SDK types.

The 3DO port code is licensed under GNU GPL version 2. See [COPYING](COPYING).
The original Bloodlust game material has separate provenance and licensing.

The frontend should call `nk_game_step()` once for each elapsed 10 ms of
simulation time. `nk_game_set_input()` supplies each player's current input
sample. Rendering can use `nk_compositor` and `nk_match_present` to build
draw commands without changing game rules. `NK_LOGICAL_WIDTH`,
`NK_LOGICAL_HEIGHT`, and `NK_LOGICAL_HZ` define the 320 × 200, 100 Hz
logical coordinate system.
