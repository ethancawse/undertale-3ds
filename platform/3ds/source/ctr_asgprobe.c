// asgprobe (build argument asgprobe, default off, test only, utasgore): hardware attribution of the step and draw
// time of busy frames (UNDERTALE's Asgore flame helix) with the ARM11 performance counters, read through Luma3DS's
// svcControlPerformanceCounter (SVC 0x3E; the stub is icsim's). Nothing in the normal text changes: the link sends
// main.c's Runner_step and Runner_drawViews calls to the wrappers here (late text), and the drawSprite entry of the
// renderer's own vtable (the bottom of every table chain) is wrapped at the first draw.
//
// Each frame counts one of six event pairs (frame number mod 6) on PMN0/PMN1, plus the cycle counter, around the step
// and around Runner_drawViews. Every other round of six frames also reads the counters around every 4th drawSprite
// call, which splits the draw into the sprite calls and the rest (those frames are summed apart: the reads cost time).
// Frames are grouped by how many drawSprite calls they made (bucket edges 64, 192, 320, 448). Every 300 frames each
// (bucket, set, sampled) group with frames logs its running sums as an "[asgprobe]" line; an analysis
// script (not included) reads them.
//
// Without counters (Azahar, or a refusal) it still counts frames and sprites and logs zeros for the events.
#ifdef CTRPORT_ASG_PROBE
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "instance.h"
#include "vm.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define APL __attribute__((noinline, noclone, section(".text.late.utasgore.probe")))
#ifdef AP_LEAN
#pragma GCC diagnostic ignored "-Wunused-function"  // the lean build drops the sequence dump and [asgtime]
#endif
#define APB __attribute__((section(".bss.late.utasgore.probe")))

__asm__(".pushsection .text.late.utasgore.probe,\"ax\",%progbits\n"
        ".arm\n.balign 4\n.global apPerfSvc\n.type apPerfSvc, %function\n"
        "apPerfSvc:\n"
        "push {r0}\nldr r0, [sp, #4]\nldr r3, [sp, #8]\nsvc 0x3e\n"
        "pop {r3}\nstm r3, {r1,r2}\nbx lr\n"
        ".size apPerfSvc, .-apPerfSvc\n"
        ".popsection\n");
Result apPerfSvc(u64* out, PerfCounterOperation op, u32 param1, u64 param2);

void __real_Runner_step(Runner* runner);
#ifdef AP_INPUTLATE
// inputlate's step wrapper, renamed in this build (CMakeLists.txt), and the real step it calls.
void apInputlateStep(Runner* runner);
void apRealStep(Runner* runner);
void apRealStep(Runner* runner) {
    __real_Runner_step(runner);
}
#define AP_STEP apInputlateStep
#else
#define AP_STEP __real_Runner_step
#endif
void __real_Runner_drawViews(Runner* runner, int32_t gameW, int32_t gameH, bool debugShowCollisionMasks);
void __wrap_Runner_step(Runner* runner) APL;
void __wrap_Runner_drawViews(Runner* runner, int32_t gameW, int32_t gameH, bool debugShowCollisionMasks) APL;

enum { AP_SETS = 6, AP_BUCKETS = 5, AP_SAMPLE = 4 };
// Set names for the log reader: 0 imiss/inst, 1 dread/dwrite misses, 2 stall-inst/stall-lsu, 3 dtlb/maintlb,
// 4 stall-hazard/branch-mispredict, 5 dread/dwrite (all accesses).
static const uint8_t apEvents[AP_SETS][2] __attribute__((section(".rodata.late.utasgore.probe"))) = {
    {PERFCOUNTEREVT_CORE_INST_CACHE_MISS, PERFCOUNTEREVT_CORE_INST_EXECUTED},
    {PERFCOUNTEREVT_CORE_DATA_CACHE_READ_MISS, PERFCOUNTEREVT_CORE_DATA_CACHE_WRITE_MISS},
    {PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST, PERFCOUNTEREVT_CORE_STALL_BY_LSU_FULL},
    {PERFCOUNTEREVT_CORE_DATA_MICRO_TLB_MISS, PERFCOUNTEREVT_CORE_MAIN_TLB_MISS},
    {PERFCOUNTEREVT_CORE_STALL_BY_DATA_HAZARD, PERFCOUNTEREVT_CORE_BRANCH_MISS_PREDICTED},
    {PERFCOUNTEREVT_CORE_DATA_CACHE_READ, PERFCOUNTEREVT_CORE_DATA_CACHE_WRITE},
};

