// flowpitch (flowend, 2026-10-28, approved): sound and video play at the game's frame clock.
//
// The 3DS screens refresh at 268,111,856 / 4,481,136 = 59.8312 Hz (the ARM11 clock over the ticks per frame), not 60,
// so a game locked to every second vblank runs 29.9156 frames a second, not 30. Music played at its own sample rate
// ran 0.28% ahead of the game's frames: a frame-timed cue came in late against it, by 114 ms at Photoshop Flowey's
// finale_3 (1214 frames: 40,580.8 ms at 29.9156 fps against 40,466.7 ms at 30; row flowend-o3ds-1 measured +108 ms
// from the rounded 29.92). The console's own [pace] windows agree: every frame shown for exactly 2 vblanks, the windows
// span 59.8315-59.8318 Hz (median) on the n3ds, o3ds, o3dsxl and o2ds (2.16 million vblanks, flowend's
// measurement). The DSP and the screens run from the same clock, so this ratio holds exactly.
//
// Every voice (music, sound effects, streams) plays at its sample rate times this, set once with the voice's rate
// (ctr_audio_system.c's flowpitchChnSetRate; ruling 10-28: the whole game runs 0.28% slow, so everything timed by
// it follows, and there is no music/sfx list to get wrong). A video without its own audio advances its wall clock at
// this rate too (ctr_video.c's flowpitchVideoUl2d), so it stays with the game's frames and with a soundtrack the
// game plays beside it. A DSP path that sets a channel's rate itself (DSP-ADPCM) multiplies by the same constant.
#ifndef CTR_MUSIC_CLOCK_H
#define CTR_MUSIC_CLOCK_H

#define CTR_MUSIC_RATE_SCALE 0.99718708f // (268111856.0 / 4481136.0) / 60.0

#endif
