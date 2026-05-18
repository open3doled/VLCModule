#define _GNU_SOURCE

/*
 * Minimal Open3D Wayland scanout cadence probe.
 *
 * This intentionally avoids VLC/Qt. It creates a fullscreen xdg-shell EGL
 * surface, alternates red/blue frames, draws the Open3D-style optical trigger
 * boxes, and records wl_surface.frame + wp_presentation timing. A stable run
 * with presentation flags containing zero_copy/hw_completion and clean optical
 * timing is the practical proof that Mutter is giving this surface the direct
 * scanout / hardware-overlay path.
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "xdg-shell-client-protocol.h"
#include "presentation-time-client-protocol.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

typedef struct
{
    const char *output_substr;
    const char *csv_path;
    double base_hz_override;
    unsigned seconds;
    unsigned repeat;
    unsigned trigger_size;
    unsigned trigger_padding;
    unsigned trigger_spacing;
    unsigned trigger_border;
    int trigger_offset_x;
    int trigger_offset_y;
    int trigger_brightness;
    const char *trigger_corner;
    bool trigger_invert;
    bool list_only;
    bool no_trigger_boxes;
    bool windowed;
    int width_override;
    int height_override;
} open3d_probe_opts_t;

typedef struct
{
    struct wl_output *wl_output;
    uint32_t global_name;
    uint32_t version;
    char name[128];
    char description[256];
    char make[128];
    char model[128];
    int32_t width;
    int32_t height;
    int32_t refresh_mhz;
    int32_t scale;
    bool current_mode;
} open3d_output_t;

typedef struct open3d_feedback
{
    struct wp_presentation_feedback *feedback;
    struct open3d_feedback *next;
    uint64_t frame_index;
    uint64_t submit_mono_ns;
    uint64_t frame_callback_mono_ns;
    uint32_t frame_callback_time_ms;
    bool blue;
} open3d_feedback_t;

typedef struct
{
    open3d_probe_opts_t opts;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct wp_presentation *presentation;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_egl_window *egl_window;
    struct wl_callback *frame_callback;

    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLContext egl_context;
    EGLSurface egl_surface;

    open3d_output_t outputs[16];
    size_t output_count;
    open3d_output_t *target_output;

    open3d_feedback_t *pending_feedback;

    FILE *csv;
    uint64_t start_mono_ns;
    uint64_t end_mono_ns;
    uint64_t last_report_mono_ns;
    uint64_t submitted_frames;
    uint64_t presented_frames;
    uint64_t discarded_frames;
    uint64_t frame_callbacks;
    uint64_t sequence_jump_total;
    uint64_t last_sequence;
    uint64_t last_present_time_ns;
    uint64_t zero_copy_frames;
    uint64_t hw_completion_frames;
    uint64_t hw_clock_frames;
    uint64_t vsync_frames;
    uint64_t callback_interval_miss;
    uint64_t last_callback_mono_ns;
    uint64_t max_callback_interval_ns;
    uint64_t max_present_interval_ns;
    uint64_t max_sequence_delta;

    int width;
    int height;
    int buffer_scale;
    bool configured;
    bool running;
    bool first_frame_submitted;
} open3d_app_t;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double output_hz(const open3d_output_t *out)
{
    return out != NULL && out->refresh_mhz > 0 ? (double)out->refresh_mhz / 1000.0 : 0.0;
}

static double effective_base_hz(const open3d_app_t *app)
{
    if (app != NULL && app->opts.base_hz_override > 1.0)
        return app->opts.base_hz_override;
    return app != NULL ? output_hz(app->target_output) : 0.0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--list] [--seconds N] [--output NAME_SUBSTR]\n"
            "          [--base-hz HZ] [--repeat N] [--csv PATH] [--windowed]\n"
            "          [--size WxH] [--no-trigger-boxes]\n"
            "          [--trigger-size PX] [--trigger-padding PX]\n"
            "          [--trigger-spacing PX] [--trigger-border PX]\n"
            "          [--trigger-offset-x PX] [--trigger-offset-y PX]\n"
            "          [--trigger-corner top-left|top-right|bottom-left|bottom-right]\n"
            "          [--trigger-brightness 0-255] [--trigger-invert]\n\n"
            "Defaults: fullscreen, repeat=1, Open3D trigger-box geometry.\n"
            "Use --repeat 2 to simulate 240/120 page-flip hold cadence.\n",
            argv0);
}

static bool parse_size(const char *text, int *width, int *height)
{
    if (text == NULL || width == NULL || height == NULL)
        return false;
    char *x = strchr(text, 'x');
    if (x == NULL)
        x = strchr(text, 'X');
    if (x == NULL)
        return false;

    char *end = NULL;
    long w = strtol(text, &end, 10);
    if (end != x)
        return false;
    long h = strtol(x + 1, &end, 10);
    if (end == x + 1 || *end != '\0')
        return false;
    if (w < 64 || h < 64 || w > 16384 || h > 16384)
        return false;
    *width = (int)w;
    *height = (int)h;
    return true;
}

static bool parse_opts(int argc, char **argv, open3d_probe_opts_t *opts)
{
    *opts = (open3d_probe_opts_t) {
        .output_substr = NULL,
        .csv_path = NULL,
        .base_hz_override = 0.0,
        .seconds = 10,
        .repeat = 1,
        .trigger_size = 13,
        .trigger_padding = 10,
        .trigger_spacing = 23,
        .trigger_border = 10,
        .trigger_offset_x = 0,
        .trigger_offset_y = 0,
        .trigger_brightness = 255,
        .trigger_corner = "top-left",
        .trigger_invert = false,
        .list_only = false,
        .no_trigger_boxes = false,
        .windowed = false,
        .width_override = 0,
        .height_override = 0,
    };

    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--list"))
            opts->list_only = true;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            opts->seconds = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--output") && i + 1 < argc)
            opts->output_substr = argv[++i];
        else if (!strcmp(argv[i], "--base-hz") && i + 1 < argc)
            opts->base_hz_override = atof(argv[++i]);
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc)
            opts->repeat = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc)
            opts->csv_path = argv[++i];
        else if (!strcmp(argv[i], "--windowed"))
            opts->windowed = true;
        else if (!strcmp(argv[i], "--no-trigger-boxes"))
            opts->no_trigger_boxes = true;
        else if (!strcmp(argv[i], "--trigger-size") && i + 1 < argc)
            opts->trigger_size = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--trigger-padding") && i + 1 < argc)
            opts->trigger_padding = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--trigger-spacing") && i + 1 < argc)
            opts->trigger_spacing = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--trigger-border") && i + 1 < argc)
            opts->trigger_border = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--trigger-offset-x") && i + 1 < argc)
            opts->trigger_offset_x = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--trigger-offset-y") && i + 1 < argc)
            opts->trigger_offset_y = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--trigger-brightness") && i + 1 < argc)
            opts->trigger_brightness = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--trigger-corner") && i + 1 < argc)
            opts->trigger_corner = argv[++i];
        else if (!strcmp(argv[i], "--trigger-invert"))
            opts->trigger_invert = true;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)
        {
            if (!parse_size(argv[++i], &opts->width_override, &opts->height_override))
            {
                fprintf(stderr, "invalid --size, expected WxH\n");
                return false;
            }
        }
        else
        {
            usage(argv[0]);
            return false;
        }
    }

    if (opts->seconds == 0)
        opts->seconds = 10;
    if (opts->repeat == 0)
        opts->repeat = 1;
    if (opts->trigger_size == 0)
        opts->trigger_size = 1;
    if (opts->trigger_brightness < 0)
        opts->trigger_brightness = 0;
    if (opts->trigger_brightness > 255)
        opts->trigger_brightness = 255;
    return true;
}

static void csv_open(open3d_app_t *app)
{
    if (app->opts.csv_path == NULL || !strcmp(app->opts.csv_path, "-"))
        app->csv = stdout;
    else
        app->csv = fopen(app->opts.csv_path, "w");

    if (app->csv == NULL)
    {
        fprintf(stderr, "failed to open CSV %s: %s\n",
                app->opts.csv_path, strerror(errno));
        return;
    }

    fputs("event,mono_ns,frame_index,color,width,height,callback_time_ms,"
          "submit_ns,present_time_ns,present_interval_ns,refresh_ns,"
          "sequence,sequence_delta,flags,vsync,hw_clock,hw_completion,"
          "zero_copy,discarded,total_sequence_jumps\n",
          app->csv);
    fflush(app->csv);
}

static void csv_close(open3d_app_t *app)
{
    if (app->csv != NULL && app->csv != stdout)
        fclose(app->csv);
    app->csv = NULL;
}

static void csv_log_submit(open3d_app_t *app, uint64_t frame_index, bool blue,
                           uint32_t callback_time_ms, uint64_t submit_ns)
{
    if (app->csv == NULL)
        return;
    fprintf(app->csv,
            "submit,%"PRIu64",%"PRIu64",%s,%d,%d,%u,%"PRIu64",,,,,,,,,,,,\n",
            mono_ns(), frame_index, blue ? "blue" : "red", app->width, app->height,
            callback_time_ms, submit_ns);
}

static void csv_log_present(open3d_app_t *app, const open3d_feedback_t *item,
                            uint64_t present_time_ns, uint64_t interval_ns,
                            uint64_t refresh_ns, uint64_t sequence,
                            uint64_t sequence_delta, uint32_t flags,
                            bool discarded)
{
    if (app->csv == NULL || item == NULL)
        return;
    const bool vsync = (flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC) != 0;
    const bool hw_clock = (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK) != 0;
    const bool hw_completion = (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION) != 0;
    const bool zero_copy = (flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY) != 0;
    fprintf(app->csv,
            "%s,%"PRIu64",%"PRIu64",%s,%d,%d,%u,%"PRIu64",%"PRIu64",%"PRIu64","
            "%"PRIu64",%"PRIu64",%"PRIu64",0x%08x,%d,%d,%d,%d,%d,%"PRIu64"\n",
            discarded ? "discard" : "present",
            mono_ns(), item->frame_index, item->blue ? "blue" : "red",
            app->width, app->height, item->frame_callback_time_ms,
            item->submit_mono_ns, present_time_ns, interval_ns, refresh_ns,
            sequence, sequence_delta, flags, vsync ? 1 : 0, hw_clock ? 1 : 0,
            hw_completion ? 1 : 0, zero_copy ? 1 : 0, discarded ? 1 : 0,
            app->sequence_jump_total);
}

static void output_geometry(void *data, struct wl_output *wl_output,
                            int32_t x, int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{
    (void)wl_output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)transform;
    open3d_output_t *out = data;
    snprintf(out->make, sizeof(out->make), "%s", make != NULL ? make : "");
    snprintf(out->model, sizeof(out->model), "%s", model != NULL ? model : "");
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)wl_output;
    open3d_output_t *out = data;
    if ((flags & WL_OUTPUT_MODE_CURRENT) || !out->current_mode)
    {
        out->width = width;
        out->height = height;
        out->refresh_mhz = refresh;
        out->current_mode = (flags & WL_OUTPUT_MODE_CURRENT) != 0;
    }
}

static void output_done(void *data, struct wl_output *wl_output)
{
    (void)data;
    (void)wl_output;
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    (void)wl_output;
    open3d_output_t *out = data;
    out->scale = factor > 0 ? factor : 1;
}

static void output_name(void *data, struct wl_output *wl_output, const char *name)
{
    (void)wl_output;
    open3d_output_t *out = data;
    snprintf(out->name, sizeof(out->name), "%s", name != NULL ? name : "");
}

static void output_description(void *data, struct wl_output *wl_output, const char *description)
{
    (void)wl_output;
    open3d_output_t *out = data;
    snprintf(out->description, sizeof(out->description), "%s",
             description != NULL ? description : "");
}

static const struct wl_output_listener output_listener = {
    output_geometry,
    output_mode,
    output_done,
    output_scale,
    output_name,
    output_description,
};

static bool output_matches(const open3d_output_t *out, const char *needle)
{
    if (needle == NULL || needle[0] == '\0')
        return true;
    return (out->name[0] && strcasestr(out->name, needle)) ||
           (out->description[0] && strcasestr(out->description, needle)) ||
           (out->make[0] && strcasestr(out->make, needle)) ||
           (out->model[0] && strcasestr(out->model, needle));
}

static void print_outputs(const open3d_app_t *app)
{
    for (size_t i = 0; i < app->output_count; ++i)
    {
        const open3d_output_t *out = &app->outputs[i];
        printf("[%zu] name=%s description=%s make=%s model=%s mode=%dx%d %.3fHz scale=%d global=%u\n",
               i,
               out->name[0] ? out->name : "(unknown)",
               out->description[0] ? out->description : "(none)",
               out->make[0] ? out->make : "(unknown)",
               out->model[0] ? out->model : "(unknown)",
               out->width, out->height, output_hz(out),
               out->scale > 0 ? out->scale : 1, out->global_name);
    }
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    open3d_app_t *app = data;
    if (!strcmp(interface, wl_compositor_interface.name))
    {
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                           version < 4 ? version : 4);
    }
    else if (!strcmp(interface, xdg_wm_base_interface.name))
    {
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    }
    else if (!strcmp(interface, wp_presentation_interface.name))
    {
        app->presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
    }
    else if (!strcmp(interface, wl_output_interface.name))
    {
        if (app->output_count >= ARRAY_SIZE(app->outputs))
            return;
        open3d_output_t *out = &app->outputs[app->output_count++];
        memset(out, 0, sizeof(*out));
        out->global_name = name;
        out->version = version < 4 ? version : 4;
        out->scale = 1;
        out->wl_output = wl_registry_bind(registry, name, &wl_output_interface, out->version);
        wl_output_add_listener(out->wl_output, &output_listener, out);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_remove,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    wm_base_ping,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)toplevel;
    (void)states;
    open3d_app_t *app = data;
    if (width > 0 && height > 0)
    {
        app->width = width * app->buffer_scale;
        app->height = height * app->buffer_scale;
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    open3d_app_t *app = data;
    app->running = false;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
                                      int32_t width, int32_t height)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    toplevel_configure,
    toplevel_close,
    toplevel_configure_bounds,
};

static void draw_rect(int fb_height, int x, int y_top, int w, int h,
                      float r, float g, float b)
{
    if (w <= 0 || h <= 0)
        return;
    int y = fb_height - y_top - h;
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if (h <= 0)
        return;
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void draw_trigger_boxes(open3d_app_t *app, bool blue)
{
    if (app->opts.no_trigger_boxes)
        return;

    const int size = (int)app->opts.trigger_size;
    const int padding = (int)app->opts.trigger_padding;
    const int spacing = (int)app->opts.trigger_spacing;
    const int border = (int)app->opts.trigger_border;
    const int step = size + spacing;
    int x_primary = padding;
    int y = padding;
    int direction = 1;

    if (!strcmp(app->opts.trigger_corner, "top-right"))
    {
        x_primary = app->width - padding - size;
        y = padding;
        direction = -1;
    }
    else if (!strcmp(app->opts.trigger_corner, "bottom-left"))
    {
        x_primary = padding;
        y = app->height - padding - size;
        direction = 1;
    }
    else if (!strcmp(app->opts.trigger_corner, "bottom-right"))
    {
        x_primary = app->width - padding - size;
        y = app->height - padding - size;
        direction = -1;
    }

    x_primary += app->opts.trigger_offset_x;
    y += app->opts.trigger_offset_y;
    if (x_primary < 0)
        x_primary = 0;
    if (y < 0)
        y = 0;
    if (x_primary + size > app->width)
        x_primary = app->width > size ? app->width - size : 0;
    if (y + size > app->height)
        y = app->height > size ? app->height - size : 0;

    int x_secondary = x_primary + direction * step;
    if (x_secondary < 0 || x_secondary + size > app->width)
    {
        const int opposite = x_primary - direction * step;
        if (opposite >= 0 && opposite + size <= app->width)
            x_secondary = opposite;
    }
    if (x_secondary < 0)
        x_secondary = 0;
    if (x_secondary + size > app->width)
        x_secondary = app->width > size ? app->width - size : 0;

    bool primary_white = !blue;
    if (app->opts.trigger_invert)
        primary_white = !primary_white;
    const float white = (float)app->opts.trigger_brightness / 255.0f;

    if (border > 0)
    {
        const int x_left = x_primary < x_secondary ? x_primary : x_secondary;
        const int x_right = x_primary > x_secondary ? x_primary : x_secondary;
        draw_rect(app->height, x_left - border, y - border,
                  (x_right - x_left) + size + 2 * border,
                  size + 2 * border, 0.0f, 0.0f, 0.0f);
    }

    draw_rect(app->height, x_primary, y, size, size,
              primary_white ? white : 0.0f,
              primary_white ? white : 0.0f,
              primary_white ? white : 0.0f);
    draw_rect(app->height, x_secondary, y, size, size,
              primary_white ? 0.0f : white,
              primary_white ? 0.0f : white,
              primary_white ? 0.0f : white);
}

static bool frame_is_blue(const open3d_app_t *app, uint64_t frame_index)
{
    const uint64_t phase = (frame_index / app->opts.repeat) & 1u;
    return phase != 0;
}

static void feedback_unlink(open3d_app_t *app, open3d_feedback_t *item)
{
    open3d_feedback_t **it = &app->pending_feedback;
    while (*it != NULL)
    {
        if (*it == item)
        {
            *it = item->next;
            return;
        }
        it = &(*it)->next;
    }
}

static void feedback_presented(void *data, struct wp_presentation_feedback *feedback,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
                               uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
                               uint32_t flags)
{
    open3d_feedback_t *item = data;
    open3d_app_t *app = NULL;
    /*
     * The app pointer is stored by walking from a process-global singleton to keep
     * this tiny diagnostic simple and avoid extending protocol user data structs.
     */
    extern open3d_app_t *open3d_scanout_probe_singleton;
    app = open3d_scanout_probe_singleton;

    const uint64_t present_time_ns =
        (((uint64_t)tv_sec_hi << 32) | (uint64_t)tv_sec_lo) * 1000000000ull + tv_nsec;
    const uint64_t sequence = ((uint64_t)seq_hi << 32) | seq_lo;
    uint64_t sequence_delta = app->last_sequence != 0 && sequence >= app->last_sequence
                                  ? sequence - app->last_sequence
                                  : 0;
    uint64_t interval_ns = app->last_present_time_ns != 0 &&
                                   present_time_ns >= app->last_present_time_ns
                               ? present_time_ns - app->last_present_time_ns
                               : 0;

    if (sequence_delta > 1)
        app->sequence_jump_total += sequence_delta - 1;
    if (sequence_delta > app->max_sequence_delta)
        app->max_sequence_delta = sequence_delta;
    if (interval_ns > app->max_present_interval_ns)
        app->max_present_interval_ns = interval_ns;

    app->last_sequence = sequence;
    app->last_present_time_ns = present_time_ns;
    app->presented_frames++;
    if (flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC)
        app->vsync_frames++;
    if (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK)
        app->hw_clock_frames++;
    if (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION)
        app->hw_completion_frames++;
    if (flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY)
        app->zero_copy_frames++;

    csv_log_present(app, item, present_time_ns, interval_ns, refresh, sequence,
                    sequence_delta, flags, false);

    feedback_unlink(app, item);
    wp_presentation_feedback_destroy(feedback);
    free(item);
}

