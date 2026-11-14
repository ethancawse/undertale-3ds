// audpar: preserve these existing helpers' exact places after the menu query.
// A new query call edge can reorder functions within their former shared section.
#if defined(ENABLE_WAD16)
#define AUDPAR_DSP_SECTION ".text.audofix_late_dsp"
#define AUDPAR_BOUNDARY_SECTION ".text.audofix_late_boundary"
#else
#define AUDPAR_DSP_SECTION ".text.audofix_late"
#define AUDPAR_BOUNDARY_SECTION ".text.audofix_late"
#endif
#line 1
// Hidden room loads: request on the game thread, apply on the DSP callback.
// No wait, source walk, vtable replacement or per-voice clock on the requester.
#define LM_BSS __attribute__((section(".bss.late.loadaudio")))
// File-name lookup hashes only. The existing phase-song spare opens the next
// intro/fight song at its quiet second; flowhitch prepares its first buffer.
// No new decoder, PCM store, play request or source-position change.
const uint32_t CtrLoadmix_songs[2][2]
    __attribute__((aligned(4), section(".rodata.late.loadaudio"))) = {
        {0x7CDAE4A4u, 0x407AC0B1u}, {0x407AC0B1u, 0x14354745u}
    };
extern const struct {
    CtrAudioSystem** target;
    void (*callback)(void*);
    ndspCallback* engine;
    LightLock* lock;
    UtaChan* channels;
    int8_t* inverse;
    volatile bool* mapped;
} CtrLoadmix_legacy;
#ifndef LM_HOST
__asm__(".pushsection .rodata.late.loadaudio,\"a\",%progbits\n.p2align 2\n"
        ".global CtrLoadmix_legacy\n.type CtrLoadmix_legacy,%object\nCtrLoadmix_legacy:\n"
        ".word gShutdownTarget,utaFrameCallback,utaEngineCallback,utaLock,utaChan,utaInv,utaMapped\n"
        ".size CtrLoadmix_legacy,.-CtrLoadmix_legacy\n.popsection\n");
#endif

static struct {
    // command: bit 0 is an open load window, bit 1 publishes its fields.
    // Totals wrap together; their signed difference is only one DSP update.
    uint32_t command, applied, dirty, goal, start;
    uint32_t frame; // game thread only, at the outgoing Step/Draw boundary
    uint32_t lastTick, heldTicks; // DSP thread only, actual physical hold time
    uint32_t watchCommand, emptyTick, unusedTicks, creditTicks; // DSP thread only
    Runner* runner;
    bool rampHeld; // decode thread only
    uint8_t waiting; // DSP: 1 initial, 2 last update empty, 3 unknown, 0 decided
} lm LM_BSS;

// Two real display vblanks per 30 Hz game frame, as in ctr_music_clock.h.
// Count in ARM11 ticks: no rounded milliseconds or per-load rounding loss.
#define LM_FRAME_TICKS 8962272u

#ifdef CTR_AUDIO_PCM_HASH
// Frozen diagnostic twins change only this slot: the load hold and the two
// additional song guesses. Rev 3m's original preparation stays on in both.
static volatile const uint32_t lmdEnabled
    __attribute__((used, section(".rodata.late.loadaudio"))) = 1;
LM_CODE bool CtrLoadmix_preparing(void) { return lmdEnabled != 0; }
typedef struct {
    u64 tick;
    uint32_t kind, command, applied, vblank, detail, frame;
    uint32_t voice[24], sequence[24], position[24];
    bool ready, printed;
} LmdEvent;
static struct {
    LmdEvent events[256];
    uint32_t count, overflow, generation, remaining;
    bool held, overflowPrinted;
} lmd LM_BSS;
static const char lmdHeader[] __attribute__((section(".rodata.late.loadaudio"))) =
    "[loadmix] %lu %lu %lu %lu %llu %lu %lu %lu\n";
// One fixed record format: kind 1..9 is an event; 10 is a physical cursor
// (channel, voice, sequence, position, 0, 0); 11 marks an incomplete ring.

