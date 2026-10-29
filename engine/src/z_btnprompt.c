// The 3DS buttons in UNDERTALE's prompts (btnprompt).
//
// UNDERTALE 1.08 runs here as its PC version (os_type Windows, global.osflavor 1), so its prompts name keyboard keys:
// the text's button escapes become bracketed key letters, the title names Z and ENTER, the instruction screen lists
// keyboard-only lines, Papyrus's date asks for a key on the keyboard, the piano rooms have a quit scribble, and the
// Tough Glove shows a key. The console versions of those prompts are in the game too, behind global.osflavor tests;
// reporting a console os_type would also change saves, quitting and content, so instead:
// - those tests take the console branch: Btnprompt_registerAll changes the constant each compares osflavor with;
// - the bracketed key letters and the title's prompt are overwritten in place, same length, with our own text: the
//   3DS button that sends each key (Btnprompt_setLabels, from the platform's button map:
//   platform/3ds/source/z_btnprompt_ctr.c);
// - scr_getbuttonsprite, which only the Tough Glove's console branch reaches, is a builtin that returns a sprite of
//   our own button glyph (the platform's).
// The game's own code then lays the prompts out as the console versions do.
//
// This file holds no game data. Where the sites are, and what their bytes are, comes from the generated table
// btnpromptTable (z_btnprompt.h), which tools/aot/btnprompt.py makes at build time from the player's own data.win by
// rules that name entries and state bytecode shapes. Any entry or string whose bytes differ from what the table says
// is left alone, with its PC prompt; the private build step supplies the table for interpreted builds too.
//
// Everything here is late: code in .text.late.btnprompt, constants in .rodata.btnprompt_in_text (listed in the late
// code, as screens' are; the generated table goes there too), statics in .bss.late.btnprompt
// (platform/3ds/late_reserve.ld). VM_create calls Btnprompt_registerAll in place of Colflip14_registerAll. A build
// without WAD 16 (DELTARUNE's) or for another platform has none of it: only those two calls, each doing nothing more.

#include "data_win.h"
#include "log.h"
#include "rvalue.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"
#include "vm_builtins.h"
#include "z_btnprompt.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__3DS__) // the late sections exist only in the 3DS link (late_reserve.ld)
#define BTN_LATE __attribute__((noinline, section(".text.late.btnprompt")))
#define BTN_CONST __attribute__((section(".rodata.btnprompt_in_text")))
#define BTN_BSS __attribute__((section(".bss.late.btnprompt")))
#else
#define BTN_LATE
#define BTN_CONST
#define BTN_BSS
#endif

#if defined(ENABLE_WAD16) && defined(__3DS__)
// platform/3ds/source/z_btnprompt_ctr.c: whether the glyphs could be read (and forgets the last VM's sprites), and the
// sprite of the button that sends the key of control ('Z', 'X' or 'C'), or -4.
bool CtrBtnprompt_begin(void);
int32_t CtrBtnprompt_buttonSprite(Renderer* renderer, char control);

// The generated table (aot_table.c, or the private fallback for interpreted builds).
extern const BtnpromptTable btnpromptTable __attribute__((weak));

#define BTNPROMPT_PUSH_STRING 0xC0060000u

static const char btnpromptScriptPrefix[] BTN_CONST = "gml_Script_";
static const char btnpromptLogOn[] BTN_CONST = "Buttons: 3DS prompts, %u of %u branches and %u of %u strings (glyphs %s)\n";
static const char btnpromptLogSkip[] BTN_CONST = "Buttons: code entry %u is not the build's, its PC prompt stays\n";
static const char btnpromptLogNone[] BTN_CONST = "Buttons: no prompt table, PC prompts\n";
static const char btnpromptYes[] BTN_CONST = "on";
static const char btnpromptNo[] BTN_CONST = "off";
// btnpromptRetained (184 B that kept the late code after the prompts in place) went in argcheck's UT late-text
// reclaim (a ruled layout event: the late code after it moved down, tools/analysis/check_shrink_layout.py).


// Private STRG indices and FNV guards, generated from the player's own data.
static const uint32_t btnpromptStringGuards[BTNPROMPT_SLOTS][2]
    __attribute__((section(".rodata.late.exportclean"))) = {
#include "btnprompt_guards.inc"
};

#line 70
// The strings found at VM_create (nullptr: not found) and their lengths.
static char* btnpromptSlot[BTNPROMPT_SLOTS] BTN_BSS;
static uint32_t btnpromptSlotLength[BTNPROMPT_SLOTS] BTN_BSS;
// What Btnprompt_setLabels last wrote into each, so that a second VM on the same strings finds them again.
static char* btnpromptWritten[BTNPROMPT_SLOTS] BTN_BSS;

static BTN_LATE CodeEntry* btnpromptEntry(VMContext* ctx, uint32_t codeIndex, uint32_t length) {
    if (codeIndex >= ctx->dataWin->code.count) return nullptr;
    CodeEntry* code = &ctx->dataWin->code.entries[codeIndex];
    return code->present && code->length == length && code->offset == 0 ? code : nullptr;
}

