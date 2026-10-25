#include "ctr_language.h"

#include <3ds.h>

const char* CtrLanguage_systemCode(void) {
    // Indexed by CFG_Language. GameMaker's os_get_language gives "zh" for both Chinese scripts on PC.
    static const char* const codes[] = { "ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh" };
    u8 language = CFG_LANGUAGE_EN;
    if (R_SUCCEEDED(cfguInit())) {
        if (R_FAILED(CFGU_GetSystemLanguage(&language))) language = CFG_LANGUAGE_EN;
        cfguExit();
    }
    if (language >= sizeof(codes) / sizeof(codes[0])) return "en";
    return codes[language];
}