typedef struct {
    uint32_t cycles, e0, e1;
} ApSnap;

typedef struct {
    uint64_t frames, sprites, drawCycles, drawMax, draw0, draw1, stepCycles, stepMax, step0, step1;
    uint64_t sampled, inCycles, in0, in1;
} ApSum;

typedef void (*ApDrawSprite)(Renderer*, int32_t, float, float, float, float, float, float, float, uint32_t, float);

static int8_t apState APB;  // 0 untried, 1 counting, -1 no counters
static uint8_t apSet APB, apSampling APB;
static uint32_t apSprites APB, apCalls APB, apFrames APB;
static uint32_t apStepCycles APB, apStep0 APB, apStep1 APB;
static uint32_t apInN APB, apInCycles APB, apIn0 APB, apIn1 APB;
static ApSum apSums[AP_BUCKETS][AP_SETS][2] APB;
extern const void* const Pixgrid_refs[2];
static ApDrawSprite apDrawSprite APB;
static Renderer* apRenderer APB;
static Runner* apRunner APB;

static APL uint32_t apGet(PerfCounterRegister reg) {
    u64 value = 0;
    apPerfSvc(&value, PERFCOUNTEROP_GET_VALUE, reg, 0);
    return (uint32_t) value;
}

static APL void apRead(ApSnap* s) {
    if (apState <= 0) {
        memset(s, 0, sizeof(*s));
        return;
    }
    s->cycles = apGet(PERFCOUNTERREG_CORE_CYCLE_COUNTER);
    s->e0 = apGet(PERFCOUNTERREG_CORE_COUNT_REG_0);
    s->e1 = apGet(PERFCOUNTERREG_CORE_COUNT_REG_1);
}

static APL void apStart(void) {
    u64 value = 0;
    s64 luma = 0;
    apState = -1;
    if (R_FAILED(svcGetSystemInfo(&luma, 0x10000, 0))) {
        logInfo("[asgprobe] no Luma3DS: no counters\n");
        return;
    }
    apPerfSvc(&value, PERFCOUNTEROP_DISABLE, 0, 0);  // releases a lock a crashed run left
    Result r = apPerfSvc(&value, PERFCOUNTEROP_ENABLE, 0, 0);
    if (R_FAILED(r)) {
        logInfo("[asgprobe] counters refused %08lX\n", (unsigned long) r);
        return;
    }
    apState = 1;
    logInfo("[asgprobe] counters on\n");
}

static APL void apSetEvents(int set) {
    if (apState <= 0) return;
    u64 value = 0;
    apPerfSvc(&value, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0, apEvents[set][0]);
    apPerfSvc(&value, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1, apEvents[set][1]);
}

// One frame's drawSprite arguments, logged once: the first frame of 320 calls or more after <game dir>/asgdump.txt is
// seen (looked for at the first draw).
static int8_t apDump APB;  // 0 not looked, 1 armed, 2 dumping this frame, -1 off or done
static uint32_t apDumpPrev APB;

// Tick attribution (<game dir>/asgtime.txt, looked for with asgdump.txt): in frames after one of 192 calls or more,
// each drawSprite call's svcGetSystemTick time goes to its item (in or out of a 672 x 512 box around the battle view
// place), with the frame's draw and step ticks. Logged as "[asgtime]" lines every 300 frames.
enum { AT_ITEMS = 4096 };
static int8_t apTime APB;  // 0 not looked, 1 on, -1 off
static uint8_t apTimeBusy APB;
static uint32_t apTimeTicks[AT_ITEMS][2] APB, apTimeCalls[AT_ITEMS][2] APB;
static uint64_t apTimeDraw APB, apTimeStep APB, apTimeFrames APB, apTimeSprites APB;
static uint32_t apTimeStepNow APB;

extern uint32_t utaCount[8], utaShadowCount[4];
extern uint64_t utaTicks[6];
extern uint32_t utaWhy[48];
extern uint8_t utaOff;

