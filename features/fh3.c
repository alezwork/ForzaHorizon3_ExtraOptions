#include "fh3.h"
#include "fh3_groove.h"
#include "fh3_lights.h"
#include "fh3_minimap.h"
#include "logging.h"
#include "sha256.h"
#include "support.h"
#ifdef FH2_AZIMUTH
#include "adapters/azimuth_host.h"
#endif

#define FH3_INI_NAME L"FH3_ExtraOptions.ini"
#define FH3_LOG_NAME L"FH3_ExtraOptions.log"
#define FH3_FILE_SIZE 85425664u
#define FH3_IMAGE_SIZE 0x5495000u
#define FH3_TIMESTAMP 0x5b16eccau

static const unsigned char fh3_file_sha256[32] = {
    0xe6,0xa3,0x62,0xa2,0xfd,0xe1,0x18,0x57,0x7a,0xb3,0x01,0x8c,0x8e,0x04,0x2e,0x62,
    0xa3,0xbb,0x42,0xf8,0x3a,0x63,0xac,0xe4,0x11,0x45,0xc0,0x86,0x45,0x27,0xa6,0x61};

typedef enum {
    FH3_GROUP_INTRO, FH3_GROUP_VIP, FH3_GROUP_TREASURE_MAP, FH3_GROUP_DLC,
    FH3_GROUP_SUN_SHADOWS, FH3_GROUP_BLUR_RIMS, FH3_GROUP_HEADLIGHTS, FH3_GROUP_GROOVE,
    FH3_GROUP_MINIMAP
} Fh3PatchGroup;

typedef struct {
    uint32_t rva;
    uint8_t size;
    Fh3PatchGroup group;
    const char *name;
    unsigned char original[8], patched[8];
} Fh3Patch;

#define VIP_TRUE {0xb8,0x01,0x00,0x00,0x00}
#define FH3_BLUR_RIM_THRESHOLD_RVA 0x1e37c8cu

