#include "fh3.h"
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
    FH3_GROUP_SUN_SHADOWS, FH3_GROUP_BLUR_RIMS
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
};

typedef struct {
    int enabled, skip_intro, offline_vip, unlock_treasure_map, unlock_dlc,
        smooth_sun_shadows, blur_rim_threshold, disable_azimuth_save_checksums;
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
    options->disable_azimuth_save_checksums =
        read_flag(L"Compatibility", L"DisableAzimuthSaveChecksums", 0);
}

static int group_enabled(Fh3PatchGroup group, const Fh3Options *options) {
    return group == FH3_GROUP_INTRO   ? options->skip_intro
         : group == FH3_GROUP_VIP     ? options->offline_vip
         : group == FH3_GROUP_DLC     ? options->unlock_dlc
         : group == FH3_GROUP_SUN_SHADOWS ? options->smooth_sun_shadows
         : group == FH3_GROUP_BLUR_RIMS ? options->blur_rim_threshold != 50
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

static int apply_patches(unsigned char *game, const Fh3Options *options) {
    prepare_blur_rim_threshold(options);
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
    log_append_value("DisableAzimuthSaveChecksums=", options->disable_azimuth_save_checksums);
}

int fh3_initialize(unsigned char *game_image, const wchar_t *executable_path,
                   const wchar_t *settings_directory) {
    Fh3Options options = {1, 1, 1, 1, 1, 1, 200, 0};
    if (!path_join(ini_path, settings_directory, FH3_INI_NAME) ||
        !path_join(log_path, settings_directory, FH3_LOG_NAME)) {
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
