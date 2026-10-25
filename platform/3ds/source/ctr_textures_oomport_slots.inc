// Main 06842c12, GCC 16.1.0 Release section extents in bytes.
// Subsection 1 follows the complete body and literal pool.
// The verifier must prove every tail is unreachable before --allow.
#if defined(__3DS__) && defined(NDEBUG) && !defined(CTRPORT_RENDER_TIMING) && !defined(CTRPORT_DEBUG_RECORD_FAIL_AFTER)
__attribute__((used, noipa, section(".text.late.oomport")))
static void oomport_ctr_textures_slots(void) {
    __asm__(
        ".pushsection .text.CtrTextures_ensureTilesBut.part.0, 1\n"
        "oomport_tail_CtrTextures_ensureTilesBut.part.0:\n"
        ".org 716\n"
        ".popsection\n"
        ".pushsection .text.finishRecording, 1\n"
        "oomport_tail_finishRecording:\n"
        ".org 640\n"
        ".popsection\n"
        ".pushsection .text.loadRoomMemory, 1\n"
        "oomport_tail_loadRoomMemory:\n"
        ".org 772\n"
        ".popsection\n"
    );
}
#endif
