/* Late sections and their fixed reserves, added to devkitARM's 3dsx.ld (CMakeLists.txt links with a copy of 3dsx.specs
   that reads this script first; ld then inserts each SECTIONS block where its INSERT says).

   Why: new code goes after the hot region so that the warm text keeps its addresses. Before
   this script, late code simply grew into the code segment's page padding, and the first change to outgrow the padding
   moved .rodata, .data and .bss by a page. Each segment now ends with a reserve of whole 4 KiB pages, so the next
   segment starts at a fixed distance from the frozen late sections, and late code, constants and data can grow by the
   reserve without moving anything that existed before (docs/3ds-hardware.md, "A fixed reserve after the late
   sections").

   Contract for a feature's late additions (<name> is the feature's name):
     code       __attribute__((noinline, section(".text.late.<name>")))
     constants  __attribute__((section(".rodata.late.<name>")))      (log formats: static const char arrays)
     data       __attribute__((section(".data.late.<name>")))        (initialised and writable; rarely needed)
     statics    __attribute__((section(".bss.late.<name>")))         (zero-initialised; after .bss, no reserve needed)
   Then append one line per kind to the lists below, at the end, in merge order. Appending never moves what is
   already there; the catch-all after each list (SORT_BY_NAME) only holds names not listed yet, which sort by name and
   so can move each other. This script is read before 3dsx.ld, so its patterns take these sections before 3dsx.ld's
   .text.*, .rodata.*, .data.* and .bss.* do.

   The frozen sections (.fatalflush, .vidfix, .heapfrag, .heapfrag_rodata) are listed in the order the linker placed
   them as orphans before the reserve existed (main 714417c9), so they keep their addresses. Don't add to them: the
   reserves are measured from their end, so growing one moves the next segment again. tools/analysis/check_layout.py
   compares a candidate ELF with its base and reports moves and the reserve left.

   UNDERTALE's retired AAC slots (aacout.cmake) are also available, at 0 B extra RAM. New inputs use
   .aacout.text.<name> (code or constants, never writable) or .aacout.rodata.<name> (constants only).
   Append the exact input name to aacout_text_inputs or aacout_rodata_inputs in aacout.cmake, in merge order;
   do not also add it to the ordinary lists below. Unlisted inputs and invalid flags fail the link. Slot ends
   stay fixed; the padding shrinks. Every earlier input keeps its address and size. Do not move existing late
   constants: a vacated fixed hole is not additional contiguous late-text tail. DELTARUNE keeps its AAC decoder
   and never uses these slots. */

