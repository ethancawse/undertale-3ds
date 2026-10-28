// Known Windows Steam files used by the builds, measured 2026-10-16, and (revision 2) UNDERTALE's macOS Steam file,
// measured 2026-10-21: the builder copies its game.ios to the card as data.win.
// names only: sizes and SHA-256 digests; no game content. Increment revision on edits.
// The 3DS keeps rows added after revision 1 in a table of their own (DATA_VERSIONS_LATE_ONLY, ctr_data_version.c's
// end), so the revision 1 table keeps its size and place.
#undef DATA_VERSIONS_REVISION
#define DATA_VERSIONS_REVISION 3
// The original runtime tables retain revision 2 on non-GOG builds, including their cache literals.
// The Python table reader sees revision 3; only a GOG compatibility build changes the runtime revision.
#if (defined(DATA_VERSIONS_BASE_ONLY) || defined(DATA_VERSIONS_LATE_ONLY)) && !defined(CTRPORT_GOG_COMPAT)
#undef DATA_VERSIONS_REVISION
#define DATA_VERSIONS_REVISION 2
#endif
#if !defined(DATA_VERSIONS_LATE_ONLY) && !defined(DATA_VERSIONS_GOG_ONLY)
DATA_VERSION("undertale", "root", 62921978ULL, "b718f8223a5bb31979ffeed10be6140c857b882fc0d0462b89d6287ae38c81c7")
DATA_VERSION("deltarune", "root", 2953218ULL, "82a7041a530407c5f17e439a97bdf53e4a24e30775a7ecbc2879369b9d2441d6")
DATA_VERSION("deltarune", "chapter1_windows", 13072876ULL, "82c2bb61b8d78cd287120f6301588fecba34ec5a890bac711b7a8774c760ec70")
DATA_VERSION("deltarune", "chapter2_windows", 66949110ULL, "047c5ab003e3e017a709c02757e119c81e0327760169512110fd276b19241e68")
DATA_VERSION("deltarune", "chapter3_windows", 126284402ULL, "c1a0925343694ec9b9adcbf2f916a720b02fd1b999286cfe8fe6a52f3320f714")
DATA_VERSION("deltarune", "chapter4_windows", 132041918ULL, "ed64789586238b52375e994e1c1cf13694dd2d0dab57d13e639b9c892e37d8f2")
DATA_VERSION("deltarune", "chapter5_windows", 165423612ULL, "370dfd141d2955d5a1960122919b16e4092b52ffbb85fda541bc4680c6b3b85c")
#endif
#if !defined(DATA_VERSIONS_BASE_ONLY) && !defined(DATA_VERSIONS_GOG_ONLY)
DATA_VERSION("undertale", "root", 62956178ULL, "91a8acf1f40bd7c45d0bdb978de55187e7fb2cd76a1e2cda41c078dd44d3565e")
#endif
// Revision 3, measured 2026-10-22: GOG / Humble / Windows 10 store / Collector's Edition / Game Pass PC data.
// Only GOG store evidence is accepted by the builder for this digest. Its row has its own late slot.
#if !defined(DATA_VERSIONS_BASE_ONLY) && !defined(DATA_VERSIONS_LATE_ONLY)
DATA_VERSION("undertale", "root", 62951906ULL, "946c738c6d6182f683431320a0e32470bc3deb50f3e8887e61998df0c4e3abc8")
#endif
