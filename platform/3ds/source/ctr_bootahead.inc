// bootahead (first update, changes bootmask and bootahead2; included at ctr_audio_system.c's end, UNDERTALE only): the
// game thread names the song the menu will ask for (z_bootahead.c), and the opener opens it into the flowend spare
// (ctr_audio_system.c, "flowend") before the menu asks.
//
// The game thread only hands the name over: CtrAudioSystem_aheadHint copies it to the heap, swaps the copy into
// bootaheadAsk (an older copy it gets back, one the opener hasn't taken, is freed) and wakes the opener. It writes none
// of the spare's statics: they are the opener's. (R411 F1: the first version wrote them from the game thread after an
// idle check that reserved nothing, so the opener could build a guess at the same time, and the open of the hinted
// name then went over the guess's open decoder, leaking its handle. tools/analysis/test_bootahead_race.py keeps that
// interleaving.) The opener takes the name at its next wait (CtrAudioSystem_chainNoteBootahead, the first link of
// flowend's note chain, so in the same note as flowend's own code): a spare holding another name is closed, unless it
// already holds this one; the name is wanted with no guess due, and the note's quiet second is taken as passed, with
// the signature the note computes, so flowend's code opens it in that same wait if nothing changed in between. The play
// that asks for that path takes the spare as it always does; a wrong name wastes one open and goes as any unused spare
// goes.
//
// This is ctr_audio_system.c's code, because the spare's statics are that file's. It reaches them only through their
// addresses, loaded from words inside the function (flowhitchOwner's pattern), and is noipa: a new C reference to them,
// even from the file's end, changed how the compiler laid out functions far above (the prototype's
// CtrAudioSystem_chainNoteFlowhitch grew 12 B, moving every late function after it). Boot-only code: it and its
// constants sit in UNDERTALE's retired AAC slot (.aacout.text.bootahead, aacout.cmake), and bootaheadAsk (4 B) is
// late bss (.bss.bootahead_late).
//
// The menu's music, kept ahead (review decision 11-09; CtrAudioSystem_menuMusic). After a menu song opens (sounds 220-226,
// 220 + the save's level, as z_bootahead.c reads them), flowend's number guess wanted the next level's song, which a
// session never plays: coming back from Settings reopened the menu song cold (72-77 ms on Azahar Old). Instead, the
// first spare takes the menu song's own file again (kept 120 s as any unused spare: Settings and back take less), and
// flowend's second spare, idle in the menu, takes the Settings jingle (sound 423), which opened cold at the Settings
// press (60-67 ms). Both open at quiet seconds, the jingle first. The opener does it all, in this link, at its waits.
// When something other than a menu song opens while the jingle is still held (Continue, Reset), the jingle's spare
// closes, so no extra decoder outlives the menu. Settings' own song still opens when it plays: which one the game
// picks is its own rule.
#if defined(ENABLE_WAD16)
bool CtrAudioSystem_aheadHint(const char* path);
#if defined(CTRPORT_HAVE_TREMOR)
#define BOOTAHEAD_ADDR(type, name) ({ type* at_; __asm__ volatile("ldr %0, 1f\n\tb 2f\n1:\t.word " #name "\n2:" : "=r"(at_) : : "memory"); at_; })
static char* bootaheadAsk __attribute__((section(".bss.bootahead_late"))); // the game thread's name, not yet taken

// Game thread: the name goes to the opener (false: no opener to take it, or too long a path for the spare).
__attribute__((noinline, noipa, optimize("Os"), section(".aacout.text.bootahead"))) bool CtrAudioSystem_aheadHint(const char* path) {
    CtrAudioSystem* ca = *BOOTAHEAD_ADDR(CtrAudioSystem*, gShutdownTarget);
    if (ca == NULL || ca->opener == NULL || ca->openerQuit || strnlen(path, FLOWEND_AHEAD_PATH) >= FLOWEND_AHEAD_PATH) return false;
    char* ask = strdup(path);
    if (ask == NULL) return false;
    free(__atomic_exchange_n(&bootaheadAsk, ask, __ATOMIC_ACQ_REL));
    LightEvent_Signal(&ca->openEvent);
    return true;
}

