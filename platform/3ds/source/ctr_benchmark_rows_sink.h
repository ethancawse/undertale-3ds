// Result helpers in other translation units must dispatch RAM-spool handles here.
// These handles are not newlib FILE objects. Ordinary FILE pointers also work.
#pragma once
#include <stdio.h>

int CtrBench_resultPuts(const char* text, FILE* stream);
