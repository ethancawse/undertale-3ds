// postkick (change pacer, 2026-10-31; UNDERTALE only): the end-of-frame instance pass runs after the GPU has the frame.
//
// main.c ends each frame with UTSW_ROOMCHANGE (Utsw_handleRoomChange: the room-change hook, offkill's early destroy of
// bullets past the view (z_offkill.c), the swarm's window mark), then CtrRenderer_endFrame, whose C3D_FrameEnd hands
// the frame to the GPU. The GPU gets nothing before that, so every millisecond of CPU work before it delays the frame's
// buffer swap by as much. On a frame that changes no room, the hook's only work is offkill's pass over every instance,
// which nothing reads before the next Step (its destroys are taken out by that Step's cleanup, as the game's own
// destroys are): 0.3-0.5 ms on o2ds at Asgore's helix peaks (~300 instances; pacer and asgore30 gseq rows, the
// "room" column), the frames whose swap misses its vblank by 0.06-0.09 ms. So on such a frame the hook is held and
// run right after the real CtrRenderer_endFrame returns, with the GPU already at work: the same calls on the same
// state, in the same order against every other call of the frame except endFrame itself (the state hash ran before
// both). A frame that changes room, restarts it or restarts the game runs the hook where it was: it loads the room
// that endFrame's caller then prefetches.
//
// Hooks: Utsw_handleRoomChange is wrapped (CMakeLists.txt, UNDERTALE only), and CtrRenderer_endFrame's wrap chain
// (ctr_wrap_chain.h) gets a last link after nopromo's: the real endFrame, then the held hook. Input is read at the
// frame's start as before (no added input latency). All late (late_reserve.ld: .text.postkick_late and
// .bss.postkick_late, after evguard's, merge order), so nothing that existed moves; in main.c and z_freebies.c only
// one call target each changes.
#include "wad_versions.h"

#if defined(ENABLE_WAD16) && defined(__3DS__)

#include <stddef.h>

#include "ctr_renderer.h"
#include "ctr_wrap_chain.h"
#include "renderer.h"
#include "runner.h"

#define PK_TEXT __attribute__((noinline, section(".text.postkick_late")))

void __real_CtrRenderer_endFrame(Renderer* renderer);

#if defined(UTSW_ON)
// The runner whose hook is held for this frame's endFrame, or none.
static Runner* pkHeld __attribute__((section(".bss.postkick_late")));

void __real_Utsw_handleRoomChange(Runner* runner);
void __wrap_Utsw_handleRoomChange(Runner* runner) PK_TEXT;
void __wrap_Utsw_handleRoomChange(Runner* runner) {
    if (runner->pendingRoom != -1) {
        __real_Utsw_handleRoomChange(runner);
        return;
    }
    pkHeld = runner;
}
#endif

void CTR_ENDFRAME_POSTKICK(Renderer* renderer) PK_TEXT;
void CTR_ENDFRAME_POSTKICK(Renderer* renderer) {
    __real_CtrRenderer_endFrame(renderer);
#if defined(UTSW_ON)
    Runner* runner = pkHeld;
    if (runner == NULL) return;
    pkHeld = NULL;
    __real_Utsw_handleRoomChange(runner);
#endif
}

#endif
