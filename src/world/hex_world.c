#include "hex.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

#ifndef HEX_PI
#define HEX_PI 3.14159265358979323846f
#endif

#ifndef SQRT3
#define SQRT3 1.7320508075688772f
#endif

static float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float pseudo_noise01(int q, int r) {
    uint32_t h = (uint32_t)q * 73856093u ^ (uint32_t)r * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
}

void hex_world_init(HexWorld *world) {
    if (!world) {
        return;
    }
    memset(world, 0, sizeof(*world));
}

void hex_world_destroy(HexWorld *world) {
    if (!world) {
        return;
    }
    free(world->tiles);
    memset(world, 0, sizeof(*world));
}

static void assign_tile_defaults(const Params *params, HexWorld *world, HexTile *tile) {
    if (!tile) {
        return;
    }
    tile->terrain = HEX_TERRAIN_OPEN;
    tile->nectar_capacity = 0.0f;
    tile->nectar_stock = 0.0f;
    tile->nectar_recharge_rate = 0.0f;
    tile->flow_capacity = 8.0f;
    tile->flags = HEX_TILE_VISIBLE;

    if (!params) {
        return;
    }

    const float rect_x = params->hive.rect_x;
    const float rect_y = params->hive.rect_y;
    const float rect_w = params->hive.rect_w;
    const float rect_h = params->hive.rect_h;
    const bool hive_enabled = rect_w > 0.0f && rect_h > 0.0f;

    if (hive_enabled) {
        if (tile->center_x >= rect_x && tile->center_x <= rect_x + rect_w &&
            tile->center_y >= rect_y && tile->center_y <= rect_y + rect_h) {
            tile->terrain = HEX_TERRAIN_HIVE;
            tile->flow_capacity = 35.0f;
            return;
        }

        float entrance_x = rect_x;
        float entrance_y = rect_y;
        const float entrance_half = params->hive.entrance_width * 0.5f;
        switch (params->hive.entrance_side) {
            case 0:
                entrance_x = rect_x + clampf(params->hive.entrance_t, 0.0f, 1.0f) * rect_w;
                entrance_y = rect_y;
                break;
            case 1:
                entrance_x = rect_x + clampf(params->hive.entrance_t, 0.0f, 1.0f) * rect_w;
                entrance_y = rect_y + rect_h;
                break;
            case 2:
                entrance_x = rect_x;
                entrance_y = rect_y + clampf(params->hive.entrance_t, 0.0f, 1.0f) * rect_h;
                break;
            case 3:
                entrance_x = rect_x + rect_w;
                entrance_y = rect_y + clampf(params->hive.entrance_t, 0.0f, 1.0f) * rect_h;
                break;
            default:
                break;
        }

        float dx = tile->center_x - entrance_x;
        float dy = tile->center_y - entrance_y;
        float entrance_dist = sqrtf(dx * dx + dy * dy);
        float axial_extent = params->hive.entrance_width > 0.0f ? params->hive.entrance_width * 0.5f : world->cell_size;
        float radial_limit = fmaxf(world->cell_size * 1.2f, entrance_half);

        bool axial_ok = false;
        if (params->hive.entrance_side == 0 || params->hive.entrance_side == 1) {
            axial_ok = fabsf(dx) <= axial_extent;
        } else {
            axial_ok = fabsf(dy) <= axial_extent;
        }
        if (axial_ok && entrance_dist <= radial_limit) {
            tile->terrain = HEX_TERRAIN_ENTRANCE;
            tile->flow_capacity = 26.0f;
            return;
        }
    }

    float local_x = tile->center_x - world->origin_x;
    float local_y = tile->center_y - world->origin_y;
    float dist = sqrtf(local_x * local_x + local_y * local_y);
    float noise = pseudo_noise01(tile->q, tile->r);

    if (dist > world->cell_size * 8.0f) {
        if (noise > 0.68f) {
            tile->terrain = HEX_TERRAIN_FLOWERS;
            tile->nectar_capacity = 240.0f + 60.0f * noise;
            tile->nectar_stock = tile->nectar_capacity * clampf(0.55f + 0.4f * (noise - 0.68f), 0.35f, 0.95f);
            tile->nectar_recharge_rate = 4.5f + 2.0f * (noise - 0.68f);
            tile->flow_capacity = 18.0f;
            return;
        }
    }

    if (noise < 0.04f) {
        tile->terrain = HEX_TERRAIN_WATER;
        tile->flow_capacity = 2.0f;
        return;
    }
    if (noise < 0.08f) {
        tile->terrain = HEX_TERRAIN_MOUNTAIN;
        tile->flow_capacity = 1.5f;
        return;
    }
    if (noise < 0.18f) {
        tile->terrain = HEX_TERRAIN_FOREST;
        tile->flow_capacity = 6.0f;
        tile->nectar_capacity = 30.0f;
        tile->nectar_stock = tile->nectar_capacity * 0.3f;
        tile->nectar_recharge_rate = 0.8f;
        return;
    }
}

