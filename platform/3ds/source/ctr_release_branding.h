#pragma once

// B10/D3: public names live after the frozen constants. The old literals are
// retained in their original translation units to preserve the shipping layout.
extern const char ctrReleaseAppName[];
extern const char ctrReleaseOptions[];
extern const char ctrReleaseControls[];
extern const char ctrReleaseBootFormat[];
extern const char ctrReleaseLogFormat[];
extern const char ctrReleaseStatsFormat[];
extern const char ctrReleaseWrongModelFormat[];
extern const char ctrReleaseLabHeading[];

// The out-of-memory screens (main.c, z_argcrash.c) tell an Old 3DS .3dsx player to install the 80 MB CIA when heap +
// linear is below this. UNDERTALE fits the standard 64 MB (decided 10-25), so it never gives that hint: 4 KB is below any
// real heap. (Not 1: GCC turns "1 > x" into "x == 0", one instruction shorter, which would move the late code after it.)
#ifdef ENABLE_WAD17
#define CTR_OOM_CIA_HINT_BELOW (72u * 1024u * 1024u)
#else
#define CTR_OOM_CIA_HINT_BELOW 4096u
#endif
