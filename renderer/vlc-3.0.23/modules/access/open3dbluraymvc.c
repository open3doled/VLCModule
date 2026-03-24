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
    add_bool("bluray-menu", true, BD_MENU_TEXT, BD_MENU_LONGTEXT, false)
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

#if defined(OPEN3D_VLC_ABI_ALIAS_T64)
extern int CDECL_SYMBOL vlc_entry__3_0_0f(vlc_set_cb, void *);
extern const char *CDECL_SYMBOL vlc_entry_copyright__3_0_0f(void);
extern const char *CDECL_SYMBOL vlc_entry_license__3_0_0f(void);
EXTERN_SYMBOL DLL_SYMBOL int CDECL_SYMBOL vlc_entry__3_0_0ft64(vlc_set_cb, void *);
EXTERN_SYMBOL DLL_SYMBOL const char *CDECL_SYMBOL vlc_entry_copyright__3_0_0ft64(void);
EXTERN_SYMBOL DLL_SYMBOL const char *CDECL_SYMBOL vlc_entry_license__3_0_0ft64(void);

EXTERN_SYMBOL DLL_SYMBOL int CDECL_SYMBOL
vlc_entry__3_0_0ft64(vlc_set_cb vlc_set, void *opaque)
{
    return vlc_entry__3_0_0f(vlc_set, opaque);
}

EXTERN_SYMBOL DLL_SYMBOL const char *CDECL_SYMBOL
vlc_entry_copyright__3_0_0ft64(void)
{
    return vlc_entry_copyright__3_0_0f();
}

EXTERN_SYMBOL DLL_SYMBOL const char *CDECL_SYMBOL
vlc_entry_license__3_0_0ft64(void)
{
    return vlc_entry_license__3_0_0f();
}
#endif

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

    vlc_mutex_t         bdj_overlay_lock; /* used to lock BD-J overlay open/close while overlays are being sent to vout */

    /* */
    vout_thread_t       *p_vout;
    open3d_subtitle_bridge_t subtitle_bridge;

    es_out_id_t         *p_dummy_video;

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

static void blurayReleaseVout(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->p_vout != NULL) {
        Open3DSubtitleBridgeDetachFromObject(VLC_OBJECT(p_sys->p_vout));
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

static void open3dblurayEnsureVout(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->p_vout != NULL)
        return;

    p_sys->p_vout = input_GetVout(p_demux->p_input);
    if (p_sys->p_vout != NULL) {
        var_AddCallback(p_sys->p_vout, "mouse-moved", onMouseEvent, p_demux);
        var_AddCallback(p_sys->p_vout, "mouse-clicked", onMouseEvent, p_demux);
    }
}

static void open3dblurayAttachSubtitleBridgeToVout(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    open3dblurayEnsureVout(p_demux);
    if (p_sys->p_vout == NULL)
        return;

    Open3DSubtitleBridgeAttachToObject(VLC_OBJECT(p_sys->p_vout),
                                       &p_sys->subtitle_bridge);
}

static bool open3dblurayUseDirectSubtitleBridge(demux_t *p_demux,
                                                const bluray_overlay_t *p_ov)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (p_sys->p_vout == NULL || p_ov == NULL || p_ov->plane != BD_OVERLAY_PG)
        return false;

    return Open3DSubtitleBridgeGetEnabledFromObject(VLC_OBJECT(p_sys->p_vout));
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

    if (p_sys->p_dummy_video)
        return p_sys->p_dummy_video;

    msg_Info(p_demux, "Start background");

    /* */
    es_format_t fmt;
    es_format_Init( &fmt, VIDEO_ES, VLC_CODEC_I420 );
    video_format_Setup( &fmt.video, VLC_CODEC_I420,
                        1920, 1080, 1920, 1080, 1, 1);
    fmt.i_priority = ES_PRIORITY_SELECTABLE_MIN;
    fmt.i_id = 4115; /* 4113 = main video. 4114 = MVC. 4115 = unused. */
    fmt.i_group = 1;

    p_sys->p_dummy_video = es_out_Add(p_demux->out, &fmt);

    if (!p_sys->p_dummy_video) {
        msg_Err(p_demux, "Error adding background ES");
        goto out;
    }

    block_t *p_block = block_Alloc(fmt.video.i_width * fmt.video.i_height *
                                   fmt.video.i_bits_per_pixel / 8);
    if (!p_block) {
        msg_Err(p_demux, "Error allocating block for background video");
        goto out;
    }

    // XXX TODO: what would be correct timestamp ???
    p_block->i_dts = p_block->i_pts = mdate() + CLOCK_FREQ/25;

    uint8_t *p = p_block->p_buffer;
    memset(p, 0, fmt.video.i_width * fmt.video.i_height);
    p += fmt.video.i_width * fmt.video.i_height;
    memset(p, 0x80, fmt.video.i_width * fmt.video.i_height / 2);

    es_out_Send(p_demux->out, p_sys->p_dummy_video, p_block);

 out:
    es_format_Clean(&fmt);
    return p_sys->p_dummy_video;
}

