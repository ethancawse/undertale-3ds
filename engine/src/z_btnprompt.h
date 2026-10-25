// UNDERTALE's console prompts: the patch table's types (z_btnprompt.c). tools/aot/btnprompt.py makes the table at build
// time from the player's own data.win and writes it into the generated aot_table.c, in the build directory; the
// repository holds only the rules it follows. The table has a fixed size whatever the data.win, so the late code after
// it keeps its place.
#pragma once

#include <stdint.h>

// A Push.i of the constant a branch compares global.osflavor with, and the constant that makes the test take the
// console branch. code: the entry's index in the data.win's CODE chunk. digest: FNV-1a (32-bit) of the whole entry as
// VM_create leaves it (variable and function operands patched), with each of its rows' words as the build's data.win
// has them. Rows of one entry are adjacent.
typedef struct {
    uint32_t code;
    uint32_t length;
    uint32_t digest;
    uint32_t offset;
    uint32_t word;
    uint32_t patched;
    uint32_t flags;
} BtnpromptCode;

// The entry's console branch draws a button sprite (scr_getbuttonsprite), so it is only taken with the glyphs.
#define BTNPROMPT_NEEDS_GLYPHS 1u
#define BTNPROMPT_CODE_MAX 8u
#define BTNPROMPT_TEXT_MAX 19u

enum { BTNPROMPT_SLOT_Z, BTNPROMPT_SLOT_X, BTNPROMPT_SLOT_C, BTNPROMPT_SLOT_TITLE, BTNPROMPT_SLOT_TITLE_JA, BTNPROMPT_SLOTS };

// A Push.s of a string the labels go into, found through the code that pushes it, and the string as the build's
// data.win has it (to check it, and to put it back).
typedef struct {
    uint32_t code;
    uint32_t length;
    uint32_t offset;
    uint32_t slot;
    char text[BTNPROMPT_TEXT_MAX + 1];
} BtnpromptString;

typedef struct {
    uint32_t codeCount;
    uint32_t stringCount;
    BtnpromptCode code[BTNPROMPT_CODE_MAX];
    BtnpromptString strings[BTNPROMPT_SLOTS];
    // The script the sprite builtin replaces (its CODE index, length and digest; length 0 when absent).
    uint32_t spriteCode;
    uint32_t spriteLength;
    uint32_t spriteDigest;
} BtnpromptTable;

// The generated table, with the prompts' other late constants (platform/3ds/late_reserve.ld).
#if defined(__3DS__)
#define BTNPROMPT_TABLE_SECTION __attribute__((section(".rodata.btnprompt_in_text")))
#else
#define BTNPROMPT_TABLE_SECTION
#endif
