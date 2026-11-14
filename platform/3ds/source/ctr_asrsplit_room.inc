// Only the shared UNDERTALE caller changes. DELTARUNE's existing caller and
// DR New-only ring/kick compatibility path stay unchanged.
#if defined(ENABLE_WAD16)
static void __attribute__((noinline, noipa, section(".text.late.asrsplit"))) asrCommandRoom(CtrRenderer* r) {
    if (gpuCmdBuf == nullptr || gpuCmdBufOffset + CMDBUF_RESERVE_WORDS + ASG_ROOM_WORDS <= gpuCmdBufSize) return;
    // makeCommandRoom's steps: go on at the ring's start when the frame before
    // is done with it and the GX queue has room (gxQueueNearlyFull, then
    // reserveGxCommands(r, 1), which only counts the command then), else run
    // the frame so far (runFrameSoFar).
    uint32_t used = r->gxFrame == r->frame ? r->gxCommands : 0;
    if (CtrGpu_queueCapacity() >= used + 1 + GX_QUEUE_RESERVE && CtrGpu_canWrapCommandBuffer(2 * CMDBUF_RESERVE_WORDS)) {
        // The query may have extended this span without permitting a wrap.
        // Like makeCommandRoom, use that room before splitting any commands.
        if (gpuCmdBufOffset + CMDBUF_RESERVE_WORDS + ASG_ROOM_WORDS <= gpuCmdBufSize) return;
        // An extension consumes the wrapper's one extension opportunity; this
        // second query now checks the ring's actual physical-wrap permission.
        if (!CtrGpu_canWrapCommandBuffer(2 * CMDBUF_RESERVE_WORDS)) goto runSoFar;
        flushVertexCache(r);
        r->clearedTarget = nullptr;
        if (r->gxFrame != r->frame) {
            r->gxFrame = r->frame;
            r->gxCommands = 0;
        }
        r->gxCommands++;
        C3D_FrameSplit(0);
        CtrGpu_wrapCommandBuffer();
        r->frameStats.splits++;
        r->boundTargetDrawn = false;
        r->drawnSinceSplit = false;
        for (uint32_t i = 0; r->surfaceCount > i; i++) r->surfaces[i].drawnSinceSplit = false;
        if (r->gameProjection) applyAllState(r); // resumeState
        if (gpuCmdBufOffset + CMDBUF_RESERVE_WORDS + ASG_ROOM_WORDS <= gpuCmdBufSize) return;
    }
runSoFar:
    ; // A label precedes a statement, before the C declaration.
    CtrSuspendedFrame suspended;
    endFrameEarly(r, &suspended);
    resumeFrame(r, &suspended);
}
#endif