LM_CODE static void lmdEvent(uint32_t kind, uint32_t detail) {
    uint32_t index = __atomic_fetch_add(&lmd.count, 1, __ATOMIC_RELAXED);
    if (index >= 256) { __atomic_store_n(&lmd.overflow, 1, __ATOMIC_RELEASE); return; }
    LmdEvent* row = &lmd.events[index];
    row->tick = svcGetSystemTick();
    row->kind = kind; row->detail = detail;
    row->command = __atomic_load_n(&lm.command, __ATOMIC_ACQUIRE);
    row->applied = __atomic_load_n(&lm.applied, __ATOMIC_ACQUIRE);
    row->vblank = C3D_FrameCounter(0);
    row->frame = lat.runner != NULL ? (uint32_t) lat.runner->frameCount : 0;
    // Only game/DSP-thread events sample channels. Swap callbacks just stamp
    // their event; no extra channel walk is introduced on the GSP thread.
    if (kind <= 6 || kind == 9) for (unsigned p = 0; p < 24; p++) {
        int id = *CtrLoadmix_legacy.mapped ? CtrLoadmix_legacy.inverse[p] : (int) p;
        row->voice[p] = id >= 0 && id < VOICE_COUNT
            ? __atomic_load_n(&lat.current[id], __ATOMIC_ACQUIRE) : 0;
        uint32_t seq = UTA_PHYS_SEQ(p);
        row->position[p] = UTA_PHYS_POS(p);
        bool same = seq == UTA_PHYS_SEQ(p) &&
            id == (*CtrLoadmix_legacy.mapped ? CtrLoadmix_legacy.inverse[p] : (int) p);
        if (id >= 0 && id < VOICE_COUNT)
            same = same && row->voice[p] == __atomic_load_n(&lat.current[id], __ATOMIC_ACQUIRE);
        row->sequence[p] = same ? seq : 0;
    }
    __atomic_store_n(&row->ready, true, __ATOMIC_RELEASE);
}

// DSP thread only: two updates after each request generation/release. These
// raw cursor snapshots are observations, not an assumed DSP acknowledgment.
LM_CODE static void lmdCallback(void) {
    uint32_t command = __atomic_load_n(&lm.command, __ATOMIC_ACQUIRE);
    bool held = __atomic_load_n(&lm.applied, __ATOMIC_ACQUIRE) != 0;
    if ((command & ~3u) != lmd.generation || held != lmd.held) {
        lmd.generation = command & ~3u; lmd.held = held; lmd.remaining = 2;
    } else if (lmd.remaining && --lmd.remaining == 0) {
        lmdEvent(held ? 4 : 6, (uint32_t) lmdEnabled);
    }
}

LM_CODE static void lmdDrain(void) {
    if (lat.runner == NULL) return;
    uint32_t frame = (uint32_t) lat.runner->frameCount;
    uint32_t count = __atomic_load_n(&lmd.count, __ATOMIC_ACQUIRE);
    if (count > 256) count = 256;
    for (uint32_t i = 0; i < count; i++) {
        LmdEvent* row = &lmd.events[i];
        if (!__atomic_load_n(&row->ready, __ATOMIC_ACQUIRE) || row->printed ||
            frame < row->frame || frame - row->frame < 60) continue;
        row->printed = true;
        logQuiet(lmdHeader, (unsigned long) i, (unsigned long) row->kind,
                 (unsigned long) row->command, (unsigned long) row->applied,
                 (unsigned long long) row->tick, (unsigned long) row->vblank,
                 (unsigned long) row->detail, (unsigned long) row->frame);
        if (row->kind <= 6 || row->kind == 9) for (unsigned p = 0; p < 24; p++)
            if (row->voice[p] || row->sequence[p])
                logQuiet(lmdHeader, (unsigned long) i, 10ul, (unsigned long) p,
                         (unsigned long) row->voice[p], (unsigned long long) row->sequence[p],
                         (unsigned long) row->position[p], 0ul, 0ul);
    }
    if (__atomic_load_n(&lmd.overflow, __ATOMIC_ACQUIRE) && !lmd.overflowPrinted) {
        lmd.overflowPrinted = true;
        logQuiet(lmdHeader, 0ul, 11ul, 0ul, 0ul, 0ull, 0ul, 0ul, 0ul);
    }
}
#define LMD_EVENT(kind, detail) lmdEvent(kind, detail)
#else
#define LMD_EVENT(kind, detail) ((void) 0)
#endif