/* Not const: the blur rim threshold value is filled from the INI. */
static Fh3Patch fh3_patches[] = {
    /* Each intro site was matched to its FH2 counterpart by the same
       config-flag test: FH2 +0x319b (disablesplashintro) / FH3 +0x32d8.
       Jumps keep their original targets; only the condition is removed. */
    {0x1724ab6, 6, FH3_GROUP_INTRO, "intro_resources_skip",
     {0x0f,0x85,0xf6,0x00,0x00,0x00}, {0xe9,0xf7,0x00,0x00,0x00,0x90}},
    {0x17265f0, 2, FH3_GROUP_INTRO, "intro_sequence_skip", {0x75,0x1e}, {0xeb,0x1e}},
    {0x1727e32, 2, FH3_GROUP_INTRO, "intro_complete_state", {0x75,0x0c}, {0xeb,0x0c}},
    {0x1729558, 6, FH3_GROUP_INTRO, "intro_resource_name_skip",
     {0x0f,0x85,0x80,0x00,0x00,0x00}, {0xe9,0x81,0x00,0x00,0x00,0x90}},
    /* "Main/IsVip" readers, matched to the FH2 offline VIP sites. Calls to
       the profile VIP getter return 1; the local profile setter is passed
       VIP for both its value and changed arguments. */
    {0x3499e5, 5, FH3_GROUP_VIP, "main_values_vip", {0xe8,0xb6,0x43,0xfb,0xff}, VIP_TRUE},
    {0x33c91e, 7, FH3_GROUP_VIP, "local_vip_profile",
     {0x41,0x0f,0xb6,0xe8,0x0f,0xb6,0xf2}, {0x40,0xb5,0x01,0x40,0xb6,0x01,0x90}},
    {0x3c5b74, 5, FH3_GROUP_VIP, "loaded_profile_vip", {0xe8,0x37,0x3c,0xcf,0x00}, VIP_TRUE},
    {0x992770, 5, FH3_GROUP_VIP, "wheelspin_display_vip", {0xe8,0x3b,0x70,0x72,0x00}, VIP_TRUE},
    {0x9826c8, 5, FH3_GROUP_VIP, "wheelspin_claim_vip", {0xe8,0xe3,0x70,0x73,0x00}, VIP_TRUE},
    /* Profile property getters registered for TreasureMapPurchased,
       IslandTreasureMapPurchased and OpusIslandTreasureMapPurchased. Each
       getter is referenced only by its registration, so return true at entry. */
    {0x1071870, 8, FH3_GROUP_TREASURE_MAP, "treasure_map_mainland_purchased",
     {0x48,0x83,0xec,0x28,0x48,0x8d,0x51,0x7b}, {0xb0,0x01,0xc3,0xcc,0x48,0x8d,0x51,0x7b}},
    {0x10713f0, 8, FH3_GROUP_TREASURE_MAP, "treasure_map_island_purchased",
     {0x48,0x83,0xec,0x28,0x48,0x8d,0x51,0x78}, {0xb0,0x01,0xc3,0xcc,0x48,0x8d,0x51,0x78}},
    {0x1071450, 8, FH3_GROUP_TREASURE_MAP, "treasure_map_opus_island_purchased",
     {0x48,0x83,0xec,0x28,0x48,0x8d,0x51,0x7e}, {0xb0,0x01,0xc3,0xcc,0x48,0x8d,0x51,0x7e}},
    /* Direct PlayerState field readers that bypass the getters: the map
       element reveal handler (FH2 barn_marker_reveal) and the per-region
       purchase check (FH2 map_check). */
    {0x4a53c4, 2, FH3_GROUP_TREASURE_MAP, "treasure_map_element_reveal", {0x74,0x7b}, {0x90,0x90}},
    {0x151f3f5, 8, FH3_GROUP_TREASURE_MAP, "treasure_map_region_check",
     {0x0f,0xb6,0xb4,0x24,0x80,0x00,0x00,0x00}, {0x40,0xb6,0x01,0x90,0x90,0x90,0x90,0x90}},
    /* 1.0.125.0 ships every car pack and both expansions in the base media
       and database; only ContentOffers ownership gates them. The startup reset
       "UPDATE ContentOffers SET IsPurchased=0" (single reference) becomes =1,
       as FH2's catalog_purchased does. */
    {0x2d30565, 1, FH3_GROUP_DLC, "dlc_offers_purchased", {'0'}, {'1'}},
    /* ShadowMapRendererAnthem11 vtable slot 8, as FH2's SmoothSunShadows:
       update all four cascades every frame (FH2 +0x21bc / FH3 +0x26d4 count),
       always take the new sun direction per cascade, and always store the
       refreshed focus direction instead of waiting for a large change. */
    {0x1b7d459, 6, FH3_GROUP_SUN_SHADOWS, "sun_shadow_all_cascades_per_frame",
     {0x8b,0x86,0xd4,0x26,0x00,0x00}, {0xb8,0x04,0x00,0x00,0x00,0x90}},
    {0x1b82711, 2, FH3_GROUP_SUN_SHADOWS, "sun_shadow_cascade_direction_refresh",
     {0x75,0x0d}, {0xeb,0x0d}},
    {0x1b82dcd, 2, FH3_GROUP_SUN_SHADOWS, "sun_shadow_focus_direction_refresh",
     {0x73,0x06}, {0x90,0x90}},
    /* FH3-only staggered mode (+0x26c7): the cascade count above is replaced
       by the schedule 1,0,2,0,1,0,3,0,... so the far cascade refreshes only
       every 16th frame. Its setter (0x1b80a50) now stores 0, keeping the
       +0x26cc write and moving ret into the padding, and the scheduler is
       bypassed in case the flag is set elsewhere. */
    {0x1b80a50, 8, FH3_GROUP_SUN_SHADOWS, "sun_shadow_staggered_setter_a",
     {0x88,0x91,0xc7,0x26,0x00,0x00,0x44,0x88}, {0x44,0x88,0x81,0xcc,0x26,0x00,0x00,0xc6}},
    {0x1b80a58, 8, FH3_GROUP_SUN_SHADOWS, "sun_shadow_staggered_setter_b",
     {0x81,0xcc,0x26,0x00,0x00,0xc3,0xcc,0xcc}, {0x81,0xc7,0x26,0x00,0x00,0x00,0xc3,0xcc}},
    {0x1b7d4a6, 2, FH3_GROUP_SUN_SHADOWS, "sun_shadow_staggered_schedule_bypass",
     {0x74,0x3e}, {0xeb,0x3e}},
    /* Per-wheel rim selection (0x1e37ab0) swaps the 3D rim for the flat
       blur_rim mesh when normalized wheel speed reaches BlurRim
       GeomBlendStart (0.5 of MaxRPM 350 in GlobalCarAttributes.xml). No
       shader reads g_BlurRimGeomBlend, so the swap cannot cross-fade. Read the
       threshold from the int3 padding after the function instead, filled
       from BlurRimThreshold at startup; above 1.0 the 3D rim always stays. */
    {FH3_BLUR_RIM_THRESHOLD_RVA, 4, FH3_GROUP_BLUR_RIMS, "blur_rim_threshold_value",
     {0xcc,0xcc,0xcc,0xcc}, {0x00,0x00,0x00,0x00}},
    {0x1e37b6d, 8, FH3_GROUP_BLUR_RIMS, "blur_rim_threshold_load",
     {0xc5,0xfa,0x10,0xa0,0x20,0x2d,0x00,0x00}, {0xc5,0xfa,0x10,0x25,0x17,0x01,0x00,0x00}},
    /* ForzaCar vtable slot 16: both pointers are filled at startup (the
       relocated native update and the DLL's headlight callback). */
    {FH3_CAR_UPDATE_SLOT_RVA, 8, FH3_GROUP_HEADLIGHTS, "headlight_toggle_car_update",
     {0}, {0}},
    /* Xbox Music radio: jmp [rip] thunks in int3 padding (address filled at
       startup), the nine service wrapper calls to the web service dispatcher
       and SongStream's MFCreateSourceReaderFromURL call routed through them. */
    {FH3_GROOVE_DISPATCHER_THUNK_RVA, 6, FH3_GROUP_GROOVE, "groove_dispatcher_thunk",
     {0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}, {0xff,0x25,0x00,0x00,0x00,0x00}},
    {FH3_GROOVE_DISPATCHER_THUNK_RVA + 6, 8, FH3_GROUP_GROOVE, "groove_dispatcher_target",
     {0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}, {0}},
    {FH3_GROOVE_MF_THUNK_RVA, 6, FH3_GROUP_GROOVE, "groove_reader_thunk",
     {0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}, {0xff,0x25,0x00,0x00,0x00,0x00}},
    {FH3_GROOVE_MF_THUNK_RVA + 6, 8, FH3_GROUP_GROOVE, "groove_reader_target",
     {0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}, {0}},
    {0x27b84c8, 5, FH3_GROUP_GROOVE, "groove_continue_get_playlist_tracks",
     {0xe8,0xf3,0xdb,0xfc,0xfe}, {0xe8,0xb3,0x80,0xf4,0xff}},
    {0x27b8788, 5, FH3_GROUP_GROOVE, "groove_continue_get_playlist_tracks_shuffle",
     {0xe8,0x33,0xd9,0xfc,0xfe}, {0xe8,0xf3,0x7d,0xf4,0xff}},
    {0x27b8a43, 5, FH3_GROUP_GROOVE, "groove_continue_get_playlists",
     {0xe8,0x78,0xd6,0xfc,0xfe}, {0xe8,0x38,0x7b,0xf4,0xff}},
    {0x27b8d03, 5, FH3_GROUP_GROOVE, "groove_get_playlist_tracks",
     {0xe8,0xb8,0xd3,0xfc,0xfe}, {0xe8,0x78,0x78,0xf4,0xff}},
    {0x27b8fc8, 5, FH3_GROUP_GROOVE, "groove_get_playlist_tracks_shuffle",
     {0xe8,0xf3,0xd0,0xfc,0xfe}, {0xe8,0xb3,0x75,0xf4,0xff}},
    {0x27b9230, 5, FH3_GROUP_GROOVE, "groove_get_playlists",
     {0xe8,0x8b,0xce,0xfc,0xfe}, {0xe8,0x4b,0x73,0xf4,0xff}},
    {0x27b94f6, 5, FH3_GROUP_GROOVE, "groove_get_song_url",
     {0xe8,0xc5,0xcb,0xfc,0xfe}, {0xe8,0x85,0x70,0xf4,0xff}},
    {0x27b9760, 5, FH3_GROUP_GROOVE, "groove_get_user_permissions",
     {0xe8,0x5b,0xc9,0xfc,0xfe}, {0xe8,0x1b,0x6e,0xf4,0xff}},
    {0x27b99c0, 5, FH3_GROUP_GROOVE, "groove_pretrial_opt_in",
     {0xe8,0xfb,0xc6,0xfc,0xfe}, {0xe8,0xbb,0x6b,0xf4,0xff}},
    {0x1991dab, 5, FH3_GROUP_GROOVE, "groove_song_reader",
     {0xe8,0xd6,0x09,0x02,0x00}, {0xe8,0xd0,0x9d,0xd7,0x00}},
    /* CForza's per-frame XboxMusicPlayer update tail jumps, for the state log. */
    {FH3_GROOVE_PLAYER_THUNK_RVA, 6, FH3_GROUP_GROOVE, "groove_player_thunk",
     {0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}, {0xff,0x25,0x00,0x00,0x00,0x00}},
    {FH3_GROOVE_PLAYER_THUNK_RVA + 6, 8, FH3_GROUP_GROOVE, "groove_player_target",
     {0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}, {0}},
    {0x1731b26, 5, FH3_GROUP_GROOVE, "groove_player_update_active",
     {0xe9,0x85,0x5e,0x21,0x00}, {0xe9,0xe5,0xaa,0xed,0x00}},
    {0x1731b39, 5, FH3_GROUP_GROOVE, "groove_player_update_inactive",
     {0xe9,0x72,0x5e,0x21,0x00}, {0xe9,0xd2,0xaa,0xed,0x00}},
    /* Minimap follows the camera (FH2 MinimapFollowsCamera): the tracking
       controller's minimap update call goes through a thunk to the DLL. */
    {FH3_MINIMAP_THUNK_RVA, 6, FH3_GROUP_MINIMAP, "minimap_camera_heading_thunk",
     {0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}, {0xff,0x25,0x00,0x00,0x00,0x00}},
    {FH3_MINIMAP_THUNK_RVA + 6, 8, FH3_GROUP_MINIMAP, "minimap_camera_heading_target",
     {0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}, {0}},
    {0x120b3d2, 5, FH3_GROUP_MINIMAP, "minimap_camera_heading_call",
     {0xe8,0xe9,0x12,0x00,0x00}, {0xe8,0x89,0x7e,0x3f,0x01}},
};

