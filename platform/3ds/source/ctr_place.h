// Where new UNDERTALE late code goes, per model (review decision 2026-11-14, audit F3-1).
//
// UNDERTALE Old: code is a new last tenant of the retired AAC text slot, .aacout.text.<unit>. Old's slot and Old's
// late text are both padded to fixed ends, so either costs the same memory, and the slot has the room.
// UNDERTALE New and DELTARUNE: code is an appended late section, .text_<unit>. New's AAC text tail is its tight pool,
// and DELTARUNE has no AAC slot.
// Constants: the AAC rodata slot on both UNDERTALE models, .aacout.rodata.<unit>; an appended late section,
// .rodata_<unit>, in DELTARUNE.
//
// The switch reads only the game and model defines that every object of the ctrport target gets (CMakeLists.txt,
// ctrport's compile definitions). It never reads the release source URL, so Old dev and Old release place a unit
// alike. A 3DS object built outside ctrport (a separate library) has no game or model define and stops here; a host
// build (no 3DS defines at all) gets no section attribute.
//
// A unit names itself once. UT_COLD_TEXT(unit) and UT_COLD_RODATA(unit) give the section attribute only, so add
// noinline and the rest yourself. Register the unit in late-order.json's per_model list, then add both names for
// each kind it uses:
// - the AAC name to aacout.cmake's list and to late-order.json's aac list;
// - the late name to late_reserve.ld and to late-order.json's late list.
// late_order.py checks the set, every use of these macros, and that no source spells a registered unit's section by
// hand.
//
// UT_COLD_TEXT_CONST(unit) puts constants in the unit's text name (registered as text only), for a unit whose
// constants must not be an AAC rodata input on UNDERTALE Old: there N8a's release-only reclaim inputs are the rodata
// slot's last (check_devonly_aac.py), so a later tenant's non-empty rodata input fails the dev-only proof
// (framelead, stepdrift 11-15). GCC refuses a C constant in a code section, so the name's tail ends GCC's own flags in
// an ARM assembler comment (@) and the constants share the code's "ax" section (z_battletouch.c's BT_TEXTRO).
// Exception: old_late_text names approved Old capacity overrides; bstall's N14 text uses its late name on Old
// too. Its constants still use AAC rodata. No other unit inherits this exception.
#ifndef CTR_PLACE_H
#define CTR_PLACE_H

#if defined(PLATFORM_3DS)

#if defined(ENABLE_WAD16) && defined(ENABLE_WAD17)
#error "ctr_place.h: a build has one game (ENABLE_WAD16 or ENABLE_WAD17)"
#endif
#if !defined(ENABLE_WAD16) && !defined(ENABLE_WAD17)
#error "ctr_place.h: a 3DS build names its game (ENABLE_WAD16 or ENABLE_WAD17)"
#endif
#if defined(CTRPORT_TARGET_O3DS) && defined(CTRPORT_TARGET_N3DS)
#error "ctr_place.h: a build has one model (CTRPORT_TARGET_O3DS or CTRPORT_TARGET_N3DS)"
#endif
#if !defined(CTRPORT_TARGET_O3DS) && !defined(CTRPORT_TARGET_N3DS)
#error "ctr_place.h: a 3DS build names its model (CTRPORT_TARGET_O3DS or CTRPORT_TARGET_N3DS)"
#endif

// Every .aacout name sits under a plain ENABLE_WAD16 guard (test_aacslot_shared.py): DELTARUNE has no AAC slot.
#if defined(ENABLE_WAD16)
#if defined(CTRPORT_TARGET_O3DS)
// Named Old text exceptions: the N14 capacity ruling moves bstall only to ordinary late text.
// Keep the default for every other unit, including names not yet registered. late-order.json records the same
// allowlist; late_floor_gate checks the actual linked section names, including suffixes and macro spellings.
#define CTR_PLACE_OLD_OVERRIDE_bstall ignored, 1
#define CTR_PLACE_SECOND(a, b, ...) b
#define CTR_PLACE_OLD_OVERRIDE(...) CTR_PLACE_SECOND(__VA_ARGS__, 0)
#define CTR_PLACE_OLD_TEXT_0(unit) __attribute__((section(".aacout.text." #unit)))
#define CTR_PLACE_OLD_TEXT_1(unit) __attribute__((section(".text_" #unit)))
#define CTR_PLACE_OLD_TEXT_PICK_I(n) CTR_PLACE_OLD_TEXT_##n
#define CTR_PLACE_OLD_TEXT_PICK(n) CTR_PLACE_OLD_TEXT_PICK_I(n)
#define UT_COLD_TEXT(unit) CTR_PLACE_OLD_TEXT_PICK(CTR_PLACE_OLD_OVERRIDE(CTR_PLACE_OLD_OVERRIDE_##unit))(unit)
#define UT_COLD_TEXT_CONST(unit) __attribute__((section(".aacout.text." #unit ",\"ax\",%progbits @")))
#else
#define UT_COLD_TEXT(unit) __attribute__((section(".text_" #unit)))
#define UT_COLD_TEXT_CONST(unit) __attribute__((section(".text_" #unit ",\"ax\",%progbits @")))
#endif
#define UT_COLD_RODATA(unit) __attribute__((section(".aacout.rodata." #unit)))
#else
#define UT_COLD_TEXT(unit) __attribute__((section(".text_" #unit)))
#define UT_COLD_TEXT_CONST(unit) __attribute__((section(".text_" #unit ",\"ax\",%progbits @")))
#define UT_COLD_RODATA(unit) __attribute__((section(".rodata_" #unit)))
#endif

#elif defined(__3DS__)
#error "ctr_place.h: this 3DS object is built outside ctrport, so it has no game or model define; place the unit in ctrport"
#else
#define UT_COLD_TEXT(unit)
#define UT_COLD_TEXT_CONST(unit)
#define UT_COLD_RODATA(unit)
#endif

#endif