#define BOOTAHEAD_MENU_FIRST 220u // the menu object's songs: 220 + the save's level (z_bootahead.c)
#define BOOTAHEAD_MENU_LAST 226u
#define BOOTAHEAD_JINGLE 423u // the Settings room's first music
void CtrAudioSystem_menuMusic(CtrAudioSystem* ca);
int CtrAudioSystem_menuSound(CtrAudioSystem* ca, const char* path, uint32_t first, uint32_t last, const char** name, size_t* len);

// The sound in first..last whose file name (before any extension) is path's last part (its name in *name, its length in
// *len); -1 if none.
__attribute__((noinline, noipa, optimize("Os"), section(".aacout.text.bootahead"))) int CtrAudioSystem_menuSound(CtrAudioSystem* ca, const char* path, uint32_t first, uint32_t last, const char** name, size_t* len) {
    const char* base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    size_t n = dot != NULL ? (size_t) (dot - base) : strlen(base);
    DataWin* dw = ca->base.audioGroups != NULL ? ca->base.audioGroups[0] : NULL;
    for (uint32_t i = first; dw != NULL && last >= i && dw->sond.count > i; i++) {
        const char* file = dw->sond.sounds[i].file;
        if (file == NULL) continue;
        const char* fb = strrchr(file, '/');
        fb = fb != NULL ? fb + 1 : file;
        const char* fd = strrchr(fb, '.');
        size_t fl = fd != NULL ? (size_t) (fd - fb) : strlen(fb);
        if (fl != n || memcmp(fb, base, n) != 0) continue;
        if (name != NULL) *name = fb, *len = fl;
        return (int) i;
    }
    return -1;
}

// Opener thread, at its wait, after any handed-over name: the menu's music kept ahead (the header above).
__attribute__((noinline, noipa, optimize("Os"), section(".aacout.text.bootahead"))) void CtrAudioSystem_menuMusic(CtrAudioSystem* ca) {
    const char* last = BOOTAHEAD_ADDR(const char, flowendLastPath);
    uint8_t* due = BOOTAHEAD_ADDR(uint8_t, flowendGuessDue);
    uint8_t* song = BOOTAHEAD_ADDR(uint8_t, flowendSongState); // the second spare: 0 none, 1 path wanted, 2 open
    char* songPath = BOOTAHEAD_ADDR(char, flowendSongPath);
    bool menu = CtrAudioSystem_menuSound(ca, last, BOOTAHEAD_MENU_FIRST, BOOTAHEAD_MENU_LAST, NULL, NULL) >= 0;
    if (menu && *due) {
        uint8_t* wanted = BOOTAHEAD_ADDR(uint8_t, flowendAheadState);
        char* held = BOOTAHEAD_ADDR(char, flowendAheadPath);
        *due = 0; // no number guess: the next level's song
        if (!(*wanted != 0 && strcmp(held, last) == 0)) {
            if (*wanted == 2) CtrDecoder_close(BOOTAHEAD_ADDR(CtrDecoder, flowendAhead));
            strcpy(held, last); // shorter than FLOWEND_AHEAD_PATH (the open wrap keeps no longer one)
            *wanted = 1;
        }
        const char* name;
        size_t len;
        // A free second spare only: one wanted or open is another guess's.
        if (*song == 0) {
            DataWin* dw = ca->base.audioGroups[0];
            const char* file = dw->sond.count > BOOTAHEAD_JINGLE ? dw->sond.sounds[BOOTAHEAD_JINGLE].file : NULL;
            if (file != NULL && CtrAudioSystem_menuSound(ca, file, BOOTAHEAD_JINGLE, BOOTAHEAD_JINGLE, &name, &len) >= 0) {
                const char* base = strrchr(last, '/');
                base = base != NULL ? base + 1 : last;
                const char* dot = strrchr(base, '.');
                int n = snprintf(songPath, FLOWEND_AHEAD_PATH, BOOTAHEAD_ADDR(const char, flowendSongName), (int) (base - last), last, (int) len,
                                 name, dot != NULL ? dot : "");
                *song = n > 0 && FLOWEND_AHEAD_PATH > n ? 1 : 0;
            }
        }
    } else if (!menu && *song != 0 && strcmp(songPath, last) != 0 &&
               CtrAudioSystem_menuSound(ca, songPath, BOOTAHEAD_JINGLE, BOOTAHEAD_JINGLE, NULL, NULL) >= 0) {
        if (*song == 2) CtrDecoder_close(BOOTAHEAD_ADDR(CtrDecoder, flowendSong)); // the menu was left another way
        *song = 0;
    }
}

