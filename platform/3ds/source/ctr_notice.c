#include "ctr_notice.h"

#include <stdlib.h>
#include <string.h>

typedef struct Notice {
    struct Notice* next;
    bool dismissed;
    bool saved;
    char key[768];
    char advice[192];
} Notice;

bool CtrNotice_pending;
static Notice* notices;
static char settings[512];

static void updatePending(void) {
    CtrNotice_pending = false;
    for (Notice* n = notices; n != NULL; n = n->next) {
        if (!n->dismissed) CtrNotice_pending = true;
    }
}

static Notice* remember(const char* key, bool dismissed) {
    for (Notice* n = notices; n != NULL; n = n->next) {
        if (strcmp(n->key, key) == 0) return n;
    }
    if (strlen(key) >= sizeof(((Notice*) 0)->key)) return NULL;
    Notice* n = calloc(1, sizeof(*n));
    if (n == NULL) return NULL;
    strcpy(n->key, key);
    n->dismissed = dismissed;
    n->saved = dismissed;
    n->next = notices;
    notices = n;
    updatePending();
    return n;
}

void CtrNotice_init(const char* settingsPath) {
    snprintf(settings, sizeof(settings), "%s", settingsPath);
    FILE* f = fopen(settings, "r");
    if (f == NULL) return;
    char line[800];
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "notice_dismissed = ", 19) == 0) remember(line + 19, true);
    }
    fclose(f);
}

void CtrNotice_missing(const char* kind, const char* path) {
    CtrNotice_report(kind, path, "Copy to the path above from your PC.\nDSP firmware: run DSP1 once.");
}

void CtrNotice_report(const char* kind, const char* path, const char* advice) {
    char key[768];
    int length = snprintf(key, sizeof(key), "%s: %s", kind, path);
    if (length < 0 || (size_t) length >= sizeof(key) || strpbrk(key, "\r\n") != NULL) return;
    Notice* n = remember(key, false);
    if (n != NULL) snprintf(n->advice, sizeof(n->advice), "%s", advice);
}

const char* CtrNotice_first(void) {
    for (Notice* n = notices; n != NULL; n = n->next) {
        if (!n->dismissed) return n->key;
    }
    return NULL;
}

const char* CtrNotice_advice(void) {
    for (Notice* n = notices; n != NULL; n = n->next) {
        if (!n->dismissed) return n->advice;
    }
    return "";
}

bool CtrNotice_dismiss(bool persist) {
    for (Notice* n = notices; n != NULL; n = n->next) {
        if (n->dismissed) continue;
        n->dismissed = true;
        n->saved = persist;
        updatePending();
        if (!persist) return true;
        FILE* f = fopen(settings, "a");
        if (f == NULL) return false;
        bool ok = fprintf(f, "\nnotice_dismissed = %s\n", n->key) >= 0;
        if (fclose(f) != 0) ok = false;
        return ok;
    }
    return true;
}

void CtrNotice_writeDismissals(FILE* file) {
    for (Notice* n = notices; n != NULL; n = n->next) {
        if (n->saved) fprintf(file, "notice_dismissed = %s\n", n->key);
    }
}