static void feedback_discarded(void *data, struct wp_presentation_feedback *feedback)
{
    open3d_feedback_t *item = data;
    extern open3d_app_t *open3d_scanout_probe_singleton;
    open3d_app_t *app = open3d_scanout_probe_singleton;

    app->discarded_frames++;
    csv_log_present(app, item, 0, 0, 0, 0, 0, 0, true);

    feedback_unlink(app, item);
    wp_presentation_feedback_destroy(feedback);
    free(item);
}

static void feedback_sync_output(void *data, struct wp_presentation_feedback *feedback,
                                 struct wl_output *output)
{
    (void)data;
    (void)feedback;
    (void)output;
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    feedback_sync_output,
    feedback_presented,
    feedback_discarded,
};

static void maybe_print_report(open3d_app_t *app)
{
    uint64_t now = mono_ns();
    if (app->last_report_mono_ns != 0 &&
        now - app->last_report_mono_ns < 1000000000ull)
        return;
    app->last_report_mono_ns = now;

    fprintf(stderr,
            "frames submitted=%"PRIu64" presented=%"PRIu64" discarded=%"PRIu64
            " callbacks=%"PRIu64" seq_jumps=%"PRIu64" zero_copy=%"PRIu64
            " hw_completion=%"PRIu64" max_present_ms=%.3f max_cb_ms=%.3f\n",
            app->submitted_frames, app->presented_frames, app->discarded_frames,
            app->frame_callbacks, app->sequence_jump_total, app->zero_copy_frames,
            app->hw_completion_frames,
            (double)app->max_present_interval_ns / 1000000.0,
            (double)app->max_callback_interval_ns / 1000000.0);
}