static BTN_LATE uint32_t btnpromptRead(const uint8_t* p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static BTN_LATE uint32_t btnpromptDigest(const uint8_t* bytes, uint32_t length, const BtnpromptCode* rows, uint32_t count) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; length > i; i++) {
        uint8_t b = bytes[i];
        for (uint32_t r = 0; count > r; r++) {
            uint32_t at = i - rows[r].offset;
            if (4 > at) b = (uint8_t) (rows[r].word >> (8 * at));
        }
        h = (h ^ b) * 0x01000193u;
    }
    return h;
}

// Takes the console branch of rows[0..count-1] (one entry) when the entry's bytes are the table's; returns how many.
static BTN_LATE uint32_t btnpromptPatch(VMContext* ctx, const BtnpromptCode* rows, uint32_t count) {
    CodeEntry* code = btnpromptEntry(ctx, rows->code, rows->length);
    uint8_t* bytes = code != nullptr ? (uint8_t*) (uintptr_t) DataWin_codeBytes(ctx->dataWin, code) : nullptr;
    bool known = bytes != nullptr;
    for (uint32_t r = 0; known && count > r; r++) {
        uint32_t word = rows[r].offset + 4 <= rows->length ? btnpromptRead(bytes + rows[r].offset) : 0;
        known = word == rows[r].word || word == rows[r].patched;
    }
    if (!known || btnpromptDigest(bytes, rows->length, rows, count) != rows->digest) {
        logInfo(btnpromptLogSkip, (unsigned) rows->code);
        return 0;
    }
    for (uint32_t r = 0; count > r; r++) {
        uint32_t patched = rows[r].patched;
        memcpy(bytes + rows[r].offset, &patched, 4);
    }
    return count;
}

static bool btnpromptFindStringPrivate(VMContext* ctx, const BtnpromptString* row);
static BTN_LATE bool btnpromptFindString(VMContext* ctx, const BtnpromptString* row) {
    // Retain the earlier 260 B slot and GCC's original call-graph emission order.
    __asm__ volatile("b 1f\n\t.space 252\n1:");
    return btnpromptFindStringPrivate(ctx, row);
}

static __attribute__((noipa, section(".text.late.exportclean")))
bool btnpromptFindStringPrivate(VMContext* ctx, const BtnpromptString* row) {
    if (row->slot >= BTNPROMPT_SLOTS) return false;
    CodeEntry* code = btnpromptEntry(ctx, row->code, row->length);
    const uint8_t* bytes = code != nullptr ? DataWin_codeBytes(ctx->dataWin, code) : nullptr;
    if (bytes == nullptr || row->slot >= BTNPROMPT_SLOTS || row->offset + 8 > row->length ||
        btnpromptRead(bytes + row->offset) != BTNPROMPT_PUSH_STRING) return false;
    uint32_t index = btnpromptRead(bytes + row->offset + 4);
    Strg* strg = &ctx->dataWin->strg;
    if (index != btnpromptStringGuards[row->slot][0] || index >= strg->count || strg->strings[index] == nullptr) return false;
    char* s = (char*) (uintptr_t) strg->strings[index];
    uint32_t length = (uint32_t) strlen(row->text);
    if (strlen(s) != length || (s != btnpromptWritten[row->slot] &&
        btnpromptDigest((const uint8_t*) s, length, nullptr, 0) != btnpromptStringGuards[row->slot][1])) return false;
    btnpromptSlot[row->slot] = s;
    btnpromptSlotLength[row->slot] = length;
    return true;
}
#line 133

static BTN_LATE RValue btnpromptGetButtonSprite(VMContext* ctx, RValue* args, int32_t argCount) {
    int32_t sprite = -4;
    if (argCount >= 1 && args[0].type == RVALUE_STRING && args[0].string != nullptr && ctx->runner != nullptr)
        sprite = CtrBtnprompt_buttonSprite(ctx->runner->renderer, args[0].string[0]);
    return RValue_makeReal((GMLReal) sprite);
}

