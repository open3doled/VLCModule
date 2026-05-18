/*****************************************************************************
 * bluray.c: Blu-ray disc support plugin
 *****************************************************************************
 * Copyright © 2010-2012 VideoLAN, VLC authors and libbluray AUTHORS
 *
 * Authors: Jean-Baptiste Kempf <jb@videolan.org>
 *          Hugo Beauzée-Luyssen <hugo@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef HAVE_GETMNTENT_R
# include <mntent.h>
#endif
#include <fcntl.h>      /* O_* */
#include <unistd.h>     /* close() */
#include <sys/stat.h>

#ifdef __APPLE__
# include <sys/param.h>
# include <sys/ucred.h>
# include <sys/mount.h>
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>                      /* demux_t */
#include <vlc_input.h>                      /* Seekpoints, chapters */
#include <vlc_atomic.h>
#include <vlc_dialog.h>                     /* BD+/AACS warnings */
#include <vlc_vout.h>                       /* vout_PutSubpicture / subpicture_t */
#include <vlc_url.h>                        /* vlc_path2uri */
#include <vlc_iso_lang.h>
#include <vlc_fs.h>

#include "../demux/mpeg/timestamps.h"
#include "../demux/timestamps_filter.h"
#include "../video_output/open3d_subtitle_bridge.h"
/* FIXME we should find a better way than including that */
#include "../../src/text/iso-639_def.h"


#include <libbluray/bluray.h>
#include <libbluray/bluray-version.h>
#include <libbluray/keys.h>
#include <libbluray/meta_data.h>
#include <libbluray/overlay.h>
#include <libbluray/clpi_data.h>

#ifndef OPEN3DBLURAY_ENABLE_MVC
#define OPEN3DBLURAY_ENABLE_MVC 1
#endif

#if OPEN3DBLURAY_ENABLE_MVC
#include <libbluray/bluray_open3d_mvc.h>
#define OPEN3DBLURAY_CODEC_MVC VLC_FOURCC('m','v','c','1')
#endif

#ifndef OPEN3DBLURAYMVC_BUILD_GIT_SHA
#define OPEN3DBLURAYMVC_BUILD_GIT_SHA "unknown"
#endif

#ifndef OPEN3DBLURAYMVC_BUILD_TIME
#define OPEN3DBLURAYMVC_BUILD_TIME "unknown"
#endif

#ifndef OPEN3DBLURAY_BUILD_GIT_SHA
#define OPEN3DBLURAY_BUILD_GIT_SHA OPEN3DBLURAYMVC_BUILD_GIT_SHA
#endif

#ifndef OPEN3DBLURAY_BUILD_TIME
#define OPEN3DBLURAY_BUILD_TIME OPEN3DBLURAYMVC_BUILD_TIME
#endif

#ifndef OPEN3DBLURAY_PLUGIN_SHORTNAME
#define OPEN3DBLURAY_PLUGIN_SHORTNAME N_("Open3D Blu-ray MVC")
#endif

#ifndef OPEN3DBLURAY_PLUGIN_DESCRIPTION
#define OPEN3DBLURAY_PLUGIN_DESCRIPTION \
    N_("Open3D stock-shaped Blu-ray MVC demuxer (libbluray-owned assembly)")
#endif

#ifndef OPEN3DBLURAY_PLUGIN_DEMUX_DESCRIPTION
#define OPEN3DBLURAY_PLUGIN_DEMUX_DESCRIPTION \
    "Open3D Blu-ray MVC demuxer"
#endif

#ifndef OPEN3DBLURAY_ACCESS_NAME
#define OPEN3DBLURAY_ACCESS_NAME "open3dbluraymvc"
#endif

#ifndef OPEN3DBLURAY_LOG_PREFIX
#define OPEN3DBLURAY_LOG_PREFIX "open3dbluraymvc"
#endif

#define OPEN3DBLURAY_SUBTITLE_OFFSET_VALID_VAR  "open3d-subtitle-offset-valid"
#define OPEN3DBLURAY_SUBTITLE_OFFSET_SIGNED_VAR "open3d-subtitle-offset-signed"
#define OPEN3DBLURAY_SUBTITLE_OFFSET_RAW_VAR    "open3d-subtitle-offset-raw"
#define OPEN3DBLURAY_SUBTITLE_OFFSET_SEQ_VAR    "open3d-subtitle-offset-seq"
#define OPEN3DBLURAY_SUBTITLE_OFFSET_FRAME_VAR  "open3d-subtitle-offset-frame"
#define OPEN3DBLURAY_FORCED_SUBTITLE_ID_BASE    0x7f000000u
#define OPEN3DBLURAY_FORCED_SUBTITLE_DESC       "Forced only"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define BD_MENU_TEXT        N_("Blu-ray menus")
#define BD_MENU_LONGTEXT    N_("Use Blu-ray menus. If disabled, "\
                                "the movie will start directly")
#define BD_REGION_TEXT      N_("Region code")
#define BD_REGION_LONGTEXT  N_("Blu-Ray player region code. "\
                                "Some discs can be played only with a correct region code.")
#define OPEN3DBLURAY_MVC_DIRECT_TIMING_TEXT \
    N_("Use direct synthetic MVC timing path")
#define OPEN3DBLURAY_MVC_DIRECT_TIMING_LONGTEXT \
    N_("Send synthetic mvc1 packets directly to the main ES output with synthetic PCR updates. " \
       "This matches the older fast-seek timing path and is useful for A/B testing seek responsiveness.")
#define OPEN3DBLURAY_MVC_SEEK_TIMING_TRACE_TEXT \
    N_("Trace post-seek MVC timing milestones")
#define OPEN3DBLURAY_MVC_SEEK_TIMING_TRACE_LONGTEXT \
    N_("Log the first stock PCR, first audio block, first synthetic MVC block, and first matched MVC block " \
       "after each seek. This is a diagnostic trace for shared-clock seek bring-up.")

static const char *const ppsz_region_code[] = {
    "A", "B", "C" };
static const char *const ppsz_region_code_text[] = {
    "Region A", "Region B", "Region C" };

#define REGION_DEFAULT   1   /* Index to region list. Actual region code is (1<<REGION_DEFAULT) */
#define LANGUAGE_DEFAULT ("eng")

#if BLURAY_VERSION >= BLURAY_VERSION_CODE(0,8,0)
# define BLURAY_DEMUX
#endif

#ifndef BD_STREAM_TYPE_VIDEO_HEVC
# define BD_STREAM_TYPE_VIDEO_HEVC 0x24
#endif

#define BD_CLUSTER_SIZE 6144
#define BD_READ_SIZE    (10 * BD_CLUSTER_SIZE)

/* Callbacks */
static int  blurayOpen (vlc_object_t *);
static void blurayClose(vlc_object_t *);

vlc_module_begin ()
    set_shortname(OPEN3DBLURAY_PLUGIN_SHORTNAME)
    set_description(OPEN3DBLURAY_PLUGIN_DESCRIPTION)

    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
    set_capability("access_demux", 200)
    add_bool("bluray-menu", false, BD_MENU_TEXT, BD_MENU_LONGTEXT, false)
    add_string("bluray-region", ppsz_region_code[REGION_DEFAULT], BD_REGION_TEXT, BD_REGION_LONGTEXT, false)
        change_string_list(ppsz_region_code, ppsz_region_code_text)
#if OPEN3DBLURAY_ENABLE_MVC
    add_bool("open3dbluraymvc-direct-mvc-timing", false,
             OPEN3DBLURAY_MVC_DIRECT_TIMING_TEXT,
             OPEN3DBLURAY_MVC_DIRECT_TIMING_LONGTEXT, true)
    add_bool("open3dbluraymvc-seek-timing-trace", false,
             OPEN3DBLURAY_MVC_SEEK_TIMING_TRACE_TEXT,
             OPEN3DBLURAY_MVC_SEEK_TIMING_TRACE_LONGTEXT, true)
#endif

    add_shortcut(OPEN3DBLURAY_ACCESS_NAME)

    set_callbacks(blurayOpen, blurayClose)

#ifdef BLURAY_DEMUX
    /* demux module */
    add_submodule()
        set_description( OPEN3DBLURAY_PLUGIN_DEMUX_DESCRIPTION )
        set_category( CAT_INPUT )
        set_subcategory( SUBCAT_INPUT_DEMUX )
        set_capability( "demux", 5 )
        set_callbacks( blurayOpen, blurayClose )
#endif

vlc_module_end ()

/* libbluray's overlay.h defines 2 types of overlay (bd_overlay_plane_e). */
#define MAX_OVERLAY 2

typedef enum OverlayStatus {
    Closed = 0,
    ToDisplay,  //Used to mark the overlay to be displayed the first time.
    Displayed,
    Outdated    //used to update the overlay after it has been sent to the vout
} OverlayStatus;

typedef struct bluray_overlay_t
{
    vlc_mutex_t         lock;
    int                 i_channel;
    OverlayStatus       status;
    subpicture_region_t *p_regions;
    int                 width, height;
    uint8_t             plane;
    int64_t             pts90k;
    bool                b_subtitle_offset_valid;
    uint8_t             i_subtitle_offset_sequence;
    uint8_t             i_subtitle_offset_raw;
    int8_t              i_subtitle_offset_signed;
    int                 i_subtitle_offset_frame;
    bool                b_palette_cache_valid;
    BD_PG_PALETTE_ENTRY palette_cache[256];

    /* pointer to last subpicture updater.
     * used to disconnect this overlay from vout when:
     * - the overlay is closed
     * - vout is changed and this overlay is sent to the new vout
     */
    struct subpicture_updater_sys_t *p_updater;
} bluray_overlay_t;

typedef struct
{
    int  i_real_pid;
    bool b_forced_only;
    bool b_auto_forced_only;
} open3dbluray_subtitle_selection_t;

typedef struct
{
    open3dbluray_subtitle_selection_t current;
    bool                              b_auto_forced_default_enabled;
    int                               i_pending_forced_startup_real_pid;
    bool                              b_pending_apply_valid;
    open3dbluray_subtitle_selection_t pending_apply;
} open3dbluray_subtitle_selection_state_t;

struct  demux_sys_t
{
    BLURAY              *bluray;
    bool                b_draining;

    /* Titles */
    unsigned int        i_title;
    unsigned int        i_longest_title;
    input_title_t       **pp_title;

    /* Events */
    DECL_ARRAY(BD_EVENT) events_delayed;

    vlc_mutex_t             pl_info_lock;
    BLURAY_TITLE_INFO      *p_pl_info;
    const BLURAY_CLIP_INFO *p_clip_info;
    enum
    {
        BD_CLIP_APP_TYPE_TS_MAIN_PATH_MOVIE = 1,
        BD_CLIP_APP_TYPE_TS_MAIN_PATH_TIMED_SLIDESHOW = 2,
        BD_CLIP_APP_TYPE_TS_MAIN_PATH_BROWSABLE_SLIDESHOW = 3,
        BD_CLIP_APP_TYPE_TS_SUB_PATH_BROWSABLE_SLIDESHOW = 4,
        BD_CLIP_APP_TYPE_TS_SUB_PATH_INTERACTIVE_MENU = 5,
        BD_CLIP_APP_TYPE_TS_SUB_PATH_TEXT_SUBTITLE = 6,
        BD_CLIP_APP_TYPE_TS_SUB_PATH_ELEMENTARY_STREAM_PATH = 7,
    } clip_application_type;

    /* Attachments */
    int                 i_attachments;
    input_attachment_t  **attachments;
    int                 i_cover_idx;

    /* Meta information */
    const META_DL       *p_meta;

    /* Menus */
    bluray_overlay_t    *p_overlays[MAX_OVERLAY];
    bool                b_fatal_error;
    bool                b_menu;
    bool                b_menu_open;
    bool                b_popup_available;
    vlc_tick_t          i_still_end_time;
    unsigned            i_redundant_end_of_title_streak;
    bool                b_probe_yield_redundant_end_of_title;
    bool                b_probe_logged_redundant_end_of_title;
    unsigned            i_probe_redundant_end_of_title_yield_count;

    vlc_mutex_t         bdj_overlay_lock; /* used to lock BD-J overlay open/close while overlays are being sent to vout */

    /* */
    vout_thread_t       *p_vout;
    open3d_subtitle_bridge_t subtitle_bridge;
    open3d_interactive_graphics_bridge_t interactive_graphics_bridge;

    es_out_id_t         *p_dummy_video;
    vlc_tick_t          i_dummy_video_next_pts;

    /* TS stream */
    es_out_t            *p_tf_out;
    es_out_t            *p_out;
    es_out_t            *p_esc_out;
    bool                b_spu_enable;       /* enabled / disabled */
    vlc_demux_chained_t *p_parser;
    bool                b_flushed;
    bool                b_pl_playing;       /* true when playing playlist */
    bool                b_selected_pg_offset_sequence_valid;
    uint8_t             i_selected_pg_offset_sequence_id;
    open3dbluray_subtitle_selection_state_t subtitle_selection;
    int                 i_forced_filter_pid;
    bool                b_forced_filter_output_active;
    block_t             *p_forced_filter_head;
    block_t             **pp_forced_filter_last;

    /* stream input */
    vlc_mutex_t         read_block_lock;

    /* Used to store bluray disc path */
    char                *psz_bd_path;

#if OPEN3DBLURAY_ENABLE_MVC
    es_out_id_t         *p_mvc_video;
    bool                b_mvc_first_unit;
    bool                b_mvc_seen_matched;
    bool                b_mvc_base_fallback_selected;
    bool                b_mvc_next_block_discontinuity;
    bool                b_mvc_hold_unmatched_until_match;
    int                 i_mvc_group;
    bool                b_mvc_group_known;
    bool                b_seek_timing_trace_active;
    unsigned            i_seek_timing_trace_epoch;
    bool                b_seek_timing_trace_saw_stock_pcr;
    bool                b_seek_timing_trace_saw_audio;
    bool                b_seek_timing_trace_saw_mvc_unit;
    bool                b_seek_timing_trace_saw_mvc_send;
    bool                b_seek_timing_trace_saw_mvc_matched;
    vlc_tick_t          i_seek_timing_last_group_pcr;
#endif
};

/*
 * Local ES index storage
 */
typedef struct
{
    es_format_t fmt;
    es_out_id_t *p_es;
    int i_next_block_flags;
    bool b_recyling;
} es_pair_t;

static bool es_pair_Add(vlc_array_t *p_array, const es_format_t *p_fmt,
                        es_out_id_t *p_es)
{
    es_pair_t *p_pair = malloc(sizeof(*p_pair));
    if (likely(p_pair != NULL))
    {
        p_pair->p_es = p_es;
        p_pair->i_next_block_flags = 0;
        p_pair->b_recyling = false;
        if(vlc_array_append(p_array, p_pair) != VLC_SUCCESS)
        {
            free(p_pair);
            p_pair = NULL;
        }
        else
        {
            es_format_Init(&p_pair->fmt, p_fmt->i_cat, p_fmt->i_codec);
            es_format_Copy(&p_pair->fmt, p_fmt);
        }
    }
    return p_pair != NULL;
}

static void es_pair_Remove(vlc_array_t *p_array, es_pair_t *p_pair)
{
    vlc_array_remove(p_array, vlc_array_index_of_item(p_array, p_pair));
    es_format_Clean(&p_pair->fmt);
    free(p_pair);
}

static es_pair_t *getEsPair(vlc_array_t *p_array,
                            bool (*match)(const es_pair_t *, const void *),
                            const void *param)
{
    for (size_t i = 0; i < vlc_array_count(p_array); ++i)
    {
        es_pair_t *p_pair = vlc_array_item_at_index(p_array, i);
        if(match(p_pair, param))
            return p_pair;
    }
    return NULL;
}

#if OPEN3DBLURAY_ENABLE_MVC
static void open3dblurayTraceSeekTimingAudio(demux_t *p_demux,
                                             const es_pair_t *p_pair,
                                             const block_t *p_block)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (!p_sys->b_seek_timing_trace_active ||
        p_sys->b_seek_timing_trace_saw_audio ||
        !p_pair || !p_block || p_pair->fmt.i_cat != AUDIO_ES)
        return;

    p_sys->b_seek_timing_trace_saw_audio = true;
    msg_Info(p_demux,
             "%s seek_timing epoch=%u first_audio pid=0x%x codec=0x%.4x group=%d pts=%" PRId64 " dts=%" PRId64 " flags=0x%x last_group_pcr=%" PRId64,
             OPEN3DBLURAY_LOG_PREFIX,
             p_sys->i_seek_timing_trace_epoch,
             p_pair->fmt.i_id,
             p_pair->fmt.i_codec,
             p_pair->fmt.i_group,
             p_block->i_pts,
             p_block->i_dts,
             p_block->i_flags,
             p_sys->i_seek_timing_last_group_pcr);
}

static void open3dblurayTraceSeekTimingMvcUnit(demux_t *p_demux,
                                               const BD_OPEN3D_MVC_UNIT *p_unit,
                                               const block_t *p_block,
                                               bool b_direct_timing)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (!p_sys->b_seek_timing_trace_active || !p_unit || !p_block)
        return;

    if (!p_sys->b_seek_timing_trace_saw_mvc_unit) {
        p_sys->b_seek_timing_trace_saw_mvc_unit = true;
        msg_Info(p_demux,
                 "%s seek_timing epoch=%u first_mvc_unit mode=%s flags=0x%x base_pts=%" PRId64 " base_dts=%" PRId64 " dep_pts=%" PRId64 " dep_dts=%" PRId64 " out_pts=%" PRId64 " out_dts=%" PRId64 " merged_size=%u",
                 OPEN3DBLURAY_LOG_PREFIX,
                 p_sys->i_seek_timing_trace_epoch,
                 b_direct_timing ? "direct" : "shared",
                 p_unit->flags,
                 p_unit->base_pts,
                 p_unit->base_dts,
                 p_unit->dependent_pts,
                 p_unit->dependent_dts,
                 p_block->i_pts,
                 p_block->i_dts,
                 p_unit->merged_size);
    }

    if ((p_unit->flags & BD_OPEN3D_MVC_UNIT_FLAG_MATCHED) &&
        !p_sys->b_seek_timing_trace_saw_mvc_matched) {
        p_sys->b_seek_timing_trace_saw_mvc_matched = true;
        msg_Info(p_demux,
                 "%s seek_timing epoch=%u first_matched_mvc mode=%s flags=0x%x base_pts=%" PRId64 " base_dts=%" PRId64 " dep_pts=%" PRId64 " dep_dts=%" PRId64 " out_pts=%" PRId64 " out_dts=%" PRId64 " merged_size=%u",
                 OPEN3DBLURAY_LOG_PREFIX,
                 p_sys->i_seek_timing_trace_epoch,
                 b_direct_timing ? "direct" : "shared",
                 p_unit->flags,
                 p_unit->base_pts,
                 p_unit->base_dts,
                 p_unit->dependent_pts,
                 p_unit->dependent_dts,
                 p_block->i_pts,
                 p_block->i_dts,
                 p_unit->merged_size);
    }
}

static void open3dblurayArmMvcRestart(demux_t *p_demux, const char *psz_reason)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    p_sys->b_mvc_first_unit = true;
    p_sys->b_mvc_seen_matched = false;
    p_sys->b_mvc_next_block_discontinuity = true;
    p_sys->b_mvc_hold_unmatched_until_match = true;

    msg_Dbg(p_demux, "%s mvc_restart_arm reason=%s",
            OPEN3DBLURAY_LOG_PREFIX, psz_reason ? psz_reason : "unknown");
}
#endif

static bool es_pair_compare_PID(const es_pair_t *p_pair, const void *p_pid)
{
    return p_pair->fmt.i_id == *((const int *)p_pid);
}

static bool es_pair_compare_ES(const es_pair_t *p_pair, const void *p_es)
{
    return p_pair->p_es == (const es_out_id_t *)p_es;
}

static bool es_pair_compare_Unused(const es_pair_t *p_pair, const void *priv)
{
    VLC_UNUSED(priv);
    return p_pair->b_recyling;
}

static es_pair_t *getEsPairByPID(vlc_array_t *p_array, int i_pid)
{
    return getEsPair(p_array, es_pair_compare_PID, &i_pid);
}

static es_pair_t *getEsPairByES(vlc_array_t *p_array, const es_out_id_t *p_es)
{
    return getEsPair(p_array, es_pair_compare_ES, p_es);
}

static int open3dblurayMakeForcedSubtitleEsId(int i_pid)
{
    return (int)(OPEN3DBLURAY_FORCED_SUBTITLE_ID_BASE |
                 ((uint32_t)i_pid & 0xffffu));
}

static bool open3dblurayDecodeForcedSubtitleEsId(int i_id, int *pi_pid)
{
    const uint32_t u_id = (uint32_t)i_id;

    if ((u_id & 0xffff0000u) != OPEN3DBLURAY_FORCED_SUBTITLE_ID_BASE)
        return false;

    const int i_pid = (int)(u_id & 0xffffu);
    if (i_pid <= 0)
        return false;

    if (pi_pid != NULL)
        *pi_pid = i_pid;
    return true;
}

static bool open3dblurayForcedSubtitleTrackExposureEnabled(void)
{
    const char *env = getenv("OPEN3D_BLURAY_EXPOSE_FORCED_ONLY_TRACKS");
    if (env == NULL || *env == '\0')
        return true;

    return strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0;
}

static bool open3dblurayTracePgsBlocksEnabled(void)
{
    const char *env = getenv("OPEN3DBLURAY_TRACE_PGS_BLOCKS");
    if (env == NULL || *env == '\0')
        return false;

    return strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0;
}

static bool open3dblurayTraceForcedFilterEnabled(void)
{
    const char *env = getenv("OPEN3DBLURAY_TRACE_FORCED_FILTER");
    if (env == NULL || *env == '\0')
        return false;

    return strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0;
}

static bool open3dblurayTraceNavInputEnabled(void)
{
    const char *env = getenv("OPEN3DBLURAY_TRACE_NAV_INPUT");
    if (env == NULL || *env == '\0')
        return false;

    return strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0;
}

static const char *open3dblurayNavKeyName(unsigned int key)
{
    switch (key) {
    case BD_VK_MOUSE_ACTIVATE: return "mouse_activate";
    case BD_VK_ENTER:          return "enter";
    case BD_VK_UP:             return "up";
    case BD_VK_DOWN:           return "down";
    case BD_VK_LEFT:           return "left";
    case BD_VK_RIGHT:          return "right";
    case BD_VK_POPUP:          return "popup";
    default:                   return "unknown";
    }
}

static const char *open3dblurayNavActionName(int query)
{
    switch (query) {
    case DEMUX_NAV_ACTIVATE: return "activate";
    case DEMUX_NAV_UP:       return "up";
    case DEMUX_NAV_DOWN:     return "down";
    case DEMUX_NAV_LEFT:     return "left";
    case DEMUX_NAV_RIGHT:    return "right";
    case DEMUX_NAV_POPUP:    return "popup";
    case DEMUX_NAV_MENU:     return "menu";
    default:                 return "unknown";
    }
}

static bool open3dblurayTraceInteractiveGraphicsBridgeEnabled(void)
{
    const char *env = getenv("OPEN3DBLURAY_TRACE_IG_BRIDGE");
    if (env == NULL || *env == '\0')
        return false;

    return strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0;
}

static bool open3dblurayTraceMenuVisualsEnabled(void)
{
    const char *env = getenv("OPEN3DBLURAY_TRACE_MENU_VISUALS");
    if (env == NULL || *env == '\0')
        return false;

    return strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0;
}

static bool open3dblurayTraceSpuSendEnabled(void)
{
    const char *env = getenv("OPEN3DBLURAY_TRACE_SPU_SEND");
    if (env == NULL || *env == '\0')
        return false;

    return strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0;
}

static bool open3dblurayTracePgRegionsEnabled(void)
{
    const char *env = getenv("OPEN3DBLURAY_TRACE_PG_REGIONS");
    if (env == NULL || *env == '\0')
        return false;

    return strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0;
}

static bool open3dblurayYieldRedundantEndOfTitleEnabled(void)
{
    const char *env = getenv("OPEN3DBLURAY_YIELD_REDUNDANT_END_OF_TITLE");
    if (env == NULL || *env == '\0')
        return false;

    return strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0;
}

static bool open3dblurayIgnoreMouseMoveProbeEnabled(void)
{
    const char *env = getenv("OPEN3DBLURAY_PROBE_IGNORE_MOUSE_MOVE");
    if (env == NULL || *env == '\0')
        return false;

    return strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0;
}

typedef struct
{
    bool enabled;
    unsigned source_width;
    unsigned source_height;
    unsigned scene_width;
    unsigned scene_height;
} open3dbluray_mouse_scene_probe_t;

static bool open3dblurayLoadMouseSceneProbe(open3dbluray_mouse_scene_probe_t *p_probe)
{
    const char *env = getenv("OPEN3DBLURAY_PROBE_MOUSE_SOURCE_TO_SCENE");
    unsigned source_width = 0;
    unsigned source_height = 0;
    unsigned scene_width = 0;
    unsigned scene_height = 0;

    if (p_probe == NULL)
        return false;

    memset(p_probe, 0, sizeof(*p_probe));

    if (env == NULL || *env == '\0')
        return false;

    if (sscanf(env, "%ux%u:%ux%u",
               &source_width, &source_height,
               &scene_width, &scene_height) != 4)
        return false;

    if (source_width == 0 || source_height == 0 ||
        scene_width == 0 || scene_height == 0)
        return false;

    p_probe->enabled = true;
    p_probe->source_width = source_width;
    p_probe->source_height = source_height;
    p_probe->scene_width = scene_width;
    p_probe->scene_height = scene_height;
    return true;
}

static bool open3dblurayNormalizeMouseCoords(const open3dbluray_mouse_scene_probe_t *p_probe,
                                             int raw_x, int raw_y,
                                             int *p_clamped_x, int *p_clamped_y,
                                             int *p_mapped_x, int *p_mapped_y)
{
    uint64_t mapped_x;
    uint64_t mapped_y;
    int clamped_x;
    int clamped_y;

    if (p_probe == NULL || !p_probe->enabled ||
        p_probe->source_width == 0 || p_probe->source_height == 0 ||
        p_probe->scene_width == 0 || p_probe->scene_height == 0 ||
        p_clamped_x == NULL || p_clamped_y == NULL ||
        p_mapped_x == NULL || p_mapped_y == NULL)
        return false;

    clamped_x = raw_x;
    clamped_y = raw_y;

    if (clamped_x < 0)
        clamped_x = 0;
    else if ((unsigned)clamped_x >= p_probe->source_width)
        clamped_x = (int)p_probe->source_width - 1;

    if (clamped_y < 0)
        clamped_y = 0;
    else if ((unsigned)clamped_y >= p_probe->source_height)
        clamped_y = (int)p_probe->source_height - 1;

    mapped_x = ((uint64_t)(unsigned)clamped_x * p_probe->scene_width) /
               p_probe->source_width;
    mapped_y = ((uint64_t)(unsigned)clamped_y * p_probe->scene_height) /
               p_probe->source_height;

    if (mapped_x >= p_probe->scene_width)
        mapped_x = p_probe->scene_width - 1;
    if (mapped_y >= p_probe->scene_height)
        mapped_y = p_probe->scene_height - 1;

    *p_clamped_x = clamped_x;
    *p_clamped_y = clamped_y;
    *p_mapped_x = (int)mapped_x;
    *p_mapped_y = (int)mapped_y;
    return true;
}

static const char *open3dblurayOverlayPlaneName(unsigned plane)
{
    switch (plane)
    {
    case BD_OVERLAY_PG:
        return "pg";
    case BD_OVERLAY_IG:
        return "ig";
    default:
        return "unknown";
    }
}

static const char *open3dblurayClipAppTypeName(unsigned app_type)
{
    switch (app_type)
    {
    case BD_CLIP_APP_TYPE_TS_MAIN_PATH_MOVIE:
        return "main-movie";
    case BD_CLIP_APP_TYPE_TS_MAIN_PATH_TIMED_SLIDESHOW:
        return "main-timed-slideshow";
    case BD_CLIP_APP_TYPE_TS_MAIN_PATH_BROWSABLE_SLIDESHOW:
        return "main-browsable-slideshow";
    case BD_CLIP_APP_TYPE_TS_SUB_PATH_BROWSABLE_SLIDESHOW:
        return "sub-browsable-slideshow";
    case BD_CLIP_APP_TYPE_TS_SUB_PATH_INTERACTIVE_MENU:
        return "sub-interactive-menu";
    case BD_CLIP_APP_TYPE_TS_SUB_PATH_TEXT_SUBTITLE:
        return "sub-text-subtitle";
    case BD_CLIP_APP_TYPE_TS_SUB_PATH_ELEMENTARY_STREAM_PATH:
        return "sub-elementary-stream";
    default:
        return "unknown";
    }
}

static const char *open3dblurayStillModeName(unsigned still_mode)
{
    switch (still_mode)
    {
    case BLURAY_STILL_NONE:
        return "none";
    case BLURAY_STILL_TIME:
        return "time";
    case BLURAY_STILL_INFINITE:
        return "infinite";
    default:
        return "unknown";
    }
}

static const char *open3dblurayStreamCodingTypeName(unsigned coding_type)
{
    switch (coding_type)
    {
    case BLURAY_STREAM_TYPE_VIDEO_MPEG1:
        return "mpeg1";
    case BLURAY_STREAM_TYPE_VIDEO_MPEG2:
        return "mpeg2";
    case BLURAY_STREAM_TYPE_VIDEO_VC1:
        return "vc1";
    case BLURAY_STREAM_TYPE_VIDEO_H264:
        return "h264";
    case BD_STREAM_TYPE_VIDEO_HEVC:
        return "hevc";
    case BLURAY_STREAM_TYPE_AUDIO_MPEG1:
        return "audio-mpeg1";
    case BLURAY_STREAM_TYPE_AUDIO_MPEG2:
        return "audio-mpeg2";
    case BLURAY_STREAM_TYPE_AUDIO_LPCM:
        return "lpcm";
    case BLURAY_STREAM_TYPE_AUDIO_AC3:
        return "ac3";
    case BLURAY_STREAM_TYPE_AUDIO_DTS:
        return "dts";
    case BLURAY_STREAM_TYPE_AUDIO_TRUHD:
        return "truehd";
    case BLURAY_STREAM_TYPE_AUDIO_AC3PLUS:
        return "ac3plus";
    case BLURAY_STREAM_TYPE_AUDIO_DTSHD:
        return "dtshd";
    case BLURAY_STREAM_TYPE_AUDIO_DTSHD_MASTER:
        return "dtshd-master";
    case BLURAY_STREAM_TYPE_AUDIO_AC3PLUS_SECONDARY:
        return "ac3plus-secondary";
    case BLURAY_STREAM_TYPE_AUDIO_DTSHD_SECONDARY:
        return "dtshd-secondary";
    case BLURAY_STREAM_TYPE_SUB_PG:
        return "pg";
    case BLURAY_STREAM_TYPE_SUB_IG:
        return "ig";
    case BLURAY_STREAM_TYPE_SUB_TEXT:
        return "text";
    default:
        return "unknown";
    }
}

static unsigned open3dblurayCountRegionChain(const subpicture_region_t *regions)
{
    unsigned count = 0;
    for (const subpicture_region_t *region = regions; region != NULL; region = region->p_next)
        ++count;
    return count;
}

typedef struct
{
    unsigned regions;
    uint64_t visible_area;
    bool bounds_valid;
    int x;
    int y;
    unsigned width;
    unsigned height;
    bool full_canvas;
} open3dbluray_region_chain_summary_t;

static void open3dbluraySummarizeRegionChain(const subpicture_region_t *regions,
                                             unsigned canvas_width,
                                             unsigned canvas_height,
                                             open3dbluray_region_chain_summary_t *summary)
{
    if (summary == NULL)
        return;

    memset(summary, 0, sizeof(*summary));

    int min_x = INT_MAX;
    int min_y = INT_MAX;
    int max_x = INT_MIN;
    int max_y = INT_MIN;

    for (const subpicture_region_t *region = regions;
         region != NULL;
         region = region->p_next)
    {
        ++summary->regions;

        const unsigned width = region->fmt.i_visible_width > 0
                             ? region->fmt.i_visible_width
                             : region->fmt.i_width;
        const unsigned height = region->fmt.i_visible_height > 0
                              ? region->fmt.i_visible_height
                              : region->fmt.i_height;
        summary->visible_area += (uint64_t)width * (uint64_t)height;

        if (width == 0 || height == 0)
            continue;

        const int region_min_x = region->i_x;
        const int region_min_y = region->i_y;
        const int region_max_x = region->i_x + (int)width;
        const int region_max_y = region->i_y + (int)height;

        if (region_min_x < min_x)
            min_x = region_min_x;
        if (region_min_y < min_y)
            min_y = region_min_y;
        if (region_max_x > max_x)
            max_x = region_max_x;
        if (region_max_y > max_y)
            max_y = region_max_y;
    }

    if (max_x > min_x && max_y > min_y)
    {
        summary->bounds_valid = true;
        summary->x = min_x;
        summary->y = min_y;
        summary->width = (unsigned)(max_x - min_x);
        summary->height = (unsigned)(max_y - min_y);
        summary->full_canvas =
            summary->x == 0 &&
            summary->y == 0 &&
            summary->width == canvas_width &&
            summary->height == canvas_height;
    }
}

static void open3dblurayTraceIgRegionChain(demux_t *p_demux,
                                           const char *phase,
                                           const bluray_overlay_t *p_ov,
                                           bool use_direct_bridge,
                                           bool menu_open)
{
    if (!open3dblurayTraceInteractiveGraphicsBridgeEnabled() ||
        p_demux == NULL || phase == NULL || p_ov == NULL ||
        p_ov->plane != BD_OVERLAY_IG)
        return;

    open3dbluray_region_chain_summary_t summary;
    open3dbluraySummarizeRegionChain(p_ov->p_regions, (unsigned)p_ov->width,
                                     (unsigned)p_ov->height, &summary);

    static open3dbluray_region_chain_summary_t last_summary;
    static char last_phase[32];
    static bool last_direct = false;
    static bool last_menu_open = false;
    static bool last_valid = false;

    if (last_valid &&
        strcmp(last_phase, phase) == 0 &&
        last_direct == use_direct_bridge &&
        last_menu_open == menu_open &&
        memcmp(&last_summary, &summary, sizeof(summary)) == 0)
        return;

    last_summary = summary;
    last_direct = use_direct_bridge;
    last_menu_open = menu_open;
    last_valid = true;
    snprintf(last_phase, sizeof(last_phase), "%s", phase);

    msg_Warn(p_demux,
             "open3dbluray trace ig-coverage phase=%s direct=%d menu_open=%d "
             "regions=%u area=%" PRIu64 " canvas=%ux%u bounds_valid=%d "
             "bounds=%d,%d %ux%u full_canvas=%d pts90k=%" PRId64,
             phase,
             use_direct_bridge ? 1 : 0,
             menu_open ? 1 : 0,
             summary.regions,
             summary.visible_area,
             p_ov->width,
             p_ov->height,
             summary.bounds_valid ? 1 : 0,
             summary.bounds_valid ? summary.x : 0,
             summary.bounds_valid ? summary.y : 0,
             summary.bounds_valid ? summary.width : 0,
             summary.bounds_valid ? summary.height : 0,
             summary.full_canvas ? 1 : 0,
             p_ov->pts90k);
}

static void open3dblurayTraceOverlayCoverage(demux_t *p_demux,
                                             const char *phase,
                                             const bluray_overlay_t *p_ov,
                                             bool use_direct_bridge,
                                             bool menu_open)
{
    if (!open3dblurayTraceMenuVisualsEnabled() ||
        p_demux == NULL || phase == NULL || p_ov == NULL)
        return;

    open3dbluray_region_chain_summary_t summary;
    const unsigned plane = (unsigned)p_ov->plane;
    const unsigned slot = plane < MAX_OVERLAY ? plane : 0;
    open3dbluraySummarizeRegionChain(p_ov->p_regions, (unsigned)p_ov->width,
                                     (unsigned)p_ov->height, &summary);

    static open3dbluray_region_chain_summary_t last_summary[MAX_OVERLAY];
    static char last_phase[MAX_OVERLAY][32];
    static bool last_direct[MAX_OVERLAY];
    static bool last_menu_open[MAX_OVERLAY];
    static bool last_valid[MAX_OVERLAY];

    if (plane < MAX_OVERLAY &&
        last_valid[slot] &&
        strcmp(last_phase[slot], phase) == 0 &&
        last_direct[slot] == use_direct_bridge &&
        last_menu_open[slot] == menu_open &&
        memcmp(&last_summary[slot], &summary, sizeof(summary)) == 0)
        return;

    if (plane < MAX_OVERLAY)
    {
        last_summary[slot] = summary;
        last_direct[slot] = use_direct_bridge;
        last_menu_open[slot] = menu_open;
        last_valid[slot] = true;
        snprintf(last_phase[slot], sizeof(last_phase[slot]), "%s", phase);
    }

    msg_Warn(p_demux,
             "open3dbluray trace overlay-coverage phase=%s plane=%s direct=%d menu_open=%d "
             "regions=%u area=%" PRIu64 " canvas=%ux%u bounds_valid=%d "
             "bounds=%d,%d %ux%u full_canvas=%d pts90k=%" PRId64,
             phase,
             open3dblurayOverlayPlaneName(plane),
             use_direct_bridge ? 1 : 0,
             menu_open ? 1 : 0,
             summary.regions,
             summary.visible_area,
             p_ov->width,
             p_ov->height,
             summary.bounds_valid ? 1 : 0,
             summary.bounds_valid ? summary.x : 0,
             summary.bounds_valid ? summary.y : 0,
             summary.bounds_valid ? summary.width : 0,
             summary.bounds_valid ? summary.height : 0,
             summary.full_canvas ? 1 : 0,
             p_ov->pts90k);
}

static void open3dblurayTracePgRegionChain(demux_t *p_demux,
                                           const char *phase,
                                           const bluray_overlay_t *p_ov)
{
    if (!open3dblurayTracePgRegionsEnabled() ||
        p_demux == NULL || phase == NULL || p_ov == NULL ||
        p_ov->plane != BD_OVERLAY_PG)
        return;

    const unsigned total_regions = open3dblurayCountRegionChain(p_ov->p_regions);
    msg_Dbg(p_demux,
            "open3dbluray trace pg-region-chain phase=%s regions=%u size=%ux%u pts90k=%" PRId64 " offset_valid=%d offset_seq=%u offset_signed=%d offset_frame=%d",
            phase,
            total_regions,
            p_ov->width,
            p_ov->height,
            p_ov->pts90k,
            p_ov->b_subtitle_offset_valid ? 1 : 0,
            p_ov->i_subtitle_offset_sequence,
            (int)p_ov->i_subtitle_offset_signed,
            p_ov->i_subtitle_offset_frame);

    unsigned index = 0;
    for (const subpicture_region_t *region = p_ov->p_regions;
         region != NULL && index < 4;
         region = region->p_next, ++index)
    {
        const picture_t *picture = region->p_picture;
        const video_format_t *picture_format =
            picture != NULL ? &picture->format : NULL;
        const plane_t *plane0 =
            picture != NULL && picture->i_planes > 0 ? &picture->p[0] : NULL;

        msg_Dbg(p_demux,
                "open3dbluray trace pg-region phase=%s idx=%u bitmap=%d text=%d xy=%d,%d fmt=%ux%u vis=%ux%u picfmt=%ux%u picvis=%ux%u planes=%d plane0_lines=%d plane0_pitch=%d plane0_visible_lines=%d plane0_visible_pitch=%d",
                phase,
                index,
                picture != NULL ? 1 : 0,
                region->p_text != NULL ? 1 : 0,
                region->i_x,
                region->i_y,
                region->fmt.i_width,
                region->fmt.i_height,
                region->fmt.i_visible_width,
                region->fmt.i_visible_height,
                picture_format != NULL ? picture_format->i_width : 0,
                picture_format != NULL ? picture_format->i_height : 0,
                picture_format != NULL ? picture_format->i_visible_width : 0,
                picture_format != NULL ? picture_format->i_visible_height : 0,
                picture != NULL ? picture->i_planes : 0,
                plane0 != NULL ? plane0->i_lines : 0,
                plane0 != NULL ? plane0->i_pitch : 0,
                plane0 != NULL ? plane0->i_visible_lines : 0,
                plane0 != NULL ? plane0->i_visible_pitch : 0);
    }

    if (index == 4 && total_regions > 4)
        msg_Dbg(p_demux,
                "open3dbluray trace pg-region phase=%s truncated=1 total_regions=%u",
                phase,
                total_regions);
}

static bool open3dblurayTraceArgbAccessEnabled(const bluray_overlay_t *p_ov,
                                               const BD_ARGB_OVERLAY *eventov,
                                               const subpicture_region_t *p_reg)
{
    if (!open3dblurayTraceInteractiveGraphicsBridgeEnabled() ||
        p_ov == NULL || eventov == NULL || p_reg == NULL)
        return false;
    return true;
}

static const char *open3dblurayArgbCmdName(uint8_t cmd)
{
    switch (cmd)
    {
        case BD_ARGB_OVERLAY_INIT:
            return "init";
        case BD_ARGB_OVERLAY_CLOSE:
            return "close";
        case BD_ARGB_OVERLAY_DRAW:
            return "draw";
        case BD_ARGB_OVERLAY_FLUSH:
            return "flush";
        default:
            return "other";
    }
}

static const char *open3dblurayArgbShapeName(const BD_ARGB_OVERLAY *eventov)
{
    if (eventov == NULL)
        return "other";

    if (eventov->x == 0 && eventov->y == 0 &&
        eventov->w == 1920 && eventov->h == 1080)
        return "full";

    if (eventov->x == 396 && eventov->y == 888 &&
        eventov->w == 1126 && eventov->h == 92)
        return "strip";

    return "other";
}

static void open3dblurayTraceArgbEntry(demux_t *p_demux,
                                       demux_sys_t *p_sys,
                                       const BD_ARGB_OVERLAY *eventov)
{
    static unsigned trace_count = 0;
    const bluray_overlay_t *p_ov = NULL;
    const subpicture_region_t *p_reg = NULL;

    if (p_demux == NULL || p_sys == NULL || eventov == NULL ||
        !open3dblurayTraceInteractiveGraphicsBridgeEnabled() ||
        trace_count >= 48)
        return;

    if (eventov->plane < MAX_OVERLAY)
    {
        p_ov = p_sys->p_overlays[eventov->plane];
        if (p_ov != NULL)
            p_reg = p_ov->p_regions;
    }

    trace_count++;
    fprintf(stderr,
            "open3dbluray raw argb-entry count=%u cmd=%s handle=%p plane=%u "
            "rect=%u,%u-%u,%u stride=%u argb_present=%d\n",
            trace_count,
            open3dblurayArgbCmdName(eventov->cmd),
            (void *)p_demux,
            (unsigned)eventov->plane,
            (unsigned)eventov->x,
            (unsigned)eventov->y,
            (unsigned)(eventov->x + eventov->w - 1),
            (unsigned)(eventov->y + eventov->h - 1),
            (unsigned)eventov->stride,
            eventov->argb != NULL ? 1 : 0);
    fflush(stderr);
    msg_Warn(p_demux,
             "open3dbluray trace argb-entry count=%u cmd=%s plane=%u pts=%" PRId64 " "
             "rect=%u,%u-%u,%u stride=%u ov_present=%d ov_size=%ux%u "
             "reg_present=%d reg_chroma=0x%08x reg_size=%ux%u argb_present=%d",
             trace_count,
             open3dblurayArgbCmdName(eventov->cmd),
             (unsigned)eventov->plane,
             eventov->pts,
             (unsigned)eventov->x,
             (unsigned)eventov->y,
             (unsigned)(eventov->x + eventov->w - 1),
             (unsigned)(eventov->y + eventov->h - 1),
             (unsigned)eventov->stride,
             p_ov != NULL ? 1 : 0,
             p_ov != NULL ? p_ov->width : 0,
             p_ov != NULL ? p_ov->height : 0,
             p_reg != NULL ? 1 : 0,
             p_reg != NULL ? (unsigned)p_reg->fmt.i_chroma : 0U,
             p_reg != NULL ? p_reg->fmt.i_width : 0,
             p_reg != NULL ? p_reg->fmt.i_height : 0,
             eventov->argb != NULL ? 1 : 0);
}

static void open3dblurayTraceArgbSkip(demux_t *p_demux,
                                      const bluray_overlay_t *p_ov,
                                      const BD_ARGB_OVERLAY *eventov,
                                      const subpicture_region_t *p_reg,
                                      const char *reason)
{
    static unsigned trace_count = 0;

    if (p_demux == NULL || eventov == NULL || reason == NULL ||
        !open3dblurayTraceInteractiveGraphicsBridgeEnabled() ||
        trace_count >= 48)
        return;

    trace_count++;
    msg_Warn(p_demux,
             "open3dbluray trace argb-skip count=%u reason=%s plane=%u "
             "rect=%u,%u-%u,%u stride=%u ov_present=%d ov_size=%ux%u "
             "reg_present=%d reg_chroma=0x%08x reg_size=%ux%u argb_present=%d",
             trace_count,
             reason,
             (unsigned)eventov->plane,
             (unsigned)eventov->x,
             (unsigned)(eventov->y),
             (unsigned)(eventov->x + eventov->w - 1),
             (unsigned)(eventov->y + eventov->h - 1),
             (unsigned)eventov->stride,
             p_ov != NULL ? 1 : 0,
             p_ov != NULL ? p_ov->width : 0,
             p_ov != NULL ? p_ov->height : 0,
             p_reg != NULL ? 1 : 0,
             p_reg != NULL ? (unsigned)p_reg->fmt.i_chroma : 0U,
             p_reg != NULL ? p_reg->fmt.i_width : 0,
             p_reg != NULL ? p_reg->fmt.i_height : 0,
             eventov->argb != NULL ? 1 : 0);
}

static bool open3dblurayReadArgbOverlayPixel(const BD_ARGB_OVERLAY *eventov,
                                             int abs_x, int abs_y,
                                             uint32_t *value)
{
    if (eventov == NULL || eventov->argb == NULL || value == NULL ||
        abs_x < eventov->x || abs_y < eventov->y ||
        abs_x >= eventov->x + eventov->w ||
        abs_y >= eventov->y + eventov->h)
        return false;

    const int rel_x = abs_x - eventov->x;
    const int rel_y = abs_y - eventov->y;
    *value = eventov->argb[rel_y * eventov->stride + rel_x];
    return true;
}

static bool open3dblurayReadArgbRegionPixel(const subpicture_region_t *p_reg,
                                            int abs_x, int abs_y,
                                            uint32_t *value)
{
    if (p_reg == NULL || p_reg->p_picture == NULL || value == NULL ||
        abs_x < 0 || abs_y < 0 ||
        abs_x >= p_reg->fmt.i_width ||
        abs_y >= p_reg->fmt.i_height)
        return false;

    const plane_t *plane = &p_reg->p_picture->p[0];
    const uint32_t *row =
        (const uint32_t *)(const void *)(plane->p_pixels + plane->i_pitch * abs_y);
    *value = row[abs_x];
    return true;
}

static void open3dblurayFormatArgbSample(char *buf, size_t buf_size,
                                         bool available, uint32_t value)
{
    if (buf_size == 0)
        return;

    if (!available)
    {
        snprintf(buf, buf_size, "na");
        return;
    }

    snprintf(buf, buf_size, "0x%08" PRIx32, value);
}

static void open3dblurayTraceArgbAccessSamples(demux_t *p_demux,
                                               const bluray_overlay_t *p_ov,
                                               const BD_ARGB_OVERLAY *eventov,
                                               const subpicture_region_t *p_reg)
{
    static unsigned trace_count = 0;
    const char *shape = "other";

    if (p_demux == NULL ||
        !open3dblurayTraceArgbAccessEnabled(p_ov, eventov, p_reg) ||
        trace_count >= 32)
        return;

    trace_count++;
    shape = open3dblurayArgbShapeName(eventov);

    uint32_t src00 = 0, src960 = 0, src500 = 0, src1000 = 0, src1300 = 0;
    uint32_t dst00 = 0, dst960 = 0, dst500 = 0, dst1000 = 0, dst1300 = 0;
    char src00_buf[16], src960_buf[16], src500_buf[16], src1000_buf[16], src1300_buf[16];
    char dst00_buf[16], dst960_buf[16], dst500_buf[16], dst1000_buf[16], dst1300_buf[16];

    open3dblurayFormatArgbSample(src00_buf, sizeof(src00_buf),
                                 open3dblurayReadArgbOverlayPixel(eventov, 0, 0, &src00), src00);
    open3dblurayFormatArgbSample(src960_buf, sizeof(src960_buf),
                                 open3dblurayReadArgbOverlayPixel(eventov, 960, 540, &src960), src960);
    open3dblurayFormatArgbSample(src500_buf, sizeof(src500_buf),
                                 open3dblurayReadArgbOverlayPixel(eventov, 500, 920, &src500), src500);
    open3dblurayFormatArgbSample(src1000_buf, sizeof(src1000_buf),
                                 open3dblurayReadArgbOverlayPixel(eventov, 1000, 920, &src1000), src1000);
    open3dblurayFormatArgbSample(src1300_buf, sizeof(src1300_buf),
                                 open3dblurayReadArgbOverlayPixel(eventov, 1300, 920, &src1300), src1300);

    open3dblurayFormatArgbSample(dst00_buf, sizeof(dst00_buf),
                                 open3dblurayReadArgbRegionPixel(p_reg, 0, 0, &dst00), dst00);
    open3dblurayFormatArgbSample(dst960_buf, sizeof(dst960_buf),
                                 open3dblurayReadArgbRegionPixel(p_reg, 960, 540, &dst960), dst960);
    open3dblurayFormatArgbSample(dst500_buf, sizeof(dst500_buf),
                                 open3dblurayReadArgbRegionPixel(p_reg, 500, 920, &dst500), dst500);
    open3dblurayFormatArgbSample(dst1000_buf, sizeof(dst1000_buf),
                                 open3dblurayReadArgbRegionPixel(p_reg, 1000, 920, &dst1000), dst1000);
    open3dblurayFormatArgbSample(dst1300_buf, sizeof(dst1300_buf),
                                 open3dblurayReadArgbRegionPixel(p_reg, 1300, 920, &dst1300), dst1300);

    fprintf(stderr,
            "open3dbluray raw argb-access count=%u shape=%s ov_plane=%u event_plane=%u "
            "ov_size=%ux%u reg_size=%ux%u rect=%d,%d-%d,%d stride=%d "
            "src00=%s src960x540=%s src500x920=%s src1000x920=%s src1300x920=%s "
            "dst00=%s dst960x540=%s dst500x920=%s dst1000x920=%s dst1300x920=%s\n",
            trace_count,
            shape,
            (unsigned)p_ov->plane,
            (unsigned)eventov->plane,
            p_ov->width,
            p_ov->height,
            p_reg->fmt.i_width,
            p_reg->fmt.i_height,
            eventov->x,
            eventov->y,
            eventov->x + eventov->w - 1,
            eventov->y + eventov->h - 1,
            eventov->stride,
            src00_buf,
            src960_buf,
            src500_buf,
            src1000_buf,
            src1300_buf,
            dst00_buf,
            dst960_buf,
            dst500_buf,
            dst1000_buf,
            dst1300_buf);
    fflush(stderr);

    msg_Warn(p_demux,
             "open3dbluray trace argb-access count=%u shape=%s ov_plane=%u event_plane=%u "
             "ov_size=%ux%u reg_size=%ux%u rect=%d,%d-%d,%d stride=%d "
             "src00=%s src960x540=%s src500x920=%s src1000x920=%s src1300x920=%s "
             "dst00=%s dst960x540=%s dst500x920=%s dst1000x920=%s dst1300x920=%s",
             trace_count,
             shape,
             (unsigned)p_ov->plane,
             (unsigned)eventov->plane,
             p_ov->width,
             p_ov->height,
             p_reg->fmt.i_width,
             p_reg->fmt.i_height,
             eventov->x,
             eventov->y,
             eventov->x + eventov->w - 1,
             eventov->y + eventov->h - 1,
             eventov->stride,
             src00_buf,
             src960_buf,
             src500_buf,
             src1000_buf,
             src1300_buf,
             dst00_buf,
             dst960_buf,
             dst500_buf,
             dst1000_buf,
             dst1300_buf);
}

static void open3dblurayTraceArgbUpdaterSamples(const bluray_overlay_t *p_ov,
                                                const subpicture_region_t *p_src_reg,
                                                const subpicture_region_t *p_dst_reg)
{
    static unsigned trace_count = 0;
    uint32_t src00 = 0, src960 = 0, src500 = 0, src1000 = 0, src1300 = 0;
    uint32_t dst00 = 0, dst960 = 0, dst500 = 0, dst1000 = 0, dst1300 = 0;
    char src00_buf[16], src960_buf[16], src500_buf[16], src1000_buf[16], src1300_buf[16];
    char dst00_buf[16], dst960_buf[16], dst500_buf[16], dst1000_buf[16], dst1300_buf[16];

    if (!open3dblurayTraceInteractiveGraphicsBridgeEnabled() ||
        p_ov == NULL || p_src_reg == NULL || p_dst_reg == NULL ||
        p_ov->plane != BD_OVERLAY_IG ||
        p_src_reg->p_picture == NULL ||
        p_dst_reg->p_picture == NULL ||
        trace_count >= 32)
        return;

    trace_count++;

    open3dblurayFormatArgbSample(src00_buf, sizeof(src00_buf),
                                 open3dblurayReadArgbRegionPixel(p_src_reg, 0, 0, &src00), src00);
    open3dblurayFormatArgbSample(src960_buf, sizeof(src960_buf),
                                 open3dblurayReadArgbRegionPixel(p_src_reg, 960, 540, &src960), src960);
    open3dblurayFormatArgbSample(src500_buf, sizeof(src500_buf),
                                 open3dblurayReadArgbRegionPixel(p_src_reg, 500, 920, &src500), src500);
    open3dblurayFormatArgbSample(src1000_buf, sizeof(src1000_buf),
                                 open3dblurayReadArgbRegionPixel(p_src_reg, 1000, 920, &src1000), src1000);
    open3dblurayFormatArgbSample(src1300_buf, sizeof(src1300_buf),
                                 open3dblurayReadArgbRegionPixel(p_src_reg, 1300, 920, &src1300), src1300);

    open3dblurayFormatArgbSample(dst00_buf, sizeof(dst00_buf),
                                 open3dblurayReadArgbRegionPixel(p_dst_reg, 0, 0, &dst00), dst00);
    open3dblurayFormatArgbSample(dst960_buf, sizeof(dst960_buf),
                                 open3dblurayReadArgbRegionPixel(p_dst_reg, 960, 540, &dst960), dst960);
    open3dblurayFormatArgbSample(dst500_buf, sizeof(dst500_buf),
                                 open3dblurayReadArgbRegionPixel(p_dst_reg, 500, 920, &dst500), dst500);
    open3dblurayFormatArgbSample(dst1000_buf, sizeof(dst1000_buf),
                                 open3dblurayReadArgbRegionPixel(p_dst_reg, 1000, 920, &dst1000), dst1000);
    open3dblurayFormatArgbSample(dst1300_buf, sizeof(dst1300_buf),
                                 open3dblurayReadArgbRegionPixel(p_dst_reg, 1300, 920, &dst1300), dst1300);

    fprintf(stderr,
            "open3dbluray raw argb-updater count=%u plane=%u ov_size=%ux%u "
            "src_reg=%ux%u dst_reg=%ux%u src00=%s src960x540=%s src500x920=%s "
            "src1000x920=%s src1300x920=%s dst00=%s dst960x540=%s dst500x920=%s "
            "dst1000x920=%s dst1300x920=%s\n",
            trace_count,
            (unsigned)p_ov->plane,
            p_ov->width,
            p_ov->height,
            p_src_reg->fmt.i_width,
            p_src_reg->fmt.i_height,
            p_dst_reg->fmt.i_width,
            p_dst_reg->fmt.i_height,
            src00_buf,
            src960_buf,
            src500_buf,
            src1000_buf,
            src1300_buf,
            dst00_buf,
            dst960_buf,
            dst500_buf,
            dst1000_buf,
            dst1300_buf);
    fflush(stderr);
}

static void open3dblurayTracePgsBlock(demux_t *p_demux,
                                      int i_pid,
                                      const block_t *p_block)
{
    static unsigned s_trace_count = 0;
    char hexbuf[3 * 32 + 1];
    size_t i_hex = 0;
    size_t i_dump = 0;

    if (!open3dblurayTracePgsBlocksEnabled() || p_block == NULL || p_block->p_buffer == NULL)
        return;

    if (s_trace_count >= 16)
        return;
    s_trace_count++;

    i_dump = p_block->i_buffer < 32 ? p_block->i_buffer : 32;
    for (size_t i = 0; i < i_dump && i_hex + 3 < sizeof(hexbuf); ++i) {
        snprintf(&hexbuf[i_hex], sizeof(hexbuf) - i_hex, "%02x%s",
                 p_block->p_buffer[i], (i + 1 < i_dump) ? " " : "");
        i_hex += (i + 1 < i_dump) ? 3 : 2;
    }
    hexbuf[i_hex] = '\0';

    msg_Info(p_demux,
             "%s trace_pgs_block pid=0x%04x size=%zu pts=%" PRId64 " dts=%" PRId64 " flags=0x%x head=%s",
             OPEN3DBLURAY_LOG_PREFIX,
             i_pid,
             p_block->i_buffer,
             p_block->i_pts,
             p_block->i_dts,
             p_block->i_flags,
             hexbuf);
}

static const char *open3dblurayPgsSegmentName(uint8_t i_segment_type)
{
    switch (i_segment_type) {
    case 0x14: return "PDS";
    case 0x15: return "ODS";
    case 0x16: return "PCS";
    case 0x17: return "WDS";
    case 0x80: return "END";
    default:   return "unknown";
    }
}

static void open3dblurayTraceSpuSend(demux_t *p_demux,
                                     int i_selected_pid,
                                     const es_pair_t *p_pair,
                                     const block_t *p_block,
                                     const char *psz_route)
{
    static unsigned s_trace_count = 0;
    int i_real_pid = -1;
    const bool b_forced_alias =
        p_pair != NULL && open3dblurayDecodeForcedSubtitleEsId(p_pair->fmt.i_id, &i_real_pid);
    const uint8_t i_segment_type =
        (p_block != NULL && p_block->p_buffer != NULL && p_block->i_buffer > 0)
            ? p_block->p_buffer[0] : 0xff;

    if (!open3dblurayTraceSpuSendEnabled() ||
        p_demux == NULL || p_pair == NULL || p_block == NULL ||
        p_pair->fmt.i_cat != SPU_ES)
        return;

    if (!b_forced_alias)
        i_real_pid = p_pair->fmt.i_id;

    if (i_selected_pid > 0 && i_real_pid != i_selected_pid)
        return;

    if (s_trace_count >= 64)
        return;
    s_trace_count++;

    msg_Info(p_demux,
             "%s trace_spu_send route=%s es_id=0x%08x pid=0x%04x forced_alias=%d selected_pid=0x%04x bytes=%zu pts=%" PRId64 " dts=%" PRId64 " flags=0x%x seg=%s(0x%02x)",
             OPEN3DBLURAY_LOG_PREFIX,
             psz_route != NULL ? psz_route : "unknown",
             p_pair->fmt.i_id,
             i_real_pid,
             b_forced_alias ? 1 : 0,
             i_selected_pid,
             p_block->i_buffer,
             p_block->i_pts,
             p_block->i_dts,
             p_block->i_flags,
             open3dblurayPgsSegmentName(i_segment_type),
             i_segment_type);
}

static unsigned open3dblurayCountTsSyncHits(const uint8_t *p_buf,
                                            size_t i_size,
                                            size_t i_stride,
                                            size_t i_sync,
                                            unsigned i_limit)
{
    unsigned i_hits = 0;
    size_t i_pos = i_sync;

    if (p_buf == NULL || i_stride == 0 || i_sync >= i_size)
        return 0;

    while (i_pos < i_size && i_hits < i_limit) {
        if (p_buf[i_pos] != 0x47)
            break;
        ++i_hits;
        if (i_size - i_pos <= i_stride)
            break;
        i_pos += i_stride;
    }

    return i_hits;
}

static bool open3dblurayTraceSubtitleSourceEnabled(void)
{
    const char *env = getenv("OPEN3D_TRACE_BLURAY_SUBTITLE_SOURCE");
    return env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
}

static void open3dblurayTraceSubtitleSourceBlock(demux_t *p_demux,
                                                 const uint8_t *p_buf,
                                                 size_t i_size)
{
    enum { OPEN3D_SUBTITLE_SOURCE_TRACE_MAX_BLOCKS = 1536 };
    static unsigned s_trace_blocks = 0;
    static unsigned s_first_hit_block = 0;
    static unsigned s_total_seen[5] = {0};
    static unsigned s_total_payload1[5] = {0};
    static unsigned s_total_payload1_unitstart1[5] = {0};
    static unsigned s_total_payload0[5] = {0};
    int first47[4] = {-1, -1, -1, -1};
    size_t i_scan = 0;
    unsigned cand188_0 = 0;
    unsigned cand188_4 = 0;
    unsigned cand192_0 = 0;
    unsigned cand192_4 = 0;
    size_t i_stride = 0;
    size_t i_sync = 0;
    unsigned i_hits = 0;
    unsigned i_packets = 0;
    unsigned seen[5] = {0};
    unsigned payload1[5] = {0};
    unsigned payload1_unitstart1[5] = {0};
    unsigned payload0[5] = {0};
    bool b_any_cluster = false;

    if (!open3dblurayTraceSubtitleSourceEnabled() ||
        p_buf == NULL || i_size < 188 ||
        s_trace_blocks >= OPEN3D_SUBTITLE_SOURCE_TRACE_MAX_BLOCKS)
        return;

    s_trace_blocks++;

    i_scan = i_size < 512 ? i_size : 512;
    for (size_t i = 0, j = 0; i < i_scan && j < 4; ++i) {
        if (p_buf[i] == 0x47)
            first47[j++] = (int)i;
    }

    cand188_0 = open3dblurayCountTsSyncHits(p_buf, i_size, 188, 0, 16);
    cand188_4 = open3dblurayCountTsSyncHits(p_buf, i_size, 188, 4, 16);
    cand192_0 = open3dblurayCountTsSyncHits(p_buf, i_size, 192, 0, 16);
    cand192_4 = open3dblurayCountTsSyncHits(p_buf, i_size, 192, 4, 16);

    if (cand188_0 > i_hits) {
        i_stride = 188;
        i_sync = 0;
        i_hits = cand188_0;
    }
    if (cand188_4 > i_hits) {
        i_stride = 188;
        i_sync = 4;
        i_hits = cand188_4;
    }
    if (cand192_0 > i_hits) {
        i_stride = 192;
        i_sync = 0;
        i_hits = cand192_0;
    }
    if (cand192_4 > i_hits) {
        i_stride = 192;
        i_sync = 4;
        i_hits = cand192_4;
    }

    if (i_hits > 0 && i_stride > 0 && i_sync < i_size) {
        i_packets = (unsigned)((i_size - i_sync) / i_stride);
        for (unsigned i = 0; i < i_packets; ++i) {
            const uint8_t *p_ts = p_buf + i * i_stride + i_sync;
            unsigned idx;
            int i_pid;
            bool b_payload;
            bool b_unit_start;

            if (p_ts + 4 > p_buf + i_size || p_ts[0] != 0x47)
                continue;

            i_pid = ((p_ts[1] & 0x1f) << 8) | p_ts[2];
            if (i_pid < 4608 || i_pid > 4612)
                continue;

            idx = (unsigned)(i_pid - 4608);
            b_payload = (p_ts[3] & 0x10) != 0;
            b_unit_start = (p_ts[1] & 0x40) != 0;

            seen[idx]++;
            b_any_cluster = true;
            if (b_payload) {
                payload1[idx]++;
                if (b_unit_start)
                    payload1_unitstart1[idx]++;
            } else {
                payload0[idx]++;
            }
        }
    }

    for (unsigned i = 0; i < 5; ++i) {
        s_total_seen[i] += seen[i];
        s_total_payload1[i] += payload1[i];
        s_total_payload1_unitstart1[i] += payload1_unitstart1[i];
        s_total_payload0[i] += payload0[i];
    }

    if (b_any_cluster && s_first_hit_block == 0)
        s_first_hit_block = s_trace_blocks;

    if (s_trace_blocks == 1) {
        msg_Dbg(p_demux,
                "TRACE subtitleSourceBlockSig block=%u bytes=%zu first47=%d,%d,%d,%d cand188_0=%u cand188_4=%u cand192_0=%u cand192_4=%u chosen=%zu@%zu hits=%u packets=%u pid4608=%u/%u/%u/%u pid4609=%u/%u/%u/%u pid4610=%u/%u/%u/%u pid4611=%u/%u/%u/%u pid4612=%u/%u/%u/%u",
                s_trace_blocks,
                i_size,
                first47[0], first47[1], first47[2], first47[3],
                cand188_0, cand188_4, cand192_0, cand192_4,
                i_stride, i_sync,
                i_hits, i_packets,
                seen[0], payload1[0], payload1_unitstart1[0], payload0[0],
                seen[1], payload1[1], payload1_unitstart1[1], payload0[1],
                seen[2], payload1[2], payload1_unitstart1[2], payload0[2],
                seen[3], payload1[3], payload1_unitstart1[3], payload0[3],
                seen[4], payload1[4], payload1_unitstart1[4], payload0[4]);
    }

    if ((s_trace_blocks % 256) == 0 || b_any_cluster ||
        s_trace_blocks == OPEN3D_SUBTITLE_SOURCE_TRACE_MAX_BLOCKS) {
        msg_Dbg(p_demux,
                "TRACE subtitleSourceWindow blocks=%u first_hit_block=%u last_bytes=%zu last_chosen=%zu@%zu last_hits=%u last_packets=%u pid4608=%u/%u/%u/%u pid4609=%u/%u/%u/%u pid4610=%u/%u/%u/%u pid4611=%u/%u/%u/%u pid4612=%u/%u/%u/%u",
                s_trace_blocks,
                s_first_hit_block,
                i_size, i_stride, i_sync, i_hits, i_packets,
                s_total_seen[0], s_total_payload1[0], s_total_payload1_unitstart1[0], s_total_payload0[0],
                s_total_seen[1], s_total_payload1[1], s_total_payload1_unitstart1[1], s_total_payload0[1],
                s_total_seen[2], s_total_payload1[2], s_total_payload1_unitstart1[2], s_total_payload0[2],
                s_total_seen[3], s_total_payload1[3], s_total_payload1_unitstart1[3], s_total_payload0[3],
                s_total_seen[4], s_total_payload1[4], s_total_payload1_unitstart1[4], s_total_payload0[4]);
    }
}

static es_pair_t *getUnusedEsPair(vlc_array_t *p_array)
{
    return getEsPair(p_array, es_pair_compare_Unused, 0);
}

/*
 * Subpicture updater
*/
struct subpicture_updater_sys_t
{
    vlc_mutex_t          lock;      // protect p_overlay pointer and ref_cnt
    bluray_overlay_t    *p_overlay; // NULL if overlay has been closed
    int                  ref_cnt;   // one reference in vout (subpicture_t), one in input (bluray_overlay_t)
};

/*
 * cut the connection between vout and overlay.
 * - called when vout is closed or overlay is closed.
 * - frees subpicture_updater_sys_t when both sides have been closed.
 */
static void unref_subpicture_updater(subpicture_updater_sys_t *p_sys)
{
    vlc_mutex_lock(&p_sys->lock);
    int refs = --p_sys->ref_cnt;
    p_sys->p_overlay = NULL;
    vlc_mutex_unlock(&p_sys->lock);

    if (refs < 1) {
        vlc_mutex_destroy(&p_sys->lock);
        free(p_sys);
    }
}

/*
 * Normalize one VLC preferred-language variable to a single ISO639-2/T code.
 *
 * This intentionally mirrors only the subset of src/input/es_out.c behavior
 * that we need here. libbluray player settings accept one language code, so we
 * use only the first list entry when VLC is configured with a comma-separated
 * preference list.
 */
static const char *open3dblurayGetPreferredLanguageCode(demux_t *p_demux,
                                                        const char *psz_var)
{
    const iso639_lang_t *pl;
    char *psz_lang;
    char *p;

    psz_lang = var_CreateGetString( p_demux, psz_var );
    if( !psz_lang )
        return LANGUAGE_DEFAULT;

    /* XXX: we will use only the first value
     * (and ignore other ones in case of a list) */
    if( ( p = strchr( psz_lang, ',' ) ) )
        *p = '\0';

    for( pl = p_languages; pl->psz_eng_name != NULL; pl++ )
    {
        if( *psz_lang == '\0' )
            continue;
        if( !strcasecmp( pl->psz_eng_name, psz_lang ) ||
            !strcasecmp( pl->psz_iso639_1, psz_lang ) ||
            !strcasecmp( pl->psz_iso639_2T, psz_lang ) ||
            !strcasecmp( pl->psz_iso639_2B, psz_lang ) )
            break;
    }

    free( psz_lang );

    if( pl->psz_eng_name != NULL )
        return pl->psz_iso639_2T;

    return LANGUAGE_DEFAULT;
}

static int open3dblurayFindPgStreamIndexByPid(const BLURAY_CLIP_INFO *p_clip_info,
                                              int i_pid,
                                              const BLURAY_STREAM_INFO **pp_stream)
{
    if (pp_stream != NULL)
        *pp_stream = NULL;

    if (p_clip_info == NULL || i_pid <= 0)
        return -1;

    for (int i = 0; i < p_clip_info->pg_stream_count; ++i) {
        const BLURAY_STREAM_INFO *p_stream = &p_clip_info->pg_streams[i];
        if (p_stream->pid != i_pid)
            continue;

        if (pp_stream != NULL)
            *pp_stream = p_stream;
        return i;
    }

    return -1;
}

static int open3dblurayFindPgStreamIndexByLanguage(const BLURAY_CLIP_INFO *p_clip_info,
                                                   const char *psz_lang,
                                                   const BLURAY_STREAM_INFO **pp_stream)
{
    if (pp_stream != NULL)
        *pp_stream = NULL;

    if (p_clip_info == NULL || psz_lang == NULL || psz_lang[0] == '\0')
        return -1;

    for (int i = 0; i < p_clip_info->pg_stream_count; ++i) {
        const BLURAY_STREAM_INFO *p_stream = &p_clip_info->pg_streams[i];
        if (strncasecmp((const char *)p_stream->lang, psz_lang, 3) != 0)
            continue;

        if (pp_stream != NULL)
            *pp_stream = p_stream;
        return i;
    }

    return -1;
}

static void open3dblurayApplyMenulessPgLanguageSetting(demux_t *p_demux,
                                                       const BLURAY_STREAM_INFO *p_stream)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->b_menu || p_stream == NULL)
        return;

    bd_set_player_setting_str(p_sys->bluray, BLURAY_PLAYER_SETTING_PG_LANG,
                              (const char *)p_stream->lang);
}

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
typedef struct bluray_esout_sys_t bluray_esout_sys_t;

static es_out_t *esOutNew(vlc_object_t*, es_out_t *, void *);
static es_out_t *escape_esOutNew(vlc_object_t *, es_out_t *);

static int   blurayControl(demux_t *, int, va_list);
static int   blurayDemux(demux_t *);

static void  blurayInitTitles(demux_t *p_demux, uint32_t menu_titles);
static int   bluraySetTitle(demux_t *p_demux, int i_title);

static void  blurayOverlayProc(void *ptr, const BD_OVERLAY * const overlay);
static void  blurayArgbOverlayProc(void *ptr, const BD_ARGB_OVERLAY * const overlay);

static int   onMouseEvent(vlc_object_t *p_vout, const char *psz_var,
                          vlc_value_t old, vlc_value_t val, void *p_data);
static int   onIntfEvent(vlc_object_t *, char const *,
                         vlc_value_t, vlc_value_t, void *);

static void  blurayRestartParser(demux_t *p_demux, bool, bool);
static void  blurayOnUserStreamSelection(demux_t *p_demux, int i_pid);
static void  blurayOnUserStreamSelectionEx(demux_t *p_demux,
                                           int i_pid,
                                           bool b_auto_forced_only);
static void  open3dblurayRememberSubtitleSelectionEx(demux_t *p_demux,
                                                     int i_pid,
                                                     bool b_forced_only,
                                                     bool b_auto_forced_only);
static bool  open3dblurayHasRememberedBaseSubtitleSelection(demux_t *p_demux,
                                                            int i_pid);
static bool  open3dblurayTakePendingForcedStartupSubtitle(demux_t *p_demux,
                                                          int i_pid);
static void  open3dbluraySchedulePendingForcedSubtitleSelection(demux_t *p_demux,
                                                                int i_pid,
                                                                bool b_auto_forced_only);
static bool  open3dblurayTakePendingForcedSubtitleSelection(demux_t *p_demux,
                                                            int *pi_pid,
                                                            bool *pb_auto_forced_only);
static bool  open3dblurayHasRememberedForcedSubtitleSelection(demux_t *p_demux,
                                                              int i_pid);
static bool  open3dblurayHasRememberedAutoForcedSubtitleSelection(demux_t *p_demux,
                                                                  int i_pid);
static bool  open3dblurayShouldAutoPromotePreferredSubtitle(demux_t *p_demux,
                                                            int i_pid,
                                                            int i_selected_pid);
static bool  open3dblurayHasForcedSubtitleTrack(demux_t *p_demux,
                                                int i_real_spu_pid);
static void  open3dblurayMaybeAutoSelectPreferredForcedSubtitle(demux_t *p_demux,
                                                                int i_real_spu_pid,
                                                                int i_selected_pid);
static bool  open3dblurayGetPreferredSubtitleStream(demux_t *p_demux,
                                                    int *pi_pid,
                                                    uint8_t *pi_offset_seq,
                                                    char psz_lang[4]);
static int   open3dblurayGetRememberedSubtitleUiId(demux_t *p_demux);
static void  open3dblurayForcedFilterClearQueued(demux_sys_t *p_sys);
static void  open3dblurayForcedFilterInvalidate(demux_t *p_demux);
static void  open3dblurayForcedFilterReset(demux_t *p_demux);
static void  open3dblurayForcedFilterSelectPid(demux_t *p_demux, int i_pid);
static int   open3dbluraySendForcedPgsBlock(demux_t *p_demux, es_out_t *p_dst_out,
                                            es_out_id_t *p_forced_es, int i_pid,
                                            block_t *p_block);
static void  open3dblurayMaybeAddForcedSubtitleTrackLocked(demux_t *p_demux,
                                                           bluray_esout_sys_t *esout_sys,
                                                           const es_pair_t *p_real_pair);
static void  open3dblurayRemoveAllForcedSubtitleTracks(demux_t *p_demux);
static void  notifyDiscontinuityToParser( demux_sys_t *p_sys );


#define STILL_IMAGE_NOT_SET    0
#define STILL_IMAGE_INFINITE  -1

#define CURRENT_TITLE p_sys->pp_title[p_demux->info.i_title]
#define CUR_LENGTH    CURRENT_TITLE->i_length

/* */
static void FindMountPoint(char **file)
{
    char *device = *file;
#ifdef HAVE_GETMNTENT_R
    /* bd path may be a symlink (e.g. /dev/dvd -> /dev/sr0), so make sure
     * we look up the real device */
    char *bd_device = realpath(device, NULL);
    if (bd_device == NULL)
        return;

    struct stat st;
    if (lstat (bd_device, &st) == 0 && S_ISBLK (st.st_mode)) {
        FILE *mtab = setmntent ("/proc/self/mounts", "r");
        if (mtab) {
            struct mntent *m, mbuf;
            char buf [8192];

            while ((m = getmntent_r (mtab, &mbuf, buf, sizeof(buf))) != NULL) {
                if (!strcmp (m->mnt_fsname, bd_device)) {
                    free(device);
                    *file = strdup(m->mnt_dir);
                    break;
                }
            }
            endmntent (mtab);
        }
    }
    free(bd_device);

#elif defined(__APPLE__)
    struct stat st;
    if (!stat (device, &st) && S_ISBLK (st.st_mode)) {
        int fs_count = getfsstat (NULL, 0, MNT_NOWAIT);
        if (fs_count > 0) {
            int bufSize = fs_count * sizeof (struct statfs);
            struct statfs* mbuf = malloc(bufSize);
            getfsstat (mbuf, bufSize, MNT_NOWAIT);
            for (int i = 0; i < fs_count; ++i)
                if (!strcmp (mbuf[i].f_mntfromname, device)) {
                    free(device);
                    *file = strdup(mbuf[i].f_mntonname);
                    free(mbuf);
                    return;
                }

            free(mbuf);
        }
    }
#else
# warning Disc device to mount point not implemented
    VLC_UNUSED( device );
#endif
}

static char *open3dblurayResolveLocalPathCaseFallback(demux_t *p_demux,
                                                      const char *psz_path)
{
    struct stat st;

    if (psz_path == NULL)
        return NULL;

    if (vlc_stat(psz_path, &st) == 0)
        return strdup(psz_path);

    char *psz_path_dup = strdup(psz_path);
    if (psz_path_dup == NULL)
        return NULL;

    char *psz_base = strrchr(psz_path_dup, '/');
    char *psz_dir = psz_path_dup;
    if (psz_base != NULL) {
        *psz_base++ = '\0';
        if (*psz_dir == '\0')
            psz_dir = "/";
    } else {
        psz_base = psz_path_dup;
        psz_dir = ".";
    }

    if (*psz_base == '\0') {
        free(psz_path_dup);
        return NULL;
    }

    DIR *p_dir = vlc_opendir(psz_dir);
    if (p_dir == NULL) {
        free(psz_path_dup);
        return NULL;
    }

    char *psz_match_path = NULL;
    unsigned i_matches = 0;
    const char *psz_entry;

    while ((psz_entry = vlc_readdir(p_dir)) != NULL) {
        if (strcmp(psz_entry, psz_base) == 0)
            continue;
        if (strcasecmp(psz_entry, psz_base) != 0)
            continue;

        char *psz_candidate = NULL;
        if (!strcmp(psz_dir, "/")) {
            if (asprintf(&psz_candidate, "/%s", psz_entry) < 0)
                psz_candidate = NULL;
        } else if (asprintf(&psz_candidate, "%s/%s", psz_dir, psz_entry) < 0) {
            psz_candidate = NULL;
        }

        if (psz_candidate == NULL)
            continue;

        if (vlc_stat(psz_candidate, &st) == 0 &&
            (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode))) {
            i_matches++;
            if (i_matches == 1) {
                psz_match_path = psz_candidate;
                continue;
            }
        }

        free(psz_candidate);
    }

    closedir(p_dir);
    free(psz_path_dup);

    if (i_matches == 1) {
        msg_Warn(p_demux,
                 "%s resolved case-mismatched local path requested=%s resolved=%s",
                 OPEN3DBLURAY_LOG_PREFIX, psz_path, psz_match_path);
        return psz_match_path;
    }

    if (i_matches > 1) {
        msg_Warn(p_demux,
                 "%s ambiguous case-mismatched local path requested=%s matches=%u",
                 OPEN3DBLURAY_LOG_PREFIX, psz_path, i_matches);
    }

    free(psz_match_path);
    return NULL;
}

static void blurayReleaseVout(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->p_vout != NULL) {
        if (var_Type(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_MENU_OPEN_VAR) == 0)
            var_Create(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_MENU_OPEN_VAR, VLC_VAR_BOOL);
        var_SetBool(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_MENU_OPEN_VAR, false);
        if (var_Type(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_FORCE_MONO_MENU_VAR) == 0)
            var_Create(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_FORCE_MONO_MENU_VAR,
                       VLC_VAR_BOOL);
        var_SetBool(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_FORCE_MONO_MENU_VAR, false);
        /*
         * The bridge objects outlive any individual vout instance. Clear the
         * wake target before detaching from the old vout so later overlay
         * updates cannot signal a destroyed prepare condition.
         */
        Open3DDirectBridgeNotifyAttachToObject(VLC_OBJECT(p_sys->p_vout), NULL, NULL);
        Open3DSubtitleBridgeSetNotifyCond(&p_sys->subtitle_bridge, NULL, NULL);
        Open3DInteractiveGraphicsBridgeSetNotifyCond(&p_sys->interactive_graphics_bridge,
                                                     NULL, NULL);
        Open3DSubtitleBridgeDetachFromObject(VLC_OBJECT(p_sys->p_vout));
        Open3DInteractiveGraphicsBridgeDetachFromObject(VLC_OBJECT(p_sys->p_vout));
        var_DelCallback(p_sys->p_vout, "mouse-moved", onMouseEvent, p_demux);
        var_DelCallback(p_sys->p_vout, "mouse-clicked", onMouseEvent, p_demux);

        for (int i = 0; i < MAX_OVERLAY; i++) {
            bluray_overlay_t *p_ov = p_sys->p_overlays[i];
            if (p_ov) {
                vlc_mutex_lock(&p_ov->lock);
                if (p_ov->i_channel != -1) {
                    msg_Err(p_demux, "blurayReleaseVout: subpicture channel exists\n");
                    vout_FlushSubpictureChannel(p_sys->p_vout, p_ov->i_channel);
                }
                p_ov->i_channel = -1;
                p_ov->status = ToDisplay;
                vlc_mutex_unlock(&p_ov->lock);

                if (p_ov->p_updater) {
                    unref_subpicture_updater(p_ov->p_updater);
                    p_ov->p_updater = NULL;
                }
            }
        }

        vlc_object_release(p_sys->p_vout);
        p_sys->p_vout = NULL;
    }
}

static void blurayClearOverlay(demux_t *p_demux, int plane);
static bool open3dblurayStillImageActive(demux_t *p_demux);

static bool open3dblurayInteractiveGraphicsOverlayHasRegions(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux != NULL ? p_demux->p_sys : NULL;

    if (p_sys == NULL)
        return false;

    bool has_regions = false;
    vlc_mutex_lock(&p_sys->bdj_overlay_lock);
    bluray_overlay_t *ov = p_sys->p_overlays[BD_OVERLAY_IG];
    if (ov != NULL)
    {
        vlc_mutex_lock(&ov->lock);
        has_regions = ov->p_regions != NULL;
        vlc_mutex_unlock(&ov->lock);
    }
    vlc_mutex_unlock(&p_sys->bdj_overlay_lock);
    return has_regions;
}

static bool open3dblurayInteractiveGraphicsOverlayPresent(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux != NULL ? p_demux->p_sys : NULL;

    if (p_sys == NULL)
        return false;

    bool present = false;
    vlc_mutex_lock(&p_sys->bdj_overlay_lock);
    bluray_overlay_t *ov = p_sys->p_overlays[BD_OVERLAY_IG];
    if (ov != NULL)
    {
        vlc_mutex_lock(&ov->lock);
        present = ov->width > 0 && ov->height > 0;
        vlc_mutex_unlock(&ov->lock);
    }
    vlc_mutex_unlock(&p_sys->bdj_overlay_lock);
    return present;
}

static bool open3dblurayShouldPreserveMenuStateOnPlaylistTransition(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux != NULL ? p_demux->p_sys : NULL;

    if (p_sys == NULL || !p_sys->b_menu_open)
        return false;

    if (open3dblurayStillImageActive(p_demux))
        return true;

    if (open3dblurayInteractiveGraphicsOverlayHasRegions(p_demux))
        return true;

    if (open3dblurayInteractiveGraphicsOverlayPresent(p_demux))
        return true;

    return false;
}

static void open3dblurayPublishMenuOpenState(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_demux->p_input != NULL) {
        if (var_Type(p_demux->p_input, OPEN3D_BLURAY_MENU_OPEN_VAR) == 0)
            var_Create(p_demux->p_input, OPEN3D_BLURAY_MENU_OPEN_VAR, VLC_VAR_BOOL);
        var_SetBool(p_demux->p_input, OPEN3D_BLURAY_MENU_OPEN_VAR, p_sys->b_menu_open);
    }
    if (p_sys->p_vout != NULL) {
        if (var_Type(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_MENU_OPEN_VAR) == 0)
            var_Create(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_MENU_OPEN_VAR,
                       VLC_VAR_BOOL);
        var_SetBool(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_MENU_OPEN_VAR,
                    p_sys->b_menu_open);
    }
}

static bool open3dblurayForceMonoMenuActive(const demux_sys_t *p_sys)
{
#if OPEN3DBLURAY_ENABLE_MVC
    /* Mono menu fallback must be the default menu-open policy for Objective B,
     * not just a side effect of the stock-base MVC handoff. Some BD-J menus can
     * open without ever toggling the synthetic mvc1 <-> stock-base switch, and
     * those runs were therefore staying on the packed-stereo presenter lane
     * (`mono_menu=0`) even in the nominal 2D menu mode. Treat any active
     * menu-open lane as mono-menu eligible; ONE_PLANE and TWO_PLANES remain
     * bounded exceptions on the vout side and only keep stereo when the live IG
     * bridge advertises a matching payload. */
    return p_sys != NULL &&
           (p_sys->b_mvc_base_fallback_selected || p_sys->b_menu_open);
#else
    VLC_UNUSED(p_sys);
    return false;
#endif
}

static void open3dblurayPublishForceMonoMenuState(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool b_force_mono = open3dblurayForceMonoMenuActive(p_sys);

    if (p_demux->p_input != NULL) {
        if (var_Type(p_demux->p_input, OPEN3D_BLURAY_FORCE_MONO_MENU_VAR) == 0)
            var_Create(p_demux->p_input, OPEN3D_BLURAY_FORCE_MONO_MENU_VAR, VLC_VAR_BOOL);
        var_SetBool(p_demux->p_input, OPEN3D_BLURAY_FORCE_MONO_MENU_VAR, b_force_mono);
    }
    if (p_sys->p_vout != NULL) {
        if (var_Type(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_FORCE_MONO_MENU_VAR) == 0)
            var_Create(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_FORCE_MONO_MENU_VAR,
                       VLC_VAR_BOOL);
        var_SetBool(VLC_OBJECT(p_sys->p_vout), OPEN3D_BLURAY_FORCE_MONO_MENU_VAR,
                    b_force_mono);
    }
}

static const char *open3dblurayInteractiveGraphicsS3DModeName(int mode)
{
    switch (mode)
    {
        case BLURAY_IG_S3D_MODE_TWOD_OUTPUT:
            return "TWOD_OUTPUT";
        case BLURAY_IG_S3D_MODE_ONE_PLANE:
            return "ONE_PLANE";
        case BLURAY_IG_S3D_MODE_TWO_PLANES:
            return "TWO_PLANES";
        default:
            return "UNKNOWN";
    }
}

static bool open3dblurayParseForcedInteractiveGraphicsS3DMode(int *mode_out)
{
    const char *env = getenv("OPEN3DBLURAY_FORCE_IG_S3D_MODE");
    char *end = NULL;
    long parsed = 0;

    if (mode_out != NULL)
        *mode_out = 0;
    if (env == NULL || *env == '\0')
        return false;

    if (strcasecmp(env, "TWOD_OUTPUT") == 0 || strcmp(env, "1") == 0)
        parsed = BLURAY_IG_S3D_MODE_TWOD_OUTPUT;
    else if (strcasecmp(env, "ONE_PLANE") == 0 || strcmp(env, "2") == 0)
        parsed = BLURAY_IG_S3D_MODE_ONE_PLANE;
    else if (strcasecmp(env, "TWO_PLANES") == 0 || strcmp(env, "3") == 0)
        parsed = BLURAY_IG_S3D_MODE_TWO_PLANES;
    else
    {
        parsed = strtol(env, &end, 10);
        if (end == env || *end != '\0')
            return false;
    }

    if (parsed < BLURAY_IG_S3D_MODE_TWOD_OUTPUT ||
        parsed > BLURAY_IG_S3D_MODE_TWO_PLANES)
        return false;

    if (mode_out != NULL)
        *mode_out = (int)parsed;
    return true;
}

static bool open3dblurayParseForcedInteractiveGraphicsS3DOffset(int *offset_out)
{
    const char *env = getenv("OPEN3DBLURAY_FORCE_IG_S3D_OFFSET");
    char *end = NULL;
    long parsed = 0;

    if (offset_out != NULL)
        *offset_out = 0;
    if (env == NULL || *env == '\0')
        return false;

    parsed = strtol(env, &end, 10);
    if (end == env || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX)
        return false;

    if (offset_out != NULL)
        *offset_out = (int)parsed;
    return true;
}

static void open3dblurayReadInteractiveGraphicsS3DState(
    demux_t *p_demux, open3d_interactive_graphics_s3d_state_t *state,
    const char *reason)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool trace = open3dblurayTraceInteractiveGraphicsBridgeEnabled();
    bool forced_mode_valid = false;
    bool forced_offset_valid = false;
    int forced_mode = 0;
    int forced_offset = 0;

    Open3DInteractiveGraphicsStereoStateClear(state);
    if (p_sys == NULL || p_sys->bluray == NULL || state == NULL)
        return;

    BLURAY_IG_S3D_STATE bluray_state;
    if (bd_get_ig_s3d_state(p_sys->bluray, &bluray_state))
    {
        state->mode_valid = bluray_state.mode_valid != 0;
        state->mode = bluray_state.mode;
        state->offset_valid = bluray_state.offset_valid != 0;
        state->offset = bluray_state.offset;
        state->epoch = bluray_state.epoch;
    }

    forced_mode_valid = open3dblurayParseForcedInteractiveGraphicsS3DMode(&forced_mode);
    forced_offset_valid = open3dblurayParseForcedInteractiveGraphicsS3DOffset(&forced_offset);
    if (forced_mode_valid)
    {
        state->mode_valid = true;
        state->mode = forced_mode;
    }
    if (forced_offset_valid)
    {
        state->offset_valid = true;
        state->offset = forced_offset;
    }

    if (trace)
    {
        if (forced_mode_valid || forced_offset_valid)
            msg_Dbg(p_demux,
                    "open3dbluray trace ig-s3d override reason=%s mode_valid=%d mode=%d(%s) offset_valid=%d offset=%d",
                    reason != NULL ? reason : "unspecified",
                    forced_mode_valid ? 1 : 0,
                    forced_mode_valid ? forced_mode : state->mode,
                    open3dblurayInteractiveGraphicsS3DModeName(forced_mode_valid ? forced_mode
                                                                                 : state->mode),
                    forced_offset_valid ? 1 : 0,
                    forced_offset_valid ? forced_offset : state->offset);
        msg_Dbg(p_demux,
                "open3dbluray trace ig-s3d read reason=%s epoch=%" PRIu64 " mode_valid=%d mode=%d(%s) offset_valid=%d offset=%d",
                reason != NULL ? reason : "unspecified",
                state->epoch,
                state->mode_valid ? 1 : 0,
                state->mode,
                open3dblurayInteractiveGraphicsS3DModeName(state->mode),
                state->offset_valid ? 1 : 0,
                state->offset);
    }
}

static void open3dblurayRefreshInteractiveGraphicsS3DBridgeState(
    demux_t *p_demux, const char *reason)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    open3d_interactive_graphics_s3d_state_t state;

    if (p_sys == NULL)
        return;

    open3dblurayReadInteractiveGraphicsS3DState(p_demux, &state, reason);
    Open3DInteractiveGraphicsBridgeSetStereoState(
        &p_sys->interactive_graphics_bridge, &state);
}

static void open3dblurayPublishPopupAvailableState(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_demux->p_input != NULL) {
        if (var_Type(p_demux->p_input, "menu-popup-available") == 0)
            var_Create(p_demux->p_input, "menu-popup-available", VLC_VAR_BOOL);
        var_SetBool(p_demux->p_input, "menu-popup-available",
                    p_sys->b_popup_available);
    }
}

static void open3dblurayClearActiveMenuState(demux_t *p_demux, const char *reason)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool had_menu_open = p_sys->b_menu_open;
    const bool had_popup_available = p_sys->b_popup_available;

    if (!had_menu_open && !had_popup_available)
        return;

    p_sys->b_menu_open = false;
    p_sys->b_popup_available = false;
    open3dblurayPublishMenuOpenState(p_demux);
    open3dblurayPublishForceMonoMenuState(p_demux);
    open3dblurayPublishPopupAvailableState(p_demux);

    if (p_sys->p_overlays[BD_OVERLAY_IG] != NULL)
        blurayClearOverlay(p_demux, BD_OVERLAY_IG);
    else
        Open3DInteractiveGraphicsBridgeClear(&p_sys->interactive_graphics_bridge);
    Open3DInteractiveGraphicsBridgeSetStereoState(&p_sys->interactive_graphics_bridge,
                                                  NULL);

    msg_Info(p_demux,
             "menu_state event=feature-playback-clear reason=%s before_open=%d before_popup=%d",
             reason != NULL ? reason : "unspecified",
             had_menu_open ? 1 : 0,
             had_popup_available ? 1 : 0);
}

static void open3dblurayRefreshTrackedVoutState(demux_t *p_demux,
                                                const char *reason)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool trace = open3dblurayTraceInteractiveGraphicsBridgeEnabled();

    if (p_sys->p_vout == NULL)
        return;

    /*
     * Replacement/reused vouts can retain the display-side "bridge disabled"
     * bit from the previous instance's Close() path. Object-tree lookups stop
     * at the first enable var they encounter, so a stale false on the vout
     * masks the input-level true bit and makes menu-open IG fall back to the
     * non-direct lane even though the bridge object itself was reattached.
     */
    Open3DSubtitleBridgeSetEnabledOnObject(VLC_OBJECT(p_sys->p_vout), true);
    Open3DInteractiveGraphicsBridgeSetEnabledOnObject(
        VLC_OBJECT(p_sys->p_vout), true);
    open3dblurayPublishMenuOpenState(p_demux);
    open3dblurayPublishForceMonoMenuState(p_demux);
    open3dblurayRefreshInteractiveGraphicsS3DBridgeState(
        p_demux, reason != NULL ? reason : "vout-refresh");
    if (trace)
        msg_Dbg(p_demux,
                "open3dbluray trace ig-bridge vout-refresh reason=%s vout=%p",
                reason != NULL ? reason : "unspecified",
                (void *)p_sys->p_vout);
}

static void open3dblurayAdoptAcquiredVout(demux_t *p_demux,
                                          vout_thread_t *current_vout,
                                          const char *reason)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool trace = open3dblurayTraceInteractiveGraphicsBridgeEnabled();

    if (current_vout == NULL)
        return;

    if (p_sys->p_vout == current_vout)
    {
        open3dblurayRefreshTrackedVoutState(
            p_demux, reason != NULL ? reason : "vout-acquire");
        vlc_object_release(current_vout);
        return;
    }

    if (p_sys->p_vout != NULL)
    {
        if (trace)
            msg_Dbg(p_demux,
                    "open3dbluray trace ig-bridge vout-rebind reason=%s old=%p new=%p",
                    reason != NULL ? reason : "unspecified",
                    (void *)p_sys->p_vout,
                    (void *)current_vout);
        blurayReleaseVout(p_demux);
    }

    p_sys->p_vout = current_vout;
    open3dblurayRefreshTrackedVoutState(
        p_demux, reason != NULL ? reason : "vout-acquire");
    if (trace)
        msg_Dbg(p_demux,
                "open3dbluray trace ig-bridge vout-acquire reason=%s vout=%p",
                reason != NULL ? reason : "unspecified",
                (void *)p_sys->p_vout);
    var_AddCallback(p_sys->p_vout, "mouse-moved", onMouseEvent, p_demux);
    var_AddCallback(p_sys->p_vout, "mouse-clicked", onMouseEvent, p_demux);
}

static void open3dblurayEnsureVout(demux_t *p_demux)
{
    vout_thread_t *current_vout = input_GetVout(p_demux->p_input);

    open3dblurayAdoptAcquiredVout(p_demux, current_vout, "vout-acquire");
}

static void open3dblurayAttachDirectBridgesToInput(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    vlc_object_t *input_obj = VLC_OBJECT(p_demux->p_input);

    if (input_obj == NULL)
        return;

    Open3DSubtitleBridgeAttachToObject(input_obj, &p_sys->subtitle_bridge);
    Open3DInteractiveGraphicsBridgeAttachToObject(
        input_obj, &p_sys->interactive_graphics_bridge);
    Open3DSubtitleBridgeSetEnabledOnObject(input_obj, true);
    Open3DInteractiveGraphicsBridgeSetEnabledOnObject(input_obj, true);
}

static void open3dblurayAttachDirectBridgesToVout(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool trace = open3dblurayTraceInteractiveGraphicsBridgeEnabled();
    open3d_interactive_graphics_bridge_t *ig_root_before = NULL;
    open3d_interactive_graphics_bridge_t *ig_root_after = NULL;
    vlc_object_t *input_obj = VLC_OBJECT(p_demux->p_input);

    open3dblurayAttachDirectBridgesToInput(p_demux);
    if (input_obj != NULL)
        ig_root_before = Open3DInteractiveGraphicsBridgeGetFromObject(input_obj);

    open3dblurayEnsureVout(p_demux);
    if (p_sys->p_vout == NULL)
        return;

    open3d_interactive_graphics_bridge_t *ig_before =
        Open3DInteractiveGraphicsBridgeGetFromObject(VLC_OBJECT(p_sys->p_vout));
    const bool ig_enabled_before =
        Open3DInteractiveGraphicsBridgeGetEnabledFromObject(VLC_OBJECT(p_sys->p_vout));

    Open3DSubtitleBridgeAttachToObject(VLC_OBJECT(p_sys->p_vout),
                                       &p_sys->subtitle_bridge);
    Open3DInteractiveGraphicsBridgeAttachToObject(VLC_OBJECT(p_sys->p_vout),
                                                  &p_sys->interactive_graphics_bridge);
    Open3DSubtitleBridgeSetEnabledOnObject(VLC_OBJECT(p_sys->p_vout), true);
    Open3DInteractiveGraphicsBridgeSetEnabledOnObject(
        VLC_OBJECT(p_sys->p_vout), true);
    Open3DSubtitleBridgeSetNotifyCond(&p_sys->subtitle_bridge,
                                      Open3DDirectBridgeNotifyCondFromObject(
                                          VLC_OBJECT(p_sys->p_vout)),
                                      Open3DDirectBridgeNotifyPendingFromObject(
                                          VLC_OBJECT(p_sys->p_vout)));
    Open3DInteractiveGraphicsBridgeSetNotifyCond(
        &p_sys->interactive_graphics_bridge,
        Open3DDirectBridgeNotifyCondFromObject(VLC_OBJECT(p_sys->p_vout)),
        Open3DDirectBridgeNotifyPendingFromObject(VLC_OBJECT(p_sys->p_vout)));
    open3dblurayRefreshInteractiveGraphicsS3DBridgeState(p_demux, "bridge-attach");
    Open3DDirectBridgeNotifyWake(VLC_OBJECT(p_sys->p_vout));
    if (input_obj != NULL)
        ig_root_after = Open3DInteractiveGraphicsBridgeGetFromObject(input_obj);
    if (trace)
    {
        open3d_interactive_graphics_bridge_t *ig_after =
            Open3DInteractiveGraphicsBridgeGetFromObject(VLC_OBJECT(p_sys->p_vout));
        const bool ig_enabled_after =
            Open3DInteractiveGraphicsBridgeGetEnabledFromObject(VLC_OBJECT(p_sys->p_vout));
        msg_Dbg(p_demux,
                "open3dbluray trace ig-bridge attach input=%p input_before=%p input_after=%p vout=%p before=%p enabled_before=%d after=%p enabled_after=%d",
                (void *)p_demux->p_input,
                (void *)ig_root_before,
                (void *)ig_root_after,
                (void *)p_sys->p_vout,
                (void *)ig_before,
                ig_enabled_before ? 1 : 0,
                (void *)ig_after,
                ig_enabled_after ? 1 : 0);
    }
}

static bool open3dblurayUseDirectSubtitleBridge(demux_t *p_demux,
                                                const bluray_overlay_t *p_ov)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->p_vout == NULL || p_ov == NULL || p_ov->plane != BD_OVERLAY_PG)
        return false;

    return Open3DSubtitleBridgeGetEnabledFromObjectTree(VLC_OBJECT(p_sys->p_vout));
}

static bool open3dblurayUseDirectInteractiveGraphicsBridge(demux_t *p_demux,
                                                           const bluray_overlay_t *p_ov)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->p_vout == NULL || p_ov == NULL || p_ov->plane != BD_OVERLAY_IG)
        return false;

    return Open3DInteractiveGraphicsBridgeGetEnabledFromObjectTree(
        VLC_OBJECT(p_sys->p_vout));
}

static bool open3dblurayUseDirectOverlayBridge(demux_t *p_demux,
                                               const bluray_overlay_t *p_ov)
{
    if (p_ov == NULL)
        return false;

    if (p_ov->plane == BD_OVERLAY_PG)
        return open3dblurayUseDirectSubtitleBridge(p_demux, p_ov);
    if (p_ov->plane == BD_OVERLAY_IG)
        return open3dblurayUseDirectInteractiveGraphicsBridge(p_demux, p_ov);
    return false;
}

static void open3dblurayDetachOverlaySubpictureFromVout(demux_t *p_demux,
                                                        bluray_overlay_t *p_ov)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_ov == NULL)
        return;

    vlc_mutex_lock(&p_ov->lock);
    if (p_sys->p_vout != NULL && p_ov->i_channel != -1)
        vout_FlushSubpictureChannel(p_sys->p_vout, p_ov->i_channel);
    p_ov->i_channel = -1;
    vlc_mutex_unlock(&p_ov->lock);

    if (p_ov->p_updater != NULL)
    {
        unref_subpicture_updater(p_ov->p_updater);
        p_ov->p_updater = NULL;
    }
}

static void open3dbluraySyncSubtitleBridgeFromOverlayLocked(demux_t *p_demux,
                                                            const bluray_overlay_t *p_ov)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_ov == NULL || p_ov->plane != BD_OVERLAY_PG)
    {
        Open3DSubtitleBridgeClear(&p_sys->subtitle_bridge);
        return;
    }

    const vlc_tick_t pts = p_ov->pts90k > 0 ? FROM_SCALE(p_ov->pts90k)
                                            : VLC_TICK_INVALID;
    open3d_subtitle_depth_state_t depth_state;
    Open3DSubtitleDepthStateSet(&depth_state,
                                p_ov->b_subtitle_offset_valid,
                                p_ov->i_subtitle_offset_sequence,
                                p_ov->i_subtitle_offset_raw,
                                p_ov->i_subtitle_offset_signed,
                                p_ov->i_subtitle_offset_frame);
    if (Open3DSubtitleBridgeUpdate(&p_sys->subtitle_bridge,
                                   p_ov->width, p_ov->height,
                                   p_ov->p_regions, pts,
                                   &depth_state) != VLC_SUCCESS)
        msg_Err(p_demux, "failed to update direct Blu-ray subtitle bridge");
}

static void open3dbluraySyncInteractiveGraphicsBridgeFromOverlayLocked(
    demux_t *p_demux, const bluray_overlay_t *p_ov)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool trace = open3dblurayTraceInteractiveGraphicsBridgeEnabled();
    open3d_interactive_graphics_s3d_state_t s3d_state;

    if (p_ov == NULL || p_ov->plane != BD_OVERLAY_IG)
    {
        Open3DInteractiveGraphicsBridgeClear(&p_sys->interactive_graphics_bridge);
        Open3DInteractiveGraphicsBridgeSetStereoState(&p_sys->interactive_graphics_bridge,
                                                      NULL);
        return;
    }

    const vlc_tick_t pts = p_ov->pts90k > 0 ? FROM_SCALE(p_ov->pts90k)
                                            : mdate();
    const unsigned region_count = open3dblurayCountRegionChain(p_ov->p_regions);
    open3dblurayReadInteractiveGraphicsS3DState(p_demux, &s3d_state,
                                                "bridge-update");
    if (Open3DInteractiveGraphicsBridgeUpdate(&p_sys->interactive_graphics_bridge,
                                              p_ov->width, p_ov->height,
                                              p_ov->p_regions, pts,
                                              &s3d_state) != VLC_SUCCESS)
    {
        msg_Err(p_demux, "failed to update direct Blu-ray interactive graphics bridge");
    }
    else if (trace)
    {
        open3dblurayTraceIgRegionChain(p_demux, "bridge-update", p_ov,
                                       open3dblurayUseDirectInteractiveGraphicsBridge(p_demux, p_ov),
                                       p_sys->b_menu_open);
        msg_Dbg(p_demux,
                "open3dbluray trace ig-bridge update plane=%u payload=%s regions=%u size=%ux%u pts=%" PRId64 " s3d_epoch=%" PRIu64 " mode_valid=%d mode=%d(%s) offset_valid=%d offset=%d",
                (unsigned)p_ov->plane,
                Open3DInteractiveGraphicsPayloadKindName(OPEN3D_IG_PAYLOAD_MONO),
                region_count,
                p_ov->width,
                p_ov->height,
                pts,
                s3d_state.epoch,
                s3d_state.mode_valid ? 1 : 0,
                s3d_state.mode,
                open3dblurayInteractiveGraphicsS3DModeName(s3d_state.mode),
                s3d_state.offset_valid ? 1 : 0,
                s3d_state.offset);
    }
}

static void open3dbluraySyncDirectOverlayBridgeFromOverlayLocked(demux_t *p_demux,
                                                                 const bluray_overlay_t *p_ov)
{
    if (p_ov == NULL)
        return;

    open3dblurayTraceOverlayCoverage(p_demux, "bridge-update", p_ov,
                                     open3dblurayUseDirectOverlayBridge(p_demux, p_ov),
                                     p_demux->p_sys->b_menu_open);

    if (p_ov->plane == BD_OVERLAY_PG)
        open3dbluraySyncSubtitleBridgeFromOverlayLocked(p_demux, p_ov);
    else if (p_ov->plane == BD_OVERLAY_IG)
        open3dbluraySyncInteractiveGraphicsBridgeFromOverlayLocked(p_demux, p_ov);
}

static void open3dblurayPublishSubtitleOffsetToVout(demux_t *p_demux,
                                                    const bluray_overlay_t *p_ov)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    open3dblurayEnsureVout(p_demux);
    vout_thread_t *p_vout = p_sys->p_vout;
    open3d_subtitle_depth_state_t depth_state;

    if (!p_vout)
        return;

    Open3DSubtitleDepthStateClear(&depth_state);

    if (p_ov && p_ov->b_subtitle_offset_valid) {
        Open3DSubtitleDepthStateSet(&depth_state,
                                    true,
                                    p_ov->i_subtitle_offset_sequence,
                                    p_ov->i_subtitle_offset_raw,
                                    p_ov->i_subtitle_offset_signed,
                                    p_ov->i_subtitle_offset_frame);
    } else {
        bool b_valid = false;
        int i_signed = 0;
        int i_raw = 0;
        int i_frame = -1;
        int i_seq = p_sys->b_selected_pg_offset_sequence_valid
                  ? p_sys->i_selected_pg_offset_sequence_id : -1;

        if (p_sys->bluray != NULL && p_sys->b_selected_pg_offset_sequence_valid) {
            const int64_t i_pts = (int64_t)bd_tell_time(p_sys->bluray);
            BD_OPEN3D_PG_OFFSET offset;
            if (bd_open3d_mvc_get_pg_offset(p_sys->bluray,
                                           p_sys->i_selected_pg_offset_sequence_id,
                                           i_pts, &offset)) {
                b_valid = true;
                i_signed = offset.signed_offset;
                i_raw = offset.raw_offset;
                i_frame = offset.frame_index;
                i_seq = offset.offset_sequence_id;
            }
        }
        Open3DSubtitleDepthStateSet(&depth_state,
                                    b_valid,
                                    b_valid ? (uint8_t)i_seq : 0xff,
                                    (uint8_t)i_raw,
                                    (int8_t)i_signed,
                                    i_frame);
    }

    Open3DSubtitleDepthPublishToObject(VLC_OBJECT(p_vout), &depth_state);
}

/*****************************************************************************
 * BD-J background video
 *****************************************************************************/

static es_out_id_t * blurayCreateBackgroundUnlocked(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    vlc_tick_t i_dummy_pts = VLC_TICK_INVALID;
    static const unsigned i_dummy_width = 1920;
    static const unsigned i_dummy_height = 1080;
    static const vlc_tick_t i_dummy_frame_duration = CLOCK_FREQ / 25;
    const size_t i_luma_size = (size_t)i_dummy_width * i_dummy_height;
    const size_t i_chroma_size = i_luma_size / 4;
    const size_t i_dummy_size = i_luma_size + 2 * i_chroma_size;

    if (p_sys->p_dummy_video)
        return p_sys->p_dummy_video;

    msg_Info(p_demux, "Start background");
    if (open3dblurayTraceMenuVisualsEnabled())
        msg_Warn(p_demux,
                 "open3dbluray trace background-create phase=start menu=%d menu_open=%d vout=%d dummy=%d",
                 p_sys->b_menu ? 1 : 0,
                 p_sys->b_menu_open ? 1 : 0,
                 p_sys->p_vout != NULL ? 1 : 0,
                 p_sys->p_dummy_video != NULL ? 1 : 0);

    /* Keep the fallback on a decoder path that does not depend on avcodec
       hardware probing. A single raw I420 black frame is enough to stand up
       a vout for menu-open IG overlays on BD-J still-image lanes when the
       packaged runtime includes the rawvideo decoder. */
    es_format_t fmt;
    es_format_Init( &fmt, VIDEO_ES, VLC_CODEC_I420 );
    fmt.b_packetized = true;
    video_format_Setup( &fmt.video, VLC_CODEC_I420,
                        i_dummy_width, i_dummy_height,
                        i_dummy_width, i_dummy_height, 1, 1);
    /*
     * Keep the synthetic stream metadata aligned with the timeline we feed to
     * the decoder. Advertising 1 fps while sending 25 fps timestamps makes the
     * menu fallback lane much harder to reason about and can stretch failures
     * into apparent multi-second/multi-minute stalls.
     */
    fmt.video.i_frame_rate = 25;
    fmt.video.i_frame_rate_base = 1;
    fmt.i_priority = ES_PRIORITY_SELECTABLE_MIN;
    fmt.i_id = 4115; /* 4113 = main video. 4114 = MVC. 4115 = unused. */
    fmt.i_group = 1;

    p_sys->p_dummy_video = es_out_Add(p_demux->out, &fmt);

    if (!p_sys->p_dummy_video) {
        msg_Err(p_demux, "Error adding background ES");
        goto out;
    }

    block_t *p_block = block_Alloc(i_dummy_size);
    if (!p_block) {
        msg_Err(p_demux, "Error allocating block for background video");
        es_out_Del(p_demux->out, p_sys->p_dummy_video);
        p_sys->p_dummy_video = NULL;
        goto out;
    }

    /*
     * The dummy background frame exists only to stand up a usable vout for
     * menu/still-image lanes. Keep it on a tiny synthetic media timeline and
     * publish a matching PCR so rawvideo does not start against a missing,
     * zero, or wall-clock-based reference.
     */
    /*
     * Each synthetic dummy ES stands up a fresh rawvideo decoder/vout lane.
     * Rebase that lane onto a tiny local media timeline every time we create
     * it, instead of carrying forward an arbitrary stale tick across prior
     * PCR resets or playlist/menu hops.
     */
    p_sys->i_dummy_video_next_pts = i_dummy_frame_duration;
    p_block->i_dts = p_block->i_pts = p_sys->i_dummy_video_next_pts;
    p_block->i_length = i_dummy_frame_duration;
    p_block->i_flags |= BLOCK_FLAG_DISCONTINUITY | BLOCK_FLAG_TYPE_I;
    memset(p_block->p_buffer, 0x00, i_luma_size);
    memset(p_block->p_buffer + i_luma_size, 0x80, 2 * i_chroma_size);
    p_block->i_buffer = i_dummy_size;
    i_dummy_pts = p_block->i_pts;
    es_out_SetPCR(p_demux->out, i_dummy_pts);
    es_out_Control(p_demux->out, ES_OUT_SET_ES, p_sys->p_dummy_video);
    es_out_Send(p_demux->out, p_sys->p_dummy_video, p_block);
    p_sys->i_dummy_video_next_pts += i_dummy_frame_duration;
    if (open3dblurayTraceMenuVisualsEnabled())
        msg_Warn(p_demux,
                 "open3dbluray trace background-create phase=send es=%d size=%ux%u pts=%" PRId64 " pcr=%" PRId64 " dur=%" PRId64,
                 p_sys->p_dummy_video != NULL ? 1 : 0,
                 fmt.video.i_width,
                 fmt.video.i_height,
                 i_dummy_pts,
                 i_dummy_pts,
                 i_dummy_frame_duration);

 out:
    es_format_Clean(&fmt);
    return p_sys->p_dummy_video;
}

static void stopBackground(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (!p_sys->p_dummy_video)
        return;

    msg_Info(p_demux, "Stop background");
    if (open3dblurayTraceMenuVisualsEnabled())
        msg_Warn(p_demux,
                 "open3dbluray trace background-create phase=stop menu=%d menu_open=%d vout=%d dummy=%d",
                 p_sys->b_menu ? 1 : 0,
                 p_sys->b_menu_open ? 1 : 0,
                 p_sys->p_vout != NULL ? 1 : 0,
                 p_sys->p_dummy_video != NULL ? 1 : 0);

    es_out_Del(p_demux->out, p_sys->p_dummy_video);
    p_sys->p_dummy_video = NULL;
}

static bool open3dblurayCurrentClipHasUsablePrimaryVideo(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux != NULL ? p_demux->p_sys : NULL;

    if (p_sys == NULL)
        return false;

    bool b_has_video = false;

    vlc_mutex_lock(&p_sys->pl_info_lock);
    const BLURAY_CLIP_INFO *p_clip = p_sys->p_clip_info;
    if (p_clip != NULL && p_clip->video_stream_count > 0) {
        const BLURAY_STREAM_INFO *p_video0 = &p_clip->video_streams[0];
        switch (p_video0->coding_type) {
        case BLURAY_STREAM_TYPE_VIDEO_MPEG1:
        case BLURAY_STREAM_TYPE_VIDEO_MPEG2:
        case BLURAY_STREAM_TYPE_VIDEO_VC1:
        case BLURAY_STREAM_TYPE_VIDEO_H264:
        case BD_STREAM_TYPE_VIDEO_HEVC:
            b_has_video = true;
            break;
        default:
            break;
        }
    }
    vlc_mutex_unlock(&p_sys->pl_info_lock);

    return b_has_video;
}

static bool open3dblurayShouldHoldBackground(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool b_clip_has_primary_video =
        open3dblurayCurrentClipHasUsablePrimaryVideo(p_demux);

    return p_sys->p_dummy_video != NULL &&
           p_sys->b_menu_open &&
           !b_clip_has_primary_video &&
           (p_sys->p_vout == NULL || open3dblurayStillImageActive(p_demux));
}

#if OPEN3DBLURAY_ENABLE_MVC
static vlc_tick_t open3dblurayTs90kToTick(int64_t ts90k)
{
    return ts90k > 0 ? FROM_SCALE(ts90k) : VLC_TICK_INVALID;
}

static bool open3dblurayUseDirectMvcTiming(demux_t *p_demux)
{
    return var_InheritBool(p_demux, "open3dbluraymvc-direct-mvc-timing");
}

static bool open3dblurayUseSeekTimingTrace(demux_t *p_demux)
{
    return var_InheritBool(p_demux, "open3dbluraymvc-seek-timing-trace");
}

static void open3dblurayStartSeekTimingTrace(demux_t *p_demux, const char *psz_reason)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (!open3dblurayUseSeekTimingTrace(p_demux))
        return;

    p_sys->b_seek_timing_trace_active = true;
    p_sys->i_seek_timing_trace_epoch++;
    p_sys->b_seek_timing_trace_saw_stock_pcr = false;
    p_sys->b_seek_timing_trace_saw_audio = false;
    p_sys->b_seek_timing_trace_saw_mvc_unit = false;
    p_sys->b_seek_timing_trace_saw_mvc_send = false;
    p_sys->b_seek_timing_trace_saw_mvc_matched = false;
    p_sys->i_seek_timing_last_group_pcr = VLC_TICK_INVALID;

    msg_Info(p_demux,
             "%s seek_timing epoch=%u start reason=%s",
             OPEN3DBLURAY_LOG_PREFIX,
             p_sys->i_seek_timing_trace_epoch,
             psz_reason ? psz_reason : "(unknown)");
}

static void open3dblurayTraceSeekTimingResetPcr(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (!p_sys->b_seek_timing_trace_active)
        return;

    msg_Info(p_demux,
             "%s seek_timing epoch=%u reset_pcr",
             OPEN3DBLURAY_LOG_PREFIX,
             p_sys->i_seek_timing_trace_epoch);
}

static void open3dblurayTraceSeekTimingStockPcr(demux_t *p_demux,
                                                int i_group,
                                                vlc_tick_t i_pcr,
                                                vlc_tick_t i_offset_pcr)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    p_sys->i_seek_timing_last_group_pcr = i_pcr;
    if (!p_sys->b_seek_timing_trace_active ||
        p_sys->b_seek_timing_trace_saw_stock_pcr)
        return;

    p_sys->b_seek_timing_trace_saw_stock_pcr = true;
    msg_Info(p_demux,
             "%s seek_timing epoch=%u first_stock_pcr group=%d pcr=%" PRId64 " offset_pcr=%" PRId64,
             OPEN3DBLURAY_LOG_PREFIX,
             p_sys->i_seek_timing_trace_epoch,
             i_group,
             i_pcr,
             i_offset_pcr);
}

static es_out_t *open3dblurayGetMvcSinkOut(demux_t *p_demux)
{
    if (open3dblurayUseDirectMvcTiming(p_demux))
        return p_demux->out;

    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->p_esc_out != NULL)
        return p_sys->p_esc_out;
    if (p_sys->p_tf_out != NULL)
        return p_sys->p_tf_out;
    return p_demux->out;
}

static void open3dblurayLogMvcInfo(demux_t *p_demux, const char *phase)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    BD_OPEN3D_MVC_INFO info;
    int have;

    if (!p_sys || !p_sys->bluray)
        return;

    memset(&info, 0, sizeof(info));
    have = bd_open3d_mvc_get_info(p_sys->bluray, &info);
    msg_Info(p_demux,
             "%s mvc_info phase=%s have=%d available=%u playitem=%d "
             "base=%s dep=%s base_pid=0x%04x dep_pid=0x%04x "
             "subpath_kind=%u subpath_type=%u sync_play_item_id=%d sync_pts=%u",
             OPEN3DBLURAY_LOG_PREFIX,
             phase ? phase : "(null)",
             have,
             info.available,
             info.playitem_index,
             info.base_clip_id,
             info.dependent_clip_id,
             info.base_pid,
             info.dependent_pid,
             info.subpath_kind,
             info.subpath_type,
             info.sync_play_item_id,
             info.sync_pts);
}

static bool open3dblurayTraceMvcGateEnabled(void)
{
    static int s_init = 0;
    static bool s_enabled = false;

    if (!s_init) {
        const char *psz_env = getenv("OPEN3DBLURAY_TRACE_MVC_GATE");
        s_enabled = psz_env != NULL && psz_env[0] != '\0' && strcmp(psz_env, "0");
        s_init = 1;
    }

    return s_enabled;
}

static void open3dblurayGetMvcTraceContext(demux_sys_t *p_sys,
                                           unsigned *pi_playlist,
                                           char psz_clip_id[6])
{
    if (pi_playlist)
        *pi_playlist = 0;
    if (psz_clip_id)
        psz_clip_id[0] = '\0';

    if (p_sys == NULL)
        return;

    vlc_mutex_lock(&p_sys->pl_info_lock);
    if (pi_playlist && p_sys->p_pl_info != NULL)
        *pi_playlist = p_sys->p_pl_info->playlist;
    if (psz_clip_id && p_sys->p_clip_info != NULL) {
        memcpy(psz_clip_id, p_sys->p_clip_info->clip_id, 5);
        psz_clip_id[5] = '\0';
    }
    vlc_mutex_unlock(&p_sys->pl_info_lock);
}

static void open3dblurayTraceMvcGate(demux_t *p_demux,
                                     uint16_t i_pid,
                                     size_t i_bytes)
{
    demux_sys_t *p_sys;
    BD_OPEN3D_MVC_INFO info;
    unsigned i_playlist = 0;
    char sz_clip_id[6];
    uint64_t i_sig = 0;
    int i_have = 0;
    static unsigned s_count = 0;
    static uint64_t s_last_sig = UINT64_MAX;

    if (!open3dblurayTraceMvcGateEnabled() || p_demux == NULL)
        return;

    p_sys = p_demux->p_sys;
    if (p_sys == NULL)
        return;

    memset(&info, 0, sizeof(info));
    if (p_sys->bluray != NULL)
        i_have = bd_open3d_mvc_get_info(p_sys->bluray, &info);
    open3dblurayGetMvcTraceContext(p_sys, &i_playlist, sz_clip_id);

    ++s_count;
    i_sig |= ((uint64_t)i_playlist & 0xffffu) << 0;
    i_sig |= ((uint64_t)(unsigned)i_have & 0xffu) << 16;
    i_sig |= ((uint64_t)info.available & 0xffu) << 24;
    i_sig |= ((uint64_t)(p_sys->b_menu_open ? 1u : 0u)) << 32;
    i_sig |= ((uint64_t)(p_sys->b_mvc_seen_matched ? 1u : 0u)) << 33;
    i_sig |= ((uint64_t)(p_sys->b_mvc_hold_unmatched_until_match ? 1u : 0u)) << 34;
    i_sig |= ((uint64_t)(p_sys->p_mvc_video != NULL ? 1u : 0u)) << 35;
    i_sig |= ((uint64_t)info.base_pid & 0xffffu) << 36;
    i_sig ^= ((uint64_t)info.dependent_pid & 0xffffu) << 52;
    for (size_t i = 0; i < 5 && sz_clip_id[0] != '\0'; ++i)
        i_sig ^= (uint64_t)(uint8_t)sz_clip_id[i] << ((i % 4u) * 8u);

    if (s_count > 8 && (s_count % 256u) != 0u && i_sig == s_last_sig)
        return;

    s_last_sig = i_sig;
    fprintf(stderr,
            "open3dbluray raw mvc-gate count=%u playlist=%u clip_id=%s menu_open=%d "
            "pid=0x%04x bytes=%zu have=%d available=%u base_pid=0x%04x dep_pid=0x%04x "
            "matched=%d hold=%d synthetic_es=%d\n",
            s_count,
            i_playlist,
            sz_clip_id[0] != '\0' ? sz_clip_id : "(none)",
            p_sys->b_menu_open ? 1 : 0,
            i_pid,
            i_bytes,
            i_have,
            info.available,
            info.base_pid,
            info.dependent_pid,
            p_sys->b_mvc_seen_matched ? 1 : 0,
            p_sys->b_mvc_hold_unmatched_until_match ? 1 : 0,
            p_sys->p_mvc_video != NULL ? 1 : 0);
    fflush(stderr);
}

static void open3dblurayTraceMvcUnitSend(demux_t *p_demux,
                                         const BD_OPEN3D_MVC_UNIT *p_unit)
{
    demux_sys_t *p_sys;
    BD_OPEN3D_MVC_INFO info;
    unsigned i_playlist = 0;
    char sz_clip_id[6];
    uint64_t i_sig = 0;
    int i_have = 0;
    static unsigned s_count = 0;
    static uint64_t s_last_sig = UINT64_MAX;

    if (!open3dblurayTraceMvcGateEnabled() || p_demux == NULL || p_unit == NULL)
        return;

    p_sys = p_demux->p_sys;
    if (p_sys == NULL)
        return;

    memset(&info, 0, sizeof(info));
    if (p_sys->bluray != NULL)
        i_have = bd_open3d_mvc_get_info(p_sys->bluray, &info);
    open3dblurayGetMvcTraceContext(p_sys, &i_playlist, sz_clip_id);

    ++s_count;
    i_sig |= ((uint64_t)i_playlist & 0xffffu) << 0;
    i_sig |= ((uint64_t)(unsigned)i_have & 0xffu) << 16;
    i_sig |= ((uint64_t)info.available & 0xffu) << 24;
    i_sig |= ((uint64_t)(p_sys->b_menu_open ? 1u : 0u)) << 32;
    i_sig |= ((uint64_t)(p_unit->flags & BD_OPEN3D_MVC_UNIT_FLAG_MATCHED ? 1u : 0u)) << 33;
    i_sig |= ((uint64_t)(p_sys->b_mvc_seen_matched ? 1u : 0u)) << 34;
    i_sig ^= ((uint64_t)p_unit->merged_size & 0xffffffffu) << 35;
    for (size_t i = 0; i < 5 && sz_clip_id[0] != '\0'; ++i)
        i_sig ^= (uint64_t)(uint8_t)sz_clip_id[i] << ((i % 4u) * 8u);

    if (s_count > 8 && (s_count % 128u) != 0u && i_sig == s_last_sig)
        return;

    s_last_sig = i_sig;
    fprintf(stderr,
            "open3dbluray raw mvc-unit count=%u playlist=%u clip_id=%s menu_open=%d "
            "have=%d available=%u flags=0x%x matched=%d merged=%u base=%u dep=%u "
            "base_pts=%" PRId64 " base_dts=%" PRId64 "\n",
            s_count,
            i_playlist,
            sz_clip_id[0] != '\0' ? sz_clip_id : "(none)",
            p_sys->b_menu_open ? 1 : 0,
            i_have,
            info.available,
            p_unit->flags,
            (p_unit->flags & BD_OPEN3D_MVC_UNIT_FLAG_MATCHED) ? 1 : 0,
            p_unit->merged_size,
            p_unit->base_size,
            p_unit->dependent_size,
            p_unit->base_pts,
            p_unit->base_dts);
    fflush(stderr);
}

static bool open3dblurayUseStockBaseFallback(demux_t *p_demux,
                                             BD_OPEN3D_MVC_INFO *p_info)
{
    demux_sys_t *p_sys;
    BD_OPEN3D_MVC_INFO info;
    int i_have;

    if (p_demux == NULL)
        return false;

    p_sys = p_demux->p_sys;
    if (p_sys == NULL || p_sys->bluray == NULL || p_sys->p_mvc_video == NULL)
        return false;

    memset(&info, 0, sizeof(info));
    i_have = bd_open3d_mvc_get_info(p_sys->bluray, &info);
    if (p_info != NULL)
        *p_info = info;

    /* The synthetic mvc1 ES can exist before any matched MVC unit actually
     * reaches the decoder. Keeping pid 0x1011 suppressed during that gap is
     * what turns early 2D/still/preroll lanes into audio-only or black-screen
     * playback. Stay on the stock base view until the synthetic lane has
     * proven it can carry matched stereo content, then fall back to the older
     * "no dependent view exists" rule for true 2D/menu clips. */
    if (!p_sys->b_mvc_seen_matched)
        return true;

    return i_have > 0 && info.available == 0 && info.dependent_pid == 0;
}

static void open3dblurayTraceMvcSelectionChange(demux_t *p_demux,
                                                const char *psz_mode,
                                                const BD_OPEN3D_MVC_INFO *p_info)
{
    demux_sys_t *p_sys;
    unsigned i_playlist = 0;
    char sz_clip_id[6];

    if (!open3dblurayTraceMvcGateEnabled() || p_demux == NULL || psz_mode == NULL)
        return;

    p_sys = p_demux->p_sys;
    if (p_sys == NULL)
        return;

    open3dblurayGetMvcTraceContext(p_sys, &i_playlist, sz_clip_id);
    fprintf(stderr,
            "open3dbluray raw mvc-select mode=%s playlist=%u clip_id=%s menu_open=%d "
            "available=%u dep_pid=0x%04x synthetic_es=%d matched=%d\n",
            psz_mode,
            i_playlist,
            sz_clip_id[0] != '\0' ? sz_clip_id : "(none)",
            p_sys->b_menu_open ? 1 : 0,
            p_info != NULL ? p_info->available : 0,
            p_info != NULL ? p_info->dependent_pid : 0,
            p_sys->p_mvc_video != NULL ? 1 : 0,
            p_sys->b_mvc_seen_matched ? 1 : 0);
    fflush(stderr);
}

static void open3dblurayTraceMvcHandoff(demux_t *p_demux,
                                        const char *psz_from,
                                        const char *psz_to)
{
    demux_sys_t *p_sys;
    unsigned i_playlist = 0;
    char sz_clip_id[6];

    if (!open3dblurayTraceMvcGateEnabled() || p_demux == NULL ||
        psz_from == NULL || psz_to == NULL)
        return;

    p_sys = p_demux->p_sys;
    if (p_sys == NULL)
        return;

    open3dblurayGetMvcTraceContext(p_sys, &i_playlist, sz_clip_id);
    fprintf(stderr,
            "open3dbluray raw mvc-handoff from=%s to=%s playlist=%u clip_id=%s "
            "menu_open=%d stock_base=%d synthetic_es=%d\n",
            psz_from,
            psz_to,
            i_playlist,
            sz_clip_id[0] != '\0' ? sz_clip_id : "(none)",
            p_sys->b_menu_open ? 1 : 0,
            p_sys->b_mvc_base_fallback_selected ? 1 : 0,
            p_sys->p_mvc_video != NULL ? 1 : 0);
    fflush(stderr);
}

static int open3dblurayEnsureMvcEs(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool b_direct_timing = open3dblurayUseDirectMvcTiming(p_demux);
    es_out_t *p_sink_out = open3dblurayGetMvcSinkOut(p_demux);
    const int i_group = b_direct_timing ? 0 : p_sys->i_mvc_group;

    if (p_sys->p_mvc_video)
        return VLC_SUCCESS;

    es_format_t fmt;
    es_format_Init(&fmt, VIDEO_ES, OPEN3DBLURAY_CODEC_MVC);
    fmt.b_packetized = true;
    fmt.i_id = 4114;
    fmt.i_group = i_group;
    fmt.i_priority = ES_PRIORITY_SELECTABLE_MIN;
    p_sys->p_mvc_video = es_out_Add(p_sink_out, &fmt);
    es_format_Clean(&fmt);
    if (!p_sys->p_mvc_video) {
        msg_Err(p_demux, "%s failed to add synthetic mvc1 ES", OPEN3DBLURAY_LOG_PREFIX);
        return VLC_EGENERIC;
    }

    es_out_Control(p_sink_out, ES_OUT_SET_ES, p_sys->p_mvc_video);
    p_sys->b_mvc_first_unit = true;
    p_sys->b_mvc_seen_matched = false;
    p_sys->b_mvc_base_fallback_selected = false;
    p_sys->b_mvc_hold_unmatched_until_match = false;
    msg_Info(p_demux, "%s created synthetic mvc1 ES group=%d via=%s",
             OPEN3DBLURAY_LOG_PREFIX,
             i_group,
             b_direct_timing ? "direct_legacy" :
             p_sys->p_esc_out != NULL ? "escape" :
             p_sys->p_tf_out != NULL ? "timestamps_filter" : "direct");
    return VLC_SUCCESS;
}

static void open3dblurayMaybeDumpMvcUnit(const BD_OPEN3D_MVC_UNIT *unit,
                                         const uint8_t *p_buf)
{
    static int s_init = 0;
    static const char *s_dump_dir = NULL;
    static unsigned long s_min_size = 0;
    static unsigned s_dump_index = 0;
    char sz_path[1024];
    FILE *fp;

    if (!s_init) {
        const char *psz_min_size;

        s_dump_dir = getenv("OPEN3DBLURAY_MVC_DUMP_DIR");
        psz_min_size = getenv("OPEN3DBLURAY_MVC_DUMP_MIN_SIZE");
        if (psz_min_size && *psz_min_size)
            s_min_size = strtoul(psz_min_size, NULL, 10);
        s_init = 1;
    }

    if (!s_dump_dir || !*s_dump_dir || !unit || !p_buf || unit->merged_size == 0)
        return;
    if (s_min_size > 0 && unit->merged_size < s_min_size)
        return;

    snprintf(sz_path, sizeof(sz_path), "%s/mvc_unit_%03u_merged.bin",
             s_dump_dir, s_dump_index);
    fp = fopen(sz_path, "wb");
    if (fp) {
        fwrite(p_buf, 1, unit->merged_size, fp);
        fclose(fp);
    }

    snprintf(sz_path, sizeof(sz_path), "%s/mvc_unit_%03u.meta",
             s_dump_dir, s_dump_index);
    fp = fopen(sz_path, "w");
    if (fp) {
        fprintf(fp,
                "flags=0x%x\n"
                "base_size=%u\n"
                "dep_size=%u\n"
                "merged_size=%u\n"
                "base_pts=%" PRId64 "\n"
                "base_dts=%" PRId64 "\n"
                "dep_pts=%" PRId64 "\n"
                "dep_dts=%" PRId64 "\n",
                unit->flags,
                unit->base_size,
                unit->dependent_size,
                unit->merged_size,
                unit->base_pts,
                unit->base_dts,
                unit->dependent_pts,
                unit->dependent_dts);
        fclose(fp);
    }

    s_dump_index++;
}

static int open3dblurayDrainMvcUnits(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool b_direct_timing = open3dblurayUseDirectMvcTiming(p_demux);
    es_out_t *p_sink_out = open3dblurayGetMvcSinkOut(p_demux);
    uint8_t *p_buf = NULL;
    uint32_t i_cap = 0;
    int i_units = 0;

    while (1) {
        BD_OPEN3D_MVC_UNIT unit;
        uint32_t i_need = i_cap;
        int i_ret;

        memset(&unit, 0, sizeof(unit));
        i_ret = bd_open3d_mvc_read_unit(p_sys->bluray, &unit, p_buf, &i_need);
        if (i_ret == 0)
            break;

        if (i_ret < 0) {
            uint8_t *p_new_buf;
            if (i_need <= i_cap || i_need == 0) {
                msg_Err(p_demux, "%s failed to read mvc unit need=%u cap=%u",
                        OPEN3DBLURAY_LOG_PREFIX, i_need, i_cap);
                break;
            }
            p_new_buf = realloc(p_buf, i_need);
            if (!p_new_buf) {
                msg_Err(p_demux, "%s failed to grow mvc unit buffer to %u bytes",
                        OPEN3DBLURAY_LOG_PREFIX, i_need);
                break;
            }
            p_buf = p_new_buf;
            i_cap = i_need;
            continue;
        }

        open3dblurayMaybeDumpMvcUnit(&unit, p_buf);

        if ((unit.flags & BD_OPEN3D_MVC_UNIT_FLAG_MATCHED) == 0 &&
            p_sys->b_mvc_seen_matched) {
            msg_Dbg(p_demux,
                    "%s mvc_unit drop(unmatched) flags=0x%x base_size=%u dep_size=%u merged_size=%u "
                    "base_pts=%" PRId64 " base_dts=%" PRId64 " dep_pts=%" PRId64 " dep_dts=%" PRId64,
                    OPEN3DBLURAY_LOG_PREFIX,
                    unit.flags,
                    unit.base_size,
                    unit.dependent_size,
                    unit.merged_size,
                    unit.base_pts,
                    unit.base_dts,
                    unit.dependent_pts,
                    unit.dependent_dts);
            continue;
        }

        if ((unit.flags & BD_OPEN3D_MVC_UNIT_FLAG_MATCHED) == 0 &&
            p_sys->b_mvc_hold_unmatched_until_match) {
            msg_Dbg(p_demux,
                    "%s mvc_unit hold(seek warmup) flags=0x%x base_size=%u dep_size=%u merged_size=%u "
                    "base_pts=%" PRId64 " base_dts=%" PRId64 " dep_pts=%" PRId64 " dep_dts=%" PRId64,
                    OPEN3DBLURAY_LOG_PREFIX,
                    unit.flags,
                    unit.base_size,
                    unit.dependent_size,
                    unit.merged_size,
                    unit.base_pts,
                    unit.base_dts,
                    unit.dependent_pts,
                    unit.dependent_dts);
            continue;
        }

        if ((unit.flags & BD_OPEN3D_MVC_UNIT_FLAG_MATCHED) == 0) {
            msg_Dbg(p_demux,
                    "%s mvc_unit warmup(base-only) flags=0x%x base_size=%u dep_size=%u merged_size=%u "
                    "base_pts=%" PRId64 " base_dts=%" PRId64 " dep_pts=%" PRId64 " dep_dts=%" PRId64,
                    OPEN3DBLURAY_LOG_PREFIX,
                    unit.flags,
                    unit.base_size,
                    unit.dependent_size,
                    unit.merged_size,
                    unit.base_pts,
                    unit.base_dts,
                    unit.dependent_pts,
                    unit.dependent_dts);
        }

        if (open3dblurayEnsureMvcEs(p_demux) != VLC_SUCCESS)
            break;

        block_t *p_block = block_Alloc(unit.merged_size);
        if (!p_block) {
            msg_Err(p_demux, "%s failed to allocate mvc block len=%u",
                    OPEN3DBLURAY_LOG_PREFIX, unit.merged_size);
            break;
        }

        if (unit.merged_size > 0)
            memcpy(p_block->p_buffer, p_buf, unit.merged_size);
        p_block->i_buffer = unit.merged_size;
        p_block->i_pts = open3dblurayTs90kToTick(unit.base_pts);
        p_block->i_dts = open3dblurayTs90kToTick(unit.base_dts);
        if (p_block->i_pts == VLC_TICK_INVALID && p_block->i_dts == VLC_TICK_INVALID) {
            msg_Dbg(p_demux,
                    "%s mvc_unit drop(no clock) flags=0x%x base_size=%u dep_size=%u merged_size=%u "
                    "base_pts=%" PRId64 " base_dts=%" PRId64 " dep_pts=%" PRId64 " dep_dts=%" PRId64,
                    OPEN3DBLURAY_LOG_PREFIX,
                    unit.flags,
                    unit.base_size,
                    unit.dependent_size,
                    unit.merged_size,
                    unit.base_pts,
                    unit.base_dts,
                    unit.dependent_pts,
                    unit.dependent_dts);
            block_Release(p_block);
            continue;
        }
        if (p_block->i_dts == VLC_TICK_INVALID)
            p_block->i_dts = p_block->i_pts;
        open3dblurayTraceSeekTimingMvcUnit(p_demux, &unit, p_block, b_direct_timing);
        if (p_sys->b_mvc_next_block_discontinuity)
            p_block->i_flags |= BLOCK_FLAG_DISCONTINUITY;
        if (p_sys->b_mvc_first_unit)
            p_sys->b_mvc_first_unit = false;
        if (b_direct_timing) {
            es_out_SetPCR(p_sink_out,
                          p_block->i_pts != VLC_TICK_INVALID ? p_block->i_pts
                                                             : p_block->i_dts);
        }
        if (p_sys->b_mvc_base_fallback_selected) {
            BD_OPEN3D_MVC_INFO info;
            memset(&info, 0, sizeof(info));
            bd_open3d_mvc_get_info(p_sys->bluray, &info);
            es_out_Control(p_sink_out, ES_OUT_SET_ES, p_sys->p_mvc_video);
            p_sys->b_mvc_base_fallback_selected = false;
            open3dblurayPublishForceMonoMenuState(p_demux);
            open3dblurayTraceMvcSelectionChange(p_demux, "synthetic", &info);
        }
        es_out_Send(p_sink_out, p_sys->p_mvc_video, p_block);
        open3dblurayTraceMvcUnitSend(p_demux, &unit);
        p_sys->b_mvc_next_block_discontinuity = false;
        if (unit.flags & BD_OPEN3D_MVC_UNIT_FLAG_MATCHED) {
            p_sys->b_mvc_seen_matched = true;
            p_sys->b_mvc_hold_unmatched_until_match = false;
        }

        msg_Dbg(p_demux,
                "%s mvc_unit flags=0x%x base_size=%u dep_size=%u merged_size=%u "
                "base_pts=%" PRId64 " base_dts=%" PRId64 " dep_pts=%" PRId64 " dep_dts=%" PRId64,
                OPEN3DBLURAY_LOG_PREFIX,
                unit.flags,
                unit.base_size,
                unit.dependent_size,
                unit.merged_size,
                unit.base_pts,
                unit.base_dts,
                unit.dependent_pts,
                unit.dependent_dts);
        i_units++;
    }

    free(p_buf);
    return i_units;
}
#endif

/*****************************************************************************
 * cache current playlist (title) information
 *****************************************************************************/

static void setTitleInfo(demux_sys_t *p_sys, BLURAY_TITLE_INFO *info)
{
    vlc_mutex_lock(&p_sys->pl_info_lock);

    if (p_sys->p_pl_info) {
        bd_free_title_info(p_sys->p_pl_info);
    }
    p_sys->p_pl_info   = info;
    p_sys->p_clip_info = NULL;

    if (p_sys->p_pl_info && p_sys->p_pl_info->clip_count) {
        p_sys->p_clip_info = &p_sys->p_pl_info->clips[0];
    }

    vlc_mutex_unlock(&p_sys->pl_info_lock);
}

/*****************************************************************************
 * create input attachment for thumbnail
 *****************************************************************************/

static void attachThumbnail(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (!p_sys->p_meta)
        return;

#if BLURAY_VERSION >= BLURAY_VERSION_CODE(0,9,0)
    if (p_sys->p_meta->thumb_count > 0 && p_sys->p_meta->thumbnails) {
        int64_t size;
        void *data;
        if (bd_get_meta_file(p_sys->bluray, p_sys->p_meta->thumbnails[0].path, &data, &size) > 0) {
            char psz_name[64];
            input_attachment_t *p_attachment;

            snprintf(psz_name, sizeof(psz_name), "picture%d_%s", p_sys->i_attachments, p_sys->p_meta->thumbnails[0].path);

            p_attachment = vlc_input_attachment_New(psz_name, NULL, "Album art", data, size);
            if (p_attachment) {
                p_sys->i_cover_idx = p_sys->i_attachments;
                TAB_APPEND(p_sys->i_attachments, p_sys->attachments, p_attachment);
            }
        }
        free(data);
    }
#endif
}

/*****************************************************************************
 * stream input
 *****************************************************************************/

static int probeStream(demux_t *p_demux)
{
    /* input must be seekable */
    bool b_canseek = false;
    vlc_stream_Control( p_demux->s, STREAM_CAN_SEEK, &b_canseek );
    if (!b_canseek) {
        return VLC_EGENERIC;
    }

    /* first sector(s) should be filled with zeros */
    ssize_t i_peek;
    const uint8_t *p_peek;
    i_peek = vlc_stream_Peek( p_demux->s, &p_peek, 2048 );
    if( i_peek != 2048 ) {
        return VLC_EGENERIC;
    }
    while (i_peek > 0) {
        if (p_peek[ --i_peek ]) {
            return VLC_EGENERIC;
        }
    }

    return VLC_SUCCESS;
}

#ifdef BLURAY_DEMUX
static int blurayReadBlock(void *object, void *buf, int lba, int num_blocks)
{
    demux_t *p_demux = (demux_t*)object;
    demux_sys_t *p_sys = p_demux->p_sys;
    int result = -1;

    assert(p_demux->s != NULL);

    vlc_mutex_lock(&p_sys->read_block_lock);

    if (vlc_stream_Seek( p_demux->s, lba * INT64_C(2048) ) == VLC_SUCCESS) {
        size_t  req = (size_t)2048 * num_blocks;
        ssize_t got;

        got = vlc_stream_Read( p_demux->s, buf, req);
        if (got < 0) {
            msg_Err(p_demux, "read from lba %d failed", lba);
        } else {
            result = got / 2048;
        }
    } else {
       msg_Err(p_demux, "seek to lba %d failed", lba);
    }

    vlc_mutex_unlock(&p_sys->read_block_lock);

    return result;
}
#endif

/*****************************************************************************
 * probing of local files
 *****************************************************************************/

/* Descriptor Tag (ECMA 167, 3/7.2) */
static int decode_descriptor_tag(const uint8_t *buf)
{
    uint16_t id;
    uint8_t  checksum = 0;
    int      i;

    id = buf[0] | (buf[1] << 8);

    /* calculate tag checksum */
    for (i = 0; i < 4; i++) {
        checksum = (uint8_t)(checksum + buf[i]);
    }
    for (i = 5; i < 16; i++) {
        checksum = (uint8_t)(checksum + buf[i]);
    }

    if (checksum != buf[4]) {
        return -1;
    }

    return id;
}

static int probeFile(const char *psz_name)
{
    struct stat stat_info;
    uint8_t peek[2048];
    unsigned i;
    int ret = VLC_EGENERIC;
    int fd;

    fd = vlc_open(psz_name, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        return VLC_EGENERIC;
    }

    if (fstat(fd, &stat_info) == -1) {
        goto bailout;
    }
    if (!S_ISREG(stat_info.st_mode) && !S_ISBLK(stat_info.st_mode)) {
        goto bailout;
    }

    /* first sector should be filled with zeros */
    if (read(fd, peek, sizeof(peek)) != sizeof(peek)) {
        goto bailout;
    }
    for (i = 0; i < sizeof(peek); i++) {
        if (peek[ i ]) {
            goto bailout;
        }
    }

    /* Check AVDP tag checksum */
    if (lseek(fd, 256 * 2048, SEEK_SET) == -1 ||
        read(fd, peek, 16) != 16 ||
        decode_descriptor_tag(peek) != 2) {
        goto bailout;
    }

    ret = VLC_SUCCESS;

bailout:
    vlc_close(fd);
    return ret;
}

/*****************************************************************************
 * blurayOpen: module init function
 *****************************************************************************/
static int blurayOpen(vlc_object_t *object)
{
    demux_t *p_demux = (demux_t*)object;
    demux_sys_t *p_sys;
    bool forced;
    uint64_t i_init_pos = 0;
    char *psz_resolved_bd_path = NULL;

    const char *error_msg = NULL;
#define BLURAY_ERROR(s) do { error_msg = s; goto error; } while(0)

    if (unlikely(!p_demux->p_input))
        return VLC_EGENERIC;

    forced = !strcasecmp(p_demux->psz_access, "bluray") ||
             !strcasecmp(p_demux->psz_access, OPEN3DBLURAY_ACCESS_NAME);

    if (!p_demux->s && p_demux->psz_file != NULL)
        psz_resolved_bd_path =
            open3dblurayResolveLocalPathCaseFallback(p_demux, p_demux->psz_file);

    if (p_demux->s) {
        if (!strcasecmp(p_demux->psz_access, "file")) {
            /* use access_demux for local files */
            return VLC_EGENERIC;
        }

        if (probeStream(p_demux) != VLC_SUCCESS) {
            return VLC_EGENERIC;
        }

    } else if (!forced) {
        if (!p_demux->psz_file) {
            free(psz_resolved_bd_path);
            return VLC_EGENERIC;
        }

        if (probeFile(psz_resolved_bd_path ? psz_resolved_bd_path : p_demux->psz_file)
            != VLC_SUCCESS) {
            free(psz_resolved_bd_path);
            return VLC_EGENERIC;
        }
    }

    /* */
    p_demux->p_sys = p_sys = vlc_obj_calloc(object, 1, sizeof(*p_sys));
    if (unlikely(!p_sys))
        return VLC_ENOMEM;

    p_sys->i_still_end_time = STILL_IMAGE_NOT_SET;
    p_sys->subtitle_selection.current.i_real_pid = -1;
    p_sys->subtitle_selection.pending_apply.i_real_pid = -1;
    p_sys->subtitle_selection.i_pending_forced_startup_real_pid = -1;
    p_sys->subtitle_selection.b_auto_forced_default_enabled = false;
    p_sys->subtitle_selection.b_pending_apply_valid = false;
    p_sys->i_forced_filter_pid = -1;
    p_sys->pp_forced_filter_last = &p_sys->p_forced_filter_head;
    p_sys->i_dummy_video_next_pts = CLOCK_FREQ / 25;
    p_sys->b_probe_yield_redundant_end_of_title =
        open3dblurayYieldRedundantEndOfTitleEnabled();
#if OPEN3DBLURAY_ENABLE_MVC
    p_sys->i_mvc_group = 1;
    p_sys->i_seek_timing_last_group_pcr = VLC_TICK_INVALID;
#endif

    /* init demux info fields */
    p_demux->info.i_update    = 0;
    p_demux->info.i_title     = 0;
    p_demux->info.i_seekpoint = 0;

    TAB_INIT(p_sys->i_title, p_sys->pp_title);
    TAB_INIT(p_sys->i_attachments, p_sys->attachments);
    ARRAY_INIT(p_sys->events_delayed);

    vlc_mutex_init(&p_sys->pl_info_lock);
    vlc_mutex_init(&p_sys->bdj_overlay_lock);
    vlc_mutex_init(&p_sys->read_block_lock); /* used during bd_open_stream() */
    Open3DSubtitleBridgeInit(&p_sys->subtitle_bridge);
    Open3DInteractiveGraphicsBridgeInit(&p_sys->interactive_graphics_bridge);

    /* request sub demuxers to skip continuity check as some split
       file concatenation are just resetting counters... */
    var_Create( p_demux, "ts-cc-check", VLC_VAR_BOOL );
    var_SetBool( p_demux, "ts-cc-check", false );
    var_Create( p_demux, "ts-standard", VLC_VAR_STRING );
    var_SetString( p_demux, "ts-standard", "mpeg" );
    var_Create( p_demux, "ts-pmtfix-waitdata", VLC_VAR_BOOL );
    var_SetBool( p_demux, "ts-pmtfix-waitdata", false );
    var_Create( p_demux, "ts-patfix", VLC_VAR_BOOL );
    var_SetBool( p_demux, "ts-patfix", false );
    var_Create( p_demux, "ts-pcr-offsetfix", VLC_VAR_BOOL );
    var_SetBool( p_demux, "ts-pcr-offsetfix", false );

    if( var_Type( p_demux->p_input, "menu-popup-available" ) == 0 )
        var_Create( p_demux->p_input, "menu-popup-available", VLC_VAR_BOOL );
    var_SetBool( p_demux->p_input, "menu-popup-available", false );
    if( var_Type( p_demux->p_input, OPEN3D_BLURAY_MENU_OPEN_VAR ) == 0 )
        var_Create( p_demux->p_input, OPEN3D_BLURAY_MENU_OPEN_VAR, VLC_VAR_BOOL );
    var_SetBool( p_demux->p_input, OPEN3D_BLURAY_MENU_OPEN_VAR, false );
    if( var_Type( p_demux->p_input, OPEN3D_BLURAY_FORCE_MONO_MENU_VAR ) == 0 )
        var_Create( p_demux->p_input, OPEN3D_BLURAY_FORCE_MONO_MENU_VAR, VLC_VAR_BOOL );
    var_SetBool( p_demux->p_input, OPEN3D_BLURAY_FORCE_MONO_MENU_VAR, false );
    Open3DSubtitleBridgeSetEnabledOnObject(VLC_OBJECT(p_demux->p_input), false);
    Open3DInteractiveGraphicsBridgeSetEnabledOnObject(VLC_OBJECT(p_demux->p_input),
                                                      false);

    var_AddCallback( p_demux->p_input, "intf-event", onIntfEvent, p_demux );

    msg_Info(p_demux,
             "%s build git=%s built=%s access=%s file=%s",
             OPEN3DBLURAY_LOG_PREFIX,
             OPEN3DBLURAY_BUILD_GIT_SHA,
             OPEN3DBLURAY_BUILD_TIME,
             p_demux->psz_access ? p_demux->psz_access : "(null)",
             p_demux->psz_file ? p_demux->psz_file : "(null)");
    if (p_sys->b_probe_yield_redundant_end_of_title) {
        msg_Info(p_demux,
                 "%s probe redundant_end_of_title_yield enabled=1",
                 OPEN3DBLURAY_LOG_PREFIX);
    }

    /* Open BluRay */
#ifdef BLURAY_DEMUX
    if (p_demux->s) {
        i_init_pos = vlc_stream_Tell(p_demux->s);

        p_sys->bluray = bd_init();
        if (!bd_open_stream(p_sys->bluray, p_demux, blurayReadBlock)) {
            bd_close(p_sys->bluray);
            p_sys->bluray = NULL;
        }
    } else
#endif
    {
        if (!p_demux->psz_file) {
            /* no path provided (bluray://). use default DVD device. */
            p_sys->psz_bd_path = var_InheritString(object, "dvd");
        } else if (psz_resolved_bd_path != NULL) {
            p_sys->psz_bd_path = psz_resolved_bd_path;
            psz_resolved_bd_path = NULL;
        } else {
            /* store current bd path */
            p_sys->psz_bd_path = strdup(p_demux->psz_file);
        }

        /* If we're passed a block device, try to convert it to the mount point. */
        FindMountPoint(&p_sys->psz_bd_path);

        p_sys->bluray = bd_open(p_sys->psz_bd_path, NULL);
    }
    if (!p_sys->bluray) {
        goto error;
    }

    /* Warning the user about AACS/BD+ */
    const BLURAY_DISC_INFO *disc_info = bd_get_disc_info(p_sys->bluray);

    /* Is it a bluray? */
    if (!disc_info->bluray_detected) {
        if (forced) {
            BLURAY_ERROR(_("Path doesn't appear to be a Blu-ray"));
        }
        goto error;
    }

    msg_Info(p_demux, "First play: %i, Top menu: %i\n"
                      "HDMV Titles: %i, BD-J Titles: %i, Other: %i",
             disc_info->first_play_supported, disc_info->top_menu_supported,
             disc_info->num_hdmv_titles, disc_info->num_bdj_titles,
             disc_info->num_unsupported_titles);

    /* AACS */
    if (disc_info->aacs_detected) {
        msg_Dbg(p_demux, "Disc is using AACS");
        if (!disc_info->libaacs_detected)
            BLURAY_ERROR(_("This Blu-ray Disc needs a library for AACS decoding"
                      ", and your system does not have it."));
        if (!disc_info->aacs_handled) {
            if (disc_info->aacs_error_code) {
                switch (disc_info->aacs_error_code) {
                case BD_AACS_CORRUPTED_DISC:
                    BLURAY_ERROR(_("Blu-ray Disc is corrupted."));
                case BD_AACS_NO_CONFIG:
                    BLURAY_ERROR(_("Missing AACS configuration file!"));
                case BD_AACS_NO_PK:
                    BLURAY_ERROR(_("No valid processing key found in AACS config file."));
                case BD_AACS_NO_CERT:
                    BLURAY_ERROR(_("No valid host certificate found in AACS config file."));
                case BD_AACS_CERT_REVOKED:
                    BLURAY_ERROR(_("AACS Host certificate revoked."));
                case BD_AACS_MMC_FAILED:
                    BLURAY_ERROR(_("AACS MMC failed."));
                }
            }
        }
    }

    /* BD+ */
    if (disc_info->bdplus_detected) {
        msg_Dbg(p_demux, "Disc is using BD+");
        if (!disc_info->libbdplus_detected)
            BLURAY_ERROR(_("This Blu-ray Disc needs a library for BD+ decoding"
                      ", and your system does not have it."));
        if (!disc_info->bdplus_handled)
            BLURAY_ERROR(_("Your system BD+ decoding library does not work. "
                      "Missing configuration?"));
    }

    /* set player region code */
    char *psz_region = var_InheritString(p_demux, "bluray-region");
    unsigned int region = psz_region ? (psz_region[0] - 'A') : REGION_DEFAULT;
    free(psz_region);
    bd_set_player_setting(p_sys->bluray, BLURAY_PLAYER_SETTING_REGION_CODE, 1<<region);

    /* set preferred languages */
    const char *psz_code = open3dblurayGetPreferredLanguageCode(p_demux, "audio-language");
    bd_set_player_setting_str(p_sys->bluray, BLURAY_PLAYER_SETTING_AUDIO_LANG, psz_code);
    psz_code = open3dblurayGetPreferredLanguageCode(p_demux, "sub-language");
    bd_set_player_setting_str(p_sys->bluray, BLURAY_PLAYER_SETTING_PG_LANG,    psz_code);
    psz_code = open3dblurayGetPreferredLanguageCode(p_demux, "menu-language");
    bd_set_player_setting_str(p_sys->bluray, BLURAY_PLAYER_SETTING_MENU_LANG,  psz_code);

    /* Get disc metadata */
    p_sys->p_meta = bd_get_meta(p_sys->bluray);
    if (!p_sys->p_meta)
        msg_Warn(p_demux, "Failed to get meta info.");

    p_sys->i_cover_idx = -1;
    attachThumbnail(p_demux);

    p_sys->b_menu = var_InheritBool(p_demux, "bluray-menu");

    /* Check BD-J capability */
    if (p_sys->b_menu && disc_info->bdj_detected && !disc_info->bdj_handled) {
        msg_Err(p_demux, "BD-J menus not supported. Playing without menus. "
                "BD-J support: %d, JVM found: %d, JVM usable: %d",
                disc_info->bdj_supported, disc_info->libjvm_detected, disc_info->bdj_handled);
        vlc_dialog_display_error(p_demux, _("Java required"),
             _("This Blu-ray disc requires Java for menus support.%s\nThe disc will be played without menus."),
             !disc_info->libjvm_detected ? _("Java was not found on your system.") : "");
        p_sys->b_menu = false;
    }

    /* Get titles and chapters */
    blurayInitTitles(p_demux, disc_info->num_hdmv_titles + disc_info->num_bdj_titles + 1/*Top Menu*/ + 1/*First Play*/);

    /*
     * Initialize the event queue, so we can receive events in blurayDemux(Menu).
     */
    msg_Info(p_demux, "%s open_step phase=before-bd-get-event-null menu=%d",
             OPEN3DBLURAY_LOG_PREFIX, p_sys->b_menu ? 1 : 0);
    bd_get_event(p_sys->bluray, NULL);
    msg_Info(p_demux, "%s open_step phase=after-bd-get-event-null menu=%d",
             OPEN3DBLURAY_LOG_PREFIX, p_sys->b_menu ? 1 : 0);

    /* Registering overlay event handler */
    bd_register_overlay_proc(p_sys->bluray, p_demux, blurayOverlayProc);

    if (p_sys->b_menu) {

        /* Register ARGB overlay handler for BD-J */
        if (disc_info->num_bdj_titles) {
            msg_Warn(p_demux,
                     "open3dbluray trace argb-register handle=%p bdj_titles=%u menu=%d buf=%p",
                     (void *)p_demux,
                     (unsigned)disc_info->num_bdj_titles,
                     p_sys->b_menu ? 1 : 0,
                     NULL);
            bd_register_argb_overlay_proc(p_sys->bluray, p_demux, blurayArgbOverlayProc, NULL);
        }

        /* libbluray will start playback from "First-Title" title */
        msg_Info(p_demux, "%s open_step phase=before-bd-play menu=%d",
                 OPEN3DBLURAY_LOG_PREFIX, p_sys->b_menu ? 1 : 0);
        int bd_play_ret = bd_play(p_sys->bluray);
        msg_Info(p_demux, "%s open_step phase=after-bd-play ret=%d",
                 OPEN3DBLURAY_LOG_PREFIX, bd_play_ret);
        if (bd_play_ret == 0)
            BLURAY_ERROR(_("Failed to start bluray playback. Please try without menu support."));

    } else {
        /* set start title number */
        if (bluraySetTitle(p_demux, p_sys->i_longest_title) != VLC_SUCCESS) {
            msg_Err(p_demux, "Could not set the title %d", p_sys->i_longest_title);
            goto error;
        }
    }

    p_sys->p_tf_out = timestamps_filter_es_out_New(p_demux->out);
    if(unlikely(!p_sys->p_tf_out))
        goto error;
    msg_Info(p_demux, "%s open_step phase=after-timestamps-filter-out",
             OPEN3DBLURAY_LOG_PREFIX);

    es_out_t *out_id = p_sys->p_tf_out;
    if (unlikely(disc_info->udf_volume_id &&
                 !strncmp(disc_info->udf_volume_id, "VLC Escape", strlen("VLC Escape"))))
    {
        p_sys->p_esc_out = escape_esOutNew(VLC_OBJECT(p_demux), p_sys->p_tf_out);
        out_id = p_sys->p_esc_out;
    }
    else
        p_sys->p_esc_out = NULL;

    p_sys->p_out = esOutNew(VLC_OBJECT(p_demux), out_id, p_demux);
    if (unlikely(p_sys->p_out == NULL))
        goto error;
    msg_Info(p_demux, "%s open_step phase=after-es-out-wrapper",
             OPEN3DBLURAY_LOG_PREFIX);

    p_sys->p_parser = vlc_demux_chained_New(VLC_OBJECT(p_demux), "ts", p_sys->p_out);
    if (!p_sys->p_parser) {
        msg_Err(p_demux, "Failed to create TS demuxer");
        goto error;
    }
    msg_Info(p_demux, "%s open_step phase=after-ts-parser",
             OPEN3DBLURAY_LOG_PREFIX);

#if OPEN3DBLURAY_ENABLE_MVC
    open3dblurayLogMvcInfo(p_demux, "open");
#endif

    p_demux->pf_control = blurayControl;
    p_demux->pf_demux   = blurayDemux;

    return VLC_SUCCESS;

error:
    free(psz_resolved_bd_path);
    if (error_msg)
        vlc_dialog_display_error(p_demux, _("Blu-ray error"), "%s", error_msg);
    blurayClose(object);

    if (p_demux->s != NULL) {
        /* restore stream position */
        if (vlc_stream_Seek(p_demux->s, i_init_pos) != VLC_SUCCESS) {
            msg_Err(p_demux, "Failed to seek back to stream start");
            return VLC_ETIMEOUT;
        }
    }

    return VLC_EGENERIC;
#undef BLURAY_ERROR
}


/*****************************************************************************
 * blurayClose: module destroy function
 *****************************************************************************/
static void blurayClose(vlc_object_t *object)
{
    demux_t *p_demux = (demux_t*)object;
    demux_sys_t *p_sys = p_demux->p_sys;

    if( p_demux->p_input != NULL &&
        var_Type( p_demux->p_input, "menu-popup-available" ) != 0 )
        var_SetBool( p_demux->p_input, "menu-popup-available", false );
    if( p_demux->p_input != NULL &&
        var_Type( p_demux->p_input, OPEN3D_BLURAY_MENU_OPEN_VAR ) != 0 )
        var_SetBool( p_demux->p_input, OPEN3D_BLURAY_MENU_OPEN_VAR, false );
    if( p_demux->p_input != NULL &&
        var_Type( p_demux->p_input, OPEN3D_BLURAY_FORCE_MONO_MENU_VAR ) != 0 )
        var_SetBool( p_demux->p_input, OPEN3D_BLURAY_FORCE_MONO_MENU_VAR, false );
    if( p_demux->p_input != NULL )
    {
        Open3DSubtitleBridgeSetEnabledOnObject(VLC_OBJECT(p_demux->p_input), false);
        Open3DInteractiveGraphicsBridgeSetEnabledOnObject(
            VLC_OBJECT(p_demux->p_input), false);
    }

    var_DelCallback( p_demux->p_input, "intf-event", onIntfEvent, p_demux );
    setTitleInfo(p_sys, NULL);

    /*
     * Close libbluray first.
     * This will close all the overlays before we release p_vout
     * bd_close(NULL) can crash
     */
    if (p_sys->bluray) {
        bd_close(p_sys->bluray);
    }

    blurayReleaseVout(p_demux);

    if (p_demux->p_input != NULL) {
        vlc_object_t *input_obj = VLC_OBJECT(p_demux->p_input);
        Open3DDirectBridgeNotifyAttachToObject(input_obj, NULL, NULL);
        Open3DSubtitleBridgeDetachFromObject(input_obj);
        Open3DInteractiveGraphicsBridgeDetachFromObject(input_obj);
    }

#if OPEN3DBLURAY_ENABLE_MVC
    if (p_sys->p_mvc_video)
        es_out_Del(open3dblurayGetMvcSinkOut(p_demux), p_sys->p_mvc_video);
#endif

    if (p_sys->p_parser)
        vlc_demux_chained_Delete(p_sys->p_parser);

    if (p_sys->p_out != NULL)
        es_out_Delete(p_sys->p_out);
    if (p_sys->p_esc_out != NULL)
        es_out_Delete(p_sys->p_esc_out);
    if(p_sys->p_tf_out)
        timestamps_filter_es_out_Delete(p_sys->p_tf_out);

    /* Titles */
    for (unsigned int i = 0; i < p_sys->i_title; i++)
        vlc_input_title_Delete(p_sys->pp_title[i]);
    TAB_CLEAN(p_sys->i_title, p_sys->pp_title);

    for (int i = 0; i < p_sys->i_attachments; i++)
      vlc_input_attachment_Delete(p_sys->attachments[i]);
    TAB_CLEAN(p_sys->i_attachments, p_sys->attachments);

    ARRAY_RESET(p_sys->events_delayed);

    vlc_mutex_destroy(&p_sys->pl_info_lock);
    vlc_mutex_destroy(&p_sys->bdj_overlay_lock);
    vlc_mutex_destroy(&p_sys->read_block_lock);
    Open3DSubtitleBridgeDestroy(&p_sys->subtitle_bridge);
    Open3DInteractiveGraphicsBridgeDestroy(&p_sys->interactive_graphics_bridge);

    open3dblurayForcedFilterClearQueued(p_sys);
    free(p_sys->psz_bd_path);
}

/*****************************************************************************
 * Elementary streams handling
 *****************************************************************************/
static uint8_t blurayGetStreamsUnlocked(demux_sys_t *p_sys,
                                        int i_stream_type,
                                        BLURAY_STREAM_INFO **pp_streams)
{
    if(!p_sys->p_clip_info)
        return 0;

    switch(i_stream_type)
    {
        case BD_EVENT_AUDIO_STREAM:
            *pp_streams = p_sys->p_clip_info->audio_streams;
            return p_sys->p_clip_info->audio_stream_count;
        case BD_EVENT_PG_TEXTST_STREAM:
            *pp_streams = p_sys->p_clip_info->pg_streams;
            return p_sys->p_clip_info->pg_stream_count;
        default:
            return 0;
    }
}

static BLURAY_STREAM_INFO * blurayGetStreamInfoUnlocked(demux_sys_t *p_sys,
                                                        int i_stream_type,
                                                        uint8_t i_stream_idx)
{
    BLURAY_STREAM_INFO *p_streams = NULL;
    uint8_t i_streams_count = blurayGetStreamsUnlocked(p_sys, i_stream_type, &p_streams);
    if(i_stream_idx < i_streams_count)
        return &p_streams[i_stream_idx];
    else
        return NULL;
}

static BLURAY_STREAM_INFO * blurayGetStreamInfoByPIDUnlocked(demux_sys_t *p_sys,
                                                             int i_pid)
{
    for(int i_type=BD_EVENT_AUDIO_STREAM; i_type<=BD_EVENT_SECONDARY_VIDEO_STREAM; i_type++)
    {
        BLURAY_STREAM_INFO *p_streams;
        uint8_t i_streams_count = blurayGetStreamsUnlocked(p_sys, i_type, &p_streams);
        for(uint8_t i=0; i<i_streams_count; i++)
        {
            if(p_streams[i].pid == i_pid)
                return &p_streams[i];
        }
    }
    return NULL;
}

static void open3dblurayUpdateSelectedSubtitleOffsetUnlocked(demux_t *p_demux, int i_pid)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    BLURAY_STREAM_INFO *p_stream = NULL;

    p_sys->b_selected_pg_offset_sequence_valid = false;
    p_sys->i_selected_pg_offset_sequence_id = 0xff;
    open3dblurayPublishSubtitleOffsetToVout(p_demux, NULL);

    if (i_pid > 0)
        p_stream = blurayGetStreamInfoByPIDUnlocked(p_sys, i_pid);

    if (!p_stream || p_stream->coding_type != BLURAY_STREAM_TYPE_SUB_PG ||
        p_stream->pg_offset_sequence_id == 0xff)
        return;

    p_sys->b_selected_pg_offset_sequence_valid = true;
    p_sys->i_selected_pg_offset_sequence_id = p_stream->pg_offset_sequence_id;

    msg_Info(p_demux,
             "%s subtitle_stream pid=0x%04x lang=%s offset_seq=%u ss=%u ss_offset_seq=%u",
             OPEN3DBLURAY_LOG_PREFIX,
             i_pid,
             p_stream->lang,
             p_stream->pg_offset_sequence_id,
             p_stream->pg_is_ss,
             p_stream->pg_ss_offset_sequence_id);

    open3dblurayPublishSubtitleOffsetToVout(p_demux, NULL);
}

static void open3dblurayLogSelectedAudioStreamUnlocked(demux_t *p_demux,
                                                       int i_pid,
                                                       const char *psz_source)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    BLURAY_STREAM_INFO *p_stream = NULL;

    if (i_pid <= 0)
        return;

    p_stream = blurayGetStreamInfoByPIDUnlocked(p_sys, i_pid);
    if (!p_stream)
        return;

    msg_Info(p_demux,
             "%s audio_stream source=%s pid=0x%04x lang=%s format=%u rate=%u",
             OPEN3DBLURAY_LOG_PREFIX,
             psz_source ? psz_source : "unknown",
             i_pid,
             p_stream->lang,
             p_stream->format,
             p_stream->rate);
}

static void setStreamLang(demux_sys_t *p_sys, es_format_t *p_fmt)
{
    vlc_mutex_lock(&p_sys->pl_info_lock);

    BLURAY_STREAM_INFO *p_stream = blurayGetStreamInfoByPIDUnlocked(p_sys, p_fmt->i_id);
    if(p_stream)
    {
        free(p_fmt->psz_language);
        p_fmt->psz_language = strndup((const char *)p_stream->lang, 3);
    }

    vlc_mutex_unlock(&p_sys->pl_info_lock);
}

static int blurayGetStreamPID(demux_sys_t *p_sys, int i_stream_type, uint8_t i_stream_idx)
{
    vlc_mutex_lock(&p_sys->pl_info_lock);

    BLURAY_STREAM_INFO *p_stream = blurayGetStreamInfoUnlocked(p_sys,
                                                               i_stream_type,
                                                               i_stream_idx);
    int i_pid = p_stream ? p_stream->pid : -1;

    vlc_mutex_unlock(&p_sys->pl_info_lock);

    return i_pid;
}

/*****************************************************************************
 * bluray fake es_out
 *****************************************************************************/
struct bluray_esout_sys_t
{
    es_out_t *p_dst_out;
    vlc_object_t *p_obj;
    vlc_array_t es; /* es_pair_t */
    bool b_entered_recycling;
    bool b_restart_decoders_on_reuse;
    void *priv;
    bool b_discontinuity;
    bool b_disable_output;
    bool b_lowdelay;
    vlc_mutex_t lock;
    struct
    {
        int i_audio_pid; /* Selected audio stream. -1 if default */
        int i_spu_pid;   /* Selected spu stream. -1 if default */
    } selected;
};

typedef struct
{
    int  i_pid;
    char psz_lang[4];
} open3dbluray_forced_pg_track_t;

enum
{
    BLURAY_ES_OUT_CONTROL_SET_ES_BY_PID = ES_OUT_PRIVATE_START,
    BLURAY_ES_OUT_CONTROL_UNSET_ES_BY_PID,
    BLURAY_ES_OUT_CONTROL_FLAG_DISCONTINUITY,
    BLURAY_ES_OUT_CONTROL_ENABLE_OUTPUT,
    BLURAY_ES_OUT_CONTROL_DISABLE_OUTPUT,
    BLURAY_ES_OUT_CONTROL_ENABLE_LOW_DELAY,
    BLURAY_ES_OUT_CONTROL_DISABLE_LOW_DELAY,
    BLURAY_ES_OUT_CONTROL_RANDOM_ACCESS,
};

static es_out_id_t *bluray_esOutAdd(es_out_t *p_out, const es_format_t *p_fmt)
{
    bluray_esout_sys_t *esout_sys = (bluray_esout_sys_t *)p_out->p_sys;
    demux_t *p_demux = esout_sys->priv;
    demux_sys_t *p_sys = p_demux->p_sys;
    es_format_t fmt;
    bool b_select = false;
    bool b_real_spu_ready = false;
    int i_real_spu_pid = -1;
    int i_selected_spu_pid = -1;

    es_format_Copy(&fmt, p_fmt);

    vlc_mutex_lock(&esout_sys->lock);

    switch (fmt.i_cat) {
    case VIDEO_ES:
#if OPEN3DBLURAY_ENABLE_MVC
        if (!p_sys->b_mvc_group_known && p_fmt->i_group > 0) {
            p_sys->i_mvc_group = p_fmt->i_group;
            p_sys->b_mvc_group_known = true;
            msg_Dbg(p_demux, "%s captured mvc group=%d from video pid=0x%x",
                    OPEN3DBLURAY_LOG_PREFIX, p_sys->i_mvc_group, p_fmt->i_id);
        }
        if (p_fmt->i_id == 0x1011) {
            /* Keep the stock base-view selected until the synthetic mvc1 path
             * actually exists. Suppressing pid 0x1011 at ES-add time leaves
             * discs with early base-only preroll/title playlists in an
             * audio-only state until MVC units eventually create the merged ES. */
            if (p_sys->p_mvc_video != NULL) {
                BD_OPEN3D_MVC_INFO info;
                b_select = open3dblurayUseStockBaseFallback(p_demux, &info);
            } else {
                b_select = true;
            }
            fmt.i_priority = ES_PRIORITY_NOT_SELECTABLE;
            break;
        }
#endif
        if(esout_sys->b_lowdelay)
        {
            fmt.video.i_frame_rate = 1; fmt.video.i_frame_rate_base = 1;
            fmt.b_packetized = true;
        }
        b_select = (p_fmt->i_id == 0x1011);
        fmt.i_priority = ES_PRIORITY_NOT_SELECTABLE;
        break;
    case AUDIO_ES:
#if OPEN3DBLURAY_ENABLE_MVC
        if (!p_sys->b_mvc_group_known && p_fmt->i_group > 0) {
            p_sys->i_mvc_group = p_fmt->i_group;
            p_sys->b_mvc_group_known = true;
            msg_Dbg(p_demux, "%s captured mvc group=%d from audio pid=0x%x",
                    OPEN3DBLURAY_LOG_PREFIX, p_sys->i_mvc_group, p_fmt->i_id);
        }
#endif
        b_select = (esout_sys->selected.i_audio_pid == p_fmt->i_id);
        fmt.i_priority = ES_PRIORITY_NOT_SELECTABLE;
        setStreamLang(p_sys, &fmt);
        break ;
    case SPU_ES:
    {
        int i_forced_real_pid = -1;
        if (open3dblurayDecodeForcedSubtitleEsId(p_fmt->i_id, &i_forced_real_pid)) {
            b_select = open3dblurayHasRememberedForcedSubtitleSelection(p_demux,
                                                                        i_forced_real_pid);
        } else {
            b_select = open3dblurayHasRememberedBaseSubtitleSelection(p_demux,
                                                                      p_fmt->i_id) &&
                       p_sys->b_spu_enable;
        }
        fmt.i_priority = ES_PRIORITY_NOT_SELECTABLE;
        setStreamLang(p_sys, &fmt);
        break ;
    }
    default:
        break ;
    }

    es_out_id_t *p_es = NULL;
    if (p_fmt->i_id >= 0) {
        /* Ensure we are not overriding anything */
        es_pair_t *p_pair = getEsPairByPID(&esout_sys->es, p_fmt->i_id);
        if (p_pair == NULL)
        {
            msg_Info(p_demux, "Adding ES %d select %d", p_fmt->i_id, b_select);
            p_es = es_out_Add(esout_sys->p_dst_out, &fmt);
            es_pair_Add(&esout_sys->es, &fmt, p_es);
            if (p_es && esout_sys->b_discontinuity) {
                es_pair_t *p_added = getEsPairByPID(&esout_sys->es, p_fmt->i_id);
                if (p_added)
                    p_added->i_next_block_flags |= BLOCK_FLAG_DISCONTINUITY;
            }
        }
        else
        {
            msg_Info(p_demux, "Reusing ES %d", p_fmt->i_id);
            p_pair->b_recyling = false;
            p_es = p_pair->p_es;
            if(!es_format_IsSimilar(p_fmt, &p_pair->fmt) ||
               p_fmt->b_packetized != p_pair->fmt.b_packetized ||
               strcmp(fmt.psz_language ? fmt.psz_language : "",
                      p_pair->fmt.psz_language ? p_pair->fmt.psz_language : "") ||
               esout_sys->b_restart_decoders_on_reuse)
            {
                es_out_Control(esout_sys->p_dst_out, ES_OUT_SET_ES_FMT, p_pair->p_es, &fmt);
                es_format_Clean(&p_pair->fmt);
                es_format_Copy(&p_pair->fmt, &fmt);
            }
        }
    }

    if (p_es)
    {
        if(b_select)
            es_out_Control(esout_sys->p_dst_out, ES_OUT_SET_ES, p_es);
        else
            es_out_Control(esout_sys->p_dst_out, ES_OUT_SET_ES_STATE, p_es, false);

        if (fmt.i_cat == SPU_ES && !open3dblurayDecodeForcedSubtitleEsId(fmt.i_id, NULL)) {
            es_pair_t *p_real_pair = getEsPairByPID(&esout_sys->es, fmt.i_id);
            open3dblurayMaybeAddForcedSubtitleTrackLocked(p_demux, esout_sys, p_real_pair);
            b_real_spu_ready = true;
            i_real_spu_pid = fmt.i_id;
            i_selected_spu_pid = esout_sys->selected.i_spu_pid;
        }
    }
    es_format_Clean(&fmt);

    vlc_mutex_unlock(&esout_sys->lock);

    if (b_real_spu_ready) {
        const bool b_pending_forced = open3dblurayTakePendingForcedStartupSubtitle(p_demux,
                                                                                    i_real_spu_pid);
        const bool b_remembered_forced = open3dblurayHasRememberedForcedSubtitleSelection(p_demux,
                                                                                           i_real_spu_pid);
        if (b_pending_forced || b_remembered_forced) {
            const bool b_auto_forced = open3dblurayHasRememberedAutoForcedSubtitleSelection(p_demux,
                                                                                             i_real_spu_pid);
            msg_Dbg(p_demux,
                    "%s deferring forced-only subtitle selection pid=0x%04x auto=%d until demux loop",
                    OPEN3DBLURAY_LOG_PREFIX,
                    i_real_spu_pid,
                    b_auto_forced ? 1 : 0);
            open3dbluraySchedulePendingForcedSubtitleSelection(p_demux,
                                                               i_real_spu_pid,
                                                               b_auto_forced);
        } else if (open3dblurayShouldAutoPromotePreferredSubtitle(p_demux,
                                                                  i_real_spu_pid,
                                                                  i_selected_spu_pid)) {
            msg_Dbg(p_demux,
                    "%s deferring preferred forced-only subtitle promotion pid=0x%04x until demux loop",
                    OPEN3DBLURAY_LOG_PREFIX,
                    i_real_spu_pid);
            open3dbluraySchedulePendingForcedSubtitleSelection(p_demux,
                                                               i_real_spu_pid,
                                                               true);
        }
    }

    return p_es;
}

static void bluray_esOutDeleteNonReusedESUnlocked(es_out_t *p_out)
{
    bluray_esout_sys_t *esout_sys = (bluray_esout_sys_t *)p_out->p_sys;

    if(esout_sys->b_discontinuity)
        esout_sys->b_discontinuity = false;

    if(!esout_sys->b_entered_recycling)
        return;

    esout_sys->b_entered_recycling = false;
    esout_sys->b_restart_decoders_on_reuse = true;

    es_pair_t *p_pair;
    while((p_pair = getUnusedEsPair(&esout_sys->es)))
    {
        msg_Info(esout_sys->p_obj, "Trashing unused ES %d", p_pair->fmt.i_id);
        es_out_Del(esout_sys->p_dst_out, p_pair->p_es);
        es_pair_Remove(&esout_sys->es, p_pair);
    }
}

static int bluray_esOutSend(es_out_t *p_out, es_out_id_t *p_es, block_t *p_block)
{
    bluray_esout_sys_t *esout_sys = (bluray_esout_sys_t *)p_out->p_sys;
    demux_t *p_demux = esout_sys->priv;
    demux_sys_t *p_sys = p_demux->p_sys;
    vlc_mutex_lock(&esout_sys->lock);

    bluray_esOutDeleteNonReusedESUnlocked(p_out);

    es_pair_t *p_pair = getEsPairByES(&esout_sys->es, p_es);
    if(p_pair && p_pair->i_next_block_flags)
    {
        p_block->i_flags |= p_pair->i_next_block_flags;
        p_pair->i_next_block_flags = 0;
    }
#if OPEN3DBLURAY_ENABLE_MVC
    if (p_block && p_pair)
        open3dblurayTraceSeekTimingAudio(p_demux, p_pair, p_block);
#endif
    if (p_block && p_pair && p_pair->fmt.i_cat == SPU_ES &&
        !open3dblurayDecodeForcedSubtitleEsId(p_pair->fmt.i_id, NULL))
        open3dblurayTracePgsBlock(p_demux, p_pair->fmt.i_id, p_block);
    if(esout_sys->b_disable_output)
    {
        block_Release(p_block);
        p_block = NULL;
    }
    else if (p_block && p_pair && p_pair->fmt.i_cat == SPU_ES &&
             !open3dblurayDecodeForcedSubtitleEsId(p_pair->fmt.i_id, NULL) &&
             open3dblurayHasRememberedForcedSubtitleSelection(p_demux, p_pair->fmt.i_id))
    {
        es_pair_t *p_forced_pair = getEsPairByPID(&esout_sys->es,
                                                  open3dblurayMakeForcedSubtitleEsId(p_pair->fmt.i_id));
        if (p_forced_pair != NULL) {
            open3dblurayTraceSpuSend(p_demux,
                                     esout_sys->selected.i_spu_pid,
                                     p_pair,
                                     p_block,
                                     "forced_filter");
            int i_send_ret = open3dbluraySendForcedPgsBlock(p_demux,
                                                            esout_sys->p_dst_out,
                                                            p_forced_pair->p_es,
                                                            p_pair->fmt.i_id,
                                                            p_block);
            vlc_mutex_unlock(&esout_sys->lock);
            return i_send_ret;
        }
    }
#if OPEN3DBLURAY_ENABLE_MVC
    else if (p_block && p_pair &&
             p_sys->p_mvc_video &&
             p_pair->fmt.i_cat == VIDEO_ES &&
             p_pair->fmt.i_id == 0x1011)
    {
        BD_OPEN3D_MVC_INFO info;
        const bool b_use_stock_base = open3dblurayUseStockBaseFallback(p_demux, &info);

        if (b_use_stock_base) {
            if (!p_sys->b_mvc_base_fallback_selected) {
                es_out_t *p_sink_out = open3dblurayGetMvcSinkOut(p_demux);

                open3dblurayTraceMvcHandoff(p_demux, "synthetic", "stock-base");
                es_out_Control(p_sink_out, ES_OUT_SET_ES_STATE, p_sys->p_mvc_video, false);
                es_out_Control(esout_sys->p_dst_out, ES_OUT_SET_ES, p_pair->p_es);
                p_sys->b_mvc_base_fallback_selected = true;
                open3dblurayPublishForceMonoMenuState(p_demux);
                open3dblurayTraceMvcSelectionChange(p_demux, "stock-base", &info);
            }
        } else if (p_sys->b_mvc_base_fallback_selected) {
            open3dblurayTraceMvcHandoff(p_demux, "stock-base", "synthetic");
            es_out_Control(esout_sys->p_dst_out, ES_OUT_SET_ES_STATE, p_pair->p_es, false);
            es_out_Control(open3dblurayGetMvcSinkOut(p_demux), ES_OUT_SET_ES,
                           p_sys->p_mvc_video);
            p_sys->b_mvc_base_fallback_selected = false;
            open3dblurayPublishForceMonoMenuState(p_demux);
            open3dblurayTraceMvcSelectionChange(p_demux, "synthetic", &info);
        }

        /* Suppress the stock base-view video payload while the synthetic mvc1 ES
         * is active, but keep feeding the TS parser so audio/subtitles survive. */
        if (!b_use_stock_base) {
            open3dblurayTraceMvcGate(p_demux, (uint16_t)p_pair->fmt.i_id,
                                     p_block->i_buffer);
            block_Release(p_block);
            p_block = NULL;
        }
    }
#endif
    if (p_block && p_pair && p_pair->fmt.i_cat == SPU_ES)
        open3dblurayTraceSpuSend(p_demux,
                                 esout_sys->selected.i_spu_pid,
                                 p_pair,
                                 p_block,
                                 "decoder");
    vlc_mutex_unlock(&esout_sys->lock);
    return (p_block) ? es_out_Send(esout_sys->p_dst_out, p_es, p_block) : VLC_SUCCESS;
}

static void bluray_esOutDel(es_out_t *p_out, es_out_id_t *p_es)
{
    bluray_esout_sys_t *esout_sys = (bluray_esout_sys_t *)p_out->p_sys;

    vlc_mutex_lock(&esout_sys->lock);

    if(esout_sys->b_discontinuity)
        esout_sys->b_discontinuity = false;

    es_pair_t *p_pair = getEsPairByES(&esout_sys->es, p_es);
    if (p_pair)
    {
        p_pair->b_recyling = true;
        esout_sys->b_entered_recycling = true;
    }

    vlc_mutex_unlock(&esout_sys->lock);
}

static int bluray_esOutControl(es_out_t *p_out, int i_query, va_list args)
{
    bluray_esout_sys_t *esout_sys = (bluray_esout_sys_t *)p_out->p_sys;
    demux_t *p_demux = esout_sys->priv;
    int i_ret;
    vlc_mutex_lock(&esout_sys->lock);

    if(esout_sys->b_disable_output &&
       i_query < ES_OUT_PRIVATE_START)
    {
        vlc_mutex_unlock(&esout_sys->lock);
        return VLC_EGENERIC;
    }

    if(esout_sys->b_discontinuity)
        esout_sys->b_discontinuity = false;

    switch(i_query)
    {
        case BLURAY_ES_OUT_CONTROL_SET_ES_BY_PID:
        case BLURAY_ES_OUT_CONTROL_UNSET_ES_BY_PID:
        {
            bool b_select = (i_query == BLURAY_ES_OUT_CONTROL_SET_ES_BY_PID);
            const int i_bluray_stream_type = va_arg(args, int);
            const int i_pid = va_arg(args, int);
            int i_effective_pid = i_pid;
            switch(i_bluray_stream_type)
            {
                case BD_EVENT_AUDIO_STREAM:
                    esout_sys->selected.i_audio_pid = i_pid;
                    break;
                case BD_EVENT_PG_TEXTST_STREAM:
                    esout_sys->selected.i_spu_pid = i_pid;
                    if (open3dblurayHasRememberedForcedSubtitleSelection(p_demux, i_pid))
                        i_effective_pid = open3dblurayMakeForcedSubtitleEsId(i_pid);
                    break;
                default:
                    break;
            }

            es_pair_t *p_pair = getEsPairByPID(&esout_sys->es, i_effective_pid);
            if(unlikely(!p_pair))
            {
                vlc_mutex_unlock(&esout_sys->lock);
                return VLC_EGENERIC;
            }

            if (i_bluray_stream_type == BD_EVENT_PG_TEXTST_STREAM &&
                i_effective_pid != i_pid) {
                es_pair_t *p_real_pair = getEsPairByPID(&esout_sys->es, i_pid);
                if (p_real_pair != NULL)
                    es_out_Control(esout_sys->p_dst_out, ES_OUT_SET_ES_STATE,
                                   p_real_pair->p_es, false);
            } else if (i_bluray_stream_type == BD_EVENT_PG_TEXTST_STREAM) {
                es_pair_t *p_forced_pair = getEsPairByPID(&esout_sys->es,
                                                          open3dblurayMakeForcedSubtitleEsId(i_pid));
                if (p_forced_pair != NULL)
                    es_out_Control(esout_sys->p_dst_out, ES_OUT_SET_ES_STATE,
                                   p_forced_pair->p_es, false);
            }

            if(b_select)
                i_ret = es_out_Control(esout_sys->p_dst_out, ES_OUT_SET_ES, p_pair->p_es);
            else
                i_ret = es_out_Control(esout_sys->p_dst_out, ES_OUT_SET_ES_STATE,
                                      p_pair->p_es, false);
            break;
        };

        case BLURAY_ES_OUT_CONTROL_FLAG_DISCONTINUITY:
        {
            esout_sys->b_discontinuity = true;
            for (size_t i = 0; i < vlc_array_count(&esout_sys->es); ++i)
            {
                es_pair_t *p_pair = vlc_array_item_at_index(&esout_sys->es, i);
                p_pair->i_next_block_flags |= BLOCK_FLAG_DISCONTINUITY;
            }
#if OPEN3DBLURAY_ENABLE_MVC
            open3dblurayArmMvcRestart(p_demux, "es_out_discontinuity");
#endif
            i_ret = VLC_SUCCESS;
        } break;

        case BLURAY_ES_OUT_CONTROL_RANDOM_ACCESS:
        {
            esout_sys->b_restart_decoders_on_reuse = !va_arg(args, int);
            i_ret = VLC_SUCCESS;
        } break;

        case BLURAY_ES_OUT_CONTROL_ENABLE_OUTPUT:
        case BLURAY_ES_OUT_CONTROL_DISABLE_OUTPUT:
        {
            esout_sys->b_disable_output = (i_query == BLURAY_ES_OUT_CONTROL_DISABLE_OUTPUT);
            i_ret = VLC_SUCCESS;
        } break;

        case BLURAY_ES_OUT_CONTROL_ENABLE_LOW_DELAY:
        case BLURAY_ES_OUT_CONTROL_DISABLE_LOW_DELAY:
        {
            esout_sys->b_lowdelay = (i_query == BLURAY_ES_OUT_CONTROL_ENABLE_LOW_DELAY);
            i_ret = VLC_SUCCESS;
        } break;

        case ES_OUT_SET_ES_DEFAULT:
        case ES_OUT_SET_ES:
        case ES_OUT_SET_ES_STATE:
            i_ret = VLC_EGENERIC;
            break;

        case ES_OUT_GET_ES_STATE:
            va_arg(args, es_out_id_t *);
            *va_arg(args, bool *) = true;
            i_ret = VLC_SUCCESS;
            break;

#if OPEN3DBLURAY_ENABLE_MVC
        case ES_OUT_RESET_PCR:
            open3dblurayTraceSeekTimingResetPcr(p_demux);
            i_ret = es_out_Control(esout_sys->p_dst_out, i_query);
            break;

        case ES_OUT_SET_GROUP_PCR:
        {
            int i_group = va_arg(args, int);
            vlc_tick_t i_pcr = va_arg(args, int64_t);
            open3dblurayTraceSeekTimingStockPcr(p_demux, i_group, i_pcr, i_pcr);
            i_ret = es_out_Control(esout_sys->p_dst_out, i_query, i_group, i_pcr);
            break;
        }
#endif

        default:
            i_ret = es_out_vaControl(esout_sys->p_dst_out, i_query, args);
            break;
    }
    vlc_mutex_unlock(&esout_sys->lock);
    return i_ret;
}

static void bluray_esOutDestroy(es_out_t *p_out)
{
    bluray_esout_sys_t *esout_sys = (bluray_esout_sys_t *)p_out->p_sys;

    for (size_t i = 0; i < vlc_array_count(&esout_sys->es); ++i)
        free(vlc_array_item_at_index(&esout_sys->es, i));
    vlc_array_clear(&esout_sys->es);
    vlc_mutex_destroy(&esout_sys->lock);
    free(p_out->p_sys);
    free(p_out);
}

static es_out_t *esOutNew(vlc_object_t *p_obj, es_out_t *p_dst_out, void *priv)
{
    es_out_t    *p_out = malloc(sizeof(*p_out));
    if (unlikely(p_out == NULL))
        return NULL;

    p_out->pf_add       = bluray_esOutAdd;
    p_out->pf_control   = bluray_esOutControl;
    p_out->pf_del       = bluray_esOutDel;
    p_out->pf_destroy   = bluray_esOutDestroy;
    p_out->pf_send      = bluray_esOutSend;

    bluray_esout_sys_t *esout_sys = malloc(sizeof(*esout_sys));
    if (unlikely(esout_sys == NULL))
    {
        free(p_out);
        return NULL;
    }
    p_out->p_sys = (es_out_sys_t *) esout_sys;
    vlc_array_init(&esout_sys->es);
    esout_sys->p_dst_out = p_dst_out;
    esout_sys->p_obj = p_obj;
    esout_sys->priv = priv;
    esout_sys->b_discontinuity = false;
    esout_sys->b_disable_output = false;
    esout_sys->b_entered_recycling = false;
    esout_sys->b_restart_decoders_on_reuse = true;
    esout_sys->b_lowdelay = false;
    esout_sys->selected.i_audio_pid = -1;
    esout_sys->selected.i_spu_pid = -1;
    vlc_mutex_init(&esout_sys->lock);
    return p_out;
}

static open3dbluray_subtitle_selection_t
open3dblurayMakeSubtitleSelection(int i_pid,
                                  bool b_forced_only,
                                  bool b_auto_forced_only)
{
    open3dbluray_subtitle_selection_t selection = {
        .i_real_pid = i_pid,
        .b_forced_only = (i_pid > 0) ? b_forced_only : false,
        .b_auto_forced_only = (i_pid > 0 && b_forced_only) ? b_auto_forced_only : false,
    };
    return selection;
}

static void open3dblurayRememberSubtitleSelectionEx(demux_t *p_demux,
                                                    int i_pid,
                                                    bool b_forced_only,
                                                    bool b_auto_forced_only)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    p_sys->subtitle_selection.current =
        open3dblurayMakeSubtitleSelection(i_pid,
                                          b_forced_only,
                                          b_auto_forced_only);
}

static void open3dblurayRememberSubtitleSelection(demux_t *p_demux,
                                                  int i_pid,
                                                  bool b_forced_only)
{
    open3dblurayRememberSubtitleSelectionEx(p_demux, i_pid, b_forced_only, false);
}

static bool open3dblurayHasRememberedBaseSubtitleSelection(demux_t *p_demux,
                                                           int i_pid)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const open3dbluray_subtitle_selection_t *p_selection =
        &p_sys->subtitle_selection.current;

    return p_selection->i_real_pid == i_pid &&
           !p_selection->b_forced_only;
}

static void open3dblurayForcedFilterClearQueued(demux_sys_t *p_sys)
{
    if (p_sys->p_forced_filter_head != NULL)
        block_ChainRelease(p_sys->p_forced_filter_head);
    p_sys->p_forced_filter_head = NULL;
    p_sys->pp_forced_filter_last = &p_sys->p_forced_filter_head;
}

static void open3dblurayForcedFilterInvalidate(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    open3dblurayForcedFilterClearQueued(p_sys);
    p_sys->b_forced_filter_output_active = false;
}

static void open3dblurayForcedFilterReset(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    open3dblurayForcedFilterInvalidate(p_demux);
    p_sys->i_forced_filter_pid = -1;
}

static void open3dblurayForcedFilterSelectPid(demux_t *p_demux, int i_pid)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->i_forced_filter_pid != i_pid)
        open3dblurayForcedFilterInvalidate(p_demux);

    p_sys->i_forced_filter_pid = i_pid;
}

typedef struct
{
    bool b_valid;
    uint16_t i_width;
    uint16_t i_height;
    uint8_t i_frame_rate;
    uint16_t i_composition_number;
    uint8_t i_composition_state;
    uint8_t i_palette_update_flag;
    uint8_t i_palette_id;
    uint8_t i_object_count;
    uint8_t i_forced_object_count;
    const uint8_t *p_payload;
} open3dbluray_pgs_pcs_t;

static bool open3dblurayParsePgsPcs(const block_t *p_block,
                                    open3dbluray_pgs_pcs_t *p_pcs)
{
    if (p_pcs != NULL)
        memset(p_pcs, 0, sizeof(*p_pcs));

    if (p_block == NULL || p_pcs == NULL || p_block->p_buffer == NULL ||
        p_block->i_buffer < 14 || p_block->p_buffer[0] != 0x16)
        return false;

    const size_t i_payload_len = ((size_t)p_block->p_buffer[1] << 8) |
                                 (size_t)p_block->p_buffer[2];
    if (p_block->i_buffer < 3 + i_payload_len || i_payload_len < 11)
        return false;

    const uint8_t *p_payload = p_block->p_buffer + 3;
    const uint8_t i_object_count = p_payload[10];
    const size_t i_needed = 11u + 8u * i_object_count;
    if (i_payload_len < i_needed)
        return false;

    p_pcs->b_valid = true;
    p_pcs->i_width = GetWBE(&p_payload[0]);
    p_pcs->i_height = GetWBE(&p_payload[2]);
    p_pcs->i_frame_rate = p_payload[4];
    p_pcs->i_composition_number = GetWBE(&p_payload[5]);
    p_pcs->i_composition_state = p_payload[7];
    p_pcs->i_palette_update_flag = p_payload[8];
    p_pcs->i_palette_id = p_payload[9];
    p_pcs->i_object_count = i_object_count;
    p_pcs->p_payload = p_payload;

    for (uint8_t i = 0; i < i_object_count; ++i) {
        const uint8_t *p_object = &p_payload[11 + 8u * i];
        if ((p_object[3] & 0x40u) != 0)
            p_pcs->i_forced_object_count++;
    }

    return true;
}

static block_t *open3dblurayBuildPgsPcsBlock(const block_t *p_src,
                                             const open3dbluray_pgs_pcs_t *p_pcs,
                                             bool b_clear_only)
{
    if (p_src == NULL || p_pcs == NULL || !p_pcs->b_valid)
        return NULL;

    const uint8_t i_output_objects = b_clear_only ? 0 : p_pcs->i_forced_object_count;
    const size_t i_payload_len = 11u + 8u * i_output_objects;
    block_t *p_out = block_Alloc(3 + i_payload_len);
    if (p_out == NULL)
        return NULL;

    block_CopyProperties(p_out, (block_t *)p_src);
    p_out->p_buffer[0] = 0x16;
    p_out->p_buffer[1] = (uint8_t)(i_payload_len >> 8);
    p_out->p_buffer[2] = (uint8_t)(i_payload_len & 0xff);
    memcpy(p_out->p_buffer + 3, p_pcs->p_payload, 11);
    p_out->p_buffer[3 + 10] = i_output_objects;
    p_out->i_buffer = 3 + i_payload_len;

    if (!b_clear_only && p_pcs->i_forced_object_count > 0) {
        size_t i_dst = 3 + 11;
        for (uint8_t i = 0; i < p_pcs->i_object_count; ++i) {
            const uint8_t *p_object = &p_pcs->p_payload[11 + 8u * i];
            if ((p_object[3] & 0x40u) == 0)
                continue;
            memcpy(&p_out->p_buffer[i_dst], p_object, 8);
            i_dst += 8;
        }
    }

    return p_out;
}

static int open3dblurayFlushForcedPgsDisplaySet(demux_t *p_demux,
                                                es_out_t *p_dst_out,
                                                es_out_id_t *p_forced_es)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    block_t *p_list = p_sys->p_forced_filter_head;
    block_t *p_block;
    block_t *p_next;
    block_t *p_pcs_block = NULL;
    open3dbluray_pgs_pcs_t pcs = { 0 };
    bool b_emit = false;
    bool b_clear_only = false;
    int i_ret = VLC_SUCCESS;

    p_sys->p_forced_filter_head = NULL;
    p_sys->pp_forced_filter_last = &p_sys->p_forced_filter_head;

    for (p_block = p_list; p_block != NULL; p_block = p_block->p_next) {
        if (p_block->p_buffer != NULL && p_block->i_buffer >= 1 &&
            p_block->p_buffer[0] == 0x16) {
            p_pcs_block = p_block;
            break;
        }
    }

    if (p_pcs_block != NULL && open3dblurayParsePgsPcs(p_pcs_block, &pcs)) {
        b_emit = true;
        if (pcs.i_forced_object_count > 0) {
            p_sys->b_forced_filter_output_active = true;
        } else {
            b_clear_only = true;
            p_sys->b_forced_filter_output_active = false;
        }
        if (open3dblurayTraceForcedFilterEnabled()) {
            msg_Info(p_demux,
                     "%s forced_filter comp=%u objects=%u forced=%u mode=%s",
                     OPEN3DBLURAY_LOG_PREFIX,
                     pcs.i_composition_number,
                     pcs.i_object_count,
                     pcs.i_forced_object_count,
                     b_clear_only ? "clear" : "forced");
        }
    } else if (open3dblurayTraceForcedFilterEnabled()) {
        msg_Warn(p_demux,
                 "%s forced_filter dropped display_set pcs_present=%d pcs_parse_ok=%d",
                 OPEN3DBLURAY_LOG_PREFIX,
                 p_pcs_block != NULL,
                 (p_pcs_block != NULL) ? open3dblurayParsePgsPcs(p_pcs_block, &pcs) : 0);
    }

    for (p_block = p_list; p_block != NULL; p_block = p_next) {
        block_t *p_out = NULL;
        p_next = p_block->p_next;
        p_block->p_next = NULL;

        if (!b_emit) {
            block_Release(p_block);
            continue;
        }

        if (p_block == p_pcs_block) {
            p_out = open3dblurayBuildPgsPcsBlock(p_block, &pcs, b_clear_only);
            block_Release(p_block);
        } else {
            p_out = p_block;
        }

        if (p_out == NULL) {
            i_ret = VLC_ENOMEM;
            continue;
        }

        if (es_out_Send(p_dst_out, p_forced_es, p_out) != VLC_SUCCESS)
            i_ret = VLC_EGENERIC;
    }

    return i_ret;
}

static int open3dbluraySendForcedPgsBlock(demux_t *p_demux,
                                          es_out_t *p_dst_out,
                                          es_out_id_t *p_forced_es,
                                          int i_pid,
                                          block_t *p_block)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_forced_es == NULL || p_block == NULL)
        return VLC_EGENERIC;

    if (p_sys->i_forced_filter_pid != i_pid)
        open3dblurayForcedFilterSelectPid(p_demux, i_pid);

    block_ChainLastAppend(&p_sys->pp_forced_filter_last, p_block);

    if (p_block->p_buffer == NULL || p_block->i_buffer < 1 || p_block->p_buffer[0] != 0x80)
        return VLC_SUCCESS;

    return open3dblurayFlushForcedPgsDisplaySet(p_demux, p_dst_out, p_forced_es);
}

static void open3dblurayRememberPendingForcedStartupSubtitle(demux_t *p_demux,
                                                             int i_pid)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    p_sys->subtitle_selection.i_pending_forced_startup_real_pid = i_pid;
}

static bool open3dblurayTakePendingForcedStartupSubtitle(demux_t *p_demux,
                                                         int i_pid)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    if (p_sys->subtitle_selection.i_pending_forced_startup_real_pid != i_pid)
        return false;

    p_sys->subtitle_selection.i_pending_forced_startup_real_pid = -1;
    return true;
}

static void open3dbluraySchedulePendingForcedSubtitleSelection(demux_t *p_demux,
                                                               int i_pid,
                                                               bool b_auto_forced_only)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    p_sys->subtitle_selection.pending_apply =
        open3dblurayMakeSubtitleSelection(i_pid, true, b_auto_forced_only);
    p_sys->subtitle_selection.b_pending_apply_valid = (i_pid > 0);
}

static bool open3dblurayTakePendingForcedSubtitleSelection(demux_t *p_demux,
                                                           int *pi_pid,
                                                           bool *pb_auto_forced_only)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const open3dbluray_subtitle_selection_t pending =
        p_sys->subtitle_selection.pending_apply;

    if (!p_sys->subtitle_selection.b_pending_apply_valid ||
        pending.i_real_pid <= 0 || !pending.b_forced_only)
        return false;

    if (pi_pid != NULL)
        *pi_pid = pending.i_real_pid;
    if (pb_auto_forced_only != NULL)
        *pb_auto_forced_only = pending.b_auto_forced_only;

    p_sys->subtitle_selection.pending_apply =
        open3dblurayMakeSubtitleSelection(-1, false, false);
    p_sys->subtitle_selection.b_pending_apply_valid = false;
    return true;
}

static bool open3dblurayHasRememberedForcedSubtitleSelection(demux_t *p_demux,
                                                             int i_pid)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const open3dbluray_subtitle_selection_t *p_selection =
        &p_sys->subtitle_selection.current;

    return p_selection->i_real_pid == i_pid &&
           p_selection->b_forced_only;
}

static bool open3dblurayHasRememberedAutoForcedSubtitleSelection(demux_t *p_demux,
                                                                 int i_pid)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const open3dbluray_subtitle_selection_t *p_selection =
        &p_sys->subtitle_selection.current;

    return p_selection->i_real_pid == i_pid &&
           p_selection->b_forced_only &&
           p_selection->b_auto_forced_only;
}

static bool open3dblurayShouldAutoPromotePreferredSubtitle(demux_t *p_demux,
                                                           int i_pid,
                                                           int i_selected_pid)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const open3dbluray_subtitle_selection_t *p_selection =
        &p_sys->subtitle_selection.current;

    if (!p_sys->subtitle_selection.b_auto_forced_default_enabled ||
        i_pid <= 0 || i_selected_pid != i_pid)
        return false;

    if (p_selection->i_real_pid < 0)
        return true;

    return p_selection->b_auto_forced_only &&
           p_selection->i_real_pid != i_pid;
}

static bool open3dblurayHasForcedSubtitleTrack(demux_t *p_demux,
                                               int i_real_spu_pid)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    if (p_sys->p_out == NULL || i_real_spu_pid <= 0)
        return false;

    bluray_esout_sys_t *esout_sys = (bluray_esout_sys_t *)p_sys->p_out->p_sys;
    const int i_forced_id = open3dblurayMakeForcedSubtitleEsId(i_real_spu_pid);
    bool b_present = false;

    vlc_mutex_lock(&esout_sys->lock);
    b_present = getEsPairByPID(&esout_sys->es, i_forced_id) != NULL;
    vlc_mutex_unlock(&esout_sys->lock);
    return b_present;
}

static void open3dblurayMaybeAutoSelectPreferredForcedSubtitle(demux_t *p_demux,
                                                               int i_real_spu_pid,
                                                               int i_selected_pid)
{
    if (!open3dblurayShouldAutoPromotePreferredSubtitle(p_demux,
                                                        i_real_spu_pid,
                                                        i_selected_pid) ||
        !open3dblurayHasForcedSubtitleTrack(p_demux, i_real_spu_pid))
        return;

    msg_Info(p_demux,
             "%s auto-selecting forced-only subtitle pid=0x%04x via preferred PG stream",
             OPEN3DBLURAY_LOG_PREFIX,
             i_real_spu_pid);
    blurayOnUserStreamSelectionEx(p_demux,
                                  open3dblurayMakeForcedSubtitleEsId(i_real_spu_pid),
                                  true);
}

static bool open3dblurayGetPreferredSubtitleStream(demux_t *p_demux,
                                                   int *pi_pid,
                                                   uint8_t *pi_offset_seq,
                                                   char psz_lang[4])
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const BLURAY_STREAM_INFO *p_stream = NULL;
    const char *psz_pref_lang;

    if (pi_pid != NULL)
        *pi_pid = -1;
    if (pi_offset_seq != NULL)
        *pi_offset_seq = 0xff;
    if (psz_lang != NULL)
        psz_lang[0] = '\0';

    if (p_sys->p_clip_info == NULL || p_sys->p_clip_info->pg_stream_count <= 0)
        return false;

    psz_pref_lang = open3dblurayGetPreferredLanguageCode(p_demux, "sub-language");
    if (open3dblurayFindPgStreamIndexByLanguage(p_sys->p_clip_info,
                                                psz_pref_lang,
                                                &p_stream) < 0 ||
        p_stream == NULL)
        return false;

    if (pi_pid != NULL)
        *pi_pid = p_stream->pid;
    if (pi_offset_seq != NULL)
        *pi_offset_seq = p_stream->pg_offset_sequence_id;
    if (psz_lang != NULL) {
        memcpy(psz_lang, p_stream->lang, 3);
        psz_lang[3] = '\0';
    }
    return true;
}

static int open3dblurayGetRememberedSubtitleUiId(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const open3dbluray_subtitle_selection_t *p_selection =
        &p_sys->subtitle_selection.current;

    if (p_selection->i_real_pid <= 0)
        return p_selection->i_real_pid;

    return p_selection->b_forced_only
         ? open3dblurayMakeForcedSubtitleEsId(p_selection->i_real_pid)
         : p_selection->i_real_pid;
}

static void open3dbluraySelectSubtitleUiId(demux_t *p_demux, int i_id)
{
    if (p_demux->p_input == NULL)
        return;

    vlc_value_t val = { .i_int = i_id };
    var_Change(p_demux->p_input, "spu-es", VLC_VAR_SETVALUE, &val, NULL);
}

static void open3dblurayInitForcedSubtitleFmt(es_format_t *p_fmt,
                                              const es_pair_t *p_real_pair,
                                              const open3dbluray_forced_pg_track_t *p_track)
{
    assert(p_real_pair != NULL);
    es_format_Copy(p_fmt, &p_real_pair->fmt);

    p_fmt->i_id = open3dblurayMakeForcedSubtitleEsId(p_track->i_pid);
    p_fmt->i_priority = ES_PRIORITY_NOT_SELECTABLE;

    free(p_fmt->psz_description);
    p_fmt->psz_description = strdup(OPEN3DBLURAY_FORCED_SUBTITLE_DESC);

    free(p_fmt->psz_language);
    p_fmt->psz_language = p_track->psz_lang[0] != '\0'
                        ? strndup(p_track->psz_lang, 3)
                        : NULL;
}

static bool open3dblurayLookupForcedSubtitleTrackInfo(demux_t *p_demux,
                                                      int i_real_pid,
                                                      open3dbluray_forced_pg_track_t *p_track)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    bool b_found = false;

    if (p_track == NULL || i_real_pid <= 0)
        return false;

    memset(p_track, 0, sizeof(*p_track));
    p_track->i_pid = i_real_pid;

    vlc_mutex_lock(&p_sys->pl_info_lock);
    for (int i = 0; p_sys->p_clip_info != NULL && i < p_sys->p_clip_info->pg_stream_count; ++i) {
        if (p_sys->p_clip_info->pg_streams[i].pid != i_real_pid)
            continue;

        memcpy(p_track->psz_lang,
               p_sys->p_clip_info->pg_streams[i].lang,
               sizeof(p_track->psz_lang) - 1);
        p_track->psz_lang[3] = '\0';
        b_found = true;
        break;
    }
    vlc_mutex_unlock(&p_sys->pl_info_lock);

    return b_found;
}

static bool open3dblurayEnsureForcedSubtitleTrackLocked(demux_t *p_demux,
                                                        bluray_esout_sys_t *esout_sys,
                                                        const es_pair_t *p_real_pair,
                                                        const open3dbluray_forced_pg_track_t *p_track,
                                                        int *pi_ui_select_id)
{
    int i_forced_id;
    es_format_t fmt;
    es_out_id_t *p_es = NULL;
    open3dbluray_forced_pg_track_t track;

    if (!open3dblurayForcedSubtitleTrackExposureEnabled())
        return false;
    if (p_real_pair == NULL || p_real_pair->fmt.i_cat != SPU_ES)
        return false;

    track = (p_track != NULL) ? *p_track : (open3dbluray_forced_pg_track_t){ 0 };
    if (track.i_pid <= 0)
        track.i_pid = p_real_pair->fmt.i_id;

    if (track.i_pid <= 0 || open3dblurayDecodeForcedSubtitleEsId(track.i_pid, NULL))
        return false;

    i_forced_id = open3dblurayMakeForcedSubtitleEsId(track.i_pid);
    if (getEsPairByPID(&esout_sys->es, i_forced_id) != NULL)
        return false;

    if (p_track == NULL && !open3dblurayLookupForcedSubtitleTrackInfo(p_demux, track.i_pid, &track))
        return false;

    es_format_Init(&fmt, UNKNOWN_ES, 0);
    open3dblurayInitForcedSubtitleFmt(&fmt, p_real_pair, &track);
    p_es = es_out_Add(esout_sys->p_dst_out, &fmt);
    if (p_es != NULL) {
        es_pair_Add(&esout_sys->es, &fmt, p_es);
        es_out_Control(esout_sys->p_dst_out, ES_OUT_SET_ES_STATE, p_es, false);
        if (pi_ui_select_id != NULL &&
            open3dblurayHasRememberedForcedSubtitleSelection(p_demux, track.i_pid)) {
            *pi_ui_select_id = open3dblurayGetRememberedSubtitleUiId(p_demux);
        }
        msg_Info(p_demux,
                 "%s added forced-only subtitle ES id=0x%08x pid=0x%04x lang=%s",
                 OPEN3DBLURAY_LOG_PREFIX,
                 i_forced_id,
                 track.i_pid,
                 track.psz_lang[0] ? track.psz_lang : "und");
    }
    es_format_Clean(&fmt);
    return p_es != NULL;
}

static void open3dblurayRemoveAllForcedSubtitleTracks(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    if (p_sys->p_out == NULL)
        return;

    bluray_esout_sys_t *esout_sys = (bluray_esout_sys_t *)p_sys->p_out->p_sys;

    vlc_mutex_lock(&esout_sys->lock);

    for (ssize_t i = (ssize_t)vlc_array_count(&esout_sys->es) - 1; i >= 0; --i) {
        es_pair_t *p_pair = vlc_array_item_at_index(&esout_sys->es, i);
        int i_real_pid = -1;

        if (!open3dblurayDecodeForcedSubtitleEsId(p_pair->fmt.i_id, &i_real_pid))
            continue;

        msg_Info(p_demux,
                 "%s removing forced-only subtitle ES id=0x%08x pid=0x%04x",
                 OPEN3DBLURAY_LOG_PREFIX,
                 p_pair->fmt.i_id,
                 i_real_pid);
        es_out_Del(esout_sys->p_dst_out, p_pair->p_es);
        es_pair_Remove(&esout_sys->es, p_pair);
    }

    vlc_mutex_unlock(&esout_sys->lock);
}

static void open3dbluraySyncForcedSubtitleTracks(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    if (!open3dblurayForcedSubtitleTrackExposureEnabled())
        return;
    if (p_sys->p_out == NULL)
        return;

    bluray_esout_sys_t *esout_sys = (bluray_esout_sys_t *)p_sys->p_out->p_sys;
    open3dbluray_forced_pg_track_t *p_tracks = NULL;
    size_t i_track_count = 0;
    int i_ui_select_id = -1;

    vlc_mutex_lock(&p_sys->pl_info_lock);
    if (p_sys->p_clip_info != NULL && p_sys->p_clip_info->pg_stream_count > 0) {
        i_track_count = p_sys->p_clip_info->pg_stream_count;
        p_tracks = calloc(i_track_count, sizeof(*p_tracks));
        if (p_tracks != NULL) {
            for (size_t i = 0; i < i_track_count; ++i) {
                p_tracks[i].i_pid = p_sys->p_clip_info->pg_streams[i].pid;
                memcpy(p_tracks[i].psz_lang,
                       p_sys->p_clip_info->pg_streams[i].lang,
                       sizeof(p_tracks[i].psz_lang));
                p_tracks[i].psz_lang[3] = '\0';
            }
        } else {
            i_track_count = 0;
        }
    }
    vlc_mutex_unlock(&p_sys->pl_info_lock);

    vlc_mutex_lock(&esout_sys->lock);

    for (size_t i = 0; i < i_track_count; ++i) {
        es_pair_t *p_real_pair = getEsPairByPID(&esout_sys->es, p_tracks[i].i_pid);
        if (p_real_pair == NULL)
            continue;

        open3dblurayEnsureForcedSubtitleTrackLocked(p_demux,
                                                   esout_sys,
                                                   p_real_pair,
                                                   &p_tracks[i],
                                                   &i_ui_select_id);
    }

    vlc_mutex_unlock(&esout_sys->lock);
    free(p_tracks);

    if (i_ui_select_id >= 0)
        open3dbluraySelectSubtitleUiId(p_demux, i_ui_select_id);
}

static void open3dblurayMaybeAddForcedSubtitleTrackLocked(demux_t *p_demux,
                                                          bluray_esout_sys_t *esout_sys,
                                                          const es_pair_t *p_real_pair)
{
    open3dblurayEnsureForcedSubtitleTrackLocked(p_demux,
                                               esout_sys,
                                               p_real_pair,
                                               NULL,
                                               NULL);
}

/*****************************************************************************
 * subpicture_updater_t functions:
 *****************************************************************************/

static bluray_overlay_t *updater_lock_overlay(subpicture_updater_sys_t *p_upd_sys)
{
    /* this lock is held while vout accesses overlay. => overlay can't be closed. */
    vlc_mutex_lock(&p_upd_sys->lock);

    bluray_overlay_t *ov = p_upd_sys->p_overlay;
    if (ov) {
        /* this lock is held while vout accesses overlay. => overlay can't be modified. */
        vlc_mutex_lock(&ov->lock);
        return ov;
    }

    /* overlay has been closed */
    vlc_mutex_unlock(&p_upd_sys->lock);
    return NULL;
}

static void updater_unlock_overlay(subpicture_updater_sys_t *p_upd_sys)
{
    assert (p_upd_sys->p_overlay);

    vlc_mutex_unlock(&p_upd_sys->p_overlay->lock);
    vlc_mutex_unlock(&p_upd_sys->lock);
}

static int subpictureUpdaterValidate(subpicture_t *p_subpic,
                                      bool b_fmt_src, const video_format_t *p_fmt_src,
                                      bool b_fmt_dst, const video_format_t *p_fmt_dst,
                                      vlc_tick_t i_ts)
{
    VLC_UNUSED(b_fmt_src);
    VLC_UNUSED(b_fmt_dst);
    VLC_UNUSED(p_fmt_src);
    VLC_UNUSED(p_fmt_dst);
    VLC_UNUSED(i_ts);

    subpicture_updater_sys_t *p_upd_sys = p_subpic->updater.p_sys;
    bluray_overlay_t         *p_overlay = updater_lock_overlay(p_upd_sys);

    if (!p_overlay) {
        return 1;
    }

    int res = p_overlay->status == Outdated;

    updater_unlock_overlay(p_upd_sys);

    return res;
}

static void subpictureUpdaterUpdate(subpicture_t *p_subpic,
                                    const video_format_t *p_fmt_src,
                                    const video_format_t *p_fmt_dst,
                                    vlc_tick_t i_ts)
{
    VLC_UNUSED(p_fmt_src);
    VLC_UNUSED(p_fmt_dst);
    VLC_UNUSED(i_ts);
    subpicture_updater_sys_t *p_upd_sys = p_subpic->updater.p_sys;
    bluray_overlay_t         *p_overlay = updater_lock_overlay(p_upd_sys);

    if (!p_overlay) {
        return;
    }

    /*
     * When this function is called, all p_subpic regions are gone.
     * We need to duplicate our regions (stored internally) to this subpic.
     */
    subpicture_region_t *p_src = p_overlay->p_regions;
    if (!p_src) {
        updater_unlock_overlay(p_upd_sys);
        return;
    }

    subpicture_region_t **p_dst = &p_subpic->p_region;
    while (p_src != NULL) {
        *p_dst = subpicture_region_Copy(p_src);
        if (*p_dst == NULL)
            break;
        open3dblurayTraceArgbUpdaterSamples(p_overlay, p_src, *p_dst);
        p_dst = &(*p_dst)->p_next;
        p_src = p_src->p_next;
    }
    if (*p_dst != NULL)
        (*p_dst)->p_next = NULL;
    p_overlay->status = Displayed;

    updater_unlock_overlay(p_upd_sys);
}

static void subpictureUpdaterDestroy(subpicture_t *p_subpic)
{
    subpicture_updater_sys_t *p_upd_sys = p_subpic->updater.p_sys;
    bluray_overlay_t         *p_overlay = updater_lock_overlay(p_upd_sys);

    if (p_overlay) {
        /* vout is closed (seek, new clip, ?). Overlay must be redrawn. */
        p_overlay->status = ToDisplay;
        p_overlay->i_channel = -1;
        updater_unlock_overlay(p_upd_sys);
    }

    unref_subpicture_updater(p_upd_sys);
}

static subpicture_t *bluraySubpictureCreate(bluray_overlay_t *p_ov)
{
    subpicture_updater_sys_t *p_upd_sys = malloc(sizeof(*p_upd_sys));
    if (unlikely(p_upd_sys == NULL)) {
        return NULL;
    }

    p_upd_sys->p_overlay = p_ov;

    subpicture_updater_t updater = {
        .pf_validate = subpictureUpdaterValidate,
        .pf_update   = subpictureUpdaterUpdate,
        .pf_destroy  = subpictureUpdaterDestroy,
        .p_sys       = p_upd_sys,
    };

    subpicture_t *p_pic = subpicture_New(&updater);
    if (p_pic == NULL) {
        free(p_upd_sys);
        return NULL;
    }

    p_pic->i_original_picture_width = p_ov->width;
    p_pic->i_original_picture_height = p_ov->height;
    p_pic->b_ephemer = true;
    p_pic->b_absolute = true;
    p_pic->b_subtitle = (p_ov->plane == BD_OVERLAY_PG);

    vlc_mutex_init(&p_upd_sys->lock);
    p_upd_sys->ref_cnt = 2;

    p_ov->p_updater = p_upd_sys;

    return p_pic;
}

/*****************************************************************************
 * User input events:
 *****************************************************************************/
static int onMouseEvent(vlc_object_t *p_vout, const char *psz_var, vlc_value_t old,
                        vlc_value_t val, void *p_data)
{
    demux_t     *p_demux = (demux_t*)p_data;
    demux_sys_t *p_sys   = p_demux->p_sys;
    open3dbluray_mouse_scene_probe_t mouse_probe;
    const int raw_x = val.coords.x;
    const int raw_y = val.coords.y;
    int clamped_x = raw_x;
    int clamped_y = raw_y;
    int mapped_x = raw_x;
    int mapped_y = raw_y;
    const bool b_ignore_mouse_move_probe =
        p_sys->b_menu &&
        open3dblurayIgnoreMouseMoveProbeEnabled();
    const bool b_mouse_scene_probe =
        p_sys->b_menu &&
        open3dblurayLoadMouseSceneProbe(&mouse_probe) &&
        open3dblurayNormalizeMouseCoords(&mouse_probe,
                                         raw_x, raw_y,
                                         &clamped_x, &clamped_y,
                                         &mapped_x, &mapped_y);
    VLC_UNUSED(old);
    VLC_UNUSED(p_vout);

    if (psz_var[6] == 'm') {  //Mouse moved
        if (open3dblurayTraceNavInputEnabled()) {
            msg_Dbg(p_demux,
                    "TRACE menu_nav source=mouse-move x=%d y=%d raw_x=%d raw_y=%d "
                    "probe_ignore_mouse_move=%d probe_mouse_source_to_scene=%d source_clamped=%d,%d source_canvas=%ux%u scene_canvas=%ux%u "
                    "menu_enabled=%d menu_open=%d popup_available=%d",
                    mapped_x, mapped_y, raw_x, raw_y,
                    b_ignore_mouse_move_probe ? 1 : 0,
                    b_mouse_scene_probe ? 1 : 0,
                    clamped_x, clamped_y,
                    b_mouse_scene_probe ? mouse_probe.source_width : 0,
                    b_mouse_scene_probe ? mouse_probe.source_height : 0,
                    b_mouse_scene_probe ? mouse_probe.scene_width : 0,
                    b_mouse_scene_probe ? mouse_probe.scene_height : 0,
                    p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
        }
        if (b_ignore_mouse_move_probe)
            return VLC_SUCCESS;
        bd_mouse_select(p_sys->bluray, -1, mapped_x, mapped_y);
    } else if (psz_var[6] == 'c') {
        msg_Info(p_demux,
                 "menu_nav source=mouse-click x=%d y=%d raw_x=%d raw_y=%d key=%s "
                 "probe_ignore_mouse_move=%d probe_mouse_source_to_scene=%d source_clamped=%d,%d source_canvas=%ux%u scene_canvas=%ux%u "
                 "menu_enabled=%d menu_open=%d popup_available=%d",
                 mapped_x, mapped_y, raw_x, raw_y,
                 open3dblurayNavKeyName(BD_VK_MOUSE_ACTIVATE),
                 b_ignore_mouse_move_probe ? 1 : 0,
                 b_mouse_scene_probe ? 1 : 0,
                 clamped_x, clamped_y,
                 b_mouse_scene_probe ? mouse_probe.source_width : 0,
                 b_mouse_scene_probe ? mouse_probe.source_height : 0,
                 b_mouse_scene_probe ? mouse_probe.scene_width : 0,
                 b_mouse_scene_probe ? mouse_probe.scene_height : 0,
                 p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
        bd_mouse_select(p_sys->bluray, -1, mapped_x, mapped_y);
        bd_user_input(p_sys->bluray, -1, BD_VK_MOUSE_ACTIVATE);
    } else {
        vlc_assert_unreachable();
    }
    return VLC_SUCCESS;
}

static int sendKeyEvent(demux_t *p_demux, demux_sys_t *p_sys, unsigned int key)
{
    msg_Info(p_demux,
             "menu_nav source=key key=%s(%u) menu_enabled=%d menu_open=%d popup_available=%d",
             open3dblurayNavKeyName(key), key,
             p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);

    if (bd_user_input(p_sys->bluray, -1, key) < 0)
        return VLC_EGENERIC;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * libbluray overlay handling:
 *****************************************************************************/

static void blurayCloseOverlay(demux_t *p_demux, int plane)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    bluray_overlay_t *ov = p_sys->p_overlays[plane];

    if (ov != NULL) {

        /* drop overlay from vout */
        if (ov->p_updater) {
            unref_subpicture_updater(ov->p_updater);
        }
        /* no references to this overlay exist in vo anymore */
        if (p_sys->p_vout && ov->i_channel != -1) {
            vout_FlushSubpictureChannel(p_sys->p_vout, ov->i_channel);
        }

        vlc_mutex_destroy(&ov->lock);
        subpicture_region_ChainDelete(ov->p_regions);
        free(ov);

        p_sys->p_overlays[plane] = NULL;
    }

    if (plane == BD_OVERLAY_PG)
        Open3DSubtitleBridgeClear(&p_sys->subtitle_bridge);
    else if (plane == BD_OVERLAY_IG)
        Open3DInteractiveGraphicsBridgeClear(&p_sys->interactive_graphics_bridge);

    for (int i = 0; i < MAX_OVERLAY; i++)
        if (p_sys->p_overlays[i])
            return;

    /* All overlays have been closed */
    blurayReleaseVout(p_demux);
}

/*
 * Mark the overlay as "ToDisplay" status.
 * This will not send the overlay to the vout instantly, as the vout
 * may not be acquired (not acquirable) yet.
 * If is has already been acquired, the overlay has already been sent to it,
 * therefore, we only flag the overlay as "Outdated"
 */
static void blurayActivateOverlay(demux_t *p_demux, int plane)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    bluray_overlay_t *ov = p_sys->p_overlays[plane];

    if(!ov)
        return;

    /*
     * If the overlay is already displayed, mark the picture as outdated.
     * We must NOT use vout_PutSubpicture if a picture is already displayed.
     */
    vlc_mutex_lock(&ov->lock);
    if (ov->status >= Displayed && p_sys->p_vout) {
        ov->status = Outdated;
        vlc_mutex_unlock(&ov->lock);
        return;
    }

    /*
     * Mark the overlay as available, but don't display it right now.
     * the blurayDemuxMenu will send it to vout, as it may be unavailable when
     * the overlay is computed
     */
    ov->status = ToDisplay;
    vlc_mutex_unlock(&ov->lock);
}

/**
 * Destroy every regions in the subpicture.
 * This is done in two steps:
 * - Wiping our private regions list
 * - Flagging the overlay as outdated, so the changes are replicated from
 *   the subpicture_updater_t::pf_update
 * This doesn't destroy the subpicture, as the overlay may be used again by libbluray.
 */
static void blurayClearOverlay(demux_t *p_demux, int plane)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    bluray_overlay_t *ov = p_sys->p_overlays[plane];

    if(!ov)
        return;

    vlc_mutex_lock(&ov->lock);

    subpicture_region_ChainDelete(ov->p_regions);
    ov->p_regions = NULL;
    ov->status = Outdated;

    vlc_mutex_unlock(&ov->lock);

    if (plane == BD_OVERLAY_PG)
        Open3DSubtitleBridgeClear(&p_sys->subtitle_bridge);
    else if (plane == BD_OVERLAY_IG)
        Open3DInteractiveGraphicsBridgeClear(&p_sys->interactive_graphics_bridge);
}

static void blurayInitOverlay(demux_t *p_demux, int plane, int width, int height)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if(p_sys->p_overlays[plane])
    {
        /* Should not happen */
        msg_Warn( p_demux, "Trying to init over an existing overlay" );
        blurayClearOverlay( p_demux, plane );
        blurayCloseOverlay( p_demux, plane );
    }

    bluray_overlay_t *ov = calloc(1, sizeof(*ov));
    if (unlikely(ov == NULL))
        return;

    ov->width = width;
    ov->height = height;
    ov->i_channel = -1;
    ov->plane = plane;
    ov->pts90k = -1;

    vlc_mutex_init(&ov->lock);

    p_sys->p_overlays[plane] = ov;

    if (plane == BD_OVERLAY_PG)
        Open3DSubtitleBridgeClear(&p_sys->subtitle_bridge);
    else if (plane == BD_OVERLAY_IG)
        Open3DInteractiveGraphicsBridgeClear(&p_sys->interactive_graphics_bridge);
}

static void blurayStoreOverlayPaletteLocked(bluray_overlay_t *ov,
                                            const BD_PG_PALETTE_ENTRY *palette)
{
    if (ov == NULL || palette == NULL)
        return;

    ov->b_palette_cache_valid = true;
    memcpy(ov->palette_cache, palette, sizeof(ov->palette_cache));
}

static void blurayApplyOverlayPaletteLocked(bluray_overlay_t *ov,
                                            subpicture_region_t *region)
{
    if (ov == NULL || region == NULL ||
        !ov->b_palette_cache_valid ||
        region->fmt.i_chroma != VLC_CODEC_YUVP ||
        region->fmt.p_palette == NULL)
        return;

    region->fmt.p_palette->i_entries = 256;
    for (int i = 0; i < 256; ++i) {
        region->fmt.p_palette->palette[i][0] = ov->palette_cache[i].Y;
        region->fmt.p_palette->palette[i][1] = ov->palette_cache[i].Cb;
        region->fmt.p_palette->palette[i][2] = ov->palette_cache[i].Cr;
        region->fmt.p_palette->palette[i][3] = ov->palette_cache[i].T;
    }
}

static void blurayApplyOverlayPaletteToRegionChainLocked(bluray_overlay_t *ov)
{
    if (ov == NULL || !ov->b_palette_cache_valid)
        return;

    for (subpicture_region_t *region = ov->p_regions;
         region != NULL; region = region->p_next)
        blurayApplyOverlayPaletteLocked(ov, region);
}

/*
 * This will draw to the overlay by adding a region to our region list
 * This will have to be copied to the subpicture used to render the overlay.
 */
static void blurayDrawOverlay(demux_t *p_demux, const BD_OVERLAY* const eventov)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    bluray_overlay_t *ov = p_sys->p_overlays[eventov->plane];
    if(!ov)
        return;

    /*
     * Compute a subpicture_region_t.
     * It will be copied and sent to the vout later.
     */
    vlc_mutex_lock(&ov->lock);

    if (eventov->palette != NULL)
        blurayStoreOverlayPaletteLocked(ov, eventov->palette);

    if (eventov->palette_update_flag)
        blurayApplyOverlayPaletteToRegionChainLocked(ov);

    /* Find a region to update */
    subpicture_region_t **pp_reg = &ov->p_regions;
    subpicture_region_t *p_reg = ov->p_regions;
    subpicture_region_t *p_last = NULL;
    while (p_reg != NULL) {
        p_last = p_reg;
        if (p_reg->i_x == eventov->x &&
            p_reg->i_y == eventov->y &&
            p_reg->fmt.i_width == eventov->w &&
            p_reg->fmt.i_height == eventov->h &&
            p_reg->fmt.i_chroma == VLC_CODEC_YUVP)
            break;
        pp_reg = &p_reg->p_next;
        p_reg = p_reg->p_next;
    }

    if (!eventov->img) {
        if (eventov->palette_update_flag) {
            vlc_mutex_unlock(&ov->lock);
            return;
        }
        if (p_reg) {
            /* drop region */
            *pp_reg = p_reg->p_next;
            subpicture_region_Delete(p_reg);
        }
        vlc_mutex_unlock(&ov->lock);
        return;
    }

    /* If there is no region to update, create a new one. */
    if (!p_reg) {
        video_format_t fmt;
        video_format_Init(&fmt, 0);
        video_format_Setup(&fmt, VLC_CODEC_YUVP, eventov->w, eventov->h, eventov->w, eventov->h, 1, 1);

        p_reg = subpicture_region_New(&fmt);
        if (p_reg) {
            p_reg->i_x = eventov->x;
            p_reg->i_y = eventov->y;
            if (p_reg->p_picture != NULL) {
                plane_t *plane = &p_reg->p_picture->p[0];
                memset(plane->p_pixels, 0xff, plane->i_pitch * plane->i_lines);
            }
            /* Append it to our list. */
            if (p_last != NULL)
                p_last->p_next = p_reg;
            else /* If we don't have a last region, then our list empty */
                ov->p_regions = p_reg;
        }
        else
        {
            vlc_mutex_unlock(&ov->lock);
            return;
        }
    }

    /* Now we can update the region, regardless it's an update or an insert */
    const BD_PG_RLE_ELEM *img = eventov->img;
    for (int y = 0; y < eventov->h; y++)
        for (int x = 0; x < eventov->w;) {
            plane_t *p = &p_reg->p_picture->p[0];
            memset(&p->p_pixels[y * p->i_pitch + x], img->color, img->len);
            x += img->len;
            img++;
        }

    blurayApplyOverlayPaletteLocked(ov, p_reg);

    vlc_mutex_unlock(&ov->lock);
    /*
     * /!\ The region is now stored in our internal list, but not in the subpicture /!\
     */
}

static void blurayOverlayProc(void *ptr, const BD_OVERLAY *const overlay)
{
    demux_t *p_demux = (demux_t*)ptr;
    demux_sys_t *p_sys = p_demux->p_sys;

    if (!overlay) {
        msg_Info(p_demux, "Closing overlays.");
        if (p_sys->p_vout)
            for (int i = 0; i < MAX_OVERLAY; i++)
                blurayCloseOverlay(p_demux, i);
        return;
    }

    if(overlay->plane >= MAX_OVERLAY)
        return;

    switch (overlay->cmd) {
    case BD_OVERLAY_INIT:
        msg_Info(p_demux, "Initializing overlay");
        vlc_mutex_lock(&p_sys->bdj_overlay_lock);
        blurayInitOverlay(p_demux, overlay->plane, overlay->w, overlay->h);
        vlc_mutex_unlock(&p_sys->bdj_overlay_lock);
        break;
    case BD_OVERLAY_CLOSE:
        vlc_mutex_lock(&p_sys->bdj_overlay_lock);
        blurayClearOverlay(p_demux, overlay->plane);
        blurayCloseOverlay(p_demux, overlay->plane);
        vlc_mutex_unlock(&p_sys->bdj_overlay_lock);
        break;
    case BD_OVERLAY_CLEAR:
        blurayClearOverlay(p_demux, overlay->plane);
        break;
    case BD_OVERLAY_FLUSH:
        if (p_sys->p_overlays[overlay->plane]) {
            bluray_overlay_t *ov = p_sys->p_overlays[overlay->plane];

            vlc_mutex_lock(&ov->lock);
            ov->pts90k = overlay->pts;
            ov->b_subtitle_offset_valid = false;

            if (ov->plane == BD_OVERLAY_PG &&
                p_sys->b_selected_pg_offset_sequence_valid) {
                BD_OPEN3D_PG_OFFSET offset;
                if (bd_open3d_mvc_get_pg_offset(p_sys->bluray,
                                               p_sys->i_selected_pg_offset_sequence_id,
                                               overlay->pts, &offset)) {
                    ov->b_subtitle_offset_valid = true;
                    ov->i_subtitle_offset_sequence = offset.offset_sequence_id;
                    ov->i_subtitle_offset_raw = offset.raw_offset;
                    ov->i_subtitle_offset_signed = offset.signed_offset;
                    ov->i_subtitle_offset_frame = offset.frame_index;
                }
            }

            if (ov->plane == BD_OVERLAY_PG)
                open3dblurayTracePgRegionChain(p_demux, "flush", ov);

            open3dbluraySyncDirectOverlayBridgeFromOverlayLocked(p_demux, ov);
            open3dblurayPublishSubtitleOffsetToVout(p_demux, ov);
            vlc_mutex_unlock(&ov->lock);
            if (open3dblurayUseDirectOverlayBridge(p_demux, ov))
                open3dblurayDetachOverlaySubpictureFromVout(p_demux, ov);
        }
        blurayActivateOverlay(p_demux, overlay->plane);
        break;
    case BD_OVERLAY_DRAW:
    case BD_OVERLAY_WIPE:
        blurayDrawOverlay(p_demux, overlay);
        break;
    default:
        msg_Warn(p_demux, "Unknown BD overlay command: %u", overlay->cmd);
        break;
    }
}

/* ARGB in word order -> byte order */
#ifdef WORDS_BIG_ENDIAN
  #define ARGB_OVERLAY_CHROMA VLC_CODEC_ARGB
#else
  #define ARGB_OVERLAY_CHROMA VLC_CODEC_BGRA
#endif

/*
 * ARGB overlay (BD-J)
 */
static void blurayInitArgbOverlay(demux_t *p_demux, int plane, int width, int height)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    blurayInitOverlay(p_demux, plane, width, height);

    if (!p_sys->p_overlays[plane]->p_regions) {
        video_format_t fmt;
        video_format_Init(&fmt, 0);
        video_format_Setup(&fmt, ARGB_OVERLAY_CHROMA, width, height, width, height, 1, 1);

        p_sys->p_overlays[plane]->p_regions = subpicture_region_New(&fmt);
    }
}

static subpicture_region_t *blurayRecreateArgbOverlayRegionLocked(
    demux_t *p_demux,
    bluray_overlay_t *ov,
    const BD_ARGB_OVERLAY *eventov,
    const char *reason)
{
    if (p_demux == NULL || ov == NULL)
        return NULL;

    unsigned width = ov->width > 0 ? (unsigned)ov->width : 0;
    unsigned height = ov->height > 0 ? (unsigned)ov->height : 0;

    if (eventov != NULL)
    {
        const unsigned draw_width = (unsigned)eventov->x + (unsigned)eventov->w;
        const unsigned draw_height = (unsigned)eventov->y + (unsigned)eventov->h;
        if (draw_width > width)
            width = draw_width;
        if (draw_height > height)
            height = draw_height;
    }

    if (width == 0 || height == 0)
        return NULL;

    subpicture_region_ChainDelete(ov->p_regions);
    ov->p_regions = NULL;

    video_format_t fmt;
    video_format_Init(&fmt, 0);
    video_format_Setup(&fmt, ARGB_OVERLAY_CHROMA,
                       width, height, width, height, 1, 1);

    subpicture_region_t *p_reg = subpicture_region_New(&fmt);
    video_format_Clean(&fmt);
    if (p_reg == NULL)
        return NULL;

    p_reg->i_x = 0;
    p_reg->i_y = 0;

    if (p_reg->p_picture != NULL)
    {
        for (int i = 0; i < p_reg->p_picture->i_planes; ++i)
        {
            plane_t *plane = &p_reg->p_picture->p[i];
            memset(plane->p_pixels, 0, plane->i_pitch * plane->i_lines);
        }
    }

    ov->p_regions = p_reg;

    msg_Warn(p_demux,
             "open3dbluray trace argb-region-recreate reason=%s plane=%u canvas=%ux%u rect=%u,%u-%u,%u",
             reason != NULL ? reason : "unspecified",
             (unsigned)ov->plane,
             width,
             height,
             eventov != NULL ? (unsigned)eventov->x : 0,
             eventov != NULL ? (unsigned)eventov->y : 0,
             eventov != NULL ? (unsigned)(eventov->x + eventov->w - 1) : 0,
             eventov != NULL ? (unsigned)(eventov->y + eventov->h - 1) : 0);
    return p_reg;
}

static void blurayDrawArgbOverlay(demux_t *p_demux, const BD_ARGB_OVERLAY* const eventov)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    bluray_overlay_t *ov = p_sys->p_overlays[eventov->plane];
    if(!ov)
    {
        open3dblurayTraceArgbSkip(p_demux, NULL, eventov, NULL, "no-overlay");
        return;
    }

    vlc_mutex_lock(&ov->lock);

    /* Find a region to update */
    subpicture_region_t *p_reg = ov->p_regions;
    if (!p_reg) {
        p_reg = blurayRecreateArgbOverlayRegionLocked(p_demux, ov, eventov,
                                                      "no-region");
        if (!p_reg) {
            open3dblurayTraceArgbSkip(p_demux, ov, eventov, NULL, "no-region");
            vlc_mutex_unlock(&ov->lock);
            return;
        }
    }

    if (p_reg->fmt.i_chroma != ARGB_OVERLAY_CHROMA) {
        p_reg = blurayRecreateArgbOverlayRegionLocked(p_demux, ov, eventov,
                                                      "bad-chroma");
        if (p_reg == NULL || p_reg->fmt.i_chroma != ARGB_OVERLAY_CHROMA) {
            open3dblurayTraceArgbSkip(p_demux, ov, eventov, p_reg, "bad-chroma");
            vlc_mutex_unlock(&ov->lock);
            return;
        }
    }

    if (eventov->x + eventov->w > p_reg->fmt.i_width ||
        eventov->y + eventov->h > p_reg->fmt.i_height) {
        p_reg = blurayRecreateArgbOverlayRegionLocked(p_demux, ov, eventov,
                                                      "bounds");
        if (p_reg == NULL ||
            eventov->x + eventov->w > p_reg->fmt.i_width ||
            eventov->y + eventov->h > p_reg->fmt.i_height) {
            open3dblurayTraceArgbSkip(p_demux, ov, eventov, p_reg, "bounds");
            vlc_mutex_unlock(&ov->lock);
            return;
        }
    }

    /* Now we can update the region */
    const uint32_t *src0 = eventov->argb;
    uint8_t        *dst0 = p_reg->p_picture->p[0].p_pixels +
                           p_reg->p_picture->p[0].i_pitch * eventov->y +
                           eventov->x * 4;
    /* always true as for now, see bd_bdj_osd_cb */
    if(likely(eventov->stride == p_reg->p_picture->p[0].i_pitch))
    {
        memcpy(dst0, src0, (eventov->stride * eventov->h - eventov->x)*4);
    }
    else
    {
        for(uint16_t h = 0; h < eventov->h; h++)
        {
            memcpy(dst0, src0, eventov->w *4);
            dst0 = dst0 + p_reg->p_picture->p[0].i_pitch;
            src0 = src0 + eventov->stride;
        }
    }

    open3dblurayTraceArgbAccessSamples(p_demux, ov, eventov, p_reg);

    vlc_mutex_unlock(&ov->lock);
    /*
     * /!\ The region is now stored in our internal list, but not in the subpicture /!\
     */
}

static void blurayArgbOverlayProc(void *ptr, const BD_ARGB_OVERLAY *const overlay)
{
    demux_t *p_demux = (demux_t*)ptr;
    demux_sys_t *p_sys = p_demux->p_sys;

    open3dblurayTraceArgbEntry(p_demux, p_sys, overlay);

    if(overlay->plane >= MAX_OVERLAY)
        return;

    switch (overlay->cmd) {
    case BD_ARGB_OVERLAY_INIT:
        vlc_mutex_lock(&p_sys->bdj_overlay_lock);
        blurayInitArgbOverlay(p_demux, overlay->plane, overlay->w, overlay->h);
        vlc_mutex_unlock(&p_sys->bdj_overlay_lock);
        break;
    case BD_ARGB_OVERLAY_CLOSE:
        vlc_mutex_lock(&p_sys->bdj_overlay_lock);
        blurayClearOverlay(p_demux, overlay->plane);
        blurayCloseOverlay(p_demux, overlay->plane);
        vlc_mutex_unlock(&p_sys->bdj_overlay_lock);
        break;
    case BD_ARGB_OVERLAY_FLUSH:
        if (p_sys->p_overlays[overlay->plane]) {
            bluray_overlay_t *ov = p_sys->p_overlays[overlay->plane];
            const bool trace_overlay = open3dblurayTraceInteractiveGraphicsBridgeEnabled();
            bool use_direct_bridge = false;

            vlc_mutex_lock(&ov->lock);
            open3dbluraySyncDirectOverlayBridgeFromOverlayLocked(p_demux, ov);
            use_direct_bridge = open3dblurayUseDirectOverlayBridge(p_demux, ov);
            if (trace_overlay)
            {
                if (overlay->plane == BD_OVERLAY_IG)
                    open3dblurayTraceIgRegionChain(p_demux, "overlay-flush", ov,
                                                   use_direct_bridge,
                                                   p_sys->b_menu_open);
                msg_Dbg(p_demux,
                        "open3dbluray trace overlay flush plane=%u regions=%u size=%ux%u direct=%d",
                        (unsigned)overlay->plane,
                        open3dblurayCountRegionChain(ov->p_regions),
                        ov->width,
                        ov->height,
                        use_direct_bridge ? 1 : 0);
            }
            vlc_mutex_unlock(&ov->lock);
            if (use_direct_bridge)
            {
                open3dblurayDetachOverlaySubpictureFromVout(p_demux, ov);
                if (trace_overlay && overlay->plane == BD_OVERLAY_IG)
                    msg_Dbg(p_demux,
                            "open3dbluray trace ig-bridge detach plane=%u channel=%d",
                            (unsigned)overlay->plane,
                            ov->i_channel);
            }
        }
        blurayActivateOverlay(p_demux, overlay->plane);
        break;
    case BD_ARGB_OVERLAY_DRAW:
        blurayDrawArgbOverlay(p_demux, overlay);
        break;
    default:
        msg_Warn(p_demux, "Unknown BD ARGB overlay command: %u", overlay->cmd);
        break;
    }
}

static void bluraySendOverlayToVout(demux_t *p_demux, bluray_overlay_t *p_ov)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool trace_overlay = open3dblurayTraceInteractiveGraphicsBridgeEnabled();
    const unsigned region_count = open3dblurayCountRegionChain(p_ov ? p_ov->p_regions : NULL);

    assert(p_ov != NULL);
    assert(p_ov->i_channel == -1);

    if (p_ov->p_updater) {
        unref_subpicture_updater(p_ov->p_updater);
        p_ov->p_updater = NULL;
    }

    subpicture_t *p_pic = bluraySubpictureCreate(p_ov);
    if (!p_pic) {
        msg_Err(p_demux, "bluraySubpictureCreate() failed");
        return;
    }

    if (p_ov->pts90k > 0)
        p_pic->i_start = p_pic->i_stop = FROM_SCALE(p_ov->pts90k);
    else
        p_pic->i_start = p_pic->i_stop = mdate();
    p_pic->i_channel = vout_RegisterSubpictureChannel(p_sys->p_vout);
    p_ov->i_channel = p_pic->i_channel;
    if (trace_overlay && p_ov->plane == BD_OVERLAY_IG)
        msg_Dbg(p_demux,
                "open3dbluray trace overlay send plane=%u regions=%u pts90k=%" PRId64 " channel=%d",
                (unsigned)p_ov->plane,
                region_count,
                p_ov->pts90k,
                p_ov->i_channel);

    /*
     * After this point, the picture should not be accessed from the demux thread,
     * as it is held by the vout thread.
     * This must be done only once per subpicture, ie. only once between each
     * blurayInitOverlay & blurayCloseOverlay call.
     */
    vout_PutSubpicture(p_sys->p_vout, p_pic);

    /*
     * Mark the picture as Outdated, as it contains no region for now.
     * This will make the subpicture_updater_t call pf_update
     */
    p_ov->status = Outdated;
}

static bool blurayTitleIsRepeating(BLURAY_TITLE_INFO *title_info,
                                   unsigned repeats, unsigned ratio)
{
#if BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 0, 0)
    const BLURAY_CLIP_INFO *prev = NULL;
    unsigned maxrepeats = 0;
    unsigned sequence = 0;
    if(!title_info->chapter_count)
        return false;

    for (unsigned int j = 0; j < title_info->chapter_count; j++)
    {
        unsigned i = title_info->chapters[j].clip_ref;
        if(i < title_info->clip_count)
        {
            if(prev == NULL ||
               /* non repeated does not need start time offset */
               title_info->clips[i].start_time == 0 ||
               /* repeats occurs on same segment */
               memcmp(title_info->clips[i].clip_id, prev->clip_id, 6) ||
               prev->in_time != title_info->clips[i].in_time ||
               prev->pkt_count != title_info->clips[i].pkt_count)
            {
                sequence = 0;
                prev = &title_info->clips[i];
                continue;
            }
            else
            {
                if(maxrepeats < sequence++)
                    maxrepeats = sequence;
            }
        }
    }
    return (maxrepeats > repeats &&
            (100 * maxrepeats / title_info->chapter_count) >= ratio);
#else
    return false;
#endif
}

static void blurayUpdateTitleInfo(input_title_t *t, BLURAY_TITLE_INFO *title_info)
{
    t->i_length = FROM_SCALE_NZ(title_info->duration);

    for (int i = 0; i < t->i_seekpoint; i++)
        vlc_seekpoint_Delete( t->seekpoint[i] );
    TAB_CLEAN(t->i_seekpoint, t->seekpoint);

    /* FIXME: have libbluray expose repeating titles */
    if(blurayTitleIsRepeating(title_info, 50, 90))
        return;

    for (unsigned int j = 0; j < title_info->chapter_count; j++) {
        seekpoint_t *s = vlc_seekpoint_New();
        if (!s) {
            break;
        }
        s->i_time_offset = FROM_SCALE_NZ(title_info->chapters[j].start);

        TAB_APPEND(t->i_seekpoint, t->seekpoint, s);
    }
}

static void blurayInitTitles(demux_t *p_demux, uint32_t menu_titles)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const BLURAY_DISC_INFO *di = bd_get_disc_info(p_sys->bluray);

    /* get and set the titles */
    uint32_t i_title = menu_titles;

    if (!p_sys->b_menu) {
        i_title = bd_get_titles(p_sys->bluray, TITLES_RELEVANT, 60);
        p_sys->i_longest_title = bd_get_main_title(p_sys->bluray);
    }

    for (uint32_t i = 0; i < i_title; i++) {
        input_title_t *t = vlc_input_title_New();
        if (!t)
            break;

        if (!p_sys->b_menu) {
            BLURAY_TITLE_INFO *title_info = bd_get_title_info(p_sys->bluray, i, 0);
            blurayUpdateTitleInfo(t, title_info);
            bd_free_title_info(title_info);

        } else if (i == 0) {
            t->psz_name = strdup(_("Top Menu"));
            t->i_flags = INPUT_TITLE_MENU | INPUT_TITLE_INTERACTIVE;
        } else if (i == i_title - 1) {
            t->psz_name = strdup(_("First Play"));
            if (di && di->first_play && di->first_play->interactive) {
                t->i_flags = INPUT_TITLE_INTERACTIVE;
            }
        } else {
            /* add possible title name from disc metadata */
            if (di && di->titles && i <= di->num_titles) {
                if (di->titles[i]->name) {
                    t->psz_name = strdup(di->titles[i]->name);
                }
                if (di->titles[i]->interactive) {
                    t->i_flags = INPUT_TITLE_INTERACTIVE;
                }
            }
        }

        TAB_APPEND(p_sys->i_title, p_sys->pp_title, t);
    }
}

static void blurayRestartParser(demux_t *p_demux, bool b_flush, bool b_random_access)
{
    /*
     * This is a hack and will have to be removed.
     * The parser should be flushed, and not destroy/created each time
     * we are changing title.
     */
    demux_sys_t *p_sys = p_demux->p_sys;
    open3dblurayForcedFilterInvalidate(p_demux);

    if(b_flush)
        es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_DISABLE_OUTPUT);

    if (p_sys->p_parser)
        vlc_demux_chained_Delete(p_sys->p_parser);

    if(b_flush)
        es_out_Control(p_sys->p_tf_out, ES_OUT_TF_FILTER_RESET);

    p_sys->p_parser = vlc_demux_chained_New(VLC_OBJECT(p_demux), "ts", p_sys->p_out);
    if (!p_sys->p_parser)
        msg_Err(p_demux, "Failed to create TS demuxer");

    es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_ENABLE_OUTPUT);

    es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_RANDOM_ACCESS, b_random_access);
}

/*****************************************************************************
 * bluraySetTitle: select new BD title
 *****************************************************************************/
static int bluraySetTitle(demux_t *p_demux, int i_title)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->b_menu) {
        int result;
        if (i_title <= 0) {
            msg_Dbg(p_demux, "Playing TopMenu Title");
            result = bd_menu_call(p_sys->bluray, -1);
        } else if (i_title >= (int)p_sys->i_title - 1) {
            msg_Dbg(p_demux, "Playing FirstPlay Title");
            result = bd_play_title(p_sys->bluray, BLURAY_TITLE_FIRST_PLAY);
        } else {
            msg_Dbg(p_demux, "Playing Title %i", i_title);
            result = bd_play_title(p_sys->bluray, i_title);
        }

        if (result == 0) {
            msg_Err(p_demux, "cannot play bd title '%d'", i_title);
            return VLC_EGENERIC;
        }

        return VLC_SUCCESS;
    }

    /* Looking for the main title, ie the longest duration */
    if (i_title < 0)
        i_title = p_sys->i_longest_title;
    else if ((unsigned)i_title > p_sys->i_title)
        return VLC_EGENERIC;

    msg_Dbg(p_demux, "Selecting Title %i", i_title);

    if (bd_select_title(p_sys->bluray, i_title) == 0) {
        msg_Err(p_demux, "cannot select bd title '%d'", i_title);
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

#if BLURAY_VERSION < BLURAY_VERSION_CODE(0,9,2)
#  define BLURAY_AUDIO_STREAM 0
#endif

static void blurayOnUserStreamSelectionEx(demux_t *p_demux,
                                          int i_pid,
                                          bool b_auto_forced_only)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    int i_forced_pid = -1;
    const bool b_forced_only = open3dblurayDecodeForcedSubtitleEsId(i_pid, &i_forced_pid);
    const bool b_explicit_spu_selection = (i_pid == -SPU_ES) ||
                                          b_forced_only ||
                                          ((i_pid & 0xff00) == 0x1200) ||
                                          i_pid == 0x1800;

    if (!b_auto_forced_only && b_explicit_spu_selection)
        p_sys->subtitle_selection.b_auto_forced_default_enabled = false;

    vlc_mutex_lock(&p_sys->pl_info_lock);

    if(i_pid == -AUDIO_ES)
        bd_select_stream(p_sys->bluray, BLURAY_AUDIO_STREAM, 0, 0);
    else if(i_pid == -SPU_ES) {
        open3dblurayRememberSubtitleSelection(p_demux, -1, false);
        open3dblurayForcedFilterReset(p_demux);
        open3dblurayUpdateSelectedSubtitleOffsetUnlocked(p_demux, -1);
        bd_select_stream(p_sys->bluray, BLURAY_PG_TEXTST_STREAM, 0, 0);
    } else if (b_forced_only && p_sys->p_clip_info) {
        const BLURAY_STREAM_INFO *p_stream = NULL;
        const int i_id = open3dblurayFindPgStreamIndexByPid(p_sys->p_clip_info,
                                                            i_forced_pid,
                                                            &p_stream);
        if (i_id >= 0 && p_stream != NULL) {
            open3dblurayRememberSubtitleSelectionEx(p_demux,
                                                    i_forced_pid,
                                                    true,
                                                    b_auto_forced_only);
            open3dblurayForcedFilterSelectPid(p_demux, i_forced_pid);
            open3dblurayUpdateSelectedSubtitleOffsetUnlocked(p_demux, i_forced_pid);
            bd_select_stream(p_sys->bluray, BLURAY_PG_TEXTST_STREAM, i_id + 1, 1);
            open3dblurayApplyMenulessPgLanguageSetting(p_demux, p_stream);
            msg_Info(p_demux,
                     "%s selected forced-only subtitle pid=0x%04x lang=%s playlist_index=%d synthetic_es=0x%08x mode=filtered_pgs",
                     OPEN3DBLURAY_LOG_PREFIX,
                     i_forced_pid,
                     (const char *)p_stream->lang,
                     i_id,
                     open3dblurayMakeForcedSubtitleEsId(i_forced_pid));
            open3dbluraySelectSubtitleUiId(p_demux,
                                          open3dblurayMakeForcedSubtitleEsId(i_forced_pid));
        } else {
            msg_Warn(p_demux, "%s forced-only subtitle selection pid=0x%04x not found in current playlist",
                     OPEN3DBLURAY_LOG_PREFIX, i_forced_pid);
        }
    } else if (p_sys->p_clip_info)
    {
        if ((i_pid & 0xff00) == 0x1100) {
            bool b_in_playlist = false;
            // audio
            for (int i_id = 0; i_id < p_sys->p_clip_info->audio_stream_count; i_id++) {
                if (i_pid == p_sys->p_clip_info->audio_streams[i_id].pid) {
                    bd_select_stream(p_sys->bluray, BLURAY_AUDIO_STREAM, i_id + 1, 1);

                    if(!p_sys->b_menu)
                        bd_set_player_setting_str(p_sys->bluray, BLURAY_PLAYER_SETTING_AUDIO_LANG,
                                  (const char *) p_sys->p_clip_info->audio_streams[i_id].lang);
                    b_in_playlist = true;
                    break;
                }
            }
            if(!b_in_playlist && !p_sys->b_menu)
            {
                /* Without menu, the selected playlist might not be correct and only
                   exposing a subset of PID, although same length */
                msg_Warn(p_demux, "Incorrect playlist for menuless track, forcing");
                es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_SET_ES_BY_PID,
                               BD_EVENT_AUDIO_STREAM, i_pid);
            }
        } else if ((i_pid & 0xff00) == 0x1200 || i_pid == 0x1800) {
            const BLURAY_STREAM_INFO *p_stream = NULL;
            const int i_id = open3dblurayFindPgStreamIndexByPid(p_sys->p_clip_info,
                                                                i_pid,
                                                                &p_stream);
            if (i_id >= 0 && p_stream != NULL) {
                open3dblurayRememberSubtitleSelectionEx(p_demux,
                                                        i_pid,
                                                        false,
                                                        false);
                open3dblurayForcedFilterReset(p_demux);
                open3dblurayUpdateSelectedSubtitleOffsetUnlocked(p_demux, i_pid);
                bd_select_stream(p_sys->bluray, BLURAY_PG_TEXTST_STREAM, i_id + 1, 1);
                open3dblurayApplyMenulessPgLanguageSetting(p_demux, p_stream);
            } else if (!p_sys->b_menu)
            {
                msg_Warn(p_demux, "Incorrect playlist for menuless track, forcing");
                es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_SET_ES_BY_PID,
                               BD_EVENT_PG_TEXTST_STREAM, i_pid);
            }
        }
    }

    vlc_mutex_unlock(&p_sys->pl_info_lock);
}

static void blurayOnUserStreamSelection(demux_t *p_demux, int i_pid)
{
    blurayOnUserStreamSelectionEx(p_demux, i_pid, false);
}

static void open3dblurayApplyStartupSubtitlePreference(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    int i_selected_spu_pid = -1;
    int i_pref_pid = -1;
    uint8_t i_pref_offset_seq = 0xff;
    char sz_pref_lang[4] = "";

    if (!p_sys->p_clip_info || p_sys->p_clip_info->pg_stream_count <= 0) {
        return;
    }

    const int i_track_id = var_InheritInteger(p_demux, "sub-track-id");
    if (i_track_id >= 0) {
        p_sys->subtitle_selection.b_auto_forced_default_enabled = false;
        msg_Dbg(p_demux,
                "open3dbluraymvc startup subtitle preference: sub-track-id=%d",
                i_track_id);
        int i_forced_pid = -1;
        if (open3dblurayDecodeForcedSubtitleEsId(i_track_id, &i_forced_pid)) {
            open3dblurayRememberPendingForcedStartupSubtitle(p_demux, i_forced_pid);
            return;
        }
        blurayOnUserStreamSelection(p_demux, i_track_id);
        return;
    }

    const int i_track = var_InheritInteger(p_demux, "sub-track");
    if (i_track >= 0 && i_track < p_sys->p_clip_info->pg_stream_count) {
        p_sys->subtitle_selection.b_auto_forced_default_enabled = false;
        msg_Dbg(p_demux,
                "open3dbluraymvc startup subtitle preference: sub-track=%d pid=0x%04x lang=%s offset_seq=%u",
                i_track,
                p_sys->p_clip_info->pg_streams[i_track].pid,
                (const char *)p_sys->p_clip_info->pg_streams[i_track].lang,
                p_sys->p_clip_info->pg_streams[i_track].pg_offset_sequence_id);
        blurayOnUserStreamSelection(p_demux, p_sys->p_clip_info->pg_streams[i_track].pid);
        return;
    }

    if (p_sys->b_menu) {
        p_sys->subtitle_selection.b_auto_forced_default_enabled = false;
        msg_Dbg(p_demux,
                "%s startup subtitle preference: menu mode active, leaving subtitle selection to disc menu",
                OPEN3DBLURAY_LOG_PREFIX);
        return;
    }

    p_sys->subtitle_selection.b_auto_forced_default_enabled =
        open3dblurayForcedSubtitleTrackExposureEnabled();
    if (!p_sys->subtitle_selection.b_auto_forced_default_enabled)
        return;

    if (open3dblurayGetPreferredSubtitleStream(p_demux,
                                               &i_pref_pid,
                                               &i_pref_offset_seq,
                                               sz_pref_lang)) {
        msg_Dbg(p_demux,
                "%s startup subtitle preference: auto forced-only preferred_lang=%s pid=0x%04x offset_seq=%u",
                OPEN3DBLURAY_LOG_PREFIX,
                sz_pref_lang,
                i_pref_pid,
                i_pref_offset_seq);
        if (open3dblurayHasForcedSubtitleTrack(p_demux, i_pref_pid)) {
            open3dblurayMaybeAutoSelectPreferredForcedSubtitle(p_demux,
                                                               i_pref_pid,
                                                               i_pref_pid);
        } else {
            open3dblurayRememberPendingForcedStartupSubtitle(p_demux, i_pref_pid);
            open3dblurayRememberSubtitleSelectionEx(p_demux,
                                                    i_pref_pid,
                                                    true,
                                                    true);
        }
        return;
    }

    if (p_sys->p_out != NULL) {
        bluray_esout_sys_t *esout_sys = (bluray_esout_sys_t *)p_sys->p_out->p_sys;
        vlc_mutex_lock(&esout_sys->lock);
        i_selected_spu_pid = esout_sys->selected.i_spu_pid;
        vlc_mutex_unlock(&esout_sys->lock);
    }

    if (i_selected_spu_pid > 0) {
        msg_Dbg(p_demux,
                "%s startup subtitle preference: auto forced-only candidate pid=0x%04x",
                OPEN3DBLURAY_LOG_PREFIX,
                i_selected_spu_pid);
        open3dblurayMaybeAutoSelectPreferredForcedSubtitle(p_demux,
                                                           i_selected_spu_pid,
                                                           i_selected_spu_pid);
    } else {
        msg_Dbg(p_demux,
                "%s startup subtitle preference: auto forced-only pending preferred PG stream",
                OPEN3DBLURAY_LOG_PREFIX);
    }
}

/*****************************************************************************
 * blurayControl: handle the controls
 *****************************************************************************/
static int blurayControl(demux_t *p_demux, int query, va_list args)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    bool     *pb_bool;
    int64_t  *pi_64;

    switch (query) {
    case DEMUX_CAN_SEEK:
    case DEMUX_CAN_PAUSE:
    case DEMUX_CAN_CONTROL_PACE:
         pb_bool = va_arg(args, bool *);
         *pb_bool = true;
         break;

    case DEMUX_GET_PTS_DELAY:
        pi_64 = va_arg(args, int64_t *);
        *pi_64 = INT64_C(1000) * var_InheritInteger(p_demux, "disc-caching");
        break;

    case DEMUX_SET_PAUSE_STATE:
    {
#ifdef BLURAY_RATE_NORMAL
        bool b_paused = (bool)va_arg(args, int);
        if (bd_set_rate(p_sys->bluray, BLURAY_RATE_NORMAL * (!b_paused)) < 0) {
            return VLC_EGENERIC;
        }
#endif
        break;
    }
    case DEMUX_SET_ES:
    {
        int i_id = va_arg(args, int);
        blurayOnUserStreamSelection(p_demux, i_id);
        break;
    }
    case DEMUX_SET_TITLE:
    {
        int i_title = va_arg(args, int);
        if (bluraySetTitle(p_demux, i_title) != VLC_SUCCESS) {
            /* make sure GUI restores the old setting in title menu ... */
            p_demux->info.i_update |= INPUT_UPDATE_TITLE | INPUT_UPDATE_SEEKPOINT;
            return VLC_EGENERIC;
        }
        blurayRestartParser(p_demux, true, false);
        notifyDiscontinuityToParser(p_sys);
        p_sys->b_draining = false;
        es_out_Control(p_demux->out, ES_OUT_RESET_PCR);
        es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_FLAG_DISCONTINUITY);
        break;
    }
    case DEMUX_SET_SEEKPOINT:
    {
        int i_chapter = va_arg(args, int);
        open3dblurayStartSeekTimingTrace(p_demux, "set_seekpoint");
        bd_seek_chapter(p_sys->bluray, i_chapter);
        blurayRestartParser(p_demux, true, false);
        notifyDiscontinuityToParser(p_sys);
        p_sys->b_draining = false;
        es_out_Control(p_demux->out, ES_OUT_RESET_PCR);
        p_demux->info.i_update |= INPUT_UPDATE_SEEKPOINT;
        break;
    }

    case DEMUX_GET_TITLE_INFO:
    {
        input_title_t ***ppp_title = va_arg(args, input_title_t***);
        int *pi_int             = va_arg(args, int *);
        int *pi_title_offset    = va_arg(args, int *);
        int *pi_chapter_offset  = va_arg(args, int *);

        /* */
        *pi_title_offset   = 0;
        *pi_chapter_offset = 0;

        /* Duplicate local title infos */
        *pi_int = 0;
        *ppp_title = vlc_alloc(p_sys->i_title, sizeof(input_title_t *));
        if(!*ppp_title)
            return VLC_EGENERIC;
        for (unsigned int i = 0; i < p_sys->i_title; i++)
        {
            input_title_t *p_dup = vlc_input_title_Duplicate(p_sys->pp_title[i]);
            if(p_dup)
                (*ppp_title)[(*pi_int)++] = p_dup;
        }

        return VLC_SUCCESS;
    }

    case DEMUX_GET_LENGTH:
    {
        int64_t *pi_length = va_arg(args, int64_t *);
        if(p_demux->info.i_title < (int) p_sys->i_title &&
           (CURRENT_TITLE->i_flags & INPUT_TITLE_INTERACTIVE))
                return VLC_EGENERIC;
        *pi_length = p_demux->info.i_title < (int) p_sys->i_title ? CUR_LENGTH : 0;
        return VLC_SUCCESS;
    }
    case DEMUX_SET_TIME:
    {
        int64_t i_time = va_arg(args, int64_t);
        open3dblurayStartSeekTimingTrace(p_demux, "set_time");
        bd_seek_time(p_sys->bluray, TO_SCALE_NZ(i_time));
        blurayRestartParser(p_demux, true, true);
        notifyDiscontinuityToParser(p_sys);
        p_sys->b_draining = false;
        es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_FLAG_DISCONTINUITY);
        return VLC_SUCCESS;
    }
    case DEMUX_GET_TIME:
    {
        int64_t *pi_time = va_arg(args, int64_t *);
        if( p_demux->info.i_title < (int) p_sys->i_title &&
           (CURRENT_TITLE->i_flags & INPUT_TITLE_INTERACTIVE))
                return VLC_EGENERIC;
        *pi_time = FROM_SCALE_NZ(bd_tell_time(p_sys->bluray));
        return VLC_SUCCESS;
    }

    case DEMUX_GET_POSITION:
    {
        double *pf_position = va_arg(args, double *);
        if(p_demux->info.i_title < (int) p_sys->i_title &&
           (CURRENT_TITLE->i_flags & INPUT_TITLE_INTERACTIVE))
                return VLC_EGENERIC;
        *pf_position = p_demux->info.i_title < (int) p_sys->i_title && CUR_LENGTH > 0 ?
                      (double)FROM_SCALE_NZ(bd_tell_time(p_sys->bluray))/CUR_LENGTH : 0.0;
        return VLC_SUCCESS;
    }
    case DEMUX_SET_POSITION:
    {
        double f_position = va_arg(args, double);
        open3dblurayStartSeekTimingTrace(p_demux, "set_position");
        bd_seek_time(p_sys->bluray, TO_SCALE_NZ(f_position*CUR_LENGTH));
        blurayRestartParser(p_demux, true, true);
        notifyDiscontinuityToParser(p_sys);
        p_sys->b_draining = false;
        es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_FLAG_DISCONTINUITY);
        return VLC_SUCCESS;
    }

    case DEMUX_GET_META:
    {
        vlc_meta_t *p_meta = va_arg(args, vlc_meta_t *);
        const META_DL *meta = p_sys->p_meta;
        if (meta == NULL)
            return VLC_EGENERIC;

        if (!EMPTY_STR(meta->di_name)) vlc_meta_SetTitle(p_meta, meta->di_name);

        if (!EMPTY_STR(meta->language_code)) vlc_meta_AddExtra(p_meta, "Language", meta->language_code);
        if (!EMPTY_STR(meta->filename)) vlc_meta_AddExtra(p_meta, "Filename", meta->filename);
        if (!EMPTY_STR(meta->di_alternative)) vlc_meta_AddExtra(p_meta, "Alternative", meta->di_alternative);

        // if (meta->di_set_number > 0) vlc_meta_SetTrackNum(p_meta, meta->di_set_number);
        // if (meta->di_num_sets > 0) vlc_meta_AddExtra(p_meta, "Discs numbers in Set", meta->di_num_sets);

        if (p_sys->i_cover_idx >= 0 && p_sys->i_cover_idx < p_sys->i_attachments) {
            char psz_url[128];
            snprintf( psz_url, sizeof(psz_url), "attachment://%s",
                      p_sys->attachments[p_sys->i_cover_idx]->psz_name );
            vlc_meta_Set( p_meta, vlc_meta_ArtworkURL, psz_url );
        }
        else if (meta->thumb_count > 0 && meta->thumbnails && p_sys->psz_bd_path) {
            char *psz_thumbpath;
            if (asprintf(&psz_thumbpath, "%s" DIR_SEP "BDMV" DIR_SEP "META" DIR_SEP "DL" DIR_SEP "%s",
                          p_sys->psz_bd_path, meta->thumbnails[0].path) > -1) {
                char *psz_thumburl = vlc_path2uri(psz_thumbpath, "file");
                free(psz_thumbpath);
                if (unlikely(psz_thumburl == NULL))
                    return VLC_ENOMEM;

                vlc_meta_SetArtURL(p_meta, psz_thumburl);
                free(psz_thumburl);
            }
        }

        return VLC_SUCCESS;
    }

    case DEMUX_GET_ATTACHMENTS:
    {
        input_attachment_t ***ppp_attach =
            va_arg(args, input_attachment_t ***);
        int *pi_int = va_arg(args, int *);

        if (p_sys->i_attachments <= 0)
            return VLC_EGENERIC;

        *pi_int = 0;
        *ppp_attach = vlc_alloc(p_sys->i_attachments, sizeof(input_attachment_t *));
        if(!*ppp_attach)
            return VLC_EGENERIC;
        for (int i = 0; i < p_sys->i_attachments; i++)
        {
            input_attachment_t *p_dup = vlc_input_attachment_Duplicate(p_sys->attachments[i]);
            if(p_dup)
                (*ppp_attach)[(*pi_int)++] = p_dup;
        }
        return VLC_SUCCESS;
    }

    case DEMUX_NAV_ACTIVATE:
        if (p_sys->b_popup_available && !p_sys->b_menu_open) {
            msg_Info(p_demux,
                     "menu_nav source=control action=%s route=%s menu_enabled=%d menu_open=%d popup_available=%d",
                     open3dblurayNavActionName(query), open3dblurayNavKeyName(BD_VK_POPUP),
                     p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
            return sendKeyEvent(p_demux, p_sys, BD_VK_POPUP);
        }
        msg_Info(p_demux,
                 "menu_nav source=control action=%s route=%s menu_enabled=%d menu_open=%d popup_available=%d",
                 open3dblurayNavActionName(query), open3dblurayNavKeyName(BD_VK_ENTER),
                 p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
        return sendKeyEvent(p_demux, p_sys, BD_VK_ENTER);
    case DEMUX_NAV_UP:
        msg_Info(p_demux,
                 "menu_nav source=control action=%s route=%s menu_enabled=%d menu_open=%d popup_available=%d",
                 open3dblurayNavActionName(query), open3dblurayNavKeyName(BD_VK_UP),
                 p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
        return sendKeyEvent(p_demux, p_sys, BD_VK_UP);
    case DEMUX_NAV_DOWN:
        msg_Info(p_demux,
                 "menu_nav source=control action=%s route=%s menu_enabled=%d menu_open=%d popup_available=%d",
                 open3dblurayNavActionName(query), open3dblurayNavKeyName(BD_VK_DOWN),
                 p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
        return sendKeyEvent(p_demux, p_sys, BD_VK_DOWN);
    case DEMUX_NAV_LEFT:
        msg_Info(p_demux,
                 "menu_nav source=control action=%s route=%s menu_enabled=%d menu_open=%d popup_available=%d",
                 open3dblurayNavActionName(query), open3dblurayNavKeyName(BD_VK_LEFT),
                 p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
        return sendKeyEvent(p_demux, p_sys, BD_VK_LEFT);
    case DEMUX_NAV_RIGHT:
        msg_Info(p_demux,
                 "menu_nav source=control action=%s route=%s menu_enabled=%d menu_open=%d popup_available=%d",
                 open3dblurayNavActionName(query), open3dblurayNavKeyName(BD_VK_RIGHT),
                 p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
        return sendKeyEvent(p_demux, p_sys, BD_VK_RIGHT);
    case DEMUX_NAV_POPUP:
        msg_Info(p_demux,
                 "menu_nav source=control action=%s route=%s menu_enabled=%d menu_open=%d popup_available=%d",
                 open3dblurayNavActionName(query), open3dblurayNavKeyName(BD_VK_POPUP),
                 p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
        return sendKeyEvent(p_demux, p_sys, BD_VK_POPUP);
    case DEMUX_NAV_MENU:
        if (p_sys->b_menu) {
            if (bd_menu_call(p_sys->bluray, -1) == 1) {
                msg_Info(p_demux,
                         "menu_nav source=control action=%s route=top_menu_call menu_enabled=%d menu_open=%d popup_available=%d",
                         open3dblurayNavActionName(query),
                         p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
                p_demux->info.i_update |= INPUT_UPDATE_TITLE | INPUT_UPDATE_SEEKPOINT;
                return VLC_SUCCESS;
            }
            msg_Err(p_demux, "Can't select Top Menu title");
            msg_Info(p_demux,
                     "menu_nav source=control action=%s route=%s fallback=top_menu_failed menu_enabled=%d menu_open=%d popup_available=%d",
                     open3dblurayNavActionName(query), open3dblurayNavKeyName(BD_VK_POPUP),
                     p_sys->b_menu, p_sys->b_menu_open, p_sys->b_popup_available);
            return sendKeyEvent(p_demux, p_sys, BD_VK_POPUP);
        }
        return VLC_EGENERIC;

    case DEMUX_CAN_RECORD:
    case DEMUX_GET_FPS:
    case DEMUX_SET_GROUP:
    case DEMUX_HAS_UNSUPPORTED_META:
    default:
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * libbluray event handling
 *****************************************************************************/
static void writeTsPacketWDiscontinuity( uint8_t *p_buf, uint16_t i_pid,
                                         const uint8_t *p_payload, uint8_t i_payload )
{
    uint8_t ts_header[] = {
        0x00, 0x00, 0x00, 0x00,                /* TP extra header (ATC) */
        0x47,
        0x40 | ((i_pid & 0x1f00) >> 8), i_pid & 0xFF, /* PUSI + PID */
        i_payload ? 0x30 : 0x20,               /* adaptation field, payload / no payload */
        192 - (4 + 5) - i_payload,             /* adaptation field length */
        0x82,                                  /* af: discontinuity indicator + priv data */
        0x0E,                                  /* priv data size */
         'V',  'L',  'C',  '_',
         'D',  'I',  'S',  'C',  'O',  'N',  'T',  'I',  'N',  'U',
    };

    memcpy( p_buf, ts_header, sizeof(ts_header) );
    memset( &p_buf[sizeof(ts_header)], 0xFF, 192 - sizeof(ts_header) - i_payload );
    if( i_payload )
        memcpy( &p_buf[192 - i_payload], p_payload, i_payload );
}

static void notifyStreamsDiscontinuity( vlc_demux_chained_t *p_parser,
                                        const BLURAY_STREAM_INFO *p_sinfo, size_t i_sinfo )
{
    for( size_t i=0; i< i_sinfo; i++ )
    {
        const uint16_t i_pid = p_sinfo[i].pid;

        block_t *p_block = block_Alloc(192);
        if (!p_block)
            return;

        writeTsPacketWDiscontinuity( p_block->p_buffer, i_pid, NULL, 0 );

        vlc_demux_chained_Send(p_parser, p_block);
    }
}

#define DONOTIFY(memb) notifyStreamsDiscontinuity( p_sys->p_parser, p_clip->memb##_streams, \
                                                   p_clip->memb##_stream_count )

static void notifyDiscontinuityToParser( demux_sys_t *p_sys )
{
    const BLURAY_CLIP_INFO *p_clip = p_sys->p_clip_info;
    if( p_clip )
    {
        DONOTIFY(audio);
        DONOTIFY(video);
        DONOTIFY(pg);
        DONOTIFY(ig);
        DONOTIFY(sec_audio);
        DONOTIFY(sec_video);
    }
}

#undef DONOTIFY

static void streamFlush( demux_sys_t *p_sys )
{
    /*
     * MPEG-TS demuxer does not flush last video frame if size of PES packet is unknown.
     * Packet is flushed only when TS packet with PUSI flag set is received.
     *
     * Fix this by emitting (video) ts packet with PUSI flag set.
     * Add video sequence end code to payload so that also video decoder is flushed.
     * Set PES packet size in the payload so that it will be sent to decoder immediately.
     */

    if (p_sys->b_flushed)
        return;

    block_t *p_block = block_Alloc(192);
    if (!p_block)
        return;

    bd_stream_type_e i_coding_type;

    /* set correct sequence end code */
    vlc_mutex_lock(&p_sys->pl_info_lock);
    if (p_sys->p_clip_info != NULL)
        i_coding_type = p_sys->p_clip_info->video_streams[0].coding_type;
    else
        i_coding_type = 0;
    vlc_mutex_unlock(&p_sys->pl_info_lock);

    uint8_t i_eos;
    switch( i_coding_type )
    {
        case BLURAY_STREAM_TYPE_VIDEO_MPEG1:
        case BLURAY_STREAM_TYPE_VIDEO_MPEG2:
        default:
            i_eos = 0xB7; /* MPEG2 sequence end */
            break;
        case BLURAY_STREAM_TYPE_VIDEO_VC1:
        case BLURAY_STREAM_TYPE_VIDEO_H264:
            i_eos = 0x0A; /* VC1 / H.264 sequence end */
            break;
        case BD_STREAM_TYPE_VIDEO_HEVC:
            i_eos = 0x48; /* HEVC sequence end NALU */
            break;
    }

    uint8_t seq_end_pes[] = {
        0x00, 0x00, 0x01, 0xe0, 0x00, 0x07, 0x80, 0x00, 0x00,  /* PES header */
        0x00, 0x00, 0x01, i_eos,                               /* PES payload: sequence end */
        0x00, /* 2nd byte for HEVC NAL, pads others */
    };

    writeTsPacketWDiscontinuity( p_block->p_buffer, 0x1011, seq_end_pes, sizeof(seq_end_pes) );

    vlc_demux_chained_Send(p_sys->p_parser, p_block);
    p_sys->b_flushed = true;
}

static void blurayResetStillImage( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->i_still_end_time != STILL_IMAGE_NOT_SET) {
        p_sys->i_still_end_time = STILL_IMAGE_NOT_SET;

        blurayRestartParser(p_demux, false, false);
        es_out_Control( p_demux->out, ES_OUT_RESET_PCR );
    }
}

static bool open3dblurayStillImageActive(demux_t *p_demux)
{
    return p_demux != NULL &&
           p_demux->p_sys != NULL &&
           p_demux->p_sys->i_still_end_time != STILL_IMAGE_NOT_SET;
}

static void blurayStillImage( demux_t *p_demux, unsigned i_timeout )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    /* time period elapsed ? */
    if (p_sys->i_still_end_time != STILL_IMAGE_NOT_SET &&
        p_sys->i_still_end_time != STILL_IMAGE_INFINITE &&
        p_sys->i_still_end_time <= mdate()) {
        msg_Dbg(p_demux, "Still image end");
        bd_read_skip_still(p_sys->bluray);

        blurayResetStillImage(p_demux);
        return;
    }

    /* show last frame as still image */
    if (p_sys->i_still_end_time == STILL_IMAGE_NOT_SET) {
        if (i_timeout) {
            msg_Dbg(p_demux, "Still image (%d seconds)", i_timeout);
            p_sys->i_still_end_time = mdate() + i_timeout * CLOCK_FREQ;
        } else {
            msg_Dbg(p_demux, "Still image (infinite)");
            p_sys->i_still_end_time = STILL_IMAGE_INFINITE;
        }

        /* flush demuxer and decoder (there won't be next video packet starting with ts PUSI) */
        streamFlush(p_sys);

        /* stop buffering */
        bool b_empty;
        es_out_Control( p_demux->out, ES_OUT_GET_EMPTY, &b_empty );
    }

    /* avoid busy loops (read returns no data) */
    msleep( 40000 );
}

static void blurayOnStreamSelectedEvent(demux_t *p_demux, uint32_t i_type, uint32_t i_id)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    int i_pid = -1;

    /* The param we get is the real stream id, not an index, ie. it starts from 1 */
    i_id--;

    if (i_type == BD_EVENT_AUDIO_STREAM) {
        i_pid = blurayGetStreamPID(p_sys, i_type, i_id);
    } else if (i_type == BD_EVENT_PG_TEXTST_STREAM) {
        i_pid = blurayGetStreamPID(p_sys, i_type, i_id);
    }

    if (i_pid > 0)
    {
        if (i_type == BD_EVENT_PG_TEXTST_STREAM)
        {
            if (open3dblurayShouldAutoPromotePreferredSubtitle(p_demux, i_pid, i_pid)) {
                open3dblurayMaybeAutoSelectPreferredForcedSubtitle(p_demux,
                                                                   i_pid,
                                                                   i_pid);
                if (open3dblurayHasRememberedForcedSubtitleSelection(p_demux, i_pid))
                    return;
            }

            open3dblurayRememberSubtitleSelectionEx(p_demux,
                                                    i_pid,
                                                    open3dblurayHasRememberedForcedSubtitleSelection(p_demux,
                                                                                                    i_pid),
                                                    open3dblurayHasRememberedAutoForcedSubtitleSelection(p_demux,
                                                                                                         i_pid));
            open3dblurayUpdateSelectedSubtitleOffsetUnlocked(p_demux, i_pid);
            if (!p_sys->b_spu_enable)
                es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_UNSET_ES_BY_PID, (int)i_type, i_pid);
            else
                es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_SET_ES_BY_PID, (int)i_type, i_pid);
            if (p_sys->b_spu_enable)
                open3dbluraySelectSubtitleUiId(p_demux,
                                               open3dblurayGetRememberedSubtitleUiId(p_demux));
        }
        else
        {
            if (i_type == BD_EVENT_AUDIO_STREAM)
                open3dblurayLogSelectedAudioStreamUnlocked(p_demux, i_pid, "event");
            es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_SET_ES_BY_PID, (int)i_type, i_pid);
        }
    }
}

static void blurayUpdatePlaylist(demux_t *p_demux, unsigned i_playlist)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    blurayRestartParser(p_demux, true, false);
    open3dblurayRemoveAllForcedSubtitleTracks(p_demux);

    /* read title info and init some values */
    if (!p_sys->b_menu)
        p_demux->info.i_title = bd_get_current_title(p_sys->bluray);
    p_demux->info.i_seekpoint = 0;
    p_demux->info.i_update |= INPUT_UPDATE_TITLE | INPUT_UPDATE_SEEKPOINT;

    BLURAY_TITLE_INFO *p_title_info = bd_get_playlist_info(p_sys->bluray, i_playlist, 0);
    if (p_title_info) {
        blurayUpdateTitleInfo(p_sys->pp_title[p_demux->info.i_title], p_title_info);
        if (p_sys->b_menu)
            p_demux->info.i_update |= INPUT_UPDATE_TITLE_LIST;
    }
    setTitleInfo(p_sys, p_title_info);
    open3dbluraySyncForcedSubtitleTracks(p_demux);

    blurayResetStillImage(p_demux);
}

static bool open3dblurayTraceClipStateEnabled(void)
{
    const char *env = getenv("OPEN3DBLURAY_TRACE_CLIP_STATE");
    return env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
}

static void blurayOnClipUpdate(demux_t *p_demux, uint32_t clip)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool b_trace_clip_state = open3dblurayTraceClipStateEnabled();

    vlc_mutex_lock(&p_sys->pl_info_lock);

    p_sys->p_clip_info = NULL;

    if (p_sys->p_pl_info && clip < p_sys->p_pl_info->clip_count) {

        p_sys->p_clip_info = &p_sys->p_pl_info->clips[clip];

    /* Let's assume a single video track for now.
     * This may brake later, but it's enough for now.
     */
        assert(p_sys->p_clip_info->video_stream_count >= 1);
    }

    CLPI_CL *clpi = bd_get_clpi(p_sys->bluray, clip);
    if (b_trace_clip_state && p_sys->p_clip_info != NULL) {
        const BLURAY_CLIP_INFO *p_clip = p_sys->p_clip_info;
        const BLURAY_STREAM_INFO *p_video0 = p_clip->video_stream_count > 0
                                           ? &p_clip->video_streams[0]
                                           : NULL;
        const unsigned i_playlist = p_sys->p_pl_info != NULL
                                  ? p_sys->p_pl_info->playlist
                                  : 0;
        const unsigned i_actual_app = clpi != NULL ? clpi->clip.application_type : 0;

        fprintf(stderr,
                "open3dbluray raw clip-state playlist=%u clip_ref=%u clip_id=%.5s "
                "tracked_app=%s(%u) clpi_app=%s(%u) still=%s(%u) still_time=%u "
                "pkt_count=%u start=%" PRIu64 " in=%" PRIu64 " out=%" PRIu64 " "
                "video=%u audio=%u pg=%u ig=%u sec_video=%u sec_audio=%u "
                "v0_pid=%u v0_type=%s(0x%02x) v0_fmt=%u v0_rate=%u v0_aspect=%u\n",
                i_playlist,
                clip,
                p_clip->clip_id,
                open3dblurayClipAppTypeName((unsigned)p_sys->clip_application_type),
                (unsigned)p_sys->clip_application_type,
                open3dblurayClipAppTypeName(i_actual_app),
                i_actual_app,
                open3dblurayStillModeName((unsigned)p_clip->still_mode),
                (unsigned)p_clip->still_mode,
                (unsigned)p_clip->still_time,
                (unsigned)p_clip->pkt_count,
                p_clip->start_time,
                p_clip->in_time,
                p_clip->out_time,
                (unsigned)p_clip->video_stream_count,
                (unsigned)p_clip->audio_stream_count,
                (unsigned)p_clip->pg_stream_count,
                (unsigned)p_clip->ig_stream_count,
                (unsigned)p_clip->sec_video_stream_count,
                (unsigned)p_clip->sec_audio_stream_count,
                p_video0 != NULL ? (unsigned)p_video0->pid : 0u,
                p_video0 != NULL ? open3dblurayStreamCodingTypeName((unsigned)p_video0->coding_type)
                                 : "none",
                p_video0 != NULL ? (unsigned)p_video0->coding_type : 0u,
                p_video0 != NULL ? (unsigned)p_video0->format : 0u,
                p_video0 != NULL ? (unsigned)p_video0->rate : 0u,
                p_video0 != NULL ? (unsigned)p_video0->aspect : 0u);
    } else if (b_trace_clip_state) {
        const unsigned i_playlist = p_sys->p_pl_info != NULL
                                  ? p_sys->p_pl_info->playlist
                                  : 0;
        const unsigned i_actual_app = clpi != NULL ? clpi->clip.application_type : 0;

        fprintf(stderr,
                "open3dbluray raw clip-state playlist=%u clip_ref=%u clip_info=missing "
                "tracked_app=%s(%u) clpi_app=%s(%u)\n",
                i_playlist,
                clip,
                open3dblurayClipAppTypeName((unsigned)p_sys->clip_application_type),
                (unsigned)p_sys->clip_application_type,
                open3dblurayClipAppTypeName(i_actual_app),
                i_actual_app);
    }

    if(clpi && clpi->clip.application_type != p_sys->clip_application_type)
    {
        if(p_sys->clip_application_type == BD_CLIP_APP_TYPE_TS_MAIN_PATH_TIMED_SLIDESHOW ||
           clpi->clip.application_type == BD_CLIP_APP_TYPE_TS_MAIN_PATH_TIMED_SLIDESHOW)
            blurayRestartParser(p_demux, false, false);

        if(clpi->clip.application_type == BD_CLIP_APP_TYPE_TS_MAIN_PATH_TIMED_SLIDESHOW)
            es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_ENABLE_LOW_DELAY);
        else
            es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_DISABLE_LOW_DELAY);
        bd_free_clpi(clpi);
    }

    vlc_mutex_unlock(&p_sys->pl_info_lock);

    open3dbluraySyncForcedSubtitleTracks(p_demux);
    open3dblurayApplyStartupSubtitlePreference(p_demux);

    blurayResetStillImage(p_demux);
}

static bool open3dblurayShouldTraceBdEvent(uint32_t event)
{
    switch ((bd_event_e)event) {
    case BD_EVENT_TITLE:
    case BD_EVENT_PLAYLIST:
    case BD_EVENT_PLAYITEM:
    case BD_EVENT_SEEK:
    case BD_EVENT_PLAYLIST_STOP:
    case BD_EVENT_DISCONTINUITY:
    case BD_EVENT_STILL:
    case BD_EVENT_STILL_TIME:
    case BD_EVENT_END_OF_TITLE:
    case BD_EVENT_IDLE:
    case BD_EVENT_POPUP:
    case BD_EVENT_MENU:
    case BD_EVENT_STEREOSCOPIC_STATUS:
        return true;
    default:
        return false;
    }
}

static void open3dblurayLogBdEventState(demux_t *p_demux, const char *psz_phase,
                                        const BD_EVENT *e, bool b_delayed)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    unsigned i_playlist = 0;
    unsigned i_clip_app = 0;

    vlc_mutex_lock(&p_sys->pl_info_lock);
    if (p_sys->p_pl_info != NULL)
        i_playlist = p_sys->p_pl_info->playlist;
    i_clip_app = (unsigned)p_sys->clip_application_type;
    vlc_mutex_unlock(&p_sys->pl_info_lock);

    const char *psz_name = bd_event_name((uint32_t)e->event);

    msg_Info(p_demux,
             "%s trace bd-event phase=%s name=%s(%d) param=%d delayed=%d "
             "title=%d seekpoint=%d playlist=%u clip_app=%s(%u) "
             "pl_playing=%d menu=%d menu_open=%d popup=%d draining=%d still_active=%d",
             OPEN3DBLURAY_LOG_PREFIX,
             psz_phase,
             psz_name != NULL ? psz_name : "UNKNOWN",
             e->event,
             e->param,
             b_delayed ? 1 : 0,
             p_demux->info.i_title,
             p_demux->info.i_seekpoint,
             i_playlist,
             open3dblurayClipAppTypeName(i_clip_app),
             i_clip_app,
             p_sys->b_pl_playing ? 1 : 0,
             p_sys->b_menu ? 1 : 0,
             p_sys->b_menu_open ? 1 : 0,
             p_sys->b_popup_available ? 1 : 0,
             p_sys->b_draining ? 1 : 0,
             p_sys->i_still_end_time != STILL_IMAGE_NOT_SET ? 1 : 0);
}

static void open3dblurayLogProbeRedundantEndOfTitleYield(demux_t *p_demux,
                                                         const BD_EVENT *e)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    unsigned i_playlist = 0;

    vlc_mutex_lock(&p_sys->pl_info_lock);
    if (p_sys->p_pl_info != NULL)
        i_playlist = p_sys->p_pl_info->playlist;
    vlc_mutex_unlock(&p_sys->pl_info_lock);

    msg_Warn(p_demux,
             "%s probe redundant_end_of_title_yield count=%u title=%d seekpoint=%d "
             "playlist=%u param=%d menu=%d menu_open=%d popup=%d draining=%d",
             OPEN3DBLURAY_LOG_PREFIX,
             p_sys->i_probe_redundant_end_of_title_yield_count,
             p_demux->info.i_title,
             p_demux->info.i_seekpoint,
             i_playlist,
             e != NULL ? e->param : 0,
             p_sys->b_menu ? 1 : 0,
             p_sys->b_menu_open ? 1 : 0,
             p_sys->b_popup_available ? 1 : 0,
             p_sys->b_draining ? 1 : 0);
}

static void open3dblurayLogRedundantEndOfTitleStorm(demux_t *p_demux,
                                                    const BD_EVENT *e,
                                                    unsigned i_streak)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    unsigned i_playlist = 0;

    vlc_mutex_lock(&p_sys->pl_info_lock);
    if (p_sys->p_pl_info != NULL)
        i_playlist = p_sys->p_pl_info->playlist;
    vlc_mutex_unlock(&p_sys->pl_info_lock);

    msg_Warn(p_demux,
             "%s fix redundant_end_of_title_storm streak=%u title=%d seekpoint=%d "
             "playlist=%u param=%d menu=%d menu_open=%d popup=%d draining=%d",
             OPEN3DBLURAY_LOG_PREFIX,
             i_streak,
             p_demux->info.i_title,
             p_demux->info.i_seekpoint,
             i_playlist,
             e != NULL ? e->param : 0,
             p_sys->b_menu ? 1 : 0,
             p_sys->b_menu_open ? 1 : 0,
             p_sys->b_popup_available ? 1 : 0,
             p_sys->b_draining ? 1 : 0);
}

static void open3dblurayHandleInterruptedPlaylistStop(demux_t *p_demux,
                                                      const char *reason)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (!p_sys->b_pl_playing)
        return;

    /* BD_EVENT_PLAYLIST_STOP is the authoritative interruption boundary for
     * menu-driven playlist hops. Handle the parser/PCR flush there so the
     * following PLAYLIST event does not force a second avoidable restart. */
    msg_Info(p_demux, "Stopping playlist playback reason=%s",
             reason != NULL ? reason : "unspecified");
    blurayRestartParser(p_demux, false, false);
    es_out_Control(p_demux->out, ES_OUT_RESET_PCR);

    if (p_sys->b_menu_open)
    {
        if (open3dblurayShouldPreserveMenuStateOnPlaylistTransition(p_demux))
        {
            const bool still_active =
                open3dblurayStillImageActive(p_demux);
            const bool ig_regions =
                open3dblurayInteractiveGraphicsOverlayHasRegions(p_demux);
            const bool ig_overlay_present =
                open3dblurayInteractiveGraphicsOverlayPresent(p_demux);
            msg_Info(p_demux,
                     "menu_state event=playlist-transition preserve=1 menu_open=%d still_active=%d ig_regions=%d ig_overlay_present=%d",
                     p_sys->b_menu_open ? 1 : 0,
                     still_active ? 1 : 0,
                     ig_regions ? 1 : 0,
                     ig_overlay_present ? 1 : 0);
        }
        else
        {
            open3dblurayClearActiveMenuState(p_demux, "playlist_transition");
        }
    }

    p_sys->b_pl_playing = false;
    p_sys->b_draining = false;
}

static void blurayHandleEvent(demux_t *p_demux, const BD_EVENT *e, bool b_delayed)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (open3dblurayShouldTraceBdEvent((uint32_t)e->event))
        open3dblurayLogBdEventState(p_demux, "enter", e, b_delayed);

    switch (e->event) {
    case BD_EVENT_TITLE:
        if (e->param == BLURAY_TITLE_FIRST_PLAY)
            p_demux->info.i_title = p_sys->i_title - 1;
        else
            p_demux->info.i_title = e->param;
        /* this is feature title, we don't know yet which playlist it will play (if any) */
        setTitleInfo(p_sys, NULL);
        /* reset title infos here ? */
        p_demux->info.i_update |= INPUT_UPDATE_TITLE | INPUT_UPDATE_SEEKPOINT; /* might be BD-J title with no video */
        open3dblurayLogBdEventState(p_demux, "after", e, b_delayed);
        break;
    case BD_EVENT_PLAYLIST:
        /* Start of playlist playback (?????.mpls) */
        blurayUpdatePlaylist(p_demux, e->param);
        if (p_sys->b_pl_playing) {
            open3dblurayHandleInterruptedPlaylistStop(
                p_demux, "playlist_event_without_stop");
        }
        p_sys->b_pl_playing = true;
        open3dblurayLogBdEventState(p_demux, "after", e, b_delayed);
        break;
    case BD_EVENT_PLAYLIST_STOP:
        open3dblurayHandleInterruptedPlaylistStop(p_demux, "playlist_stop");
        open3dblurayLogBdEventState(p_demux, "after", e, b_delayed);
        break;
    case BD_EVENT_PLAYITEM:
        notifyDiscontinuityToParser(p_sys);
        blurayOnClipUpdate(p_demux, e->param);
        open3dblurayLogBdEventState(p_demux, "after", e, b_delayed);
        break;
    case BD_EVENT_CHAPTER:
        if (e->param && e->param < 0xffff)
          p_demux->info.i_seekpoint = e->param - 1;
        else
          p_demux->info.i_seekpoint = 0;
        p_demux->info.i_update |= INPUT_UPDATE_SEEKPOINT;
        break;
    case BD_EVENT_PLAYMARK:
    case BD_EVENT_ANGLE:
        break;
    case BD_EVENT_SEEK:
        /* Seek will happen with any chapter/title or bd_seek(),
           but also BD-J initiated. We can't make the difference
           between input or vm ones, better double flush/pcr reset
           than break the clock by throwing post random access PCR */
        open3dblurayStartSeekTimingTrace(p_demux, "bd_event_seek");
        blurayRestartParser(p_demux, true, true);
        notifyDiscontinuityToParser(p_sys);
        es_out_Control(p_sys->p_out, ES_OUT_RESET_PCR);
        es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_FLAG_DISCONTINUITY);
        break;
#if BLURAY_VERSION >= BLURAY_VERSION_CODE(0,8,1)
    case BD_EVENT_UO_MASK_CHANGED:
        /* This event could be used to grey out unselectable items in title menu */
        break;
#endif
    case BD_EVENT_MENU:
        msg_Warn(p_demux,
                 "%s menu_event_handle param=%d delayed=%d before_open=%d popup_available=%d",
                 OPEN3DBLURAY_LOG_PREFIX,
                 e->param,
                 b_delayed ? 1 : 0,
                 p_sys->b_menu_open ? 1 : 0,
                 p_sys->b_popup_available ? 1 : 0);
        p_sys->b_menu_open = e->param;
        if (p_sys->b_menu_open)
            open3dblurayAttachDirectBridgesToVout(p_demux);
        open3dblurayPublishMenuOpenState(p_demux);
        open3dblurayPublishForceMonoMenuState(p_demux);
        msg_Info(p_demux, "menu_state event=menu open=%d popup_available=%d",
                 p_sys->b_menu_open, p_sys->b_popup_available);
        open3dblurayLogBdEventState(p_demux, "after", e, b_delayed);
        break;
    case BD_EVENT_POPUP:
        p_sys->b_popup_available = e->param;
        open3dblurayPublishPopupAvailableState(p_demux);
        msg_Info(p_demux, "menu_state event=popup available=%d menu_open=%d",
                 p_sys->b_popup_available, p_sys->b_menu_open);
        open3dblurayLogBdEventState(p_demux, "after", e, b_delayed);
        /* TODO: show / hide pop-up menu button in gui ? */
        break;

    /*
     * Errors
     */
    case BD_EVENT_ERROR:
        /* fatal error (with menus) */
        vlc_dialog_display_error(p_demux, _("Blu-ray error"),
                                 "Playback with BluRay menus failed");
        p_sys->b_fatal_error = true;
        break;
    case BD_EVENT_ENCRYPTED:
        vlc_dialog_display_error(p_demux, _("Blu-ray error"),
                                 "This disc seems to be encrypted");
        p_sys->b_fatal_error = true;
        break;
    case BD_EVENT_READ_ERROR:
        msg_Err(p_demux, "bluray: read error\n");
        break;

    /*
     * stream selection events
     */
    case BD_EVENT_PG_TEXTST:
        p_sys->b_spu_enable = e->param;
        break;
    case BD_EVENT_AUDIO_STREAM:
    case BD_EVENT_PG_TEXTST_STREAM:
         if(b_delayed)
             blurayOnStreamSelectedEvent(p_demux, e->event, e->param);
         else
             ARRAY_APPEND(p_sys->events_delayed, *e);
        break;
    case BD_EVENT_IG_STREAM:
    case BD_EVENT_SECONDARY_AUDIO:
    case BD_EVENT_SECONDARY_AUDIO_STREAM:
    case BD_EVENT_SECONDARY_VIDEO:
    case BD_EVENT_SECONDARY_VIDEO_STREAM:
    case BD_EVENT_SECONDARY_VIDEO_SIZE:
        break;

    /*
     * playback control events
     */
    case BD_EVENT_STILL:
    case BD_EVENT_STILL_TIME:
        blurayStillImage(p_demux, e->param);
        open3dblurayLogBdEventState(p_demux, "after", e, b_delayed);
        break;
    case BD_EVENT_DISCONTINUITY:
        /* reset demuxer (partially decoded PES packets must be dropped) */
        blurayRestartParser(p_demux, false, true);
        es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_FLAG_DISCONTINUITY);
        open3dblurayLogBdEventState(p_demux, "after", e, b_delayed);
        break;
    case BD_EVENT_END_OF_TITLE:
        if(p_sys->b_pl_playing)
        {
            notifyDiscontinuityToParser(p_sys);
            blurayRestartParser(p_demux, false, false);
            p_sys->b_draining = true;
            p_sys->b_pl_playing = false;
        }
        open3dblurayLogBdEventState(p_demux, "after", e, b_delayed);
        break;
    case BD_EVENT_IDLE:
        /* nothing to do (ex. BD-J is preparing menus, waiting user input or running animation) */
        /* avoid busy loop (bd_read() returns no data) */
        msleep( 40000 );
        open3dblurayLogBdEventState(p_demux, "after", e, b_delayed);
        break;

    default:
    {
        const char *psz_name = bd_event_name((uint32_t)e->event);
        unsigned i_playlist = 0;

        vlc_mutex_lock(&p_sys->pl_info_lock);
        if (p_sys->p_pl_info != NULL)
            i_playlist = p_sys->p_pl_info->playlist;
        vlc_mutex_unlock(&p_sys->pl_info_lock);

        msg_Warn(p_demux,
                 "%s unhandled bd-event name=%s(%d) param=%d delayed=%d "
                 "title=%d playlist=%u menu_open=%d popup=%d pl_playing=%d",
                 OPEN3DBLURAY_LOG_PREFIX,
                 psz_name != NULL ? psz_name : "UNKNOWN",
                 e->event,
                 e->param,
                 b_delayed ? 1 : 0,
                 p_demux->info.i_title,
                 i_playlist,
                 p_sys->b_menu_open ? 1 : 0,
                 p_sys->b_popup_available ? 1 : 0,
                 p_sys->b_pl_playing ? 1 : 0);
        break;
    }
    }
}

static bool blurayIsBdjTitle(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    unsigned int i_title = p_demux->info.i_title;
    const BLURAY_DISC_INFO *di = bd_get_disc_info(p_sys->bluray);

    if (di && di->titles) {
        if ((i_title <= di->num_titles && di->titles[i_title] && di->titles[i_title]->bdj) ||
            (i_title == p_sys->i_title - 1 && di->first_play && di->first_play->bdj)) {
          return true;
        }
    }

    return false;
}

static void blurayHandleOverlays(demux_t *p_demux, int nread)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    vlc_mutex_lock(&p_sys->bdj_overlay_lock);

    for (int i = 0; i < MAX_OVERLAY; i++) {
        bluray_overlay_t *ov = p_sys->p_overlays[i];
        if (!ov) {
            continue;
        }
        vlc_mutex_lock(&ov->lock);
        bool display = ov->status == ToDisplay;
        const unsigned region_count = open3dblurayCountRegionChain(ov->p_regions);
        const int i_channel = ov->i_channel;
        vlc_mutex_unlock(&ov->lock);
        if (display) {
            const bool trace_overlay = open3dblurayTraceInteractiveGraphicsBridgeEnabled();
            const bool trace_visuals = open3dblurayTraceMenuVisualsEnabled();
            const bool bdj_title = blurayIsBdjTitle(p_demux);
            const bool still_active = open3dblurayStillImageActive(p_demux);
            const bool clip_has_primary_video =
                open3dblurayCurrentClipHasUsablePrimaryVideo(p_demux);
            const bool need_legacy_background =
                (!p_sys->p_vout && !p_sys->p_dummy_video && p_sys->b_menu &&
                 !p_sys->p_pl_info && nread == 0 && bdj_title);
            const bool need_menu_open_ig_background =
                (!p_sys->p_dummy_video && p_sys->b_menu &&
                 p_sys->b_menu_open &&
                 !clip_has_primary_video &&
                 p_sys->p_vout == NULL &&
                 (nread == 0 || still_active) &&
                 ov->plane == BD_OVERLAY_IG && region_count > 0);
            if (p_sys->p_vout == NULL)
                open3dblurayEnsureVout(p_demux);

            /* NOTE: we might want to enable background video always when there's no video stream playing.
               Now, with some discs, there are perioids (even seconds) during which the video window
               disappears and just playlist is shown.
               (sometimes BD-J runs slowly ...)
            */
            if (trace_visuals)
                msg_Warn(p_demux,
                         "open3dbluray trace background-gate plane=%s regions=%u menu=%d menu_open=%d vout=%d dummy=%d pl_info=%d nread=%d bdj_title=%d still=%d clip_has_video=%d create=%d",
                         open3dblurayOverlayPlaneName((unsigned)ov->plane),
                         region_count,
                         p_sys->b_menu ? 1 : 0,
                         p_sys->b_menu_open ? 1 : 0,
                         p_sys->p_vout != NULL ? 1 : 0,
                         p_sys->p_dummy_video != NULL ? 1 : 0,
                         p_sys->p_pl_info != NULL ? 1 : 0,
                         nread,
                         bdj_title ? 1 : 0,
                         still_active ? 1 : 0,
                         clip_has_primary_video ? 1 : 0,
                         (need_legacy_background || need_menu_open_ig_background) ? 1 : 0);

            if (need_legacy_background || need_menu_open_ig_background) {

                /* Only stand up the synthetic background when there is no
                   tracked vout at all. Replacing an already-live still-image
                   vout with a dummy rawvideo ES is what tends to spawn a
                   second top-level window at menu-open and split the menu
                   overlay from the background video. */
                if (blurayCreateBackgroundUnlocked(p_demux) != NULL)
                    open3dblurayEnsureVout(p_demux);
            }

            if (p_sys->p_vout != NULL) {
                open3dblurayAttachDirectBridgesToVout(p_demux);
                open3dblurayTraceOverlayCoverage(p_demux, "dispatch", ov,
                                                 open3dblurayUseDirectOverlayBridge(p_demux, ov),
                                                 p_sys->b_menu_open);
                if (trace_overlay && ov->plane == BD_OVERLAY_IG)
                    open3dblurayTraceIgRegionChain(p_demux, "dispatch", ov,
                                                   open3dblurayUseDirectOverlayBridge(p_demux, ov),
                                                   p_sys->b_menu_open);
                if (trace_overlay && ov->plane == BD_OVERLAY_IG)
                    msg_Dbg(p_demux,
                            "open3dbluray trace overlay dispatch plane=%u regions=%u channel=%d vout=%d direct=%d",
                            (unsigned)ov->plane,
                            region_count,
                            i_channel,
                            p_sys->p_vout != NULL ? 1 : 0,
                            open3dblurayUseDirectOverlayBridge(p_demux, ov) ? 1 : 0);
                if (ov->plane == BD_OVERLAY_PG)
                    open3dblurayPublishSubtitleOffsetToVout(p_demux, ov);
                if (open3dblurayUseDirectOverlayBridge(p_demux, ov)) {
                    open3dblurayDetachOverlaySubpictureFromVout(p_demux, ov);
                    vlc_mutex_lock(&ov->lock);
                    ov->status = Displayed;
                    vlc_mutex_unlock(&ov->lock);
                } else {
                    bluraySendOverlayToVout(p_demux, ov);
                }
            } else if (trace_overlay && ov->plane == BD_OVERLAY_IG) {
                msg_Dbg(p_demux,
                        "open3dbluray trace overlay dispatch plane=%u regions=%u channel=%d vout=%d direct=%d",
                        (unsigned)ov->plane,
                        region_count,
                        i_channel,
                        0,
                        0);
            }
        }
    }

    vlc_mutex_unlock(&p_sys->bdj_overlay_lock);
}

static int onIntfEvent( vlc_object_t *p_input, char const *psz_var,
                        vlc_value_t oldval, vlc_value_t val, void *p_data )
{
    (void)p_input; (void) psz_var; (void) oldval;
    demux_t *p_demux = p_data;
    demux_sys_t *p_sys = p_demux->p_sys;
    const bool trace = open3dblurayTraceInteractiveGraphicsBridgeEnabled();

    if (val.i_int == INPUT_EVENT_VOUT) {
        vout_thread_t *current_vout = input_GetVout(p_demux->p_input);

        if (trace)
            msg_Dbg(p_demux,
                    "open3dbluray trace ig-bridge vout-event tracked=%p current=%p menu_open=%d popup=%d",
                    (void *)p_sys->p_vout,
                    (void *)current_vout,
                    p_sys->b_menu_open ? 1 : 0,
                    p_sys->b_popup_available ? 1 : 0);

        if (current_vout != NULL)
            open3dblurayAdoptAcquiredVout(p_demux, current_vout, "intf-event");
        else
        {
            if (trace && p_sys->p_vout != NULL)
                msg_Dbg(p_demux,
                        "open3dbluray trace ig-bridge vout-event-defer tracked=%p current=%p",
                        (void *)p_sys->p_vout,
                        (void *)current_vout);
            open3dblurayRefreshTrackedVoutState(p_demux, "intf-event-defer");
        }

        blurayHandleOverlays(p_demux, 1);
    }

    return VLC_SUCCESS;
}

static int blurayDemux(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;
    BD_EVENT e;
    int i_pending_forced_pid = -1;
    bool b_pending_forced_auto = false;

    if(p_sys->b_draining)
    {
        bool b_empty = false;
        if(es_out_Control(p_sys->p_out, ES_OUT_GET_EMPTY, &b_empty) != VLC_SUCCESS || b_empty)
        {
            es_out_Control(p_sys->p_out, ES_OUT_RESET_PCR);
            p_sys->b_draining = false;
        }
        else
        {
            msg_Dbg(p_demux, "Draining...");
            msleep( 40000 );
            return VLC_DEMUXER_SUCCESS;
        }
    }

    block_t *p_block = block_Alloc(BD_READ_SIZE);
    if (!p_block)
        return VLC_DEMUXER_EGENERIC;

    int nread;

    if (p_sys->b_menu == false) {
        nread = bd_read(p_sys->bluray, p_block->p_buffer, BD_READ_SIZE);
        while (bd_get_event(p_sys->bluray, &e)) {
            if (e.event == BD_EVENT_MENU) {
                msg_Warn(p_demux,
                         "%s menu_event_dispatch source=bd_get_event menu_mode=0 param=%d",
                         OPEN3DBLURAY_LOG_PREFIX,
                         e.param);
            }
            blurayHandleEvent(p_demux, &e, false);
        }
    } else {
        bool b_yield_after_redundant_end_of_title = false;

        nread = bd_read_ext(p_sys->bluray, p_block->p_buffer, BD_READ_SIZE, &e);
        while (e.event != BD_EVENT_NONE) {
            if (e.event == BD_EVENT_MENU) {
                msg_Warn(p_demux,
                         "%s menu_event_dispatch source=%s menu_mode=1 param=%d",
                         OPEN3DBLURAY_LOG_PREFIX,
                         nread >= 0 ? "bd_read_ext" : "bd_read_ext(error)",
                         e.param);
            }
            const bool b_redundant_end_of_title =
                e.event == BD_EVENT_END_OF_TITLE &&
                !p_sys->b_pl_playing;
            if (b_redundant_end_of_title)
                p_sys->i_redundant_end_of_title_streak++;
            else
                p_sys->i_redundant_end_of_title_streak = 0;
            const bool b_probe_redundant_end_of_title =
                p_sys->b_probe_yield_redundant_end_of_title &&
                b_redundant_end_of_title;
            blurayHandleEvent(p_demux, &e, false);
            if (b_probe_redundant_end_of_title) {
                p_sys->i_probe_redundant_end_of_title_yield_count++;
                if (!p_sys->b_probe_logged_redundant_end_of_title ||
                    (p_sys->i_probe_redundant_end_of_title_yield_count % 1024u) == 0u) {
                    open3dblurayLogProbeRedundantEndOfTitleYield(p_demux, &e);
                    p_sys->b_probe_logged_redundant_end_of_title = true;
                }
                b_yield_after_redundant_end_of_title = true;
                break;
            }
            if (b_redundant_end_of_title &&
                p_sys->i_redundant_end_of_title_streak >= 2u) {
                if (p_sys->i_redundant_end_of_title_streak == 2u ||
                    (p_sys->i_redundant_end_of_title_streak % 1024u) == 0u) {
                    open3dblurayLogRedundantEndOfTitleStorm(
                        p_demux, &e, p_sys->i_redundant_end_of_title_streak);
                }
                b_yield_after_redundant_end_of_title = true;
                break;
            }
            bd_get_event(p_sys->bluray, &e);
            if (e.event == BD_EVENT_MENU) {
                msg_Warn(p_demux,
                         "%s menu_event_dispatch source=bd_get_event menu_mode=1 param=%d",
                         OPEN3DBLURAY_LOG_PREFIX,
                         e.param);
            }
        }
        if (b_yield_after_redundant_end_of_title)
            msleep(40000);
    }

    /* Process delayed selections events */
    for(int i=0; i<p_sys->events_delayed.i_size; i++)
        blurayHandleEvent(p_demux, &p_sys->events_delayed.p_elems[i], true);
    p_sys->events_delayed.i_size = 0;

    if (open3dblurayTakePendingForcedSubtitleSelection(p_demux,
                                                       &i_pending_forced_pid,
                                                       &b_pending_forced_auto)) {
        msg_Dbg(p_demux,
                "%s applying deferred forced-only subtitle selection pid=0x%04x auto=%d",
                OPEN3DBLURAY_LOG_PREFIX,
                i_pending_forced_pid,
                b_pending_forced_auto ? 1 : 0);
        blurayOnUserStreamSelectionEx(p_demux,
                                      open3dblurayMakeForcedSubtitleEsId(i_pending_forced_pid),
                                      b_pending_forced_auto);
    }

    blurayHandleOverlays(p_demux, nread);
    open3dblurayPublishSubtitleOffsetToVout(p_demux, NULL);

    if (nread <= 0) {
        block_Release(p_block);
        if (p_sys->b_fatal_error || nread < 0) {
            msg_Err(p_demux, "bluray: stopping playback after fatal error\n");
            return VLC_DEMUXER_EGENERIC;
        }
        if (!p_sys->b_menu) {
            return VLC_DEMUXER_EOF;
        }
        return VLC_DEMUXER_SUCCESS;
    }

    p_block->i_buffer = nread;
    open3dblurayTraceSubtitleSourceBlock(p_demux, p_block->p_buffer, p_block->i_buffer);

    if (open3dblurayShouldHoldBackground(p_demux)) {
        if (open3dblurayTraceMenuVisualsEnabled())
            msg_Warn(p_demux,
                     "open3dbluray trace background-create phase=hold menu=%d menu_open=%d vout=%d dummy=%d nread=%d",
                     p_sys->b_menu ? 1 : 0,
                     p_sys->b_menu_open ? 1 : 0,
                     p_sys->p_vout != NULL ? 1 : 0,
                     p_sys->p_dummy_video != NULL ? 1 : 0,
                     nread);
    } else {
        stopBackground(p_demux);
    }

#if OPEN3DBLURAY_ENABLE_MVC
    vlc_demux_chained_Send(p_sys->p_parser, p_block);
    open3dblurayDrainMvcUnits(p_demux);
#else
    vlc_demux_chained_Send(p_sys->p_parser, p_block);
#endif

    p_sys->b_flushed = false;

    return VLC_DEMUXER_SUCCESS;
}

/*****************************************************************************
 * bluray Escape es_out
 *****************************************************************************/
struct escape_es_id
{
    es_out_id_t *es;
    bool drop_first;
    bool is_synthetic_mvc;
    vlc_tick_t first_dts;
    vlc_tick_t anchor_pcr;
};

struct escape_esout_sys
{
    demux_t *p_demux;
    es_out_t *dst_out;
    vlc_tick_t  offset_pcr;
    vlc_tick_t  last_group_pcr;

    vlc_array_t es_ids; /* escape_es_id */
};

static es_out_id_t *escape_esOutAdd(es_out_t *out, const es_format_t *fmt)
{
    struct escape_esout_sys *esout_sys = (struct escape_esout_sys *)out->p_sys;

    struct escape_es_id *esc_id = malloc(sizeof(*esc_id));
    if (!esc_id)
        return NULL;
    esc_id->es = es_out_Add(esout_sys->dst_out, fmt);
    if (!esc_id->es)
    {
        free(esc_id);
        return NULL;
    }
    esc_id->first_dts = -1;
    esc_id->anchor_pcr = VLC_TICK_INVALID;
    esc_id->is_synthetic_mvc =
#if OPEN3DBLURAY_ENABLE_MVC
        fmt->i_cat == VIDEO_ES && fmt->i_codec == OPEN3DBLURAY_CODEC_MVC;
#else
        false;
#endif
    /* Stock video goes through a first-block preroll bridge here, but the
     * synthetic mvc1 restart unit is decoder-critical after seek. If we mark
     * that first merged unit as preroll, edge264mvc may not see the refresh
     * family until much later in the title. */
    esc_id->drop_first = fmt->i_cat == VIDEO_ES && !esc_id->is_synthetic_mvc;
    if (vlc_array_append(&esout_sys->es_ids, esc_id) != VLC_SUCCESS)
    {
        es_out_Del(esout_sys->dst_out, esc_id->es);
        free(esc_id);
        return NULL;
    }
    return esc_id->es;
}

static struct escape_es_id *escape_GetEscOutId(es_out_t *out, es_out_id_t *es,
                                               size_t *out_idx)
{
    struct escape_esout_sys *esout_sys = (struct escape_esout_sys *)out->p_sys;

    for (size_t i = 0; i < vlc_array_count(&esout_sys->es_ids); ++i)
    {
        struct escape_es_id *esc_id = vlc_array_item_at_index(&esout_sys->es_ids, i);
        if (esc_id->es == es)
        {
            if (out_idx)
                *out_idx = i;
            return esc_id;
        }
    }
    return NULL;
}

static int escape_esOutSend(es_out_t *out, es_out_id_t *es, block_t *block)
{
    struct escape_esout_sys *esout_sys = (struct escape_esout_sys *)out->p_sys;
    struct escape_es_id *esc_id = escape_GetEscOutId(out, es, NULL);
    if (esc_id == NULL)
        return VLC_EGENERIC;

    if (esout_sys->offset_pcr != -1)
    {
        if (esc_id->first_dts == -1)
        {
            esc_id->first_dts =
                block->i_dts != VLC_TICK_INVALID ? block->i_dts : block->i_pts;
            if (esc_id->is_synthetic_mvc &&
                esout_sys->last_group_pcr != VLC_TICK_INVALID)
                esc_id->anchor_pcr = esout_sys->last_group_pcr;
            else
                esc_id->anchor_pcr = esout_sys->offset_pcr;
            if (esc_id->drop_first)
                block->i_flags |= BLOCK_FLAG_PREROLL;
        }
        if (esc_id->first_dts != VLC_TICK_INVALID &&
            esc_id->anchor_pcr != VLC_TICK_INVALID)
        {
            vlc_tick_t offset = esc_id->anchor_pcr - esc_id->first_dts;
            if (block->i_pts != VLC_TICK_INVALID)
                block->i_pts += offset;
            if (block->i_dts != VLC_TICK_INVALID)
                block->i_dts += offset;
        }
    }

#if OPEN3DBLURAY_ENABLE_MVC
    if (esc_id->is_synthetic_mvc && esout_sys->p_demux) {
        demux_sys_t *p_sys = esout_sys->p_demux->p_sys;
        if (p_sys->b_seek_timing_trace_active &&
            !p_sys->b_seek_timing_trace_saw_mvc_send) {
            p_sys->b_seek_timing_trace_saw_mvc_send = true;
            msg_Info(esout_sys->p_demux,
                     "%s seek_timing epoch=%u first_mvc_send first_dts=%" PRId64 " anchor_pcr=%" PRId64 " translated_pts=%" PRId64 " translated_dts=%" PRId64 " flags=0x%x last_group_pcr=%" PRId64,
                     OPEN3DBLURAY_LOG_PREFIX,
                     p_sys->i_seek_timing_trace_epoch,
                     esc_id->first_dts,
                     esc_id->anchor_pcr,
                     block->i_pts,
                     block->i_dts,
                     block->i_flags,
                     p_sys->i_seek_timing_last_group_pcr);
        }
    }
#endif

    return es_out_Send(esout_sys->dst_out, es, block);
}

static void escape_esOutDel(es_out_t *out, es_out_id_t *es)
{
    struct escape_esout_sys *esout_sys = (struct escape_esout_sys *)out->p_sys;
    size_t index;
    struct escape_es_id *esc_id = escape_GetEscOutId(out, es, &index);
    if (esc_id == NULL)
        return;

    vlc_array_remove(&esout_sys->es_ids, index);
    es_out_Del(esout_sys->dst_out, es);
    free(esc_id);
}

static int escape_esOutControl(es_out_t *p_out, int query, va_list args)
{
    struct escape_esout_sys *esout_sys = (struct escape_esout_sys *)p_out->p_sys;
    int ret;

    switch (query)
    {
        case ES_OUT_RESET_PCR:
#if OPEN3DBLURAY_ENABLE_MVC
            if (esout_sys->p_demux)
                open3dblurayTraceSeekTimingResetPcr(esout_sys->p_demux);
#endif
            for (size_t i = 0; i < vlc_array_count(&esout_sys->es_ids); ++i)
            {
                struct escape_es_id *esc_id = vlc_array_item_at_index(&esout_sys->es_ids, i);
                esc_id->first_dts = -1;
                esc_id->anchor_pcr = VLC_TICK_INVALID;
            }
            esout_sys->offset_pcr = -1;
            esout_sys->last_group_pcr = VLC_TICK_INVALID;

            ret = es_out_vaControl(esout_sys->dst_out, query, args);
            break;
        case ES_OUT_SET_GROUP_PCR:
        {
            int group = va_arg( args, int );
            vlc_tick_t pcr = va_arg( args, int64_t );

            esout_sys->last_group_pcr = pcr;
            if (esout_sys->offset_pcr == -1)
                esout_sys->offset_pcr = pcr;
#if OPEN3DBLURAY_ENABLE_MVC
            if (esout_sys->p_demux)
                open3dblurayTraceSeekTimingStockPcr(esout_sys->p_demux, group, pcr,
                                                    esout_sys->offset_pcr);
#endif
            ret = es_out_Control(esout_sys->dst_out, query, group, pcr);
            break;
        }
        default:
            ret = es_out_vaControl(esout_sys->dst_out, query, args);
            break;
    }
    return ret;
}

static void escape_esOutDestroy(es_out_t *p_out)
{
    struct escape_esout_sys *esout_sys = (struct escape_esout_sys *)p_out->p_sys;

    vlc_array_clear(&esout_sys->es_ids);
    free(p_out->p_sys);
    free(p_out);
}

static es_out_t *escape_esOutNew(vlc_object_t *p_obj, es_out_t *dst_out)
{
    es_out_t *out = malloc(sizeof(*out));
    if (unlikely(out == NULL))
        return NULL;

    out->pf_add       = escape_esOutAdd;
    out->pf_control   = escape_esOutControl;
    out->pf_del       = escape_esOutDel;
    out->pf_destroy   = escape_esOutDestroy;
    out->pf_send      = escape_esOutSend;

    struct escape_esout_sys *esout_sys = malloc(sizeof(*esout_sys));
    if (unlikely(esout_sys == NULL))
    {
        free(out);
        return NULL;
    }
    out->p_sys = (es_out_sys_t *) esout_sys;
    vlc_array_init(&esout_sys->es_ids);
    esout_sys->p_demux = (demux_t *)p_obj;
    esout_sys->offset_pcr = -1;
    esout_sys->last_group_pcr = VLC_TICK_INVALID;
    esout_sys->dst_out = dst_out;

    var_Create( p_obj, "ts-trust-pcr", VLC_VAR_BOOL );
    var_SetBool( p_obj, "ts-trust-pcr", false );
    return out;
}
