#ifndef RENDER_HEX_H
#define RENDER_HEX_H

#include <stdbool.h>
#include <stddef.h>

#include "hex.h"
#include "render.h"

typedef struct RenderHexParams {
    const HexWorld *world;
    size_t selected_index;
    bool enabled;
    bool draw_on_top;
} RenderHexParams;

void render_hex_set(Render *render, const RenderHexParams *params);

#endif  // RENDER_HEX_H
