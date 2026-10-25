// Host-only UTF-8 paths. Windows CRT argv and stdio otherwise use the system ANSI code page.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <wchar.h>
#include <direct.h>
static inline wchar_t* texprep_wide(const char* text) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!size) { errno = EINVAL; return NULL; }
    wchar_t* wide = malloc((size_t) size * sizeof(wchar_t));
    if (!wide) { errno = ENOMEM; return NULL; }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, size)) {
        free(wide); errno = EINVAL; return NULL;
    }
    return wide;
}
static inline FILE* texprep_fopen(const char* path, const char* mode) {
    wchar_t* p = texprep_wide(path);
    wchar_t* m = texprep_wide(mode);
    FILE* file = p && m ? _wfopen(p, m) : NULL;
    free(p); free(m); return file;
}
static inline int texprep_remove(const char* path) {
    wchar_t* p = texprep_wide(path);
    int result = p ? _wremove(p) : -1;
    free(p); return result;
}
static inline int texprep_rename(const char* from, const char* to) {
    wchar_t* f = texprep_wide(from);
    wchar_t* t = texprep_wide(to);
    int result = f && t ? _wrename(f, t) : -1;
    free(f); free(t); return result;
}
static inline int texprep_wmkdir(const char* path) {
    wchar_t* p = texprep_wide(path);
    int result = p ? _wmkdir(p) : -1;
    free(p); return result;
}
#define fopen texprep_fopen
#define remove texprep_remove
#define rename texprep_rename
#define _mkdir texprep_wmkdir
#endif