// Frame log (on unless <game dir>/asgnoframes.txt): every frame, one "[asgframe]" line with its runner frame, sprite calls, step
// and draw ticks, the ticks inside drawSprite, the uta counters' and plan refusals' changes over the frame, and the
// four items that took the most drawSprite ticks (item:ticks/calls). Every drawSprite call is timed.
static int8_t apFrameLog APB;  // 0 not looked, 1 on, -1 off
static uint32_t apFTicks[AT_ITEMS] APB;
static uint16_t apFCalls[AT_ITEMS] APB;
static uint16_t apFTouched[AT_ITEMS] APB;
static uint32_t apFTouchedN APB, apFSprTicks APB;
static uint32_t apFUta[8] APB, apFWhy[48] APB;
// (asgore30) drawSpritePart and drawRectangle calls of the frame, timed like drawSprite's (the renderer's own table's
// entries): count, ticks, the last part item and the part texels drawn.
typedef void (*ApDrawPart)(Renderer*, int32_t, float, float, float, float, float, float, float, float, float, float, float, uint32_t, float);
typedef void (*ApDrawRect)(Renderer*, float, float, float, float, uint32_t, float, bool);
static ApDrawPart apDrawPart APB;
static ApDrawRect apDrawRect APB;
static uint32_t apPartN APB, apPartT APB, apRectN APB, apRectT APB, apPartItem APB, apPartPix APB;
// (asgore30) Draw-sequence dump (<game dir>/asgseq.txt): frames 370, 560, 698 and 699 record every drawSprite, part
// and rectangle call (kind, item, object, the renderer's draw calls and vertices after it, the open batch's texture),
// written to <game dir>/asgseq.log after the frame.
uint32_t CtrRenderer_probeCalls(Renderer* renderer, uint32_t* vertices, const void** tex);
extern uint32_t asbProbe[4];   // z_asgborder.c (asgore30's native draw_self_border_true)
typedef struct {
    char kind;
    int16_t item, object;
    uint16_t calls, vertices;
    uint32_t tex, ticks;
} ApSeq;
enum { AP_SEQ = 1536 };
static int8_t apSeqOn APB;  // 0 not looked, 1 on, -1 off
static uint8_t apSeqFrame APB;
static uint32_t apSeqN APB;
static ApSeq apSeqs[AP_SEQ] APB;
static APL void apSeqAdd(Renderer* renderer, char kind, int32_t item, uint32_t ticks) {
    if (!apSeqFrame || apSeqN >= AP_SEQ) return;
    ApSeq* q = &apSeqs[apSeqN++];
    const VMContext* ctx = apRunner != nullptr ? apRunner->vmContext : nullptr;
    const Instance* inst = ctx != nullptr ? ctx->currentInstance : nullptr;
    uint32_t v = 0;
    const void* tex = nullptr;
    q->calls = (uint16_t) CtrRenderer_probeCalls(renderer, &v, &tex);
    q->vertices = (uint16_t) v;
    q->tex = (uint32_t) (uintptr_t) tex;
    q->kind = kind;
    q->item = (int16_t) item;
    q->object = inst != nullptr ? (int16_t) inst->objectIndex : -1;
    q->ticks = ticks;
}
static APL void apSeqWrite(int frame) {
    FILE* f = fopen(CTRPORT_GAME_DIR "/asgseq.log", "a");
    if (f == nullptr) return;
    for (uint32_t i = 0; apSeqN > i; i++) {
        const ApSeq* q = &apSeqs[i];
        fprintf(f, "%d %lu %c %d %d %u %u %08lX %lu\n", frame, (unsigned long) i, q->kind, q->item, q->object, (unsigned) q->calls,
                (unsigned) q->vertices, (unsigned long) q->tex, (unsigned long) q->ticks);
    }
    fclose(f);
    apSeqN = 0;
}
#ifdef AP_LEAN
// (asgore30) The lean probe (the step-phase build): no draw-sequence dump or [asgtime] log, to fit the late reserve.
#define apSeqAdd(renderer, kind, item, ticks) ((void) 0)
#define apSeqWrite(frame) ((void) 0)
#endif
static APL void apFrameLine(Runner* runner, uint32_t drawTicks) {
    char why[200], top[160];
    int w = 0, m = 0;
    why[0] = top[0] = 0;
    for (int i = 0; 48 > i && w < (int) sizeof(why) - 16; i++) {
        uint32_t d = utaWhy[i] - apFWhy[i];
        if (d != 0) w += snprintf(why + w, sizeof(why) - (size_t) w, "%d:%lu,", i, (unsigned long) d);
        apFWhy[i] = utaWhy[i];
    }
    uint32_t ud[8];
    for (int i = 0; 8 > i; i++) ud[i] = utaCount[i] - apFUta[i], apFUta[i] = utaCount[i];
    for (int n = 0; 4 > n; n++) {
        int best = -1;
        uint32_t bt = 0;
        for (uint32_t j = 0; apFTouchedN > j; j++) {
            int it = apFTouched[j];
            if (apFTicks[it] > bt) bt = apFTicks[it], best = it;
        }
        if (best < 0) break;
        m += snprintf(top + m, sizeof(top) - (size_t) m, "%d:%lu/%u,", best, (unsigned long) bt, (unsigned) apFCalls[best]);
        apFTicks[best] = 0;
    }
    logInfo("[asgframe] f=%d spr=%lu step=%lu draw=%lu sprt=%lu uta=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu why=%s top=%s part=%lu/%lu/%lu/%lu rect=%lu/%lu asb=%lu/%lu/%lu/%lu\n",
            runner->frameCount, (unsigned long) apSprites, (unsigned long) apTimeStepNow, (unsigned long) drawTicks, (unsigned long) apFSprTicks,
            (unsigned long) ud[0], (unsigned long) ud[1], (unsigned long) ud[2], (unsigned long) ud[3], (unsigned long) ud[4],
            (unsigned long) ud[5], (unsigned long) ud[6], (unsigned long) ud[7], why, top, (unsigned long) apPartN, (unsigned long) apPartT,
            (unsigned long) apPartItem, (unsigned long) apPartPix, (unsigned long) apRectN, (unsigned long) apRectT,
            (unsigned long) asbProbe[0], (unsigned long) asbProbe[1], (unsigned long) asbProbe[2], (unsigned long) asbProbe[3]);
    apPartN = apPartT = apRectN = apRectT = apPartPix = 0;
    for (uint32_t j = 0; apFTouchedN > j; j++) apFTicks[apFTouched[j]] = 0, apFCalls[apFTouched[j]] = 0;
    apFTouchedN = 0;
    apFSprTicks = 0;
}
static APL void apTimeLog(void) {
    logInfo("[asgtime] frames=%llu sprites=%llu draw=%llu step=%llu\n", apTimeFrames, apTimeSprites, apTimeDraw, apTimeStep);
    logInfo("[asgtime] uta %lu %lu %lu %lu %lu %lu %lu %lu\n", (unsigned long) utaCount[0], (unsigned long) utaCount[1], (unsigned long) utaCount[2],
            (unsigned long) utaCount[3], (unsigned long) utaCount[4], (unsigned long) utaCount[5], (unsigned long) utaCount[6], (unsigned long) utaCount[7]);
    logInfo("[asgtime] shadow %lu %lu %lu %lu\n", (unsigned long) utaShadowCount[0], (unsigned long) utaShadowCount[1],
            (unsigned long) utaShadowCount[2], (unsigned long) utaShadowCount[3]);
    logInfo("[asgtime] parts %llu %llu %llu %llu %llu\n", utaTicks[1], utaTicks[2], utaTicks[3], utaTicks[4], utaTicks[5]);
    logInfo("[asgtime] asb native=%lu pre=%lu slot=%lu border=%lu\n", (unsigned long) asbProbe[0], (unsigned long) asbProbe[1],
            (unsigned long) asbProbe[2], (unsigned long) asbProbe[3]);
    char why[300];
    int w = 0;
    why[0] = 0;
    for (int i = 0; 48 > i && w < (int) sizeof(why) - 16; i++) {
        if (utaWhy[i] != 0) w += snprintf(why + w, sizeof(why) - (size_t) w, "%d:%lu,", i, (unsigned long) utaWhy[i]);
    }
    logInfo("[asgtime] why %s\n", why);
    uint8_t shown[AT_ITEMS / 8];
    memset(shown, 0, sizeof(shown));
    for (int n = 0; 16 > n; n++) {
        int best = -1;
        uint32_t bestTicks = 0;
        for (int i = 0; AT_ITEMS > i; i++) {
            uint32_t t = apTimeTicks[i][0] + apTimeTicks[i][1];
            if ((shown[i >> 3] & (1u << (i & 7))) == 0 && t > bestTicks) best = i, bestTicks = t;
        }
        if (best < 0) break;
        shown[best >> 3] |= (uint8_t) (1u << (best & 7));
        logInfo("[asgtime] item=%d in=%lu/%lu out=%lu/%lu\n", best, (unsigned long) apTimeTicks[best][0], (unsigned long) apTimeCalls[best][0],
                (unsigned long) apTimeTicks[best][1], (unsigned long) apTimeCalls[best][1]);
    }
}
#ifdef AP_LEAN
#define apTimeLog() ((void) 0)
#endif