bool hex_world_create(HexWorld *world, const Params *params) {
    if (!world) {
        return false;
    }
    hex_world_destroy(world);
    hex_world_init(world);
    if (!params || !params->hex.enabled) {
        return true;
    }
    const int q_min = params->hex.q_min;
    const int q_max = params->hex.q_max;
    const int r_min = params->hex.r_min;
    const int r_max = params->hex.r_max;
    const int q_span = q_max - q_min + 1;
    const int r_span = r_max - r_min + 1;
    if (q_span <= 0 || r_span <= 0) {
        LOG_ERROR("hex_world_create: invalid bounds (%d..%d, %d..%d)", q_min, q_max, r_min, r_max);
        return false;
    }
    world->cell_size = params->hex.cell_size;
    world->origin_x = params->hex.origin_x;
    world->origin_y = params->hex.origin_y;
    world->q_min = (int16_t)q_min;
    world->q_max = (int16_t)q_max;
    world->r_min = (int16_t)r_min;
    world->r_max = (int16_t)r_max;
    world->width = (uint16_t)q_span;
    world->height = (uint16_t)r_span;
    size_t count = (size_t)q_span * (size_t)r_span;
    world->count = count;

    if (count == 0) {
        return true;
    }

    world->tiles = (HexTile *)calloc(count, sizeof(HexTile));
    if (!world->tiles) {
        LOG_ERROR("hex_world_create: failed to allocate %zu tiles", count);
        return false;
    }

    size_t index = 0;
    for (int r = r_min; r <= r_max; ++r) {
        for (int q = q_min; q <= q_max; ++q) {
            HexTile *tile = &world->tiles[index++];
            tile->q = (int16_t)q;
            tile->r = (int16_t)r;
            float center_x = (SQRT3 * world->cell_size) * ((float)q + (float)r * 0.5f);
            float center_y = (1.5f * world->cell_size) * (float)r;
            tile->center_x = world->origin_x + center_x;
            tile->center_y = world->origin_y + center_y;
            assign_tile_defaults(params, world, tile);
        }
    }
    return true;
}

bool hex_world_in_bounds(const HexWorld *world, int q, int r) {
    if (!world || !world->tiles) {
        return false;
    }
    return q >= world->q_min && q <= world->q_max && r >= world->r_min && r <= world->r_max;
}

size_t hex_world_index(const HexWorld *world, int q, int r) {
    if (!hex_world_in_bounds(world, q, r)) {
        return SIZE_MAX;
    }
    size_t qi = (size_t)(q - world->q_min);
    size_t ri = (size_t)(r - world->r_min);
    return ri * (size_t)world->width + qi;
}

HexTile *hex_world_tile_mut(HexWorld *world, int q, int r) {
    size_t index = hex_world_index(world, q, r);
    if (index == SIZE_MAX || !world) {
        return NULL;
    }
    return &world->tiles[index];
}

const HexTile *hex_world_tile(const HexWorld *world, int q, int r) {
    size_t index = hex_world_index(world, q, r);
    if (index == SIZE_MAX || !world) {
        return NULL;
    }
    return &world->tiles[index];
}

