#ifndef HEX_H
#define HEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "params.h"

#define HEX_CORNER_COUNT 6

typedef enum HexTerrain {
    HEX_TERRAIN_OPEN = 0,
    HEX_TERRAIN_FOREST = 1,
    HEX_TERRAIN_MOUNTAIN = 2,
    HEX_TERRAIN_WATER = 3,
    HEX_TERRAIN_HIVE = 4,
    HEX_TERRAIN_FLOWERS = 5,
    HEX_TERRAIN_ENTRANCE = 6,
    HEX_TERRAIN_COUNT
} HexTerrain;

enum {
    HEX_TILE_VISIBLE = 0x01
};

typedef struct HexTile {
    int16_t q;
    int16_t r;
    uint8_t terrain;
    float nectar_stock;
    float nectar_capacity;
    float nectar_recharge_rate;
    float flow_capacity;
    float center_x;
    float center_y;
    uint8_t flags;
} HexTile;

typedef struct HexWorld {
    float cell_size;
    float origin_x;
    float origin_y;
    int16_t q_min;
    int16_t q_max;
    int16_t r_min;
    int16_t r_max;
    uint16_t width;
    uint16_t height;
    size_t count;
    HexTile *tiles;
} HexWorld;

void hex_world_init(HexWorld *world);
bool hex_world_create(HexWorld *world, const Params *params);
void hex_world_destroy(HexWorld *world);

bool hex_world_in_bounds(const HexWorld *world, int q, int r);
size_t hex_world_index(const HexWorld *world, int q, int r);
HexTile *hex_world_tile_mut(HexWorld *world, int q, int r);
const HexTile *hex_world_tile(const HexWorld *world, int q, int r);

void hex_axial_to_world(const HexWorld *world, int q, int r, float *out_x, float *out_y);
void hex_world_to_axial_f(const HexWorld *world, float world_x, float world_y, float *out_qf, float *out_rf);
void hex_axial_round(float qf, float rf, int *out_q, int *out_r);
bool hex_world_pick(const HexWorld *world, float world_x, float world_y, int *out_q, int *out_r, size_t *out_index);

void hex_corner_offset(float cell_size, int corner_index, float *out_dx, float *out_dy);
void hex_tile_palette(uint8_t terrain, float *out_rgba);

#endif  // HEX_H
