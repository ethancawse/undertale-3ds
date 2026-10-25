#pragma once

// Share the frozen menu's existing text without duplicating its literal pool.
typedef struct {
    const char* const* labels;
    const char* const* help;
    const char* const* stereoHelp;
    const char* const* smoothingHelp;
} CtrMenuText;

extern const CtrMenuText ctrMenuText;
