#ifndef CTRPORT_CTR_FPSBOX_H
#define CTRPORT_CTR_FPSBOX_H

#include <stdbool.h>
#include <stdint.h>

// Main-thread sampling, before the framebuffer painter reads the two digits.
void CtrFpsbox_update(char* digits);
// Restore the menu under the counter before its next transparent paint.
void CtrFpsbox_restoreMenu(uint16_t* shown, const uint16_t* page, bool top);

#endif
