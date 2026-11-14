// Settings use stdio, whereas the game's bench sandbox implements FileSystem.
// Block persistent settings opens before either gamedir's RAM cache or SD sees
// them. Menu changes still apply to this run, including the prompt labels.
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#ifdef ENABLE_WAD16
#define BS_CODE __attribute__((noinline, section(".aacout.text.benchsettings")))
#define BS_RO __attribute__((section(".aacout.rodata.benchsettings")))
#else
#define BS_CODE __attribute__((noinline, section(".text.late.benchsettings")))
#define BS_RO __attribute__((section(".rodata.benchsettings_in_text")))
#endif

bool CtrBench_running(void);
bool CtrBench_pickerOpen(void);
bool CtrScreens_benchLaunched(void);
static const char bsSettings[] BS_RO = "settings.ini";

BS_CODE FILE* CtrBenchSettings_fopen(const char* path, const char* mode) {
    if (mode[0] != 'r' || strchr(mode, '+') != NULL) {
        const char* name = strrchr(path, '/');
        name = name != NULL ? name + 1 : path;
        if (strcasecmp(name, bsSettings) == 0 &&
            (CtrBench_running() || CtrBench_pickerOpen() || CtrScreens_benchLaunched())) {
            errno = EROFS; // skipped, including all append links after a failed rewrite
            return NULL;
        }
    }
    return fopen(path, mode);
}
