// Retain main 80f52b4840 GCC 16.1.0 Release input-section order.
// Empty declarations stop GCC call-graph ordering from moving other functions.
// Vacated diagnostic slots contain unused bytes; no fixed virtual addresses.
#if defined(ENABLE_WAD17) && defined(__3DS__) && defined(NDEBUG)
__asm__(
    ".pushsection .text.loadTicks,\"ax\",%progbits\n.balign 4\n"
    ".popsection\n"
    ".pushsection .text.exitAfterScreen,\"ax\",%progbits\n.balign 4\n"
    ".popsection\n"
    ".pushsection .text.logMemory,\"ax\",%progbits\n.balign 8\n"
    ".popsection\n"
    ".pushsection .text.dataVersionProgress,\"ax\",%progbits\n.balign 4\n"
    ".popsection\n"
    ".pushsection .text.loadAutoshotFrames,\"ax\",%progbits\n.balign 4\n"
    ".popsection\n"
    ".pushsection .text.osGetMemRegionSize,\"ax\",%progbits\n.balign 4\n"
    ".popsection\n"
    ".pushsection .text.setupNotices.part.0,\"ax\",%progbits\n.balign 4\n"
    ".popsection\n"
    ".pushsection .text.fatalScreen,\"axR\",%progbits\n.balign 8\n"
    ".space 284\n"
    ".popsection\n"
    ".pushsection .text.outOfMemoryScreen,\"axR\",%progbits\n.balign 8\n"
    ".space 512\n"
    ".popsection\n"
    ".pushsection .text.allocStringsBuffer,\"ax\",%progbits\n.balign 4\n"
    ".popsection\n"
    ".pushsection .text.__wrap_abort,\"ax\",%progbits\n.balign 4\n"
    ".popsection\n"
    ".pushsection .text.startup.main,\"ax\",%progbits\n.balign 8\n"
    ".popsection\n"
);
#endif