#include "ctr_audio_transport_mix.inc"

LM_CODE static bool lmHeld(void) {
#ifdef CTR_AUDIO_PCM_HASH
    if (!lmdEnabled) return false;
#endif
    return __atomic_load_n(&lm.applied, __ATOMIC_ACQUIRE);
}

LM_CODE static void lmPaused(int p, bool paused) {
    bool held = lmHeld();
    PT_PHYS_PAUSED(p, paused || held);
    // Publish AFTER the physical write, even if the sampled hold was false.
    // Entry/release may have consumed the preceding repair before this write.
    __atomic_store_n(&lm.dirty, 1, __ATOMIC_RELEASE);
}

LM_CODE static void lmReset(void) {
    __atomic_store_n(&lm.command, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&lm.applied, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&lm.dirty, 0, __ATOMIC_RELEASE);
    lm.runner = NULL;
    lm.goal = lm.start = lm.frame = lm.lastTick = lm.heldTicks = 0;
    lm.watchCommand = lm.emptyTick = lm.unusedTicks = lm.creditTicks = 0;
    lm.waiting = 0;
    __atomic_store_n(&lm.rampHeld, false, __ATOMIC_RELEASE);
}

#ifndef LM_HOST
static void gomDspCallback(void* data); static void ptDspCallback(void* data); __attribute__((noinline, noipa, optimize("Os"), section(AUDPAR_DSP_SECTION))) static void audofixDspCallback(void* data); LM_CODE static void lmRegister(ndspCallback callback, void* data) {
    lmReset();
    LOADCLASS_PHYS_CALLBACK(callback != NULL ? gomDspCallback : NULL, data); // loadclass 4c -> gomusic -> play-through -> audofix H1 -> lmCallback (file end)
}
#endif

// Is there a source queued for this mix? 1 empty, 0 ready, -1 busy/unknown.
// A cached voice or feeder tail conservatively counts as ready. Stream
// status is read under its existing producer lock, never by waiting on it.
// Called only until the first ready observation of an open load window.
LM_CODE static int lmEmpty(CtrAudioSystem* ca) {
    if (LightLock_TryLock(CtrLoadmix_legacy.lock) != 0) return -1;
    bool ready = false;
    for (unsigned p = 0; p < VOICE_COUNT; p++)
        if (CtrLoadmix_legacy.channels[p].state != UTA_IDLE || ca->voices[p].kind == VOICE_SFX) ready = true;
    LightLock_Unlock(CtrLoadmix_legacy.lock);
    if (ready) return 0;
    for (unsigned i = 0; i < STREAM_CONTEXTS; i++) {
        StreamContext* s = &ca->streams[i];
        if (LightLock_TryLock(&s->lock) != 0) return -1;
        for (unsigned b = 0; b < STREAM_BUFFERS; b++) {
            unsigned status = s->bufs[b].wb.status;
            if (status == NDSP_WBUF_QUEUED || status == NDSP_WBUF_PLAYING) ready = true;
        }
        LightLock_Unlock(&s->lock);
        if (ready) return 0;
    }
    return 1;
}

