// bootahead (first update, change bootmask, review decision 11-07; UNDERTALE only): the menu's song and the menu's picture
// behind the loading bar.
//
// After the loading bar, every boot walks the story, the title and the menu. The menu shows how far the save got: a
// level from 0 to 7 picks its song and the sprites its Draw puts behind the words. The song's open took 60-70 ms on
// Azahar after the menu's first picture, and the level-6 picture's sprites loaded at draw time: a shown frame of
// 83-109 ms. At the story's room load (z_utstall.c's bootahead block: the first room load of a boot, hidden behind the
// full bar), this file works the level out as the menu's Create will and names that song to the music spare
// (ctr_bootahead.inc); z_utstall.c then loads the level's tiles from the plan's scene for it.
//
// The level, as the menu object's Create works it out: 0; 1 if file0 exists, and then one step for each of [Toriel]
// TS, [Papyrus] PD, [Undyne] UD, [Alphys] AD and [F7] F7 above 0, in that order, stopping at the first that isn't; 7
// if [EndF] EndF is above 0. Its song is sound 220 + level, and sound 220 at level 7. The game reads them through
// file_exists("file0") and ini_open("undertale.ini") with ini_read_real. This is only a performance hint, and it may
// be stale: it reads the files from the file system itself, as text, with a parse of its own (Ini_parse,
// Ini_getString, atof), and never goes through the game's ini_open or the INI the runner keeps open or cached. It only
// reads. When the files on the card differ from what the game will read (a missing or unreadable file, or an INI the
// game has open and not yet written back), the hint is wrong: a wrong answer wastes one open and some tiles, and the
// game still reads its files itself and plays what it asks for.
//
// Its own file, so ctr_audio_system.c and z_utstall.c gain no C code outside their own late blocks. Boot-only code:
// its code and constants sit in UNDERTALE's retired AAC slot (.aacout.text.bootahead and .aacout.rodata.bootahead,
// platform/3ds/aacout.cmake), at 0 B extra RAM; it has no statics.

#if defined(ENABLE_WAD16)
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_win.h"
#include "file_system.h"
#include "ini.h"
#include "log.h"
#include "runner.h"

#define BOOTAHEAD_MENU_SONG 220u // the menu object's song at level 0
#define BA_CODE __attribute__((section(".aacout.text.bootahead")))
#define BA_CONST __attribute__((section(".aacout.rodata.bootahead")))

bool CtrAudioSystem_aheadHint(const char* path); // ctr_bootahead.inc, at ctr_audio_system.c's end
int CtrBootahead_begin(Runner* runner);

static const char bootaheadSave[] BA_CONST = "file0";
static const char bootaheadIni[] BA_CONST = "undertale.ini";
// The menu's steps in order: section, key.
static const char bootaheadSteps[5][2][8] BA_CONST = {{"Toriel", "TS"}, {"Papyrus", "PD"}, {"Undyne", "UD"}, {"Alphys", "AD"}, {"F7", "F7"}};
static const char bootaheadEnd[] BA_CONST = "EndF"; // section and key
static const char bootaheadOgg[] BA_CONST = "%s.ogg";
static const char bootaheadAs[] BA_CONST = "%s";
static const char bootaheadLog[] BA_CONST = "Boot ahead: menu level %d, %s asked for: %d\n";

// ini_read_real(section, key, 0) > 0, as the files on the card say.
BA_CODE static bool bootaheadPositive(const IniFile* ini, const char* section, const char* key) {
    const char* value = ini != NULL ? Ini_getString(ini, section, key) : NULL;
    return value != NULL && atof(value) > 0.0;
}

BA_CODE static int bootaheadLevel(FileSystem* fs) {
    char* text = fs->vtable->readFileText(fs, bootaheadIni);
    IniFile* ini = text != NULL ? Ini_parse(text) : NULL;
    free(text);
    int level = 0;
    if (fs->vtable->fileExists(fs, bootaheadSave)) {
        level = 1;
        for (int i = 0; 5 > i && bootaheadPositive(ini, bootaheadSteps[i][0], bootaheadSteps[i][1]); i++) level = i + 2;
    }
    if (bootaheadPositive(ini, bootaheadEnd, bootaheadEnd)) level = 7;
    if (ini != NULL) Ini_free(ini);
    return level;
}

// The story's room load, before its own tiles: the level, and its song named to the spare. Returns the level.
BA_CODE int CtrBootahead_begin(Runner* runner) {
    FileSystem* fs = runner->fileSystem;
    int level = bootaheadLevel(fs);
    // The path the audio system's open will be given for that sound (ctr_audio_system.c's resolveExternalPath).
    uint32_t index = BOOTAHEAD_MENU_SONG + (uint32_t) (level == 7 ? 0 : level);
    DataWin* dw = runner->dataWin;
    const char* file = dw != NULL && dw->sond.count > index ? dw->sond.sounds[index].file : NULL;
    if (file == NULL || file[0] == 0) return level;
    char name[256];
    snprintf(name, sizeof(name), strchr(file, '.') != NULL ? bootaheadAs : bootaheadOgg, file);
    char* path = fs->vtable->resolvePath(fs, name);
    bool asked = path != NULL && CtrAudioSystem_aheadHint(path);
    logInfo(bootaheadLog, level, path != NULL ? path : name, (int) asked);
    free(path);
    return level;
}
#endif
