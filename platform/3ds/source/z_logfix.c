// logfix (change scanout, logaudit G8, 11-08; both games): audio log records arrive whole, each on its own line.
//
// The audio system's decode and loader threads queue their log lines for the game thread (ctr_audio_system.c's
// postMessage), each formatted into a 192-byte slot. A longer record was cut at 191 characters and lost its newline,
// and the log's stamp then went on with no line end, so the next record was glued after it. UNDERTALE's sound-effect
// summary ("[adpcm-mono] installed ...", every 5 s with the [perf] line) is about 195-215 characters: every one in
// maintainer's logs of 11-08 was cut (1670 in one, 1155 in the other), ending in the middle of its "cores" or "ms max"
// fields, and an audit read the HOME presses glued after two of them as missing.
//
// Two parts:
//   - CtrLogfix_vsnprintf: ctr_audio_system.c's own formatting calls come here by name (CMake's compile definitions):
//     the same vsnprintf, and when the text did not fit, its end becomes " [cut]" and the newline its format ended
//     with. So no record from that file can lose its line end, and a cut one says so. Nothing changes for a text that
//     fits.
//   - CtrLogfix_post (UNDERTALE's sound-effect pack): the pack's posts (CtrAdpcm_musicLegacy.post, the table in
//     ctr_adpcm_pack.inc) come here instead of postMessage. A record that fits a slot is posted as it is; a longer one
//     is posted as several lines, split after a "; " where it can be, each later line starting with the record's
//     tag and "(cont.)", so every field arrives and every line ends. Its lines go into the queue together, in one
//     hold of its lock (CtrLogfix_postLines, appended to ctr_audio_system.c), or, with too few slots free, none of
//     them: no other line comes between them and no record arrives in part. The queue's overflow stays as it was
//     (each line not queued counts as a dropped message); nothing waits for room.
//
// Off the frame path: the posts come at most once per stats window and at a sound pack's installs. Its code and
// constants are appended last in late text (.text_logfix, .rodata.logfix_in_text); no data, no new statics.
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__3DS__) && defined(__GNUC__)
#define LF_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_logfix")))
#define LF_RO __attribute__((section(".rodata.logfix_in_text")))
#else
#define LF_CODE
#define LF_RO
#endif

static const char lfCutLine[] LF_RO = " [cut]\n";
static const char lfCut[] LF_RO = " [cut]";

int CtrLogfix_vsnprintf(char* buffer, size_t size, const char* format, va_list args);
LF_CODE int CtrLogfix_vsnprintf(char* buffer, size_t size, const char* format, va_list args) {
    int wanted = vsnprintf(buffer, size, format, args);
    if (wanted < 0 || size > (size_t) wanted || sizeof(lfCutLine) > size) return wanted;
    size_t length = strlen(format);
    bool line = length > 0 && format[length - 1] == '\n';
    const char* mark = line ? lfCutLine : lfCut;
    size_t bytes = line ? sizeof(lfCutLine) : sizeof(lfCut);
    // Opaque to the compiler, so the copy reads the late constant and no copy of its text goes to the main .rodata.
    __asm__("" : "+r"(mark));
    memcpy(buffer + size - bytes, mark, bytes);
    return wanted;
}

#ifdef CTRPORT_ADPCM_SFXPACK
// ctr_audio_system.c's postMessage (static there): this name is a one-branch way in appended at that file's end.
void CtrLogfix_postMessage(void* ca, const char* format, ...);
// And its queue's way in for a record of several lines, appended there too: all of them or none, in one hold of its lock.
bool CtrLogfix_postLines(void* ca, uint32_t count, void (*fill)(void* ctx, uint32_t i, char* slot, uint32_t size), void* ctx);

// A slot holds 191 characters and the NUL (ctr_audio_system.c's MESSAGE_LENGTH).
#define LF_SLOT_TEXT 191u
#define LF_TEXT 384u
#define LF_TAG_MAX 24u
#define LF_CONT 9u
// A cut after a "; " is taken only past half the piece's room, so a piece is at least 78 characters (the room less a
// 24-character tag and " (cont.) ", halved) and the 383 of a record are at most 5 pieces.
#define LF_PIECES 5u

static const char lfWhole[] LF_RO = "%s";
static const char lfCont[] LF_RO = " (cont.) ";

typedef struct {
    const char* text;
    uint32_t tag;
    uint16_t at[LF_PIECES], take[LF_PIECES];
} LfPieces;

// CtrLogfix_postLines' fill, with the queue's lock held: piece i into its slot. The later pieces start with the
// record's tag and " (cont.) "; each ends with a newline.
static LF_CODE void lfFill(void* ctx, uint32_t i, char* slot, uint32_t size) {
    const LfPieces* p = (const LfPieces*) ctx;
    const char* cont = lfCont;
    // Opaque to the compiler, as in CtrLogfix_vsnprintf.
    __asm__("" : "+r"(cont));
    uint32_t n = 0, take = p->take[i];
    if (i > 0) {
        memcpy(slot, p->text, p->tag);
        memcpy(slot + p->tag, cont, LF_CONT);
        n = p->tag + LF_CONT;
    }
    if (n + take + 2u > size) take = size - n - 2u;
    memcpy(slot + n, p->text + p->at[i], take);
    slot[n + take] = '\n';
    slot[n + take + 1u] = '\0';
}

void CtrLogfix_post(void* ca, const char* format, ...);
LF_CODE void CtrLogfix_post(void* ca, const char* format, ...) {
    char text[LF_TEXT];
    va_list args;
    va_start(args, format);
    int wanted = CtrLogfix_vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (wanted < 0) return;
    size_t length = strlen(text);
    if (LF_SLOT_TEXT >= length) {
        CtrLogfix_postMessage(ca, lfWhole, text);
        return;
    }
    LfPieces pieces;
    pieces.text = text;
    // The tag: "[adpcm-mono]" and the like, up to its "]".
    pieces.tag = 0;
    if (text[0] == '[') {
        const char* end = memchr(text, ']', LF_TAG_MAX);
        if (end != NULL) pieces.tag = (uint32_t) (end - text) + 1u;
    }
    if (text[length - 1] == '\n') length--;
    size_t at = 0;
    uint32_t count = 0;
    while (length > at && LF_PIECES > count) {
        // Room for the piece: the slot less its newline, and for a later piece the tag and " (cont.) ".
        size_t room = count == 0 ? LF_SLOT_TEXT - 1u : LF_SLOT_TEXT - 1u - pieces.tag - LF_CONT;
        size_t take = length - at;
        if (take > room) {
            // After the last "; " that fits past half the room, keeping the ';'; else just where the room ends.
            size_t cut = 0;
            for (size_t i = room; i > room / 2u && cut == 0; i--) {
                if (text[at + i - 1] == ';' && text[at + i] == ' ') cut = i;
            }
            take = cut != 0 ? cut : room;
        }
        pieces.at[count] = (uint16_t) at;
        pieces.take[count] = (uint16_t) take;
        count++;
        at += take;
        while (length > at && text[at] == ' ') at++;
    }
    // One admission for the whole record: no other line between its pieces, and none of it queued without the rest.
    CtrLogfix_postLines(ca, count, lfFill, &pieces);
}
#endif