SECTIONS
{
  /* ===== Code: frozen late sections, then each feature's late code, then the reserve up to the .rodata page. ===== */
  .fatalflush : { *(.fatalflush) }
  .vidfix : { *(.vidfix) }
  .heapfrag : { *(.heapfrag) }
  /* Each late section starts at the location counter (". :"), not at it rounded up to its strictest input's
     alignment: one 8-aligned input (a double literal) would otherwise move every earlier late function on a
     target whose late code starts 4 mod 8 (DELTARUNE o3DS, 0x308ffc). The 8-aligned input is padded where it lands
     (docs/3ds-hardware.md, "A 2**3 late input section used to shift every late function by +4 B"). */
  .late.text . :
  {
    __late_text_start = ABSOLUTE(.);
    /* Features, in merge order (append only): */
    *(.text.late.minigames)
    *(.text.late.smartbatch)
    *(.text.late.benchromfs)
    *(.text.late.pmengine)
    *(.text.late.sandbox)
    /* relprep's code (the Legal page viewer, ctr_legal.c's drawViewer and updateLegal, and CtrLegal_mountSelf) is
       cold and its size follows the release source URL (the builder's release profile, ruling 2026-10-29), so it
       sits at the end of late text (after cdbox, below): a URL change then moves no other code. In its old place, the
       smallest pad (0-31 B) that keeps every later late function on its stage72 / cand26 cache-line phase: CMake
       substitutes RELPREP_TEXT_PHASE, the address mod 32 that followed relprep in stage72 for this game and model. */
    __relprep_text_gap_start = ABSOLUTE(.);
    . += ((/* RELPREP_TEXT_PHASE */ 0 - __relprep_text_gap_start) & 31);
    __relprep_text_gap_end = ABSOLUTE(.);
    /* RELPREP_ROMFS_text */
    *(.text.late.loadtime)
    /* freebytes H1 hole (freebytes, 2026-10-31; UNDERTALE Old only): the late code above that never runs in a
       played frame is compiled for size there (CTR_COLD_OS), and the bytes that frees stay a hole in place, so
       nothing after it moves. Whole cold loading-screen functions fill the hole (z_loadscreen.c names them
       .text.h1hole.<hole> with CTR_H1_SEC, outside .text.late.* so no catch-all takes them first); a pad ends it
       on the stage address mod 4096 (CMake substitutes H1<HOLE>_TEXT_PHASE; other targets: the pad's own start,
       0 B), unless the hole starts past its fixed end (<HOLE>_TEXT_LIMIT: a test profile's extra late code; then
       0 B). The ASSERTs after this block stop a release link if any hole is overfilled. The other H1 holes have
       the same form. */
    *(.text.h1hole.loadtime)
    __h1loadtime_text_gap_start = ABSOLUTE(.);
    . += __h1loadtime_text_gap_start <= /* H1LOADTIME_TEXT_LIMIT */ __h1loadtime_text_gap_start
         ? ((/* H1LOADTIME_TEXT_PHASE */ 0 - __h1loadtime_text_gap_start) & 4095) : 0;
    __h1loadtime_text_gap_end = ABSOLUTE(.);
    *(EXCLUDE_FILE (*z_hitcheck.o) .text.late.colflip)
    /* colflip: collision_compat.c is all late code; this takes its out-of-line copies of header functions
       (rvalue.h's RValue_toRealOther and RValue_toInt32Other, renamed there), which can't carry the section attribute. */
    *collision_compat.o(.text .text.*)
    *(.text.late.heapfrag)
    /* Not listed yet: */
    *(.text.late.utpolish)
    *(.text.late.titanthrash .text.late.titanthrash.*)
    /* screens (after relprep): after every late input already linked, so that none of those moves. */
    *(.text.late.screens)
    /* screens: libctru's CFGU_GetModelNintendo2DS, first linked by it (the original 2DS's backlight), kept out of .text,
       where a library function new to the link would move every function after it. Then ctr_screens.c's constants:
       the late constants' reserve is too small for them on some targets. */
    *(.text.CFGU_GetModelNintendo2DS)
    *(.rodata.screens_in_text)
    /* colflip: UNDERTALE's collision rules (collision_gm14.c, all late code), after every late input
       already linked, then that file's out-of-line copies of header functions (none expected). */
    *(.text.late.colflip_gm14)
    *collision_gm14.o(.text .text.*)
    /* benchromfs (after colflip_gm14): the pre-window observer, after every late input already linked. */
    *(.text.late.benchromfs.prewindow)
    /* noports (after benchromfs): the native port slots' late code, after every late input already linked. */
    *(.text.late.noports)
    *(.text.late.noports3)
    /* tpaggrow (after noports): dynamic texture-page slots, after every late input already linked. */
    *(.text.late.tpaggrow)
    /* contention (after tpaggrow): the Old models' tile copies in VRAM. */
    *(.text.late.contention)
    /* texcache (after contention): packed-copy value ranking. */
    *(.text.late.texcache)
    /* knfuse (after texcache): Native fusion. */
    *(.text.late.knfuse)
    /* coverage (after knfuse): the RomFS bench supplement and the state dump's name helper. */
    *(.text.late.coverage)
    *(.text.h1hole.coverage)
    __h1coverage_text_gap_start = ABSOLUTE(.);
    . += __h1coverage_text_gap_start <= /* H1COVERAGE_TEXT_LIMIT */ __h1coverage_text_gap_start
         ? ((/* H1COVERAGE_TEXT_PHASE */ 0 - __h1coverage_text_gap_start) & 4095) : 0;
    __h1coverage_text_gap_end = ABSOLUTE(.);
    /* o3titan8 (after coverage): reserved for its opt-in phase layout; empty in default builds. */
    *(.text.late.o3titan8)
    /* lockfix (after o3titan8): empty selectors (main's titanthrash 210e822d carries the fix). */
    *(.text.late.lockfix)
    /* typed3 (after lockfix): typed AOT helpers and binding validation. */
    *(.text.late.typed3)
    /* texcache-f1 (after typed3): Smart smoothing's first-look scans. */
    *(.text.late.texcachef1)
    /* alarm14 (after texcache-f1): UNDERTALE's alarm setter (alarm_gm14.c, all late code), then that
       file's out-of-line copies of rvalue.h's conversions. */
    *(.text.late.alarm14)
    *alarm_gm14.o(.text .text.*)
    /* uthud (pixfix A, after alarm14; utgrid reverted): view_snap, UNDERTALE only. */
    *(.text.late.uthud)
    /* utline (after uthud): utpolish's whole-texel thin lines (the Undyne shield). */
    *(.text.late.utpolish.line)
    /* pixfix (task D, DELTARUNE only): after utline, so that none of those moves. */
    *(.text.late.pixfix)
    /* exithang (after pixfix D): bounded exit, deadline and exit log (ctr_exit.c, ctr_exit_log.c). */
    *(.text.late.exithang)
    /* colline (colflip follow-up check, slim; after exithang): UNDERTALE's line, circle, distance and colour builtins (colline_gm14.c, all late code),
       after every late input already linked, then that file's out-of-line copies of rvalue.h's conversions. */
    *(.text.late.colline_gm14)
    *colline_gm14.o(.text .text.*)
    /* menupause (after colline): the options menu holds the audio (ctr_audio_system.c, ctr_video.c).
       Its backlight and screenshot code in ctr_screens.c goes in that file's .late.screens sections. */
    *(.text.late.menupause)
    /* utaudio (after menupause): ctr_audio_system.c's late code, then its constants (the late constants'
       reserve is too small on UNDERTALE New). */
    *(.text.late.utaudio)
    *(.rodata.utaudio_in_text)
    /* ringstart (after utaudio): contention's command ring start reuse and run-on (ctr_gpu_queue.c). */
    *(.text.late.ringstart)
    /* pixgrid (pixfix task F, UNDERTALE only; after ringstart), so that none of those moves. Then its
       constants (those in ctr_pixgrid.c and the tables at the end of ctr_renderer.c), as screens' are: UNDERTALE
       New's late constants page is nearly full. */
    *(.text.late.pixgrid)
    *(.rodata.pixgrid_in_text)
    /* btnprompt (after pixgrid): UNDERTALE's console prompts, then their constants (z_btnprompt.c and
       z_btnprompt_ctr.c, so the late constants' reserve takes nothing), then those files' out-of-line copies of
       header functions and literals (none expected). */
    *(.text.late.btnprompt)
    *(.rodata.btnprompt_in_text)
    *z_btnprompt*(.text .text.RValue_* .text.DataWin_* .rodata .rodata.str*)
    /* argui (after btnprompt): options menu, first-boot hint and FPS counter (ctr_argui.c), then its
       constants: the late constants' reserve would keep 703 B on UNDERTALE New with them (new late
       constants go in the late text block). */
    *(.text.late.argui)
    *(.rodata.late.argui)
    /* utqa (UT 1.0 QA; after argui): outline rectangles' edges in whole texels at half resolution. */
    *(.text.late.utqa)
    *(.rodata.utqa_in_text)
    /* exithang follow-up (after utqa): bounded diagnostic file delivery, then its 12 B table
       (CtrExit_filesLinkState; new late constants go in the late text block). */
    *(.text.late.exithang.files)
    *(.rodata.late.exithang.files)
    /* chapterleak (after the exithang follow-up): the chapter switch gives back the renderer's effect
       textures (z_chapterleak.c, ctr_renderer.c's tail). */
    *(.text.late.chapterleak)
    /* texevict (after chapterleak): keepLinearFloor's one-tile eviction. */
    *(.text.late.texevict)
    /* pixpress (pixfix task E, DELTARUNE only): at the end of the list, so that none of those moves; its
       constants after it. */
    *(.text.late.pixpress)
    *(.rodata.pixpress_in_text)
    /* pixnslice (pixfix task C, DELTARUNE only): after pixpress, so that none of those moves; its constants after it. */
    *(.text.late.pixnslice)
    *(.rodata.pixnslice_in_text)
    /* utaudio (after pixnslice): the declick and loop-seam code (ctr_audio_system.c, ctr_audio_utaudio.h's
       UTA_LATE), then its constants (new late constants go in the late text block). The earlier
       utaNoticeMissing stays alone in .text.late.utaudio above, so nothing linked after it moves. */
    *(.text.late.utaudio2)
    *(.rodata.late.utaudio)
    /* kawkaw (after utaudio): palette copies lend from tiles no one wanted since the room began. */
    *(.text.late.kawkaw)
    /* thrashport (after kawkaw): Old palette-copy retries (ctr_renderer_palette_retry.inc). */
    *(.text.late.thrashport)
    /* exithang mid-file follow-up (after thrashport): GSP stack and bounded TCP tail drain. */
    *(.text.late.exithang.midfile)
    /* Only the newly referenced public wrappers: soc_recvfrom.o's internal
       SOC helpers already exist in the baseline and keep their slots. */
    *libctru.a:soc_recv.o(.text.recv)
    *libctru.a:soc_recvfrom.o(.text.recvfrom .rodata.recvfrom.str1.4)
    /* layerfix (last): layer Draw mutation helpers (runner.c, vm_builtins.c). */
    *(.text.late.layerfix)
    /* argtower (after layerfix): Old-model Flowery tower faces (ctr_renderer.c, main.c). */
    *(.text.late.perfflowery)
    /* vramtouch (after argtower): Azahar only, every new VRAM block touched (ctr_vram_touch.c). */
    *(.text.late.vramtouch.*)
    /* texprep (after vramtouch): the page header filled with its padding cleared (ctr_texture_pack.c). */
    *(.text.late.texprep)
    /* nativemac (relprep; after texprep): UNDERTALE's macOS data (ctr_data_version.c's later rows and
       their lookup, ctr_pixgrid.c's second key), then its constants (in the late text block, as utaudio's). */
    *(.text.late.nativemac)
    *(.rodata.nativemac_in_text)
    /* argcrash (after nativemac): friendly crash screen and release-quiet logging (z_argcrash.c). */
    *(.text.late.argcrash)
    *(.rodata.argcrash_in_text)
    /* engfix (after argcrash): game_end closes the game (main.c) and the spare-texture GPU wait (ctr_renderer.c, ctr_gpu_queue.c). */
    *(.text.late.engfix)
    /* optmenu (after engfix): the release options menu (ctr_optmenu.c), then its constants. */
    *(.text.late.optmenu)
    *(.rodata.late.optmenu)
    *(.text.late.coverage.rooms)
    /* pillar (after coverage): exact fountain mesh and its constants, DELTARUNE only. */
    *(.text.late.pillar)
    *(.rodata.pillar_in_text)
    /* loadscreen (after pillar): the loading screens' thread, drawing and load wraps (z_loadscreen.c). */
    *(.text.late.loadscreen)
    *(.rodata.loadscreen_in_text)
    /* roomplan (after loadscreen): the room plan (z_roomplan.c), its constants after it. */
    *(.text.late.roomplan)
    *(.rodata.roomplan_in_text)
    /* freebytes H1 (UNDERTALE Old only): the loading screen above gave M B of whole functions to the H1 holes, so
       this pad of M - 4096 B (M mod 4096, on the stage address mod 4096) moves all the late code below by exactly
       -4096 B: one whole I-cache set period, so every function below keeps its cache sets. Other targets: 0 B. */
    __h1rend_text_gap_start = ABSOLUTE(.);
    . += __h1rend_text_gap_start <= /* H1REND_TEXT_LIMIT */ __h1rend_text_gap_start
         ? ((/* H1REND_TEXT_PHASE */ 0 - __h1rend_text_gap_start) & 4095) : 0;
    __h1rend_text_gap_end = ABSOLUTE(.);
    /* engfix item 6 (after roomplan): the Flowery dash floor copy (ctr_renderer.c), DELTARUNE only. */
    *(.text.late.engfix6)
    /* roomload (after engfix item 6): the room change finishes the room's code and tile reads (z_roomload.c,
       ctr_textures.c's tail). */
    *(.text.late.roomload)
    /* roomload: its constants (z_roomload.c), here because the late constants' reserve is short on UNDERTALE New. */
    *(.rodata.roomload_in_text)
    /* cmdlabel (after roomload; DELTARUNE only): the command-label prototype, then its constants. */
    *(.text.late.cmdlabel)
    *(.rodata.cmdlabel_in_text)
    /* o2crash (after cmdlabel): the lab guard's sleep, parked when the heap is gone (ctr_lab.c). */
    *(.text.late.o2crash)
    /* rpfix (after o2crash): the rooms.dtm filter by a room index (z_roomplan.c's end), its constants after it. */
    *(.text.late.rpfix)
    *(.rodata.rpfix_in_text)
    /* pillarchk (after rpfix, merge order): the fountain's per-cell second-sample transform (ctr_pillar_exact.inc), DELTARUNE
       only. */
    *(.text.late.pillarchk)
    /* boxedge (after pillarchk, merge order): thin bars drawn whole texels across at half resolution (ctr_boxedge.c, then its
       layout table at the end of ctr_renderer.c). */
    *(.text.late.boxedge)
    *(.rodata.boxedge_in_text)
    /* dogcheck (after boxedge, merge order; DELTARUNE only): the loading screen's dog on its rope (z_loadscreen.c). */
    *(.text.late.dogcheck)
    /* drold (G9, after dogcheck's lines, merge order): escape elision's materializer (vm_aot_escape.c), DELTARUNE only. */
    *(.text.late.drold)
    /* drold: the out-of-line copies GCC makes of header inlines for those cold functions (.text.unlikely.*) and
       their strings, so that nothing in the main text or the string pools moves. */
    *vm_aot_escape.o(.text .text.* .rodata .rodata.*)
    /* natsurf6 (after drold, merge order): native-scale game surfaces beyond the application surface's size (ctr_natsurf6.c). */
    *(.text.late.natsurf6)
    /* natsurf6: its constants (ctr_natsurf6.c, ctr_renderer.c's Natsurf6_refs/layout), here because the late constants'
       reserve is short on UNDERTALE Old (138 B left before natsurf6). */
    *(.rodata.natsurf6_in_text)
    /* exportclean (after natsurf6, merge order): UNDERTALE's prompt string finder (z_btnprompt.c), its 260 B
       slot kept in btnprompt's region. */
    *(.text.late.exportclean)
    /* asgdraw (after exportclean, merge order): runs of fine draws held as one (ctr_renderer.c's tail). */
    *(.text.late.asgdraw)
    /* hammer601 (after asgdraw, merge order): the never-called function that pads loadRequestedFrom's slot (ctr_textures.c); the pad
       lands in loadRequestedFrom's own section, and --gc-sections drops this 4-byte function. */
    *(.text.late.hammer601)
    /* flowergpu (after hammer601, merge order): DELTARUNE New's exact tower face retention (vine_new_retain.inc). */
    *(.text.late.flowergpu)
    /* bggrid (after flowergpu, merge order; DELTARUNE only): the battle grid between output pixels, then its constants. */
    *(.text.late.bggrid)
    *(.rodata.bggrid_in_text)
    /* asgroom (after bggrid, merge order; both games): room for a whole fine sprite before it
       starts (ctr_renderer.c's asgRoomSlow). */
    *(.text.late.asgroom)
    /* utstall (after asgroom, merge order; UNDERTALE only): UNDERTALE's battle and later plan
       tiles in the hidden room load (z_utstall.c, z_roomplan.c's end), then its constants. */
    *(.text.late.utstall)
    *(.text.h1hole.utstall)
    __h1utstall_text_gap_start = ABSOLUTE(.);
    . += __h1utstall_text_gap_start <= /* H1UTSTALL_TEXT_LIMIT */ __h1utstall_text_gap_start
         ? ((/* H1UTSTALL_TEXT_PHASE */ 0 - __h1utstall_text_gap_start) & 4095) : 0;
    __h1utstall_text_gap_end = ABSOLUTE(.);
    *(.rodata.utstall_in_text)
    /* planrec (after utstall; room plan recorder builds only): RoomPlan_recordCode. */
    *(.text.late.planrec)
    /* aotclean (G1, frozen after the late text before it): DELTARUNE's AOT host table
       (aot_hostsyms.cmake), 128 words whatever its count, so that a new host import moves nothing. */
    *(.rodata.late.aotclean)
    /* cut10 (after the frozen host table): defaults and legacy override normalization.
       Appending here keeps the existing host-table data at its pinned address. */
    *(.text.late.cut10)
    /* panelgrid (cmdlabel, after cut10 and so after the frozen host table; DELTARUNE only,
       ctr_panelgrid.c): the battle panel on the pixel grid, then its constants. */
    *(.text.late.panelgrid)
    *(.rodata.panelgrid_in_text)
    /* benchfid (after panelgrid, so after the frozen host table): the bench
       window hook, the hidden room-change frame reporter, then its constants. */
    *(.text.late.benchfid)
    *(.text.late.benchfid.hidden)
    *(.text.late.benchfid.hidden.constants)
    /* crispart (after benchfid, so after the frozen host table; DELTARUNE
       only): approved native art pack/runtime (z_crispart.c and ctr_renderer.c's accessors); constants follow
       its code in the text reserve, as cmdlabel. */
    *(.text.late.crispart)
    *(.rodata.late.crispart)
    /* First linked by crispart's private-atlas destruction; keep this library entry out of the frozen .text. */
    *ctr_gpu_queue.o(.text.C3D_FrameSync)
    /* smartcut (after crispart, so after the frozen host table; both games): Smart smoothing's
       boxes only where read (ctr_renderer.c's and ctr_benchmark.c's tails), then its constants (in the late text
       block, as on its branch). */
    *(.text.late.smartcut)
    *(.rodata.smartcut_in_text)
    /* inputlate (after smartcut, so after the frozen host table; UNDERTALE only): the HID
       latch (ctr_input.c, z_inputlate.c), then its constants. */
    *(.text.late.inputlate)
    *(.rodata.inputlate_in_text)
    /* smalltext (after inputlate, so after the frozen host table; UNDERTALE only): Crisp small
       text's opt-in test keys (z_smalltext.c, all late code), then its constants and the table at ctr_renderer.c's
       end, then that file's out-of-line copies of header functions. */
    *(.text.late.smalltext)
    *(.rodata.smalltext_in_text)
    /* freebytes r5b phase pad (dec-1029-phase-pads, c26rev r5 F1): UNDERTALE reads settings.ini's path from z_loadscreen.c, so z_smalltext.o's constants are shorter;
       this pad keeps its out-of-line header copies (TextUtils_decodeUtf8) on stage75zd's phase. CMake substitutes
       SMALLTEXTRO_TEXT_PHASE (stage75zd's address mod 32 here; DELTARUNE: the pad's own start, 0 B). */
    __smalltextro_text_gap_start = ABSOLUTE(.);
    . += ((/* SMALLTEXTRO_TEXT_PHASE */ 0 - __smalltextro_text_gap_start) & 31);
    __smalltextro_text_gap_end = ABSOLUTE(.);
    *z_smalltext.o(.text .text.* .rodata .rodata.*)
    /* Phase pad (as utflames', below): SMALLTEXT_TEXT_PHASE, stage72's address mod 32 after smalltext's group, so
       growth there (flowend's atlas) does not re-phase pacer, oomport and the utflames group. */
    __smalltext_text_gap_start = ABSOLUTE(.);
    . += ((/* SMALLTEXT_TEXT_PHASE */ 0 - __smalltext_text_gap_start) & 31);
    __smalltext_text_gap_end = ABSOLUTE(.);
    /* pacer (after smalltext, so after the frozen host table; both games): log-only bench
       timing (ctr_bench_io.c) and the FPS marker (ctr_fps_clock.c), then their constants. */
    *(.text.late.pacer)
    *(.rodata.pacer_in_text)
    *(.text.late.pacer_io)
    *(.rodata.pacer_io_in_text)
    /* Defensive placement for any new readonly input. The policy's direct
       volatile-source copy also avoids mergeable string pools: their suffix
       merging can remove old strings elsewhere in this same late block. */
    *ctr_bench_io.o(.rodata .rodata.*)
    *(.text.late.pacer_vc)
    *(.rodata.pacer_vc_in_text)
    *ctr_fps_clock.o(.rodata .rodata.*)
    /* oomport (after pacer; both games): checked optional renderer/history arrays. */
    *(.text.late.oomport)
    /* utflames (after oomport; UNDERTALE only): the fire bullets' fast paths (z_utflames.c) and
       the generated functions' late copies that use them (gmlaot.py's aotu_<n>), then z_utflames.c's out-of-line
       copies of header functions (none expected). */
    *(EXCLUDE_FILE (*aot_table.o) .text.late.utflames)
    /* Phase pad (as utflames', below): UTFAOT_TEXT_PHASE, stage72's address mod 32 after z_utflames.o's fast paths,
       so their growth (UT-039) does not re-phase the generated aotu_<n> copies after them (same order as before). */
    __utfaot_text_gap_start = ABSOLUTE(.);
    . += ((/* UTFAOT_TEXT_PHASE */ 0 - __utfaot_text_gap_start) & 31);
    __utfaot_text_gap_end = ABSOLUTE(.);
    *(.text.late.utflames)
    *z_utflames.o(.text .text.* .rodata .rodata.*)
    /* Phase pad (integrate21 stage73, ruling 2026-10-29): the smallest pad (0-31 B) that puts the next late
       function on its stage72 / cand26 cache-line phase, so growth of utflames above (UT-039) does not re-phase the
       hot code after it. CMake substitutes UTFLAMES_TEXT_PHASE (stage72's address mod 32 here, per model; DELTARUNE:
       the start itself, a 0 B pad). See docs/3ds-hardware.md, "Late-code phase pads". */
    __utflames_text_gap_start = ABSOLUTE(.);
    . += ((/* UTFLAMES_TEXT_PHASE */ 0 - __utflames_text_gap_start) & 31);
    __utflames_text_gap_end = ABSOLUTE(.);
    /* utflowey (after utflames; both games): the room load's swap of older rooms' tiles
       (ctr_textures.c's end), then its constants. */
    *(.text.late.utflowey)
    *(.rodata.utflowey_in_text)
    /* utflowey: UNDERTALE's late copies of generated functions (gmlaot.py's LATE_COPIES), the whole file with its
       constants and any out-of-line copies of header functions; DELTARUNE's AOT has no aot_late.c. */
    /* freebytes r5b phase pads inside aot_late.o: its conversion helpers first; then aotSlotFind,
       requireMessageFormatted and the store helper, in stage75zd's order, on stage75zd's phase of its aotSlotFind
       (AOTLHELP_TEXT_PHASE); then the generated copies (aotl_<n>) and the constants, aotl_31 on stage75zd's phase
       (AOTLGEN_TEXT_PHASE), the constants last. The same in the cold-only build, where aot_late.o keeps its own
       copies. DELTARUNE: 0 B. */
    *aot_late.o(.text.RValue_* .text.aotToInt32Slow .text.AotLate_toInt32Slow)
    *(.text.h1hole.aotlhelp)
    __aotlhelp_text_gap_start = ABSOLUTE(.);
    . += __aotlhelp_text_gap_start <= /* AOTLHELP_TEXT_LIMIT */ __aotlhelp_text_gap_start
         ? ((/* AOTLHELP_TEXT_PHASE */ 0 - __aotlhelp_text_gap_start) & /* AOTLHELP_TEXT_MASK */ 31) : 0;
    __aotlhelp_text_gap_end = ABSOLUTE(.);
    *aot_late.o(.text.aotSlotFind* .text.requireMessageFormatted* .text.aotStoreSlow .text.AotLate_storeSlow)
    *(.text.h1hole.aotlgen)
    __aotlgen_text_gap_start = ABSOLUTE(.);
    . += __aotlgen_text_gap_start <= /* AOTLGEN_TEXT_LIMIT */ __aotlgen_text_gap_start
         ? ((/* AOTLGEN_TEXT_PHASE */ 0 - __aotlgen_text_gap_start) & /* AOTLGEN_TEXT_MASK */ 31) : 0;
    __aotlgen_text_gap_end = ABSOLUTE(.);
    *aot_late.o(.text .text.*)
    *aot_late.o(.rodata .rodata.*)
    /* freebytes r5b phase pad (dec-1029-phase-pads, c26rev r5 F1): aot_late.o now holds the one aotStoreSlow and drops its own conversion copies, so the
       utflowey, krsharp and utsfx code after it (Smalltext_drawTexture) keeps stage75zd's phase. CMake substitutes
       AOTLATE_TEXT_PHASE (stage75zd's address mod 32 here; DELTARUNE: 0 B). */
    *(.text.h1hole.aotlate)
    __aotlate_text_gap_start = ABSOLUTE(.);
    . += __aotlate_text_gap_start <= /* AOTLATE_TEXT_LIMIT */ __aotlate_text_gap_start
         ? ((/* AOTLATE_TEXT_PHASE */ 0 - __aotlate_text_gap_start) & /* AOTLATE_TEXT_MASK */ 31) : 0;
    __aotlate_text_gap_end = ABSOLUTE(.);
    /* utflowey iteration 3 (UNDERTALE only): Photoshop Flowey's pipe Draw body (z_utflowey.c) and the generated
       functions' wrappers that call it (gmlaot.py's aotu_<n>), their constants, then z_utflowey.c's out-of-line
       copies of header functions (none expected). */
    *(.text.late.utflowey3)
    *(.rodata.utflowey3_in_text)
    *z_utflowey.o(.text .text.* .rodata .rodata.*)
    /* krsharp (smoothux, after utflowey; UNDERTALE only): the KR label kept sharp
       (ctr_krsharp.c), then its constants. */
    *(.text.late.krsharp)
    *(.rodata.krsharp_in_text)
    /* utsfx (after dlgvid; UNDERTALE only): the sound plan (z_utsfx.c, ctr_audio_system.c's
       tail), its constants after it. */
    *(.text.late.utsfx)
    *(.rodata.utsfx_in_text)
    /* freebytes r5b phase pad (dec-1029-phase-pads, c26rev r5 F1): z_utsfx.c uses z_roomplan.c's strings, so the shimmer, utkill and utdust code after it keeps
       stage75zd's phase. CMake substitutes UTSFX_TEXT_PHASE (stage75zd's address mod 32 here; DELTARUNE: 0 B). */
    __utsfx_text_gap_start = ABSOLUTE(.);
    . += ((/* UTSFX_TEXT_PHASE */ 0 - __utsfx_text_gap_start) & 31);
    __utsfx_text_gap_end = ABSOLUTE(.);
    /* utflames heat shimmer (after utsfx; UNDERTALE only): the shimmer script's builtin
       (z_utflames_waver.c), then its constants. */
    *(.text.late.utflames.waver)
    *z_utflames_waver.o(.text .text.* .rodata .rodata.*)
    /* utkill (after the utflames shimmer; UNDERTALE only): plain sprite draws (ctr_renderer.c's
       tail, utkDrawSprite). Then utdust: destroyed instances leave the object lists in batches, and gravity keeps
       its cosine and sine (z_utdust.c; no constants, and anything else of the file lands here too). */
    *(.text.late.utkill)
    /* freebytes round 4 phase pad (lead ruling 2026-10-29, as utflames'): UNDERTALE Old's StateHash_runDraw
       (state_hash.c, the last input above) is smaller, so the smallest pad (0-31 B) keeps the late code after it on
       its stage72 cache-line phase. CMake substitutes STATEHASH_TEXT_PHASE (stage72's address mod 32 here; other
       targets: the pad's own start, a 0 B pad). */
    __statehash_text_gap_start = ABSOLUTE(.);
    . += ((/* STATEHASH_TEXT_PHASE */ 0 - __statehash_text_gap_start) & 31);
    __statehash_text_gap_end = ABSOLUTE(.);
    *(.text.late.utdust)
    *z_utdust*(.text .text.* .rodata .rodata.*)
    /* utflames falling water (integrate round 16, the round's first late text; UNDERTALE only): the generated
       wrapper and the native body of the foreground strips' Draw (z_utflames_wfall.c), then its constants. */
    *(.text.late.utflames.wfall)
    *z_utflames_wfall.o(.text .text.* .rodata .rodata.*)
    /* utold lava (integrate round 16, after utflames' falling water; UNDERTALE only): the lava2 script's fast path,
       then the generated numeric caller and regions with their header copies and constants (aot_utold_*.c;
       DELTARUNE emits no such objects). */
    *(.text.late.utlava)
    /* freebytes r5b phase pad (dec-1029-phase-pads, c26rev r5 F1): the lava waver's generated loop (aotul_<n>) first, on stage75zd's phase, then the rest of
       aot_utold_loop.o (its own helper copies went to aot_late.o); then the regions file on its phase. CMake
       substitutes UTLAVA_TEXT_PHASE and UTOLDREG_TEXT_PHASE (stage75zd's addresses mod 32 of aotul_11 and
       aotult_11_00B8; DELTARUNE emits neither file: 0 B). */
    *(.text.h1hole.utlava)
    __utlava_text_gap_start = ABSOLUTE(.);
    . += __utlava_text_gap_start <= /* UTLAVA_TEXT_LIMIT */ __utlava_text_gap_start
         ? ((/* UTLAVA_TEXT_PHASE */ 0 - __utlava_text_gap_start) & /* UTLAVA_TEXT_MASK */ 31) : 0;
    __utlava_text_gap_end = ABSOLUTE(.);
    *aot_utold_loop.o(.text.aotul_*)
    /* freebytes r5b: aot_utold_loop.o's conversion helpers (cold-only build), then its own aotSlotFind copy and what
       follows it on stage75zd's phase (UTLHELP_TEXT_PHASE; DELTARUNE: 0 B), the constants last. */
    *aot_utold_loop.o(.text.RValue_* .text.aotToInt32Slow)
    *(.text.h1hole.utlhelp)
    __utlhelp_text_gap_start = ABSOLUTE(.);
    . += __utlhelp_text_gap_start <= /* UTLHELP_TEXT_LIMIT */ __utlhelp_text_gap_start
         ? ((/* UTLHELP_TEXT_PHASE */ 0 - __utlhelp_text_gap_start) & /* UTLHELP_TEXT_MASK */ 31) : 0;
    __utlhelp_text_gap_end = ABSOLUTE(.);
    *aot_utold_loop.o(.text .text.*)
    *(.text.h1hole.utloop)
    __h1utloop_text_gap_start = ABSOLUTE(.);
    . += __h1utloop_text_gap_start <= /* H1UTLOOP_TEXT_LIMIT */ __h1utloop_text_gap_start
         ? ((/* H1UTLOOP_TEXT_PHASE */ 0 - __h1utloop_text_gap_start) & 4095) : 0;
    __h1utloop_text_gap_end = ABSOLUTE(.);
    *aot_utold_loop.o(.rodata .rodata.*)
    *(.text.h1hole.utoldreg)
    __utoldreg_text_gap_start = ABSOLUTE(.);
    . += __utoldreg_text_gap_start <= /* UTOLDREG_TEXT_LIMIT */ __utoldreg_text_gap_start
         ? ((/* UTOLDREG_TEXT_PHASE */ 0 - __utoldreg_text_gap_start) & /* UTOLDREG_TEXT_MASK */ 31) : 0;
    __utoldreg_text_gap_end = ABSOLUTE(.);
    *aot_utold_*.o(.text .text.* .rodata .rodata.*)
    /* utkill vapor (integrate round 16, after utold's lava; UNDERTALE only): the monster dust's native Draw and
       pixel Create (z_utkill_vapor.c), with its strings and anything else of the file. */
    *(.text.late.utkill.vapor)
    *z_utkill_vapor*(.text .text.* .rodata .rodata.*)
    /* utkill swarm (after utkill vapor): the monster dust as a swarm (z_utkill_swarm.c, runner.c's draw list rebuild
       for it; UNDERTALE only), with its strings and anything else of the file. */
    *(.text.late.utswarm)
    *z_utkill_swarm*(.text .text.* .rodata .rodata.*)
    /* utkill waver (after the swarm; UNDERTALE only): the waving rows script's builtin (z_utkill_waver.c), then its
       constants. */
    *(.text.late.utkill.waver)
    *(.text.late.utkill.waverbatch)
    *z_utkill_waver.o(.text .text.* .rodata .rodata.*)
    /* bootlog (after utkill waver, before the catch-all; both games): the boot's log unseen unless Other
       screen is Stats and log (main.c's tail), then its constants. */
    *(.text.late.bootlog)
    *(.rodata.bootlog_in_text)
    /* freebytes round 4 phase pad (as statehash's above): UNDERTALE Old compiles two once-a-run bootlog functions
       for size, and the boot-log fix (10-29) changed the group on every target (four functions, for size everywhere). CMake substitutes BOOTLOG_TEXT_PHASE
       (stage73's address mod 32 here, per game and model). */
    __bootlog_text_gap_start = ABSOLUTE(.);
    . += ((/* BOOTLOG_TEXT_PHASE */ 0 - __bootlog_text_gap_start) & 31);
    __bootlog_text_gap_end = ABSOLUTE(.);
    /* utasgore (round 17, after bootlog, before the catch-all; UNDERTALE only): fine sprites outside the view and in
       steady held runs (ctr_renderer.c's tail, utaFineSprite, which utkDrawSprite hands Smart's fine sprites to). */
    *(.text.late.utasgore)
    /* Any other late text (round 14 dropped the wildcard ahead of the host table: it placed nothing). Not
       ctr_renderer.c's or z_itemverdict.c's, whose smartcut sections come last (round 17). */
    *(SORT_BY_NAME(EXCLUDE_FILE (*z_bottomframe*.o *ctr_optmenu.o *z_bottompaint.o *z_boxcache.o *ctr_screens.o *ctr_rosalina.o *z_rosabl.o *z_dimlight.o *z_utlife.o *z_hitcheck.o *z_roomload.o *z_upd1_options.o *z_upd1_audio.o *z_upd1_text.o *z_rngexact* *z_thinart.o *z_dlghold.o *z_redpix.o *z_labcut.o *ctr_fpsbox.o *ctr_legal.o *ctr_legal_romfs.o *z_colexact.o *z_utwindow.o *z_asrtext* *z_adpcm* *ctr_adpcm_probe.o *ctr_audio_system.o *z_knightnew_history.o *z_flowcode.o *ctr_data_version.o *z_gogut* *ctr_pixgrid* *z_builderx_review* *z_builderx_new* *z_builderx_old* *z_builderx_receipt* *runner.o *z_loadscreen.o *z_symverdict* *z_smalltext_dr* *z_utstall* *z_sdmigrate* *ctr_renderer.o *z_itemverdict.o *z_phaseplan* *z_utold_scalar* *ctr_benchmark.o *z_sdlat* *vm_builtins.o *z_gamedir* *ctr_flowervramg_promotion.o *ctr_memory.o *ctr_flowervramg_palette.o *ctr_textures.o *z_hashmenu.o *state_hash.o *random.o *z_saveaudit.o *z_saveaudit_format.o *z_saveaudit_scan.o *libswkbd_late.a:* *ctr_input.o *ctr_frame_pacer.o *z_shopsplit.o *z_vramtop.o) .text.late.*))
    /* builderx UT-only adapters, after every round-14 fixed section. DR excludes this object. */
    *z_builderx_review*(.text .text.* .rodata .rodata.*)
    *(.text.h1hole.bxreview)
    __h1bxreview_text_gap_start = ABSOLUTE(.);
    . += __h1bxreview_text_gap_start <= /* H1BXREVIEW_TEXT_LIMIT */ __h1bxreview_text_gap_start
         ? ((/* H1BXREVIEW_TEXT_PHASE */ 0 - __h1bxreview_text_gap_start) & 4095) : 0;
    __h1bxreview_text_gap_end = ABSOLUTE(.);
    *(.text.late.builderxnew)
    *(.text.late.builderxold)
    /* drstep F1 (integrate round 16, after the builderx adapters; DELTARUNE only, empty in UNDERTALE builds): the
       destroy-reference sweep's loop (runner.c's end). runner.o is out of the catch-all above: its other late
       sections (colflip, layerfix, utkill, utswarm) each have their own line. */
    *(.text.late.drstep)
    /* packscreen (integrate round 16, after drstep, merge order; both games): the first time setup's texture
       preparation inside the loading screen (z_loadscreen.c's end), then its constants. z_loadscreen.o is out of
       the catch-all above (its other late sections have their own lines). */
    *(.text.late.packscreen)
    *(.rodata.packscreen_in_text)
    /* symverdict (UNDERTALE only, DELTARUNE New's has its own lines last;
       after packscreen, merge order): symmetric halving's first look from the build (z_symverdict.c, all late
       code), then its constants and the table at ctr_renderer.c's end, then that file's out-of-line copies of
       header functions. z_symverdict.o is out of the catch-all above. */
    *(.text.late.symverdict)
    *(.rodata.symverdict_in_text)
    *z_symverdict.o(.text .text.* .rodata .rodata.*)
    /* utstall item 7 (integrate round 16, after symverdict, merge order; UNDERTALE only): the plan scene's
       remaining code read after the tiles (z_utstall.c, its new roomLoadFinish wrap), then its constants.
       z_utstall.o is out of the catch-all above (its other late sections have their own lines). */
    *(.text.late.utstall7)
    *(.rodata.utstall7_in_text)
    /* sdmigrate (after utstall item 7; both games): z_sdmigrate.c, the old SD folder move's stub and DELTARUNE's
       chapter check line (change hashfix, 2026-10-27), then its constants. z_sdmigrate.o is out of the catch-all above. */
    *(.text.late.sdmigrate)
    *(.rodata.sdmigrate_in_text)
    *z_sdmigrate.o(.text .text.* .rodata .rodata.*)
    /* swkbd (UNDERTALE only): Mettaton's essay opens the software keyboard. Last in the block (after sdmigrate,
       merge order), so that nothing linked before it moves; its sections are named outside .text.late.* so that the
       catch-all above leaves them here. z_swkbd.c (a library linked after libctru's original members) with its
       constants and anything else of that library, then the library code it brings in: libctru's keyboard applet and
       the applet launch functions apt.o had not linked. */
    /* Pin the original compiler order; the applet bodies now have retained branch entries. */
    *(.text.swkbd_late.readInt)
    *(.text.swkbd_late.refuseXZ)
    *(.text.swkbd_late.keyFor)
    *(.text.swkbd_late.takeText)
    *(.text.swkbd_late.lookupVarId)
    *(.text.swkbd_late.resolve)
    *(.text.swkbd_late.finishFeed)
    *(.text.swkbd_late.openKeyboard)
    *(.text.swkbd_late.readPrompt)
    *(.text.swkbd_late.findEssay)
    *(.text.swkbd_late.watchStep)
    *(.text.swkbd_late.feedStep)
    *(.text.swkbd_late.CTR_STEP_SWKBD)
    *(.text.swkbd_late.CTR_ENDFRAME_SWKBD)
    *(.rodata.swkbd_late)
    /* The library's named sections are explicit; leave essayend's new section for the append below. */
    *libswkbd_late.a:*(.text .rodata)
    *libctru.a:swkbd.o(.text .text.* .rodata .rodata.*)
    *libctru.a:apt.o(.text.aptLaunchLibraryApplet .text.APT_PrepareToStartLibraryApplet .text.APT_StartLibraryApplet
                     .text.aptSetMessageCallback .text.APT_SendParameter)
    /* symverdict on DELTARUNE New (last; empty in other builds): the same
       code as UNDERTALE's above, compiled through z_symverdict_dr.c with sections named symverdict_dr, then its
       constants and ctr_renderer.c's Symverdict_layout. z_symverdict_dr.o is out of the catch-all above. */
    *(.text.late.symverdict_dr)
    *(.rodata.symverdict_dr_in_text)
    *z_symverdict_dr.o(.text .text.* .rodata .rodata.*)
    /* smartcut D (round 17, after symverdict on DELTARUNE New, merge order; UNDERTALE only): Smart smoothing's tile
       quads straight into the batch (ctr_renderer.c's tail), then its test-build constants. */
    *(.text.late.smartdirect)
    *(.rodata.smartdirect_in_text)
    /* smartcut item verdicts (after smartcut D; both games): the build's per-item Smart smoothing verdicts, read at a
       room change through the CtrRenderer_loadFontVerdicts chain (z_itemverdict.c, ctr_wrap_chain.h), then its
       constants. Then any other late text of ctr_renderer.c and z_itemverdict.c, both out of the catch-all above
       (every other late section of ctr_renderer.c has its own line before it). */
    *(.text.late.itemverdict)
    *(.rodata.itemverdict_in_text)
    /* dimlight: keep the renderer's existing sorted inputs here; its new helper
       belongs to the appended late line, after every fixed hole. */
    *ctr_renderer.o(.text.late.asgdraw)
    *ctr_renderer.o(.text.late.asgroom)
    *ctr_renderer.o(.text.late.chapterleak)
    *ctr_renderer.o(.text.late.cmdlabel)
    *ctr_renderer.o(.text.late.contention)
    *ctr_renderer.o(.text.late.crispart)
    *ctr_renderer.o(.text.late.engfix)
    *ctr_renderer.o(.text.late.engfix6)
    *ctr_renderer.o(.text.late.knfuse)
    *ctr_renderer.o(.text.late.krsharp)
    *ctr_renderer.o(.text.late.natsurf6)
    *ctr_renderer.o(.text.late.oomport)
    *ctr_renderer.o(.text.late.roomload)
    *ctr_renderer.o(.text.late.screens)
    *ctr_renderer.o(.text.late.smartbatch)
    *ctr_renderer.o(.text.late.smartcut)
    *ctr_renderer.o(.text.late.smartdirect)
    *ctr_renderer.o(.text.late.texcache)
    *ctr_renderer.o(.text.late.texcachef1)
    *ctr_renderer.o(.text.late.utasgore)
    *ctr_renderer.o(.text.late.utasgore.probe)
    *ctr_renderer.o(.text.late.utflpk)
    *ctr_renderer.o(.text.late.utkill)
    *ctr_renderer.o(.text.late.utpolish.line)
    *ctr_renderer.o(.text.late.utqa)
    *z_itemverdict.o(SORT_BY_NAME(.text.late.*))
    /* rosalina2 (round 17, after smartcut's item verdicts, merge order; both games): the Rosalina watcher
       (ctr_rosalina.c and the accessor at ctr_screens.c's end), then its constants. Named outside .text.late.* so the
       catch-all above leaves them here. */
    *(.text.rosalina2_late)
    *(.rodata.rosalina2_in_text)
    /* dimlight helpers stay for the appended line. Existing watcher text
       is selected above; do not swallow new .text.late.* inputs here. */
    *ctr_rosalina.o(.text .text.rosalina2_late .rodata .rodata.*)
    /* tilerun (round 17, after rosalina2, merge order; both games): drawRoom's and utkDrawRoom's runs of legacy
       tiles, one call per run (runner.c's end). */
    *(.text.late.tilerun)
    /* utflames native answers (round 17, after tilerun; UNDERTALE only): collision_circle's no-hit answer
       (z_utflames.c's end), then its constants. */
    *(.text_utfnative_late)
    *(.rodata_utfnative_late)
    /* utflames helix (round 17, after the native answers; UNDERTALE only): the sine shape's step (z_utflames.c's end). */
    *(.text_utfhelix_late)
    /* Phase pad (as utflames' above): UTFHELIX_TEXT_PHASE, stage72's address mod 32 after the helix step. */
    __utfhelix_text_gap_start = ABSOLUTE(.);
    . += ((/* UTFHELIX_TEXT_PHASE */ 0 - __utfhelix_text_gap_start) & 31);
    __utfhelix_text_gap_end = ABSOLUTE(.);
    /* utflowey (round 17, after utflames helix, merge order; UNDERTALE only): a cut scene's copy-backed tiles look
       older in play (z_utstall.c __wrap_RoomLoad_collect), then its constant. */
    *(.text.late.utflpin)
    *(.rodata.utflpin_in_text)
    /* phaseplan (DELTARUNE only, last, after utflpin, merge order): a long room's later tiles readied a phase ahead
       (z_phaseplan.c), then its constants. z_phaseplan.o is out of the catch-all above. */
    *(.text.late.phaseplan)
    *(.rodata.phaseplan_in_text)
    *z_phaseplan.o(.text .text.* .rodata .rodata.*)
    /* utold scalar3 (round 17, after phaseplan, merge order; UNDERTALE only): the lava's numeric draw_sprite_part call
       (z_utold_scalar.c, all late code; z_utold_scalar.o is out of the catch-all above), then the registered builtin's
       address it compares against (vm_builtins.c's end). */
    *(.text.late.utoldscalar)
    *z_utold_scalar.o(.text .text.* .rodata .rodata.*)
    *(.rodata.utoldscalar_in_text)
    /* pacer RAM spool (round 17, after utold scalar3, merge order; both games): the bench's result files kept in RAM
       until an excluded boundary, the run's end or a normal quit (ctr_benchmark_rows.inc), then its constants. Then any
       other late text of ctr_benchmark.c, which is out of the catch-all above (its other late sections have their own
       lines before it). */
    *(.text.late.pacer_rows)
    *(.rodata.pacer_rows_in_text)
    *ctr_benchmark.o(SORT_BY_NAME(.text.late.*))
    /* builderx receipt binding (last, merge order): archive timestamp lookup is cold too. */
    *(.text.late.builderxreceipt)
    *libctru.a:archive_dev.o(.text.archive_getmtime)
    *libctru.a:fs.o(.text.FSUSER_ControlArchive)
    /* gogut (round 17): GOG-only compatibility and lab root moved to the end of late text (zq44g, betasmoke R2-B2):
       see the gogut lines after bakkeep. */
    /* sdlat (after symverdict_dr): profile-only Azahar SD elapsed floors. */
    *(.text.late.sdlat) *z_sdlat*(.text .text.*)
    /* benchshown (utflames, round 18, after sdlat, merge order; both games): the bench's presentation v1 fields
       (ctr_benchfid_rooms.inc's end), then their constants. Named outside .text.late.* so that the catch-all above
       leaves them here. */
    *(.text.benchshown_late .text.benchshown_late.*)
    /* utstall item 8 step 1 (round 18, after benchshown, merge order; both games): the reader's game-thread reads on the game thread's open files
       (ctr_tile_reader.c and ctr_textures.c tails). Named apart from .text.late.* so the catch-all leaves it here. */
    *(.text.utstall8_late)
    /* utstall item 8 step 1b' (both games): a finished pack worker closes its data.win (ctr_texture_pack.c's tail). */
    *(.text.utstall8b_late)
    /* utstall item 8 step 1c (both games): the complete marker stands in for the worker's header checks. */
    *(.text.utstall8c_late)
    /* utflowey packed tier (round 18, after utstall item 8, merge order; UNDERTALE only): a page is promoted only
       on a repeat SD read, and the Old budget comes from the game's measured heap need (z_utstall.c utflPkCollect),
       then its constant. */
    *(.text.late.utflpk)
    *(.rodata.utflpk_in_text)
    /* drresid (DELTARUNE New, last, merge order; empty in other builds): fights' later tiles streamed in ahead of
       their first draw (z_drresid.c). Its sections aren't named .text.late.*, so no catch-all above takes them. */
    *(.text.drresid_late)
    *(.rodata.drresid_late)
    *z_drresid.o(.text .text.* .rodata .rodata.*)
    /* utold partial sprites (round 19, last, merge order; both games): draw_sprite_part's partial-tile path
       (ctr_renderer_parts.inc). Named outside .text.late.* so ctr_renderer.o's own late line above leaves it here. */
    *(.text.utpart_late)
    /* cheaplevers target FPS (round 19, after utold partial sprites, merge order; both games): the game-visible
       simulation rate (vm_builtins.c, out of the catch-all above; its noports3 code has its own line). */
    *(.text.late.cheaplevers)
    /* freebytes r5b phase pad (dec-1029-phase-pads, c26rev r5 F1): UNDERTALE 3DS aliases gameFpsRealTarget to gameFpsTarget, so the code after it
       (utfldraw, Asgore's UtFlames_borderDraw) keeps stage75zd's phase. CMake substitutes CHEAPLEVERS_TEXT_PHASE
       (stage75zd's address mod 32 here; DELTARUNE: 0 B). */
    __cheaplevers_text_gap_start = ABSOLUTE(.);
    . += ((/* CHEAPLEVERS_TEXT_PHASE */ 0 - __cheaplevers_text_gap_start) & 31);
    __cheaplevers_text_gap_end = ABSOLUTE(.);
    /* builderx chapter setup (round 19, after cheaplevers, merge order; DELTARUNE only in effect): the uncached
       chapter check's setup line (z_loadscreen.c, out of the catch-all above). */
    *(.text.late.builderxhash)
    /* utflowey turned culls (round 19, after builderx, before flowhalf, merge order; UNDERTALE only): a turned
       run's petals outside the view tested against the kept pre-cull state (ctr_renderer.c's end). Named outside
       .text.late.* so ctr_renderer.c's catch-all above leaves it here. */
    *(.text.utfldraw_late)
    /* flowhalf (before utflowey R4; UNDERTALE o3ds halfart builds and halfcensus test builds only, so no other build has it): the
       half-resolution art patch's wraps (ctr_halfart.c) and the draw-scale census (ctr_halfcensus.c). Named outside
       .text.late.* so the catch-all above leaves it here and nothing before it moves. */
    *(.text_flowhalf_late)
    /* spikerec (round 20, after flowhalf, merge order; playtest builds with build.sh's spikerec word only, both
       games, so no release build has these sections): the spike recorder
       (z_spikerec.c, ctr_audio_system.c's accessor), its constants, then that file's out-of-line copies (none
       expected). Named outside .text.late.* so the catch-all above leaves it here. */
    *(.text.spikerec_late)
    *(.rodata.spikerec_in_text)
    *z_spikerec.o(.text .text.* .rodata .rodata.*)
    /* hintpos (round 20, after spikerec, merge order; both games): the first-boot hint ("SELECT: Options") drawn
       off the game's picture (ctr_hintpos.c), then its constants, then that file's out-of-line copies (none
       expected). Named outside .text.late.* so the catch-alls above leave them here. */
    *(.text.hintpos_late)
    *(.rodata.hintpos_in_text)
    *ctr_hintpos.o(.text .text.* .rodata .rodata.*)
    /* builderx exact cold verification (round 20, after hintpos, merge order; DELTARUNE in effect): the
       cold chapter SHA checker (ctr_data_version_cold.inc) and its constants. */
    *(.text.late.builderxcold)
    *(.rodata.builderxcold_in_text)
    /* soulshake (round 20, after builderx, merge order; UNDERTALE only): view_snap stands down while the view
       scrolls (ctr_uthud_snap.c's end). Named outside .text.late.* so the catch-all leaves it here. Then the Crisp
       soul's place allowance (z_smalltext.c's end), named outside .text.* so z_smalltext.o's line leaves it here too.
       Last so that nothing merged before it moves. */
    *(.text.soulshake_late)
    *(.text_soulshake_crisp)
    /* gamedir (gamedir2, round 21, last, after soulshake, merge order; both games): the game folder's listing
       and settings.ini's copy (z_gamedir.c), then its constants on DELTARUNE Old 3DS only (late constants elsewhere,
       .rodata_gamedir2_late). z_gamedir.o is out of the catch-all above. */
    *(.text.late.gamedir)
    *(.rodata.gamedir_in_text)
    *z_gamedir.o(.text .text.* .rodata .rodata.*)
    /* gamedir2's menu sounds (ctr_audio_system.c's CtrAudio_menuSound), named outside .text.late.*. */
    *(.text_gamedir2_late)
    /* legalui (round 21, last, after soulshake, merge order; both games): the Legal notices and Credits pages
       (ctr_legal.c's end), then their constants (in late text: DELTARUNE Old's late constants had 505 B left).
       Named outside .text.late.* and .rodata.late.* so the catch-alls above leave them here. */
    *(.text.legalui_late)
    *(.rodata.legalui_in_text)
    /* smalltext on DELTARUNE New (sansfont round 3, last, after soulshake, merge order; empty in other builds): the
       same code as UNDERTALE's smalltext above, compiled through z_smalltext_dr.c with sections named smalltext_dr,
       then its constants and ctr_renderer.c's tables for it. z_smalltext_dr.o is out of the catch-all above. */
    *(.text.late.smalltext_dr)
    *(.rodata.smalltext_dr_in_text)
    *z_smalltext_dr.o(.text .text.* .rodata .rodata.*)
    /* bubbleclip (round 21, after soulshake, merge order; UNDERTALE only): a writer's carried Crisp line gives pixels back at its
       gaps to keep off its bubble's edge (z_smalltext.c's end), then its constants. Named outside .text.* and .rodata.* so z_smalltext.o's line
       above leaves them here. */
    *(.text_bubbleclip_late)
    /* asgorelong T1 pad (UNDERTALE only): 188 B in bubbleclip's slot, so T1's 188 B shorter stBubbleClip leaves every
       later late symbol where it was (z_smalltext.c, bcPad, ASL_BCPAD words; never read). */
    KEEP(*(.text_asgorelong_bcpad))
    *(.rodata_bubbleclip_late)
    /* credfix (after flowhalf, merge order; both games): sound asset gain multiplies each instance's gain
       (ctr_audio_system.c's end) and the start-up notice for a missing credits.txt / Chapter 1 lang_en.json
       (z_credfix.c). Named outside .text.late.* so the catch-all above leaves it here. */
    *(.text.credfix_late)
    /* credfix (last, merge order): UNDERTALE's text lookup front that returns text without an escape before its
       letter loop (gmlaot.py's GETTEXT_FAST), the whole generated file with its constants. */
    *aot_gettext.o(.text .text.* .rodata .rodata.*)
    *(.text.late.flowervramg_cia80)
    *(.text.late.flowervramg_palette)
    *(.text.late.flowervramg_promotion)
    /* flowfull code paging (round 21, last, merge order; DELTARUNE New flowcode=lazy builds only, default Off, so no
       release build has these sections): z_flowcode.c. z_flowcode.o is out of the catch-all above. */
    *(.text.late.flowfullcode)
    /* knightnew GPU-1 (round 21, last, merge order; DELTARUNE New only, empty elsewhere): z_knightnew_history.c and
       ctr_renderer.c's KnightHistory_layout table. z_knightnew_history.o is out of the catch-all above. */
    *(.text.late.knightnew)
    *(.rodata.knightnew_in_text)
    /* utflowey R4 (round 21f, last, after knightnew, merge order; UNDERTALE only): a whole set's room load gets the
       time its big pages need on the Old models (z_utstall.c's __wrap_CtrTextures_roomLoadFinish; the round-16 wrap
       keeps its bytes as usFinish7), then its constant. z_utstall.o is out of the catch-all above. */
    *(.text.late.utflr4)
    *(.rodata.utflr4_in_text)
    /* utflowey2 (option 4, round 21f, last, after utflowey R4, merge order; UNDERTALE Old only): packed copies in spare
       linear memory (z_utstall.c's end), then its constants. Named outside .text.late.* so the catch-all above leaves
       it here. */
    *(.text.utfl2_late)
    *(.rodata.utfl2_in_text)
    /* dodgefront (credfix, charter rev 1 option 3; last, merge order; UNDERTALE Old only, empty elsewhere): the centred
       text line's hand front and its binding (z_dodgefront.c, vm.c's VM_aotBindFront), then their constants. */
    *(.text.dodgefront_late)
    *(.rodata.dodgefront_in_text)
    *z_dodgefront.o(.text .text.* .rodata .rodata.*)
    /* credcap (credfix, charter rev 1 item 4; last, merge order; both games): the sound gain
       cap at 1 (ctr_audio_system.c's credcapSetSoundGain). */
    *(.text.credcap_late)
    /* climbnoise (round 22, merge order; DELTARUNE New only, empty elsewhere): the tower textures' Azahar-only
       linear-memory free (vine_new_retain.inc's cnTowerFree). Named outside .text.late.* so that ctr_renderer.o's
       catch-all above leaves it here. Ahead of credfix's leavekeep, which came later, so cand22's place holds
       (exittrail, between them in cand22, is empty in release builds). */
    *(.text.climbnoise_late)
    /* leavekeep (credfix, charter rev 1 item 1; last in release builds, merge order; both games): the lab leaves'
       exit without the heap unmap and the delayed lab crash test (z_leavekeep.c), then their constants. */
    *(.text.leavekeep_late)
    *(.rodata.leavekeep_in_text)
    *z_leavekeep.o(.text .text.* .rodata .rodata.*)
    /* exittrail (last, merge order; build.sh's exittrail word only, never a release): the exit trail
       (z_exittrail.c) and its constants. */
    *(.text.exittrail_late)
    *z_exittrail.o(.text .text.* .rodata .rodata.*)
    /* flowend (after soulshake, merge order; both games): queued sound reads between the pieces of a cache decode
       (ctr_audio_system.c's end, the CtrDecoder_read wrap). Named outside .text.late.* so the catch-all leaves it here. */
    *(.text.flowend_late)
    /* flowend: the stream open-ahead's constants (in late text, named outside .rodata.late.*). */
    *(.rodata.flowend_in_text)
    /* cullcrisp (credfix, charter rev 1; last, merge order: after flowend so its place holds; UNDERTALE, and
       DELTARUNE New through z_smalltext_dr.c): Crisp text wholly out of view draws nothing (z_smalltext.c's end).
       Named outside .text.* so the smalltext lines above leave it here. */
    *(.text_cullcrisp_late)
    *(.text_cullcrisp_dr_late)
    /* repack (round 20, last, merge order; DELTARUNE New only): per-room texture repack (ctr_repack.inc). */
    *(.text.repack_late)
    *(.rodata.repack_in_text)
    /* offkill (offdraw, last, merge order; UNDERTALE only, empty in DELTARUNE): the early destroy of bullets past
       the view (z_offkill.c), then its constants (in late text: UNDERTALE Old's late constants are short), then that
       file's out-of-line copies of header functions (none expected). Named outside .text.late.* so the catch-all above
       leaves it here. */
    __offkill_text_start = ALIGN(4);
    *(.text.offkill_late)
    /* offkillfast (pacer 10-31): OffKill_roomChange walks the listed objects' lists, 516 B where it was 520; its
       old 520 B stay its own, so its constants and everything after keep their addresses. DELTARUNE: empty, nothing. */
    . = . > __offkill_text_start ? MAX(., __offkill_text_start + 520) : .;
    *(.rodata.offkill_in_text)
    *z_offkill.o(.text .text.* .rodata .rodata.*)
    /* freewins (round 2, last, merge order; both games): the draw-pass bounds and fireDrawSubtype's per-instance
       end (runner.c's end). runner.o is out of the catch-all above. */
    *(.text.late.freewins)
    /* freewins (after its runner.c line; both games): the stats block's heap figures (main.c's end). Named outside
       .text.late.* so the catch-all above leaves it here. */
    *(.text.freewins_late)
    /* freewins (round 3, after its round 2 lines; both games): ds_map_add's borrowed strings (vm_builtins.c's end)
       and the saved room states on first use (runner.c's end); runner.o and vm_builtins.o are out of the catch-all
       above. Then z_btnprompt.c's slot test for the borrowed strings (UNDERTALE only), named outside .text.late.* so
       the catch-all above leaves it here. */
    *(.text.late.freewins3)
    *(.text.freewins_r3_late)
    /* stallfix rev 3 (last, merge order; UNDERTALE, and DELTARUNE New only: empty in DELTARUNE Old): the bench's held-frame lines (ctr_benchheld.inc, ctr_audio_system.c's
       CtrAudioSystem_heldLoadTicks), then their constants. Named outside .text.late.* so the catch-alls leave it here. */
    *(.text.stallfix_late)
    *(.text.stallfix_late.constants)
    /* utorder (credfix, ruling 2026-10-27; last, merge order; UNDERTALE only, empty elsewhere): the draw order
       key's wraps (z_utorder.c), then the full rebuild by key (runner.c's end; runner.o is out of the catch-all). */
    *(.text.utorder_late)
    *(.text.late.utorder)
    /* utgapfix (last, merge order; UNDERTALE only): string_char_at's ASCII walk and its binding (z_utflames.c's
       end), then its constant. Named outside .text.* and .rodata.* so z_utflames.o's line above leaves them here. */
    *(.text_utgapfix_late)
    *(.rodata_utgapfix_in_text)
    /* freewins (round 4, after its round 3 lines; UNDERTALE only): L2, the whole set's make-room in linear memory
       (z_utstall_l2.c, usWholeEvict), named outside .text.late.* so the catch-all above leaves it here. */
    *(.text.freewins_r4_late)
    /* textfront (simplefix, last, merge order; UNDERTALE Old only, empty elsewhere): the centred-text scripts' hand
       fronts with the view cull and their binding (z_textfront.c), then their constants (in late text: UNDERTALE
       Old's late constants are short), then that file's out-of-line copies of header functions (none expected).
       Named outside .text.late.* so the catch-all above leaves it here. */
    *(.text.textfront_late)
    *(.rodata.textfront_in_text)
    *z_textfront.o(.text .text.* .rodata .rodata.*)
    /* writerfront (freebies, after textfront; UNDERTALE Old only, empty elsewhere and in nowriterfront builds): the
       dialogue writer's Draw front, its binding and vm.c's VM_aotSwapFront, then its constants and that file's
       out-of-line copies of header functions. Named outside .text.late.* so the catch-all above leaves it here. */
    *(.text.writerfront_late)
    *(.rodata.writerfront_in_text)
    *z_writerfront.o(.text .text.* .rodata .rodata.*)
    /* noshots (credfix, ruling 2026-10-28; last, merge order; both games): the BenchScript_takeShot wrap that
       skips scene shot steps in timing rows (z_noshots.c). Named outside .text.late.* so the catch-all leaves it here. */
    *(.text.noshots_late)
    *(.rodata.noshots_in_text)
    *z_noshots.o(.text .text.* .rodata .rodata.*)
    /* benchfix (last, merge order; both games): a tour room's warm-up starts after a warp frame longer than the
       warm-up (ctr_benchmark.c's end, the benchfidBeginFrame wrap). Named outside .text.late.* so ctr_benchmark.o's
       line above leaves it here. */
    *(.text.benchfix_late)
    /* benchfix (after it, merge order; both games): the sweep start's chapter-1 data.win answer (ctr_benchmark.c's
       end, the _stat_r wrap). */
    *(.text.benchfix_stat_late)
    /* benchfix (after it, merge order; both games): a bench window's own budget of utpolish's "Frame over" lines
       (ctr_benchmark.c's end). */
    *(.text.benchfix_lines_late)
    /* freebies restart preload (last, merge order; UNDERTALE only, empty in DELTARUNE): restarting the room into another
       planned scene gets the room load (z_freebies.c), then its constants. Named outside .text.late.* so the
       catch-alls above leave them here. */
    *(.text.freebies_late)
    *(.rodata.freebies_in_text)
    *z_freebies.o(.text .text.* .rodata .rodata.*)
    /* benchfix round 6 (last, merge order; both games): the bench's frame work, stat answer and fence timing moved out
       of the wraps above (which keep their slots), the held lines' window boundary and report (ctr_benchmark.c's
       end). Named outside .text.late.* so ctr_benchmark.o's line above leaves it here. */
    *(.text.benchfix_r6_late)
    /* sfxcopy (aotprof, last, merge order; both games): buildSfx's word-at-a-time dual-mono check and copy
       (ctr_sfxcopy.inc, in ctr_audio_system.c). Named outside .text.late.* so the catch-all leaves it here. */
    *(.text.sfxcopy_late)
    /* freewins (round 4c, after its round 4 line; UNDERTALE Old only): L1, the overflow allocator's linear piece
       (z_utstall_l2.c), and the heap reliever's wrap that gives it the texture cache; then L1's log line constants. */
    *(.text.freewins_r4b_late)
    *(.rodata.freewins_r4_in_text)
    /* bboxround (simplefix, last, merge order; both games): Collision_computeBBox's edge rounding (z_bboxround.c:
       a VFP truncation front, the double's bits for the rest). Named outside .text.late.* so the catch-all above
       leaves it here. */
    *(.text.bboxround_late)

    /* flowpitch (flowend, last, merge order; both games): every voice and ctr_video.c's wall clock at the game's
       frame clock (ctr_audio_system.c's end, ctr_music_clock.h) and its log line. */
    *(.text.flowpitch_late)
    *(.rodata.flowpitch_in_text)
    *(.text.late.adpcm .text.late.adpcm.*)
    /* ADPCM's newly linked libctru entry; absent from the frozen UT controls. */
    *(.text.ndspChnSetAdpcmCoefs)
    /* codeclose F1/F6 (last, merge order; UNDERTALE Old only, empty elsewhere): the plan's code after the tiles through
       roomload's coalesced, non-aborting reader (z_roomload.c's end, the RoomPlan_roomCode wrap), then its constants.
       Named outside .text.late.* so the catch-all above leaves it here. */
    *(.text.codeclose_late)
    *(.rodata.codeclose_in_text)
    /* A cached sound's PCM freed once no stopped uta channel reads it (ctr_audio_system.c's end; both games), then
       its literal table and log text. Named outside .text.late.* so the catch-alls above leave them here. */
    *(.text.pcmwait_late)
    *(.rodata.pcmwait_in_text)
    /* boardgrid (simplefix, last, merge order; DELTARUNE New only, empty elsewhere): Chapter 3's board grid rebuild
       (z_boardgrid.c, bound by gmlaot.py's BOARD_NATIVE). Named outside .text.late.* so the catch-all above leaves it
       here. */
    *(.text.boardgrid_late)
    /* benchfix round 7 (last, merge order; both games): the held lines' window boundary with argbenchr6's M1-M4
       fixed (ctr_benchmark.c's end; round 6's two functions above keep their slots and call these); DELTARUNE Old,
       which has no held lines, has only the tour warm-up's restart (bfTourBefore). Named outside .text.late.* so
       ctr_benchmark.o's line above leaves it here. */
    *(.text.benchfix_r7_late)
    /* frametrace (benchfix, last, merge order; test builds with build.sh's frametrace word only, both games, so
       no release build has these sections): the frame trace (z_frametrace.c, the accessors at ctr_benchmark.c's and
       ctr_frame_pacer.c's ends), its constants, then that file's out-of-line copies (none expected). Named outside
       .text.late.* so the catch-all above leaves it here. */
    *(.text.frametrace_late)
    *(.rodata.frametrace_in_text)
    *z_frametrace.o(.text .text.* .rodata .rodata.*)
    /* utold Continue transition (utold, last, merge order; UNDERTALE only, empty in DELTARUNE): the title
       menu's Continue hook (z_utold_contblack.c). Named outside .text.late.* so the catch-alls leave it here. */
    *(.text.utoldcontblack_late)
    *(.rodata.utoldcontblack_late)
    /* lbind (aotprof, last, merge order; both games): aotBind's name test, offset and blob length first
       (z_lbind.c). Named outside .text.late.* so the catch-all leaves it here. */
    *(.text.lbind_late)
    *z_lbind.o(.text .text.* .rodata .rodata.*)
    /* nopromo (freebies r12 rev 2, last, merge order; UNDERTALE only, empty in DELTARUNE): once the packed budget is 0
       with nothing packed, Draw asks no promotions (z_freebies.c, the CtrRenderer_endFrame chain's link after
       freebies), then its constants. Named outside .text and .rodata so z_freebies.o's line above leaves them here. */
    *(.late_nopromo)
    *(.late_nopromo_ro)
    /* knightgpu L-A (last, merge order; DELTARUNE New only, empty elsewhere): the application surface's
       unused full-resolution memory lent to hot spilled surfaces (ctr_renderer.c's end). Named
       outside .text.late.* so ctr_renderer.o's own late line above leaves it here. */
    *(.text.knightgpu_late)
    /* utold c112dtm rev 4 (merge order): rooms.dtm at the reader's stop (ctr_textures.c's tail), then its log
       line. Named outside .text.late.* so the catch-alls above leave it here.
       Padded to rev 3's span on UNDERTALE Old so nothing after it moves; empty elsewhere, no pad. The pad is a
       declared gap (__utolddtm_text_gap_*, zero fill, at most the 320 B slot) so check_smoothux_layout names it. */
    __utolddtm_late_start = .;
    *(.text.utolddtm_late)
    *(.rodata.utolddtm_late)
    __utolddtm_text_gap_start = ABSOLUTE(.);
    . = . > __utolddtm_late_start ? __utolddtm_late_start + 320 : .;
    __utolddtm_text_gap_end = ABSOLUTE(.);
    /* utext (credfix T1, ruling 2026-10-28; last, merge order; UNDERTALE only, empty elsewhere): steady text
       glyphs straight into the open batch (ctr_renderer.c's end, utxEmitPageRegion). Named outside .text.late.*. */
    *(.text.utext_late)
    /* usstop (credfix, UT-002; last, merge order; UNDERTALE only, empty in DELTARUNE): the sound plan worker held idle
       before shutdownAudio's frees (ctr_audio_system.c's end, the CtrReadAhead_shutdown wrap), then the exit trail
       build's provoke test constants. Named outside .text.late.* so the catch-alls leave it here. */
    *(.text.usstop_late)
    /* usstop rev 5 pad (UNDERTALE only, empty in DELTARUNE): 4 B so rev 5's growth of the section above is 96 B, not
       92, and every late symbol after it keeps its cand26 cache-line phase; then the svcExitProcess wrap, which comes
       from the same file but has its own section so the pad can sit before it. */
    KEEP(*(.text.usstop_pad))
    *(.text.usstop_exit)
    *(.usstop_keep_text) /* z_leavekeep.c's CtrLeavekeep_ownerKeep: named outside .text.* (its file's catch-all) */
    *(.rodata.usstop_in_text)
    /* asrtext (last, merge order): exact-string PC-height glyph selection. */
    *(.text.late.asrtext)
    /* flowend burst (last, merge order; UNDERTALE only, empty in DELTARUNE): a step's plays past the first four
       start at the frame's audio update (ctr_audio_system.c's end, the vtable's playSound and update entries).
       Named outside .text.late.* so the catch-alls leave it here. */
    *(.text.flowburst_late)
    *(.rodata.flowburst_in_text)
    /* creddodge (last, merge order): the glyph view test with integer compares. */
    *(.text.creddodge_late)
    /* cdid (creddodge lever 2, last, merge order; both games): the instance-by-id lookup's entry-index cache
       (stb_ds.c's end, cdHmgetKey). Named outside .text.late.* so the catch-all above leaves it here. */
    *(.text.cdid_late)
    /* cdbox (creddodge lever 3, last, merge order; UNDERTALE only, empty in DELTARUNE): colline_gm14.h's bbox
       reads from one Collision_computeBBox (vm_builtins.c's end, cdBoxSide). vm_builtins.o is out of the catch-all above, and
       layerfix's input check takes only .text.late.* names from it. */
    *(.text.late.cdbox)
    *(.text.late.colexact)
    /* utbugs4 (last, merge order): virtual window position and final picture translation. */
    *(.text.late.utbugs4) *z_utwindow.o(.text .text.*)
    /* tbox (creddodge; UNDERTALE only; before relprep): the string box fitted to the alignment
       (ctr_renderer.c's end, cdReachBoxLen and the cdReachBox thunk). Named outside .text.late.*. */
    *(.text.tbox_late)
    /* pacebot (pacer, last before relprep, merge order; both games): with the game on the bottom screen, the
       pacer keeps to the bottom screen's vblanks (ctr_frame_pacer.c's end: the C3D_FrameCounter, gspWaitForEvent,
       gspSetEventCallback, CtrPacer_setGameScreen and CtrPacer_frameEnding wraps). Named outside .text.late.* so the
       catch-alls leave it here. */
    *(.text.pacebot_late)
    /* relprep's code, last (moved from after sandbox, above): cold, and its size follows the release source URL. */
    *(.text.late.relprep)
    *(.text.h1hole.relprep)
    __h1relprep_text_gap_start = ABSOLUTE(.);
    . += __h1relprep_text_gap_start <= /* H1RELPREP_TEXT_LIMIT */ __h1relprep_text_gap_start
         ? ((/* H1RELPREP_TEXT_PHASE */ 0 - __h1relprep_text_gap_start) & 4095) : 0;
    __h1relprep_text_gap_end = ABSOLUTE(.);
    /* flowstream (after relprep, merge order: nothing before it moves; UNDERTALE Old only, empty
       elsewhere): the background refill of plan tiles evicted in play, its reader and page file hooks
       (z_flowstream.c) and its install (ctr_textures.c's end). */
    *(.text.flowstream_late)
    *(.rodata.flowstream_in_text)
    /* trailfront (asrfight, after flowstream and before fpsbox, merge order; UNDERTALE Old writerfront builds
       only, empty elsewhere): the SAVE Asriel rainbow trail's Draw front and its binding (z_trailfront.c), then its
       constants. */
    *(.text.trailfront_late)
    *(.rodata.trailfront_in_text)
    *z_trailfront.o(.text .text.* .rodata .rodata.*)
    /* fpsbox (last, merge order; tail replacement a39a00d2f, 2026-10-30): the once-per-second display reading and
       menu restore, and the tail-only flush wrappers (ctr_optmenu.c's fpsboxMenuFlush / fpsboxFlushConsoles), after
       every other late text input so no existing late address moves. */
    *(.text.late.fpsbox)
    *(.text.fpsbox_late)
    /* asgborder (asgore30, after fpsbox, merge order; UNDERTALE only, empty elsewhere): the native body of
       draw_self_border_true, Asgore's sided fire clipped to the box, with its wrapper (z_asgborder.c, aot_table.c's
       aotu_<k>), then anything else of the file. Named outside .text.late.* so the catch-alls leave it here. */
    *(.text.asgborder_late)
    *z_asgborder.o(.text .text.* .rodata .rodata.*)
    /* freebytes r5b phase pad (dec-1029-phase-pads, c26rev r5 F1): z_asgborder.o calls aot_late.c's aotStoreSlow, so exitpanic and the code after it keep
       stage75zd's phase. CMake substitutes ASGBORDER_TEXT_PHASE (stage75zd's address mod 32 here; DELTARUNE: 0 B). */
    *(.text.h1hole.asgborder)
    __asgborder_text_gap_start = ABSOLUTE(.);
    . += __asgborder_text_gap_start <= /* ASGBORDER_TEXT_LIMIT */ __asgborder_text_gap_start
         ? ((/* ASGBORDER_TEXT_PHASE */ 0 - __asgborder_text_gap_start) & /* ASGBORDER_TEXT_MASK */ 31) : 0;
    __asgborder_text_gap_end = ABSOLUTE(.);
    /* exitpanic (after asgborder, merge order; both games): the exit-wide heap keep (z_leavekeep.c's
       CtrSvcControlMemory_exitpanic). Named outside .text.* so z_leavekeep.o's catch-all leaves it here. */
    *(.exitpanic_text)
    /* itemfront phase pad (lead GO 2026-10-30 under dec-1029-phase-pads): UNDERTALE Old's itemfront keeps its
       console-measured cache-line phase (asrfight frozen/if4: ifDigits at 0x66e398, phase 24), so the smallest pad
       (0-31 B, zero-filled) puts it there after exitpanic. CMake substitutes ITEMFRONT_TEXT_PHASE (UNDERTALE Old 24;
       other targets, where itemfront is empty: the pad's own start, a 0 B pad). */
    __itemfront_text_gap_start = ABSOLUTE(.);
    . += ((/* ITEMFRONT_TEXT_PHASE */ 0 - __itemfront_text_gap_start) & 31);
    __itemfront_text_gap_end = ABSOLUTE(.);
    /* itemfront (asrfight, after exitpanic, merge order; UNDERTALE Old writerfront builds only, empty elsewhere):
       the item-name scripts' fronts and their binding (z_itemfront.c), its constants, then the file's other code. */
    *(.text.itemfront_late)
    *(.rodata.itemfront_in_text)
    *z_itemfront.o(.text .text.* .rodata .rodata.*)
    /* evguard phase pad (lead GO 2026-10-30 under dec-1029-phase-pads): UNDERTALE Old's evguard keeps the cache-line
       phase its console rows and rev 2 identity build had (asgore30 frozen/evg1 and evg2: VM_beginEventRunGuard at
       0x66f3b0, phase 16), so the smallest pad (0-31 B, zero-filled) puts it there after itemfront. CMake substitutes
       EVGUARD_TEXT_PHASE (UNDERTALE Old 16; other targets: the pad's own start, a 0 B pad). */
    __evguard_text_gap_start = ABSOLUTE(.);
    . += ((/* EVGUARD_TEXT_PHASE */ 0 - __evguard_text_gap_start) & 31);
    __evguard_text_gap_end = ABSOLUTE(.);
    /* evguard (asgore30, after itemfront, merge order; UNDERTALE only, empty elsewhere): the runner's event runs
       skipping the events of code that starts with a false test of a global (z_evguard.c), then anything else of
       the file. */
    *(.text.evguard_late)
    *z_evguard.o(.text .text.* .rodata .rodata.*)
    /* pacerflt phase pad (lead GO 2026-10-30 under dec-1029-phase-pads): UNDERTALE Old's prefilter keeps the
       cache-line phase of its console rows (pacer's prefstack rows, B: UtFlames_rectPre at 0x66f6e0,
       phase 0), so the smallest pad (0-31 B, zero-filled) puts it there after evguard. CMake substitutes
       PACERFLT_TEXT_PHASE (UNDERTALE Old 0; other targets: the pad's own start, a 0 B pad). */
    __pacerflt_text_gap_start = ABSOLUTE(.);
    . += ((/* PACERFLT_TEXT_PHASE */ 0 - __pacerflt_text_gap_start) & 31);
    __pacerflt_text_gap_end = ABSOLUTE(.);
    /* pacerflt (pacer 2026-10-28; after evguard, merge order): UtFlames_rectPre, the helix rectangle test's
       far-bullet answer (z_utflames.c). Named outside .text.* so neither the catch-all nor the early z_utflames.o
       line takes it. */
    *(.text_pacerflt_late)
    /* flowhitch phase pad (lead GO 2026-10-31 under dec-1029-phase-pads): UNDERTALE Old's flowhitch keeps the
       cache-line phase of its rev 3m console rows (measured phase 0x18), so the smallest pad (0-31 B, zero-filled)
       puts it there after pacerflt. CMake substitutes FLOWHITCH_TEXT_PHASE (UNDERTALE Old 24; other targets: the
       pad's own start, a 0 B pad). */
    __flowhitch_text_gap_start = ABSOLUTE(.);
    . += ((/* FLOWHITCH_TEXT_PHASE */ 0 - __flowhitch_text_gap_start) & 31);
    __flowhitch_text_gap_end = ABSOLUTE(.);
    /* flowhitch (flowend r22, after pacerflt, merge order; UNDERTALE only, empty in DELTARUNE): a sound
       the linear guard shed plays as a stream until the next room change, and its replays start from a spare
       decoder (ctr_audio_system.c's chain links). Last, so every earlier late function keeps its address. */
    *(.text.flowhitch_late)
    /* flowhitch rev 4 (flowend r25, after rev 3's, UNDERTALE only): the musical's first and repeated songs. */
    *(.text.flowhitch4_late)
    /* loadaudio: appended after rev 3m flowhitch; freeze excess load time. */
    *(.text.late.loadaudio)
    /* exitpanic rosacore (last, after loadaudio, merge order; both games): the Rosalina watcher's thread create,
       without the core-1 try the kernel refuses before the app's CPU time limit is set (z_leavekeep.c's
       CtrRosalina_threadCreate). */
    *(.exitpanic_rosa_text)
    /* postkick phase pad (lead 2026-10-31 under dec-1029-phase-pads; postkick runs every frame): UNDERTALE Old's
       postkick keeps the cache-line phase of its console rows (pacer's postkick rows, B:
       __wrap_Utsw_handleRoomChange at 0x6703e8, phase 8), so the smallest pad (0-31 B, zero-filled) puts it there
       after rosacore. CMake substitutes POSTKICK_TEXT_PHASE (UNDERTALE Old 8; other targets: the pad's own start,
       a 0 B pad). */
    __postkick_text_gap_start = ABSOLUTE(.);
    . += ((/* POSTKICK_TEXT_PHASE */ 0 - __postkick_text_gap_start) & 31);
    __postkick_text_gap_end = ABSOLUTE(.);
    /* postkick (pacer, after rosacore and before labcut, merge order; UNDERTALE only, empty elsewhere):
       main.c's end-of-frame instance pass held until the GPU has the frame (z_postkick.c), then anything else
       of the file. */
    *(.text.postkick_late)
    *z_postkick.o(.text .text.* .rodata .rodata.*)
    /* labcut: append LAST, after all selected player-facing units. */
    *(.text.late.labcut)
    /* utrect (uthunt, after labcut, merge order; UNDERTALE only, empty elsewhere): the filled-rectangle script's
       builtin and Utgap_registerAll's wrap that registers it (z_utrect.c), then its constants and anything else of the
       file. Named outside .text.late.* so the catch-all above leaves it here. */
    *(.text.utrect_late)
    *(.rodata.utrect_in_text)
    *z_utrect.o(.text .text.* .rodata .rodata.*)
    /* freebytes H1 (UNDERTALE Old only; last): the loading-screen functions the H1 holes had no room for, so that
       the loading screen still gives at least 4096 B (its end pad, above, moves the code after it by -4096 B).
       Late code added after this line moves nothing hot. Empty elsewhere. */
    *(.text.h1hole.end)
    /* uttc (uthunt T1, after freebytes H1's end pieces, merge order; UNDERTALE only, empty elsewhere): the tile pre-filter at
       runner.c's end (uttcDrawTileRun, uttcBuild) and its constants (runner.o is excluded
       from the catch-all above). Last, so no earlier late function moves. */
    *(.text.late.uttc)
    *(.rodata.uttc_in_text)
    /* knightgpu d2 (last, merge order; DELTARUNE New only, empty elsewhere): tiled draws' tile quads out of view
       culled and held additive passes made a texture tile at a time (ctr_renderer.c's end). Named outside .text.late.*
       so ctr_renderer.o's own late line above leaves it here. */
    *(.text.knightgpu_tiled_late)
    /* knightgpu O9 (after d2, merge order; DELTARUNE New only, empty elsewhere): blend and fog calls held while a batch
       is open, their check stubs and the stubs' table (ctr_renderer.c's end). */
    *(.text.knightgpu_lazy_late)
    *(.rodata.knightgpu_lazy_in_text)
    /* depthmove (last, merge order; DELTARUNE only, empty in UNDERTALE): automatic depth layers' element lists as
       deques: runner.o's Depthmove_free (z_depthmove.h; runner.o is out of the catch-all, and layerfix's input check
       needs its late name), then z_depthmove.c, its constants and out-of-line copies. z_depthmove.c's code is named
       outside .text.late.* so the catch-all leaves it here. Its message string, then dmRegrowTail (added after the
       first queued rows, so placed after them to keep their addresses). */
    *(.text.late.depthmove)
    *(.text.depthmove_late)
    *(.rodata.depthmove_late)
    *(.text.depthmove_late_tail)
    *z_depthmove.o(.text .text.* .rodata .rodata.*)
    /* knightgpu T1 (after depthmove, merge order; DELTARUNE New only, empty elsewhere): O9's eager blend and fog
       setters with citro3d's state written straight into the command buffer, and their table (ctr_renderer.c's end).
       Inside .late.text, never an output section of its own (docs/3ds-hardware.md, 75zg). */
    *(.text.knightgpu_texenv_late)
    *(.rodata.knightgpu_texenv_in_text)
    /* knsurf (last, merge order; DELTARUNE New only, empty elsewhere): strips appended to their live surface batch
       (ctr_renderer_knsurf.inc). Inside .late.text, never in an output section of its own: a separate output section
       after .late.text left the 3dsx with an absolute relocation on an instruction word, and the loader refused the
       ROM (docs/3ds-hardware.md, 75zg). Only ld's ARM-to-Thumb veneer (____wrap_Crispart_drawTexture_from_arm, one
       caller, caDraw), which ld puts after the last code input, moves (lead 10-31 18:5x). fpsbox's
       .text.fpsbox75zm_veneer later retains that entry explicitly, as caDraw is reached during real drawing. */
    *(.text.knsurf_late)
    /* exitpanic argbind (last, after knsurf, merge order; DELTARUNE only, empty in UNDERTALE): argui's per-load
       pointers emptied at a chapter's unload and set at its texture set-up and session start (z_argbind.c). Code
       only: ld puts its ARM-to-Thumb veneer after the block's last code, and the word 8 B after that veneer must stay
       reserve padding (3dsxtool writes a relocation there), so its names go to late rodata (.rodata.argbind_late). */
    *(.text.argbind_late)
    /* creddodge pop (creddodge 10-31; last, merge order; UNDERTALE only, empty elsewhere): aotSitePop's builtin
       write through an instance id (vm.c's end, cdPop). Named outside .text.late.* so the catch-all above leaves it
       here; last, so no other late function moves. */
    *(.text.cdstep_late)
    /* knightgpu A (last, merge order; DELTARUNE New only, empty elsewhere): bank A compaction, its moves and the
       promotion after (ctr_renderer.c's end, ctr_memory.c's end). Last in .late.text, after argbind and cdPop, so
       no earlier late function moves; only ld's ARM-to-Thumb veneer, which follows the last code input, moves. */
    *(.text.knightgpu_compact_late)
    /* dprank (uthunt; last, merge order; both games): drawPassFind, the draw pass's one responder read at its
       recorded draw rank before the scan (runner.c's drawPassFirst calls it, padded to its old size). runner.o is
       out of the catch-all above. Last in .late.text, after knightgpu A, so no earlier late function moves; only
       ld's ARM-to-Thumb veneer, which follows the last code input, moves. */
    *(.text.late.dprank)
    /* creddodge presize (creddodge 10-31; last, after knightgpu A and dprank, merge order; UNDERTALE only, empty elsewhere):
       a new instance's selfVars capacity (colline_gm14.c's end). Named outside .text.* so colline_gm14.o's line above
       leaves it here. */
    *(.cdcap_late)
    /* benchcut (flowend r30, last, after creddodge presize, merge order; both games; bench only): the sweep's alive test by its cached
       object index, its attack label described only when a read changes (ctr_benchmark.c's end), and a kept set
       whose source has no instance failing without its error text (bench_script.c's end). Named outside
       .text.late.* so no catch-all above takes it; last, so no earlier late function moves. */
    *(.text.benchcut_late)
    /* creddodge D2' (creddodge 11-01; last, after presize and benchcut, merge order; UNDERTALE only, empty elsewhere): Smart's
       premultiplied text colour, out of utxEmitPageRegion so that its slot keeps 2504 B (ctr_renderer.c's end). */
    *(.text.cdtail_late)
    /* codeclose textcase (lever 1, last, after creddodge D2', merge order; UNDERTALE Old only, empty elsewhere): the dialogue script's
       case-table front, its table read and its prefetchRoom link (z_textcase.c), then its constants (in late text:
       UNDERTALE Old's late constants are short). Named outside .text.late.* so the catch-all above leaves it here;
       last, so no earlier late function moves. */
    *(.text.codeclose_case_late)
    *(.rodata.codeclose_case_in_text)
    *z_textcase.o(.text .text.* .rodata .rodata.*)
    /* codeclose writerfirst (lever 2, last, merge order; UNDERTALE Old writerfront builds only, empty elsewhere): the
       wrapper that gives a writer's first Draw writerfront's front, its prefetchRoom link (z_writerfirst.c) and vm.c's
       VM_aotBoundFn, then its constants and that file's out-of-line copies of header functions. Named outside
       .text.late.* so the catch-all above leaves it here; last, so no earlier late function moves. */
    *(.text.codeclose_wf_late)
    *(.rodata.codeclose_wf_in_text)
    *z_writerfirst.o(.text .text.* .rodata .rodata.*)
    /* sndcut (flowend r31; last, after codeclose writerfirst, merge order; UNDERTALE only, empty elsewhere): stopSound's late copy, a free
       voice skipped without voiceLive's call (ctr_audio_system.c's end). Named outside .text.late.* so no catch-all
       above takes it; last, so no earlier late function moves. */
    *(.text.sndcut_late)
    KEEP(*(.rodata.sndcut_in_text)) /* sndcutKeep: no code refers to it */
    /* drpix (last, Old DELTARUNE): resolve native colour geometry and guard compact surface sampling.
       Named outside .text.late.* so ctr_renderer.o's earlier catch-all leaves it here, inside .late.text. */
    *(.text.drpix_late)
    /* inflight (flowend r35; last, after sndcut and drpix, merge order; UNDERTALE only, empty elsewhere): a
       burst play missing a sound whose read is on its way leaves the deferred voices deferred (ctr_audio_system.c's
       end). */
    *(.text.inflight_late)
    /* omegaflowey L2 (last, after inflight, merge order; UNDERTALE Old only, empty elsewhere): the sound plan's stream
       scene, its sounds marked to stream for the room (z_utsfx.c's usPreloadOld, ctr_audio_system.c's end), then its
       log line. Named outside .text.late.* so no catch-all above takes it; last, so no earlier late function moves. */
    *(.text.omegaflowey_late)
    *(.rodata.omegaflowey_in_text)
    /* utburst (flowend r37; last, after drpix, inflight and omegaflowey, merge order; UNDERTALE only, empty elsewhere):
       Omega Flowey's vine Create without its sound scripts' frames and the burst's held sound calls (z_utburst.c,
       bound by gmlaot.py's UTF_NATIVE through aot_table.c's wrappers), ctr_audio_system.c's CtrAudio_burstDeferAll,
       then z_utburst.o's constants and anything else of the file. Named outside .text.late.* so no catch-all above
       takes it; last, so no earlier late function moves. */
    *(.text.utburst_late)
    *z_utburst.o(.text .text.* .rodata .rodata.*)
    /* flowerhw D'+E (last, after utburst, merge order; DELTARUNE New only, empty elsewhere in release):
       tower cuts 6, 7 and 7d (vine_new_retain.inc). Last in .late.text, so no earlier late function moves; only
       ld's ARM-to-Thumb veneer, which follows the last code input, moves. Code only: ld puts the veneer after the
       last code input, and a constant after it would sit on the 3dsxtool veneer + 8 relocation (argbind, above),
       so the strings go to late rodata (.rodata.flowerhw_late). */
    *(.text.flowerhw_late)
    *(.text_flowerhw_tail)
    /* knightgpu SY (after flowerhw D'+E, before flowerhw tshare: drpix, FC, then the tower fix, 11-01; DELTARUNE
       New only, empty elsewhere): tiles' copies in VRAM given up only when that makes room (ctr_renderer.c's end).
       Named outside .text.late.* so ctr_renderer.o's own late line above leaves it here. */
    *(.text.knightgpu_sy_late)
    /* FC rev 2 grows SY (kgYieldMirrors gives FC's copies up first): the smallest pad (0-31 B) that puts FC's code
       on stage dr75's cache-line phase (dec-1029-phase-pads, c26rev r103 F2). CMake substitutes KGSY_TEXT_PHASE. */
    __kgsy_text_gap_start = ABSOLUTE(.);
    . += ((/* KGSY_TEXT_PHASE */ 0 - __kgsy_text_gap_start) & 31);
    __kgsy_text_gap_end = ABSOLUTE(.);
    /* knightgpu FC (after SY, before flowerhw tshare; DELTARUNE New only, empty elsewhere): tiles' copies in gaps a
       room's wanted VRAM can't use (ctr_renderer.c's and ctr_memory.c's ends). Named outside .text.late.*. */
    *(.text.knightgpu_fc_late)
    /* FC rev 2's own code (.text.knightgpu_fc_late_fc2), after kgfcBeginFrame, and its vramMemAlign links, a section
       each (dropped where nothing calls vramMemAlign). */
    *(.text.knightgpu_fc_late_*)
    /* Then the smallest pad (0-31 B) that puts flowerhw tshare's code and ld's veneer after it on stage dr75's
       cache-line phase (c26rev r103 F2). CMake substitutes KGFC_TEXT_PHASE. */
    __kgfc_text_gap_start = ABSOLUTE(.);
    . += ((/* KGFC_TEXT_PHASE */ 0 - __kgfc_text_gap_start) & 31);
    __kgfc_text_gap_end = ABSOLUTE(.);
    /* flowerhw tshare (last, after knightgpu FC, merge order; DELTARUNE New only, empty elsewhere): cut 3's shared
       depth texture waits for VRAM at step 0 instead of backing off (vine_new_retain.inc's end; vrTowerDraw's call target
       only). Named outside .text.late.* so ctr_renderer.o's catch-all above leaves it here; last, so no earlier late
       function moves; only ld's ARM-to-Thumb veneer, which follows the last code input, moves. */
    *(.text.tshare_late)
    /* restartexact (last, after flowerhw tshare, merge order; both games): game_restart as the PC runner does it (runner.c's end; runner.o
       is out of the catch-alls above). */
    *(.text.late.restartexact)
    /* racefix (last, after restartexact, merge order): the save writer's retry and the end of "Saving failed" once every
       lost save file is saved again (ctr_file_system.c's end). */
    *(.text.racefix_late)
    /* audofix (c26rev r90 + reviews B and C; last, after racefix, merge order; both games): the options menu's hold
       unpauses its own sound (M1), time away (HOME, sleep, Rosalina) in a load window isn't load time (H1,
       UNDERTALE, ctr_loadaudio_mix.inc's end), soundLength keeps a small WAV's blob while a stream reads it (F1), a
       pending seek clears its stream's channel only while the stream still owns it (F2), and a played-out stream's
       context is freed before a live stream is stopped for one (S2) (ctr_audio_system.c's end). Named outside
       .text.late.* so no catch-all above takes it; last, so no earlier late function moves. */
    /* audpar: split only the existing tail helpers to retain their original order and bytes. */
    *(.text.audofix_late .text.audofix_late_dsp .text.audofix_late_boundary)
    /* hitbox (hitbox 11-01; last, after audofix, merge order; both games, UNDERTALE's path only does work): a
       non-precise sprite's stored collision mask kept at load (DataWin_sep0Masks) and GMS 1.4's sprite_collision_mask
       (collision_gm14.c's end), then its builtin name. Named outside .text.* so collision_gm14.o's line above leaves
       it here; last, so no earlier late function moves. */
    *(.hitbox_late)
    *(.rodata.hitbox_in_text)
    /* utring (flowend r39; last, after hitbox, merge order; UNDERTALE Old only, empty elsewhere): Omega Flowey's ring boss's
       Create (z_utring.c, bound by gmlaot.py's UTF_NATIVE through aot_table.c's wrapper, which is here too), the
       wrapper's operand table, then z_utring.o's constants and anything else of the file. Last, so no earlier late function moves. */
    *(.text.utring_late)
    *(.text.utring_ops)
    *(.utring_sw)
    *z_utring.o(.text .text.* .rodata .rodata.*)
    /* btlentry (last, after utring, merge order; UNDERTALE Old only, empty elsewhere): a battle group's planned tiles
       read on the reader during the encounter animation (z_btlentry.c). Named outside .text.late.* so no catch-all
       above takes it. A fixed 7,428 B slot (lead, 11-02, 75zo: r3 is 4,040 B here and r4 rev 2 + r5 add about 3 KB),
       so its in-place growth no longer moves the late code after it; on UNDERTALE Old only, empty elsewhere, no
       pad. The pad is a declared gap (__btlentry_text_gap_*, zero fill) so check_smoothux_layout names it
       (SLOT_PADS); outgrowing the slot fails the link (the location counter cannot move backwards). */
    __btlentry_late_start = .;
    *(.text.btlentry_late)
    *(.rodata.btlentry_in_text)
    __btlentry_text_gap_start = ABSOLUTE(.);
    . = . > __btlentry_late_start ? __btlentry_late_start + 7428 : .;
    __btlentry_text_gap_end = ABSOLUTE(.);
    /* titandiag (last, merge order; build.sh's titandiag word only, never a release): the Titan CPU recorder
       (z_titandiag.c) and its constants. */
    *(.text.titandiag_late)
    *(.rodata.titandiag_in_text)
    *z_titandiag.o(.text .text.* .rodata .rodata.*)
    /* titancpu L1 (last, merge order; DELTARUNE New only, empty elsewhere): lazy surface hand-off splits
       (vine_titancpu.inc, through vine_old_parent.py's routing). Named outside .text.late.* so that ctr_renderer.o's
       catch-all above leaves it here. */
    *(.text.titancpu_late)
    /* titancpu L4 (last, merge order; DELTARUNE New only, empty elsewhere): path_add_point growth
       (VMBuiltins_titanPathAddPoint, vm_builtins.c's end; vm_builtins.o is out of the catch-all above). */
    *(.text.late.titancpu)
    /* titancpu lever 1 (last, merge order; DELTARUNE New only, empty elsewhere): typed regions' draw_surface_part
       draws (vine_titancpu.inc's Titan_stripDraw, ctr_natsurf6.c's Titan_ns6PassesDraw), after every other late
       function so that none of them moves. KEEP: in knightgpu dspe session builds only Titan_stripDrawDspe (at the
       dspe tail) calls it, outside the Knight and Titan rooms (its room gate), and before that gate nothing did: kept,
       every later late function stays where 75zq has it, whatever calls it (lead ruling, 11-03). */
    KEEP(*(.text.titancpu_tail))
    /* redpix (last, merge order): UT cold tile conversion only; original converter keeps its slot. */
    *z_redpix.o(.text.late.redpix)
    /* pacerflush (pacer gbot; last, after redpix, merge order; both games): main's consoles write-back, the top
       screen's at the frame's end with the game below (z_pacerflush.c). Named outside .text.late.* so no
       catch-all above takes it; last, so no earlier late function moves. */
    *(.text.pacerflush_late)
    /* fpsbox75zm (after pacerflush, merge order; fpsrev: its block grows in place, so anything late after it (audofix_feed)
       needs its exact layout re-run whenever fpsbox changes):
       final-pixel counter composition, after every existing input. */
    *(.text.fpsbox75zm_veneer)
    *(.text.fpsbox75zm_late)
    *(.rodata.fpsbox75zm_late)
    /* audofix drone (playtest 11-01, UNDERTALE): a looping stream of a sound whole in the SFX cache reads the cached copy
       (ctr_audio_system.c's end); its own name, after audofix's, so none of those moves (75zo: last, after fpsbox). The load play-through (11-02,
       ctr_loadaudio_mix.inc's end) is in the same section. Then its log line. */
    *(.text.audofix_feed)
    *(.rodata.audofix_in_text)
    /* dlghold (last, after audofix_feed, merge order; both games): a menu handoff ends the dialogue hold
       (z_dlghold.c). On DELTARUNE New, Crisp art's ARM entry stays fpsbox's retained one
       (.text.fpsbox75zm_veneer, above; z_crispart.c names it): dlghold adds no entry of its own. */
    *(.text.late.dlghold)
    /* flowerhw L2 (last, after dlghold, merge order (75zo; after flowerhw tshare on dr75); DELTARUNE New only, empty elsewhere): the phase plan's
       eviction, which drops the wanted marks of the tiles it took (z_phaseplan.c's ppEvict). Named outside .text.*
       so z_phaseplan.o's catch-all above leaves it here; last, so no earlier late function moves. In 75zo Crisp art's
       ARM entry is fpsbox's retained one (above), not an ld veneer after the last input, so nothing else moves. */
    *(.text_flowerhw_l2)
    /* omegaflowey bubbleread (last, after flowerhw L2, merge order; UNDERTALE only, empty elsewhere): the font-verdict
       chain's link that reads bubbleclip's bubble table in the room change (z_smalltext.c's end). Named outside .text.*
       so z_smalltext.o's line above leaves it here; last, so no earlier late function moves. */
    *(.omegaflowey_bubbleread)
    /* omegaflowey ofloader (last, after bubbleread, merge order; UNDERTALE only, empty elsewhere): the audio loader's
       kept-open files (z_ofloader.c), then its log lines. Named outside .text.* and .rodata.*: no catch-all takes them. */
    *(.omegaflowey_ofloader)
    *(.omegaflowey_ofloader_ro)
    /* ctrlui (last, after omegaflowey ofloader, merge order; both games): the options menu's Controls page
       (ctr_ctrlui.c), then its constants. Named outside .text.late.* so no catch-all above takes them; last, so no
       earlier late code moves. */
    *(.text.ctrlui_late)
    *(.rodata.ctrlui_in_text)
    /* ctrlui open (last, after the Controls page's constants; both games): CtrCtrlui_openPage, the screens' open's end
       (ctr_ctrlui.c). Its own section, so the page's code and constants above keep their 75zp addresses (R119). */
    *(.text.ctrlui_open)
    /* GPU kick (pacer, from cheaplevers; rev3 gate; last, after ctrlui open (75zq), merge order; DELTARUNE New only, empty
       elsewhere): every earlier late function keeps its address. */
    *(.text.cheaplevers_kick_late)
    /* omegaflowey ifg (last, after the GPU kick (75zq), merge order; UNDERTALE only, empty elsewhere): the early destroy of
       bullets inactive for good (z_offkill.c's OffKill_doomed), then its constants. Named outside .text.* and
       .rodata.* so z_offkill.o's line above leaves them here; last, so no earlier late code moves. */
    *(.omegaflowey_ifg)
    *(.omegaflowey_ifg_ro)
    /* omegaflowey W1 (after omegaflowey ifg (75zq), merge order, before the repack test sections; UNDERTALE Old only, empty elsewhere): the preload link that asks
       for a planned compressed sound effect's decode at the room change (ctr_audio_system.c's end, flamePreload). Named
       outside .text.* so no catch-all above takes it; appended, so no earlier late code moves. */
    *(.omegaflowey_flame)
    /* flowerhw sun (after omegaflowey W1 (75zq), merge order, before the repack test sections; DELTARUNE New only, empty elsewhere): the phase plan's detection
       of its next phase and its readied set (z_phaseplan.c's ppNextDrawn, ppNoteReadied). A section of its own so that
       L2's ppEvict keeps its address; appended, so no earlier late code moves. Crisp art's ARM entry is fpsbox's retained
       one (above), not an ld veneer after the last input, so nothing else moves either (check_layout vs 75zq6: 0). */
    *(.text_flowerhw_sun)
    /* gsfix (after flowerhw sun (75zq), merge order, before the repack test sections; both games, UNDERTALE's a stub):
       the game's screen target makes VRAM room instead of aborting (ctr_renderer.c's end, and main.c's
       CtrScreens_menuOnShown), then its constants. Named outside .text.late.* so no catch-all above takes them;
       appended, so no earlier late code moves. */
    *(.text.gsfix_late)
    *(.rodata.gsfix_in_text)
    /* sharebuf (titancpu, after gsfix (75zq unit 16), merge order, before the repack test sections; DELTARUNE New
       only, empty elsewhere): surfaces that share one buffer (ctr_renderer_sharebuf.inc). Named outside .text.late.*
       (ctr_renderer.o's catch-all); appended, so no earlier late code moves. */
    *(.text.sharebuf_late)
    /* savefail (after gsfix and sharebuf (75zq unit 17), merge order, both games): ctr_optmenu.c's
       CtrOptmenu_saveWarn, main.c's SAVE FAILED line kept off an open options menu (chg-savefail-optpage).
       Appended, so no earlier late code moves. */
    *(.text.savewarn_late)
    /* flowerhw cold (after savefail (75zq unit 19), merge order, before the repack test sections; DELTARUNE New only): the
       first room after a save or chapter load finishes its seed (z_drresid.c's CtrRoomload_prefetchRoom wrap), then its
       constants. Named outside .text.* and .rodata.* so z_drresid.o's line above leaves them here; appended, so no
       earlier late code moves. */
    *(.text_flowerhw_cold)
    *(.flowerhw_cold_ro)
    /* drhitbox (after flowerhw cold (75zq unit 20), merge order, before the repack test sections; DELTARUNE only):
       the retained-bbox branch and the collision corrections. Own names bypass earlier catch-alls. */
    *(.text.late.drhitbox_bridge)
    *z_drhitbox.o(.text .text.*)
    /* sharebuf G2 (titancpu, after drhitbox (75zq unit 21), merge order, before the repack test sections; DELTARUNE
       New only, empty elsewhere and in build.sh sharebuf=g1): the dirty-band clears (ctr_renderer_sharebuf_g2.inc).
       Appended, so G1 and everything before keep their addresses (check_sharebuf_layout.py --g2). */
    *(.text.sharebuf_g2_late)
    /* flowerhw stfix (after sharebuf G2 (75zq unit 23), merge order, before the repack test sections; DELTARUNE New only,
       empty elsewhere): smalltext's room-change wrapper, which puts its outer table on a renderer once
       (z_smalltext_dr.c). Named outside .text.* so z_smalltext_dr.o's line above leaves it here; appended, so no
       earlier late code moves. */
    *(.text_flowerhw_stfix)
    /* knightgpu dspe (after flowerhw stfix (75zq21), merge order, before the repack test sections; DELTARUNE New only,
       empty elsewhere and in knightgpu-dspe=off builds): typed regions' draw_surface_part_ext calls
       (Knight_scalarSurfacePartExt and its bound test, vm_builtins.c's end; vm_builtins.o is out of the catch-all
       above), then their draw session (vine_titancpu.inc's Knight_stripDrawExt, named outside .text.* so
       ctr_renderer.o's catch-all leaves it here). Appended, so no earlier late code moves. */
    *(.text.late.knightgpu_dspe)
    *(.text_knightgpu_dspe)
    /* g2log (titancpu, after knightgpu dspe (75zq22), merge order, before the repack test sections; DELTARUNE New
       only, empty elsewhere and in build.sh sharebuf=g1): sharebuf G2's clears line, once a room visit and only when a
       clear was saved (ctr_renderer_sharebuf_g2.inc's sbLogClearsRoom). Named outside .text.* so ctr_renderer.o's
       catch-all leaves it here; appended, so no earlier late code moves. */
    *(.text_titancpu_g2log)
    /* dofl (titancpu, after titancpu g2log (75zq23), merge order, before the repack test sections; DELTARUNE
       only, empty in UNDERTALE, whose ofloader sits above): the audio loader's kept-open files (z_ofloader.c), then
       its log lines. Named outside .text.* and .rodata.*: no catch-all takes them. */
    *(.titancpu_dofl)
    *(.titancpu_dofl_ro)
    /* KM rev 5 (pacer, 75zq append-only port, after titancpu dofl (zq25's last late text), merge order,
       before the repack test sections; DELTARUNE New only, empty elsewhere): the GPU kick's chain, gate and
       frame-end hold (ctr_gpu_kick5.inc, ctr_gpu_kick5_renderer.inc). KM rev 4.1's .text.cheaplevers_kick_late
       above keeps its place and size; its exports branch here. Appended, so no earlier late code moves. */
    *(.text.pacer_km5_late)
    /* pacer C3 (after KM rev 5 (75zq unit 30), merge order, before the repack test sections; UNDERTALE Old only, empty elsewhere): the
       Vorbis codebook share (ctr_audio_system.c's end) and its constants. Own names bypass every catch-all; appended,
       so no earlier late code moves. */
    *(.pacer_share)
    *(.pacer_share_ro)
    /* weirdshd pinwheel (after pacer C3 (75zq unit 31), merge order, before the repack test sections; DELTARUNE New only,
       empty elsewhere): the certified pinwheel renderer (ctr_pinwheel.inc, ctr_pinwheel_fast.inc) and its in-text
       table. Own names bypass every catch-all; appended, so no earlier late code moves. */
    __weirdshd_pinwheel_start = ABSOLUTE(.);
    *(.rodata.weirdshd_pinwheel_in_text)
    *(.text.weirdshd_pinwheel_late)
    __weirdshd_pinwheel_end = ABSOLUTE(.);
    /* flowerhw vrgen (after weirdshd pinwheel (75zq unit 34), merge order, before the repack test sections; DELTARUNE
       New only, empty elsewhere): the tower code forgets a renderer when it goes (vine_new_retain.inc's
       Vine_rendererGone, through vine_old_observer_compile.py's routing of screensDestroy's tail call). Named outside
       .text.* so ctr_renderer.o's catch-all leaves it here; appended, so no earlier late code moves. */
    *(.text_flowerhw_vrgen)
    /* titancpu L-F (after flowerhw vrgen (75zq unit 35), merge order, before the repack test sections; DELTARUNE New
       only, empty elsewhere): the fountain mesh's copy with tile reuse and the clip skip (vine_titancpu_lf.inc's
       Titan_lfMeshRegion, through vine_old_observer_compile.py's routing of fountainMeshRegion's two callers; the
       old body keeps its place in .text.late.pillar). Named outside .text.* so ctr_renderer.o's catch-all leaves it
       here; appended, so no earlier late code moves. */
    *(.text_titancpu_lf)
    /* omegaflowey pcorder (drawonce rev 3; after titancpu L-F (75zq unit 37), merge order, before the repack test
       sections; UNDERTALE only, empty in DELTARUNE): utkDrawRoom's mid-pass step in GameMaker 1.4's order (runner.c's
       end). Named outside .text.* so runner.o's lines above leave it here; appended, so no earlier late code moves. */
    *(.text_omegaflowey_pcorder)
    /* utlife A (after omegaflowey pcorder (75zq unit 39), merge order, before the repack test sections; UNDERTALE
       only, empty in DELTARUNE): Outside Room's inclusive compatibility bounds. Appended, so no earlier late code
       moves. */
    *(.text.late.utlife)
    /* spdmemo (creddodge, after utlife A (75zq unit 40), merge order, before the repack test
       sections; both games): the speed/direction memo in front of Instance_computeSpeedFromComponents
       (z_spdmemo.c, reached through --wrap).
       Named outside .text.late.* so the catch-alls leave it here; appended, so no earlier late code moves. */
    *(.text_spdmemo)
    /* ofpoll (creddodge, after spdmemo, merge order, before the repack test sections; both games):
       CtrOpenFiles_poll's check of the keepers without the lock (ctr_open_files.c's end, reached through --wrap).
       Named outside .text.* so the catch-alls leave it here; appended, so no earlier late code moves. */
    *(.text_ofpoll)
    /* glyphsafe (auditfixe DEEP-02, after ofpoll, merge order; both games): the check that argui's text has
       every font page and tile it reads (z_glyphsafe.c, reached through --wrap), then that file's out-of-line
       copies of header functions (none expected). Named outside .text.late.*; appended, so no earlier late code
       moves. */
    *(.text_glyphsafe)
    *z_glyphsafe.o(.text .text.* .rodata .rodata.*)
    /* inisafe (auditfixe DEEP-01, after glyphsafe, merge order, before the repack test sections; both games): the
       INI cache drop in front of every builtin that changes a file (z_inisafe.c, reached through --wrap), then its
       constants, then that file's out-of-line copies of header functions (none expected). Named outside
       .text.late.* so the catch-alls leave it here; appended, so no earlier late code moves. */
    *(.text_inisafe)
    *(.rodata.inisafe_in_text)
    *z_inisafe.o(.text .text.* .rodata .rodata.*)
    /* dndrel (auditfixe SCOURS3-002, after inisafe, merge order; UNDERTALE, empty in DELTARUNE): the relative
       action_set_motion and Hitbox_registerAll's wrap (z_dndrel.c, reached through --wrap), its names, then that
       file's out-of-line copies of header functions; appended. */
    *(.text_dndrel)
    *(.rodata.dndrel_in_text)
    *z_dndrel.o(.text .text.* .rodata .rodata.*)
    /* bakkeep (auditfixe SCOURSOL2-001, after dndrel, merge order; both games): CtrSafeSave_forgetBackup's wrap
       (z_bakkeep.c, reached through --wrap), then that file's out-of-line copies of header functions; appended. */
    *(.text_bakkeep)
    *z_bakkeep.o(.text .text.* .rodata .rodata.*)
    /* gdretry (auditfixe SCOURT6-001 = SCOURS2-003, after bakkeep, merge order; both games): z_gamedir.c's
       failed settings.ini write kept owed and tried again at the exit (gdWritten, gdNotWritten, which gdSaveNow calls
       in place of logInfo, and gdOwedAtExit with gdStopWorker, R176 F1), and the listing-off hook (gdLogInfo,
       gdKeepOn, and its branches gdKeepPaths and gdKeepRecover, R176 F2). Named outside .text.late.*, so
       z_gamedir.o's rule above leaves it here; appended, so no earlier late code moves. */
    *(.text_gdretry)
    /* symkeep (auditfixe SCOURT5-003, after bakkeep, merge order; UNDERTALE, empty in DELTARUNE): the symverdict
       link's wrap that gives symmetric halving's copy array its whole capacity while it is empty, CtrArray_add's
       that draws the batches before that array grows (R193), the caps link's that names the renderer (z_symkeep.c,
       reached through --wrap), then that file's out-of-line copies of header functions; appended. */
    *(.text_symkeep)
    *z_symkeep.o(.text .text.* .rodata .rodata.*)
    /* credfail (auditfixe DEEP-11, after symkeep, merge order; UNDERTALE, empty in DELTARUNE): CtrLegalui_open's
       wrap that says on the Credits page when it has no credits (z_credfail.c, reached through --wrap), its text,
       then that file's out-of-line copies of header functions; appended. */
    *(.text_credfail)
    *(.rodata.credfail_in_text)
    *z_credfail.o(.text .text.* .rodata .rodata.*)
    /* sleepflush (auditfixe SCOURS2-005, after gdretry, merge order; both games): the APT hook that flushes the
       save queue on sleep and HOME, and main.c's CtrFileSystem_create/destroy wraps (z_sleepflush.c, reached through
       --wrap), then that file's out-of-line copies of header functions (none expected); appended. */
    *(.text_sleepflush)
    *z_sleepflush.o(.text .text.* .rodata .rodata.*)
    /* sleepflush r2 (auditfixe3, SCOURS2-005 r2 / R178 F1; both games): the deadline log line's named arrays
       (z_sleepflush.c; not a mergeable string section, so no earlier late string folds into them), then the bounded
       flush (ctr_file_system.c's CtrFileSystem_flushBy, z_sleepflush.c's sleepflushBounded). After r1's code, which
       keeps its bytes but the hook's call target. Appended. */
    *(.rodata_sleepflush)
    *(.text_sleepflush_r2)
    /* auditfixh (after dndrel, merge order; UNDERTALE only): the menu's caps query under the state hash. */
    *(.text.late.auditfixh)
    /* wrapfix (auditfixe SCOURT2-001, after credfail, merge order; UNDERTALE, empty in DELTARUNE): draw_text_ext's
       copy whose wrap keeps a too-wide word whole, and UtKill_registerAll's wrap that registers it (z_wrapfix.c,
       reached through --wrap), its name, then that file's out-of-line copies of header functions; appended. */
    *(.text_wrapfix)
    *(.rodata.wrapfix_in_text)
    *z_wrapfix.o(.text .text.* .rodata .rodata.*)
    /* overlayread (auditfixe3, SCOURSOL3-002, after sleepflush r2; both games): the overlay file system's checked
       whole-file readers and the OverlayFileSystem_create wrap (z_overlayread.c, reached through
       -Wl,--wrap=OverlayFileSystem_create), then its one message (a named array, not a mergeable string). Appended. */
    *(.text_overlayread)
    *(.rodata_overlayread)
    *z_overlayread.o(.text .text.* .rodata .rodata.*)
    /* overlayread R195c (auditfixe3): ctr_file_system.c's CtrFileSystem_readStrict, readWhole for
       z_overlayread.c's CtrFileSystem_readWhole (readSaveFile's and readIndexed's whole-file read). Appended. */
    *(.text_overlayread_fs)
    /* credclip (auditfixe CREDPOP-001, after wrapfix, merge order; UNDERTALE, empty in DELTARUNE): the menu font
       painter's wrap that kept the Credits roll's rows outside its window black (z_credclip.c; retired by credroll
       below, it keeps its slot as credclipRetired_text, never called), then that file's out-of-line copies of header
       functions; appended. KEEP (CREDCLIP-002): nothing refers to the retired wrap, and the slot must keep its bytes. */
    KEEP(*(.text_credclip))
    *z_credclip.o(.text .text.* .rodata .rodata.*)
    /* codeverify (auditfixe2 AUD-16, after credclip, merge order, before the repack test sections; UNDERTALE Old
       only, empty elsewhere): rename's wrap, which reads the bytecode file back on the boot that writes it, the
       move's hook that names that file (z_btlentry.c's end), then its mode string ("rb"). Named outside
       .text.late.*; appended. */
    *(.text_codeverify)
    *(.rodata.codeverify_in_text)
    /* legalheld (auditfixe2 SCOURSOL2-004, after codeverify, merge order; both games): the wrap of
       CtrLegalui_update that keeps a held key repeating past 255 frames (ctr_legal.c's end). Named outside
       .text.late.*; appended, so CtrLegalui_update keeps its size (RULES 3adf7ac34). */
    *(.text_legalheld)
    /* argcrashfix (auditfixe2 SCOURS4-004, after legalheld, merge order; both games): the crash screen's
       painter with the look-ahead wrap and the pixel bound (z_argcrash.c's end; SCOURS4-005's gate is .text_argcrashgate,
       after dlgtop). Named outside .text.late.*; appended, so .text.late.argcrash keeps its size and every symbol its
       place (RULES 3adf7ac34). */
    *(.text_argcrashfix)
    /* utfade (utfade, after overlayread R195c; UNDERTALE only, empty in DELTARUNE): a door's room change
       shows black while the room loads: the room change link (z_utfade.c) and the pacer's black (ctr_frame_pacer.c's
       end), then z_utfade.c's out-of-line copies of header functions (none expected). Its two object names are late
       constants (.rodata.utfade_late). Appended. */
    *(.text_utfade)
    *z_utfade.o(.text .text.*)
    /* datarefuse (auditfixe, playblocker #8, after argcrashfix, merge order; UNDERTALE only, empty in DELTARUNE): main.c's
       data-version notice (z_datarefuse.c, reached through a compile definition on main.c). Since R243b only the
       44 B slot R215 had here: a branch to the code in .text_datarefuse_warn, after GOG's below, so GOG's late code
       keeps its addresses. */
    *(.text_datarefuse)
    /* gogut (GOG UNDERTALE only, empty in every Steam and DELTARUNE build; moved here by integrate21 zq44g, betasmoke
       R2-B2): GOG's own late code (z_gogut.c, ctr_renderer.c's Smart texel and font-folder helpers, ctr_pixgrid.c's
       data check), then its constants. It sat after round 16 (and ctr_renderer.o's .text.late.* line took most of it
       earlier still), ahead of the freebytes H1 holes and the other fixed gaps whose ends come from the Steam layout:
       5,146 B there pushed relprep and asgborder past their limits, and no release build of GOG UNDERTALE linked on
       the Old 3DS. Here, after every fixed hole, the layout up to the holes is Steam's; GOG's two extra late pages
       take the code. Named .text_gogut, outside .text.late.*, so no catch-all or per-file late line takes it first
       (tools/release/test_gogut_late_order.py pins both). Later appends go AFTER this block (lead ruling 11-05):
       dlgtop and utfade A follow it, so code put above it would move them in Steam builds and every GOG late
       symbol in GOG builds. Each new unit appends at the end of late text, before the repack test pair, and adds
       its input to test_gogut_late_order.py's AFTER_GOGUT (or AFTER_GOGUT_OWNERS for a per-object catch-all). In
       GOG builds the inputs after this block sit gogut's size later than in Steam; nothing before it moves. */
    *(.text_gogut)
    *(.rodata.gogut_in_text)
    /* dlgtop UT-only cold loading helpers, after all release inputs. */
    *(.text.late.dlgtop)
    *(.rodata.late.dlgtop)
    /* utfade A (utfade, after dlgtop, the last release input, merge order; UNDERTALE Old only, empty elsewhere):
       a door's destination read during its fade-out (z_btlentry.c's end), then its object names and log lines. Named
       outside .text.late.* and .rodata.* so no catch-all takes them; appended, so no earlier late code moves. */
    *(.text_utfadea)
    *(.rodata.utfadea_in_text)
    /* datarefuse warn (auditfixe R243b, after gogut and utfade A; UNDERTALE only, empty in DELTARUNE): the data.win warning, the
       crash and hang reports' UNSUPPORTED line and the offkill bypass (z_datarefuse.c; its old slot is above). After
       GOG's code so every GOG late address stays; its address differs between Steam and GOG builds. */
    *(.text_datarefuse_warn)
    *z_datarefuse.o(.text .text.*)
    /* rosabl (rosafreeze3 bug 2, after the datarefuse warn, merge order; both games): the Rosalina watcher's
       buttons through the combo's hold and the app's bottom power-off that the hold keeps back (z_rosabl.c, reached
       through names CMakeLists.txt changes in ctr_rosalina.c and ctr_screens.c), then that file's out-of-line copies
       of header functions and its constants; named outside .text.late.*, so the catch-alls leave it here. After
       GOG's block (AFTER_GOGUT); appended, so every earlier late symbol keeps its place. */
    *(.text_rosabl)
    /* dimlight: do not consume this object's new .text.late.dimlight getter.
       Shipped Old/New maps have only .rodata here; retain internal header helpers explicitly. */
    *z_rosabl.o(.text .text.__* .rodata .rodata.*)
    /* thinart r4: reviewed private EX-entry artwork, appended after landing inputs. */
    *(.text.late.thinart)
    /* auditfixg-r2 SCOURS3-004: save-provided room-name bounds helper and its
       fallback string, in code space so the late-rodata reserve is untouched. */
    *(.auditfixg_r2_room_text)
    *(.rodata.auditfixg_r2_room_in_text)
    /* auditfixg-r2 SCOURS4-003: shared initial rule-file load and signed OBJT
       bounds refusal; original offkill/doom input sections retain their extents. */
    *(.auditfixg_r2_offrules_text)
    /* auditfixg-r2 SCOURS2-009: owned heartbeat creation, bounded joins and
       the exit-owner adapter; no additional per-frame operations or strings. */
    *(.auditfixg_r2_heartbeat_text)
    /* regaudit F2: wakeable heartbeat waits and nonblocking fatal exit. */
    *(.auditfixg_f2_heartbeat_text)
    /* Newly reachable libctru timeout adapter (40 B, discarded in all four
       frozen R2 links). Keep its first live copy out of the frozen warm text. */
    *libctru.a:synchronization.o(.text.syncArbitrateAddressWithTimeout)
    /* pacer A + E1 (after auditfixg, merge order; UNDERTALE Old only, empty elsewhere): the audio threads' arena
       allocator (ctr_audio_system.c's end) and its log formats. Own names bypass every catch-all. Appended after
       the latest release input (AFTER_GOGUT), so every earlier Steam and GOG late address stays put. */
    *(.pacer_arena)
    *(.pacer_arena_ro)
    /* rngexact (after pacer E1, merge order; both games): native RNG mappings and builtin wrappers, including
       header helpers and constants. Appended after the latest release input (AFTER_GOGUT), so every earlier
       Steam and GOG late address stays put. */
    *z_rngexact*(.text .text.* .rodata .rodata.*)
    /* utsdread (auditfixe2 UTSCOURD-001, after rngexact, merge order; UNDERTALE only, empty in DELTARUNE): the crash screen's SD card read
       title: argcrashSdOut, which notes whether the latest fatal line is an SD read, and argcrashPaintSd, which paints
       that title (z_argcrash.c's end), then its constants. Named outside .text.late.*. After GOG's block and dlgtop's
       lines (R238 placement fix, 11-05: above GOG's block it moved GOG's late code and constants in GOG builds),
       so no earlier late address moves in any build (RULES 3adf7ac34). */
    *(.text_utsdread)
    *(.rodata.utsdread_in_text)
    /* utsdmem (auditfixe2 UTSCOURD F4, regaudit F4, after utsdread; UNDERTALE only, empty in DELTARUNE): the
       data.win check's buffer allocation, noted (ctr_data_version.c's CtrDataVersion_buffer), and the crash screen's
       paint call that shows its failure as out of memory (z_argcrash.c's argcrashPaintMem). */
    *(.text_utsdmem)
    /* auditfixg r1 port (after utsdmem, merge order; UNDERTALE only): cold reseed and isolated health/lives dispatch.
       Preserve all earlier owners, including the GOG block and dlgtop KEPT inputs. */
    *random.o(.text.late.auditfixg)
    *runner.o(.text.late.auditfixg)
    /* pacealign (auditfixe Option A, after the auditfixg r1 port, merge order; UNDERTALE only, empty in DELTARUNE): a bench
       window's edges cut and log the pacer's numbers (z_pacealign.c, ctr_benchmark.c's and ctr_frame_pacer.c's
       ends), then its log lines. Named outside .text.late.* and .rodata.* so no catch-all takes them; appended, so no
       earlier late code moves. */
    *(.text_pacealign)
    *(.rodata.pacealign_in_text)
    /* SCOURS4-005 and the fatal claim's guards (auditfixe2 R216b, after pacealign, merge order; R216d: UNDERTALE
       only, all empty in DELTARUNE: source/ctr_r216_ut.h), appended so no earlier late code or constant moves (RULES
       3adf7ac34): the loading bar's
       draw gate and the crash report's halt of it (z_loadscreen.c's end: lsDraw, psDraw, CtrLoadscreen_halt,
       CtrLoadscreen_drawing, lsSwapTop), the gate every crash-screen paint goes through and the painter's release
       when its thread parks for good (z_argcrash.c's end: argcrashPaintGated, argcrashLcdSkip, CtrArgcrash_labSleep),
       the fatal claim's clock (ctr_lab.c's end: CtrLab_fatalAt, and since R216b4 the claimants' way-in clock
       trampolines), the save guard (ctr_file_system.c's end: no save after a fatal claim, a deadline for those
       before it), and the Rosalina watcher's stop before a crash screen paints (R240 F1, ctr_rosalina.c's end:
       CtrRosalina_quiet; R216b5: CtrRosalina_dark and CtrRosalina_threadPub there, and z_argcrash.c's
       CtrArgcrash_quiet; R216c: its CtrArgcrash_depart; R216d: CtrRosalina_watchRun, the watcher's thread, which
       lights the bottom after a crash stop). Named outside .text.late.* so the
       catch-alls leave them here. No constants (no late rodata). */
    *(.text_lshalt)
    *(.text_argcrashgate)
    *(.text_labfatal)
    *(.text_saveguard)
    *(.text_rosaquiet)
    /* leftcode (auditfixe R295, after R216's .text_rosaquiet, merge order; UNDERTALE Old only, empty elsewhere): the room's
       object code that the room load's first step left, read after the whole room load (z_roomload.c's end, reached
       through ctr_argui.c's call by a name CMakeLists.txt changes), then its log line. Named outside .text.late.*;
       appended (AFTER_GOGUT), so every earlier late symbol keeps its place. No statics of its own (rl's). */
    *(.text_leftcode)
    *(.rodata.leftcode_in_text)
    /* codeclose's wrap after review R300b (auditfixe, after leftcode, merge order; UNDERTALE Old only, empty
       elsewhere): ccRoomCode, the body the wrap's kept place in .text.codeclose_late branches to (z_roomload.c's
       end). Appended (AFTER_GOGUT), so every earlier late symbol keeps its place. No constants or statics of its own. */
    *(.text_ccfix)
    /* dlgtop F3/F7 (after codeclose's .text_ccfix, merge order): cold console carry backup and bottom-only
       first-picture gate. */
    *(.text.dlgtopfix_late)
    /* R270: guarded cold carry bodies, appended after the reviewed player text. */
    *(.text.late.dlgtopguard)
    /* P5-1 (after dlgtop's .text.late.dlgtopguard, merge order): public UT's file-trigger refusal, appended
       after every landing input. */
    *(.text_p51)
    *(.rodata.p51_in_text)
    /* utcredepi R327 (after P5-1's .rodata.p51_in_text, merge order; UNDERTALE Old only, empty elsewhere):
       __wrap_CtrTextures_roomLoadedOld, room-load promotion intent. Appended (AFTER_GOGUT). */
    *(.text_utcredepi)
    /* credroll (creddodge CREDCLIP-002, after utcredepi, merge order; UNDERTALE, empty in DELTARUNE): the menu
       font painter's wrap that draws the Credits roll to the screen's top and bottom edges (z_credclip.c, reached
       through --wrap; CREDPOP-001's wrap keeps its slot above as credclipRetired_text). Named outside .text.late.*
       and outside z_credclip.o's catch-all; appended, so no earlier late code moves. */
    *(.text_credroll)
    /* dlglinger (UNDERTALE only): reject stale missing-draw dialogue replay. */
    *(.text.late.dlglinger)
    /* Shared fatal exit (saveaudit R339, after dlglinger, merge order): disable painting before graphics release; no state. */
    *(.text_fatalgfx)
    /* crispgap (after fatalgfx, merge order; UNDERTALE only, empty in DELTARUNE): Crisp text's even gaps (z_smalltext.c's
       end). Named outside .text.* so no catch-all takes it; appended, so no earlier late code moves. */
    *(.text_crispgap)
    /* smoothdef (after crispgap, merge order; UNDERTALE only, empty in DELTARUNE): settings.ini's Smoothing part lines
       as they were meant when saved (z_smalltext.c's stReadFileParts), and the help lines that say Recommended. */
    *(.text_smoothdef)
    *(.text_smoothdef_str)
    /* thinart edge (R336, after smoothdef, merge order): UT battle-border coverage; outside .text.late.* so the
       earlier catch-all cannot move existing inputs. Inside edge (R336b): PC coverage for the star attack's
       backing outline. */
    *(.text.thinart_edge_late)
    *(.text.thinart_outline_late)
    /* bulletsplit (creddodge, after thinart outline, merge order; UNDERTALE Old writerfront builds only, empty elsewhere):
       the credits bullets' text drawn after their run (z_bulletsplit.c), its constants and that file's out-of-line
       copies of header functions. Named outside .text.late.* so no catch-all takes it; appended, so no earlier late
       code moves. */
    *(.text_bulletsplit)
    *(.rodata.bulletsplit_in_text)
    *z_bulletsplit.o(.text .text.* .rodata .rodata.*)
    /* saveaudit: strict UNDERTALE startup directory reads and backup recovery. */
    KEEP(*z_saveaudit.o(.text.late.saveaudit))
    /* saveaudit: native UT text/INI serialization and its out-of-line helpers. */
    *z_saveaudit_format.o(.text .text.*)
    /* R333: bounded foreign-entry skips and explicit startup error text. */
    *z_saveaudit_scan.o(.text .text.*)
    *(.text_saveauditfatal)
    *(.rodata_saveauditfatal)
    /* gomusic (omegaflowey Bug 1, after saveaudit, merge order; UNDERTALE only, empty in DELTARUNE): copies of one
       song started in one step join sample-locked (ctr_audio_system.c's end), then their log line. Named outside
       .text.late.* and .rodata.* so no catch-all takes them; appended, so no earlier late code moves. */
    *(.text_gomusic)
    *(.rodata.gomusic_in_text)
    /* sfxshed (omegaflowey, after gomusic, merge order; UNDERTALE Old only, empty elsewhere): a whole set's room
       load sheds cached sound effects for the set tiles the linear floor stopped (z_utstall.c's end, utshedFinish; R4's
       call goes there, its only change), then its log line. Named outside .text.* and .rodata.* so no catch-all takes
       them; appended, so no earlier late code moves. */
    *(.text_sfxshed)
    *(.rodata.sfxshed_in_text)
    /* audpar (last, merge order; UNDERTALE only): paused audio query membership. */
    *(.text.late.audpar)
    *(.text.late.audpar.offset)
    /* gounder (gounder, after audpar, merge order; UNDERTALE Old only, empty elsewhere): copies of the song at unequal
       pitches share one decode (ctr_audio_system.c's end), then its log lines. Named as gomusic's, for the same reason. */
    *(.text_gounder)
    *(.rodata.gounder_in_text)
    /* sansspawn (after gounder, merge order; UNDERTALE Old only, empty elsewhere): draw_self_border_true's first call
       on an instance (z_sansspawn.c, and aot_table.c's aotuf_<k>). Named outside .text.late.*, so no catch-all takes
       it; appended, so no earlier late code moves. No constants, no statics. */
    *(.text_sansspawn)
    /* crisphud (after sansspawn, merge order; UNDERTALE only, empty in DELTARUNE): the battle HUD out of crispgap's even
       gaps (z_smalltext.c's end). Named outside .text.*, so no catch-all takes it; appended, so no earlier late code moves. */
    *(.text_crisphud)
    /* credwait (crispgap, after crisphud, merge order; UNDERTALE only, empty in DELTARUNE): a moved Legal or Credits
       page waits for its screen's vblank before the copy (z_credclip.c's end, reached from ctr_legal.c's wrap). Named
       outside .text.* so z_credclip.o's catch-all leaves it here; appended, so no earlier late code moves. */
    *(.text_credwait)
    /* menupress (menusound, after credwait, merge order; both games): a press in the options menu has its sound
       only if it changed something (ctr_optmenu.c's optKeysHeard, and the hooks at the end of ctr_legal.c and ctr_ctrlui.c).
       Named outside .text.*, so no catch-all takes it; appended, so no earlier late code moves. Its statics are
       .bss.menupress_late, below; its constants follow it (polish audit's new row text). */
    *(.text_menupress)
    *(.rodata.menupress_in_text)
    KEEP(*(.rodata.noticekeep_in_text)) /* noticeKeep: no code refers to it */
    /* homeaudio (after menupress, merge order; UNDERTALE only, empty in DELTARUNE): the sound stops with the picture on HOME
       (z_argcrash.c's and ctr_audio_system.c's ends). Named outside .text.*, so no catch-all takes it; appended, so no
       earlier late code moves. No constants. With it libctru's aptShouldJumpToHome (its one caller is this code, so the
       link only now keeps it; in .text it would move every warm function after apt.o's by 56 B). */
    *(.text.aptShouldJumpToHome)
    *(.text_homeaudio)
    /* poffroom (after credwait, merge order; UNDERTALE Old only, empty elsewhere): the snow floor's Draw at rest
       (z_snowfloor.c, its wrapper and operands from aot_table.c) and draw_circle's fan as quads (ctr_renderer.c's end).
       Named outside .text.* so ctr_renderer.o's catch-all leaves it here; appended, so no earlier late code moves. */
    *(.text_snowfloor)
    *(.text_snowfloor_ops)
    *(.text_fanquads)
    /* dlgblank (crispgap, after poffroom, merge order; UNDERTALE only, empty in DELTARUNE): a bottom transfer into
       the buffer on screen starts in the bottom's blank (ctr_gpu_queue.c's end). Named outside .text.* so its catch-all
       leaves it here; appended, so no earlier late code moves. */
    *(.text_dlgblank)
    /* menuhold (menusound, review R379 F2, after dlgblank, merge order; both games): held Up or Down on the options
       menu's lists, the ">" on the rows that open a page, and the FPS counter's corner over that menu (ctr_optmenu.c, and the
       two list ends in ctr_ctrlui.c and ctr_legal.c), then its constant. Named outside .text.*, so no catch-all takes it;
       appended, so no earlier late code moves. */
    *(.text_menuhold)
    *(.rodata.menuhold_in_text)
    /* keep-lit (dlglight, after every other append, merge order; UNDERTALE only, empty in DELTARUNE): the bottom
       backlight stays on with Other screen Off and dialogue there (z_keeplit.c). Appended, so no earlier late code moves.
       It is the last S7 line: R380c's UT-001 lowering, then the first-update appends, follow it. */
    *z_keeplit.o(.text .text.* .rodata .rodata.*)
    /* UT-001 lowering (rosaport, after keep-lit, merge order; UNDERTALE Old only, empty elsewhere): z_rosalow.c's
       code and constants, and ctr_rosalina.c's one function for it. Named outside .text.late.*, so the catch-alls leave
       it here; appended, so no earlier late code moves. */
    *(.text_rosalow)
    *z_rosalow.o(.text .text.* .rodata .rodata.*)
    /* First update (upd1), appended after every S7 line and R380c's lowering, in merge order, so no S7 late code moves. */
    /* upd1-s7: fallible room-code arena, first after the lowering. */
    *(.text.late.upd1)
    /* parity: saved-room reinsertion order, after upd1-s7; no per-frame code or state. */
    *(.text.late.parity)
    /* drawbatch seam copy (after parity, merge order; UNDERTALE only, empty elsewhere): a fine draw whose texels span
       tiles draws from one copy of its tiles' parts (ctr_renderer.c's end) and its log formats. Named outside
       .text.late.* so ctr_renderer.o's catch-all leaves it here. */
    *(.drawbatch_text)
    *(.drawbatch_text_ro)
    /* oflog (first update, bootmask, after drawbatch, merge order; UNDERTALE only, empty in DELTARUNE): a room
       change's open-files line skips the handle census where the kernel gives no count (z_oflog.c, through --wrap).
       Its code and its one string; appended, so no earlier late code moves. */
    *z_oflog.o(.text .text.* .rodata .rodata.*)
    /* entryload partial tiles (entryload, after oflog, merge order; UNDERTALE Old only, empty elsewhere):
       the dialogue fonts' tiles kept packed in the heap (ctr_textures.c's end), then its constants. Named
       outside .text.*, so no catch-all takes it; appended, so no earlier late code moves. */
    *(.text_entryload)
    *(.rodata.entryload_in_text)
    /* thinrule (first update, after entryload, merge order; R414 F2: after every late line S8a links; UNDERTALE
       only, empty in DELTARUNE): Smart smoothing's whole-rows table (z_thinrule.c) and the fallback that draws from
       it (ctr_renderer.c; emitPageRegionVisible stays in place, only its call to smartEmitAveraged now goes to
       thinruleAveraged), then its constants. Named outside .text.* and .rodata.*, so no catch-all takes them;
       appended, so no earlier late code moves. */
    *(.text_thinrule)
    *(.rodata.thinrule_in_text)
    /* stackfloor (stackfloor, after thinrule, merge order; R414 F2 rule: after every late line S8b links;
       UNDERTALE only, empty in DELTARUNE): no thread stack under 8 KiB; ctr_memory.c's threadCreate wrap calls
       it (z_stackfloor.c). Appended, so no earlier late code moves. */
    *z_stackfloor.o(.text .text.* .rodata .rodata.*)
    /* menufirst (menufirst, after stackfloor, merge order (F2); UNDERTALE only, empty in DELTARUNE): the options menu's
       sounds cached at each room load and its page copied at its screen's vblank (ctr_optmenu.c's end). Named outside
       .text.*, so no catch-all takes it; appended, so no earlier late code moves. No constants, no statics. */
    *(.text_menufirst)
    /* scanout (scanout, after entryload on S8a; in S8b after menufirst and every append merged before it, merge
       order; UNDERTALE only, empty in DELTARUNE): a setup notice's page painted in its screen's spare buffer and copied
       at that screen's vblank (z_scanout.c). Named outside .text.*, so no catch-all takes it; appended, so no earlier
       late code moves. No constants, no statics. */
    *(.text_scanout)
    /* packopen (packopen, after entryload on S8a; in S8b after scanout and every append merged before it, merge
       order): startup-only loader handle warming (ctr_audio_system.c's end); no data or bss. The catch-all
       excludes ctr_audio_system.o, so this line places it; appended, so no earlier late code moves. */
    *(.text.late.packopen)
    /* loadclass (loadclass, after packopen, merge order; UNDERTALE only, empty in DELTARUNE): the opener's note
       link that opens the intro story's song at boot and asks flowend's phase-song spare for the title's noise
       and the cast roll's first song (ctr_audio_system.c's end). The catch-all excludes ctr_audio_system.o, so
       this line places it; appended, so no earlier late code moves. No constants, no statics. */
    *(.text.late.loadclass)
    /* musicloop (musicloop, after menufirst on its branch; in S8b after loadclass, merge order; UNDERTALE only,
       empty in DELTARUNE): a stopped stream's PC fade and the both-channel loop seam (ctr_audio_system.c's end).
       Named outside .text.*, so no catch-all takes it; appended, so no earlier late code moves. No constants, no
       statics. */
    *(.text_musicloop)
    /* errtext E1 (errtext, after keep-lit on S7; in S8b after musicloop, merge order; UNDERTALE only, empty
       in DELTARUNE): expanded button names and game-font sprites (z_errtext_e1.c), its constants and
       ctr_renderer.c's two words for it. No file wildcard above matches z_errtext_e1.o; appended, so no
       earlier late code moves. */
    *(.text.errtext_e1)
    *(.rodata.errtext_e1_in_text)
    *z_errtext_e1.o(.text .text.* .rodata .rodata.*)
    /* menuborder (menuborder, after packopen on its branch; in S8b after errtext E1, merge order; UNDERTALE
       only, empty in DELTARUNE): the menu frame's third ring, drawn on the spare page just before its copy
       (z_menuborder.c). Named outside .text.*, so no catch-all takes it; appended, so no earlier late code moves. */
    *(.text_menuborder)
    /* bonepulse (bonepulse, after packopen on its branch; in S8b after menuborder, merge order): bone objects'
       sprite parts and rectangles moved as one piece on a halved target (ctr_renderer.c's end), then its constants
       (the three object names). Named outside .text.* and .rodata.*, so no catch-all takes them; appended, so no
       earlier late code moves. */
    *(.text_bonepulse)
    *(.rodata.bonepulse_in_text)
    /* snowkick (snowkick, after packopen on its branch; in S8b after bonepulse, merge order; UNDERTALE Old
       only, empty elsewhere): the snow floor's Draw with kicked balls (z_snowkick.c), then gmlaot's wrapper for it
       and its operands (aot_table.c, UTF_NATIVE_NEXT). Named outside .text.*, so no catch-all takes them; appended,
       so no earlier late code moves. No statics; its constants are in its code. */
    *(.text_snowkick)
    *(.text_snowkick_ops)
    /* thinslot (logsweep, after entryload on S8a; in S8b after snowkick, merge order; UNDERTALE only, empty in
       DELTARUNE): UT-028's bubble art after any number of Smart copies and on GOG, as links wrapped around thinart's
       unchanged ones (z_thinart.c's end), then its one constant. Named outside .text.* and .rodata.*, so no catch-all
       takes them; appended, so no earlier late code moves. Its one flag is in the padding of thinart's statics. */
    *(.text_thinslot)
    *(.rodata.thinslot_in_text)
    /* noticescroll (after packopen on its branch; in S8b after thinslot, merge order): the Legal/Credits bulk
       clear, with its r1-preserving ABI bridge. No state. ctr_legal.o is excluded from the catch-all; appended, so
       every existing symbol keeps its address. */
    *(.text.late.noticescroll)
    /* menuquiet (menuborder, 11-08; in S8b after noticescroll, merge order): the options menu's silent
       back/close classification (ctr_optmenu.c). Appended after every composed input, so no earlier late code
       moves. */
    *(.text_menuquiet)
    /* gmlstubs (gmlstubs, R424b; after entryload on S8a, in S8b after menuquiet, merge order): legacy
       background variables, code and constants both in appended late text, then its bridge. Appended, so no
       earlier late code moves. */
    *z_gmlstubs.o(.text .text.* .rodata .rodata.*)
    *(.gmlstubs_bridge)
    /* papvoice (papvoice, after packopen on its branch; in S8b after gmlstubs, merge order; UNDERTALE only,
       empty in DELTARUNE): the sound plan's small text voices read at their play (z_utsfx.c's and
       ctr_audio_system.c's ends). Named outside .text.*, so no catch-all takes it; appended, so no earlier late
       code moves. No constants, no statics. */
    *(.text_papvoice)
    /* popbot (popbot, after menuborder on its branch; in S8b after papvoice, merge order; UNDERTALE only,
       empty in DELTARUNE): overworld popups on
       the bottom screen with the dialogue (z_popbot.c), its constants and ctr_renderer.c's layout words for it. No
       file wildcard above matches z_popbot.o; named outside .text.*, appended, so no earlier late code moves. */
    *(.text_popbot)
    *(.rodata.popbot_in_text)
    *z_popbot.o(.text .text.* .rodata .rodata.*)
    /* quitblack (after packopen on its branch; in S8b after popbot, merge order; UNDERTALE): the two-screen Quit
       present and menu adapters. Outside .text.late.* so the optmenu/renderer catch-alls above cannot consume the
       new tail. No state. */
    *(.text_quitblack)
    /* essayend (UNDERTALE only): applet return and the hold/alarm finish; original swkbd entries retain their slots. */
    *(.text.late.essayend)
    /* btmoving (tilemiss round 6, 11-08; after packopen on its branch, in S8b after essayend; UNDERTALE Old
       only, empty elsewhere): btlentry's check that the soul is still flying to its battle spot (z_btlentry.c's
       btMoving) and its choice of what to read ahead (btTarget: a New Home story battle reads its room's own set),
       then their names. Outside btlentry's fixed slot; named outside .text.*, so no catch-all takes it; appended,
       so no earlier late code moves. Its state lives in btlentry's bt (taken from the code path's buffer): no bss
       is added. */
    *(.text.btmoving_late)
    *(.rodata.btmoving_in_text)
    /* errtext centering (errtext R417d; in S8b after btmoving, the last late input, merge order; never beside
       E1's old line): the long-button cell's centering geometry. No earlier slot grows. */
    *(.errtext_center_late)
    /* dlgwhite (dlgtop, white-box fix 78c6b3d19; in S8b after errtext centring, the last late input, merge
       order): keep fills in recorded dialogue. */
    *(.text_dlgwhite)
    /* numcache (loadclass, after dlgwhite, merge order; both games): the size a thread's number-cache walk
       takes and, on UNDERTALE Old 3DS, the audio arena's calloc zeroing its whole block (z_numcache.c). Named
       outside .text.late.* so no catch-all takes them; appended, so no earlier late code moves. No constants, no
       statics. */
    *(.text_numcache)
    /* dwkeep (loadclass 10, after numcache, merge order; UNDERTALE only, empty in DELTARUNE): data.win's later
       opens to read as sub-files of one kept handle (z_dwkeep.c), then its constants, kept in late text. Named
       outside .text.late.* and .rodata.late.* so no catch-all takes them; appended, so no earlier late code moves. */
    *(.text_dwkeep)
    *(.rodata.dwkeep_in_text)
    /* closehand (loadclass 12a, after dwkeep, merge order; UNDERTALE Old only, empty elsewhere): the room
       change's texture file closes handed to the tile reader (ctr_textures.c's and ctr_tile_reader.c's ends). Named
       outside .text.late.*; appended, so no earlier late code moves. No constants, no statics. */
    *(.text_closehand)
    /* popbot2 (popbot, steps 5 and 6; after closehand (loadclass), respin merge order; UNDERTALE only, empty in DELTARUNE): the
       popups' logic since step 5 (z_popbot2.c; z_popbot.c's wrappers call into it), its constants and
       ctr_renderer.c's two later layout words. Named outside .text.*, so no catch-all takes them; appended, so no
       earlier late code moves. */
    *(.text_popbot2)
    *(.rodata.popbot2_in_text)
    *z_popbot2.o(.text .text.* .rodata .rodata.*)
    /* battlesplit (battlesplit, after popbot2, respin merge order; UNDERTALE only, empty in DELTARUNE): the split battle layout,
       ctr_renderer.c's layer and present and z_battlesplit.c's routing, settings and their constants. Named outside
       .text.*, and no file wildcard above matches z_battlesplit.o; appended, so no earlier late code moves. */
    *(.text_battlesplit)
    *z_battlesplit.o(.text .text.* .rodata .rodata.*)
    /* battletouch (battletouch, after menuborder on its branch; UNDERTALE only, empty in DELTARUNE): the stylus in
       battles (z_battletouch.c), its constants, and libctru's hidTouchRead, first linked by it (CMakeLists.txt drops that
       line for DELTARUNE, whose Old pin keeps it in the library tail). Named outside .text.late.* and no file wildcard
       above matches z_battletouch.o; appended, so no earlier late code moves. */
    *(.text.battletouch_late)
    *(.rodata.battletouch_in_text)
    *z_battletouch.o(.text .text.* .rodata .rodata.*)
    *libctru.a:hid.o(.text.hidTouchRead)
    /* crispread (crispread, after packopen on its branch; in the S8b respin after battletouch,
       merge order; UNDERTALE only, empty in DELTARUNE): Mettaton EX's letters kept apart (z_smalltext.c's end: stCarry's
       and bcMake's even-gap calls go here first), then its one constant. Named outside .text.* and .rodata.*, so
       no catch-all takes them; appended, so no earlier late code moves. No statics. */
    *(.text_crispread)
    *(.rodata_crispread)
    /* menutime (scanout, ethanlog5 row 13, after btmoving on its branch, merge order; UNDERTALE only, empty in
       DELTARUNE): the options menu's open and close timed for the log, its glyphs read at a room load and the
       closing press's redraw skipped (z_menutime.c), ctr_optmenu.c's CtrOptmenu_pressResumes, then their
       constants. Named outside .text.* and .rodata.*, so no catch-all takes them; appended, so no earlier late code
       moves. */
    *(.text_menutime)
    *(.text_menutime_opt)
    *(.rodata.menutime_in_text)
    /* logfix (scanout, logaudit G8, after menutime on its branch, merge order; both games): z_logfix.c's
       vsnprintf for ctr_audio_system.c (a cut text keeps its newline and says so) and the sound-effect pack's post that
       splits a long record, then their constants. Named outside .text.* and .rodata.*, so no catch-all takes them;
       appended, so no earlier late code moves. No data. */
    *(.text_logfix)
    *(.rodata.logfix_in_text)
    /* heldslow (scanout, logaudit G1/G2, after logfix; UNDERTALE only, empty in DELTARUNE): ctr_frame_pacer.c's
       appended record keeping, then z_heldslow.c's printer and its constants. */
    *(.text_heldslow)
    *(.text_heldslow_print)
    *(.rodata.heldslow_in_text)
    /* heldslow's bottom count (UNDERTALE only): ctr_gpu_queue.c's appended code around dlgblank's answer. */
    *(.text_heldslow_blank)
    /* fightpix (in the S8b respin after scanout's heldslow, merge order; UT only): cold meter-alpha packing and image-fallback hooks. */
    *(.text.fightpix_late)
    /* labguard (labguard, 11-08; UNDERTALE non-public builds only, empty in every public build): the lab's
       startup guard and bounded leave (z_labguard.c), its code and constants. Appended, so every existing symbol keeps
       its address. */
    *z_labguard.o(.text .text.* .rodata .rodata.*)
    /* loadclass 4c (loadclass, after closehand, merge order; UNDERTALE only, empty in DELTARUNE): two layers of one
       song join sample-locked (ctr_audio_system.c's end). Named outside .text.late.* so no catch-all takes it;
       appended, so no earlier late code moves. No constants, no statics. */
    *(.text_lcpair)
    /* loadclass 8 and 9 (loadclass, after 4c, merge order; UNDERTALE only, empty in DELTARUNE): the DSP-miss log
       line and the later sounds opened ahead (ctr_audio_system.c's end), then 8's template, kept in late text.
       Named outside .text.late.* and .rodata.late.* so no catch-all takes them; appended, so no earlier late
       code moves. */
    *(.text_lclater)
    *(.rodata.lclater_in_text)
    *(.text.late.dimlight)
    /* dimlight: its newly linked SDK calibration reader (76 B) must also stay out of ordinary text. */
    *libctru.a:cfgu.o(.text.CFG_GetConfigInfoBlk8)
    /* dimlight startup-hang correction: newly linked public LCD getter, cold and appended. */
    *libctru.a:gsplcd.o(.text.gspLcdGetSessionHandle)
    /* dimlight: public SRV session getter for zero-wait LCD acquisition. */
    *libctru.a:srv.o(.text.srvGetSessionHandle)
    /* dimlight: append its read-only formats here, as screens/argui do above.
       New Steam has only 245 B in late rodata; these formats need 285 B. */
    *(.rodata.late.dimlight)
    /* handover: after noon inputs; lab arming and main's completed stall-tap delivery. */
    *(.text.late.handover)
    *(.text.late.handover.h1)
    /* asgorelong (asgorelong; in S8b after dlgwhite, the last late input, merge order; UNDERTALE only,
       empty in DELTARUNE): the fine sprite path with an ember's tile switch planned (ctr_renderer.c's end), copied
       from utasgore's functions, which keep their place. Named outside .text.*, so no catch-all takes it; appended,
       so no earlier late code moves. No constants, no statics. */
    *(.text_asgorelong)
    /* asgorelong T1 (after asgorelong's line; UNDERTALE only): the bubble clip's plan lookup and what decides a plan is
       still good (z_smalltext.c's bcPlanFor). Named outside .text.*, so no catch-all takes it; appended, so no earlier
       late code moves. */
    *(.text_asgorelong_bc)
    /* firstboot (firstboot, 11-08; after fightpix, the last late input, merge order; UNDERTALE only, empty
       in DELTARUNE): the first time setup's moving bar (z_loadscreen.c's CtrFirstboot_packRun, its fill math inlined
       from ctr_firstboot_fill.h) and the worker's done and claimed work it reads (ctr_texture_pack.c's
       CtrPackWorker_firstbootWork, at that file's end). Named outside .text.*, so no catch-all takes it; appended, so
       no earlier late code moves. No constants outside the text, no statics. */
    *(.text_firstboot)
    /* hplcopy (musicloop, after musicloop on its branch; in S8b after dlgwhite, merge order; UNDERTALE Old only,
       empty elsewhere): a short looping stream
       played fast reads a copy decoded on the app core (ctr_audio_system.c's end), then its log lines. Named outside
       .text.*, so no catch-all takes it; appended, so no earlier late code moves. */
    *(.text_hplcopy)
    *(.rodata.hplcopy_in_text)
    /* hitcheck: collision-only PC float bbox and pair test. */
    KEEP(*(.text.late.hitcheck))
    *z_hitcheck.o(.text .text.* .rodata .rodata.*)
    /* battletouch, appended (battletouch, 11-08; UNDERTALE only, empty in DELTARUNE): everything z_battletouch.c
       added after its first slot above (021a570b8): the SOUL kinds after red, their names and the merge's body (the
       slot's CtrInput_take wrap branches here, padded to its old size). Named outside .text.* and .rodata.*, so the
       z_battletouch.o line above does not take them; appended, so the slot and everything after it keep their
       addresses. New battletouch code goes to the third pair at the end of late text. */
    *(.text_battletouch2)
    *(.rodata_battletouch2)
    /* drawbirth R397b: new admission helper, after every frozen S7 text symbol (in S8b after battletouch2, the
       last late input, merge order). */
    *(.text.late.drawbirth)
    /* seamlocal (musicloop row 23, after hplcopy, merge order; UNDERTALE, empty elsewhere): the local loop-seam
       rule's read layer, cached-loop fix and registration (ctr_audio_system.c's end). */
    *(.text_seamlocal)
    /* tilenames (tilemiss round 10, 11-08; after btmoving, merge order; UNDERTALE Old only, empty elsewhere): a log
       line for each tile a draw reads from the SD card in play (z_utstall.c's end, CtrTilenames_*; entryload's wraps
       call it), then its log formats. Named outside .text.late.* and .rodata.late.*, so no catch-all takes them;
       appended, so no earlier late code moves. */
    *(.text.tilenames_late)
    *(.rodata.tilenames_in_text)
    /* bthand (tilemiss, 11-08; after tilenames, merge order; UNDERTALE Old only, empty elsewhere): btlentry's
       finished reads handed to the battle room's load (z_btlentry.c's end, CtrBtHand_roomLoad; ctr_textures.c's room
       load call goes to it, then to utfade A's), then its log format. Appended, so no earlier late code moves; its
       state is btlentry's bt and utfade A's ua: no bss is added. */
    *(.text.bthand_late)
    *(.rodata.bthand_in_text)
    /* battlesplit placement (c) (battlesplit, after batch 1 went to review; UNDERTALE only, empty in DELTARUNE):
       CtrRenderer_bsPresent5, the seam table and its readers (ctr_renderer.c's and z_battlesplit.c's
       .text_battlesplit5 and .rodata_battlesplit5). Appended, so nothing before it moves. */
    *(.text_battlesplit5)
    *(.rodata_battlesplit5)
    /* crispread2 (crispread, per-line B, after crispread on its branch; in dev after battlesplit5, merge order;
       UNDERTALE only, empty in DELTARUNE): a line of Mettaton EX's keeps 1 px letter gaps when the whole line fits its
       bubble (z_smalltext.c's end: stCarry's and bcPlanFor's calls go here first). Named outside .text.*, so no
       catch-all takes it; appended, so no earlier late code moves. No constants; its state is .bss_crispread2. */
    *(.text_crispread2)
    /* battlesplit menu-pick log (battlesplit, playtest1 P1; UNDERTALE only, empty in DELTARUNE): the log line
       that times the bottom after a FIGHT/ACT/ITEM/MERCY pick: its constants (z_battlesplit.c's .rodata_battlesplit6),
       then its code (.text_battlesplit6). Appended after crispread2, so nothing before it moves. */
    *(.rodata_battlesplit6)
    *(.text_battlesplit6)
    /* battletouch, third pair (battletouch, 11-08; after crispread2 on its branch, after battlesplit6 on dev: the
       last late input then; UNDERTALE only, empty in DELTARUNE): new battletouch code after its second pair went into
       dev: Way B's options rows on the main page and the pen log (z_battletouch.c's .text_battletouch3 and
       .rodata_battletouch3). Named outside .text.* and .rodata.*; appended, so nothing before it moves. */
    *(.text_battletouch3)
    *(.rodata_battletouch3)
    /* battlesplit, pick log v2 fixes (battlesplit, c26rev5 on P6; UNDERTALE only, empty in DELTARUNE): the
       split end that also gives up the dialogue row (ctr_renderer.c's CtrRenderer_bsLeave8), and the pick log
       with its screen sampler in test builds only (z_battlesplit.c's .rodata_battlesplit8, then
       .text_battlesplit8), in place of the pick log's second version (battlesplit7, gone in the post-G8 move
       commit). Appended, so nothing before it moves. */
    *(.rodata_battlesplit8)
    *(.text_battlesplit8)
    /* layout (layout S1, after battlesplit8, merge order; UNDERTALE only, empty in DELTARUNE): the element layout
       model, its settings links and the stylus's bottom-screen answer (z_layout.c), its constants and ctr_renderer.c's
       two words for it. Named outside .text.* and .rodata.*; no file wildcard above matches z_layout.o; appended, so no
       earlier late code moves. */
    *(.text_layout)
    *(.rodata_layout_in_text)
    *z_layout.o(.text .text.* .rodata .rodata.*)
    /* bonegate: appended after layout (merge order, dv8); UT release, diagnostics excluded. */
    *(.text_bonegate)
    *(.rodata.bonegate_in_text)
    /* battlesplit, Muffet (battlesplit, ADAPT group 56; UNDERTALE only, empty in DELTARUNE): her encounter
       table (bsTables9, gone with the compiled tables in the post-G8 move commit) and the capture hook that reads
       the arena layer for sprite_create_from_surface of the application surface on a split frame (z_battlesplit.c's
       .rodata_battlesplit9, then .text_battlesplit9 from z_battlesplit.c and ctr_renderer.c). Appended after bonegate
       (merge order, dv8), so nothing before it moves. */
    *(.rodata_battlesplit9)
    *(.text_battlesplit9)
    /* paintchg: cache composition, appended after every existing late input. */
    *(.text.late.paintchg)
    *z_boxcache.o(.text .text.* .rodata .rodata.*)
    /* First referenced by paintchg; keep the new libctru entry out of warm text. */
    *libctru.a:synchronization.o(.text.RecursiveLock_TryLock)
    /* Idle painter contract: a new append, never growth of an earlier section. */
    *(.text.late.paintchg_idle)
    *z_bottompaint.o(.text .text.* .rodata .rodata.*)
    /* paintchg provider LCD scope fix; the whole provider tail after battlesplit9 (merge order, dv8). */
    *(.paintchg_lcdscope_code)
    *(.paintchg_lcdscope_const)
    /* asrsplit (UNDERTALE Old and New): guarded fine-sprite command-room slow path;
       appended after paintchg's tail (merge order, dv8), no constants or mutable state. ctr_renderer.o is excluded
       from the catch-all. */
    *(.text.late.asrsplit)
    /* battletouch, fourth pair (battletouch, 11-08, I2; after layout, the last late input then, and so in the
       late-order registry; UNDERTALE only, empty in DELTARUNE): the first-battle stylus hint (z_battletouch.c's
       .text_battletouch4 and .rodata_battletouch4). Its own pair so the third pair's code keeps its addresses;
       appended after asrsplit (merge order, dv8), so nothing before it moves.
       Pinned at its dv8r size (lead, 11-09: 0x1d53 = 7,507 B, text and rodata) on UNDERTALE, so its later changes
       no longer move the late code after it; empty in DELTARUNE, no pad. New code goes to the fifth pair (below,
       last before repack_test); outgrowing the slot fails the link. The pad is zero fill between the
       __bt4_text_gap_* marks. */
    __bt4_late_start = .;
    *(.text_battletouch4)
    *(.rodata_battletouch4)
    __bt4_text_gap_start = ABSOLUTE(.);
    . = . > __bt4_late_start ? __bt4_late_start + 0x1d53 : .;
    __bt4_text_gap_end = ABSOLUTE(.);
    /* layout, appended pair (layout F8, F8b and S3; appended after battletouch's fourth pair, merge order, dv8;
       UNDERTALE only, empty in DELTARUNE): code changed or added after .text_layout went into dev, so .text_layout and
       everything after it keep their addresses: F8's guard and yield (lyGuardMove8, CtrLayout_yield8; lyStage's call
       and ctr_renderer.c's yield hook name them, the old two stay unused), the keep-lit question and the scene
       table's loader (z_layout.c's .text_layout2 and .rodata_layout2_in_text). Named outside .text.* and .rodata.*,
       so the z_layout.o line above takes neither. */
    *(.text_layout2)
    *(.rodata_layout2_in_text)
    /* battlesplit on the scene table (layout S3, after layout's appended pair; UNDERTALE only, empty in
       DELTARUNE): the admissions, places and encounter classes from layout's scene table; no table, every battle
       Original (z_battlesplit.c's .text_bsscene). The four calls were pointed here; appended, so nothing before it
       moves. */
    *(.text_bsscene)
    /* battlesplit, a room's own split scene (battlesplit, the first Flowey's tutorial fight; UNDERTALE only, empty
       in DELTARUNE): the room let in as a battle room with its own group, its place and classes from its scene in
       the scene table (z_battlesplit.c's .text_battlesplit10). Appended after the scene table's, so nothing before
       it moves. */
    *(.text_battlesplit10)
    /* battlesplit, the lift's guard (battlesplit; UNDERTALE only, empty in DELTARUNE): the guard that no longer
       moves the bottom, and the placement switch's key off (z_battlesplit.c's .rodata_battlesplit11, then
       .text_battlesplit11). Napstablook's tears and the fights with no lift are scene table rows. Appended after the
       room scene's. */
    *(.rodata_battlesplit11)
    *(.text_battlesplit11)
    /* battlesplit, the router and the runs (battlesplit; UNDERTALE only, empty in DELTARUNE): a draw across
       the band and the guard's own draw on both screens, and each later draw of a plain or turned run routed as
       the full way would route it (z_battlesplit.c's and ctr_renderer.c's .text_battlesplit11f). Appended after
       the guard's. */
    *(.text_battlesplit11f)
    /* battlesplit13 (battlesplit; UNDERTALE only, empty in DELTARUNE): CtrRenderer_bsBegun13, the split's bottom
       target made again when the screen set-up inside beginFrame dropped it, and its log line (ctr_renderer.c's
       .rodata_battlesplit13, then .text_battlesplit13). Appended after the router's. */
    *(.rodata_battlesplit13)
    *(.text_battlesplit13)
    /* homesave (UT only): lifecycle drain and early watchdog pause; no new mutable state.
       Names bypass the old generic .text.late.* and z_sleepflush.o(.text.* .rodata.*) catch-alls. */
    *(.homesave_code .homesave_const)
    /* bottomflip (bonepulse, 11-09; after battletouch's fourth pair, merge order; UNDERTALE only, empty in
       DELTARUNE): a held bottom flip presented from the bottom's own vblank, and both screens' real shown counts
       (the end of ctr_frame_pacer.c), code and its log line. Named outside .text.late.* and .rodata.late.* so no
       catch-all takes them. */
    *(.text.bottomflip_late)
    *(.rodata.bottomflip_in_text)
    /* movehold (bonepulse, 11-09; after bottomflip, merge order; UNDERTALE only, empty in DELTARUNE): the
       game picture a screen move kept up, counted as its frame's (the end of ctr_frame_pacer.c). Code only; its
       state is in heldslow's heap state. Named outside .text.late.* so no catch-all takes it. */
    *(.text.movehold_late)
    /* reclaim (bonepulse, 11-09; after movehold, merge order; UNDERTALE only, empty in DELTARUNE): main.c's
       reclaimBottomForConsoles makes only the consoles' screen single buffered, not the game's after a move to the
       bottom (the end of ctr_gpu_queue.c). Code only. Named outside .text.late.* so no catch-all takes it. */
    *(.text.reclaim_late)
    /* hitcheck readers (hitcheck, V11; after reclaim, merge order): outside .text.* so colline's earlier object
       catch-all cannot consume it. */
    KEEP(*(.text_hitcheck_readers_late))
    /* lcstop (loadclass, 11-09; after hitcheck, merge order; UNDERTALE only, empty
       in DELTARUNE): the asset query's short window after a stop, and the stop entries that note it
       (ctr_audio_system.c's end); the pointers keep sndcutStopSound and ctrStopAll, which the vtable no longer
       names. Appended, so nothing before it moves. */
    *(.text_lcstop)
    KEEP(*(.rodata_lcstop_in_text))
    /* noticescroll follow-up: resident records and bounded FILE fallback; preserve the composed clear slot. */
    *(.text.late.noticescroll_cache)
    /* Port menu copies retain their presentation slot; no shared renderer change or extra wait. */
    *(.text.late.noticescroll_menucopy)
    *(.text.late.benchsettings .rodata.benchsettings_in_text)
    /* shopsplit: completed-frame crops, appended after composed dv8q. */
    *(.text.late.shopsplit)
    *(.rodata.late.shopsplit)
    /* battletouch, fifth pair (battletouch, 11-09; appended after shopsplit's pair, merge order; UNDERTALE only,
       empty in DELTARUNE): code moved out of the pinned fourth pair (the pad, the cursor put-back, the menu leave)
       and code added after the pin (z_battletouch.c's .text_battletouch5 and .rodata_battletouch5). */
    *(.text_battletouch5)
    *(.rodata_battletouch5)
    /* gpuburst F1 (UNDERTALE Old): CtrMemory_vramAllocTop by VRAM bank (z_vramtop.c), appended after dv8s. */
    *(.text.late.gpuburst)
    /* bootline: UT console-only startup swap; appended after gpuburst's line (compose order, dv8u). */
    *(.text.bootline_console_late)
    /* letterdraw (last, merge order; UNDERTALE Old only, empty elsewhere): writerfront's once-per-data.win check
       (z_writerfront.c's wfCheck), moved here so writerfront's slot keeps its size with the record-then-draw code in
       it. Named outside .text.* so z_writerfront.o's line above leaves it here. */
    *(.text_letterdraw_late)
    /* battletouch's pen-log context (lead 11-10; CTRPORT_PEN_LOG, dev and touch-test builds only, empty in every
       release build): z_battletouch.c's btPenNote and its helpers, appended last when it merged; later appends
       follow it. Test builds' late addresses already differ from release (dev-only code sits earlier too). */
    *(.text_battletouchpen)
    *(.rodata_battletouchpen)
    /* battlesplit-rework (lead 11-10; UNDERTALE only, empty in DELTARUNE): the bottom backlight through a split
       frame's set-up (z_battlesplitbsr.c, ctr_renderer.c's CtrRenderer_bsLitBsr), appended after dv8u3's appends
       (letterdraw's), so nothing before it moves; then its development-only part (z_dimlight.c's backlight log and
       z_battlesplitbsr.c's failed-open log; empty in every release build). Named outside .text.late.* and
       .bss.late.*, so the catch-alls above leave them here. */
    *(.text_bsr)
    *(.text_bsrdev)
    *(.rodata_bsrdev_in_text)
    /* layoutd (layout, 11-10; commit D, the held top target's lend; UNDERTALE only, empty in DELTARUNE): D's new
       code and constants in z_layout.c and z_battlesplit.c. After battlesplit-rework's sections, before the repack
       test pair (compose dv8u6), so no earlier late address moves; nothing pins an address here. Named outside .text.*
       and .rodata.* (as battletouch's fifth pair): z_layout.o and z_battlesplit.o have object catch-alls above. */
    *(.text_layoutd)
    *(.rodata_layoutd)
    /* battletouch's stylus hint (lead 11-10; compose dv8u8, appended after layoutd's pair, dv8u7's last, ruling 7;
       UNDERTALE only, empty in DELTARUNE): the once-a-session hint that replaced the 11-08 one (z_battletouch.c's
       .text_battletouchhint and .rodata_battletouchhint; the old hint's code stays in the pinned fourth pair). Named
       outside .text.* and .rodata.*, so no rule above takes them; appended, so nothing before it moves. */
    *(.text_battletouchhint)
    *(.rodata_battletouchhint)
    /* asrhold bench-ready ready-clear (lead 11-10; compose dv8u9, after the hint pair): 176 B per model. */
    *(.text_asrholdbenchready)
    /* touchui step 1, after dv8q in merge order: shared sample reader, synthetic keys,
       registered sound wrapper and late constants. State remains on the heap. */
    *(.touchui_text)
    *(.touchui_ro)
    *z_touchui.o(.text .text.* .rodata .rodata.*)
    *ctr_renderer.o(.text.CtrRenderer_getDisplay)
    /* 11-10: cold naming code/constants leave AAC; after shopsplit and our existing append. */
    *(.touchname_text)
    *(.touchname_ro)
    /* touchui queue packages only: append after the existing touchui code and constants. */
    *(.touchqueue_text)
    *(.touchqueue_ro)
    /* gpuburst C(i) (UNDERTALE): the draw alpha byte's stubs and their table (ctr_renderer.c's end), appended
       after asrhold's bench-ready section (dv8u9's last, after the hint pair), before the repack test pair, so no earlier late address moves.
       Compose dv8u12 (integrate21): after touchui's block, dv8u11's last. */
    *(.text.late.alphabyte)
    *(.rodata.late.alphabyte)
    *(.text_dlgflash)
    *(.rodata.dlgflash_in_text)
    /* S4: infrequent loan acquire/return and room-change arbitration. */
    *(.text.late.dlgtop4)
    /* mexdlg native-M adapter: appended ordinary late text, no fixed address. */
    *(.text_mexdlg .rodata_mexdlg)
    *(.text_screenmovestamp)
    *(.rodata_screenmovestamp)
    *(.text_menulag)
    /* Shops and SAVE: whole-game-bottom input only; no AAC or static state. */
    *(.text_touchshops)
    *(.rodata_touchshops)
    /* fpsghost: accepted menu actions retire a deselected top-margin counter. */
    *(.text.late.fpsghost)
    *(.text_fpsghost_return)
    /* letterdraw D4 on dv8u19: ordinary late input after fpsghost (sfxlate follows; composed in that order). */
    *(.text_letterdraw4_late)
    /* sfxlate: room-258 fan preparation; repack test inputs remain last. */
    *(.text_sfxlate)
    /* letterdraw glyph-pair memo (lead 11-11, after sfxlate on dv8u21; UNDERTALE Old only, empty elsewhere): z_smalltext.c's
       stCarry memo (its file end). Named outside .text.*, so z_smalltext.o's object line leaves it here; appended,
       so nothing before it moves. */
    *(.text_letterdraw_memo)
    /* letterdraw plan variants (lead 11-11, L1, after the memo; UNDERTALE Old only, empty elsewhere): z_smalltext.c's
       bcPlanFor body and its variant block (its file end). Named outside .text.*, so z_smalltext.o's object line leaves
       it here; appended, so nothing before it moves. */
    *(.text_letterdraw_plan)
    /* Enemy speech tap key: append only, after all composed ordinary late tenants. */
    *(.text_battletouchbubble)
    /* battlesplit22: the Split's top margin (what reaches above the view, drawn above the band); UNDERTALE only. */
    *(.text_bsmargin22 .rodata_bsmargin22)
    /* repack test install (repacka, 11-02; build.sh repacktest= test builds only, empty in every release build):
       diag/ctr_repack_test.c's code and constants, last, so adding them moves no other late address of that test
       build (its late addresses still differ from release). Keep these last; new appends go above this comment. */
    *(.repack_test)
    *(.repack_test_ro)
  }
  __late_text_end = ABSOLUTE(.);
  /* 192 KiB (grown in whole 4 KiB pages from 16 KiB, each time late code outgrew it): screens' options page took
     12.3 KB on every target and colflip's UNDERTALE rules 3.5 KB; then noports left UNDERTALE o3DS about 2 KB
     with tpaggrow, texcache (2.3 KB), ring reuse, rdfix2 and utgrid still to come; then typed3, contention,
     texcache F1, alarm14 and utgrid plus layerfix and colflip did not fit in
     96 KiB (UNDERTALE New had 12.5 KB left);
     then layerfix (6.4 KB) and roomload (about 15.2 KB) overflowed
     UNDERTALE's 128 KiB on Old and New (15.7 and 17.0 KB were left), with argtower (8.3 KB) and the engine levers
     still to come (bigger code is fine when it is faster). Growing this moves .rodata, .data and .bss by whole pages only (no text
     address changes): check_layout.py --expect-shift verifies section-uniform OBJECT page moves;
     inspect all remaining failures (TLS separately), and require tools/analysis/shiftproof.py PROVEN.
     DELTARUNE gets two more 4 KiB pages, 200 KiB: Old had 3223 B
     left with bggrid (4045 B) and crispart (5162 B) still to merge, and New 4491 B after bggrid with crispart to
     come. CMakeLists.txt replaces the marked size for DELTARUNE only, so UNDERTALE keeps 192 KiB and its addresses. */
  __late_text_limit = ALIGN(__late_text_start, 0x1000) + /* LATE_TEXT_RESERVE */ 0x30000;
  /* Filled with zeros so the code segment stays contiguous in the file (3dsxtool copies segments by file size). */
  /* (Addresses are unsigned, so an overflow must not reach the subtraction: the ASSERT below reports it instead.) */
  .late.text.reserve :
  {
    BYTE(0);
    . += __late_text_end < __late_text_limit ? __late_text_limit - __late_text_end - 1 : 0;
  }
  ASSERT(__late_text_end < __late_text_limit,
         "late code (.text.late.*) outgrew its reserve: see platform/3ds/late_reserve.ld")
  /* freebytes H1 hole limits (flushrev R4-F2). On UNDERTALE Old CMake substitutes each hole's fixed end in the
     frozen H1 layout as <HOLE>_TEXT_LIMIT: the hole pads to its stage address mod 4096 while its start is at
     or before that end, and pads nothing when it is not (a test profile that adds late code, which packs).
     In release builds (CTRPORT_RELEASE_SOURCE_URL set) CMake also substitutes <HOLE>_TEXT_STRICT, so any
     overfilled hole stops the link whatever the overflow (the old pad < 2048 test let 2,052 B or more
     through, wrapped into the next page). Other targets: the hole's own start and end, always true. */
  ASSERT(__h1loadtime_text_gap_end <= /* H1LOADTIME_TEXT_STRICT */ __h1loadtime_text_gap_end,
         "freebytes H1: hole loadtime overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__h1coverage_text_gap_end <= /* H1COVERAGE_TEXT_STRICT */ __h1coverage_text_gap_end,
         "freebytes H1: hole coverage overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__h1utstall_text_gap_end <= /* H1UTSTALL_TEXT_STRICT */ __h1utstall_text_gap_end,
         "freebytes H1: hole utstall overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__h1bxreview_text_gap_end <= /* H1BXREVIEW_TEXT_STRICT */ __h1bxreview_text_gap_end,
         "freebytes H1: hole bxreview overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__h1relprep_text_gap_end <= /* H1RELPREP_TEXT_STRICT */ __h1relprep_text_gap_end,
         "freebytes H1: hole relprep overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__h1utloop_text_gap_end <= /* H1UTLOOP_TEXT_STRICT */ __h1utloop_text_gap_end,
         "freebytes H1: hole utloop overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__h1rend_text_gap_end <= /* H1REND_TEXT_STRICT */ __h1rend_text_gap_end,
         "freebytes H1: hole rend overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__aotlhelp_text_gap_end <= /* AOTLHELP_TEXT_STRICT */ __aotlhelp_text_gap_end,
         "freebytes H1: hole aotlhelp overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__aotlgen_text_gap_end <= /* AOTLGEN_TEXT_STRICT */ __aotlgen_text_gap_end,
         "freebytes H1: hole aotlgen overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__aotlate_text_gap_end <= /* AOTLATE_TEXT_STRICT */ __aotlate_text_gap_end,
         "freebytes H1: hole aotlate overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__utlava_text_gap_end <= /* UTLAVA_TEXT_STRICT */ __utlava_text_gap_end,
         "freebytes H1: hole utlava overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__utlhelp_text_gap_end <= /* UTLHELP_TEXT_STRICT */ __utlhelp_text_gap_end,
         "freebytes H1: hole utlhelp overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__utoldreg_text_gap_end <= /* UTOLDREG_TEXT_STRICT */ __utoldreg_text_gap_end,
         "freebytes H1: hole utoldreg overfilled (see platform/3ds/late_reserve.ld)")
  ASSERT(__asgborder_text_gap_end <= /* ASGBORDER_TEXT_STRICT */ __asgborder_text_gap_end,
         "freebytes H1: hole asgborder overfilled (see platform/3ds/late_reserve.ld)")
}
INSERT AFTER .text;

