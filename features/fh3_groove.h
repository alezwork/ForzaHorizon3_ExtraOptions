#ifndef FH2_FH3_GROOVE_H
#define FH2_FH3_GROOVE_H
#include "extra_options.h"

/* The Xbox Music (Groove) radio station, restored with local music files.
   Its nine web service wrappers call the Forza web service dispatcher, whose
   XboxMusic service is long gone; those calls are routed through an int3
   padding thunk (jmp [rip]) into this module, which answers them from a music
   folder. SongStream opens each song with MFCreateSourceReaderFromURL (also
   routed here, for the log), and CForza's per-frame XboxMusicPlayer update is
   routed here to log the station's state. */
#define FH3_GROOVE_DISPATCHER_RVA 0x17860c0u
#define FH3_GROOVE_DISPATCHER_THUNK_RVA 0x2700580u
#define FH3_GROOVE_MF_READER_RVA 0x19b2786u
#define FH3_GROOVE_MF_THUNK_RVA 0x270bb80u
#define FH3_GROOVE_PLAYER_UPDATE_RVA 0x19479b0u
#define FH3_GROOVE_PLAYER_THUNK_RVA 0x260c610u

typedef struct {
    int log;                      /* write GROOVE lines to the mod log */
    wchar_t folder[MAX_PATH];     /* music folder, without a trailing slash */
} Fh3GrooveConfig;

void fh3_groove_configure(unsigned char *game_image, const Fh3GrooveConfig *config);
uintptr_t fh3_groove_dispatcher_callback(void);
uintptr_t fh3_groove_reader_callback(void);
uintptr_t fh3_groove_player_callback(void);
#endif
