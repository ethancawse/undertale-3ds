# undertale-3ds

An unofficial fan port of UNDERTALE to the Nintendo 3DS.

You build it from your own copy of UNDERTALE.

## Status

UNDERTALE 1.0 beta: coming in early November. The newest build is being measured on every console now, and I'll play the hardest scenes on it myself. The beta goes out once I can't notice lag in them, after a final check.

DELTARUNE: in progress. It comes after UNDERTALE.

## Progress to 1.0

The goal: every scene running at a locked 30 fps on every model.

The bars count the scenes we measure on real consoles that pass. They come from the builds before the newest one (29 October 2026); the newest build is being measured now.

```
New 3DS / New 3DS XL / New 2DS XL   ███████████████████░   97%
3DS / 3DS XL / 2DS                  ████████████████░░░░   78%
Overall                             █████████████████░░░   87%
```

A scene passes only when every single frame in play is ready in time, with no dropped frames. Some scenes that fail miss by a single frame here and there, which is hard to notice while playing. That's why the beta is judged by playing the worst scenes.

New 3DS: 63 of 65 measured scenes pass. The other two (the Omega Flowey finale and one credits scene) dropped no frames, but their single slowest frame still needs measuring.

Older models: 54 of 69 measured scenes pass on a 3DS (45 of 71 on 28 October). Left:

- The Omega Flowey finale
- Asriel's fight, and two of the scenes where you save your friends during it
- Mettaton's opera
- Parts of the fights with Asgore and Mad Dummy
- Lemon Bread in the True Lab
- The ending credits
- The end of Undyne's bridge in Waterfall
- One Snowdin scene that dropped no frames but still needs its slowest frame measured
- A final check of every scene

Asriel, Mad Dummy and Lemon Bread were measured before the 29 October sound fix, and Asgore before the changes below.

We also measured every room of a full playthrough on the older models, on an earlier build: 589 of 595 rooms pass on a 3DS.

## What changed since the last update (29 October, evening)

- Older models: fixed a crash that could happen when sounds stopped, for example at a room change. A sound channel's buffers were cleared before the channel was reset, and the system's sound thread could read them in between.
- Older models: fixed a rare crash after quitting the game. When the loader gives the game no way back, the game now keeps its memory at exit instead of freeing it while its own threads may still be running.
- Older models: Asgore's fight is smoother. Engine fixes cut the late frames in our console test runs of the fight from 13 to 5, then from 7 to 3. It isn't at a locked 30 yet.
- Older models: the item names in Asriel's DREAM menu no longer stutter.
- Older models: 4 KB more room for engine code, which these fixes use.
- Also new: the Genocide ending's window shake now shows as picture motion; with the game on the bottom screen, frames are timed to that screen; the FPS counter counts the frames actually shown; and no boot log appears when the other screen is turned off.

## Setbacks

- An experiment to speed up Asgore's fight made Omega Flowey's fight slower, so it was taken back out.
- The beta is still not out. The hardest scenes on the older models, above all the ending credits, still drop frames.

## Known issues

- On older models, some heavy battles and the ending credits can slow down.
- On older models, some sounds can start late or briefly interrupt play; songs in Mettaton's opera can start about a fifth of a second late.
- On older models, opening the Rosalina menu while the game runs can freeze the console. Save before using it.
- Smart smoothing can lose fine lines in some large artwork, such as Mettaton NEO, Asriel's wings and Asgore's cape.
- Opening Options lets the music fade out instead of stopping at once, and a sound that was playing can be cut off or play twice afterwards.
- A few stray red pixels can appear around the last Froggit in the ending credits. We're looking into it.
- Rarely, an older model can crash when you press HOME, and a 2DS can freeze or crash around Asriel's fight or the ending credits. We're looking into it. Save first.
- If music and sounds go silent after you've started the game several times, restart the console.

## Supported consoles

- New 3DS, New 3DS XL, New 2DS XL
- 3DS, 3DS XL, 2DS

## What you need

- UNDERTALE 1.08 for PC, from Steam or GOG
- A Windows, Linux or Mac computer
- A 3DS that can run homebrew, and an SD card (freshly formatted, with plenty of free space, is best)

## How it works

A small builder app on your computer finds your copy of UNDERTALE and builds a .3dsx or .cia for your console. It can also bring your PC save over. Your copy of the game stays yours, so please don't share the files it makes.

## FAQ

**Is this piracy?**
No. Nothing from the game is here. The builder only works with a copy you own.

**What about DELTARUNE?**
Later, in its own repository, starting with the New 3DS models.

**Something looks or plays differently from the PC version.**
Please open an issue with your console model and where it happened.

## How it was made

Most of the code was written with AI models (mainly Claude) under my supervision and direction.

## Who am I?

Hi, I'm Ethan. I'm 21 and in my fourth year of Computer Science at Western University (the University of Western Ontario).

UNDERTALE and DELTARUNE mean a lot to me, and I wanted to play them on the 3DS. So I made this.

Yes, a lot of this port was built with AI assistance. That doesn't mean it was rushed. Every room, font and sprite was checked against the original, down to the sub-pixel, so it looks and feels right on the 3DS's small screens. It was made with a lot of love and care.

I hope you enjoy playing it as much as I enjoyed making it.

If you want to say hi or tell me what you think, please don't hesitate to reach out; my Discord is 3lit3hax.

## Credits

- UNDERTALE by Toby Fox.
- Engine: a fork of [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch), used under the GNU AGPL v3. Modified from Butterscotch, 2026.
- devkitPro, libctru and citro3d.
- Other libraries: see [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).

## Disclaimer

Unofficial fan project, not affiliated with or endorsed by Toby Fox, Fangamer, YoYo Games or Nintendo. UNDERTALE is a trademark of Toby Fox. Nintendo 3DS is a trademark of Nintendo.

## Licence

GNU Affero General Public License v3. See [LICENSE](LICENSE).
