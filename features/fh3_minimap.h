#ifndef FH2_FH3_MINIMAP_H
#define FH2_FH3_MINIMAP_H
#include "extra_options.h"

/* MapViewControllerTracking (vtable slot 27, 0x120b170) calls its minimap
   update 0x120c6c0 at 0x120b3d2, routed through a jmp [rip] thunk in the int3
   padding at 0x2603260 (FH2: call 0xbab0f6 -> 0xbabb90, thunk 0xbab888). */
#define FH3_MINIMAP_CALL_RVA 0x120b3d2u
#define FH3_MINIMAP_UPDATE_RVA 0x120c6c0u
#define FH3_MINIMAP_ROTATES_RVA 0x120efd0u
#define FH3_MINIMAP_THUNK_RVA 0x2603260u

void fh3_minimap_configure(unsigned char *game_image);
/* Address written into the thunk. */
uintptr_t fh3_minimap_callback(void);
#endif
