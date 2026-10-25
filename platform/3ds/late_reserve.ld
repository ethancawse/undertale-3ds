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
   compares a candidate ELF with its base and reports moves and the reserve left. */

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
    *(.text.late.relprep)
    /* RELPREP_ROMFS_text */
    *(.text.late.loadtime)
    *(.text.late.colflip)
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
    *ctrport.dir/source/ctr_gpu_queue.o(.text.C3D_FrameSync)
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
    *z_smalltext.o(.text .text.* .rodata .rodata.*)
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
    *(.text.late.utflames)
    *z_utflames.o(.text .text.* .rodata .rodata.*)
    /* utflowey (after utflames; both games): the room load's swap of older rooms' tiles
       (ctr_textures.c's end), then its constants. */
    *(.text.late.utflowey)
    *(.rodata.utflowey_in_text)
    /* utflowey: UNDERTALE's late copies of generated functions (gmlaot.py's LATE_COPIES), the whole file with its
       constants and any out-of-line copies of header functions; DELTARUNE's AOT has no aot_late.c. */
    *aot_late.o(.text .text.* .rodata .rodata.*)
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
    /* utflames heat shimmer (after utsfx; UNDERTALE only): the shimmer script's builtin
       (z_utflames_waver.c), then its constants. */
    *(.text.late.utflames.waver)
    *z_utflames_waver.o(.text .text.* .rodata .rodata.*)
    /* utkill (after the utflames shimmer; UNDERTALE only): plain sprite draws (ctr_renderer.c's
       tail, utkDrawSprite). Then utdust: destroyed instances leave the object lists in batches, and gravity keeps
       its cosine and sine (z_utdust.c; no constants, and anything else of the file lands here too). */
    *(.text.late.utkill)
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
    *z_utkill_waver.o(.text .text.* .rodata .rodata.*)
    /* bootlog (after utkill waver, before the catch-all; both games): the boot's log unseen unless Other
       screen is Stats and log (main.c's tail), then its constants. */
    *(.text.late.bootlog)
    *(.rodata.bootlog_in_text)
    /* Any other late text (round 14 dropped the wildcard ahead of the host table: it placed nothing). */
    *(SORT_BY_NAME(EXCLUDE_FILE (*z_builderx_review* *z_builderx_new* *z_builderx_old* *runner.o *z_loadscreen.o *z_symverdict* *z_utstall* *z_sdmigrate*) .text.late.*))
    /* builderx UT-only adapters, after every round-14 fixed section. DR excludes this object. */
    *z_builderx_review*(.text .text.* .rodata .rodata.*)
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
    /* sdmigrate (after utstall item 7; both games): the first launch's move of the old SD folder
       (z_sdmigrate.c), then its constants. z_sdmigrate.o is out of the catch-all above. */
    *(.text.late.sdmigrate)
    *(.rodata.sdmigrate_in_text)
    *z_sdmigrate.o(.text .text.* .rodata .rodata.*)
    /* swkbd (UNDERTALE only): Mettaton's essay opens the software keyboard. Last in the block (after sdmigrate,
       merge order), so that nothing linked before it moves; its sections are named outside .text.late.* so that the
       catch-all above leaves them here. z_swkbd.c (a library linked after libctru's original members) with its
       constants and anything else of that library, then the library code it brings in: libctru's keyboard applet and
       the applet launch functions apt.o had not linked. */
    *(.text.swkbd_late)
    *(.rodata.swkbd_late)
    *libswkbd_late.a:*(.text .text.* .rodata .rodata.*)
    *libctru.a:swkbd.o(.text .text.* .rodata .rodata.*)
    *libctru.a:apt.o(.text.aptLaunchLibraryApplet .text.APT_PrepareToStartLibraryApplet .text.APT_StartLibraryApplet
                     .text.aptSetMessageCallback .text.APT_SendParameter)
    /* symverdict on DELTARUNE New (last; empty in other builds): the same
       code as UNDERTALE's above, compiled through z_symverdict_dr.c with sections named symverdict_dr, then its
       constants and ctr_renderer.c's Symverdict_layout. z_symverdict_dr.o is out of the catch-all above. */
    *(.text.late.symverdict_dr)
    *(.rodata.symverdict_dr_in_text)
    *z_symverdict_dr.o(.text .text.* .rodata .rodata.*)
    __late_text_end = ABSOLUTE(.);
  }
  /* 192 KiB (grown in whole 4 KiB pages from 16 KiB, each time late code outgrew it): screens' options page took
     12.3 KB on every target and colflip's UNDERTALE rules 3.5 KB; then noports left UNDERTALE o3DS about 2 KB
     with tpaggrow, texcache (2.3 KB), ring reuse, rdfix2 and utgrid still to come; then typed3, contention,
     texcache F1, alarm14 and utgrid plus layerfix and colflip did not fit in
     96 KiB (UNDERTALE New had 12.5 KB left);
     then layerfix (6.4 KB) and roomload (about 15.2 KB) overflowed
     UNDERTALE's 128 KiB on Old and New (15.7 and 17.0 KB were left), with argtower (8.3 KB) and the engine levers
     still to come (bigger code is fine when it is faster). Growing this moves .rodata, .data and .bss by whole pages only (no text
     address changes): check with check_layout.py --expect-shift and tools/analysis/shiftproof.py.
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
    KEEP(*(.rodata.late.relprep))
    /* RELPREP_ROMFS_rodata */
    *(.rodata.late.loadtime)
    *(.rodata.late.colflip)
    /* colflip: the switch tables of those copies. */
    *collision_compat.o(.rodata .rodata.*)
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
    *(SORT_BY_NAME(EXCLUDE_FILE (*z_builderx_new*) .rodata.late.*))
    *(.rodata.late.builderxnew)
    __late_rodata_end = ABSOLUTE(.);
  }
  /* 8 KiB (at first 4 KiB): late code's constants are mostly log formats; heapfrag's are 472 B, and
     screens' 3.4 KB had to go into late text instead. UNDERTALE n3DS had 1.2 KB left. */
  /* CMake substitutes 0x4000 in its build-local copy for profile/reachgate only, and 0x3000 for UNDERTALE Old 3DS
     (45 B were left). Growing this moves .data and .bss by whole pages
     only: check with check_layout.py --expect-shift and tools/analysis/shiftproof.py. */
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
    *(SORT_BY_NAME(EXCLUDE_FILE (*z_builderx_new* *z_builderx_old*) .data.late.*))
    *(.data.late.builderxnew)
    *(.data.late.builderxold)
    *(.data.late.builderxoldconst)
    /* utoldloop: append copied local caches here, never in ordinary BSS. */
    *aot_utold_*.o(.data .data.* .bss .bss.* COMMON)
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
    *(SORT_BY_NAME(EXCLUDE_FILE (*z_loadscreen.o *z_symverdict* *z_utstall* *z_sdmigrate* *libswkbd_late.a:*) .bss.late.*))
    /* packscreen (integrate round 16, after the catch-all, merge order): the setup line it draws. */
    *(.bss.late.packscreen)
    /* symverdict (after packscreen; UNDERTALE only): the table's bits. */
    *(.bss.late.symverdict)
    *z_symverdict.o(.bss .bss.* COMMON)
    /* utstall item 7 (integrate round 16, after symverdict, merge order): the plan scene's code ids. */
    *(.bss.late.utstall7)
    /* sdmigrate (after utstall item 7; both games): the folder move's path buffers (z_sdmigrate.c). */
    *(.bss.late.sdmigrate)
    *z_sdmigrate.o(.bss .bss.* COMMON)
    /* swkbd (last, after sdmigrate, merge order; UNDERTALE only): the essay keyboard's state, then libctru's
       keyboard applet's. */
    *(.bss.late.swkbd)
    *libswkbd_late.a:*(.bss .bss.* COMMON)
    *libctru.a:swkbd.o(.bss .bss.* COMMON)
    /* symverdict on DELTARUNE New (last, round 17, merge order): the table's bits (z_symverdict_dr.c). */
    *(.bss.late.symverdict_dr)
    *z_symverdict_dr.o(.bss .bss.* COMMON)
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