SECTIONS
{
  /* ===== Read-only data, frozen late constants: as an orphan this landed after .rodata and before the C++ unwind
     tables (.ARM.extab, .ARM.exidx, present when openh264 is linked), so it goes there. ===== */
  .heapfrag_rodata : { *(.heapfrag_rodata) }
}
INSERT BEFORE .ARM.extab;

SECTIONS
{
  /* ===== Read-only data: the features' late constants after the unwind tables (which then never move, and whose
     __exidx_end stays theirs), then the reserve up to the data page. ===== */
  .late.rodata . :
  {
    __late_rodata_start = ABSOLUTE(.);
    /* Features, in merge order (append only): */
    *(.rodata.late.benchromfs)
    *(.rodata.late.sandbox)
    /* relprep: ctr_legal.o's constants (the Legal page's source URL and heading, whose size follows the release
       source URL) are last in late constants (after FREEBYTES_RODATA, below); the other files' relprep constants
       stay. Then the smallest pad (0-31 B) that keeps every later late constant on its stage72 cache-line phase
       (CMake substitutes RELPREP_RODATA_PHASE, the address mod 32 that followed relprep in stage72). */
    KEEP(*(EXCLUDE_FILE (*z_bottomframe*.o *ctr_legal.o) .rodata.late.relprep))
    __relprep_rodata_gap_start = ABSOLUTE(.);
    . += ((/* RELPREP_RODATA_PHASE */ 0 - __relprep_rodata_gap_start) & 31);
    __relprep_rodata_gap_end = ABSOLUTE(.);
    /* RELPREP_ROMFS_rodata */
    *(.rodata.late.loadtime)
    *(.rodata.late.colflip)
    /* colflip: the switch tables of those copies. */
    *collision_compat.o(.rodata .rodata.*)
    /* argcheck (UT late-text reclaim): in UNDERTALE collision_compat.c is not linked; its constants' place,
       same bytes (collision_gm14.c). */
    KEEP(*(.rodata.late.colflipgap))
    *(.rodata.late.heapfrag)
    /* Not listed yet: */
    *(.rodata.late.utpolish)
    /* screens (after relprep): after every late input already linked, so that none of those moves. */
    *(.rodata.late.screens)
    /* colflip: collision_gm14.c's constants, last. */
    *(.rodata.late.colflip_gm14)
    *collision_gm14.o(.rodata .rodata.*)
    /* benchromfs (after colflip_gm14): the pre-window observer, after every late input already linked. */
    *(.rodata.late.benchromfs.prewindow)
    /* noports (after benchromfs). */
    *(.rodata.late.noports3)
    /* tpaggrow (after noports). */
    *(.rodata.late.tpaggrow)
    /* contention (after tpaggrow): the Old models' tile copies in VRAM. */
    *(.rodata.late.contention)
    /* texcache (after contention): packed-copy value ranking. */
    *(.rodata.late.texcache)
    /* knfuse (after texcache): Native fusion. */
    *(.rodata.late.knfuse)
    /* coverage (after knfuse): the RomFS bench supplement and the state dump's name helper. */
    *(.rodata.late.coverage)
    /* o3titan8 (after coverage): reserved for its opt-in phase layout; empty in default builds. */
    *(.rodata.late.o3titan8)
    /* lockfix (after o3titan8): empty selectors (main's titanthrash 210e822d carries the fix). */
    *(.rodata.late.lockfix)
    /* typed3 (after lockfix): typed AOT helpers and binding validation; includes the cold
       ctrAotHostSymbols validation table. */
    KEEP(*(.rodata.late.typed3))
    /* alarm14 (after typed3): alarm_gm14.c's constants (none expected). */
    *alarm_gm14.o(.rodata .rodata.*)
    /* uthud (pixfix A, after alarm14; utgrid reverted): view_snap, UNDERTALE only. */
    *(.rodata.late.uthud)
    /* pixfix (task D, DELTARUNE only): after uthud, so that none of those moves. */
    *(.rodata.late.pixfix)
    /* exithang (after pixfix D): bounded exit, deadline and exit log (ctr_exit.c, ctr_exit_log.c). */
    *(.rodata.late.exithang)
    /* colline (colflip follow-up check, slim; after exithang): colline_gm14.c's constants, last. */
    *(.rodata.late.colline_gm14)
    *colline_gm14.o(.rodata .rodata.*)
    /* menupause (after colline): its constants. */
    *(.rodata.late.menupause)
    /* ringstart (after menupause): contention's command ring start reuse and run-on (ctr_gpu_queue.c). */
    *(.rodata.late.ringstart)
    /* argui: its constants are in the late text block, after its code. */
    /* thrashport (after kawkaw): Old palette-copy retries (ctr_renderer_palette_retry.inc). */
    *(.rodata.late.thrashport)
    /* layerfix (last): layer Draw mutation helpers (runner.c, vm_builtins.c). */
    *(.rodata.late.layerfix)
    /* argtower (after layerfix): Old-model Flowery tower faces (ctr_renderer.c, main.c). */
    *(.rodata.late.perfflowery)
    /* argcrash (after nativemac): friendly crash screen and release-quiet logging (z_argcrash.c). */
    *(.rodata.late.argcrash)
    /* engfix (after argcrash): game_end closes the game (main.c) and the spare-texture GPU wait (ctr_renderer.c, ctr_gpu_queue.c). */
    *(.rodata.late.engfix)
    *(.rodata.late.coverage.rooms)
    /* roomload: none here (its constants are in late text, .rodata.roomload_in_text). */
    *(.rodata.late.exportclean)
    *(.rodata.late.flowergpu)

    *(.rodata.late.benchfid)
    *(.rodata.late.inputlate)
    *(.rodata.late.pacer)
    /* oomport (integrate round 15, after pacer): checked optional renderer/history arrays. */
    *(.rodata.late.oomport)
    *(SORT_BY_NAME(EXCLUDE_FILE (*z_bottomframe*.o *z_bottompaint.o *z_boxcache.o *ctr_log.o *z_rosabl.o *z_dimlight.o *z_upd1_options.o *z_upd1_audio.o *z_upd1_text.o *z_saveaudit.o *z_saveaudit_format.o *z_saveaudit_scan.o *z_thinart.o *z_dlghold.o *z_redpix.o *ctr_legal.o *z_utwindow.o *z_asrtext* *z_adpcm* *ctr_adpcm_probe.o *ctr_audio_system.o *z_knightnew_history.o *z_flowcode.o *ctr_flowervramg_promotion.o *ctr_flowervramg_palette.o *z_gogut* *z_builderx_new* *z_builderx_receipt* *z_sdlat* *ctr_input.o *ctr_fpsbox.o) .rodata.late.*))
    *(.rodata.late.builderxnew)
    *(.rodata.late.builderxreceipt)
    *(.rodata.late.gogut)
    *(.rodata.late.sdlat) *z_sdlat*(.rodata .rodata.*)
    /* gamedir (gamedir2, round 21, last; both games but DELTARUNE Old 3DS, where they stay in late text):
       z_gamedir.c's constants. */
    *(.rodata_gamedir2_late)
    /* credfix (last, merge order): the missing-file notice's names and text. */
    *(.rodata.credfix_late)
    *(.rodata.late.flowervramg_palette)
    *(.rodata.late.flowervramg_promotion)
    /* flowfull code paging (round 21, last, merge order; DELTARUNE New flowcode=lazy builds only, default Off, so no
       release build has these sections): z_flowcode.c. z_flowcode.o is out of the catch-all above. */
    *(.rodata.late.flowfullcode)

    *(.rodata.late.adpcm)
    /* snap names (credfix 8232d6954, last, merge order; both games): the L+SELECT scan prefix "snap_"
       (ctr_screenshot_path.c). Named outside .rodata.late.* so the catch-all above leaves it here. */
    *(.rodata.snapname_late)
    /* knightgpu L-A (last, merge order): log formats, profile builds only (ctr_renderer.c's end). Named outside
       .rodata.late.* so the catch-all above leaves them here. */
    *(.rodata.knightgpu_late)
    /* freebytes (last, merge order): on UNDERTALE Old 3DS only, CMakeLists.txt takes these constants out of the late
       text block (argcrash's and pacer_io's lines and ctr_bench_io.o's catch-all) and puts them here: late code is
       the scarce reserve there, and its late constants page had 3.6 KB spare. Every other target keeps them in late
       text. */
    /* FREEBYTES_RODATA */
    /* utbugs4 (last, merge order): virtual window position and final picture translation. */
    *(.rodata.utbugs4_late) *z_utwindow.o(.rodata .rodata.*)
    /* relprep: ctr_legal.o's constants, last (moved from relprep's place above): their size follows the release
       source URL, so a URL change moves no other constant. */
    KEEP(*ctr_legal.o(.rodata.late.relprep))
    *(.rodata.late.loadaudio)
    /* exitpanic argbind (last, merge order; DELTARUNE only): the five argui asset names (z_argbind.c). Named outside
       .rodata.late.* so the catch-all above leaves them here. */
    *(.rodata.argbind_late)
    /* knightgpu A (last, merge order; DELTARUNE New only): log formats, profile builds only (ctr_renderer.c's end). */
    *(.rodata.knightgpu_compact_late)
    /* flowerhw D'+E (last, after knightgpu A, merge order; DELTARUNE New only): the tower cuts' log strings
       (vine_new_retain.inc). Here, not in late text, so nothing follows the ld veneer there. */
    *(.rodata.flowerhw_late)
    /* knightgpu SY (after flowerhw D'+E, before flowerhw tshare): its log format, profile builds only
       (ctr_renderer.c's end). */
    *(.rodata.knightgpu_sy_late)
    /* knightgpu FC (after SY, before flowerhw tshare): its log format, profile builds only (ctr_renderer.c's end). */
    *(.rodata.knightgpu_fc_late)
    /* flowerhw tshare (last, merge order; DELTARUNE New only): its three log formats (vine_new_retain.inc's end). */
    *(.rodata.tshare_late)
    /* racefix (last, merge order): the save writer's retry log line (ctr_file_system.c's end). */
    *(.rodata.racefix_late)
    /* redpix (last, merge order): fingerprints and atlas positions of the two UT heads, Steam and GOG. */
    *z_redpix.o(.rodata.late.redpix)
    /* dlghold (last): names only, resolved at chapter initialization. */
    *(.rodata.late.dlghold)
    /* knightgpu FC rev 2 (after tshare, so every late constant before keeps its address, c26rev r103 F1; DELTARUNE New
       only): its log formats and give-up reasons (ctr_renderer.c's end). */
    *(.rodata.knightgpu_fc2_late)
    /* GPU kick (pacer, from cheaplevers; last, after knightgpu FC rev 2 (75zq), merge order; DELTARUNE New only, empty
       elsewhere): its log formats. Every earlier late constant keeps its address. */
    *(.rodata.cheaplevers_kick_late)
    /* sharebuf (titancpu, last, after the GPU kick, merge order; DELTARUNE New only): its log formats and the
       list's path. Every earlier late constant keeps its address. */
    *(.rodata.sharebuf_late)
    /* drhitbox (last, after sharebuf (75zq unit 17), merge order; DELTARUNE only). */
    *z_drhitbox.o(.rodata .rodata.*)
    /* sharebuf G2 (titancpu, last, after drhitbox, merge order; DELTARUNE New only): its log formats and the
       addresses of the G1 functions it calls. */
    *(.rodata.sharebuf_g2_late)
    /* KM rev 5 (pacer, last, after sharebuf G2, merge order; DELTARUNE New only): its bench-end totals and
       trace formats (the two it shares with KM rev 4.1 stay in rev 4.1's place). Every earlier late constant
       keeps its address. */
    *(.rodata.pacer_km5_late)
    /* utfade (utfade, last, after KM rev 5; UNDERTALE only): the two object names (z_utfade.c), then its other
       constants (none expected). */
    *(.rodata.utfade_late)
    *z_utfade.o(.rodata .rodata.*)
    /* datarefuse (auditfixe, last, after KM rev 5, merge order; UNDERTALE only): R215's two constants, the
       read-error stop's message and the mismatch stop's never-read placeholder (z_datarefuse.c). Every earlier late
       constant keeps its address. */
    *(.rodata.datarefuse_late)
    /* datarefuse warn (R243b): the warning's text and the UNSUPPORTED line's format, after R215's two. */
    *(.rodata.datarefuse_warn)
    *z_datarefuse.o(.rodata .rodata.*)
    /* thinart r4: reviewed private EX-entry artwork, appended after landing inputs. */
    *(.rodata.late.thinart)
    *(.rodata.utcredepi_late)
    /* saveaudit: startup error messages, after every existing constant. */
    *z_saveaudit.o(.rodata.late.saveaudit)
    *z_saveaudit_format.o(.rodata .rodata.*)
    *z_saveaudit_scan.o(.rodata .rodata.*)
    *(.rodata.late.audpar)
    /* upd1-s7: pending-settings path, after S7 constants. */
    *(.rodata.late.upd1)
    /* fightpix (after upd1-s7, merge order; UT only): guarded source fingerprints, no artwork. */
    *(.rodata.fightpix_late)
    *(.rodata.late.handover)
    *(.rodata.late.paintchg)
    *(.rodata.paintchg_idle_late)
    *(.rodata.late.noticescroll_pack)
    *(.rodata.late.fpsghost)
    *(.rodata_sfxlate)
    __late_rodata_end = ABSOLUTE(.);
  }
  /* 8 KiB (at first 4 KiB): late code's constants are mostly log formats; heapfrag's are 472 B, and
     screens' 3.4 KB had to go into late text instead. UNDERTALE n3DS had 1.2 KB left. */
  /* CMake substitutes 0x4000 in its build-local copy for profile/reachgate only, and 0x3000 for UNDERTALE Old 3DS
     (45 B were left). Growing this moves .data and .bss by whole pages
     only: check_layout.py --expect-shift verifies section-uniform OBJECT page moves; inspect remaining
     failures (including TLS separately) and require tools/analysis/shiftproof.py PROVEN. */
  __late_rodata_limit = ALIGN(__late_rodata_start, 0x1000) + /* TEST_RODATA_RESERVE */ 0x2000;
  .late.rodata.reserve :
  {
    BYTE(0);
    . += __late_rodata_end < __late_rodata_limit ? __late_rodata_limit - __late_rodata_end - 1 : 0;
  }
  ASSERT(__late_rodata_end < __late_rodata_limit,
         "late constants (.rodata.late.*) outgrew their reserve: see platform/3ds/late_reserve.ld")
}
INSERT AFTER .ARM.exidx;

