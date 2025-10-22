#include "ui.h"

#include <glad/glad.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

#define UI_PANEL_WIDTH 320.0f
#define UI_PANEL_MARGIN 16.0f
#define UI_HAMBURGER_SIZE 28.0f
#define UI_SLIDER_HEIGHT 18.0f
#define UI_SLIDER_SPACING 40.0f
#define UI_FONT_SCALE 2.0f
#define UI_CHAR_WIDTH (5.0f * UI_FONT_SCALE)
#define UI_CHAR_HEIGHT (7.0f * UI_FONT_SCALE)
#define UI_CHAR_ADVANCE (UI_CHAR_WIDTH + UI_FONT_SCALE)

typedef struct {
    float x, y, w, h;
} UiRect;

typedef struct {
    float r, g, b, a;
} UiColor;

typedef struct {
    const char *label;
    float min_value;
    float max_value;
    float step;
    float *value;
    int id;
} SliderSpec;

static bool ui_rect_contains(const UiRect *rect, float px, float py);
static size_t ui_add_rect(float x, float y, float w, float h, UiColor color);
static float ui_measure_text(const char *text);
static void ui_draw_text(float x, float y, const char *text, UiColor color);

static float ui_clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

typedef struct {
    float x, y;
    float r, g, b, a;
} UiVertex;

typedef struct {
    char ch;
    unsigned char rows[7];
} UiGlyph;

typedef struct {
    bool panel_open;
    bool mouse_over_panel;
    bool capturing_mouse;
    bool prev_mouse_down;
    int active_slider;
    bool dirty;
    bool reinit_required;
    bool has_params;
    bool sim_paused;
    float mouse_x;
    float mouse_y;
    Params baseline;
    Params *runtime;

    UiVertex *vertices;
    size_t vert_count;
    size_t vert_capacity;

    bool wants_mouse;
    bool wants_keyboard;

    bool action_toggle_pause;
    bool action_step;
    bool action_apply;
    bool action_reset;
    bool action_reinit;
    bool action_focus_queen;
    bool action_toggle_hex_grid;
    bool action_toggle_hex_layer;

    GLuint program;
    GLuint vao;
    GLuint vbo;
    GLint resolution_uniform;

    bool show_hive_overlay;
    bool hex_show_grid;
    bool hex_draw_on_top;
    bool has_camera;
    float cam_center_x;
    float cam_center_y;
    float cam_zoom;
    int fb_width;
    int fb_height;
    bool selected_valid;
    bool selected_panel_open;
    BeeDebugInfo selected_bee;
    bool hex_selected_valid;
    HexTile hex_selected_tile;
    float panel_scroll;
    float panel_content_height;
    float panel_visible_height;
    float panel_last_width;
} UiState;

static UiState g_ui;

static const char *const UI_VERTEX_SHADER =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec4 a_color;\n"
    "out vec4 v_color;\n"
    "uniform vec2 u_resolution;\n"
    "void main(){\n"
    "    vec2 ndc = vec2((a_pos.x / u_resolution.x)*2.0 - 1.0, 1.0 - (a_pos.y / u_resolution.y)*2.0);\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *const UI_FRAGMENT_SHADER =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "out vec4 frag_color;\n"
    "void main(){\n"
    "    frag_color = v_color;\n"
    "}\n";

static UiColor ui_color_rgba(float r, float g, float b, float a) {
    UiColor c = {r, g, b, a};
    return c;
}

static const char *ui_hex_terrain_name(uint8_t terrain) {
    switch (terrain) {
        case HEX_TERRAIN_OPEN:
            return "OPEN";
        case HEX_TERRAIN_FOREST:
            return "FOREST";
        case HEX_TERRAIN_MOUNTAIN:
            return "MOUNTAIN";
        case HEX_TERRAIN_WATER:
            return "WATER";
        case HEX_TERRAIN_HIVE:
            return "HIVE";
        case HEX_TERRAIN_FLOWERS:
            return "FLOWERS";
        case HEX_TERRAIN_ENTRANCE:
            return "ENTRANCE";
        default:
            return "UNKNOWN";
    }
}

static bool ui_range_intersects(float y, float h, float top, float bottom);

