#ifndef HEX_DRAW_H
#define HEX_DRAW_H

#include <stdbool.h>
#include <stddef.h>

#include "hex.h"
#include "render_hex.h"

typedef struct HexDrawContext HexDrawContext;

bool hex_draw_init(HexDrawContext **out_ctx);
void hex_draw_shutdown(HexDrawContext **ctx);
bool hex_draw_render(HexDrawContext *ctx,
                     const RenderHexParams *params,
                     int fb_width,
                     int fb_height,
                     const float cam_center[2],
                     float cam_zoom);

#endif  // HEX_DRAW_H