static void stopBackground(demux_t *p_demux)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if (!p_sys->p_dummy_video) {
        return;
    }

    msg_Info(p_demux, "Stop background");

    es_out_Del(p_demux->out, p_sys->p_dummy_video);
    p_sys->p_dummy_video = NULL;
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
        es_out_Send(p_sink_out, p_sys->p_mvc_video, p_block);
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

    const char *error_msg = NULL;
#define BLURAY_ERROR(s) do { error_msg = s; goto error; } while(0)

    if (unlikely(!p_demux->p_input))
        return VLC_EGENERIC;

    forced = !strcasecmp(p_demux->psz_access, "bluray") ||
             !strcasecmp(p_demux->psz_access, OPEN3DBLURAY_ACCESS_NAME);

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
            return VLC_EGENERIC;
        }

        if (probeFile(p_demux->psz_file) != VLC_SUCCESS) {
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

    var_AddCallback( p_demux->p_input, "intf-event", onIntfEvent, p_demux );

    msg_Info(p_demux,
             "%s build git=%s built=%s access=%s file=%s",
             OPEN3DBLURAY_LOG_PREFIX,
             OPEN3DBLURAY_BUILD_GIT_SHA,
             OPEN3DBLURAY_BUILD_TIME,
             p_demux->psz_access ? p_demux->psz_access : "(null)",
             p_demux->psz_file ? p_demux->psz_file : "(null)");

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
    bd_get_event(p_sys->bluray, NULL);

    /* Registering overlay event handler */
    bd_register_overlay_proc(p_sys->bluray, p_demux, blurayOverlayProc);

    if (p_sys->b_menu) {

        /* Register ARGB overlay handler for BD-J */
        if (disc_info->num_bdj_titles)
            bd_register_argb_overlay_proc(p_sys->bluray, p_demux, blurayArgbOverlayProc, NULL);

        /* libbluray will start playback from "First-Title" title */
        if (bd_play(p_sys->bluray) == 0)
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

    p_sys->p_parser = vlc_demux_chained_New(VLC_OBJECT(p_demux), "ts", p_sys->p_out);
    if (!p_sys->p_parser) {
        msg_Err(p_demux, "Failed to create TS demuxer");
        goto error;
    }

#if OPEN3DBLURAY_ENABLE_MVC
    open3dblurayLogMvcInfo(p_demux, "open");
#endif

    p_demux->pf_control = blurayControl;
    p_demux->pf_demux   = blurayDemux;

    return VLC_SUCCESS;

error:
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
            /* Keep the stock TS parser alive for non-video ES, but do not let the
             * base-view H.264 track compete with the synthetic merged mvc1 path. */
            b_select = false;
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
        /* Suppress the stock base-view video payload while the synthetic mvc1 ES
         * is active, but keep feeding the TS parser so audio/subtitles survive. */
        block_Release(p_block);
        p_block = NULL;
    }
#endif
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
    VLC_UNUSED(old);
    VLC_UNUSED(p_vout);

    if (psz_var[6] == 'm')   //Mouse moved
        bd_mouse_select(p_sys->bluray, -1, val.coords.x, val.coords.y);
    else if (psz_var[6] == 'c') {
        bd_mouse_select(p_sys->bluray, -1, val.coords.x, val.coords.y);
        bd_user_input(p_sys->bluray, -1, BD_VK_MOUSE_ACTIVATE);
    } else {
        vlc_assert_unreachable();
    }
    return VLC_SUCCESS;
}

