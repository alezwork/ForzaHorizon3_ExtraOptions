#ifndef FH2_FH3_LIGHTS_H
#define FH2_FH3_LIGHTS_H
#include "extra_options.h"

/* ForzaCar vtable slot 16 (per-car update), hooked to toggle the local car's
   headlights from a button of the game's own controller state. */
#define FH3_CAR_UPDATE_SLOT_RVA 0x42e06d0u
#define FH3_CAR_UPDATE_RVA 0x344780u

typedef struct {
    unsigned button_mask;     /* XInput-layout mask in the game's controller, 0x40 = left stick */
} Fh3LightsConfig;

void fh3_lights_configure(unsigned char *game_image, const Fh3LightsConfig *config);
/* Address written into the vtable slot. */
uintptr_t fh3_lights_callback(void);
#endif
