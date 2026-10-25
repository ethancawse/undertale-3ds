#pragma once

// Per-chapter AOT modules.
//
// A build with aot=chapters keeps no generated code of its own. Each DELTARUNE chapter's generated functions are
// linked on their own, at the start of the heap (CTR_AOT_MODULE_BASE), against the main program's symbols, and go into
// the romfs as aot/<chapter folder>.aotm. When a chapter starts, CtrAotModule_load reads its module into that
// region, which __system_allocateHeaps keeps out of the heap, makes it executable and hands its table to the VM. The
// Old 3DS's 4 MB of code then serves one chapter at a time instead of being shared by five.

#include <stdbool.h>
#include <stdint.h>

#define CTR_AOT_MODULE_BASE 0x08000000u
#define CTR_AOT_MODULE_MAGIC 0x4D544F41u   // "AOTM"
#define CTR_AOT_MODULE_VERSION 1u

struct AotTable;

// The start of every module, at CTR_AOT_MODULE_BASE.
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;            // sizeof(CtrAotModuleHeader)
    uint32_t base;                  // the address the module was linked for
    uint32_t imageEnd;              // the end of what the file holds
    uint32_t codeEnd;               // [base, codeEnd): code and read-only data, made read-only and executable
    uint32_t bssStart, bssEnd;      // zeroed after loading
    const struct AotTable* table;
    // The address of every symbol of the main program the module uses, as the module was linked: the main program
    // keeps the same list (ctrAotHostSymbols), so a module linked against another build of it is refused.
    const uintptr_t* hostSymbols;
    uint32_t hostSymbolCount;
} CtrAotModuleHeader;

#ifdef CTRPORT_AOT_MODULES

// The region the modules load into, reserved by __system_allocateHeaps (ctr_memory.c).
#define CTR_AOT_MODULE_REGION_SIZE ((uint32_t) CTRPORT_AOT_REGION_MB * 1024u * 1024u)

// Whether __system_allocateHeaps reserved the region.
bool CtrAotModule_regionReserved(void);

// Loads the module for the data.win at dataWinPath (by its folder's name) and hands its table to the VM, or, when
// there is none or it can't be used, tells the VM there is no generated code. Before the VMContext is created; the
// previous one must be freed by then.
void CtrAotModule_load(const char* dataWinPath);

#endif
