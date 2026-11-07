#include "fh3_groove.h"
#include "logging.h"
#include "support.h"

/* Local music radio for the Xbox Music (Groove) station.

   The station talks to the Forza title web service "Forza.WebServices.XboxMusic"
   through the dispatcher 0x17860c0:
     dispatcher(service, method, request {.. std::string path @+0x28 ..}, 0, 0,
                &result_pointer, 8, result_type, 0, -1, &task)
   The result is a flat struct written to *result_pointer; the task is an
   object whose vtable is polled by the title server call pump (0x17bc740):
   slot 2 returns the status (2/3 = running, >= 0 = done, < 0 = failed), slot
   0/1 add and drop a reference, slot 4 detaches it (0x1786fe0). When it is
   done the pump calls the requester's ITitleServerCallHandler, which reads the
   struct. The service no longer exists, so every XboxMusic call is answered
   here: the struct is filled from the music folder and a task that is already
   done is returned.

   Result structs (offsets from the result pointer):
   GetUserPermissions -> XboxMusicAuthentication +0x80:
     bool HasSubscription, IsSubscriptionAvailableForPurchase, IsPreTrialAvailable
     (copied to +0x78..+0x7a by 0x1946e10; the station needs the first two).
   GetPlaylists -> PlaylistBrowser +0xbc (read by 0x1946b30):
     u32 count (<= 25); 25 x 0x3fc {i32 track count; wchar id[251] @+4;
     wchar name[257] @+0x1fa}; bool more @+0x63a4; wchar token @+0x63a6.
   GetPlaylistTracks[Shuffle] / Continue... -> Playlist +0x78, a zeroed
   0x297c0 buffer (read by 0x1946650): u32 lists @0 (1); bool valid @+8;
     u32 count @+0xc (<= 25); 25 x 0x1a78 tracks @+0x10 {wchar id[251];
     wchar title @+0x1f6 -> SongStream +0xf8; wchar artist @+0x5fc ->
     SongStream +0xd8; u32 duration @+0x1a74}; bool more @+0x295c8;
     wchar token @+0x295ca.
   GetSongURL -> SongStream +0x1b0: wchar songUrl[], copied to +0x98 by
     0x1946d80 and opened with MFCreateSourceReaderFromURL; a file path works. */

#define MAX_LOG_LINES 600
#define MAX_TRACKS 8192
#define MAX_PLAYLISTS 25
#define PAGE_TRACKS 25
#define MAX_DEPTH 6
#define TEXT_CHARS 120

#define PLAYLIST_STRIDE 0x3fcu
#define PLAYLIST_ID 0x4u
#define PLAYLIST_NAME 0x1fau
#define PLAYLISTS_MORE 0x63a4u
#define TRACKS_LISTS 0x0u
#define TRACKS_VALID 0x8u
#define TRACKS_COUNT 0xcu
#define TRACKS_FIRST 0x10u
#define TRACK_STRIDE 0x1a78u
#define TRACK_TITLE 0x1f6u
#define TRACK_ARTIST 0x5fcu
#define TRACKS_MORE 0x295c8u
#define TRACKS_TOKEN 0x295cau
#define SONG_URL_CHARS 1024u
#define ID_CHARS 250u

#define PLAYER_GLOBAL_RVA 0x4e6d770u
#define AUDIO_GLOBAL_RVA 0x4e728e8u

static const char xbox_music_service[] = "Forza.WebServices.XboxMusic, Forza.WebServices";

typedef HRESULT (*Dispatcher)(const char *, const char *, void *, uintptr_t, uintptr_t, uintptr_t,
                              uintptr_t, uintptr_t, uintptr_t, uintptr_t, void **);
typedef HRESULT (*ReaderFromUrl)(const wchar_t *, void *, void **);
typedef uintptr_t (*PlayerUpdate)(void *, uintptr_t, uintptr_t, uintptr_t);

static unsigned char *game;
static Fh3GrooveConfig config;
static volatile LONG lines;

/* ---- log ----------------------------------------------------------------- */

static void append_hex(char *out, size_t *at, uint64_t value, unsigned digits) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = digits; i; --i) out[(*at)++] = hex[(value >> ((i - 1) * 4)) & 15];
}

static void append_text(char *out, size_t *at, size_t capacity, const char *text) {
    while (text && *text && *at + 1 < capacity) out[(*at)++] = *text++;
}

static void append_wide(char *out, size_t *at, size_t capacity, const wchar_t *text) {
    while (text && *text && *at + 1 < capacity) {
        out[(*at)++] = *text < 0x80 ? (char)*text : '?';
        ++text;
    }
}

static void append_number(char *out, size_t *at, size_t capacity, unsigned value) {
    char digits[12];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count && *at + 1 < capacity) out[(*at)++] = digits[--count];
}

/* `line` holds at least LINE_SIZE bytes. */
#define LINE_SIZE 720
#define LINE_ROOM (LINE_SIZE - 16)

static void log_line(char *line, size_t at) {
    if (at > LINE_SIZE - 3) at = LINE_SIZE - 3;
    line[at++] = '\r';
    line[at++] = '\n';
    line[at] = 0;
    if (config.log && InterlockedIncrement(&lines) <= MAX_LOG_LINES) log_append(line);
}

/* ---- the music library --------------------------------------------------- */

typedef struct {
    wchar_t *path;
    wchar_t title[TEXT_CHARS], artist[TEXT_CHARS];
    unsigned playlist; /* top-level folder playlist, 0 for files in the root */
} Track;

typedef struct {
    wchar_t name[TEXT_CHARS];
    unsigned count;
} PlaylistInfo;