typedef struct {
    int enabled, skip_intro, offline_vip, unlock_treasure_map, unlock_dlc,
        smooth_sun_shadows, blur_rim_threshold, headlight_toggle, disable_azimuth_save_checksums,
        local_music_radio, minimap_follows_camera;
    Fh3LightsConfig lights;
    Fh3GrooveConfig radio;
} Fh3Options;

static const char default_ini[] =
    "; Forza Horizon 3 Extra Options (Xbox One 1.0.125.0, title 4CE1DFAB)\r\n"
    "; 1 = enabled, 0 = disabled.\r\n"
    "\r\n"
    "[Global]\r\n"
    "Enabled=1\r\n"
    "\r\n"
    "[Frontend]\r\n"
    "; Skip the developer and publisher opening videos.\r\n"
    "SkipIntro=1\r\n"
    "\r\n"
    "[Content]\r\n"
    "; VIP status: VIP wheelspins, rewards and profile flag.\r\n"
    "OfflineVIP=1\r\n"
    "; Treasure Map for the mainland and both expansions.\r\n"
    "UnlockTreasureMap=1\r\n"
    "; All car packs, Car Pass cars and both expansions.\r\n"
    "UnlockDLC=1\r\n"
    "\r\n"
    "[Graphics]\r\n"
    "; Move sun shadows smoothly: refresh every cascade each frame.\r\n"
    "SmoothSunShadows=1\r\n"
    "; Wheel speed (percent of the game's blur MaxRPM) where the spinning rim\r\n"
    "; is swapped for the flat blurred rim. 50 = game default (~15 mph),\r\n"
    "; above 100 = never swap, the real 3D rim always spins.\r\n"
    "BlurRimThreshold=200\r\n"
    "\r\n"
    "[HUD]\r\n"
    "; When the minimap rotates, turn it with the camera instead of the car.\r\n"
    "MinimapFollowsCamera=1\r\n"
    "\r\n"
    "[Vehicle]\r\n"
    "; Toggle the headlights of any car with a button of the game's controller.\r\n"
    "; The left stick is the game's horn by default: move the horn elsewhere in\r\n"
    "; the game's controller settings.\r\n"
    "HeadlightToggle=1\r\n"
    "; Button mask: 64 = left stick, 128 = right stick.\r\n"
    "HeadlightButton=64\r\n"
    "\r\n"
    "[Radio]\r\n"
    "; Bring back the Groove radio station with your own music: put songs\r\n"
    "; (mp3, m4a, aac, wma, wav, flac) in the music folder. Each subfolder\r\n"
    "; becomes a playlist; \"Artist - Title.mp3\" names show artist and title.\r\n"
    "; Turn the station on in Settings > Audio and pick a playlist there.\r\n"
    "LocalMusicRadio=1\r\n"
    "; Empty = the FH3_Music folder next to this INI.\r\n"
    "MusicFolder=\r\n"
    "; Log the station's requests and state to FH3_ExtraOptions.log.\r\n"
    "GrooveLog=1\r\n"
    "\r\n"
    "[Compatibility]\r\n"
    "; Bypass Azimuth's save size/hash rejection and rebuild missing commit markers.\r\n"
    "DisableAzimuthSaveChecksums=0\r\n";

