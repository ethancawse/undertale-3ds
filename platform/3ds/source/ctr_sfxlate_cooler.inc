// Old UNDERTALE room-258 fan preparation. Only these appended sections enter the binary;
// the original hplcopy worker/feed/cancellation remain byte-identical. 
#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS) && defined(CTRPORT_HAVE_TREMOR)
#define SFC_CODE __attribute__((noinline, noipa, used, optimize("Os"), section(".text_sfxlate")))
#define SFC_RO __attribute__((section(".rodata_sfxlate")))
#define SFC_PCM 1121024u
#define SFC_WORK 262144u
#define SFC_LINEAR_FLOOR 3145728u
// Assembler aliases avoid adding IPA edges to original static functions. 
void sfcOldSkip(Renderer*) __asm__("__wrap_CtrRenderer_skipPresent");
void sfcWork(void*) __asm__("hplWork");
bool sfcReap(HplState*) __asm__("hplReap");
void sfcRelease(HplState*, int) __asm__("hplRelease");
char* sfcResolve(CtrAudioSystem*, const char*) __asm__("resolveExternalPath.isra.0");
u32 sfcLinearFree(void) __asm__("__wrap_linearSpaceFree");
static const char sfcName[] SFC_RO = "mus_deeploop2";
static const char sfcRoom[] SFC_RO = "room_truelab_cooler";
// Bytes; dt is whole ms. m is sampled minimum free, a is the allocator's
// arena high-water since boot, not a live-allocation peak. 
static const char sfcLog[] SFC_RO = "[sfc] f%ld s%ld h%lu l%lu a%lu m%lu n%lu dt%lu\n";
SFC_CODE static uint32_t sfcHeap(void) {
    uint32_t used = (uint32_t) mallinfo().uordblks;
    return __ctru_heap_size >= used ? __ctru_heap_size - used : 0;
}
SFC_CODE static void sfcNote(Runner* r, int state, uint32_t minimum, uint32_t linearMinimum, u64 start) {
    struct mallinfo mi = mallinfo();
    logInfo(sfcLog, (long) r->frameCount, (long) state, (unsigned long) sfcHeap(),
            (unsigned long) sfcLinearFree(), (unsigned long) mi.arena, (unsigned long) minimum, (unsigned long) linearMinimum,
            (unsigned long) ((svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC));
}
SFC_CODE void sfcSkip(Renderer* renderer) {
    sfcOldSkip(renderer);
    Runner* r = renderer->runner;
    if (r == NULL || r->pendingRoom != 258 || r->dataWin == NULL || r->dataWin->room.count <= 258 ||
        strcmp(r->dataWin->room.rooms[258].name, sfcRoom) != 0) return;
    CtrAudioSystem* ca = *ctrPcmWaitUta.target;
    HplState* s = HPL_STATE();
    if (ca == NULL || s == NULL || ca->base.audioGroups == NULL) return;
    DataWin* dw = ca->base.audioGroups[0];
    int sound = -1;
    for (uint32_t i = 0; i < dw->sond.count; ++i) {
        if (strcmp(dw->sond.sounds[i].name, sfcName) == 0) { sound = (int) i; break; }
    }
    if (sound < 0 || dw->sond.sounds[sound].file == NULL) return;
    sfcReap(s);
    int slot = -1, worker = -1, existing = -1;
    for (int i = 0; i < HPL_JOBS; ++i) {
        if (s->jobs[i] != NULL && s->jobs[i]->sound == sound) existing = i;
        if (s->jobs[i] == NULL) slot = i;
        if (s->threads[i] == NULL) worker = i;
    }
    u64 start = svcGetSystemTick();
    uint32_t minimum = sfcHeap();
    uint32_t linearMinimum = sfcLinearFree();
    sfcNote(r, 0, minimum, linearMinimum, start);
    HplJob* job = existing >= 0 ? s->jobs[existing] : NULL;
    if (job != NULL) { slot = existing; goto wait_for_copy; } // one copy only; wait even on reentry 
    if (slot < 0 || worker < 0 || minimum < CtrTileReader_heapReserve() + SFC_PCM + SFC_WORK ||
        linearMinimum < SFC_LINEAR_FLOOR) { sfcNote(r, 3, minimum, linearMinimum, start); return; }
    // No allocation in this entry may shed textures or fall back to linear. 
    bool relief = CtrMemory_allowRelief(false);
    job = (HplJob*) calloc(1, sizeof(HplJob));
    if (job != NULL) {
        job->path = sfcResolve(ca, dw->sond.sounds[sound].file);
        if (job->path == NULL || strlen(job->path) >= 256) {
            free(job->path); free(job); job = NULL;
        }
    }
    if (job == NULL) {
        CtrMemory_allowRelief(relief); sfcNote(r, 3, sfcHeap(), linearMinimum, start); return;
    }
    job->sound = sound;
    job->state = HPL_RUNNING;
    job->start = start;
    // Hide only pointer provenance: GCC otherwise replaces this memcpy with
// a duplicate anonymous string in ordinary .rodata.str1.1. 
    const char* copyName = sfcName;
    __asm__("" : "+r"(copyName));
    memcpy(job->name, copyName, sizeof(sfcName));
    __atomic_store_n(&s->jobs[slot], job, __ATOMIC_RELEASE);
    s->threads[worker] = threadCreate(sfcWork, job, HPL_STACK, HPL_PRIORITY, 0, false);
    CtrMemory_allowRelief(relief);
    if (s->threads[worker] == NULL) {
        job->state = HPL_FAILED; sfcRelease(s, slot); sfcNote(r, 3, sfcHeap(), linearMinimum, start); return;
    }
wait_for_copy:;
    int state;
    do {
        uint32_t freeNow = sfcHeap();
        if (freeNow < minimum) minimum = freeNow;
        uint32_t linearNow = sfcLinearFree();
        if (linearNow < linearMinimum) linearMinimum = linearNow;
        state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
        if (state != HPL_RUNNING) break;
        svcSleepThread(1000000); // yield core 0; no update of a voiceless job 
    } while (svcGetSystemTick() - start < 2000ULL * CPU_TICKS_PER_MSEC);
    if (state != HPL_READY || job->bytes != SFC_PCM || job->frames != 280256 || job->channels != 2 ||
        job->rate != 44100 || minimum < CtrTileReader_heapReserve() || linearMinimum < SFC_LINEAR_FLOOR) {
        sfcRelease(s, slot);
        state = state == HPL_RUNNING ? 4 : 3; // fallback, never a qualification PASS 
    }
    sfcReap(s);
    sfcNote(r, state, minimum, linearMinimum, start);
}
#endif