static APL void apSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                         float yscale, float angleDeg, uint32_t color, float alpha) {
    apSprites++;
    if (apDump == 2) {
        const VMContext* ctx = apRunner != nullptr ? apRunner->vmContext : nullptr;
        const Instance* inst = ctx != nullptr ? ctx->currentInstance : nullptr;
        logInfo("[asgdump] %lu t=%ld x=%.3f y=%.3f o=%.1f,%.1f s=%.4f,%.4f a=%.2f c=%06lX al=%.3f ob=%ld id=%lu ev=%ld dp=%ld\n",
                (unsigned long) apSprites, (long) tpagIndex, (double) x, (double) y, (double) originX, (double) originY, (double) xscale,
                (double) yscale, (double) angleDeg, (unsigned long) color, (double) alpha, inst != nullptr ? (long) inst->objectIndex : -1L,
                inst != nullptr ? (unsigned long) inst->instanceId : 0UL, ctx != nullptr ? (long) ctx->currentEventType : -1L,
                apRunner != nullptr ? (long) apRunner->drawDepth : 0L);
    }
    if (apFrameLog > 0) {
        uint64_t t0 = svcGetSystemTick();
        apDrawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
        uint32_t dt = (uint32_t) (svcGetSystemTick() - t0);
        int item = 0 <= tpagIndex && AT_ITEMS > tpagIndex ? tpagIndex : AT_ITEMS - 1;
        if (apFCalls[item] == 0 && AT_ITEMS > apFTouchedN) apFTouched[apFTouchedN++] = (uint16_t) item;
        apFTicks[item] += dt;
        if (apFCalls[item] != 0xFFFF) apFCalls[item]++;
        apFSprTicks += dt;
        apSeqAdd(renderer, 'S', tpagIndex, dt);
        if (apTimeBusy) {
            int out = x < -16.0f || x > 656.0f || y < -16.0f || y > 496.0f;
            apTimeTicks[item][out] += dt;
            apTimeCalls[item][out]++;
        }
        return;
    }
    if (apTimeBusy) {
        uint64_t t0 = svcGetSystemTick();
        apDrawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
        uint32_t dt = (uint32_t) (svcGetSystemTick() - t0);
        int item = 0 <= tpagIndex && AT_ITEMS > tpagIndex ? tpagIndex : AT_ITEMS - 1;
        int out = x < -16.0f || x > 656.0f || y < -16.0f || y > 496.0f;
        apTimeTicks[item][out] += dt;
        apTimeCalls[item][out]++;
        return;
    }
    if (apSampling && (apCalls++ % AP_SAMPLE) == 0) {
        ApSnap a, b;
        apRead(&a);
        apDrawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
        apRead(&b);
        apInN++;
        apInCycles += b.cycles - a.cycles;
        apIn0 += b.e0 - a.e0;
        apIn1 += b.e1 - a.e1;
        return;
    }
    apDrawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}