static int sendKeyEvent(demux_sys_t *p_sys, unsigned int key)
{
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

    if (eventov->palette) {
        p_reg->fmt.p_palette->i_entries = 256;
        for (int i = 0; i < 256; ++i) {
            p_reg->fmt.p_palette->palette[i][0] = eventov->palette[i].Y;
            p_reg->fmt.p_palette->palette[i][1] = eventov->palette[i].Cb;
            p_reg->fmt.p_palette->palette[i][2] = eventov->palette[i].Cr;
            p_reg->fmt.p_palette->palette[i][3] = eventov->palette[i].T;
        }
    }

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

            open3dbluraySyncSubtitleBridgeFromOverlayLocked(p_demux, ov);
            open3dblurayPublishSubtitleOffsetToVout(p_demux, ov);
            vlc_mutex_unlock(&ov->lock);
            if (open3dblurayUseDirectSubtitleBridge(p_demux, ov))
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

static void blurayDrawArgbOverlay(demux_t *p_demux, const BD_ARGB_OVERLAY* const eventov)
{
    demux_sys_t *p_sys = p_demux->p_sys;

    bluray_overlay_t *ov = p_sys->p_overlays[eventov->plane];
    if(!ov)
        return;

    vlc_mutex_lock(&ov->lock);

    /* Find a region to update */
    subpicture_region_t *p_reg = ov->p_regions;
    if (!p_reg || p_reg->fmt.i_chroma != ARGB_OVERLAY_CHROMA ||
        eventov->x + eventov->w > p_reg->fmt.i_width ||
        eventov->y + eventov->h > p_reg->fmt.i_height) {
        vlc_mutex_unlock(&ov->lock);
        return;
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

    vlc_mutex_unlock(&ov->lock);
    /*
     * /!\ The region is now stored in our internal list, but not in the subpicture /!\
     */
}

static void blurayArgbOverlayProc(void *ptr, const BD_ARGB_OVERLAY *const overlay)
{
    demux_t *p_demux = (demux_t*)ptr;
    demux_sys_t *p_sys = p_demux->p_sys;

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
            return sendKeyEvent(p_sys, BD_VK_POPUP);
        }
        return sendKeyEvent(p_sys, BD_VK_ENTER);
    case DEMUX_NAV_UP:
        return sendKeyEvent(p_sys, BD_VK_UP);
    case DEMUX_NAV_DOWN:
        return sendKeyEvent(p_sys, BD_VK_DOWN);
    case DEMUX_NAV_LEFT:
        return sendKeyEvent(p_sys, BD_VK_LEFT);
    case DEMUX_NAV_RIGHT:
        return sendKeyEvent(p_sys, BD_VK_RIGHT);
    case DEMUX_NAV_POPUP:
        return sendKeyEvent(p_sys, BD_VK_POPUP);
    case DEMUX_NAV_MENU:
        if (p_sys->b_menu) {
            if (bd_menu_call(p_sys->bluray, -1) == 1) {
                p_demux->info.i_update |= INPUT_UPDATE_TITLE | INPUT_UPDATE_SEEKPOINT;
                return VLC_SUCCESS;
            }
            msg_Err(p_demux, "Can't select Top Menu title");
            return sendKeyEvent(p_sys, BD_VK_POPUP);
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
            es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_SET_ES_BY_PID, (int)i_type, i_pid);
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

static void blurayOnClipUpdate(demux_t *p_demux, uint32_t clip)
{
    demux_sys_t *p_sys = p_demux->p_sys;

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

static void blurayHandleEvent(demux_t *p_demux, const BD_EVENT *e, bool b_delayed)
{
    demux_sys_t *p_sys = p_demux->p_sys;

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
        break;
    case BD_EVENT_PLAYLIST:
        /* Start of playlist playback (?????.mpls) */
        blurayUpdatePlaylist(p_demux, e->param);
        if (p_sys->b_pl_playing) {
            /* previous playlist was stopped in middle. flush to avoid delay */
            msg_Info(p_demux, "Stopping playlist playback");
            blurayRestartParser(p_demux, false, false);
            es_out_Control( p_demux->out, ES_OUT_RESET_PCR );
        }
        p_sys->b_pl_playing = true;
        break;
    case BD_EVENT_PLAYITEM:
        notifyDiscontinuityToParser(p_sys);
        blurayOnClipUpdate(p_demux, e->param);
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
        p_sys->b_menu_open = e->param;
        break;
    case BD_EVENT_POPUP:
        p_sys->b_popup_available = e->param;
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
    case BD_EVENT_STILL_TIME:
        blurayStillImage(p_demux, e->param);
        break;
    case BD_EVENT_DISCONTINUITY:
        /* reset demuxer (partially decoded PES packets must be dropped) */
        blurayRestartParser(p_demux, false, true);
        es_out_Control(p_sys->p_out, BLURAY_ES_OUT_CONTROL_FLAG_DISCONTINUITY);
        break;
    case BD_EVENT_END_OF_TITLE:
        if(p_sys->b_pl_playing)
        {
            notifyDiscontinuityToParser(p_sys);
            blurayRestartParser(p_demux, false, false);
            p_sys->b_draining = true;
            p_sys->b_pl_playing = false;
        }
        break;
    case BD_EVENT_IDLE:
        /* nothing to do (ex. BD-J is preparing menus, waiting user input or running animation) */
        /* avoid busy loop (bd_read() returns no data) */
        msleep( 40000 );
        break;

    default:
        msg_Warn(p_demux, "event: %d param: %d", e->event, e->param);
        break;
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
        vlc_mutex_unlock(&ov->lock);
        if (display) {
            if (p_sys->p_vout == NULL)
                open3dblurayEnsureVout(p_demux);

            /* NOTE: we might want to enable background video always when there's no video stream playing.
               Now, with some discs, there are perioids (even seconds) during which the video window
               disappears and just playlist is shown.
               (sometimes BD-J runs slowly ...)
            */
            if (!p_sys->p_vout && !p_sys->p_dummy_video && p_sys->b_menu &&
                !p_sys->p_pl_info && nread == 0 &&
                blurayIsBdjTitle(p_demux)) {

                /* Looks like there's no video stream playing.
                   Emit blank frame so that BD-J overlay can be drawn. */
                if(blurayCreateBackgroundUnlocked(p_demux) != NULL)
                    p_sys->p_vout = input_GetVout(p_demux->p_input);
            }

            if (p_sys->p_vout != NULL) {
                open3dblurayAttachSubtitleBridgeToVout(p_demux);
                open3dblurayPublishSubtitleOffsetToVout(p_demux, ov);
                if (open3dblurayUseDirectSubtitleBridge(p_demux, ov)) {
                    open3dblurayDetachOverlaySubpictureFromVout(p_demux, ov);
                    vlc_mutex_lock(&ov->lock);
                    ov->status = Displayed;
                    vlc_mutex_unlock(&ov->lock);
                } else {
                    bluraySendOverlayToVout(p_demux, ov);
                }
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

    if (val.i_int == INPUT_EVENT_VOUT) {

        vlc_mutex_lock(&p_sys->bdj_overlay_lock);
        if( p_sys->p_vout != NULL ) {
            blurayReleaseVout(p_demux);
        }
        vlc_mutex_unlock(&p_sys->bdj_overlay_lock);

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
        while (bd_get_event(p_sys->bluray, &e))
            blurayHandleEvent(p_demux, &e, false);
    } else {
        nread = bd_read_ext(p_sys->bluray, p_block->p_buffer, BD_READ_SIZE, &e);
        while (e.event != BD_EVENT_NONE) {
            blurayHandleEvent(p_demux, &e, false);
            bd_get_event(p_sys->bluray, &e);
        }
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

    stopBackground(p_demux);

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