static BTN_LATE void btnpromptLoad(VMContext* ctx) {
    DataWin* dw = ctx->dataWin;
    const BtnpromptTable* t = &btnpromptTable;
    memset(btnpromptSlot, 0, sizeof(btnpromptSlot));
    memset(btnpromptSlotLength, 0, sizeof(btnpromptSlotLength));
    if (dw == nullptr || dw->mappedFile != nullptr || dw->gen8.wadVersion != 16) return;
    if (t == nullptr) {
        logInfo(btnpromptLogNone);
        return;
    }
    bool glyphs = CtrBtnprompt_begin();
    uint32_t branches = 0, strings = 0, glyphBranches = 0;
    for (uint32_t first = 0; t->codeCount > first;) {
        const BtnpromptCode* row = &t->code[first];
        uint32_t end = first + 1;
        while (t->codeCount > end && t->code[end].code == row->code) end++;
        if (glyphs || (row->flags & BTNPROMPT_NEEDS_GLYPHS) == 0) {
            uint32_t taken = btnpromptPatch(ctx, row, end - first);
            branches += taken;
            if ((row->flags & BTNPROMPT_NEEDS_GLYPHS) != 0) glyphBranches += taken;
        }
        first = end;
    }
    for (uint32_t i = 0; t->stringCount > i; i++) {
        if (btnpromptFindString(ctx, &t->strings[i])) strings++;
    }
    // The sprite builtin only with the branch that calls it: anywhere else scr_getbuttonsprite stays the game's. Its
    // name is the script's, without the entry prefix.
    if (glyphBranches != 0) {
        size_t prefix = sizeof(btnpromptScriptPrefix) - 1;
        CodeEntry* code = t->spriteLength != 0 ? btnpromptEntry(ctx, t->spriteCode, t->spriteLength) : nullptr;
        const uint8_t* bytes = code != nullptr && code->name != nullptr && strncmp(code->name, btnpromptScriptPrefix, prefix) == 0
            ? DataWin_codeBytes(dw, code) : nullptr;
        if (bytes != nullptr && btnpromptDigest(bytes, t->spriteLength, nullptr, 0) == t->spriteDigest &&
            VM_findBuiltin(ctx, code->name + prefix) == nullptr) {
            VM_registerBuiltin(ctx, code->name + prefix, btnpromptGetButtonSprite);
        } else {
            logInfo(btnpromptLogSkip, (unsigned) t->spriteCode);
        }
    }
    logInfo(btnpromptLogOn, (unsigned) branches, (unsigned) t->codeCount, (unsigned) strings, (unsigned) t->stringCount,
            glyphs ? btnpromptYes : btnpromptNo);
}

// vm_builtins.c's ds_map_add (FreewinsMap_key and FreewinsMap_value): a slot's string is rewritten in place, so a map
// copies it rather than borrowing it. After freewins' other late code (late_reserve.ld).
__attribute__((noinline, section(".text.freewins_r3_late"))) bool Btnprompt_isSlot(const char* s) {
    for (uint32_t slot = 0; BTNPROMPT_SLOTS > slot; slot++) {
        if (btnpromptSlot[slot] == s) return true;
    }
    return false;
}
#endif // ENABLE_WAD16 && __3DS__

// Called by VM_create through COLFLIP_REGISTER_ALL (UNDERTALE builds), before the function cache is built from the
// builtins.
BTN_LATE void Btnprompt_registerAll(VMContext* ctx) {
#if defined(ENABLE_WAD16)
    void Colline14_registerAll(VMContext*);
    Colline14_registerAll(ctx);  // Colflip14_registerAll, then colline's builtins (colline_gm14.c)
#else
    Colflip14_registerAll(ctx);
#endif
#if defined(__3DS__) && defined(ENABLE_WAD16)
    btnpromptLoad(ctx);
#endif
}

// letters: the 3DS button for Z, X and C ('A', 'B', 'X', 'Y', 'L' or 'R'; 0 keeps the game's own label). title: the
// title's prompt, at most as long as the game's (padded with spaces on both sides; nullptr keeps the game's).
BTN_LATE void Btnprompt_setLabels(const char letters[3], const char* title) {
#if !defined(ENABLE_WAD16) || !defined(__3DS__)
    (void) letters;
    (void) title;
#else
    for (uint32_t slot = 0; BTNPROMPT_SLOTS > slot; slot++) {
        char* s = btnpromptSlot[slot];
        uint32_t length = btnpromptSlotLength[slot];
        if (s == nullptr) continue;
        // The game's own text of the slot, from the table (a slot was only found through its row).
        const char* original = nullptr;
        for (uint32_t i = 0; btnpromptTable.stringCount > i; i++) {
            if (btnpromptTable.strings[i].slot == slot) original = btnpromptTable.strings[i].text;
        }
        if (original == nullptr) continue;
        if (BTNPROMPT_SLOT_TITLE > slot) {
            // Our own label, "[" then the button's letter then "]", or the game's own back.
            if (length != 3) continue;
            bool ours = letters != nullptr && letters[slot] != 0;
            s[0] = ours ? '[' : original[0];
            s[1] = ours ? letters[slot] : original[1];
            s[2] = ours ? ']' : original[2];
        } else {
            const char* text = title != nullptr && strlen(title) <= length ? title : original;
            uint32_t n = (uint32_t) strlen(text);
            uint32_t lead = (length - n) / 2;  // the title is drawn from its left edge: split the spaces to stay centred
            for (uint32_t i = 0; length > i; i++) s[i] = lead <= i && lead + n > i ? text[i - lead] : ' ';
        }
        btnpromptWritten[slot] = s;
    }
#endif
}