static const struct wl_callback_listener frame_listener;

static void submit_frame(open3d_app_t *app, uint32_t callback_time_ms)
{
    if (!app->configured || !app->running)
        return;

    if (mono_ns() >= app->end_mono_ns)
    {
        app->running = false;
        return;
    }

    if (!app->first_frame_submitted && app->egl_window != NULL)
    {
        wl_egl_window_resize(app->egl_window, app->width, app->height, 0, 0);
        app->first_frame_submitted = true;
    }

    const uint64_t frame_index = app->submitted_frames;
    const bool blue = frame_is_blue(app, frame_index);

    glViewport(0, 0, app->width, app->height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glScissor(0, 0, app->width, app->height);
    glEnable(GL_SCISSOR_TEST);
    if (blue)
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    else
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_trigger_boxes(app, blue);
    glDisable(GL_SCISSOR_TEST);

    open3d_feedback_t *item = calloc(1, sizeof(*item));
    if (item != NULL)
    {
        item->frame_index = frame_index;
        item->blue = blue;
        item->frame_callback_time_ms = callback_time_ms;
        item->frame_callback_mono_ns = app->last_callback_mono_ns;
        item->submit_mono_ns = mono_ns();
        item->next = app->pending_feedback;
        app->pending_feedback = item;
        if (app->presentation != NULL)
        {
            item->feedback = wp_presentation_feedback(app->presentation, app->surface);
            wp_presentation_feedback_add_listener(item->feedback, &feedback_listener, item);
        }
    }

    app->frame_callback = wl_surface_frame(app->surface);
    wl_callback_add_listener(app->frame_callback, &frame_listener, app);

    csv_log_submit(app, frame_index, blue, callback_time_ms, mono_ns());
    if (!eglSwapBuffers(app->egl_display, app->egl_surface))
    {
        fprintf(stderr, "eglSwapBuffers failed: 0x%x\n", eglGetError());
        app->running = false;
        return;
    }
    app->submitted_frames++;
    maybe_print_report(app);
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t callback_data)
{
    open3d_app_t *app = data;
    if (callback != NULL)
        wl_callback_destroy(callback);
    if (app->frame_callback == callback)
        app->frame_callback = NULL;

    const uint64_t now = mono_ns();
    if (app->last_callback_mono_ns != 0)
    {
        const uint64_t interval = now - app->last_callback_mono_ns;
        if (interval > app->max_callback_interval_ns)
            app->max_callback_interval_ns = interval;
        const double hz = effective_base_hz(app);
        if (hz > 1.0)
        {
            const uint64_t expected = (uint64_t)llround(1000000000.0 / hz);
            if (interval > expected + expected / 2)
                app->callback_interval_miss++;
        }
    }
    app->last_callback_mono_ns = now;
    app->frame_callbacks++;
    submit_frame(app, callback_data);
}

static const struct wl_callback_listener frame_listener = {
    frame_done,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial)
{
    open3d_app_t *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    app->configured = true;

    if (app->width <= 0 || app->height <= 0)
    {
        app->width = app->opts.width_override > 0
                         ? app->opts.width_override
                         : (app->target_output != NULL && app->target_output->width > 0
                                ? app->target_output->width
                                : 1920);
        app->height = app->opts.height_override > 0
                          ? app->opts.height_override
                          : (app->target_output != NULL && app->target_output->height > 0
                                 ? app->target_output->height
                                 : 1080);
    }

    if (!app->first_frame_submitted)
        submit_frame(app, 0);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_configure,
};

open3d_app_t *open3d_scanout_probe_singleton;

static bool init_wayland(open3d_app_t *app)
{
    app->display = wl_display_connect(NULL);
    if (app->display == NULL)
    {
        fprintf(stderr, "wl_display_connect failed; are you in a Wayland session?\n");
        return false;
    }

    app->registry = wl_display_get_registry(app->display);
    wl_registry_add_listener(app->registry, &registry_listener, app);
    wl_display_roundtrip(app->display);
    wl_display_roundtrip(app->display);

    if (app->opts.list_only)
    {
        print_outputs(app);
        return true;
    }

    if (app->compositor == NULL || app->wm_base == NULL)
    {
        fprintf(stderr, "missing required Wayland globals: compositor=%d xdg_wm_base=%d\n",
                app->compositor != NULL, app->wm_base != NULL);
        return false;
    }
    xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);

    for (size_t i = 0; i < app->output_count; ++i)
    {
        if (output_matches(&app->outputs[i], app->opts.output_substr))
        {
            app->target_output = &app->outputs[i];
            break;
        }
    }
    if (app->target_output == NULL && app->output_count > 0)
        app->target_output = &app->outputs[0];
    if (app->target_output == NULL)
    {
        fprintf(stderr, "no wl_output advertised by compositor\n");
        return false;
    }

    app->buffer_scale = app->target_output->scale > 0 ? app->target_output->scale : 1;
    app->width = app->opts.width_override > 0 ? app->opts.width_override : app->target_output->width;
    app->height = app->opts.height_override > 0 ? app->opts.height_override : app->target_output->height;
    if (app->width <= 0)
        app->width = 1920;
    if (app->height <= 0)
        app->height = 1080;

    fprintf(stderr,
            "target output name=%s description=%s mode=%dx%d %.3fHz effective_base=%.6fHz target_flip=%.6fHz scale=%d presentation=%s repeat=%u\n",
            app->target_output->name[0] ? app->target_output->name : "(unknown)",
            app->target_output->description[0] ? app->target_output->description : "(none)",
            app->target_output->width, app->target_output->height,
            output_hz(app->target_output), effective_base_hz(app),
            effective_base_hz(app) / (double)app->opts.repeat, app->buffer_scale,
            app->presentation != NULL ? "available" : "unavailable",
            app->opts.repeat);

    app->surface = wl_compositor_create_surface(app->compositor);
    wl_surface_set_buffer_scale(app->surface, app->buffer_scale);

    struct wl_region *opaque = wl_compositor_create_region(app->compositor);
    if (opaque != NULL)
    {
        wl_region_add(opaque, 0, 0, app->width / app->buffer_scale,
                      app->height / app->buffer_scale);
        wl_surface_set_opaque_region(app->surface, opaque);
        wl_region_destroy(opaque);
    }

    app->xdg_surface = xdg_wm_base_get_xdg_surface(app->wm_base, app->surface);
    xdg_surface_add_listener(app->xdg_surface, &xdg_surface_listener, app);
    app->toplevel = xdg_surface_get_toplevel(app->xdg_surface);
    xdg_toplevel_add_listener(app->toplevel, &toplevel_listener, app);
    xdg_toplevel_set_title(app->toplevel, "Open3D Wayland scanout probe");
    xdg_toplevel_set_app_id(app->toplevel, "open3d-wayland-scanout-probe");
    if (app->opts.windowed)
        xdg_toplevel_set_min_size(app->toplevel, app->width / app->buffer_scale,
                                  app->height / app->buffer_scale);
    else
        xdg_toplevel_set_fullscreen(app->toplevel, app->target_output->wl_output);
    wl_surface_commit(app->surface);
    return true;
}

