#include "hex_draw.h"

#include <glad/glad.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

typedef struct HexInstance {
    float center[2];
    float scale;
    float color[4];
} HexInstance;

struct HexDrawContext {
    GLuint program;
    GLuint vao;
    GLuint corner_vbo;
    GLuint instance_vbo;
    HexInstance *cpu_instances;
    size_t capacity;
    GLint u_screen;
    GLint u_cam_center;
    GLint u_cam_zoom;
    float unit_corners[HEX_CORNER_COUNT * 2];
};

static const char *kHexVertexShaderSrc =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_corner;\n"
    "layout(location=1) in vec2 a_center_world;\n"
    "layout(location=2) in float a_scale_world;\n"
    "layout(location=3) in vec4 a_color_rgba;\n"
    "uniform vec2 u_screen;\n"
    "uniform vec2 u_cam_center;\n"
    "uniform float u_cam_zoom;\n"
    "out vec4 v_color_rgba;\n"
    "void main() {\n"
    "    vec2 pos_world = a_center_world + a_corner * a_scale_world;\n"
    "    vec2 pos_px = (pos_world - u_cam_center) * u_cam_zoom + 0.5 * u_screen;\n"
    "    vec2 ndc;\n"
    "    ndc.x = (pos_px.x / u_screen.x) * 2.0 - 1.0;\n"
    "    ndc.y = 1.0 - (pos_px.y / u_screen.y) * 2.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color_rgba = a_color_rgba;\n"
    "}\n";

static const char *kHexFragmentShaderSrc =
    "#version 330 core\n"
    "in vec4 v_color_rgba;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    frag = v_color_rgba;\n"
    "}\n";

static float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static GLuint compile_shader(GLenum type, const char *src, char *log_buf, size_t log_cap) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        if (log_buf && log_cap > 0) {
            glGetShaderInfoLog(shader, (GLsizei)log_cap, NULL, log_buf);
        }
        glDeleteShader(shader);
        return 0;
    }
    if (log_buf && log_cap > 0) {
        log_buf[0] = '\0';
    }
    return shader;
}

static GLuint link_program(GLuint vs, GLuint fs, char *log_buf, size_t log_cap) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        if (log_buf && log_cap > 0) {
            glGetProgramInfoLog(program, (GLsizei)log_cap, NULL, log_buf);
        }
        glDeleteProgram(program);
        return 0;
    }
    if (log_buf && log_cap > 0) {
        log_buf[0] = '\0';
    }
    return program;
}

static void fill_unit_corners(float *dst) {
    if (!dst) {
        return;
    }
    for (int i = 0; i < HEX_CORNER_COUNT; ++i) {
        float dx = 0.0f;
        float dy = 0.0f;
        hex_corner_offset(1.0f, i, &dx, &dy);
        dst[i * 2 + 0] = dx;
        dst[i * 2 + 1] = dy;
    }
}

bool hex_draw_init(HexDrawContext **out_ctx) {
    if (!out_ctx) {
        return false;
    }
    HexDrawContext *ctx = (HexDrawContext *)calloc(1, sizeof(HexDrawContext));
    if (!ctx) {
        return false;
    }

    char log_buffer[1024] = {0};
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kHexVertexShaderSrc, log_buffer, sizeof(log_buffer));
    if (!vs) {
        LOG_ERROR("hex_draw: vertex shader compile failed:\n%s", log_buffer);
        free(ctx);
        return false;
    }
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kHexFragmentShaderSrc, log_buffer, sizeof(log_buffer));
    if (!fs) {
        LOG_ERROR("hex_draw: fragment shader compile failed:\n%s", log_buffer);
        glDeleteShader(vs);
        free(ctx);
        return false;
    }
    ctx->program = link_program(vs, fs, log_buffer, sizeof(log_buffer));
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ctx->program) {
        LOG_ERROR("hex_draw: program link failed:\n%s", log_buffer);
        free(ctx);
        return false;
    }

    ctx->u_screen = glGetUniformLocation(ctx->program, "u_screen");
    ctx->u_cam_center = glGetUniformLocation(ctx->program, "u_cam_center");
    ctx->u_cam_zoom = glGetUniformLocation(ctx->program, "u_cam_zoom");

    glGenVertexArrays(1, &ctx->vao);
    glGenBuffers(1, &ctx->corner_vbo);
    glGenBuffers(1, &ctx->instance_vbo);

    fill_unit_corners(ctx->unit_corners);

    glBindVertexArray(ctx->vao);

    glBindBuffer(GL_ARRAY_BUFFER, ctx->corner_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 sizeof(ctx->unit_corners),
                 ctx->unit_corners,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

    glBindBuffer(GL_ARRAY_BUFFER, ctx->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(HexInstance), (void *)offsetof(HexInstance, center));
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(HexInstance), (void *)offsetof(HexInstance, scale));
    glVertexAttribDivisor(2, 1);

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(HexInstance), (void *)offsetof(HexInstance, color));
    glVertexAttribDivisor(3, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    *out_ctx = ctx;
    LOG_INFO("hex_draw: initialized hex renderer");
    return true;
}