static unsigned char hash_buffer[65536];
static wchar_t ini_path[PATH_CAPACITY], log_path[PATH_CAPACITY];

static void write_default_ini(void) {
    HANDLE file = CreateFileW(ini_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, default_ini, sizeof(default_ini) - 1, &written, NULL);
    CloseHandle(file);
}

static int read_flag(const wchar_t *section, const wchar_t *key, int fallback) {
    return GetPrivateProfileIntW(section, key, fallback, ini_path) != 0;
}

static const wchar_t *settings_directory;

static void read_options(Fh3Options *options) {
    write_default_ini();
    options->enabled = read_flag(L"Global", L"Enabled", 1);
    options->skip_intro = read_flag(L"Frontend", L"SkipIntro", 1);
    options->offline_vip = read_flag(L"Content", L"OfflineVIP", 1);
    options->unlock_treasure_map = read_flag(L"Content", L"UnlockTreasureMap", 1);
    options->unlock_dlc = read_flag(L"Content", L"UnlockDLC", 1);
    options->smooth_sun_shadows = read_flag(L"Graphics", L"SmoothSunShadows", 1);
    options->blur_rim_threshold =
        (int)GetPrivateProfileIntW(L"Graphics", L"BlurRimThreshold", 200, ini_path);
    if (options->blur_rim_threshold < 0) options->blur_rim_threshold = 0;
    if (options->blur_rim_threshold > 1000) options->blur_rim_threshold = 1000;
    options->headlight_toggle = read_flag(L"Vehicle", L"HeadlightToggle", 1);
    options->local_music_radio = read_flag(L"Radio", L"LocalMusicRadio", 1);
    options->radio.log = read_flag(L"Radio", L"GrooveLog", 1);
    GetPrivateProfileStringW(L"Radio", L"MusicFolder", L"", options->radio.folder,
                             ARRAY_COUNT(options->radio.folder), ini_path);
    size_t length = 0;
    while (options->radio.folder[length]) ++length;
    while (length && (options->radio.folder[length - 1] == '\\' || options->radio.folder[length - 1] == '/'))
        options->radio.folder[--length] = 0;
    if (!length) {
        static wchar_t folder[PATH_CAPACITY];
        if (path_join(folder, settings_directory, L"FH3_Music")) {
            for (length = 0; folder[length] && length + 1 < ARRAY_COUNT(options->radio.folder); ++length)
                options->radio.folder[length] = folder[length];
            options->radio.folder[length] = 0;
        }
    }
    options->minimap_follows_camera = read_flag(L"HUD", L"MinimapFollowsCamera", 1);
    options->lights.button_mask = GetPrivateProfileIntW(L"Vehicle", L"HeadlightButton", 0x40, ini_path);
    options->disable_azimuth_save_checksums =
        read_flag(L"Compatibility", L"DisableAzimuthSaveChecksums", 0);
}

