#ifndef UI_H
#define UI_H

#include <stdbool.h>

#include "hex.h"
#include "params.h"
#include "platform.h"
#include "render.h"
#include "sim.h"

typedef struct UiActions {
    bool toggle_pause;
    bool step_once;
    bool apply;
    bool reset;
    bool reinit_required;
    bool focus_queen;
    bool toggle_hex_grid;
    bool toggle_hex_layer;
} UiActions;

void ui_init(void);
void ui_shutdown(void);
void ui_sync_to_params(const Params *baseline, Params *runtime);
UiActions ui_update(const Input *input, bool sim_paused, float dt_sec);
void ui_render(int framebuffer_width, int framebuffer_height);
bool ui_wants_mouse(void);
bool ui_wants_keyboard(void);
void ui_set_viewport(const RenderCamera *camera, int framebuffer_width, int framebuffer_height);
void ui_enable_hive_overlay(bool enabled);
void ui_set_selected_bee(const BeeDebugInfo *info, bool valid);
void ui_set_hex_overlay(bool show_grid, bool draw_on_top);
void ui_set_selected_hex(const HexTile *tile, bool valid);

#endif  // UI_H
