#ifndef OPEN3D_WAYLAND_WINDOW_H
#define OPEN3D_WAYLAND_WINDOW_H 1

#include <stdbool.h>
#include <stdint.h>

#include <vlc_vout_window.h>

#define OPEN3D_WAYLAND_WINDOW_API_MAGIC 0x4f334457u /* O3DW */
#define OPEN3D_WAYLAND_WINDOW_API_VERSION 1u

typedef struct
{
    bool available;
    uint32_t clock_id;
    uint64_t requested;
    uint64_t presented;
    uint64_t discarded;
    uint64_t sequence_jump_total;
    uint64_t last_present_id;
    uint64_t last_schedule_index;
    uint64_t last_present_mono_us;
    uint64_t last_present_time_ns;
    uint64_t last_present_interval_ns;
    uint64_t last_refresh_ns;
    uint64_t last_sequence;
    uint64_t last_sequence_delta;
    uint32_t last_flags;
    bool last_right_eye;
} open3d_wayland_presentation_stats_t;

typedef struct open3d_wayland_window_api_t
{
    uint32_t magic;
    uint32_t version;
    bool (*request_presentation_feedback)(vout_window_t *window,
                                          uint64_t present_id,
                                          uint64_t schedule_index,
                                          bool right_eye);
    bool (*snapshot_presentation)(vout_window_t *window,
                                  open3d_wayland_presentation_stats_t *stats);
} open3d_wayland_window_api_t;

static inline open3d_wayland_window_api_t *
Open3DWaylandWindowApi(vout_window_t *window)
{
    if (window == NULL || window->sys == NULL)
        return NULL;

    open3d_wayland_window_api_t *api =
        (open3d_wayland_window_api_t *)window->sys;
    if (api->magic != OPEN3D_WAYLAND_WINDOW_API_MAGIC ||
        api->version != OPEN3D_WAYLAND_WINDOW_API_VERSION)
        return NULL;
    return api;
}

#endif