void hex_draw_shutdown(HexDrawContext **ctx_ptr) {
    if (!ctx_ptr || !*ctx_ptr) {
        return;
    }
    HexDrawContext *ctx = *ctx_ptr;
    glDeleteBuffers(1, &ctx->corner_vbo);
    glDeleteBuffers(1, &ctx->instance_vbo);
    glDeleteVertexArrays(1, &ctx->vao);
    if (ctx->program) {
        glDeleteProgram(ctx->program);
    }
    free(ctx->cpu_instances);
    free(ctx);
    *ctx_ptr = NULL;
}

static bool ensure_capacity(HexDrawContext *ctx, size_t desired) {
    if (desired == 0) {
        return true;
    }
    if (desired <= ctx->capacity) {
        return true;
    }
    size_t new_capacity = ctx->capacity ? ctx->capacity : 256;
    while (new_capacity < desired) {
        new_capacity *= 2;
    }
    HexInstance *new_mem = (HexInstance *)realloc(ctx->cpu_instances, new_capacity * sizeof(HexInstance));
    if (!new_mem) {
        LOG_ERROR("hex_draw: failed to allocate %zu instances", new_capacity);
        return false;
    }
    ctx->cpu_instances = new_mem;
    ctx->capacity = new_capacity;
    return true;
}

bool hex_draw_render(HexDrawContext *ctx,
                     const RenderHexParams *params,
                     int fb_width,
                     int fb_height,
                     const float cam_center[2],
                     float cam_zoom) {
    if (!ctx || !params || !params->enabled) {
        return false;
    }
    const HexWorld *world = params->world;
    if (!world || !world->tiles || world->count == 0) {
        return false;
    }
    if (fb_width <= 0 || fb_height <= 0 || cam_zoom <= 0.0f) {
        return false;
    }

    size_t visible_count = 0;
    const size_t total = world->count;
    for (size_t i = 0; i < total; ++i) {
        const HexTile *tile = &world->tiles[i];
        if (tile->flags & HEX_TILE_VISIBLE) {
            ++visible_count;
        }
    }

    if (visible_count == 0) {
        return false;
    }
    if (!ensure_capacity(ctx, visible_count)) {
        return false;
    }

    const bool selected_valid = params->selected_index != SIZE_MAX && params->selected_index < world->count;
    HexInstance highlight = {0};
    bool highlight_ready = false;

    size_t cursor = 0;
    for (size_t i = 0; i < total; ++i) {
        const HexTile *tile = &world->tiles[i];
        if ((tile->flags & HEX_TILE_VISIBLE) == 0) {
            continue;
        }
        HexInstance *inst = &ctx->cpu_instances[cursor++];
        inst->center[0] = tile->center_x;
        inst->center[1] = tile->center_y;
        inst->scale = world->cell_size;
        float color[4];
        hex_tile_palette(tile->terrain, color);
        if (tile->terrain == HEX_TERRAIN_ENTRANCE) {
            inst->scale = world->cell_size * 1.02f;
        }
        bool is_selected = selected_valid && i == params->selected_index;
        if (is_selected) {
            color[0] = clampf(color[0] * 1.3f, 0.0f, 1.0f);
            color[1] = clampf(color[1] * 1.3f, 0.0f, 1.0f);
            color[2] = clampf(color[2] * 1.3f, 0.0f, 1.0f);
            color[3] = 1.0f;
        }
        inst->color[0] = color[0];
        inst->color[1] = color[1];
        inst->color[2] = color[2];
        inst->color[3] = color[3];

        if (is_selected) {
            highlight = *inst;
            highlight.scale = inst->scale * 1.03f;
            highlight.color[0] = 1.0f;
            highlight.color[1] = 1.0f;
            highlight.color[2] = 1.0f;
            highlight.color[3] = 1.0f;
            highlight_ready = true;
        }
    }

    glUseProgram(ctx->program);
    glUniform2f(ctx->u_screen, (float)fb_width, (float)fb_height);
    glUniform2f(ctx->u_cam_center, cam_center ? cam_center[0] : 0.0f, cam_center ? cam_center[1] : 0.0f);
    glUniform1f(ctx->u_cam_zoom, cam_zoom);

    glBindVertexArray(ctx->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(visible_count * sizeof(HexInstance)),
                 ctx->cpu_instances,
                 GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, HEX_CORNER_COUNT, (GLsizei)visible_count);

    if (highlight_ready) {
        glBufferData(GL_ARRAY_BUFFER, sizeof(HexInstance), &highlight, GL_STREAM_DRAW);
        glLineWidth(3.0f);
        glDrawArraysInstanced(GL_LINE_LOOP, 0, HEX_CORNER_COUNT, 1);
        glLineWidth(1.0f);
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    return true;
}