LM_CODE static void lmCallback(void* data) {
    CtrAudioSystem* ca = (CtrAudioSystem*) data;
    uint32_t now = (uint32_t) svcGetSystemTick();
    if (__atomic_load_n(&lm.applied, __ATOMIC_ACQUIRE))
        lm.heldTicks += now - lm.lastTick;
    lm.lastTick = now;
    uint32_t command = __atomic_load_n(&lm.command, __ATOMIC_ACQUIRE);
    uint32_t goal = __atomic_load_n(&lm.goal, __ATOMIC_RELAXED);
    uint32_t start = __atomic_load_n(&lm.start, __ATOMIC_RELAXED);
    // A publishing game thread never makes the DSP callback wait or spin.
    if ((command & 2u) || command != __atomic_load_n(&lm.command, __ATOMIC_ACQUIRE)) {
        (*CtrLoadmix_legacy.engine)(data);
        return;
    }
    if (command != lm.watchCommand) {
        lm.watchCommand = command;
        lm.waiting = command & 1u ? 1 : 0;
        lm.unusedTicks = 0;
    }
    if (lm.waiting) {
        int empty = lmEmpty(ca);
        if (command != __atomic_load_n(&lm.command, __ATOMIC_ACQUIRE)) {
            (*CtrLoadmix_legacy.engine)(data);
            return;
        }
        if (empty > 0) {
            if (lm.waiting == 2) {
                uint32_t elapsed = lm.unusedTicks + now - lm.emptyTick;
                lm.unusedTicks = elapsed < LM_FRAME_TICKS ? elapsed : LM_FRAME_TICKS;
            }
            lm.emptyTick = now;
            lm.waiting = 2;
        } else if (empty == 0) {
            // Restore only the observed unused allowance, at most one frame,
            // once a source exists in this SAME window. An entirely empty load
            // earns no credit for a later song. Never seek or skip samples.
            lm.creditTicks += lm.unusedTicks;
            lm.waiting = 0;
        } else if (lm.waiting == 2) lm.waiting = 3;
    }
    if (command & 1u) goal += now - start - LM_FRAME_TICKS;
    bool dirty = __atomic_exchange_n(&lm.dirty, 0, __ATOMIC_ACQ_REL);
    bool wanted = (int32_t) (goal - lm.heldTicks - lm.creditTicks) > 0;
#ifdef CTR_AUDIO_PCM_HASH
    wanted = wanted && lmdEnabled;
    lmdCallback();
#endif
    if (dirty || wanted != __atomic_load_n(&lm.applied, __ATOMIC_ACQUIRE)) {
        if (LightLock_TryLock(CtrLoadmix_legacy.lock) != 0) {
            __atomic_store_n(&lm.dirty, 1, __ATOMIC_RELEASE);
            (*CtrLoadmix_legacy.engine)(data);
            return;
        }
#ifdef CTR_AUDIO_PCM_HASH
        // Post-write repairs are now unconditional. Record real transport
        // transitions only, rather than filling the finite diagnostic ring
        // with repeated same-state repairs during ordinary play.
        if (wanted != __atomic_load_n(&lm.applied, __ATOMIC_ACQUIRE))
            LMD_EVENT(wanted ? 3 : 5, 0);
#endif
        CtrAudio_transportMixLocked(ca, wanted, -1);
        __atomic_store_n(&lm.applied, wanted, __ATOMIC_RELEASE);
        LightLock_Unlock(CtrLoadmix_legacy.lock);
    }
    // Freeze utaCb too: it is the tail deadline, not just a service counter.
    // Keep the engine notification so the decoder can prepare unplayed PCM.
    if (lmHeld()) (*CtrLoadmix_legacy.engine)(data);
    else CtrLoadmix_legacy.callback(data);
}

// Decoder preparation continues during a load, while positioned-mix ramps
// earn no wall-clock progress from it. The original inlined ramp body stays.
LM_CODE static int lmDecodeWait(LightEvent* event, s64 timeout) {
    int result = (LightEvent_WaitTimeout)(event, timeout);
    bool held = lmHeld();
    CtrAudioSystem* ca = *CtrLoadmix_legacy.target;
    if (ca != NULL && (held || __atomic_load_n(&lm.rampHeld, __ATOMIC_ACQUIRE))) {
        LightLock_Lock(&ca->rampLock);
        ca->rampTick = svcGetSystemTick();
        LightLock_Unlock(&ca->rampLock);
    }
    __atomic_store_n(&lm.rampHeld, held, __ATOMIC_RELEASE);
    return result;
}