static Track *tracks;
static unsigned track_count, playlist_count;
static PlaylistInfo playlists[MAX_PLAYLISTS];
static unsigned *order;  /* scratch for the page being served */
static int scanned;

static size_t wide_length(const wchar_t *text) {
    size_t length = 0;
    while (text[length]) ++length;
    return length;
}

static void wide_copy(wchar_t *out, size_t capacity, const wchar_t *text, size_t length) {
    size_t at = 0;
    for (; at + 1 < capacity && at < length && text[at]; ++at) out[at] = text[at];
    out[at] = 0;
}

static void wide_from_ascii(wchar_t *out, size_t capacity, const char *text) {
    size_t at = 0;
    for (; at + 1 < capacity && text[at]; ++at) out[at] = (unsigned char)text[at];
    out[at] = 0;
}

static wchar_t lower(wchar_t c) { return c >= 'A' && c <= 'Z' ? (wchar_t)(c + 32) : c; }

static int is_music(const wchar_t *name) {
    static const char *const extensions[] = {".mp3", ".m4a", ".aac", ".wma", ".wav", ".flac", ".mp4"};
    size_t length = wide_length(name);
    for (unsigned e = 0; e < ARRAY_COUNT(extensions); ++e) {
        size_t n = text_length(extensions[e]), i = 0;
        if (length <= n) continue;
        while (i < n && lower(name[length - n + i]) == (wchar_t)extensions[e][i]) ++i;
        if (i == n) return 1;
    }
    return 0;
}

/* "Artist - Title.mp3" -> artist and title; otherwise the folder is the artist. */
static void describe(Track *track, const wchar_t *file, const wchar_t *folder) {
    size_t length = wide_length(file);
    while (length && file[length] != '.') --length;
    if (!length) length = wide_length(file);
    for (size_t i = 0; i + 3 <= length; ++i) {
        if (file[i] == ' ' && file[i + 1] == '-' && file[i + 2] == ' ') {
            wide_copy(track->artist, TEXT_CHARS, file, i);
            wide_copy(track->title, TEXT_CHARS, file + i + 3, length - i - 3);
            return;
        }
    }
    wide_copy(track->title, TEXT_CHARS, file, length);
    wide_copy(track->artist, TEXT_CHARS, folder, wide_length(folder));
}

static wchar_t scan_path[32768];

static void scan(size_t length, unsigned depth, unsigned playlist, const wchar_t *folder_name) {
    if (length + 4 >= ARRAY_COUNT(scan_path)) return;
    scan_path[length] = '\\';
    scan_path[length + 1] = '*';
    scan_path[length + 2] = 0;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(scan_path, &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t *name = found.cFileName;
        size_t name_length = wide_length(name);
        if (name[0] == '.' || length + 2 + name_length >= ARRAY_COUNT(scan_path)) continue;
        wide_copy(scan_path + length + 1, ARRAY_COUNT(scan_path) - length - 1, name, name_length);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth >= MAX_DEPTH) continue;
            unsigned child = playlist;
            if (!depth && playlist_count < MAX_PLAYLISTS) {
                child = playlist_count++;
                wide_copy(playlists[child].name, TEXT_CHARS, name, name_length);
            }
            scan(length + 1 + name_length, depth + 1, child, name);
        } else if (is_music(name) && track_count < MAX_TRACKS) {
            size_t path_length = length + 1 + name_length;
            wchar_t *path = HeapAlloc(GetProcessHeap(), 0, (path_length + 1) * sizeof(wchar_t));
            if (!path) continue;
            wide_copy(path, path_length + 1, scan_path, path_length);
            Track *track = &tracks[track_count++];
            track->path = path;
            track->playlist = playlist;
            describe(track, name, folder_name);
            ++playlists[playlist].count;
            if (playlist) ++playlists[0].count;
        }
        scan_path[length + 1] = 0;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    scan_path[length] = 0;
}

static void scan_library(void) {
    if (scanned) return;
    scanned = 1;
    tracks = HeapAlloc(GetProcessHeap(), 0, sizeof(Track) * MAX_TRACKS);
    order = HeapAlloc(GetProcessHeap(), 0, sizeof(unsigned) * MAX_TRACKS);
    if (!tracks || !order) return;
    playlist_count = 1;
    wide_from_ascii(playlists[0].name, TEXT_CHARS, "All music");
    size_t length = wide_length(config.folder);
    wide_copy(scan_path, ARRAY_COUNT(scan_path), config.folder, length);
    scan(length, 0, 0, L"Local music");
    char line[LINE_SIZE];
    size_t at = 0;
    append_text(line, &at, LINE_ROOM, "GROOVE: music folder ");
    append_wide(line, &at, LINE_ROOM - 60, config.folder);
    append_text(line, &at, LINE_ROOM, ": ");
    append_number(line, &at, LINE_ROOM, track_count);
    append_text(line, &at, LINE_ROOM, " songs, ");
    append_number(line, &at, LINE_ROOM, playlist_count);
    append_text(line, &at, LINE_ROOM, " playlists");
    log_line(line, at);
}

/* ---- the fake task -------------------------------------------------------- */

typedef struct {
    void **vtable;
    volatile LONG references;
    HRESULT status;
} Task;