static APL void apPart(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x, float y,
                       float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    uint64_t t0 = svcGetSystemTick();
    apDrawPart(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, alpha);
    uint32_t dt = (uint32_t) (svcGetSystemTick() - t0);
    apPartT += dt;
    apSeqAdd(renderer, 'P', tpagIndex, dt);
    apPartN++;
    apPartItem = (uint32_t) tpagIndex;
    if (srcW > 0.0f && srcH > 0.0f) apPartPix += (uint32_t) (srcW * srcH);
}

static APL void apRect(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    uint64_t t0 = svcGetSystemTick();
    apDrawRect(renderer, x1, y1, x2, y2, color, alpha, outline);
    uint32_t dt = (uint32_t) (svcGetSystemTick() - t0);
    apRectT += dt;
    apSeqAdd(renderer, 'R', -1, dt);
    apRectN++;
}

static APL void apLog(void) {
    for (int b = 0; AP_BUCKETS > b; b++) {
        for (int s = 0; AP_SETS > s; s++) {
            for (int k = 0; 2 > k; k++) {
                const ApSum* m = &apSums[b][s][k];
                if (m->frames == 0) continue;
                logInfo("[asgprobe] f=%lu b=%d s=%d k=%d n=%llu spr=%llu dc=%llu dm=%llu d0=%llu d1=%llu sc=%llu sm=%llu s0=%llu "
                        "s1=%llu in=%llu ic=%llu i0=%llu i1=%llu\n",
                        (unsigned long) apFrames, b, s, k, m->frames, m->sprites, m->drawCycles, m->drawMax, m->draw0, m->draw1,
                        m->stepCycles, m->stepMax, m->step0, m->step1, m->sampled, m->inCycles, m->in0, m->in1);
            }
        }
    }
}

