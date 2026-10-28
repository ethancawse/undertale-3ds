# undertale-3ds

An unofficial fan port of UNDERTALE to the Nintendo 3DS.

You build it from your own copy of UNDERTALE.

## Status

UNDERTALE 1.0 beta: not out yet, and no download yet. It ships when every scene runs at a steady 30 fps on the older models too, inside the normal 64 MB .3dsx, and has had a final check.

DELTARUNE: in progress. It comes after UNDERTALE.

## Progress to 1.0

The goal: every scene running at a locked 30 fps on every model.

The bars count the scenes we measure on real consoles that pass (28 October 2026):

```
New 3DS / New 3DS XL / New 2DS XL   ███████████████████░   97%
3DS / 3DS XL / 2DS                  █████████████░░░░░░░   63%
Overall                             ████████████████░░░░   80%
```

The bars went down since the last update, mainly because the test got stricter. A scene used to pass when its slowest 1% of frames were fast enough. Now every single frame in play must be ready in time, with no dropped frames.

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

## What changed since 25 October

- Objects at the same depth now draw in the same order as on PC. This fixes Undyne's battle buttons looking dim and the order of Mettaton's smoke.
- New 3DS: the long pauses entering a battle in Snowdin and at the end of Undyne's bridge are gone.
- Older models: the Omega Flowey finale no longer keeps reloading its art from the SD card.
- Opening the Rosalina menu with the other screen off no longer freezes the game.
- The first-boot "SELECT: Options" hint no longer covers the game.
- Crisp text now covers Sans's font and the ending credits.

## Supported consoles

- New 3DS, New 3DS XL, New 2DS XL
- 3DS, 3DS XL, 2DS

## What you need

- UNDERTALE 1.08 for PC, from Steam or GOG
- A Windows, Linux or Mac computer
- A 3DS that can run homebrew, and an SD card

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
