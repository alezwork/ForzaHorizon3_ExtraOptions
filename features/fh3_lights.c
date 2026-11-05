#include "fh3_lights.h"
#include "logging.h"
#include "support.h"

/* Headlight toggle for Forza Horizon 3, ported from FH2's vehicle controls.
   FH2 field / setter            FH3
   +0x51a1 manual  (0xf74ee0)    +0x7851 (0x18ab1e0)
   +0x51a2 forced  (0xf74ef0)    +0x7852 (0x18ab1f0)
   +0x51b4 automatic             +0x7868
   The local player is game(0x4e5bb48)+0x288 -> 0x31fb40(holder, &player),
   released through vtable +0x18 exactly as the game's own callers do; the
   player's car is the ForzaCar that player owns (see owned_by).

   The button comes from the game's own controller state, so any pad the game
   sees works (including a DualSense fed in by another mod). CControllerManager
   is at 0x4e72e38 (FH2 0x365d7c8); its selected controller pointer is +0xd048
   (FH2 +0xab28) into nine 0x1720-byte controllers from +0x10 (FH2 0x1300).
   FH2 keeps an XInput-layout button word at controller +0xc0; FH3 keeps it
   at +0x80 (found in game by pressing A, B, X and Y and confirmed with L3). */

#define CAR_VTABLE_RVA 0x42e0650u
#define GAME_GLOBAL_RVA 0x4e5bb48u
#define GAME_PLAYER_OFFSET 0x288u
#define GET_PLAYER_CAR_RVA 0x31fb40u
#define SET_MANUAL_RVA 0x18ab1e0u
#define SET_FORCED_RVA 0x18ab1f0u
#define MANUAL_OFFSET 0x7851u
#define FORCED_OFFSET 0x7852u
#define AUTOMATIC_OFFSET 0x7868u
#define CONTROLLER_MANAGER_RVA 0x4e72e38u
#define SELECTED_CONTROLLER_OFFSET 0xd048u
#define CONTROLLER_ARRAY_OFFSET 0x10u
#define CONTROLLER_STRIDE 0x1720u
#define CONTROLLER_COUNT 9u
#define BUTTON_WORD_OFFSET 0x80u
#define MAX_DIAGNOSTICS 24

typedef void (*CarUpdate)(void *, float, float, unsigned);
typedef void *(*PlayerCarGetter)(void *, void **);
typedef void (*ByteSetter)(void *, unsigned char);
typedef void (*ObjectRelease)(void *);

static unsigned char *game;
static Fh3LightsConfig config;
static struct {
    void *car, *player;
    int active, enabled, held, hooked, driver_seen;
    void *reported_player;
    unsigned frame;
} state;
static unsigned diagnostics;

static void diagnose(const char *label, uintptr_t value) {
    if (diagnostics++ >= MAX_DIAGNOSTICS) return;
    static const char hex[] = "0123456789abcdef";
    char line[96];
    size_t at = 0;
    for (const char *p = "FH3 LIGHTS: "; *p; ++p) line[at++] = *p;
    for (const char *p = label; *p && at < 72; ++p) line[at++] = *p;
    line[at++] = '0';
    line[at++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) line[at++] = hex[(value >> shift) & 15];
    line[at++] = '\r';
    line[at++] = '\n';
    line[at] = 0;
    log_append(line);
}

static uintptr_t vtable_rva(void *object) {
    void *vt;
    if (!memory_read_pointer(object, &vt)) return 0;
    return (uintptr_t)vt - (uintptr_t)game;
}

/* ---- the game's controller ---------------------------------------------- */

static unsigned char *selected_controller(void) {
    unsigned char *manager, *controller;
    if (!memory_read_pointer(game + CONTROLLER_MANAGER_RVA, (void **)&manager) || !manager ||
        !memory_read_pointer(manager + SELECTED_CONTROLLER_OFFSET, (void **)&controller)) {
        return NULL;
    }
    uintptr_t first = (uintptr_t)manager + CONTROLLER_ARRAY_OFFSET, at = (uintptr_t)controller;
    if (at < first || at >= first + CONTROLLER_COUNT * CONTROLLER_STRIDE ||
        (at - first) % CONTROLLER_STRIDE) {
        return NULL;
    }
    return controller;
}

static int button_down(void) {
    unsigned char *controller = selected_controller();
    unsigned short buttons;
    if (!controller || !memory_read(controller + BUTTON_WORD_OFFSET, &buttons, sizeof(buttons))) {
        static int reported;
        if (!reported) {
            reported = 1;
            diagnose("no selected game controller, manager field ", (uintptr_t)controller);
        }
        return 0;
    }
    return (buttons & config.button_mask) != 0;
}