// (asgore30, step lever) The step split up: runner.c's phase marks (RUNNER_PHASE, through runner.h's
// RUNNER_PHASE_PROBE) and its event runs (VM_beginEventRun to VM_endEventRun, linker-wrapped), each charged the
// frame's counter pair since the last mark. A run's time (the dispatch loop's per-instance checks and every event of
// the run) goes to its code's class, not to the phase; VM_runEventBound calls count the run's events. Summed per
// sprite bucket and counter set like the step above, logged every 300 frames as "[asgphase]" lines. Without counters
// (Azahar) the cycles are system ticks.
enum { PH_N = 14, CL_N = 4 };
static const char* apPhName[PH_N] APB;  // phase names by first sight; 0 is the step before the first mark
static uint32_t apPhNow[PH_N][3] APB, apClNow[CL_N][3] APB, apClEv[CL_N] APB, apClRun[CL_N] APB;
static uint8_t apPh APB, apCl APB, apInStep APB, apRunDepth APB;
static ApSnap apMark APB;
static uint32_t apPhSum[AP_BUCKETS][AP_SETS][PH_N][3] APB, apClSum[AP_BUCKETS][AP_SETS][CL_N][5] APB;  // may wrap: read deltas
static uint32_t apPhFrames[AP_BUCKETS][AP_SETS] APB;
// The classes, by the FNV-1a hash of their code entry's name (the kit names no game code): the helix bullet's Step, the
// ember's Step and the bullet parent's End Step; any other code is the last class.
static const uint32_t apClHash[CL_N - 1] __attribute__((section(".rodata.late.utasgore.probe"))) = {
    0x9369E845u, 0x0E782A8Au, 0x51DDBF66u};
static APL uint32_t apFnv(const char* s) {
    uint32_t h = 0x811C9DC5u;
    while (*s != 0) h = (h ^ (uint8_t) *s++) * 0x01000193u;
    return h;
}

static APL void apPhRead(ApSnap* s) {
    if (apState > 0) {
        apRead(s);
    } else {
        s->cycles = (uint32_t) svcGetSystemTick();
        s->e0 = s->e1 = 0;
    }
}

static APL void apCharge(uint32_t* to) {
    ApSnap s;
    apPhRead(&s);
    to[0] += s.cycles - apMark.cycles;
    to[1] += s.e0 - apMark.e0;
    to[2] += s.e1 - apMark.e1;
    apMark = s;
}

void Asgprobe_phase(const char* name) APL;
void Asgprobe_phase(const char* name) {
    if (!apInStep) return;
    int i = 1;
    while (PH_N - 1 > i && apPhName[i] != nullptr && apPhName[i] != name) i++;
    if (apPhName[i] == nullptr) apPhName[i] = name;
    apCharge(apPhNow[apPh]);
    apPh = (uint8_t) i;
}

void __real_VM_beginEventRun(VMContext* ctx, VMEventRun* run, int32_t codeIndex, int32_t eventType, int32_t eventSubtype, int32_t ownerObjectIndex);
void __real_VM_endEventRun(VMContext* ctx, VMEventRun* run);
void __real_VM_runEventBound(VMContext* ctx, VMEventRun* run, Instance* instance);
void __wrap_VM_beginEventRun(VMContext* ctx, VMEventRun* run, int32_t codeIndex, int32_t eventType, int32_t eventSubtype, int32_t ownerObjectIndex) APL;
void __wrap_VM_endEventRun(VMContext* ctx, VMEventRun* run) APL;
void __wrap_VM_runEventBound(VMContext* ctx, VMEventRun* run, Instance* instance) APL;
void __wrap_VM_beginEventRun(VMContext* ctx, VMEventRun* run, int32_t codeIndex, int32_t eventType, int32_t eventSubtype, int32_t ownerObjectIndex) {
    if (apInStep && apRunDepth++ == 0) {
        apCharge(apPhNow[apPh]);
        const char* name = ctx->dataWin->code.entries[codeIndex].name;
        const uint32_t h = name != nullptr ? apFnv(name) : 0;
        int c = 0;
        while (CL_N - 1 > c && (name == nullptr || h != apClHash[c])) c++;
        apCl = (uint8_t) c;
        apClRun[c]++;
    }
    __real_VM_beginEventRun(ctx, run, codeIndex, eventType, eventSubtype, ownerObjectIndex);
}
void __wrap_VM_endEventRun(VMContext* ctx, VMEventRun* run) {
    __real_VM_endEventRun(ctx, run);
    if (apInStep && apRunDepth != 0 && --apRunDepth == 0) {
        apCharge(apClNow[apCl]);
        apCl = CL_N;
    }
}
void __wrap_VM_runEventBound(VMContext* ctx, VMEventRun* run, Instance* instance) {
    if (CL_N > apCl) apClEv[apCl]++;
    __real_VM_runEventBound(ctx, run, instance);
}