static uintptr_t task_add(Task *self) { return (uintptr_t)InterlockedIncrement(&self->references); }
static uintptr_t task_release(Task *self) {
    LONG left = InterlockedDecrement(&self->references);
    if (!left) HeapFree(GetProcessHeap(), 0, self);
    return (uintptr_t)(left < 0 ? 0 : left);
}
static uintptr_t task_status(Task *self) { return (uintptr_t)(uint32_t)self->status; }
static uintptr_t task_nothing(Task *self) { (void)self; return 0; }
static void *task_methods[] = {
    (void *)task_add,     (void *)task_release, (void *)task_status,  (void *)task_nothing,
    (void *)task_nothing, (void *)task_nothing, (void *)task_nothing, (void *)task_nothing,
    (void *)task_nothing, (void *)task_nothing, (void *)task_nothing, (void *)task_nothing};

static HRESULT finish(void **out, HRESULT status) {
    Task *task = HeapAlloc(GetProcessHeap(), 0, sizeof(Task));
    if (!task) return E_OUTOFMEMORY;
    task->vtable = task_methods;
    task->references = 1;
    task->status = status;
    if (out) *out = task;
    else task_release(task);
    return S_OK;
}

/* ---- answers -------------------------------------------------------------- */

static const char *std_string(const unsigned char *field, char *copy, size_t capacity) {
    /* MSVC std::string: 16-byte SSO buffer, size @+0x10, capacity @+0x18. */
    size_t size, reserved;
    const char *text = (const char *)field;
    copy[0] = 0;
    if (!memory_read(field + 0x10, &size, sizeof(size)) ||
        !memory_read(field + 0x18, &reserved, sizeof(reserved)) || size >= capacity) {
        return copy;
    }
    if (reserved >= 16 && !memory_read_pointer(field, (void **)&text)) return copy;
    if (!memory_read(text, copy, size)) size = 0;
    copy[size] = 0;
    return copy;
}

/* Digits at *text, advancing it; `fallback` when there are none. */
static unsigned read_number(const char **text, unsigned fallback) {
    const char *at = *text;
    if (*at < '0' || *at > '9') return fallback;
    unsigned value = 0;
    while (*at >= '0' && *at <= '9') value = value * 10 + (unsigned)(*at++ - '0');
    *text = at;
    return value;
}

/* Number after the first `tag` in `text`, or `fallback`. */
static unsigned number_after(const char *text, const char *tag, unsigned fallback) {
    const char *found = text_find(text, text_length(text), tag);
    if (!found) return fallback;
    found += text_length(tag);
    return read_number(&found, fallback);
}

/* Continuation token fh3c<playlist>o<offset>s<seed>; 0 when absent. */
static int read_token(const char *text, unsigned *playlist, unsigned *offset, uint32_t *seed) {
    const char *at = text_find(text, text_length(text), "fh3c");
    if (!at) return 0;
    at += 4;
    *playlist = read_number(&at, 0);
    if (*at++ != 'o') return 0;
    *offset = read_number(&at, 0);
    if (*at++ != 's') return 0;
    *seed = read_number(&at, 0);
    return 1;
}

static void put_wide(unsigned char *out, size_t capacity, const wchar_t *text) {
    wchar_t *target = (wchar_t *)out;
    size_t at = 0;
    for (; text && text[at] && at + 1 < capacity; ++at) target[at] = text[at];
    target[at] = 0;
}

static void put_ascii(unsigned char *out, size_t capacity, const char *text) {
    wchar_t wide[64];
    wide_from_ascii(wide, ARRAY_COUNT(wide), text);
    put_wide(out, capacity, wide);
}

static void ascii_id(char *out, const char *prefix, unsigned value) {
    size_t at = 0;
    append_text(out, &at, 32, prefix);
    append_number(out, &at, 32, value);
    out[at] = 0;
}

static void answer_permissions(unsigned char *result) {
    result[0] = 1; /* HasSubscription */
    result[1] = 1; /* IsSubscriptionAvailableForPurchase */
    result[2] = 0; /* IsPreTrialAvailable */
}

static unsigned answer_playlists(unsigned char *result) {
    unsigned served = 0;
    for (unsigned p = 0; p < playlist_count && served < MAX_PLAYLISTS; ++p) {
        if (!playlists[p].count) continue;
        unsigned char *entry = result + 4 + served++ * PLAYLIST_STRIDE;
        char id[32];
        ascii_id(id, "fh3p", p);
        *(int32_t *)entry = (int32_t)playlists[p].count;
        put_ascii(entry + PLAYLIST_ID, ID_CHARS, id);
        put_wide(entry + PLAYLIST_NAME, 256, playlists[p].name);
    }
    *(uint32_t *)result = served;
    result[PLAYLISTS_MORE] = 0;
    return served;
}