SECTIONS
{
  /* ===== Writable data: after .data and the linker's .got, exactly 4 KiB, so .tdata, the init arrays and .bss
     move by whole pages only. ===== */
  .late.data . :
  {
    __late_data_start = ABSOLUTE(.);
    /* Features, in merge order (append only): */
    *(.data.late.benchromfs)
    *(.data.late.relprep)
    /* RELPREP_ROMFS_data */
    /* Not listed yet: */
    /* screens (after relprep): after every late input already linked, so that none of those moves. */
    *(.data.late.screens)
    /* contention (after tpaggrow): the Old models' tile copies in VRAM. */
    *(.data.late.contention)
    /* knfuse (after texcache): Native fusion. */
    *(.data.late.knfuse)
    /* o3titan8 (after coverage): reserved for its opt-in phase layout; empty in default builds. */
    *(.data.late.o3titan8)
    /* lockfix (after o3titan8): empty selectors (main's titanthrash 210e822d carries the fix). */
    *(.data.late.lockfix)
    /* typed3 (after lockfix): typed AOT helpers and binding validation. */
    *(.data.late.typed3)
    /* exithang (after pixfix D): bounded exit, deadline and exit log (ctr_exit.c, ctr_exit_log.c). */
    *(.data.late.exithang)
    /* ringstart (after exithang): contention's command ring start reuse and run-on (ctr_gpu_queue.c). */
    *(.data.late.ringstart)
    /* argui (after ringstart): its data. */
    *(.data.late.argui)
    /* exithang follow-up (after argui): bounded diagnostic file delivery's data. */
    *(.data.late.exithang.files)
    /* thrashport (after kawkaw): Old palette-copy retries (ctr_renderer_palette_retry.inc). */
    *(.data.late.thrashport)
    /* layerfix (last): layer Draw mutation helpers (runner.c, vm_builtins.c). */
    *(.data.late.layerfix)
    /* argtower (after layerfix): Old-model Flowery tower faces (ctr_renderer.c, main.c). */
    *(.data.late.perfflowery)
    /* argcrash (after nativemac): friendly crash screen and release-quiet logging (z_argcrash.c). */
    *(.data.late.argcrash)
    /* engfix (after argcrash): game_end closes the game (main.c) and the spare-texture GPU wait (ctr_renderer.c, ctr_gpu_queue.c). */
    *(.data.late.engfix)

    *(.data.late.benchfid)
    *(.data.late.inputlate)
    *(.data.late.pacer)
    *(.data.late.pacer_io)
    *(.data.late.pacer_vc)
    /* utold lava (integrate round 16): its data. */
    *(.data.late.utlava)
    *(SORT_BY_NAME(EXCLUDE_FILE (*z_bottomframe*.o *z_bottompaint.o *z_boxcache.o *ctr_rosalina.o *z_hitcheck.o *z_rosabl.o *z_dimlight.o *z_upd1_options.o *z_upd1_audio.o *z_upd1_text.o *z_thinart.o *z_labcut.o *ctr_fpsbox.o *z_utwindow.o *z_asrtext* *z_adpcm* *ctr_adpcm_probe.o *ctr_audio_system.o *z_knightnew_history.o *z_flowcode.o *z_gogut* *z_builderx_new* *z_builderx_old* *z_loadscreen.o *ctr_benchmark.o *z_sdlat* *ctr_legal.o) .data.late.*))
    *(.data.late.builderxnew)
    *(.data.late.builderxold)
    *(.data.late.builderxoldconst)
    /* utoldloop: append copied local caches here, never in ordinary BSS. */
    *aot_utold_*.o(.data .data.* .bss .bss.* COMMON)
    /* pacer RAM spool (round 17, last, merge order): the spool's pointer, then any other late data of ctr_benchmark.c,
       which is out of the catch-all above. */
    *(.data.late.pacer_rows)
    *ctr_benchmark.o(SORT_BY_NAME(.data.late.*))
    *(.data.late.gogut)
    *(.data.late.sdlat) *z_sdlat*(.data .data.*)
    /* builderx chapter setup (round 19, last, merge order): its progress state. */
    *(.data.late.builderxhash)
    /* credfix (last, merge order): any initialised data of UNDERTALE's text lookup front (none expected). */
    *aot_gettext.o(.data .data.*)
    /* flowfull code paging (round 21, last, merge order; DELTARUNE New flowcode=lazy builds only, default Off, so no
       release build has these sections): z_flowcode.c. z_flowcode.o is out of the catch-all above. */
    *(.data.late.flowfullcode)
    /* knightnew GPU-1 (round 21, last, merge order; DELTARUNE New only, empty elsewhere): z_knightnew_history.c and
       ctr_renderer.c's KnightHistory_layout table. z_knightnew_history.o is out of the catch-all above. */
    *(.data.late.knightnew)
    /* benchfix (last, merge order; both games): the sweep start's chapter-1 data.win answer (ctr_benchmark.c's end,
       the _stat_r wrap). Named outside .data.late.* so ctr_benchmark.o's line above leaves it here. */
    *(.data.benchfix_late)
    /* benchfix (after it, merge order; both games): the window line budget's state. */
    *(.data.benchfix_lines_late)
    /* benchfix round 6 (last, merge order; both games): the held lines' window boundary snapshot. */
    *(.data.benchfix_r6_late)

    *(.data.late.adpcm)
    /* freebytes round 3 (last, merge order): on UNDERTALE Old 3DS only, CMakeLists.txt takes ctr_screens.c's settings
       strings out of the late text block and puts them here: late code is the scarce reserve there, and this page
       had 3.3 KB spare. Read only, never written. Every other target keeps them in late text. */
    /* FREEBYTES_DATA */
    /* utbugs4 (last, merge order): virtual window position and final picture translation. */
    *(.data.late.utbugs4)
    /* fpsbox75zm: DELTARUNE painter/cache lifetime lock, after existing data. */
    *(.data.fpsbox75zm_late)
    /* GPU kick (pacer, from cheaplevers; last, after fpsbox75zm (75zq), merge order; DELTARUNE New only, empty elsewhere;
       it has no data today). */
    *(.data.cheaplevers_kick_late)
    /* drhitbox (last, after the GPU kick (75zq), merge order; DELTARUNE only). */
    *z_drhitbox.o(.data .data.*)
    /* KM rev 5 (pacer, last, after drhitbox, merge order; DELTARUNE New only; it has no data today). */
    *(.data.pacer_km5_late)
    /* pacer C3 (last, after KM rev 5, merge order; UNDERTALE Old only): the codebook share's lock and slots. */
    *(.pacer_share_data)
    /* thinart r4: reviewed private EX-entry artwork, appended after landing inputs. */
    *(.data.late.thinart)
    /* pacer A (last, after thinart, merge order; UNDERTALE Old only): the audio arena's lock, lists and counters. */
    *(.pacer_arena_data)
    *(.data.utcredepi_late)
    /* drawbatch R400: zeroed draw counters in the paid data reserve, after all existing data. */
    *(.data.drawbatch_late)
    /* errtext E1: explicit zero-initialised mutable state in the paid reserve;
       no new BSS, preserving Old's heap page boundary. */
    *(.data_errtext_e1)
    *z_errtext_e1.o(.data .data.*)
    /* bonepulse (after drawbatch on its branch; in S8b after errtext E1, merge order): the three bone objects'
       indices, found once (ctr_renderer.c's end). */
    *(.data.bonepulse_late)
    /* battlesplit: its state (a pointer and a few flags, explicitly initialised), after E1's; no BSS. */
    *(.data_battlesplit)
    *z_battlesplit.o(.data .data.*)
    /* battletouch: its state, explicitly initialised in the paid data reserve (no new BSS, as errtext E1). */
    *(.data_battletouch)
    *z_battletouch.o(.data .data.*)
    /* menutime (scanout, ethanlog5 row 13; UNDERTALE only, empty in DELTARUNE): z_menutime.c's state, explicit
       zeroes in this paid reserve (new late statics would move the Old 3DS's __end__ over a page). */
    *(.data.menutime_late)
    /* heldslow (scanout, logaudit G1/G2; UNDERTALE only): one pointer to its heap state, 4 bytes. */
    *(.data.heldslow_late)
    /* fightpix (in the S8b respin after scanout's heldslow, merge order; UT only): cached epoch bias, refreshed on exact-mode changes. */
    *(.data.fightpix_late)
    *(.data.late.dimlight)
    /* hplcopy and seamlocal (musicloop) keep no late data: their state is on the heap (RULES 11-08). */
    /* layout (layout S1, after bonepulse, merge order; UNDERTALE only): its 8-byte root (explicitly initialised;
       no BSS). */
    *(.data_layout)
    *z_layout.o(.data .data.*)
    /* layout's appended data (layout F8 and S3 on dv8d): lyRoot2, the state added after lyRoot went into dev
       (z_layout.c's .data_layout2; outside .data.*, so the line above doesn't take it). Last, so nothing moves. */
    *(.data_layout2)
    *(.data.late.bottomframe)
    /* bottomflip (bonepulse, 11-09; after layout, merge order; UNDERTALE only): one pointer to its heap state,
       4 bytes. */
    *(.data.bottomflip_late)
    /* noticescroll: 60 B document metadata; heap blocks exist only while Legal is open. */
    *(.data.late.noticescroll)
    *(.data_dlgflash)
    /* letterdraw glyph-pair memo (lead 11-11; UNDERTALE Old only): one pointer to its heap block, 4 bytes. */
    *(.data_letterdraw_memo)
    /* letterdraw plan variants (lead 11-11, L1; UNDERTALE Old only): one pointer to its heap block, 4 bytes. */
    *(.data_letterdraw_plan)
    __late_data_end = ABSOLUTE(.);
  }
  __late_data_limit = __late_data_start + 0x1000;
  .late.data.reserve :
  {
    BYTE(0);
    . += __late_data_end < __late_data_limit ? __late_data_limit - __late_data_end - 1 : 0;
  }
  ASSERT(__late_data_end < __late_data_limit,
         "late data (.data.late.*) outgrew its reserve: see platform/3ds/late_reserve.ld")
}
INSERT BEFORE .tdata;