// The frame's phase and run counters into its bucket's sums (from the draw wrapper, which knows the bucket).
static APL void apPhAdd(int bucket) {
    apPhFrames[bucket][apSet]++;
    for (int i = 0; PH_N > i; i++) {
        for (int j = 0; 3 > j; j++) apPhSum[bucket][apSet][i][j] += apPhNow[i][j];
    }
    for (int c = 0; CL_N > c; c++) {
        for (int j = 0; 3 > j; j++) apClSum[bucket][apSet][c][j] += apClNow[c][j];
        apClSum[bucket][apSet][c][3] += apClEv[c];
        apClSum[bucket][apSet][c][4] += apClRun[c];
    }
    memset(apPhNow, 0, sizeof(apPhNow));
    memset(apClNow, 0, sizeof(apClNow));
    memset(apClEv, 0, sizeof(apClEv));
    memset(apClRun, 0, sizeof(apClRun));
}

static APL void apPhLog(void) {
    char line[900];
    int w = 0;
    for (int i = 1; PH_N > i && apPhName[i] != nullptr; i++) {
        w += snprintf(line + w, sizeof(line) - (size_t) w, " %d=%s", i, apPhName[i]);
        if (w >= (int) sizeof(line) - 64) break;
    }
    logInfo("[asgphase] names%s\n", line);
    for (int b = 0; AP_BUCKETS > b; b++) {
        for (int s = 0; AP_SETS > s; s++) {
            if (apPhFrames[b][s] == 0) continue;
            w = 0;
            for (int i = 0; PH_N > i; i++) {
                const uint32_t* v = apPhSum[b][s][i];
                if (v[0] == 0) continue;
                w += snprintf(line + w, sizeof(line) - (size_t) w, " p%d=%lu/%lu/%lu", i, (unsigned long) v[0], (unsigned long) v[1], (unsigned long) v[2]);
                if (w >= (int) sizeof(line) - 64) break;
            }
            for (int c = 0; CL_N > c && (int) sizeof(line) - 96 > w; c++) {
                const uint32_t* v = apClSum[b][s][c];
                w += snprintf(line + w, sizeof(line) - (size_t) w, " c%d=%lu/%lu/%lu/%lu/%lu", c, (unsigned long) v[0], (unsigned long) v[1],
                              (unsigned long) v[2], (unsigned long) v[3], (unsigned long) v[4]);
            }
            logInfo("[asgphase] f=%lu b=%d s=%d n=%lu%s\n", (unsigned long) apFrames, b, s, (unsigned long) apPhFrames[b][s], line);
        }
    }
}

void __wrap_Runner_step(Runner* runner) {
    if (apState == 0) apStart();
    apSet = (uint8_t) (apFrames % AP_SETS);
    apSampling = (uint8_t) (apState > 0 && ((apFrames / AP_SETS) & 1) != 0);
    apSetEvents(apSet);
    ApSnap a, b;
    uint64_t stepT0 = svcGetSystemTick();
    apRead(&a);
    apPhRead(&apMark);
    apPh = 0;
    apCl = CL_N;
    apRunDepth = 0;
    apInStep = 1;
    AP_STEP(runner);
    apCharge(apPhNow[apPh]);
    apInStep = 0;
    apTimeStepNow = (uint32_t) (svcGetSystemTick() - stepT0);
    apRead(&b);
    apStepCycles = b.cycles - a.cycles;
    apStep0 = b.e0 - a.e0;
    apStep1 = b.e1 - a.e1;
}

