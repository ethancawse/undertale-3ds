# undertale-3ds

An unofficial fan port of UNDERTALE to the Nintendo 3DS.

You build it from your own copy of UNDERTALE.

## Status

UNDERTALE 1.0 beta: not out yet, and no download yet. Today (29 October) I'm playing the hardest scenes myself on an Old 3DS XL. The beta goes out once I can't notice lag in them, after a final check.

DELTARUNE: in progress. It comes after UNDERTALE.

## Progress to 1.0

The goal: every scene running at a locked 30 fps on every model.

The bars count the scenes we measure on real consoles that pass (28 October 2026; the newest build is still being measured):

```
New 3DS / New 3DS XL / New 2DS XL   ███████████████████░   97%
3DS / 3DS XL / 2DS                  █████████████░░░░░░░   63%
Overall                             ████████████████░░░░   80%
```

A scene passes only when every single frame in play is ready in time, with no dropped frames. Some scenes that fail miss by a single frame here and there, which is hard to notice while playing. That's why the beta is judged by playing the worst scenes.

New 3DS: 65 of 67 measured scenes pass. Left: a hitch in the Omega Flowey finale, one credits scene to re-measure, and a final check.

Older models: 45 of 71 measured scenes pass. Left:

- The Omega Flowey finale
- Asriel's fight and transformation
- Mettaton's opera and Mettaton EX
- Parts of the fights with Asgore, Undyne and Mad Dummy, and the True Lab
- The ending credits
- A few rooms in Snowdin, Waterfall and Hotland
- Six scenes that look fine but still need the stricter measurement
- A final check of every scene

## What changed since 28 October

- Sound fixes: memory used by a sound is now freed only after the 3DS's sound processor has finished with it.
- Older models: the builder now makes a compressed pack of the game's sound effects, on your computer, from your own copy of the game. If that step fails, the game uses the original sounds.
- SD cards: cleaning up our test consoles' SD cards showed that a cluttered card slows loading. Weeks of test files had left game files split into many pieces (one data file in 102). After a cleanup, files opened faster and the Homebrew Launcher loaded much faster. For the best loading times, use a freshly formatted SD card with plenty of free space.

## Still open

- Some scenes on the older models still drop frames: the Omega Flowey finale, the ending credits and a few room transitions.
- A rare crash on the 2DS after the ending credits. We're looking into it.

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
