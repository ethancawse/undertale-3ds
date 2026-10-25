#pragma once

#include <stdbool.h>
#include <stdio.h>

// Main-thread setup notices. A problem is identified by its kind and full SD
// path, independently of the chapter/runner lifetime. No polling or I/O when idle.
extern bool CtrNotice_pending;
void CtrNotice_init(const char* settingsPath);
void CtrNotice_missing(const char* kind, const char* path);
void CtrNotice_report(const char* kind, const char* path, const char* advice);
const char* CtrNotice_first(void);
const char* CtrNotice_advice(void);
// persist=false is only for unattended timeout; it never suppresses a later boot.
bool CtrNotice_dismiss(bool persist);
// The settings writer replaces the file, so it must retain these entries.
void CtrNotice_writeDismissals(FILE* file);