void __wrap_Runner_drawViews(Runner* runner, int32_t gameW, int32_t gameH, bool debugShowCollisionMasks) {
    Renderer* renderer = runner->renderer;
    apRunner = runner;
    // (asgore30) The renderer's own table, at the bottom of every chain, is the one wrapped: smalltext's outer table
    // (installed at room changes in front of whatever table is there, and whose drawSprite forwards to the table it
    // copied) and a copy in front of it made each a wrapper of the other, which looped (stDrawSprite -> apSprite ->
    // stDrawSprite). Pixgrid_refs[0] is ctr_renderer.c's own vtable; a later write to its drawSprite is wrapped again.
    RendererVtable* own = (RendererVtable*) Pixgrid_refs[0];
    if (renderer != nullptr && own != nullptr && own->drawSprite != apSprite) {
        apDrawSprite = own->drawSprite;
        own->drawSprite = apSprite;
        apRenderer = renderer;
    }
    // The front table's own entries for these (the outer tables hold copies of the renderer's, and forward neither):
    // patched in place; a table later copied from it keeps the patch, and a new one is patched again.
    RendererVtable* front = renderer != nullptr ? (RendererVtable*) renderer->vtable : nullptr;
    if (front != nullptr && front->drawSpritePart != apPart) {
        apDrawPart = front->drawSpritePart;
        front->drawSpritePart = apPart;
    }
    if (front != nullptr && front->drawRectangle != apRect) {
        apDrawRect = front->drawRectangle;
        front->drawRectangle = apRect;
    }
    if (apDump == 0) {
        FILE* f = fopen(CTRPORT_GAME_DIR "/asgdump.txt", "r");
        apDump = f != nullptr ? 1 : -1;
        if (f != nullptr) fclose(f);
        f = fopen(CTRPORT_GAME_DIR "/asgtime.txt", "r");
        apTime = f != nullptr ? 1 : -1;
        if (f != nullptr) fclose(f);
        f = fopen(CTRPORT_GAME_DIR "/asgnoframes.txt", "r");
        apFrameLog = f != nullptr ? -1 : 1;  // (asgore30) on unless asgnoframes.txt: hardware rows can't place flag files
        if (f != nullptr) fclose(f);
        f = fopen(CTRPORT_GAME_DIR "/asgseq.txt", "r");
        apSeqOn = f != nullptr ? 1 : -1;
        if (f != nullptr) fclose(f);
        f = fopen(CTRPORT_GAME_DIR "/utaoff.txt", "r");
        utaOff = f != nullptr ? 1 : 0;
        if (f != nullptr) fclose(f);
        if (utaOff) logInfo("[asgtime] utaoff: every fine sprite goes the full way\n");
    } else if (apDump == 2) {
        apDump = -1;
    } else if (apDump == 1 && apDumpPrev >= 320) {
        apDump = 2;  // the frame after a busy one: likely busy too
    }
    apSprites = 0;
    apInN = apInCycles = apIn0 = apIn1 = 0;
    apTimeBusy = (uint8_t) (apTime > 0 && apDumpPrev >= 192);
    ApSnap a, b;
    uint64_t drawT0 = svcGetSystemTick();
    apRead(&a);
    int seqFrame = runner->frameCount;
    apSeqFrame = (uint8_t) (apSeqOn > 0 && (seqFrame == 370 || seqFrame == 560 || seqFrame == 698 || seqFrame == 699));
    apSeqN = 0;
    __real_Runner_drawViews(runner, gameW, gameH, debugShowCollisionMasks);
    apRead(&b);
    if (apSeqFrame) {
        apSeqFrame = 0;
        apSeqWrite(seqFrame);
    }
    if (apFrameLog > 0) apFrameLine(runner, (uint32_t) (svcGetSystemTick() - drawT0));
    if (apTimeBusy) {
        apTimeDraw += svcGetSystemTick() - drawT0;
        apTimeStep += apTimeStepNow;
        apTimeFrames++;
        apTimeSprites += apSprites;
    }
    apDumpPrev = apSprites;
    int bucket = apSprites >= 448 ? 4 : apSprites >= 320 ? 3 : apSprites >= 192 ? 2 : apSprites >= 64 ? 1 : 0;
    ApSum* s = &apSums[bucket][apSet][apSampling];
    uint32_t dc = b.cycles - a.cycles;
    s->frames++;
    s->sprites += apSprites;
    s->drawCycles += dc;
    if (dc > s->drawMax) s->drawMax = dc;
    s->draw0 += b.e0 - a.e0;
    s->draw1 += b.e1 - a.e1;
    s->stepCycles += apStepCycles;
    if (apStepCycles > s->stepMax) s->stepMax = apStepCycles;
    s->step0 += apStep0;
    s->step1 += apStep1;
    s->sampled += apInN;
    s->inCycles += apInCycles;
    s->in0 += apIn0;
    s->in1 += apIn1;
    apPhAdd(bucket);
    if (++apFrames % 300 == 0) {
        apLog();
        apPhLog();
        if (apTime > 0) apTimeLog();
    }
}
#endif