static float ui_draw_slider_group(const SliderSpec *sliders,
                                  size_t slider_count,
                                  float text_x,
                                  float slider_width,
                                  float cursor_y,
                                  UiColor text_color,
                                  float *panel_max_x,
                                  bool mouse_pressed,
                                  bool mouse_down,
                                  float scroll,
                                  float view_top,
                                  float view_bottom) {
    for (size_t i = 0; i < slider_count; ++i) {
        const SliderSpec *spec = &sliders[i];
        float label_y = cursor_y - scroll;
        if (ui_range_intersects(label_y, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(text_x, label_y, spec->label, text_color);
        }
        if (panel_max_x) {
            float text_width = ui_measure_text(spec->label);
            if (text_x + text_width > *panel_max_x) {
                *panel_max_x = text_x + text_width;
            }
        }
        UiRect slider_rect = {text_x, cursor_y + 18.0f - scroll, slider_width, UI_SLIDER_HEIGHT};
        bool slider_visible = ui_range_intersects(slider_rect.y, slider_rect.h, view_top, view_bottom);
        bool hovered = slider_visible && ui_rect_contains(&slider_rect, g_ui.mouse_x, g_ui.mouse_y);
        if (slider_visible) {
            ui_add_rect(slider_rect.x, slider_rect.y, slider_rect.w, slider_rect.h,
                        ui_color_rgba(0.15f, 0.15f, 0.18f, 0.95f));
        }
        if (panel_max_x) {
            float edge = slider_rect.x + slider_rect.w;
            if (edge > *panel_max_x) {
                *panel_max_x = edge;
            }
        }

        float range = spec->max_value - spec->min_value;
        float ratio = (range > 0.0f) ? ((*spec->value - spec->min_value) / range) : 0.0f;
        ratio = ui_clampf(ratio, 0.0f, 1.0f);
        float fill_w = slider_rect.w * ratio;
        UiColor track = hovered ? ui_color_rgba(0.2f, 0.4f, 0.7f, 1.0f)
                                : ui_color_rgba(0.25f, 0.25f, 0.3f, 1.0f);
        if (slider_visible) {
            ui_add_rect(slider_rect.x, slider_rect.y, fill_w, slider_rect.h, track);
        }
        float knob_x = slider_rect.x + fill_w - 6.0f;
        if (slider_visible) {
            ui_add_rect(knob_x, slider_rect.y - 2.0f, 12.0f, slider_rect.h + 4.0f,
                        ui_color_rgba(0.9f, 0.9f, 0.9f, 1.0f));
        }

        bool active = g_ui.active_slider == spec->id;
        if (hovered && mouse_pressed) {
            g_ui.active_slider = spec->id;
            g_ui.capturing_mouse = true;
            active = true;
        }
        if (active) {
            if (mouse_down) {
                float t = (g_ui.mouse_x - slider_rect.x) / slider_rect.w;
                t = ui_clampf(t, 0.0f, 1.0f);
                float new_value = spec->min_value + t * range;
                if (spec->step > 0.0f && range > 0.0f) {
                    float steps = roundf((new_value - spec->min_value) / spec->step);
                    new_value = spec->min_value + steps * spec->step;
                }
                new_value = ui_clampf(new_value, spec->min_value, spec->max_value);
                if (fabsf(new_value - *spec->value) > 0.0001f) {
                    *spec->value = new_value;
                }
            } else {
                g_ui.active_slider = -1;
                g_ui.capturing_mouse = false;
            }
        }

        if (spec->value == &g_ui.runtime->motion_spawn_speed_mean) {
            float min_allowed = g_ui.runtime->motion_min_speed;
            float max_allowed = g_ui.runtime->motion_max_speed;
            if (max_allowed < min_allowed) {
                max_allowed = min_allowed;
            }
            *spec->value = ui_clampf(*spec->value, min_allowed, max_allowed);
        } else if (spec->value == &g_ui.runtime->motion_spawn_speed_std) {
            if (*spec->value < 0.0f) {
                *spec->value = 0.0f;
            }
        }

        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.1f", *spec->value);
        float value_x = slider_rect.x + slider_rect.w + 10.0f;
        float value_y = slider_rect.y - 2.0f;
        if (ui_range_intersects(value_y, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(value_x, value_y, buffer, text_color);
        }
        if (panel_max_x) {
            float val_width = ui_measure_text(buffer);
            float edge = value_x + val_width;
            if (edge > *panel_max_x) {
                *panel_max_x = edge;
            }
        }
        cursor_y += UI_SLIDER_SPACING;
    }
    return cursor_y;
}

static bool ui_rect_contains(const UiRect *rect, float px, float py) {
    return px >= rect->x && px <= rect->x + rect->w && py >= rect->y && py <= rect->y + rect->h;
}

static bool ui_range_intersects(float y, float h, float top, float bottom) {
    float min_y = y;
    float max_y = y + h;
    return max_y >= top && min_y <= bottom;
}

static void ui_reserve_vertices(size_t additional) {
    if (g_ui.vert_count + additional <= g_ui.vert_capacity) {
        return;
    }
    size_t new_capacity = g_ui.vert_capacity ? g_ui.vert_capacity : 1024;
    while (g_ui.vert_count + additional > new_capacity) {
        new_capacity *= 2;
    }
    UiVertex *new_vertices = (UiVertex *)realloc(g_ui.vertices, new_capacity * sizeof(UiVertex));
    if (!new_vertices) {
        LOG_ERROR("ui: failed to grow vertex buffer");
        return;
    }
    g_ui.vertices = new_vertices;
    g_ui.vert_capacity = new_capacity;
}

static void ui_push_vertex(float x, float y, UiColor color) {
    ui_reserve_vertices(1);
    if (!g_ui.vertices) {
        return;
    }
    UiVertex *v = &g_ui.vertices[g_ui.vert_count++];
    v->x = x;
    v->y = y;
    v->r = color.r;
    v->g = color.g;
    v->b = color.b;
    v->a = color.a;
}

static size_t ui_add_rect(float x, float y, float w, float h, UiColor color) {
    ui_reserve_vertices(6);
    if (!g_ui.vertices) {
        return g_ui.vert_count;
    }
    size_t start = g_ui.vert_count;
    ui_push_vertex(x, y, color);
    ui_push_vertex(x + w, y, color);
    ui_push_vertex(x + w, y + h, color);

    ui_push_vertex(x, y, color);
    ui_push_vertex(x + w, y + h, color);
    ui_push_vertex(x, y + h, color);
    return start;
}

static void ui_update_rect(size_t start, float x, float y, float w, float h) {
    if (!g_ui.vertices || start + 6 > g_ui.vert_count) {
        return;
    }
    UiVertex *verts = &g_ui.vertices[start];
    verts[0].x = x;
    verts[0].y = y;
    verts[1].x = x + w;
    verts[1].y = y;
    verts[2].x = x + w;
    verts[2].y = y + h;

    verts[3].x = x;
    verts[3].y = y;
    verts[4].x = x + w;
    verts[4].y = y + h;
    verts[5].x = x;
    verts[5].y = y + h;
}

static float ui_measure_text(const char *text) {
    if (!text) {
        return 0.0f;
    }
    float line_width = 0.0f;
    float max_width = 0.0f;
    while (*text) {
        if (*text == '\n') {
            if (line_width > max_width) {
                max_width = line_width;
            }
            line_width = 0.0f;
        } else {
            line_width += UI_CHAR_ADVANCE;
        }
        ++text;
    }
    if (line_width > max_width) {
        max_width = line_width;
    }
    return max_width;
}

static const char *ui_role_name(uint8_t role) {
    switch (role) {
        case BEE_ROLE_QUEEN: return "QUEEN";
        case BEE_ROLE_NURSE: return "NURSE";
        case BEE_ROLE_HOUSEKEEPER: return "HOUSEKEEPER";
        case BEE_ROLE_STORAGE: return "STORAGE";
        case BEE_ROLE_FORAGER: return "FORAGER";
        case BEE_ROLE_SCOUT: return "SCOUT";
        case BEE_ROLE_GUARD: return "GUARD";
        default: return "UNKNOWN";
    }
}

static const char *ui_mode_name(uint8_t mode) {
    switch (mode) {
        case BEE_MODE_IDLE: return "IDLE";
        case BEE_MODE_OUTBOUND: return "OUTBOUND";
        case BEE_MODE_FORAGING: return "FORAGING";
        case BEE_MODE_RETURNING: return "RETURNING";
        case BEE_MODE_ENTERING: return "ENTERING";
        case BEE_MODE_UNLOADING: return "UNLOADING";
        default: return "UNKNOWN";
    }
}

static const char *ui_intent_name(uint8_t intent) {
    switch (intent) {
        case BEE_INTENT_FIND_PATCH: return "FIND PATCH";
        case BEE_INTENT_HARVEST: return "HARVEST";
        case BEE_INTENT_RETURN_HOME: return "RETURN HOME";
        case BEE_INTENT_UNLOAD: return "UNLOAD";
        case BEE_INTENT_REST: return "REST";
        case BEE_INTENT_EXPLORE: return "EXPLORE";
        default: return "UNKNOWN";
    }
}

static void ui_world_to_screen(float wx, float wy, float *sx, float *sy) {
    float zoom = g_ui.cam_zoom > 0.0f ? g_ui.cam_zoom : 1.0f;
    float cx = g_ui.cam_center_x;
    float cy = g_ui.cam_center_y;
    *sx = (wx - cx) * zoom + 0.5f * (float)g_ui.fb_width;
    *sy = (wy - cy) * zoom + 0.5f * (float)g_ui.fb_height;
}

static void ui_draw_hive_segment_horizontal(float ax, float bx, float y_world, UiColor color, float thickness) {
    if (fabsf(bx - ax) < 1e-4f) {
        return;
    }
    float sx0, sy0, sx1, sy1;
    ui_world_to_screen(ax, y_world, &sx0, &sy0);
    ui_world_to_screen(bx, y_world, &sx1, &sy1);
    float width = fabsf(sx1 - sx0);
    if (width < 1.0f) {
        return;
    }
    float x = fminf(sx0, sx1);
    float y = sy0 - 0.5f * thickness;
    ui_add_rect(x, y, width, thickness, color);
}

static void ui_draw_hive_segment_vertical(float ay, float by, float x_world, UiColor color, float thickness) {
    if (fabsf(by - ay) < 1e-4f) {
        return;
    }
    float sx0, sy0, sx1, sy1;
    ui_world_to_screen(x_world, ay, &sx0, &sy0);
    ui_world_to_screen(x_world, by, &sx1, &sy1);
    float height = fabsf(sy1 - sy0);
    if (height < 1.0f) {
        return;
    }
    float y = fminf(sy0, sy1);
    float x = sx0 - 0.5f * thickness;
    ui_add_rect(x, y, thickness, height, color);
}

static void ui_draw_hive_overlay(void) {
    if (!g_ui.show_hive_overlay || !g_ui.runtime || !g_ui.has_camera) {
        return;
    }
    if (g_ui.fb_width <= 0 || g_ui.fb_height <= 0) {
        return;
    }
    const Params *p = g_ui.runtime;
    if (p->hive.rect_w <= 0.0f || p->hive.rect_h <= 0.0f) {
        return;
    }

    const float x = p->hive.rect_x;
    const float y = p->hive.rect_y;
    const float w = p->hive.rect_w;
    const float h = p->hive.rect_h;

    const int side = p->hive.entrance_side;
    const float half_gap = p->hive.entrance_width * 0.5f;
    float gap_min = 0.0f;
    float gap_max = 0.0f;
    if (side == 0 || side == 1) {
        float gap_center = x + p->hive.entrance_t * w;
        gap_min = fmaxf(x, gap_center - half_gap);
        gap_max = fminf(x + w, gap_center + half_gap);
    } else {
        float gap_center = y + p->hive.entrance_t * h;
        gap_min = fmaxf(y, gap_center - half_gap);
        gap_max = fminf(y + h, gap_center + half_gap);
    }

    UiColor wall_color = ui_color_rgba(0.95f, 0.75f, 0.15f, 0.9f);
    UiColor gap_color = ui_color_rgba(0.2f, 0.85f, 0.35f, 0.9f);
    float thickness = fmaxf(2.0f, g_ui.cam_zoom * 0.8f);

    // Top
    if (side == 0) {
        if (gap_min - x > 1e-4f) {
            ui_draw_hive_segment_horizontal(x, gap_min, y, wall_color, thickness);
        }
        if (x + w - gap_max > 1e-4f) {
            ui_draw_hive_segment_horizontal(gap_max, x + w, y, wall_color, thickness);
        }
        if (gap_max > gap_min) {
            ui_draw_hive_segment_horizontal(gap_min, gap_max, y, gap_color, thickness);
        }
    } else {
        ui_draw_hive_segment_horizontal(x, x + w, y, wall_color, thickness);
    }

    // Bottom
    float y_bottom = y + h;
    if (side == 1) {
        if (gap_min - x > 1e-4f) {
            ui_draw_hive_segment_horizontal(x, gap_min, y_bottom, wall_color, thickness);
        }
        if (x + w - gap_max > 1e-4f) {
            ui_draw_hive_segment_horizontal(gap_max, x + w, y_bottom, wall_color, thickness);
        }
        if (gap_max > gap_min) {
            ui_draw_hive_segment_horizontal(gap_min, gap_max, y_bottom, gap_color, thickness);
        }
    } else {
        ui_draw_hive_segment_horizontal(x, x + w, y_bottom, wall_color, thickness);
    }

    // Left
    if (side == 2) {
        if (gap_min - y > 1e-4f) {
            ui_draw_hive_segment_vertical(y, gap_min, x, wall_color, thickness);
        }
        if (y + h - gap_max > 1e-4f) {
            ui_draw_hive_segment_vertical(gap_max, y + h, x, wall_color, thickness);
        }
        if (gap_max > gap_min) {
            ui_draw_hive_segment_vertical(gap_min, gap_max, x, gap_color, thickness);
        }
    } else {
        ui_draw_hive_segment_vertical(y, y + h, x, wall_color, thickness);
    }

    // Right
    float x_right = x + w;
    if (side == 3) {
        if (gap_min - y > 1e-4f) {
            ui_draw_hive_segment_vertical(y, gap_min, x_right, wall_color, thickness);
        }
        if (y + h - gap_max > 1e-4f) {
            ui_draw_hive_segment_vertical(gap_max, y + h, x_right, wall_color, thickness);
        }
        if (gap_max > gap_min) {
            ui_draw_hive_segment_vertical(gap_min, gap_max, x_right, gap_color, thickness);
        }
    } else {
        ui_draw_hive_segment_vertical(y, y + h, x_right, wall_color, thickness);
    }
}

void ui_set_viewport(const RenderCamera *camera, int framebuffer_width, int framebuffer_height) {
    g_ui.fb_width = framebuffer_width;
    g_ui.fb_height = framebuffer_height;
    if (!camera || framebuffer_width <= 0 || framebuffer_height <= 0) {
        g_ui.has_camera = false;
        return;
    }
    g_ui.cam_center_x = camera->center_world[0];
    g_ui.cam_center_y = camera->center_world[1];
    g_ui.cam_zoom = camera->zoom > 0.0f ? camera->zoom : 1.0f;
    g_ui.has_camera = true;
}

void ui_enable_hive_overlay(bool enabled) {
    g_ui.show_hive_overlay = enabled;
}

void ui_set_selected_bee(const BeeDebugInfo *info, bool valid) {
    if (valid && info) {
        g_ui.selected_bee = *info;
        g_ui.selected_valid = true;
        g_ui.selected_panel_open = true;
    } else {
        g_ui.selected_valid = false;
        g_ui.selected_panel_open = false;
    }
}

void ui_set_hex_overlay(bool show_grid, bool draw_on_top) {
    g_ui.hex_show_grid = show_grid;
    g_ui.hex_draw_on_top = draw_on_top;
}

void ui_set_selected_hex(const HexTile *tile, bool valid) {
    if (valid && tile) {
        g_ui.hex_selected_tile = *tile;
        g_ui.hex_selected_valid = true;
    } else {
        g_ui.hex_selected_valid = false;
    }
}
#define GLYPH(ch, r0, r1, r2, r3, r4, r5, r6) \
    { ch, { r0, r1, r2, r3, r4, r5, r6 } }

typedef struct {
    char ch;
    const char *rows[7];
} UiGlyphPattern;

static unsigned char ui_row_bits_from_pattern(const char *pattern) {
    unsigned char result = 0;
    for (int i = 0; i < 5; ++i) {
        result <<= 1;
        if (pattern[i] == '#') {
            result |= 1;
        }
    }
    return result;
}

static const UiGlyphPattern g_glyph_patterns[] = {
    GLYPH(' ', ".....", ".....", ".....", ".....", ".....", ".....", "....."),
    GLYPH('0', " ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "),
    GLYPH('1', "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "),
    GLYPH('2', " ### ", "#   #", "    #", "  ## ", " #   ", "#    ", "#####"),
    GLYPH('3', " ### ", "#   #", "    #", " ### ", "    #", "#   #", " ### "),
    GLYPH('4', "   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "),
    GLYPH('5', "#####", "#    ", "#    ", "#### ", "    #", "#   #", " ### "),
    GLYPH('6', " ### ", "#   #", "#    ", "#### ", "#   #", "#   #", " ### "),
    GLYPH('7', "#####", "    #", "   # ", "  #  ", "  #  ", "  #  ", "  #  "),
    GLYPH('8', " ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "),
    GLYPH('9', " ### ", "#   #", "#   #", " ####", "    #", "#   #", " ### "),
    GLYPH('A', " ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"),
    GLYPH('B', "#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "),
    GLYPH('C', " ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "),
    GLYPH('D', "#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "),
    GLYPH('E', "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"),
    GLYPH('F', "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "),
    GLYPH('G', " ### ", "#   #", "#    ", "#  ##", "#   #", "#   #", " ### "),
    GLYPH('H', "#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"),
    GLYPH('I', " ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "),
    GLYPH('J', "  ###", "   # ", "   # ", "   # ", "#  # ", "#  # ", " ##  "),
    GLYPH('K', "#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"),
    GLYPH('L', "#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"),
    GLYPH('M', "#   #", "## ##", "# # #", "#   #", "#   #", "#   #", "#   #"),
    GLYPH('N', "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"),
    GLYPH('O', " ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "),
    GLYPH('P', "#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "),
    GLYPH('Q', " ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"),
    GLYPH('R', "#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"),
    GLYPH('S', " ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "),
    GLYPH('T', "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "),
    GLYPH('U', "#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "),
    GLYPH('V', "#   #", "#   #", "#   #", "#   #", " # # ", " # # ", "  #  "),
    GLYPH('W', "#   #", "#   #", "# # #", "# # #", "# # #", "## ##", "#   #"),
    GLYPH('X', "#   #", " # # ", "  #  ", "  #  ", "  #  ", " # # ", "#   #"),
    GLYPH('Y', "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "),
    GLYPH('Z', "#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"),
    GLYPH(':', ".....", "  #  ", ".....", ".....", "  #  ", ".....", "....."),
    GLYPH('.', ".....", ".....", ".....", ".....", ".....", "  #  ", "....."),
    GLYPH('-', ".....", ".....", ".....", " ### ", ".....", ".....", "....."),
    GLYPH('+', ".....", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "....."),
    GLYPH('(', "   # ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "   # "),
    GLYPH(')', " #   ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " #   "),
    GLYPH('/', "    #", "   # ", "   # ", "  #  ", " #   ", " #   ", "#    "),
    GLYPH('%', "#   #", "   # ", "  #  ", "  #  ", " #   ", " #   ", "#   #")
};

static UiGlyph g_glyphs[sizeof(g_glyph_patterns) / sizeof(g_glyph_patterns[0])];
static size_t g_glyph_count = 0;
static bool g_glyphs_ready = false;

static void ui_build_glyph_cache(void) {
    if (g_glyphs_ready) {
        return;
    }
    g_glyph_count = sizeof(g_glyph_patterns) / sizeof(g_glyph_patterns[0]);
    for (size_t i = 0; i < g_glyph_count; ++i) {
        g_glyphs[i].ch = g_glyph_patterns[i].ch;
        for (int row = 0; row < 7; ++row) {
            g_glyphs[i].rows[row] = ui_row_bits_from_pattern(g_glyph_patterns[i].rows[row]);
        }
    }
    g_glyphs_ready = true;
}

static const UiGlyph *ui_find_glyph(char ch) {
    if (!g_glyphs_ready) {
        ui_build_glyph_cache();
    }
    for (size_t i = 0; i < g_glyph_count; ++i) {
        if (g_glyphs[i].ch == ch) {
            return &g_glyphs[i];
        }
    }
    return &g_glyphs[0];
}

static void ui_draw_text(float x, float y, const char *text, UiColor color) {
    float cursor_x = x;
    float cursor_y = y;
    while (*text) {
        char ch = *text++;
        if (ch == '\n') {
            cursor_x = x;
            cursor_y += UI_CHAR_HEIGHT + UI_FONT_SCALE;
            continue;
        }
        const UiGlyph *glyph = ui_find_glyph(ch);
        for (int row = 0; row < 7; ++row) {
            unsigned char bits = glyph->rows[row];
            for (int col = 0; col < 5; ++col) {
                if (bits & (1 << (4 - col))) {
                    float px = cursor_x + col * UI_FONT_SCALE;
                    float py = cursor_y + row * UI_FONT_SCALE;
                    ui_add_rect(px, py, UI_FONT_SCALE, UI_FONT_SCALE, color);
                }
            }
        }
        cursor_x += UI_CHAR_ADVANCE;
    }
}

static void ui_draw_selected_bee_panel(void) {
    if (!g_ui.selected_panel_open || !g_ui.selected_valid) {
        return;
    }
    if (g_ui.fb_width <= 0 || g_ui.fb_height <= 0) {
        return;
    }

    typedef struct {
        char text[128];
        UiColor color;
        float spacing_after;
    } BeePanelLine;

    BeePanelLine lines[12];
    size_t line_count = 0;
    const float padding = 16.0f;
    const float min_panel_width = 220.0f;

    UiColor bg = ui_color_rgba(0.10f, 0.10f, 0.14f, 0.94f);
    UiColor header = ui_color_rgba(0.95f, 0.95f, 0.98f, 1.0f);
    UiColor text_color = ui_color_rgba(0.85f, 0.88f, 0.92f, 1.0f);
    UiColor accent = ui_color_rgba(0.30f, 0.65f, 0.95f, 1.0f);

    const BeeDebugInfo *info = &g_ui.selected_bee;
    char line[128];

    #define ADD_LINE(TEXT_EXPR, COLOR_EXPR, SPACING_VALUE)                                \
        do {                                                                              \
            if (line_count < sizeof(lines) / sizeof(lines[0])) {                          \
                strncpy(lines[line_count].text, (TEXT_EXPR), sizeof(lines[line_count].text) - 1); \
                lines[line_count].text[sizeof(lines[line_count].text) - 1] = '\0';        \
                lines[line_count].color = (COLOR_EXPR);                                   \
                lines[line_count].spacing_after = (SPACING_VALUE);                        \
                ++line_count;                                                             \
            }                                                                             \
        } while (0)

    ADD_LINE("BEE INFO", header, 24.0f);

    snprintf(line, sizeof(line), "BEE #%zu", info->index);
    ADD_LINE(line, accent, 20.0f);

    snprintf(line, sizeof(line), "ROLE: %s", ui_role_name(info->role));
    ADD_LINE(line, text_color, 18.0f);

    snprintf(line, sizeof(line), "INTENT: %s", ui_intent_name(info->intent));
    ADD_LINE(line, text_color, 18.0f);

    snprintf(line, sizeof(line), "MODE: %s", ui_mode_name(info->mode));
    ADD_LINE(line, text_color, 18.0f);

    snprintf(line, sizeof(line), "STATUS: %s HIVE", info->inside_hive ? "INSIDE" : "OUTSIDE");
    ADD_LINE(line, text_color, 18.0f);

    int energy_pct = (int)(info->energy * 100.0f + 0.5f);
    float load_pct_f = (info->capacity_uL > 0.0f) ? (info->load_nectar / info->capacity_uL) * 100.0f : 0.0f;
    if (load_pct_f < 0.0f) load_pct_f = 0.0f;
    if (load_pct_f > 100.0f) load_pct_f = 100.0f;
    snprintf(line, sizeof(line), "ENERGY %d%%  |  NECTAR %.1f uL (%.0f%%)", energy_pct, info->load_nectar, load_pct_f);
    ADD_LINE(line, text_color, 18.0f);

    snprintf(line, sizeof(line), "CAPACITY %.1f uL  |  HARVEST %.1f uL/s", info->capacity_uL, info->harvest_rate_uLps);
    ADD_LINE(line, text_color, 18.0f);

    snprintf(line, sizeof(line), "SPEED %.1f PX/S  |  AGE %.1f DAYS", info->speed, info->age_days);
    ADD_LINE(line, text_color, 18.0f);

    snprintf(line, sizeof(line), "LOCATION: (%.1f, %.1f)", info->pos_x, info->pos_y);
    ADD_LINE(line, text_color, 24.0f);

    #undef ADD_LINE

    float max_width = 0.0f;
    for (size_t i = 0; i < line_count; ++i) {
        float width = ui_measure_text(lines[i].text);
        if (width > max_width) {
            max_width = width;
        }
    }

    float panel_width = fmaxf(min_panel_width, max_width + padding * 2.0f);
    float origin_y = UI_PANEL_MARGIN;
    float origin_x = (float)g_ui.fb_width - panel_width - UI_PANEL_MARGIN;
    if (origin_x < UI_PANEL_MARGIN) {
        origin_x = UI_PANEL_MARGIN;
    }
    float text_x = origin_x + padding;
    float cursor_y = origin_y + 18.0f;

    size_t bg_idx = ui_add_rect(origin_x, origin_y, panel_width, 1.0f, bg);

    for (size_t i = 0; i < line_count; ++i) {
        ui_draw_text(text_x, cursor_y, lines[i].text, lines[i].color);
        cursor_y += lines[i].spacing_after;
    }

    float panel_h = (cursor_y + 12.0f) - origin_y;
    ui_update_rect(bg_idx, origin_x, origin_y, panel_width, panel_h);
}
static GLuint ui_create_shader(const char *vs_src, const char *fs_src) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    GLint compiled = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(vs, (GLsizei)sizeof(log), NULL, log);
        LOG_ERROR("ui: vertex shader compile error: %s", log);
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(fs, (GLsizei)sizeof(log), NULL, log);
        LOG_ERROR("ui: fragment shader compile error: %s", log);
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
        LOG_ERROR("ui: shader link error: %s", log);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

void ui_init(void) {
    memset(&g_ui, 0, sizeof(g_ui));
    ui_build_glyph_cache();
    g_ui.program = ui_create_shader(UI_VERTEX_SHADER, UI_FRAGMENT_SHADER);
    g_ui.resolution_uniform = glGetUniformLocation(g_ui.program, "u_resolution");
    g_ui.show_hive_overlay = true;
    g_ui.hex_show_grid = true;
    g_ui.hex_draw_on_top = false;
    g_ui.has_camera = false;
    g_ui.cam_zoom = 1.0f;
    g_ui.selected_valid = false;
    g_ui.selected_panel_open = false;
    g_ui.hex_selected_valid = false;
    g_ui.panel_scroll = 0.0f;
    g_ui.panel_content_height = 0.0f;
    g_ui.panel_visible_height = 0.0f;
    g_ui.panel_last_width = UI_PANEL_WIDTH;

    glGenVertexArrays(1, &g_ui.vao);
    glGenBuffers(1, &g_ui.vbo);
    glBindVertexArray(g_ui.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_ui.vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex), (void *)offsetof(UiVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(UiVertex), (void *)offsetof(UiVertex, r));
    glBindVertexArray(0);

    g_ui.vert_capacity = 2048;
    g_ui.vertices = (UiVertex *)malloc(g_ui.vert_capacity * sizeof(UiVertex));
    g_ui.active_slider = -1;
}

void ui_shutdown(void) {
    free(g_ui.vertices);
    g_ui.vertices = NULL;
    g_ui.vert_capacity = 0;
    g_ui.vert_count = 0;
    g_glyphs_ready = false;
    g_glyph_count = 0;

    if (g_ui.vbo) {
        glDeleteBuffers(1, &g_ui.vbo);
        g_ui.vbo = 0;
    }
    if (g_ui.vao) {
        glDeleteVertexArrays(1, &g_ui.vao);
        g_ui.vao = 0;
    }
    if (g_ui.program) {
        glDeleteProgram(g_ui.program);
        g_ui.program = 0;
    }
}

void ui_sync_to_params(const Params *baseline, Params *runtime) {
    if (!baseline || !runtime) {
        g_ui.has_params = false;
        g_ui.runtime = NULL;
        return;
    }
    g_ui.baseline = *baseline;
    g_ui.runtime = runtime;
    g_ui.has_params = true;
    g_ui.dirty = false;
    g_ui.reinit_required = false;
}

static void ui_begin_frame(const Input *input) {
    g_ui.vert_count = 0;
    g_ui.action_toggle_pause = false;
    g_ui.action_step = false;
    g_ui.action_apply = false;
    g_ui.action_reset = false;
    g_ui.action_reinit = false;
    g_ui.action_focus_queen = false;
    g_ui.action_toggle_hex_grid = false;
    g_ui.action_toggle_hex_layer = false;
    g_ui.wants_mouse = false;
    g_ui.wants_keyboard = false;

    if (!g_ui.has_params || !g_ui.runtime) {
        return;
    }

    g_ui.mouse_x = input ? input->mouse_x_px : 0.0f;
    g_ui.mouse_y = input ? input->mouse_y_px : 0.0f;
    bool mouse_down = input ? input->mouse_left_down : false;
    bool mouse_pressed = input ? input->mouse_left_pressed : false;

    UiColor panel_bg = ui_color_rgba(0.08f, 0.08f, 0.10f, 0.92f);
    UiColor accent = ui_color_rgba(0.25f, 0.60f, 0.98f, 1.0f);
    UiColor border = ui_color_rgba(0.2f, 0.2f, 0.2f, 1.0f);
    UiColor text = ui_color_rgba(1.0f, 1.0f, 1.0f, 1.0f);

    ui_draw_hive_overlay();

    UiRect hamburger = {UI_PANEL_MARGIN, UI_PANEL_MARGIN, UI_HAMBURGER_SIZE, UI_HAMBURGER_SIZE};
    bool hamburger_hover = ui_rect_contains(&hamburger, g_ui.mouse_x, g_ui.mouse_y);
    UiColor burger_col = hamburger_hover ? accent : ui_color_rgba(0.9f, 0.9f, 0.9f, 1.0f);
    ui_add_rect(hamburger.x, hamburger.y, hamburger.w, hamburger.h, ui_color_rgba(0.15f, 0.15f, 0.18f, 0.95f));
    float line_padding = 6.0f;
    for (int i = 0; i < 3; ++i) {
        float ly = hamburger.y + 6.0f + i * (line_padding + 4.0f);
        ui_add_rect(hamburger.x + 6.0f, ly, hamburger.w - 12.0f, 4.0f, burger_col);
    }
    if (mouse_pressed && hamburger_hover) {
        g_ui.panel_open = !g_ui.panel_open;
    }

    UiRect panel_rect = {UI_PANEL_MARGIN, UI_PANEL_MARGIN + UI_HAMBURGER_SIZE + 12.0f, UI_PANEL_WIDTH, 0.0f};

    if (!g_ui.panel_open) {
        g_ui.mouse_over_panel = false;
        g_ui.wants_mouse = g_ui.capturing_mouse;
        g_ui.wants_keyboard = false;
        g_ui.prev_mouse_down = mouse_down;
        g_ui.panel_scroll = 0.0f;
        g_ui.panel_content_height = 0.0f;
        ui_draw_selected_bee_panel();
        return;
    }

    float view_height = (float)g_ui.fb_height - panel_rect.y - UI_PANEL_MARGIN;
    if (view_height < 200.0f) {
        view_height = 200.0f;
    }
    float max_view_height = (float)g_ui.fb_height - panel_rect.y;
    if (max_view_height < 80.0f) {
        max_view_height = 80.0f;
    }
    if (view_height > max_view_height) {
        view_height = max_view_height;
    }
    float prev_panel_width = g_ui.panel_last_width > 0.0f ? g_ui.panel_last_width : UI_PANEL_WIDTH;
    float max_prev_scroll = fmaxf(0.0f, g_ui.panel_content_height - view_height);
    if (input && input->wheel_y != 0) {
        float mx = g_ui.mouse_x;
        float my = g_ui.mouse_y;
        bool over_panel = mx >= panel_rect.x && mx <= panel_rect.x + prev_panel_width &&
                          my >= panel_rect.y && my <= panel_rect.y + view_height;
        if (over_panel || g_ui.capturing_mouse || g_ui.mouse_over_panel) {
            g_ui.panel_scroll -= (float)input->wheel_y * 30.0f;
        }
    }
    g_ui.panel_scroll = ui_clampf(g_ui.panel_scroll, 0.0f, max_prev_scroll);
    g_ui.panel_visible_height = view_height;

    float scroll = g_ui.panel_scroll;
    float view_top = panel_rect.y;
    float view_bottom = view_top + view_height;

    float cursor_y = panel_rect.y + 18.0f;
    float content_width = UI_PANEL_WIDTH - 40.0f;
    float panel_max_x = panel_rect.x + UI_PANEL_WIDTH;

    size_t panel_bg_start = ui_add_rect(panel_rect.x, panel_rect.y, UI_PANEL_WIDTH, view_height, panel_bg);
    size_t panel_border_start = ui_add_rect(panel_rect.x, panel_rect.y, UI_PANEL_WIDTH, view_height, border);

    float text_x = panel_rect.x + 20.0f;
    if (ui_range_intersects(cursor_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(text_x, cursor_y - scroll, "SIM CONTROLS", text);
    }
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("SIM CONTROLS"));
    cursor_y += 24.0f;

    if (ui_range_intersects(cursor_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(text_x, cursor_y - scroll, "HEX GRID", text);
    }
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("HEX GRID"));
    cursor_y += 24.0f;

    UiRect grid_rect = {text_x, cursor_y - scroll, content_width, 30.0f};
    bool grid_visible = ui_range_intersects(grid_rect.y, grid_rect.h, view_top, view_bottom);
    panel_max_x = fmaxf(panel_max_x, grid_rect.x + grid_rect.w);
    UiColor toggle_off = ui_color_rgba(0.20f, 0.20f, 0.25f, 1.0f);
    if (grid_visible) {
        ui_add_rect(grid_rect.x, grid_rect.y, grid_rect.w, grid_rect.h, g_ui.hex_show_grid ? accent : toggle_off);
        if (ui_range_intersects(grid_rect.y + 6.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(grid_rect.x + 8.0f, grid_rect.y + 6.0f, "SHOW HEX GRID", text);
            const char *state_txt = g_ui.hex_show_grid ? "ON" : "OFF";
            float state_w = ui_measure_text(state_txt);
            ui_draw_text(grid_rect.x + grid_rect.w - state_w - 8.0f, grid_rect.y + 6.0f, state_txt, text);
        }
    }
    if (mouse_pressed && ui_rect_contains(&grid_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.hex_show_grid = !g_ui.hex_show_grid;
        g_ui.action_toggle_hex_grid = true;
    }
    cursor_y += grid_rect.h + 8.0f;

    UiRect layer_rect = {text_x, cursor_y - scroll, content_width, 30.0f};
    bool layer_visible = ui_range_intersects(layer_rect.y, layer_rect.h, view_top, view_bottom);
    panel_max_x = fmaxf(panel_max_x, layer_rect.x + layer_rect.w);
    if (layer_visible) {
        ui_add_rect(layer_rect.x, layer_rect.y, layer_rect.w, layer_rect.h, g_ui.hex_draw_on_top ? accent : toggle_off);
        if (ui_range_intersects(layer_rect.y + 6.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(layer_rect.x + 8.0f, layer_rect.y + 6.0f, "DRAW HEXES ON TOP", text);
            const char *state_txt = g_ui.hex_draw_on_top ? "ON" : "OFF";
            float state_w = ui_measure_text(state_txt);
            ui_draw_text(layer_rect.x + layer_rect.w - state_w - 8.0f, layer_rect.y + 6.0f, state_txt, text);
        }
    }
    if (mouse_pressed && ui_rect_contains(&layer_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.hex_draw_on_top = !g_ui.hex_draw_on_top;
        g_ui.action_toggle_hex_layer = true;
    }
    cursor_y += layer_rect.h + 12.0f;

    if (ui_range_intersects(cursor_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(text_x, cursor_y - scroll, "SELECTED TILE", text);
    }
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("SELECTED TILE"));
    cursor_y += 22.0f;

    float info_x = text_x + 8.0f;
    float info_y = cursor_y;
    if (g_ui.hex_selected_valid) {
        char buf[128];
        struct {
            const char *label;
            float value;
            bool is_int;
            bool draw;
        } lines[] = {
            {"Q", (float)g_ui.hex_selected_tile.q, true, true},
            {"R", (float)g_ui.hex_selected_tile.r, true, true},
        };
        for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
            if (!lines[i].draw) {
                continue;
            }
            if (lines[i].is_int) {
                snprintf(buf, sizeof(buf), "%s: %d", lines[i].label, (int)lines[i].value);
            } else {
                snprintf(buf, sizeof(buf), "%s: %.1f", lines[i].label, lines[i].value);
            }
            if (ui_range_intersects(info_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
                ui_draw_text(info_x, info_y - scroll, buf, text);
            }
            panel_max_x = fmaxf(panel_max_x, info_x + ui_measure_text(buf));
            info_y += 18.0f;
        }

        snprintf(buf, sizeof(buf), "CENTER X: %.1f", g_ui.hex_selected_tile.center_x);
        if (ui_range_intersects(info_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(info_x, info_y - scroll, buf, text);
        }
        panel_max_x = fmaxf(panel_max_x, info_x + ui_measure_text(buf));
        info_y += 18.0f;

        snprintf(buf, sizeof(buf), "CENTER Y: %.1f", g_ui.hex_selected_tile.center_y);
        if (ui_range_intersects(info_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(info_x, info_y - scroll, buf, text);
        }
        panel_max_x = fmaxf(panel_max_x, info_x + ui_measure_text(buf));
        info_y += 18.0f;

        const char *terrain_name = ui_hex_terrain_name(g_ui.hex_selected_tile.terrain);
        snprintf(buf, sizeof(buf), "TERRAIN: %s", terrain_name);
        if (ui_range_intersects(info_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(info_x, info_y - scroll, buf, text);
        }
        panel_max_x = fmaxf(panel_max_x, info_x + ui_measure_text(buf));
        info_y += 18.0f;

        snprintf(buf, sizeof(buf), "NECTAR STOCK: %.1f", g_ui.hex_selected_tile.nectar_stock);
        if (ui_range_intersects(info_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(info_x, info_y - scroll, buf, text);
        }
        panel_max_x = fmaxf(panel_max_x, info_x + ui_measure_text(buf));
        info_y += 18.0f;

        snprintf(buf, sizeof(buf), "NECTAR CAPACITY: %.1f", g_ui.hex_selected_tile.nectar_capacity);
        if (ui_range_intersects(info_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(info_x, info_y - scroll, buf, text);
        }
        panel_max_x = fmaxf(panel_max_x, info_x + ui_measure_text(buf));
        info_y += 18.0f;

        snprintf(buf, sizeof(buf), "NECTAR RECHARGE: %.2f", g_ui.hex_selected_tile.nectar_recharge_rate);
        if (ui_range_intersects(info_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(info_x, info_y - scroll, buf, text);
        }
        panel_max_x = fmaxf(panel_max_x, info_x + ui_measure_text(buf));
        info_y += 18.0f;

        snprintf(buf, sizeof(buf), "FLOW CAPACITY: %.1f", g_ui.hex_selected_tile.flow_capacity);
        if (ui_range_intersects(info_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(info_x, info_y - scroll, buf, text);
        }
        panel_max_x = fmaxf(panel_max_x, info_x + ui_measure_text(buf));
        info_y += 24.0f;
    } else {
        if (ui_range_intersects(info_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(info_x, info_y - scroll, "NONE", text);
        }
        panel_max_x = fmaxf(panel_max_x, info_x + ui_measure_text("NONE"));
        info_y += 24.0f;
    }
    cursor_y = info_y;

    SliderSpec motion_sliders[] = {
        {"MIN SPEED", 0.0f, 200.0f, 1.0f, &g_ui.runtime->motion_min_speed, 0},
        {"MAX SPEED", 0.0f, 200.0f, 1.0f, &g_ui.runtime->motion_max_speed, 1},
        {"HEADING JITTER", 0.0f, 180.0f, 1.0f, &g_ui.runtime->motion_jitter_deg_per_sec, 2},
        {"BOUNCE MARGIN", 0.0f, fminf(g_ui.runtime->world_width_px, g_ui.runtime->world_height_px) * 0.5f, 1.0f, &g_ui.runtime->motion_bounce_margin, 3},
        {"SPAWN SPEED MEAN", 0.0f, 200.0f, 1.0f, &g_ui.runtime->motion_spawn_speed_mean, 4},
        {"SPAWN SPEED STD", 0.0f, 120.0f, 1.0f, &g_ui.runtime->motion_spawn_speed_std, 5},
    };

    motion_sliders[4].min_value = g_ui.runtime->motion_min_speed;
    motion_sliders[4].max_value = g_ui.runtime->motion_max_speed;
    if (motion_sliders[4].max_value < motion_sliders[4].min_value) {
        motion_sliders[4].max_value = motion_sliders[4].min_value;
    }

    float slider_x = text_x;
    float slider_width = content_width;

    cursor_y = ui_draw_slider_group(motion_sliders,
                                    sizeof(motion_sliders) / sizeof(motion_sliders[0]),
                                    slider_x,
                                    slider_width,
                                    cursor_y,
                                    text,
                                    &panel_max_x,
                                    mouse_pressed,
                                    mouse_down,
                                    scroll,
                                    view_top,
                                    view_bottom);

    if (ui_range_intersects(cursor_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(text_x, cursor_y - scroll, "FORAGING", text);
    }
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("FORAGING"));
    cursor_y += 24.0f;

    SliderSpec forage_sliders[] = {
        {"HARVEST RATE (uL/s)", 1.0f, 300.0f, 1.0f, &g_ui.runtime->bee.harvest_rate_uLps, 100},
        {"CARRY CAPACITY (uL)", 10.0f, 120.0f, 1.0f, &g_ui.runtime->bee.capacity_uL, 101},
        {"UNLOAD RATE (uL/s)", 10.0f, 400.0f, 5.0f, &g_ui.runtime->bee.unload_rate_uLps, 102},
        {"REST RECOVERY (/s)", 0.05f, 1.5f, 0.01f, &g_ui.runtime->bee.rest_recovery_per_s, 103},
        {"FLIGHT SPEED", 10.0f, 200.0f, 1.0f, &g_ui.runtime->bee.speed_mps, 104},
        {"SEEK ACCEL", 10.0f, 600.0f, 5.0f, &g_ui.runtime->bee.seek_accel, 105},
        {"ARRIVE TOL", 1.0f, 300.0f, 1.0f, &g_ui.runtime->bee.arrive_tol_world, 106},
    };

    float arrive_min = g_ui.runtime->bee_radius_px * 2.0f;
    if (arrive_min > forage_sliders[6].min_value) {
        forage_sliders[6].min_value = arrive_min;
    }
    if (forage_sliders[6].max_value < forage_sliders[6].min_value + 1.0f) {
        forage_sliders[6].max_value = forage_sliders[6].min_value + 1.0f;
    }

    cursor_y = ui_draw_slider_group(forage_sliders,
                                    sizeof(forage_sliders) / sizeof(forage_sliders[0]),
                                    slider_x,
                                    slider_width,
                                    cursor_y,
                                    text,
                                    &panel_max_x,
                                    mouse_pressed,
                                    mouse_down,
                                    scroll,
                                    view_top,
                                    view_bottom);
    if (g_ui.runtime->bee.arrive_tol_world < forage_sliders[6].min_value) {
        g_ui.runtime->bee.arrive_tol_world = forage_sliders[6].min_value;
    }

    if (g_ui.runtime->motion_min_speed > g_ui.runtime->motion_max_speed) {
        g_ui.runtime->motion_max_speed = g_ui.runtime->motion_min_speed;
    }
    g_ui.runtime->motion_spawn_speed_mean = ui_clampf(g_ui.runtime->motion_spawn_speed_mean,
                                                   g_ui.runtime->motion_min_speed,
                                                   g_ui.runtime->motion_max_speed);
    if (g_ui.runtime->motion_spawn_speed_std < 0.0f) {
        g_ui.runtime->motion_spawn_speed_std = 0.0f;
    }

    if (ui_range_intersects(cursor_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(text_x, cursor_y - scroll, "SPAWN MODE", text);
    }
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("SPAWN MODE"));
    cursor_y += 20.0f;
    float button_w = (content_width - 10.0f) * 0.5f;
    UiRect uniform_rect = {text_x, cursor_y - scroll, button_w, 28.0f};
    UiRect gaussian_rect = {text_x + button_w + 10.0f, cursor_y - scroll, button_w, 28.0f};
    bool uniform_active = g_ui.runtime->motion_spawn_mode == SPAWN_VELOCITY_UNIFORM_DIR;
    bool gaussian_active = g_ui.runtime->motion_spawn_mode == SPAWN_VELOCITY_GAUSSIAN_DIR;
    bool uniform_visible = ui_range_intersects(uniform_rect.y, uniform_rect.h, view_top, view_bottom);
    bool gaussian_visible = ui_range_intersects(gaussian_rect.y, gaussian_rect.h, view_top, view_bottom);
    if (uniform_visible) {
        ui_add_rect(uniform_rect.x, uniform_rect.y, uniform_rect.w, uniform_rect.h,
                    uniform_active ? accent : ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    }
    if (gaussian_visible) {
        ui_add_rect(gaussian_rect.x, gaussian_rect.y, gaussian_rect.w, gaussian_rect.h,
                    gaussian_active ? accent : ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    }
    panel_max_x = fmaxf(panel_max_x, gaussian_rect.x + gaussian_rect.w);
    if (uniform_visible && ui_range_intersects(uniform_rect.y + 6.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(uniform_rect.x + 8.0f, uniform_rect.y + 6.0f, "UNIFORM", text);
    }
    if (gaussian_visible && ui_range_intersects(gaussian_rect.y + 6.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(gaussian_rect.x + 8.0f, gaussian_rect.y + 6.0f, "GAUSSIAN", text);
    }
    if (mouse_pressed) {
        if (ui_rect_contains(&uniform_rect, g_ui.mouse_x, g_ui.mouse_y)) {
            g_ui.runtime->motion_spawn_mode = SPAWN_VELOCITY_UNIFORM_DIR;
        } else if (ui_rect_contains(&gaussian_rect, g_ui.mouse_x, g_ui.mouse_y)) {
            g_ui.runtime->motion_spawn_mode = SPAWN_VELOCITY_GAUSSIAN_DIR;
        }
    }
    cursor_y += 40.0f;

    if (ui_range_intersects(cursor_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(text_x, cursor_y - scroll, "BEE COUNT", text);
    }
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("BEE COUNT"));
    cursor_y += 22.0f;
    UiRect minus_rect = {text_x, cursor_y - scroll, 28.0f, 24.0f};
    UiRect plus_rect = {text_x + 120.0f, cursor_y - scroll, 28.0f, 24.0f};
    bool minus_visible = ui_range_intersects(minus_rect.y, minus_rect.h, view_top, view_bottom);
    bool plus_visible = ui_range_intersects(plus_rect.y, plus_rect.h, view_top, view_bottom);
    if (minus_visible) {
        ui_add_rect(minus_rect.x, minus_rect.y, minus_rect.w, minus_rect.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    }
    if (plus_visible) {
        ui_add_rect(plus_rect.x, plus_rect.y, plus_rect.w, plus_rect.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    }
    panel_max_x = fmaxf(panel_max_x, plus_rect.x + plus_rect.w);
    if (minus_visible && ui_range_intersects(minus_rect.y + 4.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(minus_rect.x + 9.0f, minus_rect.y + 4.0f, "-", text);
    }
    if (plus_visible && ui_range_intersects(plus_rect.y + 4.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(plus_rect.x + 7.0f, plus_rect.y + 4.0f, "+", text);
    }

    if (mouse_pressed && ui_rect_contains(&minus_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        if (g_ui.runtime->bee_count > 1) {
            g_ui.runtime->bee_count = g_ui.runtime->bee_count > 100 ? g_ui.runtime->bee_count - 100 : g_ui.runtime->bee_count - 1;
        }
    }
    if (mouse_pressed && ui_rect_contains(&plus_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.runtime->bee_count += (g_ui.runtime->bee_count >= 100 ? 100 : 1);
        if (g_ui.runtime->bee_count > 1000000) {
            g_ui.runtime->bee_count = 1000000;
        }
    }

    char bee_buf[32];
    snprintf(bee_buf, sizeof(bee_buf), "%zu", g_ui.runtime->bee_count);
    float bee_text_y = cursor_y + 4.0f - scroll;
    if (ui_range_intersects(bee_text_y, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(text_x + 40.0f, bee_text_y, bee_buf, text);
    }
    panel_max_x = fmaxf(panel_max_x, text_x + 40.0f + ui_measure_text(bee_buf));
    cursor_y += 36.0f;

    if (ui_range_intersects(cursor_y - scroll, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(text_x, cursor_y - scroll, "WORLD SIZE", text);
    }
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("WORLD SIZE"));
    cursor_y += 24.0f;
    UiRect world_minus_w = {text_x, cursor_y - scroll, 28.0f, 24.0f};
    UiRect world_plus_w = {text_x + 120.0f, cursor_y - scroll, 28.0f, 24.0f};
    UiRect world_minus_h = {text_x, cursor_y + 32.0f - scroll, 28.0f, 24.0f};
    UiRect world_plus_h = {text_x + 120.0f, cursor_y + 32.0f - scroll, 28.0f, 24.0f};

    bool world_minus_w_visible = ui_range_intersects(world_minus_w.y, world_minus_w.h, view_top, view_bottom);
    bool world_plus_w_visible = ui_range_intersects(world_plus_w.y, world_plus_w.h, view_top, view_bottom);
    bool world_minus_h_visible = ui_range_intersects(world_minus_h.y, world_minus_h.h, view_top, view_bottom);
    bool world_plus_h_visible = ui_range_intersects(world_plus_h.y, world_plus_h.h, view_top, view_bottom);

    if (world_minus_w_visible) {
        ui_add_rect(world_minus_w.x, world_minus_w.y, world_minus_w.w, world_minus_w.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    }
    if (world_plus_w_visible) {
        ui_add_rect(world_plus_w.x, world_plus_w.y, world_plus_w.w, world_plus_w.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    }
    if (world_minus_h_visible) {
        ui_add_rect(world_minus_h.x, world_minus_h.y, world_minus_h.w, world_minus_h.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    }
    if (world_plus_h_visible) {
        ui_add_rect(world_plus_h.x, world_plus_h.y, world_plus_h.w, world_plus_h.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    }
    panel_max_x = fmaxf(panel_max_x, fmaxf(world_plus_w.x + world_plus_w.w, world_plus_h.x + world_plus_h.w));
    if (world_minus_w_visible && ui_range_intersects(world_minus_w.y + 4.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(world_minus_w.x + 9.0f, world_minus_w.y + 4.0f, "-", text);
    }
    if (world_plus_w_visible && ui_range_intersects(world_plus_w.y + 4.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(world_plus_w.x + 7.0f, world_plus_w.y + 4.0f, "+", text);
    }
    if (world_minus_h_visible && ui_range_intersects(world_minus_h.y + 4.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(world_minus_h.x + 9.0f, world_minus_h.y + 4.0f, "-", text);
    }
    if (world_plus_h_visible && ui_range_intersects(world_plus_h.y + 4.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(world_plus_h.x + 7.0f, world_plus_h.y + 4.0f, "+", text);
    }

    if (mouse_pressed && ui_rect_contains(&world_minus_w, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.runtime->world_width_px = fmaxf(100.0f, g_ui.runtime->world_width_px - 100.0f);
    }
    if (mouse_pressed && ui_rect_contains(&world_plus_w, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.runtime->world_width_px += 100.0f;
    }
    if (mouse_pressed && ui_rect_contains(&world_minus_h, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.runtime->world_height_px = fmaxf(100.0f, g_ui.runtime->world_height_px - 100.0f);
    }
    if (mouse_pressed && ui_rect_contains(&world_plus_h, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.runtime->world_height_px += 100.0f;
    }

    char world_buf[48];
    snprintf(world_buf, sizeof(world_buf), "W %.0f", g_ui.runtime->world_width_px);
    float world_w_text_y = cursor_y + 4.0f - scroll;
    if (ui_range_intersects(world_w_text_y, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(text_x + 40.0f, world_w_text_y, world_buf, text);
    }
    panel_max_x = fmaxf(panel_max_x, text_x + 40.0f + ui_measure_text(world_buf));
    snprintf(world_buf, sizeof(world_buf), "H %.0f", g_ui.runtime->world_height_px);
    float world_h_text_y = cursor_y + 36.0f - scroll;
    if (ui_range_intersects(world_h_text_y, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(text_x + 40.0f, world_h_text_y, world_buf, text);
    }
    panel_max_x = fmaxf(panel_max_x, text_x + 40.0f + ui_measure_text(world_buf));
    cursor_y += 72.0f;

    UiRect pause_rect = {text_x, cursor_y - scroll, (content_width - 10.0f) * 0.5f, 28.0f};
    UiRect step_rect = {text_x + pause_rect.w + 10.0f, cursor_y - scroll, pause_rect.w, 28.0f};
    bool pause_visible = ui_range_intersects(pause_rect.y, pause_rect.h, view_top, view_bottom);
    bool step_visible = ui_range_intersects(step_rect.y, step_rect.h, view_top, view_bottom);
    if (pause_visible) {
        ui_add_rect(pause_rect.x, pause_rect.y, pause_rect.w, pause_rect.h, accent);
    }
    if (step_visible) {
        ui_add_rect(step_rect.x, step_rect.y, step_rect.w, step_rect.h, ui_color_rgba(0.3f, 0.3f, 0.35f, 1.0f));
    }
    panel_max_x = fmaxf(panel_max_x, step_rect.x + step_rect.w);
    if (pause_visible && ui_range_intersects(pause_rect.y + 6.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(pause_rect.x + 8.0f, pause_rect.y + 6.0f, g_ui.sim_paused ? "RESUME" : "PAUSE", text);
    }
    if (step_visible && ui_range_intersects(step_rect.y + 6.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(step_rect.x + 8.0f, step_rect.y + 6.0f, "STEP", text);
    }
    if (mouse_pressed && ui_rect_contains(&pause_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.action_toggle_pause = true;
    }
    if (mouse_pressed && ui_rect_contains(&step_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.action_step = true;
    }
    cursor_y += 40.0f;

    UiRect queen_rect = {text_x, cursor_y - scroll, content_width, 28.0f};
    bool queen_visible = ui_range_intersects(queen_rect.y, queen_rect.h, view_top, view_bottom);
    UiColor queen_button = ui_color_rgba(0.95f, 0.30f, 0.85f, 1.0f);
    if (queen_visible) {
        ui_add_rect(queen_rect.x, queen_rect.y, queen_rect.w, queen_rect.h, queen_button);
    }
    panel_max_x = fmaxf(panel_max_x, queen_rect.x + queen_rect.w);
    if (queen_visible && ui_range_intersects(queen_rect.y + 6.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(queen_rect.x + 8.0f, queen_rect.y + 6.0f, "FIND QUEEN", text);
    }
    if (mouse_pressed && ui_rect_contains(&queen_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.action_focus_queen = true;
    }
    cursor_y += 40.0f;

    bool dirty_now = false;
    const Params *runtime = g_ui.runtime;
    const Params *baseline = &g_ui.baseline;
    if (fabsf(runtime->motion_min_speed - baseline->motion_min_speed) > 0.0001f ||
        fabsf(runtime->motion_max_speed - baseline->motion_max_speed) > 0.0001f ||
        fabsf(runtime->motion_jitter_deg_per_sec - baseline->motion_jitter_deg_per_sec) > 0.0001f ||
        fabsf(runtime->motion_bounce_margin - baseline->motion_bounce_margin) > 0.0001f ||
        fabsf(runtime->motion_spawn_speed_mean - baseline->motion_spawn_speed_mean) > 0.0001f ||
        fabsf(runtime->motion_spawn_speed_std - baseline->motion_spawn_speed_std) > 0.0001f ||
        runtime->motion_spawn_mode != baseline->motion_spawn_mode ||
        runtime->bee_count != baseline->bee_count ||
        fabsf(runtime->world_width_px - baseline->world_width_px) > 0.0001f ||
        fabsf(runtime->world_height_px - baseline->world_height_px) > 0.0001f ||
        fabsf(runtime->bee.harvest_rate_uLps - baseline->bee.harvest_rate_uLps) > 0.0001f ||
        fabsf(runtime->bee.capacity_uL - baseline->bee.capacity_uL) > 0.0001f ||
        fabsf(runtime->bee.unload_rate_uLps - baseline->bee.unload_rate_uLps) > 0.0001f ||
        fabsf(runtime->bee.rest_recovery_per_s - baseline->bee.rest_recovery_per_s) > 0.0001f ||
        fabsf(runtime->bee.speed_mps - baseline->bee.speed_mps) > 0.0001f ||
        fabsf(runtime->bee.seek_accel - baseline->bee.seek_accel) > 0.0001f ||
        fabsf(runtime->bee.arrive_tol_world - baseline->bee.arrive_tol_world) > 0.0001f) {
        dirty_now = true;
    }
    g_ui.dirty = dirty_now;
    g_ui.reinit_required = (runtime->bee_count != baseline->bee_count) ||
                           fabsf(runtime->world_width_px - baseline->world_width_px) > 0.0001f ||
                           fabsf(runtime->world_height_px - baseline->world_height_px) > 0.0001f;

    float apply_content_y = cursor_y;
    float reset_content_y = cursor_y + 40.0f;
    UiRect apply_rect = {text_x, apply_content_y - scroll, content_width, 30.0f};
    UiRect reset_rect = {text_x, reset_content_y - scroll, content_width, 30.0f};
    bool apply_visible = ui_range_intersects(apply_rect.y, apply_rect.h, view_top, view_bottom);
    bool reset_visible = ui_range_intersects(reset_rect.y, reset_rect.h, view_top, view_bottom);
    UiColor apply_color = g_ui.dirty ? accent : ui_color_rgba(0.3f, 0.3f, 0.35f, 1.0f);
    if (apply_visible) {
        ui_add_rect(apply_rect.x, apply_rect.y, apply_rect.w, apply_rect.h, apply_color);
    }
    if (reset_visible) {
        ui_add_rect(reset_rect.x, reset_rect.y, reset_rect.w, reset_rect.h, ui_color_rgba(0.25f, 0.25f, 0.30f, 1.0f));
    }
    if (apply_visible && ui_range_intersects(apply_rect.y + 8.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(apply_rect.x + 8.0f, apply_rect.y + 8.0f, "APPLY", text);
    }
    if (reset_visible && ui_range_intersects(reset_rect.y + 8.0f, UI_CHAR_HEIGHT, view_top, view_bottom)) {
        ui_draw_text(reset_rect.x + 8.0f, reset_rect.y + 8.0f, "RESET", text);
    }
    panel_max_x = fmaxf(panel_max_x, reset_rect.x + reset_rect.w);

    if (mouse_pressed && ui_rect_contains(&apply_rect, g_ui.mouse_x, g_ui.mouse_y) && g_ui.dirty) {
        g_ui.action_apply = true;
        g_ui.action_reinit = g_ui.reinit_required;
    }
    if (mouse_pressed && ui_rect_contains(&reset_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        *g_ui.runtime = g_ui.baseline;
        g_ui.dirty = false;
        g_ui.reinit_required = false;
        g_ui.action_reset = true;
        g_ui.action_apply = true;
        g_ui.action_reinit = false;
    }

    if (g_ui.reinit_required) {
        float notice_y = reset_content_y + 40.0f - scroll;
        if (ui_range_intersects(notice_y, UI_CHAR_HEIGHT, view_top, view_bottom)) {
            ui_draw_text(text_x, notice_y, "REINIT REQUIRED", text);
        }
        panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("REINIT REQUIRED"));
    }

    float content_height = (reset_content_y + 80.0f) - panel_rect.y;
    panel_rect.h = g_ui.panel_visible_height;
    panel_rect.w = fmaxf(UI_PANEL_WIDTH, (panel_max_x - panel_rect.x) + 20.0f);
    ui_update_rect(panel_bg_start, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h);
    ui_update_rect(panel_border_start, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h);
    g_ui.mouse_over_panel = ui_rect_contains(&panel_rect, g_ui.mouse_x, g_ui.mouse_y);
    g_ui.wants_mouse = g_ui.capturing_mouse || g_ui.mouse_over_panel;
    g_ui.wants_keyboard = true;
    g_ui.panel_last_width = panel_rect.w;
    g_ui.panel_content_height = content_height;
    float max_scroll = fmaxf(0.0f, g_ui.panel_content_height - g_ui.panel_visible_height);
    g_ui.panel_scroll = ui_clampf(g_ui.panel_scroll, 0.0f, max_scroll);

    ui_draw_selected_bee_panel();

    if (g_ui.active_slider >= 0 && !mouse_down) {
        g_ui.active_slider = -1;
        g_ui.capturing_mouse = false;
    }

    g_ui.prev_mouse_down = mouse_down;
}

UiActions ui_update(const Input *input, bool sim_paused, float dt_sec) {
    (void)dt_sec;
    g_ui.sim_paused = sim_paused;

    UiActions actions = {0};
    ui_begin_frame(input);

    if (!g_ui.has_params || !g_ui.runtime) {
        return actions;
    }

    if (g_ui.action_toggle_pause) {
        actions.toggle_pause = true;
    }
    if (g_ui.action_step) {
        actions.step_once = true;
    }
    if (g_ui.action_apply) {
        actions.apply = true;
        actions.reinit_required = g_ui.action_reinit;
    }
    if (g_ui.action_reset) {
        actions.reset = true;
    }
    if (g_ui.action_focus_queen) {
        actions.focus_queen = true;
    }
    if (g_ui.action_toggle_hex_grid) {
        actions.toggle_hex_grid = true;
    }
    if (g_ui.action_toggle_hex_layer) {
        actions.toggle_hex_layer = true;
    }

    return actions;
}

void ui_render(int framebuffer_width, int framebuffer_height) {
    if (!g_ui.vertices || g_ui.vert_count == 0 || !g_ui.program) {
        return;
    }

    glUseProgram(g_ui.program);
    glUniform2f(g_ui.resolution_uniform, (float)framebuffer_width, (float)framebuffer_height);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    glBindVertexArray(g_ui.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_ui.vbo);
    glBufferData(GL_ARRAY_BUFFER, g_ui.vert_count * sizeof(UiVertex), g_ui.vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLint)g_ui.vert_count);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glUseProgram(0);
}

bool ui_wants_mouse(void) {
    return g_ui.wants_mouse;
}

bool ui_wants_keyboard(void) {
    return g_ui.wants_keyboard;
}