/* ---- headlights ---------------------------------------------------------- */

/* As FH2's get_player_car (0x2da060): the player object is the same one the
   game compares against every car's owner. FH2 reads the owner through
   0xf62270 (ForzaCar +0x4d00). In FH3 the current driver is ForzaCar +0x7360:
   0x18829e0 seats a player by calling the old driver's CPlayer::SetCar
   (vtable +0x288) and 0x18b00d0, which stores the new one there. +0x7368
   (read by 0x188e420) is only a copy of it taken at some events (0x18b66b0),
   so it is accepted as well but is not enough on its own. 0x31fb40 yields
   the local CPlayer. */
#define PLAYER_VTABLE_RVA 0x42e6998u
#define CAR_DRIVER_OFFSET 0x7360u
#define CAR_OWNER_OFFSET 0x7368u

static void *local_player(void) {
    void *instance, *holder, *player = NULL;
    if (!memory_read_pointer(game + GAME_GLOBAL_RVA, &instance) ||
        !memory_read_pointer((unsigned char *)instance + GAME_PLAYER_OFFSET, &holder) || !holder) {
        return NULL;
    }
    ((PlayerCarGetter)(game + GET_PLAYER_CAR_RVA))(holder, &player);
    if (!player) return NULL;
    void *vt;
    ObjectRelease release;
    if (memory_read_pointer(player, &vt) && memory_read((unsigned char *)vt + 0x18, &release, sizeof(release))) {
        /* Only the pointer is kept: the player outlives this frame's update. */
        release(player);
    }
    if (vtable_rva(player) != PLAYER_VTABLE_RVA) {
        static int reported;
        if (!reported) {
            reported = 1;
            diagnose("local player is not a CPlayer, vtable +", vtable_rva(player));
        }
        return NULL;
    }
    return player;
}

static int owned_by(void *car, void *player) {
    void *driver = NULL, *owner = NULL;
    memory_read_pointer((unsigned char *)car + CAR_DRIVER_OFFSET, &driver);
    memory_read_pointer((unsigned char *)car + CAR_OWNER_OFFSET, &owner);
    if (driver && !state.driver_seen) {
        /* One line proving the hook runs and what the cars hold. */
        state.driver_seen = 1;
        diagnose("first driven car, driver vtable +", vtable_rva(driver));
    }
    return player && (driver == player || owner == player);
}

static void apply(void *car) {
    ((ByteSetter)(game + SET_FORCED_RVA))(car, (unsigned char)state.enabled);
    ((ByteSetter)(game + SET_MANUAL_RVA))(car, 1);
}

static void toggle(void *car) {
    unsigned char manual, forced, automatic;
    unsigned char *bytes = car;
    if (!memory_read(bytes + MANUAL_OFFSET, &manual, 1) || !memory_read(bytes + FORCED_OFFSET, &forced, 1) ||
        !memory_read(bytes + AUTOMATIC_OFFSET, &automatic, 1)) {
        return;
    }
    int current = state.active && state.car == car ? state.enabled : manual ? forced : automatic;
    state.car = car;
    state.active = 1;
    state.enabled = !current;
    apply(car);
    diagnose(state.enabled ? "headlights on, car " : "headlights off, car ", (uintptr_t)car);
}

static void car_update(void *car, float dt, float frame_dt, unsigned frame) {
    ((CarUpdate)(game + FH3_CAR_UPDATE_RVA))(car, dt, frame_dt, frame);
    if (!state.hooked) {
        state.hooked = 1;
        diagnose("car update hook running, car ", (uintptr_t)car);
    }
    if (frame != state.frame) {
        /* Resolve the local player once per frame, not once per car. */
        state.frame = frame;
        state.player = local_player();
        if (state.player != state.reported_player) {
            state.reported_player = state.player;
            diagnose("local player ", (uintptr_t)state.player);
        }
    }
    if (!owned_by(car, state.player)) return;
    if (state.car != car) {
        /* Another car: the game's own automatic lights take over again. */
        diagnose("player car ", (uintptr_t)car);
        state.active = 0;
        state.car = car;
    }
    int down = button_down();
    if (down && !state.held) toggle(car);
    state.held = down;
    if (state.active) apply(car);
}

void fh3_lights_configure(unsigned char *game_image, const Fh3LightsConfig *settings) {
    game = game_image;
    config = *settings;
    state.car = state.player = NULL;
    state.active = state.held = 0;
    diagnostics = 0;
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCWSTR)(uintptr_t)car_update, &pinned);
}

uintptr_t fh3_lights_callback(void) {
    return (uintptr_t)car_update;
}
