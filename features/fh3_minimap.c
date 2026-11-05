#include "fh3_minimap.h"
#include "logging.h"
#include "support.h"

/* "Minimap follows camera", ported from FH2's features/minimap.c: when the
   minimap rotates, it turns with the camera's look direction instead of the
   car's heading, so looking around with the orbit camera turns the map too.

   FH2                                     FH3
   game(0x34aba60) +0x48 +0x98 simulation  CForza(0x4e5bb48) +0x58
                                           ForzaSimulationManager, std::list of
                                           shared_ptr<ForzaSimulation> at +0x8
                                           (walked the same way by 0x172faed)
   simulation +0x2d0 views                 Simulation +0x80 views (0x193dcfe)
   SplitView/MultiCamView +0xc0 camera     +0x100 (slot 8, 0x3e5400)
   Camera::ForzaMultiCam +0x260 matrix     +0x280 (slot 10, 0x18eeb80)
   rotation test 0xbadff0                  0x120efd0 */

#define GAME_GLOBAL_RVA 0x4e5bb48u
#define SIMULATION_MANAGER_OFFSET 0x58u
#define SIMULATION_LIST_OFFSET 0x8u
#define FORZA_SIMULATION_VTABLE_RVA 0x42e0a78u
#define VIEWS_OFFSET 0x80u
#define SPLIT_VIEW_VTABLE_RVA 0x42e9858u
#define MULTICAM_VIEW_VTABLE_RVA 0x462f9e0u
#define VIEW_CAMERA_OFFSET 0x100u
#define MULTICAM_VTABLE_RVA 0x42ef2b0u
#define CAMERA_MATRIX_OFFSET 0x280u
#define MAX_SIMULATIONS 8

typedef void (*MapUpdate)(void *, const float *, const float *, float, float);
typedef unsigned char (*MapRotates)(void *);

static unsigned char *game;

static int has_vtable(void *object, unsigned rva) {
    void *vtable;
    return object && memory_read_pointer(object, &vtable) && vtable == game + rva;
}

static int simulation_heading(void *simulation, float direction[4]) {
    void *views, *view, *camera;
    float matrix[16];
    if (!has_vtable(simulation, FORZA_SIMULATION_VTABLE_RVA) ||
        !memory_read_pointer((unsigned char *)simulation + VIEWS_OFFSET, &views) || !views ||
        !memory_read_pointer(views, &view) ||
        (!has_vtable(view, SPLIT_VIEW_VTABLE_RVA) && !has_vtable(view, MULTICAM_VIEW_VTABLE_RVA)) ||
        !memory_read_pointer((unsigned char *)view + VIEW_CAMERA_OFFSET, &camera) ||
        !has_vtable(camera, MULTICAM_VTABLE_RVA) ||
        !memory_read((unsigned char *)camera + CAMERA_MATRIX_OFFSET, matrix, sizeof(matrix))) {
        return 0;
    }
    /* As in FH2, the cached view matrix stores the look direction in its
       third column. */
    float x = matrix[2], y = matrix[6], z = matrix[10];
    if (!float_is_finite(x) || !float_is_finite(y) || !float_is_finite(z)) return 0;
    float horizontal = x * x + z * z, length = horizontal + y * y;
    if (!(horizontal > 0.000001f && length > 0.5f && length < 1.5f)) return 0;
    direction[0] = x;
    direction[1] = 0;
    direction[2] = z;
    direction[3] = 0;
    return 1;
}

static int camera_heading(float direction[4]) {
    void *forza, *manager, *head, *node;
    if (!memory_read_pointer(game + GAME_GLOBAL_RVA, &forza) || !forza ||
        !memory_read_pointer((unsigned char *)forza + SIMULATION_MANAGER_OFFSET, &manager) || !manager ||
        !memory_read_pointer((unsigned char *)manager + SIMULATION_LIST_OFFSET, &head) || !head ||
        !memory_read_pointer(head, &node)) {
        return 0;
    }
    for (unsigned count = 0; node && node != head && count < MAX_SIMULATIONS; ++count) {
        void *simulation;
        if (memory_read_pointer((unsigned char *)node + 0x10, &simulation) &&
            simulation_heading(simulation, direction)) {
            return 1;
        }
        if (!memory_read_pointer(node, &node)) break;
    }
    return 0;
}

static void update_minimap(void *controller, const float *position, const float *direction,
                           float speed, float zoom) {
    _Alignas(16) float heading[4];
    if (((MapRotates)(game + FH3_MINIMAP_ROTATES_RVA))(controller) && camera_heading(heading)) {
        static int reported;
        if (!reported) {
            reported = 1;
            log_append("FH3 MINIMAP: following the camera heading\r\n");
        }
        direction = heading;
    }
    ((MapUpdate)(game + FH3_MINIMAP_UPDATE_RVA))(controller, position, direction, speed, zoom);
}

void fh3_minimap_configure(unsigned char *game_image) {
    game = game_image;
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCWSTR)(uintptr_t)update_minimap, &pinned);
}

uintptr_t fh3_minimap_callback(void) {
    return (uintptr_t)update_minimap;
}