static uint32_t next_random(uint32_t *state) {
    uint32_t x = *state ? *state : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

/* Tracks of `playlist` in folder order, or shuffled by `seed`. */
static unsigned playlist_order(unsigned playlist, uint32_t seed) {
    unsigned count = 0;
    for (unsigned t = 0; t < track_count; ++t) {
        if (!playlist || tracks[t].playlist == playlist) order[count++] = t;
    }
    if (seed) {
        uint32_t state = seed;
        for (unsigned i = count; i > 1; --i) {
            unsigned j = next_random(&state) % i, swap = order[i - 1];
            order[i - 1] = order[j];
            order[j] = swap;
        }
    }
    return count;
}

static unsigned answer_tracks(unsigned char *result, unsigned playlist, unsigned offset, uint32_t seed) {
    unsigned count = playlist < playlist_count ? playlist_order(playlist, seed) : 0;
    unsigned served = 0;
    for (unsigned i = offset; i < count && served < PAGE_TRACKS; ++i, ++served) {
        const Track *track = &tracks[order[i]];
        unsigned char *entry = result + TRACKS_FIRST + served * TRACK_STRIDE;
        char id[32];
        ascii_id(id, "fh3x", order[i]);
        put_ascii(entry, ID_CHARS, id);
        put_wide(entry + TRACK_TITLE, 256, track->title);
        put_wide(entry + TRACK_ARTIST, 256, track->artist);
    }
    *(uint32_t *)(result + TRACKS_LISTS) = 1;
    result[TRACKS_VALID] = 1;
    *(uint32_t *)(result + TRACKS_COUNT) = served;
    result[TRACKS_MORE] = 0;
    if (offset + served < count) {
        /* Continuation token: fh3c<playlist>o<next offset>s<seed>. */
        char token[64];
        size_t at = 0;
        append_text(token, &at, sizeof(token), "fh3c");
        append_number(token, &at, sizeof(token), playlist);
        append_text(token, &at, sizeof(token), "o");
        append_number(token, &at, sizeof(token), offset + served);
        append_text(token, &at, sizeof(token), "s");
        append_number(token, &at, sizeof(token), seed);
        token[at] = 0;
        result[TRACKS_MORE] = 1;
        put_ascii(result + TRACKS_TOKEN, ID_CHARS, token);
    }
    return served;
}

static HRESULT answer(const char *method, const char *path, unsigned char *result, void **task,
                      char *line, size_t *at) {
    scan_library();
    if (!tracks || !order) return finish(task, E_OUTOFMEMORY);
    if (text_equal_ignore_case(method, "GetUserPermissions")) {
        answer_permissions(result);
        append_text(line, at, LINE_ROOM, " -> subscribed");
        return finish(task, S_OK);
    }
    if (text_equal_ignore_case(method, "GetPlaylists") ||
        text_equal_ignore_case(method, "ContinueGetPlaylists")) {
        append_text(line, at, LINE_ROOM, " -> playlists ");
        append_number(line, at, LINE_ROOM, answer_playlists(result));
        return finish(task, S_OK);
    }
    int shuffle = text_find(method, text_length(method), "Shuffle") != NULL;
    if (text_find(method, text_length(method), "PlaylistTracks")) {
        unsigned playlist = 0, offset = 0;
        uint32_t seed = 0;
        if (!read_token(path, &playlist, &offset, &seed)) {
            playlist = number_after(path, "fh3p", 0);
            offset = 0;
            seed = shuffle ? (uint32_t)GetTickCount64() | 1u : 0;
        }
        if (playlist >= playlist_count) playlist = 0;
        unsigned served = answer_tracks(result, playlist, offset, seed);
        append_text(line, at, LINE_ROOM, " -> playlist ");
        append_number(line, at, LINE_ROOM, playlist);
        append_text(line, at, LINE_ROOM, " songs ");
        append_number(line, at, LINE_ROOM, offset);
        append_text(line, at, LINE_ROOM, "+");
        append_number(line, at, LINE_ROOM, served);
        return finish(task, served ? S_OK : E_FAIL);
    }
    if (text_equal_ignore_case(method, "GetSongURL")) {
        unsigned index = number_after(path, "fh3x", ~0u);
        if (index >= track_count) {
            append_text(line, at, LINE_ROOM, " -> unknown song");
            return finish(task, E_FAIL);
        }
        put_wide(result, SONG_URL_CHARS - 16, tracks[index].path);
        /* XboxMusicRadioStation (0x19a70f0) only plays URLs that contain
           audio.mp3, audio.m4a, audio.mp4 or transcode/mp3, and audio.mp3
           selects a separate MP3 path (0x1983da0). The marker routes every
           file to the Media Foundation reader; open_reader removes it. */
        wchar_t *url = (wchar_t *)result;
        size_t end = wide_length(url);
        static const wchar_t marker[] = L"|audio.m4a";
        for (size_t i = 0; marker[i]; ++i) url[end++] = marker[i];
        url[end] = 0;
        append_text(line, at, LINE_ROOM, " -> ");
        append_wide(line, at, LINE_ROOM, tracks[index].path);
        return finish(task, S_OK);
    }
    append_text(line, at, LINE_ROOM, " -> done");
    return finish(task, S_OK);
}

static HRESULT dispatch(const char *service, const char *method, void *request, uintptr_t a4,
                        uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9,
                        uintptr_t a10, void **task) {
    char line[LINE_SIZE], path[400];
    size_t at = 0;
    if (!service || !method || !text_equal_ignore_case(service, xbox_music_service)) {
        return ((Dispatcher)(game + FH3_GROOVE_DISPATCHER_RVA))(service, method, request, a4, a5,
                                                                a6, a7, a8, a9, a10, task);
    }
    if (request) std_string((unsigned char *)request + 0x28, path, sizeof(path));
    else path[0] = 0;
    append_text(line, &at, LINE_ROOM, "GROOVE: ");
    append_text(line, &at, LINE_ROOM, method);
    append_text(line, &at, LINE_ROOM, " ");
    append_text(line, &at, LINE_ROOM - 200, path);
    unsigned char *result = NULL;
    HRESULT hr;
    if (!a6 || !memory_read_pointer((void *)a6, (void **)&result) || !result) {
        append_text(line, &at, LINE_ROOM, " -> no result buffer");
        hr = finish(task, E_FAIL);
    } else {
        hr = answer(method, path, result, task, line, &at);
    }
    log_line(line, at);
    return hr;
}

/* Azimuth's MFCreateSourceReaderFromURL maps the game's Xbox paths (G:\ is
   the game folder) and cannot open a Windows path such as D:\Music\a.mp3
   (0x80070003). Songs from the music folder are therefore opened with the
   host's own Media Foundation from System32; the reader it returns is an
   ordinary IMFSourceReader, which SongStream only uses through COM. */
typedef HRESULT(WINAPI *StartupFn)(ULONG, DWORD);
typedef HRESULT(WINAPI *ComInitFn)(void *, DWORD);
#define LOAD_FROM_SYSTEM32 0x00000800u
#define MF_STARTUP_VERSION 0x00020070u
#define COM_MULTITHREADED 0x0u

typedef HRESULT(WINAPI *CreateTypeFn)(void **);
typedef HRESULT(WINAPI *CreateBufferFn)(DWORD, void **);

static ReaderFromUrl host_reader;
static ComInitFn host_com_init;
static CreateTypeFn host_create_type, host_create_sample;
static CreateBufferFn host_create_buffer;
static volatile LONG host_state; /* 0 = not tried, 1 = loading, 2 = ready, 3 = unavailable */
static HRESULT host_startup = E_FAIL;

static int host_media_foundation(void) {
    if (InterlockedCompareExchange(&host_state, 1, 0) == 0) {
        HMODULE plat = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_FROM_SYSTEM32);
        HMODULE readwrite = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_FROM_SYSTEM32);
        HMODULE combase = LoadLibraryExW(L"combase.dll", NULL, LOAD_FROM_SYSTEM32);
        StartupFn startup = plat ? (StartupFn)(uintptr_t)GetProcAddress(plat, "MFStartup") : NULL;
        if (combase) host_com_init = (ComInitFn)(uintptr_t)GetProcAddress(combase, "CoInitializeEx");
        if (host_com_init) host_com_init(NULL, COM_MULTITHREADED);
        if (startup) host_startup = startup(MF_STARTUP_VERSION, 0);
        if (plat) {
            host_create_type = (CreateTypeFn)(uintptr_t)GetProcAddress(plat, "MFCreateMediaType");
            host_create_sample = (CreateTypeFn)(uintptr_t)GetProcAddress(plat, "MFCreateSample");
            host_create_buffer = (CreateBufferFn)(uintptr_t)GetProcAddress(plat, "MFCreateMemoryBuffer");
        }
        if (readwrite && host_startup >= 0 && host_create_sample && host_create_buffer) {
            host_reader = (ReaderFromUrl)(uintptr_t)GetProcAddress(readwrite, "MFCreateSourceReaderFromURL");
        }
        InterlockedExchange(&host_state, host_reader ? 2 : 3);
    }
    while (host_state == 1) Sleep(1);
    return host_state == 2;
}