SECTIONS
{
  /* ===== Zero-initialised statics: after .bss, where growth moves nothing (only __end__). ===== */
  /* Not NOLOAD: that would leave it outside the data segment, where the loader maps nothing. Its inputs are NOBITS,
     so it takes no file space. */
  .late.bss . :
  {
    /* Features, in merge order (append only): */
    *(.bss.late.benchromfs)
    *(.bss.late.relprep)
    /* RELPREP_ROMFS_bss */
    *(.bss.late.loadtime)
    *(.bss.late.colflip)
    /* argcheck (UT late-text reclaim): collision_compat.c's statics' place in UNDERTALE (collision_gm14.c). */
    KEEP(*(.bss.late.colflipgap))
    *(.bss.late.heapfrag)
    /* Not listed yet: */
    *(.bss.late.utpolish)
    /* screens (after relprep): after every late input already linked, so that none of those moves. */
    *(.bss.late.screens)
    /* colflip: collision_gm14.c's statics, last. */
    *(.bss.late.colflip_gm14)
    /* benchromfs (after colflip_gm14): the pre-window observer, after every late input already linked. */
    *(.bss.late.benchromfs.prewindow)
    /* noports: its late statics (lateNameCache), named so that later features' statics go after them. */
    *(.bss.late.noports3)
    /* contention (after tpaggrow): the Old models' tile copies in VRAM. */
    *(.bss.late.contention)
    /* texcache (after contention): packed-copy value ranking. */
    *(.bss.late.texcache)
    /* knfuse (after texcache): Native fusion. */
    *(.bss.late.knfuse)
    /* coverage (after knfuse): the RomFS bench supplement and the state dump's name helper. */
    *(.bss.late.coverage)
    /* o3titan8 (after coverage): reserved for its opt-in phase layout; empty in default builds. */
    *(.bss.late.o3titan8)
    /* lockfix (after o3titan8): empty selectors (main's titanthrash 210e822d carries the fix). */
    *(.bss.late.lockfix)
    /* typed3 (after lockfix): typed AOT helpers and binding validation. */
    *(.bss.late.typed3)
    /* uthud (pixfix A, after alarm14; utgrid reverted): view_snap, UNDERTALE only. */
    *(.bss.late.uthud)
    /* pixfix (task D, DELTARUNE only): after uthud, so that none of those moves. */
    *(.bss.late.pixfix)
    /* exithang (after pixfix D): bounded exit, deadline and exit log (ctr_exit.c, ctr_exit_log.c). */
    *(.bss.late.exithang)
    /* colline (colflip follow-up check, slim; after exithang): colline_gm14.c's statics, last. */
    *(.bss.late.colline_gm14)
    /* menupause (after colline): its statics. */
    *(.bss.late.menupause)
    /* ringstart (after menupause): contention's command ring start reuse and run-on (ctr_gpu_queue.c). */
    *(.bss.late.ringstart)
    /* pixgrid (pixfix task F, UNDERTALE only; after ringstart), so that none of those moves. */
    *(.bss.late.pixgrid)
    /* btnprompt (after pixgrid): the glyphs as read and the prompts' strings. */
    *(.bss.late.btnprompt)
    /* argui (after btnprompt): its statics. */
    *(.bss.late.argui)
    /* exithang follow-up (after argui): bounded diagnostic file delivery's statics. */
    *(.bss.late.exithang.files)
    /* pixpress (pixfix task E, DELTARUNE only): at the end of the list, so that none of those moves. */
    *(.bss.late.pixpress)
    /* pixnslice (pixfix task C, DELTARUNE only): after pixpress, so that none of those moves. */
    *(.bss.late.pixnslice)
    /* utaudio (after pixnslice): ctr_audio_system.c's late statics. */
    *(.bss.late.utaudio)
    /* thrashport (after kawkaw): Old palette-copy retries (ctr_renderer_palette_retry.inc). */
    *(.bss.late.thrashport)
    *(.bss.late.exithang.midfile)
    /* layerfix (last): layer Draw mutation helpers (runner.c, vm_builtins.c). */
    *(.bss.late.layerfix)
    /* argtower (after layerfix): Old-model Flowery tower faces (ctr_renderer.c, main.c). */
    *(.bss.late.perfflowery)
    /* vramtouch (after argtower): whether this is Azahar. */
    *(.bss.late.vramtouch)
    /* argcrash (after nativemac): friendly crash screen and release-quiet logging (z_argcrash.c). */
    *(.bss.late.argcrash)
    /* engfix (after argcrash): game_end closes the game (main.c) and the spare-texture GPU wait (ctr_renderer.c, ctr_gpu_queue.c). */
    *(.bss.late.engfix)
    /* optmenu (after engfix): the options menu's statics. */
    *(.bss.late.optmenu)
    *(.bss.late.coverage.rooms)
    /* pillar (after coverage): the exact fountain mesh's statics, DELTARUNE only. */
    *(.bss.late.pillar)
    /* loadscreen (after pillar): its state. */
    *(.bss.late.loadscreen)
    /* roomplan (after loadscreen): the room plan's statics. */
    *(.bss.late.roomplan)
    /* roomload (after engfix item 6): the room change finishes the room's code and tile reads (z_roomload.c,
       ctr_textures.c's tail). */
    *(.bss.late.roomload)
    /* cmdlabel (after roomload; DELTARUNE only). */
    *(.bss.late.cmdlabel)
    /* o2crash (after cmdlabel): the lab guard's sleep, parked when the heap is gone (ctr_lab.c). */
    *(.bss.late.o2crash)
    /* boxedge (after o2crash): the drawSprite it calls (ctr_boxedge.c). */
    *(.bss.late.boxedge)
    /* dogcheck (after boxedge, merge order; DELTARUNE only): what the rope and dog covered in each buffer. */
    *(.bss.late.dogcheck)
    /* cmdlabel smartmix (after dogcheck): its switch and marks, apart so .bss.late.cmdlabel keeps its size. */
    *(.bss.late.cmdlabel.mix)
    /* drold (G9, after dogcheck's lines, merge order): escape elision's point_direction results (vm_aot_escape.c). */
    *(.bss.late.drold)
    /* natsurf6 (after drold, merge order): native-scale game surfaces beyond the application surface's size (ctr_natsurf6.c). */
    *(.bss.late.natsurf6)
    /* asgdraw (after natsurf6, merge order): the held run's state. */
    *(.bss.late.asgdraw)
    /* flowergpu (after asgdraw, merge order). */
    *(.bss.late.flowergpu)
    /* bggrid (after flowergpu, merge order; DELTARUNE only). */
    *(.bss.late.bggrid)
    /* utstall (after bggrid, merge order). */
    *(.bss.late.utstall)
    /* panelgrid (cmdlabel, after utstall; DELTARUNE only). */
    *(.bss.late.panelgrid)
    /* benchfid (after panelgrid). */
    *(.bss.late.benchfid)
    *(.bss.late.benchfid.hidden)
    /* crispart (after benchfid; DELTARUNE only). */
    *(.bss.late.crispart)
    *(.bss.late.inputlate)
    /* smalltext (after inputlate; UNDERTALE only): Crisp small text's state (z_smalltext.c). */
    *(.bss.late.smalltext)
    *z_smalltext.o(.bss .bss.* COMMON)
    *(.bss.late.pacer)
    /* oomport (integrate round 15, after pacer): checked optional renderer/history arrays. */
    *(.bss.late.oomport)
    /* utflowey (after oomport). */
    *(.bss.late.utflowey)
    /* utflowey iteration 3: z_utflowey.c's sin/cos table. */
    *(.bss.late.utflowey3)
    /* krsharp (after utflowey). */
    *(.bss.late.krsharp)
    /* utsfx (after krsharp; UNDERTALE only): the sound plan's statics. */
    *(.bss.late.utsfx)
    /* utflames heat shimmer: the shimmer script's code entry and variables. */
    *(.bss.late.utflames)
    /* utkill: the plain sprite draw's kept tile part; utdust: the destroyed-instance batch,
       its set and the last gravity direction's pair. */
    *(.bss.late.utkill)
    *(.bss.late.utdust)
    /* utkill vapor (after utdust): the monster dust's last data string and its length (z_utkill_vapor.c). */
    *(.bss.late.utkill.vapor)
    /* utkill swarm (after utkill vapor): the swarm, its state and counters (z_utkill_swarm.c). */
    *(.bss.late.utswarm)
    *z_utkill_swarm*(.bss .bss.* COMMON)
    /* bootlog (after utkill swarm, before the catch-all): its state, and shots builds' boot capture. */
    *(.bss.late.bootlog)
    /* utasgore (round 17, after bootlog, before the catch-all): the steady path's plan cache (utaKeys). */
    *(.bss.late.utasgore)
    *(SORT_BY_NAME(EXCLUDE_FILE (*z_bottomframe*.o *z_bottompaint.o *z_boxcache.o *z_rosabl.o *z_dimlight.o *z_upd1_options.o *z_upd1_audio.o *z_upd1_text.o *z_thinart.o *z_labcut.o *ctr_fpsbox.o *z_utwindow.o *z_asrtext* *z_adpcm* *ctr_adpcm_probe.o *ctr_audio_system.o *z_knightnew_history.o *z_flowcode.o *z_gogut* *z_loadscreen.o *z_symverdict* *z_smalltext_dr* *z_utstall* *z_sdmigrate* *libswkbd_late.a:* *ctr_renderer.o *z_itemverdict.o *z_phaseplan* *z_sdlat* *z_gamedir* *ctr_flowervramg_palette.o *z_saveaudit_format.o *z_shopsplit.o) .bss.late.*))
    /* packscreen (integrate round 16, after the catch-all, merge order): the setup line it draws. */
    *(.bss.late.packscreen)
    /* symverdict (after packscreen; UNDERTALE only): the table's bits. */
    *(.bss.late.symverdict)
    *z_symverdict.o(.bss .bss.* COMMON)
    /* utstall item 7 (integrate round 16, after symverdict, merge order): the plan scene's code ids. */
    *(.bss.late.utstall7)
    /* sdmigrate (after utstall item 7; both games): the chapter check's state, then the placeholder that keeps the
       retired folder move's buffers' bytes (z_sdmigrate.c), so nothing after it moves. Both KEEP'd: UNDERTALE never
       reads the state and nothing reads the placeholder, and --gc-sections would drop them. */
    KEEP(*(.bss.late.sdmigrate))
    /* bonegate (dv8; UT release, diagnostics excluded): its 8 B state, carved from the placeholder below (smSpare is
       8 B shorter under the same condition, z_sdmigrate.c SM_CARVE_BONEGATE), so the block and every later address stay.
       After hf (4-aligned, 12 B) and before smSpare (1-aligned), so no padding is added. */
    KEEP(*(.bss.bonegate_late))
    /* paintchg (dv8, after bonegate, merge order): one heap root each, 4 B, carved the same way (z_sdmigrate.c
       SM_CARVE_BOXCACHE, UT Old only; SM_CARVE_BOTTOMPAINT, UT); all mutable cache payload is allocated. Both
       objects are out of the catch-all above. 4-aligned and before smSpare, so no padding is added. */
    KEEP(*(.bss.late.paintchg))
    KEEP(*(.bss.late.paintchg_idle))
    KEEP(*z_sdmigrate.o(.bss.sdmigrate_spare))
    *z_sdmigrate.o(.bss .bss.* COMMON)
    /* swkbd (last, after sdmigrate, merge order; UNDERTALE only): the essay keyboard's state, then libctru's
       keyboard applet's. */
    *(.bss.late.swkbd)
    *libswkbd_late.a:*(.bss .bss.* COMMON)
    *libctru.a:swkbd.o(.bss .bss.* COMMON)
    /* symverdict on DELTARUNE New (last, round 17, merge order): the table's bits (z_symverdict_dr.c). */
    *(.bss.late.symverdict_dr)
    *z_symverdict_dr.o(.bss .bss.* COMMON)
    /* smartcut (round 17, last, merge order): the item table's last load and the direct tile path's test-build
       counters. Then any other late statics of ctr_renderer.c and z_itemverdict.c, both out of the catch-all above. */
    *(.bss.late.smartcut)
    *ctr_renderer.o(SORT_BY_NAME(.bss.late.*))
    *z_itemverdict.o(SORT_BY_NAME(.bss.late.*))
    /* rosalina2 (round 17, after smartcut, merge order): the Rosalina watcher's statics (ctr_rosalina.c). */
    *(.bss.rosalina2_late)
    *ctr_rosalina.o(.bss .bss.* COMMON)
    /* utflames native answers (round 17, after rosalina2, merge order; UNDERTALE only): collision_circle's kept box (z_utflames.c). */
    *(.bss.utfnative_late)
    /* utflames helix (round 17, after the native answers; UNDERTALE only): the sine shape's soul box and sine memo. */
    *(.bss.utfhelix_late)
    /* utflowey (round 17, after utflames helix, merge order; UNDERTALE only): the cut scene's aged tiles and their
       real frames. */
    *(.bss.late.utflpin)
    /* phaseplan (DELTARUNE only, last, after utflpin): the current room's phases (z_phaseplan.c). */
    *(.bss.late.phaseplan)
    *z_phaseplan.o(.bss .bss.* COMMON)
    *(.bss.late.gogut)
    *(.bss.late.sdlat) *z_sdlat*(.bss .bss.* COMMON)
    /* benchshown (last, merge order): the presentation v1 counts per room visit (named outside .bss.late.*, so that
       the catch-all above leaves them here). */
    *(.bss.benchshown_late)
    /* utstall item 8 step 1 (both games, last, merge order): the reader's game cache pointer (ctr_textures.c).
       Named outside .bss.late.* so the sorted catch-all above leaves it here. */
    *(.bss.utstall8_late)
    /* utstall item 8 step 1c (both games): the complete marker's last answer (ctr_texture_pack.c). */
    *(.bss.utstall8c_late)
    /* utflowey packed tier (round 18, after utstall item 8, merge order; UNDERTALE Old only): each page's SD reads. */
    *(.bss.late.utflpk)
    /* drresid (DELTARUNE New, last, merge order): its state (z_drresid.c). */
    *(.bss.drresid_late)
    *z_drresid.o(.bss .bss.* COMMON)
    /* utold partial sprites (round 19, last, merge order; both games): the partial-tile path's tile cache. Named outside
       .bss.late.* so ctr_renderer.o's own late line above leaves it here. */
    *(.bss.utpart_late)
    /* utflowey turned culls (round 19, before flowhalf, merge order; UNDERTALE only): the kept pre-cull state. */
    *(.bss.utfldraw_late)
    /* flowhalf (last, halfart and halfcensus builds only): the patch's state and the census's counters. */
    *(.bss.flowhalf_late)
    /* spikerec (round 20, merge order; playtest builds only; named outside .bss.late.*): the spike ring (8 KiB) and its state (z_spikerec.c). */
    *(.bss.spikerec_late)
    *z_spikerec.o(.bss .bss.* COMMON)
    /* soulshake (round 20, merge order; UNDERTALE only): the view follower's state. */
    *(.bss.soulshake_late)
    /* gamedir (last, after soulshake, merge order): the layer's state (z_gamedir.c). */
    *(.bss.late.gamedir)
    *z_gamedir.o(.bss .bss.* COMMON)
    /* gamedir2's menu sounds' state (ctr_audio_system.c), named outside .bss.late.*. */
    *(.bss.gamedir2_late)
    /* legalui (round 21, last, after soulshake, merge order): the Legal notices and Credits pages' state (ctr_legal.c's end). */
    *(.bss.legalui_late)
    /* smalltext on DELTARUNE New (sansfont round 3, last, merge order): Crisp small text's state (z_smalltext_dr.c). */
    *(.bss.late.smalltext_dr)
    *z_smalltext_dr.o(.bss .bss.* COMMON)
    /* bubbleclip (round 21, after soulshake, merge order; UNDERTALE only): the writer's variable ids, the bubble table and the writers' plans
       (z_smalltext.c's end).
       Named outside .bss.* so z_smalltext.o's line above leaves it here. */
    *(.bss_bubbleclip_late)
    /* credfix (after flowhalf, merge order): each voice's own gain. */
    *(.bss.credfix_late)
    /* credfix: any statics of UNDERTALE's text lookup front (aot_gettext.c; none expected). */
    *aot_gettext.o(.bss .bss.* COMMON)
    *(.bss.late.flowervramg_palette)
    /* flowfull code paging (round 21, last, merge order; DELTARUNE New flowcode=lazy builds only, default Off, so no
       release build has these sections): z_flowcode.c. z_flowcode.o is out of the catch-all above. */
    *(.bss.late.flowfullcode)
    /* knightnew GPU-1 (round 21, last, merge order; DELTARUNE New only, empty elsewhere): z_knightnew_history.c and
       ctr_renderer.c's KnightHistory_layout table. z_knightnew_history.o is out of the catch-all above. */
    *(.bss.late.knightnew)
    /* utflowey2 (option 4, round 21f, last, merge order; UNDERTALE Old only): its state (z_utstall.c's end). */
    *(.bss.utfl2_late)
    /* dodgefront (credfix, last, merge order; UNDERTALE Old only): the front's lookups (z_dodgefront.c). */
    *(.bss.dodgefront_late)
    *z_dodgefront.o(.bss .bss.* COMMON)
    /* leavekeep (credfix; last in release builds, merge order): the exit mark and the delayed crash test's state. */
    *(.bss.leavekeep_late)
    *z_leavekeep.o(.bss .bss.* COMMON)
    /* exittrail (last, merge order; exittrail builds only): the trail's line ring and state. */
    *(.bss.exittrail_late)
    *z_exittrail.o(.bss .bss.* COMMON)
    /* flowend (last, merge order; UNDERTALE only): the room's planned sounds kept from the shedding (ctr_audio_system.c's
       end). Named outside .bss.late.* so the catch-alls above leave it here. */
    *(.bss.flowend_late)
    /* repack (DELTARUNE New, last, merge order): its state (ctr_repack.inc). */
    *(.bss.repack_late)
    /* offkill (offdraw, last, merge order; UNDERTALE only): the entries of romfs:/offkill.bin and their count
       (z_offkill.c). Named outside .bss.late.* so the catch-alls above leave it here. */
    *(.bss.offkill_late)
    *z_offkill.o(.bss .bss.* COMMON)
    /* flowend L5 (last, merge order; UNDERTALE only): a background decode's loop-seam measure (ctr_audio_system.c).
       Its own section, so the statics in .bss.flowend_late keep their offsets from that section's anchor. */
    *(.bss.flowend_seam)
    /* freewins (round 2, last, merge order; both games): the stats block's last heap figures and its window count
       (main.c's end). Named outside .bss.late.* so the catch-all above leaves it here. */
    *(.bss.freewins_late)
    /* stallfix rev 3 (last, merge order; both games): the held frames of the scene being measured (ctr_benchheld.inc). */
    *(.bss.stallfix_late)
    /* utorder (credfix, last, merge order; UNDERTALE only): the draw order key counter (z_utorder.c). */
    *(.bss.utorder_late)
    /* utgapfix (last, merge order; UNDERTALE only): the builtin string_char_at's walk falls back to (z_utflames.c's end). */
    *(.bss.utgapfix_late)
    /* textfront (simplefix, last, merge order; UNDERTALE Old only): the fronts' lookups (z_textfront.c). Named
       outside .bss.late.* so the catch-alls above leave it here. */
    *(.bss.textfront_late)
    *z_textfront.o(.bss .bss.* COMMON)
    /* writerfront (freebies, after textfront; UNDERTALE Old only): the front's lookups (z_writerfront.c). */
    *(.bss.writerfront_late)
    *z_writerfront.o(.bss .bss.* COMMON)
    /* freebies restart preload (last, merge order; UNDERTALE only): the plan, the restart and the last scene key
       (z_freebies.c). Named outside .bss.late.* so the catch-alls above leave it here. */
    *(.bss.freebies_late)
    *z_freebies.o(.bss .bss.* COMMON)
    /* freewins (round 4c, after its round 2 line; UNDERTALE Old only): L1's texture cache and its busy flag
       (z_utstall_l2.c). */
    *(.bss.freewins_r4_late)
    /* freewins (round 5, after its round 4c line; UNDERTALE Old only): the two big ds_maps' storage parked between
       a VM reset and their first ds_map_add (vm_builtins.c's end, freewins_maps.h). */
    *(.bss.freewins_r5_late)

    /* flowpitch (flowend, last, merge order; both games): its log-once flag (ctr_audio_system.c's end). */
    *(.bss.flowpitch_late)
    *(.bss.late.adpcm)
    /* The PCM blocks waiting for their channel (ctr_audio_system.c's end). */
    *(.bss.pcmwait_late)
    /* benchfix round 7 (last, merge order; both games): the held boundary's room start and hidden-read flag
       (ctr_benchmark.c's end); DELTARUNE Old: the tour warm-up's last room (bfTourRoom). */
    *(.bss.benchfix_r7_late)
    /* frametrace (benchfix, last, merge order; test builds only): its state, the open files' names and the
       dump's 4 KiB buffer (z_frametrace.c; the records themselves are allocated). */
    *(.bss.frametrace_late)
    *z_frametrace.o(.bss .bss.* COMMON)
    /* utold Continue transition (last, merge order): the original script code index. */
    *(.bss.utoldcontblack_late)
    /* knightgpu L-A (last, merge order; DELTARUNE New only): the lending state (ctr_renderer.c's end). Named outside
       .bss.late.* so ctr_renderer.o's own late line above leaves it here. */
    *(.bss.knightgpu_late)
    /* utold c112dtm: the pending record's cache (4 B). */
    *(.bss.utolddtm_late)
    /* utext (credfix T1, last, merge order; UNDERTALE test builds only, empty in release): UTX_SHADOW's counters. */
    *(.bss.utext_late)
    /* usstop (last, merge order): the provoke test's read count (exit trail builds only). */
    *(.bss.usstop_late)
    /* asrtext (last, merge order): the optional glyph rule in the loaded pack. */
    *(.bss.late.asrtext)
    /* flowend burst (last, merge order): the step's play count and pending-voice bits (8 B, UNDERTALE only). */
    *(.bss.flowburst_late)
    /* usstop rev 5 (last, merge order): the owner's veto on the exit's heap frees (1 B, UNDERTALE only; its own
       section so the existing usstop statics keep their places). */
    *(.bss.usstop_veto)
    /* cdid (creddodge lever 2, last, merge order; both games): the 16 cached entry indexes (64 B). */
    *(.bss.cdid_late)
    /* cdbox (creddodge lever 3, last, merge order; UNDERTALE only): the box kept for the next three bbox reads. */
    *(.bss.cdbox_late)
    /* utbugs4 (last, merge order): unmodified virtual window coordinates. */
    *(.bss.late.utbugs4)
    /* tbox (creddodge; after utbugs4): two bits of cursor verdict per font (16 B) and the fitted reach (16 B). */
    *(.bss.tbox_late)
    /* pacebot (pacer, last production statics, merge order): the GPU queue's VBlank1 callback, chained (4 B). */
    *(.bss.pacebot_late)
    /* flowpre (flowend r20, last, merge order; UNDERTALE only): the Omega Flowey phase song's spare decoder, its
       path and state (1,017 B); its own last section so every earlier late static keeps its address. */
    *(.bss.flowpre_late)
    /* flowstream (last, merge order: after flowpre; UNDERTALE Old only): the plan tiles seen resident,
       and the refill read. */
    *(.bss.flowstream_late)
    /* trailfront (asrfight, after flowstream and before fpsbox, merge order; UNDERTALE Old only): its lookups
       and cached builtins (37 B). */
    *(.bss.trailfront_late)
    /* fpsbox (last, merge order; tail replacement, after flowstream and trailfront): main-thread display sampler
       (12 B). */
    *(.bss.late.fpsbox)
    /* exitpanic (merge order; both games): the loader's return address record (4 B, z_leavekeep.c's lkLoader).
       Named outside .bss.* so z_leavekeep.o's line above leaves it here. */
    *(.bss_exitpanic_late)
    /* itemfront (asrfight, after exitpanic, merge order; UNDERTALE Old only): its lookups (~60 B). */
    *(.bss.itemfront_late)
    /* evguard (asgore30, after itemfront, merge order; UNDERTALE only): the current run's test (24 B). */
    *(.bss.evguard_late)
    /* flowhitch (flowend r22, after evguard, merge order; UNDERTALE only): the sounds the guard shed
       this room, their forms before, the spare decoders and first buffers of rev 3; its own last section so every
       earlier late static keeps its address. */
    *(.bss.flowhitch_late)
    /* flowhitch rev 4 (flowend r25, after rev 3's, UNDERTALE only): its room and song marks. */
    *(.bss.flowhitch4_late)
    *(.bss.late.loadaudio) /* after every existing flowhitch static */
    /* postkick (pacer, after loadaudio, merge order; UNDERTALE only): the held runner (4 B). */
    *(.bss.postkick_late)
    /* utrect (uthunt, after postkick, merge order; UNDERTALE only): the script's code index and its draw's
       function index (8 B). */
    *(.bss.utrect_late)
    /* uttc (uthunt T1, after utrect, merge order; UNDERTALE only): the tile index's key and block pointer. */
    *(.bss.uttc_late)
    /* knightgpu d2 (last, merge order; DELTARUNE New only): the quads of a held pass kept to be made a tile at a time
       (ctr_renderer.c's end). Named outside .bss.late.* so ctr_renderer.o's own late line above leaves it here. */
    *(.bss.knightgpu_tiled_late)
    /* knightgpu O9 (after d2, merge order; DELTARUNE New only): the held calls (ctr_renderer.c's end). */
    *(.bss.knightgpu_lazy_late)
    /* knightgpu A (after O9, merge order; DELTARUNE New only): the compaction's block lists and plan (ctr_renderer.c's
       and ctr_memory.c's ends). */
    *(.bss.knightgpu_compact_late)
    /* creddodge presize (creddodge, last, after knightgpu A, merge order; UNDERTALE only): each object's last
       selfVars capacity (1 KiB, colline_gm14.c's end). */
    *(.bss.cdcap_late)
    /* benchcut (flowend r30, after knightgpu A and creddodge presize, merge order; both games; bench only): the
       label shadows. */
    *(.bss.benchcut_late)
    /* codeclose textcase (lever 1, after benchcut, merge order; UNDERTALE Old only): the table's header and
       pointer, its state and the direct-jump count (z_textcase.c). */
    *(.bss.codeclose_case_late)
    *z_textcase.o(.bss .bss.* COMMON)
    /* codeclose writerfirst (lever 2, after textcase, merge order; UNDERTALE Old writerfront builds only): the Draw's
       code index, the wrapped function, the variables and the count (z_writerfirst.c). */
    *(.bss.codeclose_wf_late)
    *z_writerfirst.o(.bss .bss.* COMMON)
    /* utburst (flowend r37, after writerfirst; UNDERTALE only): the script checks and the held vines. */
    *(.bss.utburst_late)
    *z_utburst.o(.bss .bss.* COMMON)
    /* flowerhw D'+E (after utburst, merge order; DELTARUNE New only): cut 7d's emit state and
       per-strip u table (vine_new_retain.inc); towerprof's counters in its test builds. */
    *(.bss.flowerhw_late)
    /* knightgpu FC (after flowerhw D'+E, before flowerhw tshare; DELTARUNE New only): its per-room state
       (ctr_renderer.c's end). */
    *(.bss.knightgpu_fc_late)
    /* flowerhw tshare (last, merge order; DELTARUNE New only): the wait's frame and counts (vine_new_retain.inc's end). */
    *(.bss.tshare_late)
    /* racefix (last, merge order): the lost save paths and their lock (ctr_file_system.c's end). */
    *(.bss.racefix_late)
    /* utring (flowend r39; last, after racefix, merge order; UNDERTALE Old only): instance_create's builtin and its data.win. */
    *(.bss.utring_late)
    *z_utring.o(.bss .bss.* COMMON)
    /* btlentry (last, after utring, merge order; UNDERTALE Old only): its object indices, room visit and reads in flight. */
    __btlentry_bss_start = .;
    *(.bss.btlentry_late)
    /* btlentry's bss slot: 520 B (r3 uses 100 B; r5 adds about 192 B), the same rule as its text slot. */
    __btlentry_bss_gap_start = ABSOLUTE(.);
    . = . > __btlentry_bss_start ? __btlentry_bss_start + 520 : .;
    __btlentry_bss_gap_end = ABSOLUTE(.);
    /* titandiag (last, merge order; titandiag builds only): the recorder's entry table and state. */
    *(.bss.titandiag_late)
    *z_titandiag.o(.bss .bss.* COMMON)
    /* titancpu L1 (last, merge order; DELTARUNE New only): the surfaces' sample stamps. */
    *(.bss.titancpu_late)
    /* pacerflush (pacer gbot; last, after titancpu, merge order; both games): where the top consoles are (z_pacerflush.c). */
    *(.bss.pacerflush_late)
    /* fpsbox75zm (last, after pacerflush, merge order): the counter state. */
    *(.bss.fpsbox75zm_late)
    /* audofix drone and load play-through (playtest 11-01/11-02, last, merge order; UNDERTALE only): the stream contexts'
       cached copies (ctr_audio_system.c's end), the load window's voice snapshot (ctr_loadaudio_mix.inc's end). */
    *(.bss.audofix_late)
    /* knightgpu FC rev 2 (after tshare, so every late static before keeps its address, c26rev r103 F1; DELTARUNE New
       only): its list of copies, the block buffer and the hot-list buffers (ctr_renderer.c's end). */
    *(.bss.knightgpu_fc2_late)
    /* omegaflowey ofloader (last, after knightgpu FC rev 2, merge order; UNDERTALE only): the audio loader's kept-open
       files and their paths (z_ofloader.c). Named outside .bss.* so the main .bss line leaves it here. */
    *(.bss_omegaflowey_ofloader)
    /* ctrlui (last, after omegaflowey ofloader, merge order; both games): the Controls page's cursor (ctr_ctrlui.c). */
    *(.bss.ctrlui_late)
    /* GPU kick (pacer, from cheaplevers; last, after ctrlui (75zq), merge order; DELTARUNE New only, empty elsewhere):
       its prefix queue view and entries, the trace and the rev3 gate. */
    *(.bss.cheaplevers_kick_late)
    /* omegaflowey ifg (last, after the GPU kick (75zq), merge order; UNDERTALE only): the entries of romfs:/offdoom.bin, their
       count and the destroyed count (z_offkill.c). Named outside .bss.* so the main .bss line leaves it here. */
    *(.bss_omegaflowey_ifg)
    /* flowerhw sun (last, after omegaflowey ifg (75zq), merge order; DELTARUNE New only, empty elsewhere): the phase plan's
       readied set (z_phaseplan.c's ppRd; named outside .bss.* so z_phaseplan.o's catch-all leaves it here). */
    *(.bss_flowerhw_l2)
    /* gsfix (last, after flowerhw sun (75zq), merge order; DELTARUNE only, empty in UNDERTALE): its VRAM block list and
       hold state (ctr_renderer.c's end). */
    *(.bss.gsfix_late)
    /* sharebuf (titancpu, last, after gsfix (75zq unit 16), merge order; DELTARUNE New only): its transients,
       the scratches it took over and the list (ctr_renderer_sharebuf.inc). */
    *(.bss.sharebuf_late)
    /* flowerhw cold (DELTARUNE New, last): the last room load's data.win and whether its room was a front-end room
       (z_drresid.c; named outside .bss.* so z_drresid.o's line leaves it here). */
    *(.bss_flowerhw_cold)
    /* drhitbox (last, after flowerhw cold (75zq unit 20), merge order; DELTARUNE only): runtime gate. */
    *(.bss.drhitbox_late)
    *z_drhitbox.o(.bss .bss.* COMMON)
    /* sharebuf G2 (titancpu, last, after drhitbox, merge order; DELTARUNE New only): its slots and counters. */
    *(.bss.sharebuf_g2_late)
    /* flowerhw stfix (last, after sharebuf G2 (75zq unit 23), merge order; DELTARUNE New only): the renderer that has
       smalltext's outer table (z_smalltext_dr.c; named outside .bss.* so the catch-alls leave it here). */
    *(.bss_flowerhw_stfix)
    /* g2log (titancpu, last, after flowerhw stfix, merge order; DELTARUNE New only): the room its line was last
       logged in. Named outside .bss.* so ctr_renderer.o's lines above leave it here. */
    *(.bss_titancpu_g2log)
    /* dofl (titancpu, last, after titancpu g2log (75zq23), merge order; DELTARUNE only): the loader's kept-open
       files and their paths (z_ofloader.c). Named outside .bss.* so the main .bss line leaves it here. */
    *(.bss_titancpu_dofl)
    /* KM rev 5 (pacer, last, after titancpu dofl (zq25's last late state), merge order; DELTARUNE New only):
       its chain, sealed entries, per-queue GPU-time records and gate (and the trace ring in gpukick-trace
       builds). KM rev 4.1's .bss.cheaplevers_kick_late above keeps its place and size. */
    *(.bss.pacer_km5_late)
    *(.bss.weirdshd_pinwheel_late)
    /* omegaflowey pcorder (last, after weirdshd pinwheel (75zq unit 34), merge order; UNDERTALE only): the flag that
       marks the draw list after a pass that went on in its starting order (runner.c's end; named outside .bss.* so the
       catch-alls leave it here). */
    *(.bss_omegaflowey_pcorder)
    /* spdmemo (creddodge, last, after omegaflowey pcorder (75zq unit 39), merge order; both games): its
       two entries (z_spdmemo.c; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.spdmemo_late)
    /* inisafe (auditfixe DEEP-01, after spdmemo, merge order; both games): the builtins its stubs lead to (z_inisafe.c;
       named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.inisafe_late)
    /* dndrel (auditfixe SCOURS3-002, after inisafe, merge order; UNDERTALE): the two builtins it leads to
       (z_dndrel.c). */
    *(.bss.dndrel_late)
    /* symkeep (auditfixe SCOURT5-003 R193, after dndrel, merge order; UNDERTALE): the renderer and its copy
       array's address (z_symkeep.c). */
    *(.bss.symkeep_late)
    /* sleepflush (auditfixe SCOURS2-005, after inisafe, merge order; both games): its file system pointer, hook
       cookie and flag (z_sleepflush.c). */
    *(.bss.sleepflush_late)
    /* wrapfix (auditfixe SCOURT2-001, after symkeep, merge order; UNDERTALE): the draw_text_ext builtin it
       replaced (z_wrapfix.c). */
    *(.bss.wrapfix_late)
    /* overlayread (auditfixe3, SCOURSOL3-002, after sleepflush; both games): its copy of the overlay vtable and
       the original's address (z_overlayread.c). */
    *(.bss.overlayread_late)
    /* codeverify (auditfixe2 AUD-16, after overlayread, merge order; UNDERTALE Old only): its 12 B of state
       (z_btlentry.c's end; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.codeverify_late)
    /* utfade (utfade, after overlayread; UNDERTALE only): z_utfade.c's statics (none in release builds; the
       probe's flag in utfadeprobe test builds). */
    *z_utfade.o(.bss .bss.* COMMON)
    /* utfade A (utfade, after utfade; UNDERTALE Old only): the doors' object indices and the door's read-ahead
       (z_btlentry.c's end; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.utfadea_late)
    /* datarefuse warn (auditfixe R215c, last, after utfade A, merge order; UNDERTALE only): the unsupported
       data.win's line for crash and hang reports, and the warning test's frame count in its test builds
       (z_datarefuse.c). */
    *(.bss.datarefuse_late)
    /* rosabl (rosafreeze3 bug 2, after the datarefuse warn, merge order; both games): the watcher's pad copy, the
       combo, the hold and the held-back power-off flag (z_rosabl.c; named outside .bss.late.*). */
    *(.bss.rosabl_late)
    /* thinart r4: reviewed private EX-entry artwork, appended after landing inputs. */
    *(.bss.late.thinart)
    /* auditfixg-r2 SCOURS2-009: two owned thread handles and the create/exit handshake. */
    *(.bss_auditfixg_r2_heartbeat)
    *(.bss_auditfixg_f2_heartbeat)
    /* utsdread (auditfixe2 UTSCOURD-001, after auditfixg F2, merge order; UNDERTALE only): whether the latest fatal
       line was an SD card read (z_argcrash.c's end; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.utsdread_late)
    /* utsdmem (auditfixe2 UTSCOURD F4, after utsdread; UNDERTALE only): whether the data.win check's buffer
       allocation failed (ctr_data_version.c's end). */
    *(.bss.utsdmem_late)
    /* pacealign (auditfixe R278b, after utsdmem, merge order; UNDERTALE only): the bench window edges waiting
       for the pacer's cut (z_pacealign.c; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.pacealign_late)
    /* SCOURS4-005 and the fatal claim's guards (auditfixe2 R216b, after pacealign, merge order; R216d: UNDERTALE
       only, empty in DELTARUNE):
       lsGate, the bar's draw gate (z_loadscreen.c's end), the crash-screen gate's state (z_argcrash.c's end), the
       fatal claim's clock (ctr_lab.c's end) and the save guard's state (ctr_file_system.c's end). Named outside
       .bss.late.* so the catch-alls leave them here; appended. */
    *(.bss.lshalt_late)
    *(.bss.argcrashfix_late)
    *(.bss.labfatal_late)
    *(.bss.saveguard_late)
    /* R216b5 (UNDERTALE only; empty in DELTARUNE): the Rosalina watcher's published handle and its create mark, and
       (R216d) its entry and the crash-stop mark (ctr_rosalina.c's end), appended; named outside .bss.* so the file's
       own catch-all above leaves it here. */
    *(.bss_rosaquiet_late)
    /* R216c (UNDERTALE only; empty in DELTARUNE): the departing crash screen's repaint deadline (z_argcrash.c's end,
       CtrArgcrash_depart), appended the same way. */
    *(.bss_argdepart_late)
    /* R270: whether this LCD move actually preserved a console page; outside
       .bss.late.* so the earlier sorted catch-all cannot move existing state. */
    *(.bss.dlgtopguard_late)
    /* utcredepi R327 (after R270's .bss.dlgtopguard_late, merge order): empty in release builds. */
    *(.bss.utcredepi_late)
    /* credroll (creddodge CREDCLIP-002, after utcredepi, merge order; UNDERTALE only): the Credits page drawn
       last, its roll position and its latest line (z_credclip.c; named outside .bss.late.*). */
    *(.bss.credroll_late)
    /* thinart edge (R336, after credroll, merge order): semantic border lookup, 12 B. */
    *(.bss.thinart_edge_late)
    /* bulletsplit (creddodge, after thinart edge, merge order; UNDERTALE Old writerfront builds only): the waiting text
       (z_bulletsplit.c; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.bulletsplit_late)
    *z_bulletsplit.o(.bss .bss.* COMMON)
    /* saveaudit: original builtin pointers, after all existing statics. */
    *z_saveaudit_format.o(.bss .bss.* COMMON)
    *(.bss.saveauditfatal_late)
    /* gomusic (omegaflowey Bug 1, after saveaudit, merge order; UNDERTALE only): the song-copy groups
       (ctr_audio_system.c's end; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.gomusic_late)
    /* sfxshed (omegaflowey R325b, after gomusic, merge order; UNDERTALE Old only): the room's sound plan as asked
       for (z_utsfx.c, read by utshedFinish; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.sfxshed_late)
    /* gounder (gounder, after audpar, merge order; UNDERTALE Old only): the shared decode's state
       (ctr_audio_system.c's end; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.gounder_late)
    /* menupress (menusound, after gounder, merge order; both games): the press being handled (ctr_optmenu.c). */
    *(.bss.menupress_late)
    /* homeaudio (after menupress, merge order; UNDERTALE only): the HOME hold's flag and voices (ctr_audio_system.c's end; named outside
       .bss.late.* so the catch-alls leave it here). */
    *(.bss.homeaudio_late)
    /* dlgblank (crispgap, after homeaudio, merge order; UNDERTALE only): VBlank1's time and a clear waiting for it. */
    *(.bss.dlgblank_late)
    /* keep-lit (dlglight, after every other append; UNDERTALE only): whether the bottom stays lit. */
    *z_keeplit.o(.bss .bss.* COMMON)
    /* UT-001 lowering (rosaport, after keep-lit, merge order; UNDERTALE Old only): z_rosalow.c's state. */
    *(.bss.rosalow_late)
    /* First update (upd1), after keep-lit (the last S7 state) and R380c's lowering, in merge order. */
    /* upd1-s7: pending-options snapshot, first after the lowering. */
    *(.bss.late.upd1)
    /* drawbatch seam copy (after upd1-s7, merge order; UNDERTALE only): the copies' slots and counters
       (ctr_renderer.c's end; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.drawbatch_late)
    /* entryload partial tiles (after drawbatch, merge order; UNDERTALE Old only): the font tiles it keeps and
       their cache (ctr_textures.c's end; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.entryload_late)
    /* thinrule (first update, after entryload, merge order; R414 F2; UNDERTALE only): the whole-rows table's
       pointers and the data.win it was read for (z_thinrule.c; named outside .bss.late.* so the catch-alls leave it
       here). */
    *(.bss.thinrule_late)
    /* menufirst R420b (after thinrule, merge order (F2); UNDERTALE only): whether a vblank wait just came, ahead of the options
       menu's copy (ctr_optmenu.c's end; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.menufirst_late)
    /* dwkeep (loadclass 10, after menufirst, merge order; UNDERTALE only): its layer's device table, the kept
       handle and path (z_dwkeep.c; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.dwkeep_late)
    /* labguard (after menufirst, merge order; UNDERTALE non-public builds only): z_labguard.c's state. */
    *z_labguard.o(.bss .bss.* COMMON)
    /* dimlight heap variant (lead approved 11-08): one 4 B process-lifetime
       heap root, after menufirst; all other mutable state is allocated once. */
    *(.bss.late.dimlight)
    /* asgorelong T1 (after menufirst, merge order; UNDERTALE only): the bubble clip's plans 3 to 15 and every plan's
       bookkeeping (z_smalltext.c's bcMore and bcAux; plans 0 to 2 stay in bc); named outside .bss.* so z_smalltext.o's
       line leaves it here. */
    *(.bss_asgorelong_late)
    /* tilenames (tilemiss round 10, after menufirst, merge order; UNDERTALE Old only): the room visit its line
       count is for (z_utstall.c's end; named outside .bss.late.* so the catch-alls leave it here). */
    *(.bss.tilenames_late)
    /* crispread2 (crispread, after menufirst on its branch; in dev after tilenames, merge order; UNDERTALE only):
       each bubble plan's 1 px gap lines (BC_SLOTS of them, asgorelong T1's 16) and whether the glyph being placed is
       on one (z_smalltext.c's end; named outside .bss.* so no catch-all takes it). */
    *(.bss_crispread2)
    /* bottomframe: three heap/context/cover root words. */
    *(.bss.late.bottomframe)
    /* UT Frame retirement: retain its old 12 B tail so no heap-boundary
       pointer changes. AAC hole markers exist only in the UT linker script. */
    __bottomframe_retired_bss_pad_start = ABSOLUTE(.);
    . += DEFINED(__aacout_text_retired_frame_start) ? 12 : 0;
    __bottomframe_retired_bss_pad_end = ABSOLUTE(.);
    /* bootahead (first update item 1, bootahead2, after bottomframe, merge order; UNDERTALE only): the menu song's
       name the game thread handed to the audio opener and the opener hasn't taken yet (ctr_bootahead.inc's
       bootaheadAsk, 4 B; named outside .bss.late.* so the catch-alls leave it here). Its code and constants are in the
       AAC slot (aacout.cmake). */
    *(.bss.bootahead_late)
    /* Old 1.0 envelope (relready-resize.md): CMakeLists.txt replaces this on UNDERTALE Old with 284 B of zeroed .bss;
       empty everywhere else. */
    /* RESIZE_BSS_RESERVE */
    /* shopsplit: 4 B ownership flags, appended after composed dv8q state. */
    *(.bss.late.shopsplit)
    /* battlesplit-rework (lead 11-10): z_battlesplitbsr.c's 1 B (the hold flag), then what development builds only
       carry (z_dimlight.c's backlight log records): appended after shopsplit's, so nothing before it moves. Named
       outside .bss.late.*, so the catch-all above leaves them here. */
    *(.bss.bsr_late)
    *(.bss.bsrdev_late)
    /* touchui: pointer to the registration/move marker heap, after existing state. */
    *(.bss.touchui_late)
    *(.bss.touchquiz_late)
    *(.bss.touchmenus_late)
    *(.bss.touchoverworld_late)
    *(.bss.touchname_late)
    /* S4 exclusive top-target loan descriptor; no GPU allocation. Outside
       .bss.late.* so ctr_renderer.o's earlier catchall cannot move old state. */
    *(.bss.dlgtop4)
    *(.bss_menulag)
    *(.bss.late.fpsghost)
    /* battlesplit22: the Split's top-margin records (UNDERTALE only). */
    *(.bss_bsmargin22_late)
    /* ballast (build.sh ballast test builds only, empty in every release build): z_ballast.c's state, last, so a
       ballast build keeps every late static's address of the release build. Keep this line last in late .bss; new
       appends go above this comment. Its code and constants are in the AAC slots (aacout.cmake). */
    *(.bss.ballast_late)
    /* 3dsxtool refuses a segment whose size isn't a multiple of 4 ("The segments is not word-aligned!"), and this
       block ends the RW segment: late statics of odd sizes (bools) would end it off a word. */
    . = ALIGN(4);
  }
  /* 3dsx.ld set these just before this block; crt0 clears .bss up to __bss_end__, so move both past the late
     statics. */
  __bss_end__ = .;
  __end__ = ABSOLUTE(.);
}
INSERT AFTER .bss;

/* Each reserve really ends where the next segment starts. */
ASSERT(ADDR(.rodata) == __late_text_limit, ".rodata does not start at the end of the late code reserve")
ASSERT(ADDR(.data) == __late_rodata_limit, ".data does not start at the end of the late constants reserve")