void hex_axial_to_world(const HexWorld *world, int q, int r, float *out_x, float *out_y) {
    if (!world || !world->tiles) {
        if (out_x) *out_x = 0.0f;
        if (out_y) *out_y = 0.0f;
        return;
    }
    float x = (SQRT3 * world->cell_size) * ((float)q + (float)r * 0.5f) + world->origin_x;
    float y = (1.5f * world->cell_size) * (float)r + world->origin_y;
    if (out_x) {
        *out_x = x;
    }
    if (out_y) {
        *out_y = y;
    }
}

void hex_world_to_axial_f(const HexWorld *world, float world_x, float world_y, float *out_qf, float *out_rf) {
    if (!world || world->cell_size <= 0.0f) {
        if (out_qf) *out_qf = 0.0f;
        if (out_rf) *out_rf = 0.0f;
        return;
    }
    float x = world_x - world->origin_x;
    float y = world_y - world->origin_y;
    float qf = (SQRT3 / 3.0f * x - 1.0f / 3.0f * y) / world->cell_size;
    float rf = (2.0f / 3.0f * y) / world->cell_size;
    if (out_qf) {
        *out_qf = qf;
    }
    if (out_rf) {
        *out_rf = rf;
    }
}

void hex_axial_round(float qf, float rf, int *out_q, int *out_r) {
    float sf = -qf - rf;
    int q = (int)lrintf(qf);
    int r = (int)lrintf(rf);
    int s = (int)lrintf(sf);

    float q_diff = fabsf((float)q - qf);
    float r_diff = fabsf((float)r - rf);
    float s_diff = fabsf((float)s - sf);

    if (q_diff > r_diff && q_diff > s_diff) {
        q = -r - s;
    } else if (r_diff > s_diff) {
        r = -q - s;
    }

    if (out_q) {
        *out_q = q;
    }
    if (out_r) {
        *out_r = r;
    }
}

bool hex_world_pick(const HexWorld *world, float world_x, float world_y, int *out_q, int *out_r, size_t *out_index) {
    if (!world || !world->tiles) {
        return false;
    }
    float qf = 0.0f;
    float rf = 0.0f;
    hex_world_to_axial_f(world, world_x, world_y, &qf, &rf);
    int q = 0;
    int r = 0;
    hex_axial_round(qf, rf, &q, &r);

    if (!hex_world_in_bounds(world, q, r)) {
        return false;
    }
    size_t index = hex_world_index(world, q, r);
    if (index == SIZE_MAX) {
        return false;
    }
    if (out_q) {
        *out_q = q;
    }
    if (out_r) {
        *out_r = r;
    }
    if (out_index) {
        *out_index = index;
    }
    return true;
}

void hex_corner_offset(float cell_size, int corner_index, float *out_dx, float *out_dy) {
    float angle_deg = 60.0f * (float)corner_index - 30.0f;
    float angle_rad = angle_deg * (HEX_PI / 180.0f);
    float dx = cell_size * cosf(angle_rad);
    float dy = cell_size * sinf(angle_rad);
    if (out_dx) {
        *out_dx = dx;
    }
    if (out_dy) {
        *out_dy = dy;
    }
}

void hex_tile_palette(uint8_t terrain, float *out_rgba) {
    static const float palette[HEX_TERRAIN_COUNT][4] = {
        {0.80f, 0.82f, 0.85f, 0.65f},  // OPEN
        {0.25f, 0.56f, 0.32f, 0.80f},  // FOREST
        {0.50f, 0.40f, 0.32f, 0.80f},  // MOUNTAIN
        {0.22f, 0.45f, 0.85f, 0.75f},  // WATER
        {0.90f, 0.74f, 0.24f, 0.90f},  // HIVE
        {0.94f, 0.54f, 0.74f, 0.85f},  // FLOWERS
        {0.35f, 0.90f, 0.95f, 0.85f}   // ENTRANCE
    };
    const size_t idx = terrain < HEX_TERRAIN_COUNT ? terrain : 0u;
    if (!out_rgba) {
        return;
    }
    out_rgba[0] = palette[idx][0];
    out_rgba[1] = palette[idx][1];
    out_rgba[2] = palette[idx][2];
    out_rgba[3] = palette[idx][3];
}
