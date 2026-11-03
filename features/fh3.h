#ifndef FH2_FH3_H
#define FH2_FH3_H
#include "extra_options.h"

#define FH3_TITLE_ID 0x4ce1dfabu

/* Forza Horizon 3 (Xbox One 1.0.125.0) profile. It owns its verification,
   INI and diagnostic log and never enters FH2's runtime or patch engine. */
int fh3_initialize(unsigned char *game_image, const wchar_t *executable_path,
                   const wchar_t *settings_directory);
#endif