/* The game asks the reader for 16-bit stereo PCM at the file's own rate
   (0x196fb20), but builds that media type with Azimuth's MFPlat and ignores
   a failed SetCurrentMediaType. The DLL sets 16-bit stereo PCM at 44.1 kHz,
   the FMOD stream's rate, with Windows objects instead. */
static const GUID mt_major_type = {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
static const GUID mt_subtype = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
static const GUID media_audio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID audio_pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID mt_channels = {0x37e48bf5, 0x645e, 0x4c5b, {0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a}};
static const GUID mt_rate = {0x5faeeae7, 0x0290, 0x4c31, {0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba}};
static const GUID mt_block = {0x322de230, 0x9eeb, 0x43bd, {0xab, 0x7a, 0xff, 0x41, 0x22, 0x51, 0x54, 0x1d}};
static const GUID mt_bytes = {0x1aab75c8, 0xcfef, 0x451c, {0xab, 0x95, 0xac, 0x03, 0x4b, 0x8e, 0x17, 0x31}};
static const GUID mt_bits = {0xf2deb57f, 0x40fa, 0x4764, {0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69}};
static const GUID mt_independent = {0xc9173739, 0x5e56, 0x461c, {0xb7, 0x13, 0x46, 0xfb, 0x99, 0x5c, 0xb9, 0x5f}};

/* IMFSourceReader: 5 GetNativeMediaType, 7 SetCurrentMediaType.
   IMFAttributes: 2 Release, 7 GetUINT32, 21 SetUINT32, 24 SetGUID. */
typedef HRESULT(STDMETHODCALLTYPE *NativeTypeFn)(void *, DWORD, DWORD, void **);
typedef HRESULT(STDMETHODCALLTYPE *SetTypeFn)(void *, DWORD, DWORD *, void *);
typedef ULONG(STDMETHODCALLTYPE *ReleaseFn)(void *);
typedef HRESULT(STDMETHODCALLTYPE *GetU32Fn)(void *, const GUID *, UINT32 *);
typedef HRESULT(STDMETHODCALLTYPE *SetU32Fn)(void *, const GUID *, UINT32);
typedef HRESULT(STDMETHODCALLTYPE *SetGuidFn)(void *, const GUID *, const GUID *);

static void **com_methods(void *object) { return *(void ***)object; }

#define STREAM_RATE 0xac44u /* the FMOD stream's fixed rate (0x198ff64) */

static HRESULT request_pcm(void *reader, UINT32 rate) {
    void *type = NULL;
    if (!host_create_type) return E_NOINTERFACE;
    HRESULT hr = host_create_type(&type);
    if (hr < 0 || !type) return hr < 0 ? hr : E_FAIL;
    void **m = com_methods(type);
    ((SetGuidFn)m[24])(type, &mt_major_type, &media_audio);
    ((SetGuidFn)m[24])(type, &mt_subtype, &audio_pcm);
    ((SetU32Fn)m[21])(type, &mt_channels, 2);
    ((SetU32Fn)m[21])(type, &mt_rate, rate);
    ((SetU32Fn)m[21])(type, &mt_bits, 16);
    ((SetU32Fn)m[21])(type, &mt_block, 4);
    ((SetU32Fn)m[21])(type, &mt_bytes, rate * 4);
    ((SetU32Fn)m[21])(type, &mt_independent, 1);
    hr = ((SetTypeFn)com_methods(reader)[7])(reader, 0, NULL, type);
    ((ReleaseFn)m[2])(type);
    return hr;
}

/* The FMOD stream plays at 44.1 kHz whatever the song's rate, so a 48 kHz
   file given at its own rate plays about 8% slow. Ask the Windows reader to
   resample to 44.1 kHz; only if it refuses, keep the file's own rate. */
static HRESULT set_pcm_output(void *reader, UINT32 *rate_out) {
    UINT32 native_rate = 0;
    void *native = NULL;
    if (((NativeTypeFn)com_methods(reader)[5])(reader, 0, 0, &native) >= 0 && native) {
        ((GetU32Fn)com_methods(native)[7])(native, &mt_rate, &native_rate);
        ((ReleaseFn)com_methods(native)[2])(native);
    }
    *rate_out = STREAM_RATE;
    HRESULT hr = request_pcm(reader, STREAM_RATE);
    if (hr < 0 && native_rate && native_rate != STREAM_RATE) {
        *rate_out = native_rate;
        hr = request_pcm(reader, native_rate);
    }
    return hr;
}

/* ---- fixed-size samples ----------------------------------------------------
   The FMOD read callback (0x198a520) pops one decoded sample per copy and
   drops whatever does not fit the 16384 bytes FMOD asked for (4096 frames,
   decodebuffersize at 0x198ff7c). Groove's AAC frames were 1024 frames =
   4096 bytes, so nothing was lost; Windows' MP3 decoder returns 1152 frames,
   which lost an eighth of every buffer and made songs sound fast-forwarded.
   The Windows reader is wrapped so ReadSample always returns 4096 bytes. */
#define CHUNK_BYTES 0x1000u
#define READER_FLAG_ERROR 0x1u
#define READER_FLAG_END_OF_STREAM 0x2u

typedef struct {
    void **vtable;
    volatile LONG references;
    void *inner;
    unsigned char *pending;
    DWORD capacity, size, offset;
    int ended;
    LONGLONG time;
} ChunkReader;

static const GUID iid_unknown_reader = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID iid_source_reader = {0x70ae66f2, 0xc809, 0x4e4f, {0x89, 0x15, 0xbd, 0xcb, 0x40, 0x6b, 0x79, 0x93}};

typedef HRESULT(STDMETHODCALLTYPE *ReadSampleFn)(void *, DWORD, DWORD, DWORD *, DWORD *, LONGLONG *, void **);
typedef HRESULT(STDMETHODCALLTYPE *ContiguousFn)(void *, void **);
typedef HRESULT(STDMETHODCALLTYPE *LockFn)(void *, BYTE **, DWORD *, DWORD *);
typedef HRESULT(STDMETHODCALLTYPE *UnlockFn)(void *);
typedef HRESULT(STDMETHODCALLTYPE *SetLengthFn)(void *, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *AddBufferFn)(void *, void *);
typedef HRESULT(STDMETHODCALLTYPE *SetTimeFn)(void *, LONGLONG);

static void **inner_methods(ChunkReader *self) { return *(void ***)self->inner; }

static HRESULT STDMETHODCALLTYPE chunk_query(ChunkReader *self, const GUID *iid, void **out) {
    if (memory_equal(iid, &iid_unknown_reader, sizeof(GUID)) || memory_equal(iid, &iid_source_reader, sizeof(GUID))) {
        InterlockedIncrement(&self->references);
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE chunk_add(ChunkReader *self) { return (ULONG)InterlockedIncrement(&self->references); }
static ULONG STDMETHODCALLTYPE chunk_release(ChunkReader *self) {
    LONG left = InterlockedDecrement(&self->references);
    if (!left) {
        ((ReleaseFn)inner_methods(self)[2])(self->inner);
        if (self->pending) HeapFree(GetProcessHeap(), 0, self->pending);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return (ULONG)left;
}

/* Append one decoded sample of the Windows reader to the pending bytes. */
static HRESULT chunk_fill(ChunkReader *self, DWORD stream) {
    DWORD index = 0, flags = 0;
    LONGLONG time = 0;
    void *sample = NULL;
    HRESULT hr = ((ReadSampleFn)inner_methods(self)[9])(self->inner, stream, 0, &index, &flags, &time, &sample);
    if (hr < 0 || (flags & (READER_FLAG_ERROR | READER_FLAG_END_OF_STREAM))) self->ended = 1;
    if (!sample) return hr;
    void *buffer = NULL;
    BYTE *data = NULL;
    DWORD max = 0, length = 0;
    if (((ContiguousFn)com_methods(sample)[41])(sample, &buffer) >= 0 && buffer) {
        if (((LockFn)com_methods(buffer)[3])(buffer, &data, &max, &length) >= 0 && data) {
            if (self->offset) {
                /* Move the unread tail to the front before growing. */
                DWORD tail = self->size - self->offset;
                /* Forward copy to a lower address: safe for the overlap. */
                memory_copy(self->pending, self->pending + self->offset, tail);
                self->size = tail;
                self->offset = 0;
            }
            if (self->size + length > self->capacity) {
                DWORD capacity = self->size + length + CHUNK_BYTES;
                unsigned char *grown = HeapAlloc(GetProcessHeap(), 0, capacity);
                if (grown) {
                    if (self->pending) {
                        memory_copy(grown, self->pending, self->size);
                        HeapFree(GetProcessHeap(), 0, self->pending);
                    }
                    self->pending = grown;
                    self->capacity = capacity;
                }
            }
            if (self->size + length <= self->capacity) {
                memory_copy(self->pending + self->size, data, length);
                self->size += length;
            }
            ((UnlockFn)com_methods(buffer)[4])(buffer);
        }
        ((ReleaseFn)com_methods(buffer)[2])(buffer);
    }
    ((ReleaseFn)com_methods(sample)[2])(sample);
    return hr;
}

static HRESULT STDMETHODCALLTYPE chunk_read(ChunkReader *self, DWORD stream, DWORD control, DWORD *actual,
                                            DWORD *flags, LONGLONG *timestamp, void **out) {
    (void)control;
    if (actual) *actual = 0;
    if (flags) *flags = 0;
    if (timestamp) *timestamp = self->time;
    if (out) *out = NULL;
    while (!self->ended && self->size - self->offset < CHUNK_BYTES) {
        if (chunk_fill(self, stream) < 0) break;
    }
    DWORD available = self->size - self->offset;
    if (!available) {
        if (flags) *flags = READER_FLAG_END_OF_STREAM;
        return S_OK;
    }
    void *buffer = NULL, *sample = NULL;
    BYTE *data = NULL;
    DWORD max = 0, length = 0;
    HRESULT hr = host_create_buffer(CHUNK_BYTES, &buffer);
    if (hr < 0 || !buffer) return hr < 0 ? hr : E_OUTOFMEMORY;
    hr = ((LockFn)com_methods(buffer)[3])(buffer, &data, &max, &length);
    if (hr >= 0 && data) {
        DWORD copy = available < CHUNK_BYTES ? available : CHUNK_BYTES;
        memory_copy(data, self->pending + self->offset, copy);
        /* The last chunk of a song is padded with silence. */
        volatile BYTE *silence = data;
        for (DWORD i = copy; i < CHUNK_BYTES; ++i) silence[i] = 0;
        self->offset += copy;
        ((UnlockFn)com_methods(buffer)[4])(buffer);
        ((SetLengthFn)com_methods(buffer)[6])(buffer, CHUNK_BYTES);
        hr = host_create_sample(&sample);
        if (hr >= 0 && sample) {
            ((AddBufferFn)com_methods(sample)[42])(sample, buffer);
            ((SetTimeFn)com_methods(sample)[36])(sample, self->time);
            /* 1024 frames at 44.1 kHz, in 100 ns units. */
            self->time += 232199;
        }
    }
    ((ReleaseFn)com_methods(buffer)[2])(buffer);
    if (out) *out = sample;
    else if (sample) ((ReleaseFn)com_methods(sample)[2])(sample);
    return hr;
}

static HRESULT STDMETHODCALLTYPE chunk_position(ChunkReader *self, const GUID *format, const void *position) {
    self->size = self->offset = 0;
    self->ended = 0;
    return ((HRESULT(STDMETHODCALLTYPE *)(void *, const GUID *, const void *))inner_methods(self)[8])(
        self->inner, format, position);
}
static HRESULT STDMETHODCALLTYPE chunk_flush(ChunkReader *self, DWORD stream) {
    self->size = self->offset = 0;
    return ((HRESULT(STDMETHODCALLTYPE *)(void *, DWORD))inner_methods(self)[10])(self->inner, stream);
}
#define CHUNK_FORWARD2(name, slot, t1, t2) \
    static HRESULT STDMETHODCALLTYPE name(ChunkReader *self, t1 a, t2 b) { \
        return ((HRESULT(STDMETHODCALLTYPE *)(void *, t1, t2))inner_methods(self)[slot])(self->inner, a, b); }
#define CHUNK_FORWARD3(name, slot, t1, t2, t3) \
    static HRESULT STDMETHODCALLTYPE name(ChunkReader *self, t1 a, t2 b, t3 c) { \
        return ((HRESULT(STDMETHODCALLTYPE *)(void *, t1, t2, t3))inner_methods(self)[slot])(self->inner, a, b, c); }
#define CHUNK_FORWARD4(name, slot, t1, t2, t3, t4) \
    static HRESULT STDMETHODCALLTYPE name(ChunkReader *self, t1 a, t2 b, t3 c, t4 d) { \
        return ((HRESULT(STDMETHODCALLTYPE *)(void *, t1, t2, t3, t4))inner_methods(self)[slot])(self->inner, a, b, c, d); }
CHUNK_FORWARD2(chunk_get_selection, 3, DWORD, BOOL *)
CHUNK_FORWARD2(chunk_set_selection, 4, DWORD, BOOL)
CHUNK_FORWARD3(chunk_native_type, 5, DWORD, DWORD, void **)
CHUNK_FORWARD2(chunk_current_type, 6, DWORD, void **)
CHUNK_FORWARD3(chunk_set_type, 7, DWORD, DWORD *, void *)
CHUNK_FORWARD4(chunk_service, 11, DWORD, const GUID *, const GUID *, void **)
CHUNK_FORWARD3(chunk_attribute, 12, DWORD, const GUID *, void *)

static void *chunk_methods[] = {
    (void *)chunk_query,     (void *)chunk_add,         (void *)chunk_release,      (void *)chunk_get_selection,
    (void *)chunk_set_selection, (void *)chunk_native_type, (void *)chunk_current_type, (void *)chunk_set_type,
    (void *)chunk_position,  (void *)chunk_read,        (void *)chunk_flush,        (void *)chunk_service,
    (void *)chunk_attribute};

/* Wrap a Windows source reader; on failure the reader is kept as it is. */
static void *chunk_wrap(void *inner) {
    ChunkReader *self = HeapAlloc(GetProcessHeap(), 0, sizeof(ChunkReader));
    if (!self) return inner;
    self->vtable = chunk_methods;
    self->references = 1;
    self->inner = inner;
    self->pending = NULL;
    self->capacity = self->size = self->offset = 0;
    self->ended = 0;
    self->time = 0;
    return self;
}

static HRESULT open_reader(const wchar_t *url, void *attributes, void **reader) {
    char line[LINE_SIZE];
    size_t at = 0;
    /* Drop the "|audio.m4a" marker added by GetSongURL: the rest is a file path. */
    wchar_t path[SONG_URL_CHARS];
    size_t length = 0;
    while (url && url[length] && url[length] != '|' && length + 1 < ARRAY_COUNT(path)) {
        path[length] = url[length];
        ++length;
    }
    path[length] = 0;
    int local = url && url[length] == '|';
    HRESULT hr, format = S_FALSE;
    UINT32 rate = 0;
    const char *via = "game";
    if (local && host_media_foundation()) {
        /* The reader thread may not have joined COM yet. */
        if (host_com_init) host_com_init(NULL, COM_MULTITHREADED);
        hr = host_reader(path, attributes, reader);
        via = "windows";
        if (hr >= 0 && reader && *reader) {
            format = set_pcm_output(*reader, &rate);
            *reader = chunk_wrap(*reader);
        }
    } else {
        hr = ((ReaderFromUrl)(game + FH3_GROOVE_MF_READER_RVA))(local ? path : url, attributes, reader);
    }
    append_text(line, &at, LINE_ROOM, "GROOVE: MFCreateSourceReaderFromURL (");
    append_text(line, &at, LINE_ROOM, via);
    append_text(line, &at, LINE_ROOM, ") ");
    append_wide(line, &at, LINE_ROOM - 60, local ? path : url);
    append_text(line, &at, LINE_ROOM, " -> hr=0x");
    append_hex(line, &at, (uint32_t)hr, 8);
    if (format != S_FALSE) {
        append_text(line, &at, LINE_ROOM, ", PCM 16-bit stereo ");
        append_number(line, &at, LINE_ROOM, rate);
        append_text(line, &at, LINE_ROOM, " Hz hr=0x");
        append_hex(line, &at, (uint32_t)format, 8);
    }
    if (local && host_state == 3) {
        append_text(line, &at, LINE_ROOM, " (Windows Media Foundation unavailable, MFStartup 0x");
        append_hex(line, &at, (uint32_t)host_startup, 8);
        append_text(line, &at, LINE_ROOM, ")");
    }
    log_line(line, at);
    return hr;
}

/* ---- state log ----------------------------------------------------------- */

typedef void *(*AudioSettingsGetter)(void *);

/* The checks of IsXboxMusicAccessible (0x1946030) and the playback state. */
static void report_state(unsigned char *player) {
    static unsigned char last[16];
    static int reported;
    unsigned char now[16] = {0};
    void *audio, *vtable, *settings = NULL;
    AudioSettingsGetter getter;
    if (memory_read_pointer(game + AUDIO_GLOBAL_RVA, &audio) && audio &&
        memory_read_pointer(audio, &vtable) &&
        memory_read((unsigned char *)vtable + 0x310, &getter, sizeof(getter)) && getter) {
        settings = getter(audio);
    }
    if (settings) {
        memory_read((unsigned char *)settings + 0x3a, &now[0], 1);
        memory_read((unsigned char *)settings + 0x80, &now[1], 1);
    }
    memory_read(player + 0x6900, &now[2], 1);  /* Groove enabled in the profile */
    memory_read(player + 0x68f8, &now[3], 1);  /* authentication state */
    memory_read(player + 0x66e8, &now[4], 2);  /* subscription flags */
    memory_read(player + 0x6668, &now[6], 1);  /* station active */
    void *playlist = NULL;
    if (memory_read_pointer(player + 0x6660, &playlist) && playlist) {
        memory_read((unsigned char *)playlist + 0x100, &now[7], 1);
    }
    if (reported && memory_equal(now, last, sizeof(now))) return;
    reported = 1;
    memory_copy(last, now, sizeof(now));
    static const char *const names[] = {"audio+3a=", " audio+80=", " enabled=", " auth=",
                                        " subscriber=", " available=", " active=", " playlist="};
    char line[LINE_SIZE];
    size_t at = 0;
    append_text(line, &at, LINE_ROOM, "GROOVE: state ");
    for (unsigned i = 0; i < ARRAY_COUNT(names); ++i) {
        append_text(line, &at, LINE_ROOM, names[i]);
        append_number(line, &at, LINE_ROOM, now[i]);
    }
    log_line(line, at);
}

static uintptr_t player_update(void *player, uintptr_t inactive, uintptr_t r8, uintptr_t r9) {
    uintptr_t result = ((PlayerUpdate)(game + FH3_GROOVE_PLAYER_UPDATE_RVA))(player, inactive, r8, r9);
    if (player) report_state(player);
    return result;
}

void fh3_groove_configure(unsigned char *game_image, const Fh3GrooveConfig *settings) {
    game = game_image;
    memory_copy(&config, settings, sizeof(config));
    lines = 0;
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCWSTR)(uintptr_t)dispatch, &pinned);
}

uintptr_t fh3_groove_dispatcher_callback(void) {
    return (uintptr_t)dispatch;
}

uintptr_t fh3_groove_reader_callback(void) {
    return (uintptr_t)open_reader;
}

uintptr_t fh3_groove_player_callback(void) {
    return (uintptr_t)player_update;
}
