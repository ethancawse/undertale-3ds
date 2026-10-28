#include "ctr_release_branding.h"
#include "ctr_release_contract.h"

#define RELEASE_RODATA __attribute__((section(".rodata.late.relprep")))
#ifdef ENABLE_WAD17
#define PORT_NAME "deltarune-3ds (unofficial)"
// The 24-column name plus a full AOT build ID would wrap the 40-column console.
#define STATS_BUILD "%.8s"
#define BOOT_LINES "%.0s\nbuild %s\n%s\n"
#else
#define PORT_NAME "undertale-3ds"
#define STATS_BUILD "%s"
#define BOOT_LINES "%.0s (%s)\n%s\n"
#endif
// Release builds have no screenshot combo; the spaces keep the string's size (and so the late rodata layout) the same.
#ifdef CTRPORT_PLAYER_SHOTS
#define STATS_FOOTER "SELECT+START exit, SELECT+L shot"
#else
#define STATS_FOOTER "SELECT+START exit               "
#endif
_Static_assert(sizeof("SELECT+START exit, SELECT+L shot") == sizeof(STATS_FOOTER), "stats footer changed size");

const char ctrReleaseAppName[] RELEASE_RODATA = PORT_NAME;
const char ctrReleaseLabHeading[] RELEASE_RODATA = "\n=== 3DS diagnostics: ";
// puts supplies the newline for the two menu headings, just as GCC did for the
// previous literal-only printf calls. Keep the existing call sites the same size.
const char ctrReleaseOptions[] RELEASE_RODATA = "\x1b[0;0H\x1b[1;36m " PORT_NAME " options\x1b[0m\x1b[K";
const char ctrReleaseControls[] RELEASE_RODATA = "\x1b[0;0H\x1b[1;36m " PORT_NAME " > Controls\x1b[0m\x1b[K";
// %.0s still consumes the existing game-title argument, preserving every caller's
// argument list and code size while showing the complete port name just once.
const char ctrReleaseBootFormat[] RELEASE_RODATA = "\x1b[1;36m" PORT_NAME "\x1b[0m" BOOT_LINES;
const char ctrReleaseLogFormat[] RELEASE_RODATA = PORT_NAME "%.0s build %s\n";
const char ctrReleaseStatsFormat[] RELEASE_RODATA =
    "\x1b[0;0H\x1b[1;36m" PORT_NAME "\x1b[0m%.0s (" STATS_BUILD ")\x1b[K\n"
    "%s, SELECT for options\x1b[K\n"
    "frame %-7d %-25.25s\n"
    "fps %5.1f/%-3lu step %5.2f max %6.2f ms\n"
    "cpu draw %5.2f ms  gpu %5.2f ms     \n"
    "%4lu draws %5lu verts  heap %5.1f MB\n"
    STATS_FOOTER;
const char ctrReleaseWrongModelFormat[] RELEASE_RODATA =
    "This n3ds build needs a New 3DS\nor New 2DS XL.\n\n"
    "On this Old 3DS/2DS, use\n" CTR_RELEASE_O3DS_FILENAME "\n";
