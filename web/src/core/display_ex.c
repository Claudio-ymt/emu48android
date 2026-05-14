// Patch display.c
#include "../core/display.c"

DWORD GetKMLColor(UINT index) {
    if (index < 64) return dwKMLColor[index];
    return 0; // Error handling
}