static bool init_egl(open3d_app_t *app)
{
    app->egl_display = eglGetDisplay((EGLNativeDisplayType)app->display);
    if (app->egl_display == EGL_NO_DISPLAY)
    {
        fprintf(stderr, "eglGetDisplay failed\n");
        return false;
    }
    if (!eglInitialize(app->egl_display, NULL, NULL))
    {
        fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(app->egl_display, attrs, &app->egl_config, 1, &n) || n < 1)
    {
        fprintf(stderr, "eglChooseConfig failed: 0x%x\n", eglGetError());
        return false;
    }

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    app->egl_context = eglCreateContext(app->egl_display, app->egl_config,
                                        EGL_NO_CONTEXT, ctx_attrs);
    if (app->egl_context == EGL_NO_CONTEXT)
    {
        fprintf(stderr, "eglCreateContext failed: 0x%x\n", eglGetError());
        return false;
    }

    app->egl_window = wl_egl_window_create(app->surface, app->width, app->height);
    if (app->egl_window == NULL)
    {
        fprintf(stderr, "wl_egl_window_create failed\n");
        return false;
    }
    app->egl_surface = eglCreateWindowSurface(app->egl_display, app->egl_config,
                                              (EGLNativeWindowType)app->egl_window,
                                              NULL);
    if (app->egl_surface == EGL_NO_SURFACE)
    {
        fprintf(stderr, "eglCreateWindowSurface failed: 0x%x\n", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(app->egl_display, app->egl_surface, app->egl_surface,
                        app->egl_context))
    {
        fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError());
        return false;
    }
    eglSwapInterval(app->egl_display, 1);
    return true;
}