// Opener thread, at each note (flowend's wrap calls it first): a name the game thread handed over becomes the spare's
// wanted path at the opener's next wait; then the menu's music. Then loadclass 4c's link, as before.
__attribute__((noinline, noipa, optimize("Os"), section(".aacout.text.bootahead"))) void CtrAudioSystem_chainNoteBootahead(const char* state) {
    CtrAudioSystem* ca = *BOOTAHEAD_ADDR(CtrAudioSystem*, gShutdownTarget);
    char* ask = NULL;
    if (__atomic_load_n(&bootaheadAsk, __ATOMIC_RELAXED) != NULL && ca != NULL && ca->opener != NULL && threadGetCurrent() == ca->opener &&
        strcmp(state, BOOTAHEAD_ADDR(const char, flowendWaiting)) == 0)
        ask = __atomic_exchange_n(&bootaheadAsk, NULL, __ATOMIC_ACQ_REL);
    if (ask != NULL) {
        CtrAudioSystem** owner = BOOTAHEAD_ADDR(CtrAudioSystem*, flowendAheadOwner);
        uint8_t* wanted = BOOTAHEAD_ADDR(uint8_t, flowendAheadState); // 0 none, 1 path wanted, 2 open
        char* held = BOOTAHEAD_ADDR(char, flowendAheadPath);
        if (*owner == NULL) *owner = ca; // flowhitchOwner's rule: a game that has opened no file yet
        // Another system's spares wait for the open wrap's own switch to drop them; this name goes with them.
        if (*owner == ca && !ca->openerQuit && !(*wanted == 2 && strcmp(held, ask) == 0)) {
            if (*wanted == 2) CtrDecoder_close(BOOTAHEAD_ADDR(CtrDecoder, flowendAhead));
            strcpy(held, ask); // shorter than FLOWEND_AHEAD_PATH (CtrAudioSystem_aheadHint)
            *BOOTAHEAD_ADDR(uint8_t, flowendGuessDue) = 0;
            uint32_t sig = CtrTextures_flowendLoads();
            for (int i = 0; STREAM_CONTEXTS > i; i++) sig = sig * 33u + (uint32_t) ca->streams[i].state + (uint32_t) (uintptr_t) ca->streams[i].path;
            *BOOTAHEAD_ADDR(uint32_t, flowendCtxSig) = sig;
            *BOOTAHEAD_ADDR(u64, flowendLastOpen) = svcGetSystemTick() - (u64) ((FLOWEND_AHEAD_IDLE_MS + 1.0) * CPU_TICKS_PER_MSEC);
            *wanted = 1;
        }
        free(ask);
    }
    if (ca != NULL && ca->opener != NULL && threadGetCurrent() == ca->opener && !ca->openerQuit &&
        *BOOTAHEAD_ADDR(CtrAudioSystem*, flowendAheadOwner) == ca && strcmp(state, BOOTAHEAD_ADDR(const char, flowendWaiting)) == 0)
        CtrAudioSystem_menuMusic(ca);
    CtrAudioSystem_chainNotePair(state);
}
#else
__attribute__((noinline, section(".aacout.text.bootahead"))) bool CtrAudioSystem_aheadHint(const char* path) {
    (void) path;
    return false;
}
#endif
#endif
