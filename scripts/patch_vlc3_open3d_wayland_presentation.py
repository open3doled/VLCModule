#!/usr/bin/env python3
from pathlib import Path
import sys


MARKER = "OPEN3D_WAYLAND_PRESENTATION_PATCH"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"Could not patch xdg-shell.c: missing {label}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("Usage: patch_vlc3_open3d_wayland_presentation.py /path/to/vlc-src")

    root = Path(sys.argv[1])
    path = root / "modules/video_output/wayland/xdg-shell.c"
    text = path.read_text()
    if MARKER in text:
        return 0

    text = replace_once(
        text,
        "#include <stdint.h>\n"
        "#include <string.h>\n"
        "#include <poll.h>\n",
        "#include <stdint.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <poll.h>\n",
        "Open3D presentation stdlib include",
    )

    text = replace_once(
        text,
        '#include "server-decoration-client-protocol.h"\n',
        '#include "server-decoration-client-protocol.h"\n'
        '#include "presentation-time-client-protocol.h"\n'
        '#include "../open3d_wayland_window.h"\n'
        '#include "presentation-time-protocol.c"\n\n'
        f"#define {MARKER} 1\n",
        "Open3D presentation includes",
    )

    text = replace_once(
        text,
        "struct vout_window_sys_t\n{\n",
        "struct open3d_wl_feedback;\n\n"
        "struct vout_window_sys_t\n{\n"
        "    open3d_wayland_window_api_t open3d_api;\n",
        "Open3D API sys prefix",
    )

    text = replace_once(
        text,
        "    struct org_kde_kwin_server_decoration_manager *deco_manager;\n"
        "    struct org_kde_kwin_server_decoration *deco;\n\n"
        "    vlc_thread_t thread;\n",
        "    struct org_kde_kwin_server_decoration_manager *deco_manager;\n"
        "    struct org_kde_kwin_server_decoration *deco;\n"
        "    struct wp_presentation *presentation;\n"
        "    struct open3d_wl_feedback *presentation_pending;\n"
        "    vlc_mutex_t presentation_lock;\n"
        "    bool presentation_available;\n"
        "    uint32_t presentation_clock_id;\n"
        "    uint64_t presentation_requested;\n"
        "    uint64_t presentation_presented;\n"
        "    uint64_t presentation_discarded;\n"
        "    uint64_t presentation_sequence_jump_total;\n"
        "    uint64_t presentation_last_present_id;\n"
        "    uint64_t presentation_last_schedule_index;\n"
        "    uint64_t presentation_last_present_mono_us;\n"
        "    uint64_t presentation_last_time_ns;\n"
        "    uint64_t presentation_last_interval_ns;\n"
        "    uint64_t presentation_last_refresh_ns;\n"
        "    uint64_t presentation_last_sequence;\n"
        "    uint64_t presentation_last_sequence_delta;\n"
        "    uint32_t presentation_last_flags;\n"
        "    bool presentation_last_right_eye;\n\n"
        "    vlc_thread_t thread;\n",
        "Open3D presentation sys fields",
    )

    presentation_code = r'''
typedef struct open3d_wl_feedback
{
    vout_window_sys_t *sys;
    struct wp_presentation_feedback *feedback;
    uint64_t present_id;
    uint64_t schedule_index;
    bool right_eye;
    struct open3d_wl_feedback *next;
} open3d_wl_feedback_t;

static uint64_t Open3DWaylandPresentationNowUs(void)
{
    vlc_tick_t now = mdate();
    return now > 0 ? (uint64_t)now : 0;
}

static void Open3DWaylandPresentationRemoveLocked(vout_window_sys_t *sys,
                                                  open3d_wl_feedback_t *item)
{
    open3d_wl_feedback_t **it = &sys->presentation_pending;
    while (*it != NULL)
    {
        if (*it == item)
        {
            *it = item->next;
            item->next = NULL;
            return;
        }
        it = &(*it)->next;
    }
}

static bool Open3DWaylandPresentationSnapshot(vout_window_t *wnd,
                                              open3d_wayland_presentation_stats_t *stats)
{
    if (wnd == NULL || wnd->sys == NULL || stats == NULL)
        return false;

    vout_window_sys_t *sys = wnd->sys;
    if (sys->open3d_api.magic != OPEN3D_WAYLAND_WINDOW_API_MAGIC)
        return false;

    vlc_mutex_lock(&sys->presentation_lock);
    stats->available = sys->presentation_available;
    stats->clock_id = sys->presentation_clock_id;
    stats->requested = sys->presentation_requested;
    stats->presented = sys->presentation_presented;
    stats->discarded = sys->presentation_discarded;
    stats->sequence_jump_total = sys->presentation_sequence_jump_total;
    stats->last_present_id = sys->presentation_last_present_id;
    stats->last_schedule_index = sys->presentation_last_schedule_index;
    stats->last_present_mono_us = sys->presentation_last_present_mono_us;
    stats->last_present_time_ns = sys->presentation_last_time_ns;
    stats->last_present_interval_ns = sys->presentation_last_interval_ns;
    stats->last_refresh_ns = sys->presentation_last_refresh_ns;
    stats->last_sequence = sys->presentation_last_sequence;
    stats->last_sequence_delta = sys->presentation_last_sequence_delta;
    stats->last_flags = sys->presentation_last_flags;
    stats->last_right_eye = sys->presentation_last_right_eye;
    vlc_mutex_unlock(&sys->presentation_lock);
    return true;
}

static void Open3DWaylandPresentationFeedbackPresented(
    void *data, struct wp_presentation_feedback *feedback,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
    uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
    open3d_wl_feedback_t *item = data;
    if (item == NULL || item->sys == NULL)
        return;

    vout_window_sys_t *sys = item->sys;
    const uint64_t seconds = ((uint64_t)tv_sec_hi << 32) | tv_sec_lo;
    const uint64_t present_time_ns = seconds * 1000000000ull + tv_nsec;
    const uint64_t sequence = ((uint64_t)seq_hi << 32) | seq_lo;
    uint64_t sequence_delta = 0;
    uint64_t interval_ns = 0;

    vlc_mutex_lock(&sys->presentation_lock);
    if (sys->presentation_last_time_ns != 0 && present_time_ns >= sys->presentation_last_time_ns)
        interval_ns = present_time_ns - sys->presentation_last_time_ns;
    if (sys->presentation_last_sequence != 0 && sequence > sys->presentation_last_sequence)
    {
        sequence_delta = sequence - sys->presentation_last_sequence;
        if (sequence_delta > 1)
            sys->presentation_sequence_jump_total += sequence_delta - 1;
    }

    sys->presentation_presented++;
    sys->presentation_last_present_id = item->present_id;
    sys->presentation_last_schedule_index = item->schedule_index;
    sys->presentation_last_present_mono_us = Open3DWaylandPresentationNowUs();
    sys->presentation_last_time_ns = present_time_ns;
    sys->presentation_last_interval_ns = interval_ns;
    sys->presentation_last_refresh_ns = refresh;
    sys->presentation_last_sequence = sequence;
    sys->presentation_last_sequence_delta = sequence_delta;
    sys->presentation_last_flags = flags;
    sys->presentation_last_right_eye = item->right_eye;
    Open3DWaylandPresentationRemoveLocked(sys, item);
    vlc_mutex_unlock(&sys->presentation_lock);

    wp_presentation_feedback_destroy(feedback);
    free(item);
}

static void Open3DWaylandPresentationFeedbackDiscarded(
    void *data, struct wp_presentation_feedback *feedback)
{
    open3d_wl_feedback_t *item = data;
    if (item != NULL && item->sys != NULL)
    {
        vout_window_sys_t *sys = item->sys;
        vlc_mutex_lock(&sys->presentation_lock);
        sys->presentation_discarded++;
        Open3DWaylandPresentationRemoveLocked(sys, item);
        vlc_mutex_unlock(&sys->presentation_lock);
    }
    wp_presentation_feedback_destroy(feedback);
    free(item);
}

static void Open3DWaylandPresentationFeedbackSyncOutput(
    void *data, struct wp_presentation_feedback *feedback, struct wl_output *output)
{
    VLC_UNUSED(data);
    VLC_UNUSED(feedback);
    VLC_UNUSED(output);
}

static const struct wp_presentation_feedback_listener open3d_presentation_feedback_cbs =
{
    Open3DWaylandPresentationFeedbackSyncOutput,
    Open3DWaylandPresentationFeedbackPresented,
    Open3DWaylandPresentationFeedbackDiscarded,
};

static bool Open3DWaylandPresentationRequest(vout_window_t *wnd,
                                             uint64_t present_id,
                                             uint64_t schedule_index,
                                             bool right_eye)
{
    if (wnd == NULL || wnd->sys == NULL || wnd->handle.wl == NULL)
        return false;

    vout_window_sys_t *sys = wnd->sys;
    if (sys->presentation == NULL)
        return false;

    open3d_wl_feedback_t *item = calloc(1, sizeof(*item));
    if (unlikely(item == NULL))
        return false;

    item->sys = sys;
    item->present_id = present_id;
    item->schedule_index = schedule_index;
    item->right_eye = right_eye;
    item->feedback = wp_presentation_feedback(sys->presentation, wnd->handle.wl);
    if (item->feedback == NULL)
    {
        free(item);
        return false;
    }

    wp_presentation_feedback_add_listener(item->feedback,
                                          &open3d_presentation_feedback_cbs,
                                          item);
    vlc_mutex_lock(&sys->presentation_lock);
    item->next = sys->presentation_pending;
    sys->presentation_pending = item;
    sys->presentation_requested++;
    vlc_mutex_unlock(&sys->presentation_lock);
    return true;
}

static void Open3DWaylandPresentationClock(void *data,
                                           struct wp_presentation *presentation,
                                           uint32_t clock_id)
{
    vout_window_t *wnd = data;
    VLC_UNUSED(presentation);
    if (wnd == NULL || wnd->sys == NULL)
        return;

    vout_window_sys_t *sys = wnd->sys;
    vlc_mutex_lock(&sys->presentation_lock);
    sys->presentation_clock_id = clock_id;
    sys->presentation_available = true;
    vlc_mutex_unlock(&sys->presentation_lock);
}

static const struct wp_presentation_listener open3d_presentation_cbs =
{
    Open3DWaylandPresentationClock,
};

static void Open3DWaylandPresentationInitApi(vout_window_sys_t *sys)
{
    memset(&sys->open3d_api, 0, sizeof(sys->open3d_api));
    sys->open3d_api.magic = OPEN3D_WAYLAND_WINDOW_API_MAGIC;
    sys->open3d_api.version = OPEN3D_WAYLAND_WINDOW_API_VERSION;
    sys->open3d_api.request_presentation_feedback =
        Open3DWaylandPresentationRequest;
    sys->open3d_api.snapshot_presentation =
        Open3DWaylandPresentationSnapshot;
}

static void Open3DWaylandPresentationDestroyPending(vout_window_sys_t *sys)
{
    if (sys == NULL)
        return;

    vlc_mutex_lock(&sys->presentation_lock);
    open3d_wl_feedback_t *item = sys->presentation_pending;
    sys->presentation_pending = NULL;
    vlc_mutex_unlock(&sys->presentation_lock);

    while (item != NULL)
    {
        open3d_wl_feedback_t *next = item->next;
        if (item->feedback != NULL)
            wp_presentation_feedback_destroy(item->feedback);
        free(item);
        item = next;
    }
}

'''
    text = replace_once(
        text,
        "\nstatic void cleanup_wl_display_read(void *data)\n",
        "\n" + presentation_code + "\nstatic void cleanup_wl_display_read(void *data)\n",
        "Open3D presentation API implementation",
    )

    text = replace_once(
        text,
        '    if (!strcmp(iface, "wl_compositor"))\n'
        '        sys->compositor = wl_registry_bind(registry, name,\n'
        '                                           &wl_compositor_interface,\n'
        '                                           (vers < 2) ? vers : 2);\n'
        '    else\n'
        '    if (!strcmp(iface, "xdg_shell"))\n',
        '    if (!strcmp(iface, "wl_compositor"))\n'
        '        sys->compositor = wl_registry_bind(registry, name,\n'
        '                                           &wl_compositor_interface,\n'
        '                                           (vers < 2) ? vers : 2);\n'
        '    else\n'
        '    if (!strcmp(iface, "wp_presentation"))\n'
        '        sys->presentation = wl_registry_bind(registry, name,\n'
        '                                            &wp_presentation_interface,\n'
        '                                            (vers < 1) ? vers : 1);\n'
        '    else\n'
        '    if (!strcmp(iface, "xdg_shell"))\n',
        "wp_presentation registry binding",
    )

    text = replace_once(
        text,
        "    sys->compositor = NULL;\n"
        "    sys->shell = NULL;\n"
        "    sys->surface = NULL;\n"
        "    sys->deco_manager = NULL;\n"
        "    sys->deco = NULL;\n"
        "    wnd->sys = sys;\n",
        "    Open3DWaylandPresentationInitApi(sys);\n"
        "    sys->compositor = NULL;\n"
        "    sys->shell = NULL;\n"
        "    sys->surface = NULL;\n"
        "    sys->deco_manager = NULL;\n"
        "    sys->deco = NULL;\n"
        "    sys->presentation = NULL;\n"
        "    sys->presentation_pending = NULL;\n"
        "    sys->presentation_available = false;\n"
        "    sys->presentation_clock_id = 0;\n"
        "    sys->presentation_requested = 0;\n"
        "    sys->presentation_presented = 0;\n"
        "    sys->presentation_discarded = 0;\n"
        "    sys->presentation_sequence_jump_total = 0;\n"
        "    sys->presentation_last_present_id = 0;\n"
        "    sys->presentation_last_schedule_index = 0;\n"
        "    sys->presentation_last_present_mono_us = 0;\n"
        "    sys->presentation_last_time_ns = 0;\n"
        "    sys->presentation_last_interval_ns = 0;\n"
        "    sys->presentation_last_refresh_ns = 0;\n"
        "    sys->presentation_last_sequence = 0;\n"
        "    sys->presentation_last_sequence_delta = 0;\n"
        "    sys->presentation_last_flags = 0;\n"
        "    sys->presentation_last_right_eye = false;\n"
        "    vlc_mutex_init(&sys->presentation_lock);\n"
        "    wnd->sys = sys;\n",
        "Open3D presentation init",
    )

    text = replace_once(
        text,
        "    if (sys->compositor == NULL || sys->shell == NULL)\n"
        "        goto error;\n\n"
        "    xdg_shell_use_unstable_version(sys->shell, XDG_SHELL_VERSION_CURRENT);\n",
        "    if (sys->compositor == NULL || sys->shell == NULL)\n"
        "        goto error;\n\n"
        "    if (sys->presentation != NULL)\n"
        "    {\n"
        "        wp_presentation_add_listener(sys->presentation,\n"
        "                                     &open3d_presentation_cbs, wnd);\n"
        "        wl_display_roundtrip(display);\n"
        "        msg_Dbg(wnd, \"open3d Wayland presentation feedback available clock_id=%u\",\n"
        "                sys->presentation_clock_id);\n"
        "    }\n"
        "    else\n"
        "        msg_Dbg(wnd, \"open3d Wayland presentation feedback unavailable\");\n\n"
        "    xdg_shell_use_unstable_version(sys->shell, XDG_SHELL_VERSION_CURRENT);\n",
        "Open3D presentation listener",
    )

    text = replace_once(
        text,
        "    if (sys->deco != NULL)\n"
        "        org_kde_kwin_server_decoration_destroy(sys->deco);\n"
        "    if (sys->deco_manager != NULL)\n",
        "    Open3DWaylandPresentationDestroyPending(sys);\n"
        "    if (sys->presentation != NULL)\n"
        "        wp_presentation_destroy(sys->presentation);\n"
        "    if (sys->deco != NULL)\n"
        "        org_kde_kwin_server_decoration_destroy(sys->deco);\n"
        "    if (sys->deco_manager != NULL)\n",
        "Open3D presentation error cleanup",
    )

    text = replace_once(
        text,
        "    wl_display_disconnect(display);\n"
        "    free(sys);\n"
        "    return VLC_EGENERIC;\n",
        "    wl_display_disconnect(display);\n"
        "    vlc_mutex_destroy(&sys->presentation_lock);\n"
        "    free(sys);\n"
        "    return VLC_EGENERIC;\n",
        "Open3D presentation error mutex cleanup",
    )

    text = replace_once(
        text,
        "    vlc_cancel(sys->thread);\n"
        "    vlc_join(sys->thread, NULL);\n\n"
        "    if (sys->deco != NULL)\n",
        "    vlc_cancel(sys->thread);\n"
        "    vlc_join(sys->thread, NULL);\n\n"
        "    Open3DWaylandPresentationDestroyPending(sys);\n"
        "    if (sys->presentation != NULL)\n"
        "        wp_presentation_destroy(sys->presentation);\n"
        "    if (sys->deco != NULL)\n",
        "Open3D presentation close cleanup",
    )

    text = replace_once(
        text,
        "    wl_display_disconnect(wnd->display.wl);\n"
        "    free(sys);\n"
        "}\n\n\n#define DISPLAY_TEXT",
        "    wl_display_disconnect(wnd->display.wl);\n"
        "    vlc_mutex_destroy(&sys->presentation_lock);\n"
        "    free(sys);\n"
        "}\n\n\n#define DISPLAY_TEXT",
        "Open3D presentation close mutex cleanup",
    )

    text = replace_once(
        text,
        "    set_callbacks(Open, Close)\n\n"
        "    add_string(\"wl-display\", NULL, DISPLAY_TEXT, DISPLAY_LONGTEXT, true)\n",
        "    set_callbacks(Open, Close)\n"
        "    add_shortcut(\"open3d_wl\")\n\n"
        "    add_string(\"wl-display\", NULL, DISPLAY_TEXT, DISPLAY_LONGTEXT, true)\n",
        "Open3D Wayland shortcut",
    )

    path.write_text(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