static void cleanup(open3d_app_t *app)
{
    while (app->pending_feedback != NULL)
    {
        open3d_feedback_t *next = app->pending_feedback->next;
        if (app->pending_feedback->feedback != NULL)
            wp_presentation_feedback_destroy(app->pending_feedback->feedback);
        free(app->pending_feedback);
        app->pending_feedback = next;
    }
    if (app->frame_callback != NULL)
        wl_callback_destroy(app->frame_callback);

    if (app->egl_display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(app->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (app->egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(app->egl_display, app->egl_surface);
        if (app->egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(app->egl_display, app->egl_context);
        eglTerminate(app->egl_display);
    }
    if (app->egl_window != NULL)
        wl_egl_window_destroy(app->egl_window);
    if (app->toplevel != NULL)
        xdg_toplevel_destroy(app->toplevel);
    if (app->xdg_surface != NULL)
        xdg_surface_destroy(app->xdg_surface);
    if (app->surface != NULL)
        wl_surface_destroy(app->surface);
    if (app->presentation != NULL)
        wp_presentation_destroy(app->presentation);
    if (app->wm_base != NULL)
        xdg_wm_base_destroy(app->wm_base);
    for (size_t i = 0; i < app->output_count; ++i)
    {
        if (app->outputs[i].wl_output != NULL)
            wl_output_destroy(app->outputs[i].wl_output);
    }
    if (app->compositor != NULL)
        wl_compositor_destroy(app->compositor);
    if (app->registry != NULL)
        wl_registry_destroy(app->registry);
    if (app->display != NULL)
        wl_display_disconnect(app->display);
    csv_close(app);
}

int main(int argc, char **argv)
{
    open3d_app_t app;
    memset(&app, 0, sizeof(app));
    app.egl_display = EGL_NO_DISPLAY;
    app.egl_context = EGL_NO_CONTEXT;
    app.egl_surface = EGL_NO_SURFACE;

    if (!parse_opts(argc, argv, &app.opts))
        return 2;

    open3d_scanout_probe_singleton = &app;

    if (!init_wayland(&app))
    {
        cleanup(&app);
        return 1;
    }
    if (app.opts.list_only)
    {
        cleanup(&app);
        return 0;
    }
    if (!init_egl(&app))
    {
        cleanup(&app);
        return 1;
    }

    csv_open(&app);

    app.start_mono_ns = mono_ns();
    app.end_mono_ns = app.start_mono_ns + (uint64_t)app.opts.seconds * 1000000000ull;
    app.running = true;

    while (app.running && wl_display_dispatch(app.display) != -1)
    {
        if (mono_ns() >= app.end_mono_ns)
            app.running = false;
    }
    wl_display_roundtrip(app.display);

    fprintf(stderr,
            "summary: submitted=%"PRIu64" presented=%"PRIu64" discarded=%"PRIu64
            " callbacks=%"PRIu64" callback_misses=%"PRIu64" sequence_jumps=%"PRIu64
            " zero_copy=%"PRIu64" hw_clock=%"PRIu64" hw_completion=%"PRIu64
            " vsync=%"PRIu64" max_sequence_delta=%"PRIu64
            " max_present_ms=%.3f max_callback_ms=%.3f\n",
            app.submitted_frames, app.presented_frames, app.discarded_frames,
            app.frame_callbacks, app.callback_interval_miss, app.sequence_jump_total,
            app.zero_copy_frames, app.hw_clock_frames, app.hw_completion_frames,
            app.vsync_frames, app.max_sequence_delta,
            (double)app.max_present_interval_ns / 1000000.0,
            (double)app.max_callback_interval_ns / 1000000.0);

    cleanup(&app);
    return 0;
}