static int group_enabled(Fh3PatchGroup group, const Fh3Options *options) {
    return group == FH3_GROUP_INTRO   ? options->skip_intro
         : group == FH3_GROUP_VIP     ? options->offline_vip
         : group == FH3_GROUP_DLC     ? options->unlock_dlc
         : group == FH3_GROUP_SUN_SHADOWS ? options->smooth_sun_shadows
         : group == FH3_GROUP_BLUR_RIMS ? options->blur_rim_threshold != 50
         : group == FH3_GROUP_HEADLIGHTS ? options->headlight_toggle
         : group == FH3_GROUP_GROOVE ? options->local_music_radio
         : group == FH3_GROUP_MINIMAP ? options->minimap_follows_camera
                                      : options->unlock_treasure_map;
}

static int verify_file(const wchar_t *path) {
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              NULL);
    if (file == INVALID_HANDLE_VALUE) {
        status_set("REFUSED: cannot read the FH3 executable for verification.");
        return 0;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart != FH3_FILE_SIZE) {
        CloseHandle(file);
        status_set("REFUSED: unsupported FH3 executable size. Only 1.0.125.0 is supported.");
        return 0;
    }
    Sha256Context context;
    sha256_initialize(&context);
    uint64_t total = 0;
    DWORD count;
    while (ReadFile(file, hash_buffer, sizeof(hash_buffer), &count, NULL) && count) {
        sha256_update(&context, hash_buffer, count);
        total += count;
    }
    CloseHandle(file);
    unsigned char digest[32];
    sha256_finalize(&context, digest);
    if (total != FH3_FILE_SIZE || !memory_equal(digest, fh3_file_sha256, 32)) {
        status_set("REFUSED: FH3 executable differs from the supported 1.0.125.0 build.");
        return 0;
    }
    return 1;
}