LM_CODE void CtrLoadmix_boundary(Runner* runner, bool loaded) {
    if (!loaded) {
        uint32_t command = __atomic_load_n(&lm.command, __ATOMIC_ACQUIRE);
        uint32_t frame = (uint32_t) runner->frameCount;
        bool close = (command & 1u) && (runner != lm.runner || frame != lm.frame);
        bool open = runner->pendingRoom >= 0 && (!(command & 1u) || close);
        if (close || open) {
            uint32_t next = (command + 4u) & ~3u;
            __atomic_store_n(&lm.command, next | 2u, __ATOMIC_SEQ_CST);
            uint32_t now = (uint32_t) svcGetSystemTick();
            if (close) {
                uint32_t frames = frame - lm.frame;
                if (runner != lm.runner || (int32_t) frames <= 0) frames = 1u;
                uint32_t goal = __atomic_load_n(&lm.goal, __ATOMIC_RELAXED);
                goal += now - __atomic_load_n(&lm.start, __ATOMIC_RELAXED) - frames * LM_FRAME_TICKS;
                __atomic_store_n(&lm.goal, goal, __ATOMIC_RELAXED);
            }
            lm.runner = runner;
            lm.frame = frame;
            __atomic_store_n(&lm.start, now, __ATOMIC_RELAXED);
            __atomic_store_n(&lm.command, next | (open ? 1u : 0u), __ATOMIC_RELEASE);
        }
    }
#ifdef CTR_AUDIO_PCM_HASH
    CtrLoadaudio_boundary(runner, loaded);
    if (loaded || runner->pendingRoom >= 0) LMD_EVENT(loaded ? 2 : 1, lat.loadCount);
#else
    (void) loaded;
#endif
}

#ifndef LM_HOST
// Keep the freebies wrapper's original single-call extent and function order.
// The audio object is excluded from earlier generic late-section collectors.
void __real_Runner_handlePendingRoomChange(Runner* runner);
__attribute__((noinline, noipa, optimize("Os"), section(AUDPAR_BOUNDARY_SECTION))) static void audofixBoundary(Runner* runner, bool loaded); void audofixPtRoomCall(Runner* runner, bool loaded) __asm__("audofixPtRoom"); LM_CODE void CtrLoadmix_handleRoomChange(Runner* runner) {
    int32_t pending = runner->pendingRoom;
    audofixPtRoomCall(runner, false); // audofix H1 + play-through (file end: audofixPtRoom)
    __real_Runner_handlePendingRoomChange(runner);
    if (pending != -1) CtrLoadmix_boundary(runner, true);
}
#endif

LM_CODE void CtrLoadmix_chainEndFrame(Renderer* renderer) {
    CTR_ENDFRAME_AFTER_LOADMIX(renderer);
#ifdef CTR_AUDIO_PCM_HASH
    lmdDrain();
#endif
}


// audofix H1 (review B #5, review C; UNDERTALE: this include is ENABLE_WAD16 only): a load window (a room change until the next frame's boundary) counts
// the wall time since lm.start as load time, and the mix holds the transport (music and SFX) for what passes a frame.
// Time the app spent away inside the window counted too: HOME and sleep put the DSP to sleep (DSPHOOK_ONSLEEP; the
// console log has "APT: putting the DSP to sleep" right after "HOME pressed"), so no frame callback runs until it
// wakes, and the Rosalina menu stops the app's threads, the DSP callback's among them. After HOME in the frame after a
// quick room change, sound stayed frozen after the return for about as long as the player was away.
// Now a gap of more than 4 frames since the DSP's last frame callback (lm.lastTick; one comes every ~5 ms while the
// DSP is awake, through any load) is time away, not load time, and the window's start moves past it. The first of
// the two threads to see the gap takes it: the DSP callback before its update (the first callback after waking), or
// the game thread before a boundary (the close may come while the DSP still sleeps). Taking it moves lm.lastTick to
// now by compare-and-swap, so only one thread moves the start for one gap; the start moves by compare-and-swap too,
// so a start the game thread replaced since (a new window) is left alone. Skipped while the game thread publishes
// (command bit 2) and while the hold is applied: then lmCallback counts the gap as held time, which cancels it, as
// before. Gaps compare unsigned (c26rev r96 F1: a signed one skipped every gap of 8-16 s and left the goal seconds in debt, so later loads went unheld): the 32-bit tick wraps every 16.02 s, and the goal only ever sees a gap modulo that, so taking the remainder takes it all; only a remainder under 4 frames stays load time (at most 4 frames held). The clock is read here, after lm.lastTick (c26rev r96b: a caller's clock read before a 5 s stop, then this thread's CAS, put a stamp the DSP had just moved forward back by the 5 s, and the close held the sound again for them); a stamp moved since the load fails the CAS.
#define AUDOFIX_AWAY_TICKS (4u * LM_FRAME_TICKS)

__attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_late"))) static void audofixLeaveOutAway(void) {
    uint32_t command = __atomic_load_n(&lm.command, __ATOMIC_ACQUIRE);
    uint32_t last = __atomic_load_n(&lm.lastTick, __ATOMIC_ACQUIRE), now = (uint32_t) svcGetSystemTick();
    if ((command & 3u) != 1u || now - last <= AUDOFIX_AWAY_TICKS ||
        __atomic_load_n(&lm.applied, __ATOMIC_ACQUIRE))
        return;
    uint32_t start = __atomic_load_n(&lm.start, __ATOMIC_ACQUIRE);
    if (!__atomic_compare_exchange_n(&lm.lastTick, &last, now, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return; // the other thread took this gap
    uint32_t from = (int32_t) (last - start) > 0 ? last : start; // away since the later of the two
    if (now != from)
        __atomic_compare_exchange_n(&lm.start, &start, start + (now - from), false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

// The DSP frame callback (lmRegister installs this in lmCallback's place).
__attribute__((noinline, noipa, optimize("Os"), section(AUDPAR_DSP_SECTION))) static void audofixDspCallback(void* data) {
    audofixLeaveOutAway();
    lmCallback(data);
}

// The game thread's boundary before a room change (H1's part; audofixPtRoom, below, calls it and adds the
// play-through's snapshot). Its size stays H1's: code placed after it in .text.audofix_late keeps its address.
__attribute__((noinline, noipa, optimize("Os"), section(AUDPAR_BOUNDARY_SECTION))) static void audofixBoundary(Runner* runner, bool loaded) {
    if (!loaded) audofixLeaveOutAway();
    CtrLoadmix_boundary(runner, loaded);
}

// ===[ audofix play-through (playtest 11-01 Lost Souls cuts, 11-02 clicks at Hotland's doors) ]===
// The load hold paused the whole mix for a load's excess over a frame (lmCallback: 10-80 ms at an ordinary door,
// 220 ms and more at the prebarrier): every sound playing at the door stopped mid-waveform and went on later, a cut in
// a song and a click in an SFX tail or ambient loop. Now the hold is for new sounds only:
// - a sound playing when the window opens (music, an ambient loop, an SFX or its tail: any voice whose serial is the
//   one the open saw, playing, stopped with a tail, or free) is never paused by the load: no edge, nothing to click;
// - a sound started inside the window (its voice's serial moved: a new play, or a voice taken from another sound) is
//   paused before its first sample (applyVoice pauses before it queues; a stream's open pauses before its buffers)
//   until the window closes, the boundary after the room's first frame's Draw, and then starts from sample 0, as on
//   the PC (the room's Create plays it, then that frame is shown). It is never paused mid-sound, so neither edge is a
//   cut: a fade would only change its attack from the PC's. When the room asks for another room before its first
//   frame (close and open at once, the frame not shown), the open keeps the snapshot: it starts at the last close.
//   This contains asgore30's fresh-song hold (a quiet window whose new song blipped for its credited frame), which it
//   replaces.
// - lmCallback's whole-mix hold never comes: its balance is set to zero each callback, an open window's elapsed time
//   credited, so its own bookkeeping (and audofix H1's) runs as before and holds nothing. The frame callback (uta's
//   feeding of cached sounds and tails) is then never frozen, so the playing sounds are fed through the load.
// The two physical pause writes of the hold (lmPaused for the game's own, the transport for lmCallback's repairs) add
// the new-sound hold (ptPhysPaused); the DSP callback (ptDspCallback, after lmCallback) unpauses, to the logical
// state, every channel this rule paused that is no longer held (TryLock: a busy lock retries the next callback).
// Costs: a song playing at a load runs on through it (by the load's excess over a frame, as on the PC); only a
// script that waits for a non-looping sound started before a load and polled after it can see that (the waiting-script
// audit).
#ifndef PT_CODE
#define PT_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.audofix_feed")))
#define PT_BSS __attribute__((section(".bss.audofix_late")))
#endif
static struct {
    uint32_t snap[VOICE_COUNT]; // each voice's serial at the window's open (game thread)
    uint32_t held; // channels this rule paused (bit p): set by any writer, cleared by the DSP callback
    volatile bool open; // the snapshot is whole (game thread)
} pt PT_BSS;

// Channel p plays a sound started inside the open window.
PT_CODE static bool ptHeld(int p) {
    CtrAudioSystem* ca = *CtrLoadmix_legacy.target;
    if (!__atomic_load_n(&pt.open, __ATOMIC_ACQUIRE) || !(__atomic_load_n(&lm.command, __ATOMIC_ACQUIRE) & 1u) ||
        ca == NULL || (unsigned) p >= VOICE_COUNT)
        return false;
    int i = *CtrLoadmix_legacy.mapped ? CtrLoadmix_legacy.inverse[p] : p;
    return (unsigned) i < VOICE_COUNT && __atomic_load_n(&ca->voices[i].serial, __ATOMIC_RELAXED) != pt.snap[i];
}

// lmPaused's and the load transport's physical pause write (any thread).
PT_CODE void ptPhysPaused(int p, bool paused) {
    bool held = ptHeld(p);
    if (held) __atomic_fetch_or(&pt.held, 1u << p, __ATOMIC_ACQ_REL);
    UTA_PHYS_PAUSED(p, paused || held);
}
#ifndef LM_HOST
__asm__(".set gomMixLink, gomPhysMix\n"); // gomusic: a stream's mix (ctr_audio_system.c's end)
__asm__(".set audofixPtPaused, " LOADCLASS_PAUSE_LINK "\n"); // loadclass 4c's hold, gomusic's, then ptPhysPaused (ctr_audio_system.c's end)
#endif

// Game thread, after the boundary: a window opened from none takes the snapshot; a close drops it.
PT_CODE static void ptBoundary(uint32_t before) {
    uint32_t command = __atomic_load_n(&lm.command, __ATOMIC_ACQUIRE);
    if (command == before || ((command & 1u) && (before & 1u))) return;
    __atomic_store_n(&pt.open, false, __ATOMIC_RELEASE);
    CtrAudioSystem* ca = *CtrLoadmix_legacy.target;
    if (!(command & 1u) || ca == NULL) return;
    for (unsigned i = 0; i < VOICE_COUNT; i++) pt.snap[i] = ca->voices[i].serial;
    __atomic_store_n(&pt.open, true, __ATOMIC_RELEASE);
}

// CtrLoadmix_handleRoomChange's first call (through its assembler name): the boundary, where a window closes and
// opens, then the snapshot. lm.command is read first; audofixLeaveOutAway (in the boundary) never writes it.
PT_CODE void audofixPtRoom(Runner* runner, bool loaded) {
    uint32_t before = __atomic_load_n(&lm.command, __ATOMIC_ACQUIRE);
    audofixBoundary(runner, loaded);
    ptBoundary(before);
}

// The DSP frame callback (lmRegister installs it).
PT_CODE static void ptDspCallback(void* data) {
    audofixLeaveOutAway(); // time away first (H1, again in audofixDspCallback: no gap left), so the credit sees its start
    uint32_t command = __atomic_load_n(&lm.command, __ATOMIC_ACQUIRE);
    if (!(command & 2u)) { // a publishing game thread: lmCallback only passes the update on
        uint32_t credit = __atomic_load_n(&lm.goal, __ATOMIC_RELAXED) - lm.heldTicks;
        if (command & 1u) credit += (uint32_t) svcGetSystemTick() - __atomic_load_n(&lm.start, __ATOMIC_RELAXED);
        lm.creditTicks = credit;
    }
    audofixDspCallback(data);
    uint32_t mask = __atomic_load_n(&pt.held, __ATOMIC_ACQUIRE);
    if (mask == 0 || LightLock_TryLock(CtrLoadmix_legacy.lock) != 0) return;
    CtrAudioSystem* ca = (CtrAudioSystem*) data;
    for (int p = 0; p < VOICE_COUNT; p++) {
        if (!((mask >> p) & 1u) || ptHeld(p)) continue;
        __atomic_fetch_and(&pt.held, ~(1u << p), __ATOMIC_ACQ_REL);
        int i = *CtrLoadmix_legacy.mapped ? CtrLoadmix_legacy.inverse[p] : p;
        const UtaChan* c = &CtrLoadmix_legacy.channels[p];
        UTA_PHYS_PAUSED(p, c->state != UTA_IDLE ? c->paused : ca->suspended || ca->voices[i].paused);
    }
    LightLock_Unlock(CtrLoadmix_legacy.lock);
}
