// The console's system language, as GameMaker's os_get_language reports it.
#ifndef CTRPORT_CTR_LANGUAGE_H
#define CTRPORT_CTR_LANGUAGE_H

// Two-letter ISO 639-1 code of the language set in System Settings ("en", "ja", ...), the form
// os_get_language returns on PC. "en" if the CFG service can't be read. Both games only test for "ja".
const char* CtrLanguage_systemCode(void);

#endif