static int verify_image(const unsigned char *game) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    return game && memory_read(game, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
           dos.e_lfanew >= 64 && dos.e_lfanew <= 4096 &&
           memory_read(game + dos.e_lfanew, &nt, sizeof(nt)) &&
           nt.Signature == IMAGE_NT_SIGNATURE &&
           nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
           nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
           nt.FileHeader.TimeDateStamp == FH3_TIMESTAMP &&
           nt.OptionalHeader.SizeOfImage == FH3_IMAGE_SIZE;
}

static void prepare_blur_rim_threshold(const Fh3Options *options) {
    float threshold = (float)options->blur_rim_threshold / 100.0f;
    for (unsigned i = 0; i < ARRAY_COUNT(fh3_patches); ++i) {
        if (fh3_patches[i].rva == FH3_BLUR_RIM_THRESHOLD_RVA) {
            memory_copy(fh3_patches[i].patched, &threshold, sizeof(threshold));
        }
    }
}

static void prepare_headlights(unsigned char *game, const Fh3Options *options) {
    uintptr_t native = (uintptr_t)(game + FH3_CAR_UPDATE_RVA), callback = fh3_lights_callback();
    for (unsigned i = 0; i < ARRAY_COUNT(fh3_patches); ++i) {
        if (fh3_patches[i].rva == FH3_CAR_UPDATE_SLOT_RVA) {
            memory_copy(fh3_patches[i].original, &native, sizeof(native));
            memory_copy(fh3_patches[i].patched, &callback, sizeof(callback));
        }
    }
    if (options->headlight_toggle) fh3_lights_configure(game, &options->lights);
}

static void prepare_groove(unsigned char *game, const Fh3Options *options) {
    uintptr_t dispatcher = fh3_groove_dispatcher_callback(), reader = fh3_groove_reader_callback(),
              player = fh3_groove_player_callback();
    for (unsigned i = 0; i < ARRAY_COUNT(fh3_patches); ++i) {
        if (fh3_patches[i].rva == FH3_GROOVE_DISPATCHER_THUNK_RVA + 6)
            memory_copy(fh3_patches[i].patched, &dispatcher, sizeof(dispatcher));
        if (fh3_patches[i].rva == FH3_GROOVE_MF_THUNK_RVA + 6)
            memory_copy(fh3_patches[i].patched, &reader, sizeof(reader));
        if (fh3_patches[i].rva == FH3_GROOVE_PLAYER_THUNK_RVA + 6)
            memory_copy(fh3_patches[i].patched, &player, sizeof(player));
    }
    if (options->local_music_radio) fh3_groove_configure(game, &options->radio);
}

static void prepare_minimap(unsigned char *game, const Fh3Options *options) {
    uintptr_t callback = fh3_minimap_callback();
    for (unsigned i = 0; i < ARRAY_COUNT(fh3_patches); ++i) {
        if (fh3_patches[i].rva == FH3_MINIMAP_THUNK_RVA + 6)
            memory_copy(fh3_patches[i].patched, &callback, sizeof(callback));
    }
    if (options->minimap_follows_camera) fh3_minimap_configure(game);
}

