# undertale-3ds

An unofficial fan port of UNDERTALE to the Nintendo 3DS.

You build it from your own copy of UNDERTALE.

## Status

UNDERTALE 1.0 beta: not out yet, and no download yet. The newest build is being measured on every console now, and I'll play the hardest scenes on it myself. The beta goes out once I can't notice lag in them, after a final check.

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

Asriel, Mad Dummy and Lemon Bread were measured before the sound fix below.

We also measured every room of a full playthrough on the older models, on an earlier build: 589 of 595 rooms pass on a 3DS.

## What changed since the last update (29 October, morning)

- On the Old 3DS and Old 3DS XL, Asriel's fight, Mad Dummy and Lemon Bread run smoothly again after the sound-pack fix. The compressed sound-effect pack from the last update had made them stutter: after each small piece of sound was decoded, the game waited on a busy system service before the piece could play. It now uses a quicker direct call.
- Older models: Omega Flowey's finale starts about 60 sounds in a single frame, and most of them were cut off again in that same frame. That took more than half of the frame's time. Now only the sounds that are still playing at the end of the frame get started.
- Fixed a rare crash after the True Pacifist ending credits (seen on a 2DS).
- Mac: the builder is now a single app with Python included, so there is nothing else to install.

## Setbacks

- The sound-effect pack in the last update made several scenes on the older models worse before this fix. The emulator we test on answers that system service instantly, so the problem only showed up on real consoles.
- The beta is still not out. The hardest scenes on the older models, above all the ending credits, still drop frames.

## Known issues

- On the 3DS, 3DS XL and 2DS, some heavy scenes can slow down, including Omega Flowey's finale, the ending credits and some room changes.
- On older models, closing the Rosalina menu can rarely freeze the console. Save before using it.
- On older models, some scenes can stutter for a moment when a sound starts.
- Some large, thin artwork can lose fine lines with Smart smoothing, including Mettaton NEO, Asriel's wings and Asgore's cape.
- During the Genocide ending, the picture stays still where the PC game's window shakes.

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
