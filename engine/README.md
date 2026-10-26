# Engine: a 3DS fork of Butterscotch

This folder is the game engine for the 3DS port. It is a fork of
[Butterscotch](https://github.com/ButterscotchRunner/Butterscotch), an open source re-implementation of the
GameMaker: Studio runner in C, which plays bytecode-compiled GameMaker games. Thanks to its authors: without
Butterscotch this port would not exist.

The fork started from upstream commit
[342371d44746](https://github.com/ButterscotchRunner/Butterscotch/commit/342371d447467cfa643b8987b57977bce8e8075c)
in October 2026.

## What it targets

- UNDERTALE (this repository) and DELTARUNE (later, separately) on the Nintendo 3DS.
- Every model: New 3DS, New 3DS XL, New 2DS XL, 3DS, 3DS XL and 2DS.
- A locked 30 fps in every scene, with game logic that behaves exactly as on PC.
- The game data always comes from the player's own copy. No game files are included here.

The engine is built only for the 3DS. Upstream's desktop, web and other console frontends are not maintained in this
fork.

## Main changes from upstream

- **3DS frontend** ([platform/3ds](../platform/3ds/)): a renderer on the 3DS GPU (citro3d), audio on the DSP with
  Ogg Vorbis decoded by Tremor, input, saves, both screens and an options menu.
- **Ahead-of-time compiler** ([tools/aot/gmlaot.py](../tools/aot/gmlaot.py)): on the player's PC, the game's hottest
  bytecode is translated to C and compiled into their build. The interpreter runs the rest.
- **Exactness checks**: a state-hash mode hashes the game state and every draw call each frame, so a change can be
  shown to leave the game identical (`src/cli/state_hash.c`).
- **GameMaker 1.4 behaviour** for UNDERTALE: collision tests and alarm rounding as the 1.4 runner does them
  (`src/collision_gm14.c`, `src/alarm_gm14.c`).
- **Memory for the Old 3DS**: texture pages converted to the GPU's tiled formats, compressed and cached on the SD
  card, loaded per tile; data.win chunks read lazily; the heap and linear memory split per model.
- **Work moved to the PC**: texture preparation ([tools/texprep](../tools/texprep/)) and per-room load plans
  ([tools/roomplan](../tools/roomplan/)) are made when the game is built. The room plans hold only indices, resolved
  against the player's data.win.
- **Display**: the 640x480 game drawn at the top screen's 320x240, with optional smoothing for text and sprites
  finer than a screen pixel.
- **Heavy scenes**: faster runner paths and native versions of some of UNDERTALE's heaviest effects (battle dust,
  fire attacks, Flowey), used only where the pixels and game state stay identical (`src/z_ut*.c`).

## Building

The engine isn't built on its own. The builder app ([tools/release/builder](../tools/release/builder/)) compiles it
with the 3DS frontend into a .3dsx or .cia from your copy of the game. See the [main README](../README.md).

## Status

UNDERTALE runs on the New 3DS models; the older models are being finished. DELTARUNE follows, New 3DS first.

## Licence

GNU Affero General Public License v3, like upstream Butterscotch: see [LICENSE](LICENSE). Third-party code is listed
in [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES).

Not affiliated with or endorsed by Toby Fox, Fangamer, Nintendo or YoYo Games.