static int apply_patches(unsigned char *game, const Fh3Options *options) {
    prepare_blur_rim_threshold(options);
    prepare_headlights(game, options);
    prepare_groove(game, options);
    prepare_minimap(game, options);
    /* Validate every requested site before writing any byte. Already patched
       bytes are accepted so a repeated initialization stays harmless. */
    for (unsigned i = 0; i < ARRAY_COUNT(fh3_patches); ++i) {
        const Fh3Patch *patch = &fh3_patches[i];
        unsigned char actual[8];
        if (!group_enabled(patch->group, options)) continue;
        if (!memory_read(game + patch->rva, actual, patch->size) ||
            (!memory_equal(actual, patch->original, patch->size) &&
             !memory_equal(actual, patch->patched, patch->size))) {
            status_set("REFUSED: FH3 patch site bytes differ. No bytes changed.");
            log_append("FH3: unexpected bytes at ");
            log_append(patch->name);
            log_append("\r\n");
            return 0;
        }
    }
    unsigned applied = 0;
    for (unsigned i = 0; i < ARRAY_COUNT(fh3_patches); ++i) {
        const Fh3Patch *patch = &fh3_patches[i];
        unsigned char *address = game + patch->rva;
        DWORD protection, ignored;
        if (!group_enabled(patch->group, options)) continue;
        if (!VirtualProtect(address, patch->size, PAGE_EXECUTE_READWRITE, &protection)) {
            status_set("REFUSED: cannot make an FH3 patch site writable.");
            return 0;
        }
        memory_copy(address, patch->patched, patch->size);
        VirtualProtect(address, patch->size, protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), address, patch->size);
        log_append("FH3: patched ");
        log_append(patch->name);
        log_append("\r\n");
        ++applied;
    }
    status_set(applied ? "OK: requested FH3 tweaks applied in memory."
                       : "OK: no FH3 tweaks requested.");
    return 1;
}

static void start_log(const Fh3Options *options) {
    HANDLE file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    log_set_file(log_path);
    log_append("Forza Horizon 3 Extra Options " MOD_VERSION " (FH2 Extra Options port)\r\n");
    log_append_value("Enabled=", options->enabled);
    log_append_value("SkipIntro=", options->skip_intro);
    log_append_value("OfflineVIP=", options->offline_vip);
    log_append_value("UnlockTreasureMap=", options->unlock_treasure_map);
    log_append_value("UnlockDLC=", options->unlock_dlc);
    log_append_value("SmoothSunShadows=", options->smooth_sun_shadows);
    log_append_value("BlurRimThreshold=", options->blur_rim_threshold);
    log_append_value("HeadlightToggle=", options->headlight_toggle);
    log_append_value("LocalMusicRadio=", options->local_music_radio);
    log_append_value("MinimapFollowsCamera=", options->minimap_follows_camera);
    log_append_value("HeadlightButton=", (int)options->lights.button_mask);
    log_append_value("DisableAzimuthSaveChecksums=", options->disable_azimuth_save_checksums);
}

int fh3_initialize(unsigned char *game_image, const wchar_t *executable_path,
                   const wchar_t *settings_directory_path) {
    /* Static: the radio folder makes it too large for the stack. Every field
       is (re)read from the INI by read_options. */
    static Fh3Options options;
    settings_directory = settings_directory_path;
    if (!path_join(ini_path, settings_directory_path, FH3_INI_NAME) ||
        !path_join(log_path, settings_directory_path, FH3_LOG_NAME)) {
        status_set("REFUSED: cannot resolve the FH3 settings path. No patches applied.");
        return 0;
    }
    read_options(&options);
    start_log(&options);
    if (!options.enabled) {
        status_set("INACTIVE: FH3 Extra Options disabled in FH3_ExtraOptions.ini.");
        log_append(status_get());
        log_append("\r\n");
        return 1;
    }
    int initialized = 0;
    if (!verify_image(game_image)) {
        status_set("REFUSED: Azimuth's mapped game does not match FH3 1.0.125.0. "
                   "No game patches applied.");
    } else if (verify_file(executable_path)) {
        initialized = apply_patches(game_image, &options);
    }
    log_append(status_get());
    log_append("\r\n");
#ifdef FH2_AZIMUTH
    if (initialized) {
        azimuth_apply_title_save_compatibility(options.disable_azimuth_save_checksums,
                                               FH3_TITLE_ID);
    }
#endif
    return initialized;
}
