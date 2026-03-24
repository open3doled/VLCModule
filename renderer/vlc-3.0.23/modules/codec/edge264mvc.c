/**
 * @file edge264mvc.c
 * @brief edge264 MVC decoder module for VLC 3.0.23
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <dlfcn.h>

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_interrupt.h>
#include <vlc_plugin.h>
#include "../video_output/open3d_subtitle_bridge.h"
#define OPEN3DBLURAYMVC_CODEC_MVC VLC_FOURCC('m','v','c','1')
#define OPEN3DMKV_CODEC_H264 VLC_FOURCC('m','k','v','h')

#define EDGE264MVC_LIB_PATH_TEXT N_("edge264 shared library path")
#define EDGE264MVC_LIB_PATH_LONGTEXT N_( \
    "Optional absolute path to libedge264 shared library. If empty, " \
    "the loader tries libedge264.so.1 then libedge264.so.")

#define EDGE264MVC_THREADS_TEXT N_("edge264 decoder worker threads")
#define EDGE264MVC_THREADS_LONGTEXT N_( \
    "Number of worker threads passed to edge264_alloc. " \
    "Use -1 for edge264 auto thread count (default), " \
    "0 to disable internal multithreading, " \
    "or a positive number for fixed threads.")

#define EDGE264MVC_DUMP_ANNEXB_TEXT N_("edge264 dump Annex-B feed path")
#define EDGE264MVC_DUMP_ANNEXB_LONGTEXT N_( \
    "Optional path to write Annex-B NAL units exactly as fed into edge264. " \
    "Intended for crash repro/debug only; disabled by default.")

#define EDGE264MVC_RECOVER_GAP_TEXT N_("Soft-error recover flush gap")
#define EDGE264MVC_RECOVER_GAP_LONGTEXT N_( \
    "Minimum decode-call gap between recover flushes after ENOTSUP/EBADMSG. " \
    "Use 0 to flush on every soft error.")

#define EDGE264MVC_ENOBUFS_LOOP_TEXT N_("ENOBUFS spin limit per NAL")
#define EDGE264MVC_ENOBUFS_LOOP_LONGTEXT N_( \
    "Maximum ENOBUFS retry spins for one NAL before starvation guard flush+resync.")

#define EDGE264MVC_ENOBUFS_DRAIN_PERIOD_TEXT N_("ENOBUFS drain pulse period")
#define EDGE264MVC_ENOBUFS_DRAIN_PERIOD_LONGTEXT N_( \
    "Run one decoder drain pulse every N ENOBUFS spins while handling a NAL.")

#define EDGE264MVC_ENOBUFS_SLEEP_US_TEXT N_("ENOBUFS retry sleep (microseconds)")
#define EDGE264MVC_ENOBUFS_SLEEP_US_LONGTEXT N_( \
    "Optional sleep between ENOBUFS retries to let threaded workers make progress.")

#define EDGE264MVC_RECOVER_VIEWEXT_TEXT N_("Recover on MVC extension soft errors")
#define EDGE264MVC_RECOVER_VIEWEXT_LONGTEXT N_( \
    "If enabled, ENOTSUP/EBADMSG from NAL types 14/20 trigger recover flush logic. " \
    "If disabled, those soft errors are counted but do not trigger decoder flush.")

#define EDGE264MVC_ADVERTISE_MULTIVIEW_TEXT N_("Advertise multiview SBS metadata")
#define EDGE264MVC_ADVERTISE_MULTIVIEW_LONGTEXT N_( \
    "If enabled, stereo-packed output is marked as multiview side-by-side. " \
    "If disabled, packed output is still side-by-side pixels but marked as 2D, " \
    "which is useful for monitor SBS viewing paths that otherwise stretch one eye.")
#define EDGE264MVC_LOG_FRAMEIDS_TEXT N_("Log stereo frame-id telemetry")
#define EDGE264MVC_LOG_FRAMEIDS_LONGTEXT N_( \
    "If enabled, logs FrameId/FrameId_mvc deltas for queued stereo frames to help " \
    "diagnose right-eye lead/lag and frame-step jitter.")
#define EDGE264MVC_LOG_SEI_TEXT N_("Log MVC/SEI core telemetry")
#define EDGE264MVC_LOG_SEI_LONGTEXT N_( \
    "If enabled, turns on edge264 core logging and forwards SEI-related lines. " \
    "Intended for MVC metadata investigation only; disabled by default.")
#define EDGE264MVC_FRAMEID_LOG_EVERY_TEXT N_("Frame-id periodic log interval")
#define EDGE264MVC_FRAMEID_LOG_EVERY_LONGTEXT N_( \
    "When frame-id logging is enabled, emit one periodic telemetry line every N queued frames.")
#define EDGE264MVC_RIGHT_HIST_DEPTH_TEXT N_("Right-eye history depth")
#define EDGE264MVC_RIGHT_HIST_DEPTH_LONGTEXT N_( \
    "If >0, keep a short history of decoded right-eye frames and select the most recent " \
    "right-eye FrameId not newer than the current base-view FrameId.")
#define EDGE264MVC_SKIP_TYPE20_NRI0_TEXT N_("Skip MVC type-20 with nal_ref_idc=0")
#define EDGE264MVC_SKIP_TYPE20_NRI0_LONGTEXT N_( \
    "If enabled, drop MVC extension NAL type 20 units with nal_ref_idc=0 (header 0x14) " \
    "before decode. Useful as a diagnostic for startup soft-error bursts.")
#define EDGE264MVC_GATE_TYPE20_TEXT N_("Gate MVC type-20 until subset SPS")
#define EDGE264MVC_GATE_TYPE20_LONGTEXT N_( \
    "If enabled, defer MVC extension NAL type 20 decoding until subset SPS " \
    "(NAL type 15) is observed. This mitigates startup type-20 soft-error bursts.")
#define EDGE264MVC_GATE_TYPE20_MAX_TEXT N_("Max startup-gated type-20 NALs")
#define EDGE264MVC_GATE_TYPE20_MAX_LONGTEXT N_( \
    "Safety cap for startup type-20 gating when subset SPS has not been seen yet. " \
    "After this many gated type-20 NALs, gating is auto-disabled for the session.")
#define EDGE264MVC_HOLD_RIGHT_BACKSTEP_TEXT N_("Hold right-eye frame on detected backstep")
#define EDGE264MVC_HOLD_RIGHT_BACKSTEP_LONGTEXT N_( \
    "If enabled, when selected right-eye frame-id would go backwards, hold the " \
    "previous selected right-eye frame (from history) to preserve monotonic order.")
#define EDGE264MVC_USE_PTS_DATES_TEXT N_("Use PTS for queued picture dates")
#define EDGE264MVC_USE_PTS_DATES_LONGTEXT N_( \
    "If enabled, queue decoded picture dates from packet PTS instead of DTS. " \
    "This is a narrow diagnostic for native MKV pacing; disabled by default.")
#define EDGE264MVC_TS_LOG_EVERY_TEXT N_("Timestamp cadence log interval")
#define EDGE264MVC_TS_LOG_EVERY_LONGTEXT N_( \
    "Emit timestamp cadence telemetry every N queued/popped picture dates, " \
    "and also when cadence deviates materially from the expected frame period. " \
    "Use 0 to disable.")
#define EDGE264MVC_NORMALIZE_TS_CADENCE_TEXT N_("Normalize queued timestamp cadence")
#define EDGE264MVC_NORMALIZE_TS_CADENCE_LONGTEXT N_( \
    "If enabled, lightly normalize queued picture dates toward the expected " \
    "frame period when native MKV packet timestamps oscillate or step backwards. " \
    "Intended for MKV pacing diagnostics and disabled by default.")

#define EDGE264MVC_TS_QUEUE_CAP 256
#define EDGE264MVC_NAL_LOOP_MAX 512
#define EDGE264MVC_DEFAULT_ENOBUFS_LOOP_MAX 128
#define EDGE264MVC_DEFAULT_ENOBUFS_LOOP_MAX_THREADED 512
#define EDGE264MVC_DEFAULT_ENOBUFS_DRAIN_PERIOD 16
#define EDGE264MVC_DEFAULT_ENOBUFS_SLEEP_US 0
#define EDGE264MVC_DEFAULT_RECOVER_FLUSH_MIN_DECODE_GAP 32
#define EDGE264MVC_DEFAULT_RECOVER_FLUSH_MIN_DECODE_GAP_THREADED 2048
#define EDGE264MVC_AVCC_NAL_MAX (16u * 1024u * 1024u)
#define EDGE264MVC_AVCC_PENDING_MAX (64u * 1024u * 1024u)
#define EDGE264MVC_DRAIN_LOOP_MAX 2048
#define EDGE264MVC_POST_DISCONT_BLOCKS 64u
#define EDGE264MVC_POST_DISCONT_ENOBUFS_LOOP_MAX 64u
#define EDGE264MVC_POST_SEEK_DEBUG_FRAMES 24u
#define EDGE264MVC_POST_SEEK_REQUIRE_STEREO_DROPS 48u
#define EDGE264MVC_RIGHT_HISTORY_MAX 16
#define EDGE264MVC_SOFT_TRACE_MAX 32u
#define EDGE264MVC_OFMD_LOG_MAX 32u
#define EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX 16u

#define EDGE264MVC_SOFT_ACTION_NONE 0u
#define EDGE264MVC_SOFT_ACTION_SKIP_VIEWEXT 1u
#define EDGE264MVC_SOFT_ACTION_SKIP_COOLDOWN 2u
#define EDGE264MVC_SOFT_ACTION_FLUSH 3u
#define EDGE264MVC_SOFT_ACTION_SKIP_NONVCL 4u

typedef struct Edge264Decoder Edge264Decoder;

typedef struct Edge264Frame {
    const uint8_t *samples[3];
    const uint8_t *samples_mvc[3];
    const uint8_t *mb_errors;
    int8_t bit_depth_Y;
    int8_t bit_depth_C;
    int16_t width_Y;
    int16_t width_C;
    int16_t height_Y;
    int16_t height_C;
    int16_t stride_Y;
    int16_t stride_C;
    int16_t stride_mb;
    int32_t FrameId;
    int32_t FrameId_mvc;
    int32_t FrameNum_abs;
    int32_t FrameNum_abs_mvc;
    int32_t PicOrderCnt;
    int32_t PicOrderCnt_mvc;
    int32_t AccessUnitId;
    int32_t AccessUnitId_mvc;
    int16_t view_id;
    int16_t view_id_mvc;
    int8_t temporal_id;
    int8_t temporal_id_mvc;
    int8_t anchor_pic_flag;
    int8_t anchor_pic_flag_mvc;
    int8_t inter_view_flag;
    int8_t inter_view_flag_mvc;
    int16_t frame_crop_offsets[4];
    void *return_arg;
} Edge264Frame;

typedef struct
{
    bool valid;
    int32_t frame_id;
    uint8_t *y;
    uint8_t *u;
    uint8_t *v;
    size_t y_cap;
    size_t u_cap;
    size_t v_cap;
} right_hist_entry_t;

typedef struct
{
    const uint8_t *y;
    const uint8_t *u;
    const uint8_t *v;
    ptrdiff_t stride_y;
    ptrdiff_t stride_c;
    int32_t frame_id;
    bool from_history;
} right_pick_t;

typedef struct
{
    uint64_t decode_call;
    uint64_t nal_idx;
    uint64_t frames_popped;
    uint64_t frames_queued;
    unsigned nal_type;
    unsigned prev_nal_type;
    unsigned nal_ref_idc;
    int ret_code;
    unsigned recover_action;
    bool wait_idr_after_flush;
    bool wait_idr_have_paramset;
    bool seen_subset_sps;
} edge264_soft_trace_t;

typedef struct
{
    void *dl_handle;

    const uint8_t *(*find_start_code)(const uint8_t *, const uint8_t *, int);
    Edge264Decoder *(*alloc)(int, void *, void *, int, void *, void *, void *);
    void (*flush)(Edge264Decoder *);
    void (*free_decoder)(Edge264Decoder **);
    int (*decode_NAL)(Edge264Decoder *, const uint8_t *, const uint8_t *, void *, void *);
    int (*get_frame)(Edge264Decoder *, Edge264Frame *, int);
    void (*return_frame)(Edge264Decoder *, void *);
} edge264_api_t;

typedef struct
{
    bool valid;
    uint8_t fps_raw;
    uint8_t fps_code;
    uint8_t count_hint_raw;
    uint8_t prefix[9];
    size_t prefix_len;
    uint8_t entries[EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX];
    size_t entry_count;
    open3d_subtitle_depth_state_t candidate_depth[EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX];
    uint64_t generation;
} edge264mvc_ofmd_state_t;

typedef struct
{
    vlc_mutex_t lock;
    edge264_api_t api;
    Edge264Decoder *decoder;

    uint8_t avcc_length_size;
    bool avcc_announced;
    uint8_t *avcc_pending;
    size_t avcc_pending_len;
    size_t avcc_pending_cap;
    bool annexb_announced;
    uint8_t *annexb_pending;
    size_t annexb_pending_len;
    size_t annexb_pending_cap;

    bool out_configured;
    unsigned out_width;
    unsigned out_height;
    bool out_stereo;

    vlc_tick_t ts_queue[EDGE264MVC_TS_QUEUE_CAP];
    uint64_t ts_queue_seq[EDGE264MVC_TS_QUEUE_CAP];
    unsigned ts_head;
    unsigned ts_count;
    uint64_t ts_next_seq;
    uint64_t ts_last_seq;
    vlc_tick_t ts_last_push;
    vlc_tick_t ts_last_pop;
    uint64_t ts_last_push_seq;
    uint64_t ts_last_pop_seq;
    vlc_tick_t ts_last_normalized;
    uint64_t ts_normalized_count;

    bool warned_missing_layout;
    bool warned_ts_overflow;
    bool warned_unsupported;
    bool warned_invalid;
    bool warned_drain_poll;
    bool warned_enobufs_guard;
    bool warned_avcc_length;
    bool warned_annexb_length;
    bool warned_reserved_nal;

    uint64_t blocks_seen;
    uint64_t nals_seen;
    uint64_t frames_popped;
    uint64_t frames_queued;
    uint64_t decode_calls;
    uint64_t decode_ret_zero;
    uint64_t decode_ret_enodata;
    uint64_t decode_ret_enobufs;
    uint64_t decode_ret_enotsup;
    uint64_t decode_ret_ebadmsg;
    uint64_t decode_ret_einval;
    uint64_t decode_ret_enomem;
    uint64_t decode_ret_other;
    uint64_t queue_calls;
    uint64_t queue_nomsg;
    uint64_t queue_einval;
    uint64_t drain_calls;
    uint64_t killed_short_circuit;
    uint64_t recover_flushes;
    uint64_t recover_flush_skips;
    uint64_t last_recover_decode_call;
    uint64_t skipped_terminal_ext_nals;
    uint64_t skipped_reserved_nals;
    uint64_t skipped_wait_idr_vcl;
    uint64_t enobufs_guard_flushes;
    uint64_t enobufs_guard_skipped_aux;
    uint64_t enobufs_drain_pulses;
    uint64_t enobufs_drain_progress;
    uint64_t enobufs_nal20;
    uint64_t enobufs_nal14;
    uint64_t enobufs_other;
    uint64_t enotsup_nal20;
    uint64_t enotsup_nal14;
    uint64_t enotsup_other;
    uint64_t ebadmsg_nal20;
    uint64_t ebadmsg_nal14;
    uint64_t ebadmsg_other;
    uint64_t first_enobufs_nal;
    unsigned first_enobufs_type;
    uint64_t first_ebadmsg_nal;
    unsigned first_ebadmsg_type;
    uint64_t first_enotsup_nal;
    unsigned first_enotsup_type;
    uint64_t frameid_samples;
    uint64_t frameid_nonzero_delta;
    uint64_t frameid_right_ahead;
    uint64_t frameid_left_ahead;
    int32_t frameid_last_delta;
    int32_t frameid_max_abs_delta;
    uint64_t selected_frameid_samples;
    uint64_t selected_frameid_nonzero_delta;
    uint64_t selected_frameid_right_ahead;
    uint64_t selected_frameid_left_ahead;
    int32_t selected_frameid_last_delta;
    int32_t selected_frameid_max_abs_delta;
    uint64_t selected_frameid_backsteps;
    uint64_t selected_frameid_repeats;
    uint64_t right_hist_selects;
    uint64_t right_hist_select_hits;
    uint64_t right_hist_select_exact;
    uint64_t right_hist_select_no_past;
    uint64_t right_hist_monotonic_forced_hold;
    uint64_t right_hist_monotonic_forced_current;
    uint64_t right_hist_monotonic_unresolved;
    uint64_t skipped_type20_nri0;
    uint64_t startup_gated_type20;
    uint64_t startup_seen_subset_sps;
    uint64_t soft_trace_dropped;
    unsigned soft_trace_count;
    unsigned last_nal_type;
    edge264_soft_trace_t soft_trace[EDGE264MVC_SOFT_TRACE_MAX];
    FILE *dump_annexb_fp;
    uint64_t dump_nals;
    bool warned_dump_io;

    unsigned recover_flush_min_decode_gap;
    unsigned enobufs_loop_max;
    unsigned enobufs_drain_period;
    unsigned enobufs_retry_sleep_us;
    bool recover_viewext_errors;
    bool advertise_multiview;
    bool log_frameids;
    bool log_sei;
    unsigned sei_payload37_logs;
    unsigned frameid_log_every;
    unsigned right_hist_depth;
    unsigned right_hist_head;
    bool skip_type20_nri0;
    bool gate_type20_until_subset_sps;
    unsigned gate_type20_max_skips;
    bool hold_right_on_backstep;
    bool use_pts_dates;
    unsigned ts_log_every;
    bool normalize_ts_cadence;
    int mkv_subtitle_plane;
    int mkv_subtitle_source_id;
    bool single_thread_mode;
    int alloc_thread_count;
    int current_thread_count;
    unsigned post_discontinuity_blocks;
    unsigned post_seek_debug_frames_left;
    unsigned post_seek_decode_debug_left;
    unsigned post_seek_require_stereo_drops_left;
    unsigned post_seek_soft_restart_budget;
    bool seen_subset_sps;
    bool warned_gate_limit;
    bool wait_idr_after_flush;
    bool wait_idr_have_paramset;
    bool wait_idr_allow_non_idr_once;
    bool extradata_seeded;
    bool skip_next_discontinuity_reopen;
    uint64_t post_seek_frames_queued;
    uint64_t post_seek_mono_frames;
    uint64_t post_seek_stereo_frames;
    uint64_t post_seek_mono_frames_dropped;
    uint64_t post_seek_invalid_date_frames;
    uint64_t post_seek_ts_backsteps;
    uint64_t post_seek_getframe_nomsg;
    uint64_t post_seek_drain_idle_loops;
    uint64_t post_seek_same_base_frame_repeats;
    uint64_t post_seek_same_right_frame_repeats;
    uint64_t hard_reopen_calls;
    uint64_t hard_reopen_from_flush;
    uint64_t hard_reopen_from_decode_discont;
    uint64_t hard_reopen_from_decode_corrupt;
    uint64_t hard_reopen_from_decode_other;
    vlc_tick_t post_seek_last_date;
    int32_t post_seek_last_base_frameid;
    int32_t post_seek_last_right_frameid;
    int32_t selected_right_last_frameid;
    uint32_t last_hard_reopen_flags;
    right_hist_entry_t right_hist[EDGE264MVC_RIGHT_HISTORY_MAX];
    bool ofmd_last_valid;
    uint8_t ofmd_last_prefix[9];
    size_t ofmd_last_prefix_len;
    uint8_t ofmd_last_entries[EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX];
    size_t ofmd_last_entry_count;
    unsigned ofmd_change_logs;
    uint64_t ofmd_samples_seen;
    edge264mvc_ofmd_state_t ofmd_extracted;
    open3d_subtitle_depth_state_t ofmd_selected_depth;
    int ofmd_selected_entry;
} edge264mvc_sys_t;

static void Edge264CoreLogCb(const char *str, void *log_arg)
{
    decoder_t *dec = (decoder_t *)log_arg;
    if (dec == NULL || str == NULL || *str == '\0')
        return;

    edge264mvc_sys_t *sys = (edge264mvc_sys_t *)dec->p_sys;
    if (sys == NULL || (!sys->log_frameids && !sys->log_sei))
        return;

    if (!sys->log_frameids && sys->log_sei)
    {
        if (strstr(str, "SEI") == NULL &&
            strstr(str, "sei_messages") == NULL &&
            strstr(str, "payloadType") == NULL &&
            strstr(str, "parse_SEI_result") == NULL)
            return;
    }

    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r'))
        len--;
    if (len == 0)
        return;
    msg_Dbg(dec, "edge264 core: %.*s", (int)len, str);
}

static size_t BuildRbspSample(const uint8_t *src, size_t src_len,
                              uint8_t *dst, size_t dst_cap)
{
    size_t out = 0;
    unsigned zero_run = 0;

    for (size_t i = 0; i < src_len && out < dst_cap; ++i)
    {
        const uint8_t byte = src[i];
        if (zero_run >= 2 && byte == 0x03)
        {
            zero_run = 0;
            continue;
        }

        dst[out++] = byte;
        if (byte == 0x00)
            zero_run++;
        else
            zero_run = 0;
    }

    return out;
}

typedef struct
{
    const uint8_t *buf;
    size_t len;
    size_t bitpos;
} open3d_bitreader_t;

static bool BitReadU(open3d_bitreader_t *br, unsigned bits, unsigned *out)
{
    if (br == NULL || out == NULL || bits > 31)
        return false;
    if ((br->len * 8u) - br->bitpos < bits)
        return false;

    unsigned value = 0;
    for (unsigned i = 0; i < bits; ++i)
    {
        const uint8_t byte = br->buf[br->bitpos >> 3];
        const unsigned shift = 7u - (unsigned)(br->bitpos & 7u);
        value = (value << 1) | ((byte >> shift) & 0x01u);
        br->bitpos++;
    }

    *out = value;
    return true;
}

static bool BitReadUE(open3d_bitreader_t *br, unsigned *out)
{
    unsigned leading_zero_bits = 0;
    unsigned bit = 0;
    while (true)
    {
        if (!BitReadU(br, 1, &bit))
            return false;
        if (bit != 0)
            break;
        leading_zero_bits++;
        if (leading_zero_bits > 30)
            return false;
    }

    if (leading_zero_bits == 0)
    {
        *out = 0;
        return true;
    }

    unsigned suffix = 0;
    if (!BitReadU(br, leading_zero_bits, &suffix))
        return false;
    *out = ((1u << leading_zero_bits) - 1u) + suffix;
    return true;
}

static size_t BitAlignZeroPad(open3d_bitreader_t *br)
{
    while ((br->bitpos & 7u) != 0)
    {
        unsigned pad = 0;
        if (!BitReadU(br, 1, &pad))
            break;
        if (pad != 0)
            break;
    }
    return br->bitpos >> 3;
}

static void BytesToHexSample(const uint8_t *buf, size_t len,
                             char *out, size_t out_cap)
{
    size_t pos = 0;

    if (out_cap == 0)
        return;

    out[0] = '\0';
    for (size_t i = 0; i < len; ++i)
    {
        const int written = snprintf(out + pos, out_cap - pos,
                                     "%s%02x", (i == 0) ? "" : " ", buf[i]);
        if (written < 0)
            break;
        if ((size_t)written >= out_cap - pos)
        {
            pos = out_cap - 1;
            break;
        }
        pos += (size_t)written;
    }
    out[pos] = '\0';
}

static void FormatUuid(const uint8_t *uuid, char *out, size_t out_cap)
{
    if (uuid == NULL || out == NULL || out_cap == 0)
        return;

    snprintf(out, out_cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9], uuid[10], uuid[11],
             uuid[12], uuid[13], uuid[14], uuid[15]);
}

static bool IsOfmdUuid(const uint8_t *uuid)
{
    static const uint8_t ofmd_uuid[16] = {
        0x17, 0xee, 0x8c, 0x60, 0xf8, 0x4d, 0x11, 0xd9,
        0x8c, 0xd6, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66,
    };
    return uuid != NULL && memcmp(uuid, ofmd_uuid, sizeof(ofmd_uuid)) == 0;
}

static void ClearOfmdExtractedState(edge264mvc_ofmd_state_t *state)
{
    if (state == NULL)
        return;

    memset(state, 0, sizeof(*state));
    for (size_t i = 0; i < EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX; ++i)
        Open3DSubtitleDepthStateClear(&state->candidate_depth[i]);
}

static void SetOfmdCandidateDepth(open3d_subtitle_depth_state_t *state,
                                  uint8_t raw, uint8_t sequence)
{
    Open3DSubtitleDepthStateClear(state);

    /*
     * The tracked MakeMKV sample uses 0x80 as the neutral/unset OFMD value.
     * Leave those entries invalid for now so later items can safely prefer the
     * static MKV fallback until a non-neutral dynamic depth is proven.
     */
    if (raw == 0x80)
        return;

    Open3DSubtitleDepthStateSet(state, true, sequence, raw, (int8_t)raw, -1);
}

static bool BuildOfmdProbeDesc(const uint8_t *payload, size_t payload_len,
                               int selected_plane,
                               char *out, size_t out_cap)
{
    if (payload == NULL || out == NULL || out_cap == 0)
        return false;
    if (payload_len < 4 || memcmp(payload, "OFMD", 4) != 0)
        return false;

    const uint8_t *data = payload + 4;
    const size_t data_len = payload_len - 4;
    const unsigned fps_raw = data_len > 0 ? data[0] : 0;
    const unsigned fps_code = fps_raw & 0x7f;
    const unsigned count_hint_raw = data_len > 6 ? data[6] : 0;
    const unsigned count_hint_entries = count_hint_raw & 0x7f;
    const size_t depth_off = data_len > 9 ? 9 : data_len;
    const size_t total_depth_bytes =
        (data_len > depth_off) ? (data_len - depth_off) : 0;
    size_t entry_count = total_depth_bytes;
    if (count_hint_entries > 0 && count_hint_entries <= total_depth_bytes)
        entry_count = count_hint_entries;
    if (entry_count > EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX)
        entry_count = EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX;
    const size_t depth_count = entry_count;
    const ssize_t direct_index =
        (selected_plane >= 0 && (size_t)selected_plane < entry_count) ?
        (ssize_t)selected_plane : -1;
    const ssize_t plus1_index =
        (selected_plane >= 0 && (size_t)(selected_plane + 1) < entry_count) ?
        (ssize_t)(selected_plane + 1) : -1;
    char depth_hex[3 * EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX];
    char prefix_hex[3 * 9];
    char direct_hex[8];
    char plus1_hex[8];
    depth_hex[0] = '\0';
    prefix_hex[0] = '\0';
    strcpy(direct_hex, "n/a");
    strcpy(plus1_hex, "n/a");
    if (data_len > 0)
    {
        const size_t prefix_count = data_len < 9 ? data_len : 9;
        BytesToHexSample(data, prefix_count, prefix_hex, sizeof(prefix_hex));
    }
    if (depth_count > 0)
        BytesToHexSample(data + depth_off, depth_count, depth_hex, sizeof(depth_hex));
    if (direct_index >= 0)
        snprintf(direct_hex, sizeof(direct_hex), "0x%02x",
                 data[depth_off + (size_t)direct_index]);
    if (plus1_index >= 0)
        snprintf(plus1_hex, sizeof(plus1_hex), "0x%02x",
                 data[depth_off + (size_t)plus1_index]);

    snprintf(out, out_cap,
             "sig=OFMD payload=%zu fps_raw=0x%02x fps_code=%u count_hint@+6=0x%02x entries=%u prefix@+0=%s depth@+9=%s direct[p=%d]=%s plus1[p=%d]=%s tail=%zu",
             payload_len, fps_raw, fps_code, count_hint_raw, count_hint_entries,
             prefix_hex[0] != '\0' ? prefix_hex : "none",
             depth_hex[0] != '\0' ? depth_hex : "none",
             selected_plane, direct_hex,
             selected_plane, plus1_hex,
             total_depth_bytes > entry_count ? (total_depth_bytes - entry_count) : 0u);
    return true;
}

static bool ParseOfmdEntrySample(const uint8_t *payload, size_t payload_len,
                                 uint8_t prefix[9], size_t *prefix_len,
                                 uint8_t entries[EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX],
                                 size_t *entry_count)
{
    if (prefix_len != NULL)
        *prefix_len = 0;
    if (entry_count != NULL)
        *entry_count = 0;
    if (payload == NULL || payload_len < 4 || memcmp(payload, "OFMD", 4) != 0)
        return false;

    const uint8_t *data = payload + 4;
    const size_t data_len = payload_len - 4;
    const unsigned count_hint_raw = data_len > 6 ? data[6] : 0;
    const unsigned count_hint_entries = count_hint_raw & 0x7f;
    const size_t depth_off = data_len > 9 ? 9 : data_len;
    const size_t total_depth_bytes =
        (data_len > depth_off) ? (data_len - depth_off) : 0;
    size_t sampled_entries = total_depth_bytes;
    if (count_hint_entries > 0 && count_hint_entries <= total_depth_bytes)
        sampled_entries = count_hint_entries;
    if (sampled_entries > EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX)
        sampled_entries = EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX;

    if (prefix != NULL && prefix_len != NULL)
    {
        *prefix_len = data_len < 9 ? data_len : 9;
        if (*prefix_len > 0)
            memcpy(prefix, data, *prefix_len);
    }
    if (entries != NULL && entry_count != NULL)
    {
        *entry_count = sampled_entries;
        if (sampled_entries > 0)
            memcpy(entries, data + depth_off, sampled_entries);
    }
    return true;
}

static bool ParseOfmdExtractedState(const uint8_t *payload, size_t payload_len,
                                    edge264mvc_ofmd_state_t *state)
{
    if (state == NULL)
        return false;

    ClearOfmdExtractedState(state);
    if (payload == NULL || payload_len < 4 || memcmp(payload, "OFMD", 4) != 0)
        return false;

    const uint8_t *data = payload + 4;
    const size_t data_len = payload_len - 4;

    state->valid = true;
    state->fps_raw = data_len > 0 ? data[0] : 0;
    state->fps_code = state->fps_raw & 0x7f;
    state->count_hint_raw = data_len > 6 ? data[6] : 0;

    if (!ParseOfmdEntrySample(payload, payload_len,
                              state->prefix, &state->prefix_len,
                              state->entries, &state->entry_count))
    {
        ClearOfmdExtractedState(state);
        return false;
    }

    for (size_t i = 0; i < state->entry_count; ++i)
        SetOfmdCandidateDepth(&state->candidate_depth[i], state->entries[i], (uint8_t)i);

    return true;
}

static void BuildOfmdCandidateDepthDesc(const edge264mvc_ofmd_state_t *state,
                                        char *out, size_t out_cap)
{
    size_t pos = 0;

    if (out == NULL || out_cap == 0)
        return;
    out[0] = '\0';
    if (state == NULL || !state->valid)
        return;

    for (size_t i = 0; i < state->entry_count; ++i)
    {
        const open3d_subtitle_depth_state_t *depth = &state->candidate_depth[i];
        const int written = snprintf(out + pos, out_cap - pos,
                                     "%s%zu:%s(raw=0x%02x signed=%d)",
                                     (i == 0) ? "" : ", ",
                                     i,
                                     depth->valid ? "valid" : "neutral",
                                     state->entries[i],
                                     depth->valid ? depth->signed_offset : 0);
        if (written < 0)
            break;
        if ((size_t)written >= out_cap - pos)
        {
            pos = out_cap - 1;
            break;
        }
        pos += (size_t)written;
    }
    out[pos] = '\0';
}

static bool HasMkvSubtitleMapping(const edge264mvc_sys_t *sys)
{
    return sys != NULL &&
           (sys->mkv_subtitle_plane >= 0 || sys->mkv_subtitle_source_id >= 0);
}

static void ReadMkvSubtitleMapping(decoder_t *dec,
                                   bool *bridge_present_out,
                                   int *plane_out,
                                   int *source_id_out)
{
    if (bridge_present_out != NULL)
        *bridge_present_out = false;
    if (plane_out != NULL)
        *plane_out = -1;
    if (source_id_out != NULL)
        *source_id_out = -1;

    if (dec == NULL)
        return;

    vlc_object_t *bridge_owner =
        dec->obj.libvlc != NULL ? VLC_OBJECT(dec->obj.libvlc) : NULL;
    bool bridge_available = false;
    bool bridge_force = false;
    int plane = -1;
    int source_id = -1;
    if (Open3DMkvSubtitleBridgeReadFromObject(bridge_owner, &bridge_available,
                                              &bridge_force, NULL,
                                              &plane, &source_id) &&
        bridge_available)
    {
        if (bridge_present_out != NULL)
            *bridge_present_out = true;
        if (plane_out != NULL)
            *plane_out = bridge_force ? plane : -1;
        if (source_id_out != NULL)
            *source_id_out = bridge_force ? source_id : -1;
        return;
    }

    if (plane_out != NULL)
        *plane_out = var_InheritInteger(dec, "open3d-mkv-subtitle-plane");
    if (source_id_out != NULL)
        *source_id_out = var_InheritInteger(dec, "open3d-mkv-subtitle-source-id");
}

static bool RefreshMkvSubtitleMappingFromVars(decoder_t *dec,
                                              edge264mvc_sys_t *sys,
                                              bool log_changes)
{
    if (dec == NULL || sys == NULL)
        return false;

    bool bridge_present = false;
    int plane = -1;
    int source_id = -1;
    ReadMkvSubtitleMapping(dec, &bridge_present, &plane, &source_id);
    if (plane == sys->mkv_subtitle_plane &&
        source_id == sys->mkv_subtitle_source_id)
        return false;

    sys->mkv_subtitle_plane = plane;
    sys->mkv_subtitle_source_id = source_id;
    if (log_changes)
    {
        msg_Dbg(dec,
                "edge264mvc refreshed MKV subtitle mapping plane=%d source_id=%d via=%s",
                sys->mkv_subtitle_plane,
                sys->mkv_subtitle_source_id,
                bridge_present ? "bridge" : "inherited");
    }
    return true;
}

static void PublishSelectedMkvOfmdDepth(decoder_t *dec, edge264mvc_sys_t *sys)
{
    if (dec == NULL || sys == NULL)
        return;

    /*
     * The decoder and the active vout share the input thread as a common
     * ancestor. Publish the selected MKV depth state there so the display
     * side can inherit it without depending on non-exported input internals.
     */
    vlc_object_t *shared_owner = dec->obj.parent;
    if (shared_owner != NULL)
        Open3DSubtitleDepthPublishToObject(shared_owner, &sys->ofmd_selected_depth);

    /*
     * Keep one root-level safety anchor too. The display side already walks the
     * VLC object ancestry, and this avoids depending on exact decoder/vout
     * reuse details while the source-specific MKV mapping stays outside here.
     */
    if (dec->obj.libvlc != NULL &&
        dec->obj.libvlc != (libvlc_int_t *)shared_owner)
        Open3DSubtitleDepthPublishToObject(VLC_OBJECT(dec->obj.libvlc),
                                           &sys->ofmd_selected_depth);
}

static void RefreshSelectedMkvOfmdDepth(decoder_t *dec, edge264mvc_sys_t *sys)
{
    RefreshMkvSubtitleMappingFromVars(dec, sys, true);
    Open3DSubtitleDepthStateClear(&sys->ofmd_selected_depth);
    sys->ofmd_selected_entry = -1;

    if (!sys->ofmd_extracted.valid)
    {
        PublishSelectedMkvOfmdDepth(dec, sys);
        return;
    }
    if (!Open3DMkvSubtitleDepthSelectPlane(sys->ofmd_extracted.candidate_depth,
                                           sys->ofmd_extracted.entry_count,
                                           sys->mkv_subtitle_plane,
                                           &sys->ofmd_selected_depth,
                                           &sys->ofmd_selected_entry))
    {
        msg_Dbg(dec,
                "edge264mvc selected MKV OFMD depth unresolved generation=%" PRIu64
                " plane=%d source_id=%d entries=%zu",
                sys->ofmd_extracted.generation,
                sys->mkv_subtitle_plane,
                sys->mkv_subtitle_source_id,
                sys->ofmd_extracted.entry_count);
        PublishSelectedMkvOfmdDepth(dec, sys);
        return;
    }

    msg_Dbg(dec,
            "edge264mvc selected MKV OFMD depth generation=%" PRIu64
            " plane=%d source_id=%d entry=%d valid=%d raw=0x%02x signed=%d",
            sys->ofmd_extracted.generation,
            sys->mkv_subtitle_plane,
            sys->mkv_subtitle_source_id,
            sys->ofmd_selected_entry,
            sys->ofmd_selected_depth.valid ? 1 : 0,
            sys->ofmd_selected_depth.raw,
            sys->ofmd_selected_depth.valid ? sys->ofmd_selected_depth.signed_offset : 0);

    PublishSelectedMkvOfmdDepth(dec, sys);
}

static void MaybeLogOfmdChange(decoder_t *dec, edge264mvc_sys_t *sys,
                               const uint8_t *payload, size_t payload_len)
{
    if (dec == NULL || sys == NULL || payload == NULL || payload_len == 0)
        return;

    uint8_t prefix[9];
    size_t prefix_len = 0;
    uint8_t entries[EDGE264MVC_OFMD_ENTRY_SAMPLE_MAX];
    size_t entry_count = 0;
    if (!ParseOfmdEntrySample(payload, payload_len,
                              prefix, &prefix_len,
                              entries, &entry_count))
        return;

    sys->ofmd_samples_seen++;

    bool changed = !sys->ofmd_last_valid ||
                   sys->ofmd_last_prefix_len != prefix_len ||
                   sys->ofmd_last_entry_count != entry_count ||
                   (prefix_len > 0 &&
                    memcmp(sys->ofmd_last_prefix, prefix, prefix_len) != 0) ||
                   (entry_count > 0 &&
                    memcmp(sys->ofmd_last_entries, entries, entry_count) != 0);
    if (!changed)
        return;

    char desc[320];
    char candidate_desc[256];
    edge264mvc_ofmd_state_t extracted;
    if (!BuildOfmdProbeDesc(payload, payload_len, sys->mkv_subtitle_plane,
                            desc, sizeof(desc)))
        return;
    if (!ParseOfmdExtractedState(payload, payload_len, &extracted))
        return;

    sys->ofmd_last_valid = true;
    sys->ofmd_last_prefix_len = prefix_len;
    sys->ofmd_last_entry_count = entry_count;
    if (prefix_len > 0)
        memcpy(sys->ofmd_last_prefix, prefix, prefix_len);
    if (entry_count > 0)
        memcpy(sys->ofmd_last_entries, entries, entry_count);
    extracted.generation = sys->ofmd_samples_seen;
    sys->ofmd_extracted = extracted;
    RefreshSelectedMkvOfmdDepth(dec, sys);

    if (!sys->log_sei || sys->ofmd_change_logs >= EDGE264MVC_OFMD_LOG_MAX)
        return;

    sys->ofmd_change_logs++;
    BuildOfmdCandidateDepthDesc(&sys->ofmd_extracted,
                                candidate_desc, sizeof(candidate_desc));

    msg_Info(dec,
             "edge264mvc ofmd change=%u seen=%" PRIu64
             " nals=%" PRIu64 " frames=%" PRIu64
             " plane=%d source_id=%d %s",
             sys->ofmd_change_logs, sys->ofmd_samples_seen,
             sys->nals_seen, sys->frames_queued,
             sys->mkv_subtitle_plane, sys->mkv_subtitle_source_id,
             desc);
    msg_Dbg(dec,
            "edge264mvc ofmd extracted generation=%" PRIu64
            " entries=%zu candidates=[%s]",
            sys->ofmd_extracted.generation,
            sys->ofmd_extracted.entry_count,
            candidate_desc[0] != '\0' ? candidate_desc : "none");
}

static void MaybeLogSeiPayloadProbe(decoder_t *dec, edge264mvc_sys_t *sys,
                                    const uint8_t *nal, const uint8_t *end)
{
    if (dec == NULL || sys == NULL || nal == NULL || end == NULL || nal >= end)
        return;
    RefreshMkvSubtitleMappingFromVars(dec, sys, false);
    const bool want_ofmd_probe = HasMkvSubtitleMapping(sys) ||
                                 (sys->log_sei && sys->ofmd_change_logs < EDGE264MVC_OFMD_LOG_MAX);
    if (!sys->log_sei && !want_ofmd_probe)
        return;
    const bool want_sei37_summary = sys->log_sei && sys->sei_payload37_logs < 16;
    const bool want_ofmd_change_log = want_ofmd_probe;
    if (!want_sei37_summary && !want_ofmd_change_log)
        return;
    if ((nal[0] & 0x1f) != 6)
        return;

    uint8_t rbsp[1024];
    const size_t nal_payload_len = (size_t)(end - nal - 1);
    const size_t rbsp_len = BuildRbspSample(nal + 1, nal_payload_len,
                                            rbsp, sizeof(rbsp));
    size_t pos = 0;

    while (pos + 1 < rbsp_len)
    {
        unsigned payload_type = 0;
        unsigned payload_size = 0;

        while (pos < rbsp_len)
        {
            const uint8_t byte = rbsp[pos++];
            payload_type += byte;
            if (byte != 0xff)
                break;
        }
        while (pos < rbsp_len)
        {
            const uint8_t byte = rbsp[pos++];
            payload_size += byte;
            if (byte != 0xff)
                break;
        }

        if (payload_size > rbsp_len - pos)
            payload_size = (unsigned)(rbsp_len - pos);

        if (payload_type == 37)
        {
            open3d_bitreader_t br = {
                .buf = rbsp + pos,
                .len = payload_size,
                .bitpos = 0,
            };
            unsigned operation_point_flag = 0;
            unsigned all_views_in_au_flag = 0;
            unsigned num_view_components = 0;
            unsigned view_ids[4] = { 0 };
            unsigned sei_op_temporal_id = 0;
            size_t view_count = 0;
            const char *parse_state = "ok";
            char nested_desc[192];
            nested_desc[0] = '\0';

            if (!BitReadU(&br, 1, &operation_point_flag))
            {
                parse_state = "truncated_op_flag";
            }
            else if (operation_point_flag == 0)
            {
                if (!BitReadU(&br, 1, &all_views_in_au_flag))
                    parse_state = "truncated_all_views";
                else if (all_views_in_au_flag == 0)
                {
                    if (!BitReadUE(&br, &num_view_components))
                        parse_state = "truncated_num_views";
                    else
                    {
                        view_count = (size_t)num_view_components + 1u;
                        for (size_t i = 0; i < view_count; ++i)
                        {
                            unsigned ignored_flag = 0;
                            if (!BitReadU(&br, 10, &view_ids[i < 4 ? i : 3]) ||
                                !BitReadU(&br, 1, &ignored_flag))
                            {
                                parse_state = "truncated_view_list";
                                break;
                            }
                        }
                    }
                }
            }
            else
            {
                if (!BitReadUE(&br, &num_view_components))
                    parse_state = "truncated_num_op_views";
                else
                {
                    view_count = (size_t)num_view_components + 1u;
                    for (size_t i = 0; i < view_count; ++i)
                    {
                        if (!BitReadU(&br, 10, &view_ids[i < 4 ? i : 3]))
                        {
                            parse_state = "truncated_op_view_list";
                            break;
                        }
                    }
                    if (strcmp(parse_state, "ok") == 0 &&
                        !BitReadU(&br, 3, &sei_op_temporal_id))
                        parse_state = "truncated_temporal_id";
                }
            }

            size_t nested_pos = 0;
            if (strcmp(parse_state, "ok") == 0)
                nested_pos = BitAlignZeroPad(&br);

            if (strcmp(parse_state, "ok") == 0 && nested_pos < payload_size)
            {
                size_t desc_pos = 0;
                const uint8_t *nested = rbsp + pos + nested_pos;
                size_t nested_len = payload_size - nested_pos;
                size_t nested_off = 0;
                unsigned nested_index = 0;
                while (nested_off + 1 < nested_len && nested_index < 3)
                {
                    unsigned nested_type = 0;
                    unsigned nested_size = 0;
                    uint8_t byte = 0;

                    do {
                        byte = nested[nested_off++];
                        nested_type += byte;
                    } while (byte == 0xff && nested_off < nested_len);

                    do {
                        if (nested_off >= nested_len)
                            break;
                        byte = nested[nested_off++];
                        nested_size += byte;
                    } while (byte == 0xff && nested_off < nested_len);

                    if (nested_size > nested_len - nested_off)
                        nested_size = (unsigned)(nested_len - nested_off);

                    if (nested_index > 0 && desc_pos + 2 < sizeof(nested_desc))
                    {
                        nested_desc[desc_pos++] = ',';
                        nested_desc[desc_pos++] = ' ';
                        nested_desc[desc_pos] = '\0';
                    }

                    if (nested_type == 5 && nested_size >= 16)
                    {
                        char uuid[40];
                        char ofmd[256];
                        ofmd[0] = '\0';
                        FormatUuid(nested + nested_off, uuid, sizeof(uuid));
                        if (IsOfmdUuid(nested + nested_off) && nested_size > 16)
                        {
                            MaybeLogOfmdChange(dec, sys,
                                               nested + nested_off + 16,
                                               nested_size - 16);
                            if (want_sei37_summary)
                            {
                                BuildOfmdProbeDesc(nested + nested_off + 16,
                                                   nested_size - 16,
                                                   sys->mkv_subtitle_plane,
                                                   ofmd, sizeof(ofmd));
                            }
                        }
                        desc_pos += (size_t)snprintf(
                            nested_desc + desc_pos, sizeof(nested_desc) - desc_pos,
                            "type=5 uuid=%s %s", uuid,
                            ofmd[0] != '\0' ? ofmd : "sig=n/a");
                    }
                    else
                    {
                        desc_pos += (size_t)snprintf(
                            nested_desc + desc_pos, sizeof(nested_desc) - desc_pos,
                            "type=%u size=%u", nested_type, nested_size);
                    }

                    nested_off += nested_size;
                    nested_index++;
                }
            }

            if (want_sei37_summary)
            {
                sys->sei_payload37_logs++;
                if (strcmp(parse_state, "ok") == 0)
                {
                    msg_Info(dec,
                             "edge264mvc sei37 sample=%u size=%u op=%u all_views=%u views=%zu first_view=%u temporal_id=%u nested=[%s]",
                             sys->sei_payload37_logs, payload_size,
                             operation_point_flag, all_views_in_au_flag,
                             view_count, view_count > 0 ? view_ids[0] : 0,
                             sei_op_temporal_id,
                             nested_desc[0] != '\0' ? nested_desc : "none");
                }
                else
                {
                    char hex_sample[3 * 24];
                    const size_t sample_len = payload_size < 24 ? payload_size : 24;
                    BytesToHexSample(rbsp + pos, sample_len, hex_sample, sizeof(hex_sample));
                    msg_Info(dec,
                             "edge264mvc sei37 sample=%u size=%u parse=%s bytes=%s%s",
                             sys->sei_payload37_logs, payload_size, parse_state,
                             hex_sample[0] != '\0' ? hex_sample : "(empty)",
                             (sample_len < payload_size) ? " ..." : "");
                }
            }
        }

        pos += payload_size;
    }
}

static int OpenDecoder(vlc_object_t *);
static void CloseDecoder(vlc_object_t *);
static int DecodeBlock(decoder_t *, block_t *);
static void FlushDecoder(decoder_t *);
static void FlushDecoderLocked(edge264mvc_sys_t *);
static void ArmPostSeekRecovery(edge264mvc_sys_t *, bool);
static Edge264Decoder *AllocEdge264Decoder(decoder_t *, edge264mvc_sys_t *, int);
static int ReopenDecoderLocked(decoder_t *, edge264mvc_sys_t *, const char *, uint32_t);
static int FeedExtradata(decoder_t *, edge264mvc_sys_t *);

vlc_module_begin()
    set_shortname(N_("edge264mvc"))
    set_description(N_("edge264 MVC video decoder"))
    set_capability("video decoder", 1)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_VCODEC)
    add_shortcut("edge264mvc")
    add_string("edge264mvc-lib", "", EDGE264MVC_LIB_PATH_TEXT,
               EDGE264MVC_LIB_PATH_LONGTEXT, true)
    add_integer("edge264mvc-threads", -1, EDGE264MVC_THREADS_TEXT,
                EDGE264MVC_THREADS_LONGTEXT, true)
    add_string("edge264mvc-dump-annexb", "", EDGE264MVC_DUMP_ANNEXB_TEXT,
               EDGE264MVC_DUMP_ANNEXB_LONGTEXT, true)
    add_integer("edge264mvc-recover-gap", EDGE264MVC_DEFAULT_RECOVER_FLUSH_MIN_DECODE_GAP,
                EDGE264MVC_RECOVER_GAP_TEXT, EDGE264MVC_RECOVER_GAP_LONGTEXT, true)
    add_integer("edge264mvc-enobufs-loop-max", EDGE264MVC_DEFAULT_ENOBUFS_LOOP_MAX,
                EDGE264MVC_ENOBUFS_LOOP_TEXT, EDGE264MVC_ENOBUFS_LOOP_LONGTEXT, true)
    add_integer("edge264mvc-enobufs-drain-period", EDGE264MVC_DEFAULT_ENOBUFS_DRAIN_PERIOD,
                EDGE264MVC_ENOBUFS_DRAIN_PERIOD_TEXT,
                EDGE264MVC_ENOBUFS_DRAIN_PERIOD_LONGTEXT, true)
    add_integer("edge264mvc-enobufs-sleep-us", EDGE264MVC_DEFAULT_ENOBUFS_SLEEP_US,
                EDGE264MVC_ENOBUFS_SLEEP_US_TEXT, EDGE264MVC_ENOBUFS_SLEEP_US_LONGTEXT, true)
    add_bool("edge264mvc-recover-viewext-errors", false,
             EDGE264MVC_RECOVER_VIEWEXT_TEXT, EDGE264MVC_RECOVER_VIEWEXT_LONGTEXT, true)
    add_bool("edge264mvc-advertise-multiview", true,
             EDGE264MVC_ADVERTISE_MULTIVIEW_TEXT,
             EDGE264MVC_ADVERTISE_MULTIVIEW_LONGTEXT, true)
    add_bool("edge264mvc-log-frameids", false,
             EDGE264MVC_LOG_FRAMEIDS_TEXT,
             EDGE264MVC_LOG_FRAMEIDS_LONGTEXT, true)
    add_bool("edge264mvc-log-sei", false,
             EDGE264MVC_LOG_SEI_TEXT,
             EDGE264MVC_LOG_SEI_LONGTEXT, true)
    add_integer("edge264mvc-frameid-log-every", 120,
                EDGE264MVC_FRAMEID_LOG_EVERY_TEXT,
                EDGE264MVC_FRAMEID_LOG_EVERY_LONGTEXT, true)
    add_integer("edge264mvc-right-history-depth", 0,
                EDGE264MVC_RIGHT_HIST_DEPTH_TEXT,
                EDGE264MVC_RIGHT_HIST_DEPTH_LONGTEXT, true)
    add_bool("edge264mvc-skip-type20-nri0", false,
             EDGE264MVC_SKIP_TYPE20_NRI0_TEXT,
             EDGE264MVC_SKIP_TYPE20_NRI0_LONGTEXT, true)
    add_bool("edge264mvc-gate-type20-until-subset-sps", true,
             EDGE264MVC_GATE_TYPE20_TEXT,
             EDGE264MVC_GATE_TYPE20_LONGTEXT, true)
    add_integer("edge264mvc-gate-type20-max-skips", 512,
                EDGE264MVC_GATE_TYPE20_MAX_TEXT,
                EDGE264MVC_GATE_TYPE20_MAX_LONGTEXT, true)
    add_bool("edge264mvc-hold-right-on-backstep", false,
             EDGE264MVC_HOLD_RIGHT_BACKSTEP_TEXT,
             EDGE264MVC_HOLD_RIGHT_BACKSTEP_LONGTEXT, true)
    add_bool("edge264mvc-use-pts-dates", false,
             EDGE264MVC_USE_PTS_DATES_TEXT,
             EDGE264MVC_USE_PTS_DATES_LONGTEXT, true)
    add_integer("edge264mvc-ts-log-every", 0,
                EDGE264MVC_TS_LOG_EVERY_TEXT,
                EDGE264MVC_TS_LOG_EVERY_LONGTEXT, true)
    add_bool("edge264mvc-normalize-ts-cadence", false,
             EDGE264MVC_NORMALIZE_TS_CADENCE_TEXT,
             EDGE264MVC_NORMALIZE_TS_CADENCE_LONGTEXT, true)
    set_callbacks(OpenDecoder, CloseDecoder)
vlc_module_end()

#if defined(OPEN3D_VLC_ABI_ALIAS_T64)
/*
 * Debian/Ubuntu VLC 3.0.x variants may look for "vlc_entry__3_0_0ft64"
 * while upstream VLC 3.0.23 modules export "vlc_entry__3_0_0f".
 * This opt-in compatibility shim exports the t64 entry symbol and forwards
 * to the upstream entry point.
 */
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

static bool IsExplicitOptIn(decoder_t *dec)
{
    if (dec->fmt_in.i_codec == OPEN3DBLURAYMVC_CODEC_MVC ||
        dec->fmt_in.i_codec == OPEN3DMKV_CODEC_H264)
        return true;

    char *codec_chain = var_InheritString(dec, "codec");
    if (codec_chain == NULL)
        return false;

    bool selected = false;
    char *saveptr = NULL;
    for (char *tok = strtok_r(codec_chain, ",", &saveptr);
         tok != NULL;
         tok = strtok_r(NULL, ",", &saveptr))
    {
        while (*tok == ' ' || *tok == '\t')
            ++tok;

        if (*tok == '\0')
            continue;

        if (!strcasecmp(tok, "edge264mvc") || !strcasecmp(tok, "edge264-mvc"))
        {
            selected = true;
            break;
        }
    }

    free(codec_chain);
    return selected;
}

static void TsQueueReset(edge264mvc_sys_t *sys)
{
    sys->ts_head = 0;
    sys->ts_count = 0;
    sys->ts_next_seq = 0;
    sys->ts_last_seq = 0;
    sys->ts_last_push = VLC_TICK_INVALID;
    sys->ts_last_pop = VLC_TICK_INVALID;
    sys->ts_last_push_seq = 0;
    sys->ts_last_pop_seq = 0;
    sys->ts_last_normalized = VLC_TICK_INVALID;
    sys->ts_normalized_count = 0;
}

static vlc_tick_t ExpectedFramePeriod(const decoder_t *dec)
{
    unsigned rate = 0;
    unsigned base = 0;

    if (dec->fmt_out.video.i_frame_rate > 0 &&
        dec->fmt_out.video.i_frame_rate_base > 0)
    {
        rate = dec->fmt_out.video.i_frame_rate;
        base = dec->fmt_out.video.i_frame_rate_base;
    }
    else if (dec->fmt_in.video.i_frame_rate > 0 &&
             dec->fmt_in.video.i_frame_rate_base > 0)
    {
        rate = dec->fmt_in.video.i_frame_rate;
        base = dec->fmt_in.video.i_frame_rate_base;
    }

    if (rate == 0 || base == 0)
        return VLC_TICK_INVALID;

    return ((vlc_tick_t)VLC_TICK_FROM_SEC(1) * base) / rate;
}

static void MaybeLogTsCadence(decoder_t *dec, edge264mvc_sys_t *sys,
                              bool push, uint64_t seq, vlc_tick_t ts,
                              unsigned depth)
{
    if (sys->ts_log_every == 0 || ts <= VLC_TICK_INVALID || seq == 0)
        return;

    vlc_tick_t *last_ts = push ? &sys->ts_last_push : &sys->ts_last_pop;
    uint64_t *last_seq = push ? &sys->ts_last_push_seq : &sys->ts_last_pop_seq;
    const char *stage = push ? "push" : "pop";

    if (*last_ts > VLC_TICK_INVALID && *last_seq > 0)
    {
        vlc_tick_t delta = ts - *last_ts;
        uint64_t seq_delta = seq - *last_seq;
        vlc_tick_t expected = ExpectedFramePeriod(dec);
        bool anomaly = seq_delta != 1;

        if (expected > VLC_TICK_INVALID)
        {
            vlc_tick_t abs_err = delta >= expected ? delta - expected : expected - delta;
            if (abs_err > expected / 2)
                anomaly = true;
        }

        if (anomaly || (seq % sys->ts_log_every) == 0)
        {
            msg_Dbg(dec,
                    "edge264mvc ts cadence %s seq=%" PRIu64 " ts=%" PRId64
                    " delta=%" PRId64 " seq_delta=%" PRIu64
                    " expected=%" PRId64 " depth=%u anomaly=%d",
                    stage, seq, (int64_t)ts, (int64_t)delta, seq_delta,
                    (int64_t)expected, depth, anomaly ? 1 : 0);
        }
    }

    *last_ts = ts;
    *last_seq = seq;
}

static vlc_tick_t NormalizeQueuedTimestamp(decoder_t *dec, edge264mvc_sys_t *sys,
                                           vlc_tick_t ts)
{
    if (!sys->normalize_ts_cadence || ts <= VLC_TICK_INVALID)
        return ts;

    vlc_tick_t expected = ExpectedFramePeriod(dec);
    if (expected <= VLC_TICK_INVALID)
        return ts;

    if (sys->ts_last_normalized <= VLC_TICK_INVALID)
    {
        sys->ts_last_normalized = ts;
        return ts;
    }

    vlc_tick_t delta = ts - sys->ts_last_normalized;
    vlc_tick_t abs_err = delta >= expected ? delta - expected : expected - delta;
    if (delta > 0 && abs_err <= expected / 2)
    {
        sys->ts_last_normalized = ts;
        return ts;
    }

    vlc_tick_t normalized = sys->ts_last_normalized + expected;
    sys->ts_last_normalized = normalized;
    sys->ts_normalized_count++;

    if (sys->ts_log_every > 0 &&
        (sys->ts_normalized_count <= 16 ||
         (sys->ts_normalized_count % sys->ts_log_every) == 0))
    {
        msg_Dbg(dec,
                "edge264mvc normalized ts raw=%" PRId64 " delta=%" PRId64
                " expected=%" PRId64 " normalized=%" PRId64 " count=%" PRIu64,
                (int64_t)ts, (int64_t)delta, (int64_t)expected,
                (int64_t)normalized, sys->ts_normalized_count);
    }

    return normalized;
}

static void TsQueuePush(decoder_t *dec, edge264mvc_sys_t *sys, vlc_tick_t ts)
{
    if (ts <= VLC_TICK_INVALID)
        return;

    ts = NormalizeQueuedTimestamp(dec, sys, ts);

    if (sys->ts_count == EDGE264MVC_TS_QUEUE_CAP)
    {
        if (!sys->warned_ts_overflow)
        {
            msg_Warn(dec, "edge264mvc timestamp queue overflow, dropping oldest timestamps");
            sys->warned_ts_overflow = true;
        }
        sys->ts_head = (sys->ts_head + 1) % EDGE264MVC_TS_QUEUE_CAP;
        sys->ts_count--;
    }

    unsigned tail = (sys->ts_head + sys->ts_count) % EDGE264MVC_TS_QUEUE_CAP;
    sys->ts_queue[tail] = ts;
    sys->ts_queue_seq[tail] = ++sys->ts_next_seq;
    sys->ts_count++;
    MaybeLogTsCadence(dec, sys, true, sys->ts_next_seq, ts, sys->ts_count);
}

static vlc_tick_t TsQueuePop(decoder_t *dec, edge264mvc_sys_t *sys, uint64_t *seq_out)
{
    if (sys->ts_count == 0)
    {
        if (seq_out != NULL)
            *seq_out = 0;
        return VLC_TICK_INVALID;
    }

    vlc_tick_t ts = sys->ts_queue[sys->ts_head];
    uint64_t seq = sys->ts_queue_seq[sys->ts_head];
    sys->ts_head = (sys->ts_head + 1) % EDGE264MVC_TS_QUEUE_CAP;
    sys->ts_count--;
    sys->ts_last_seq = seq;
    if (seq_out != NULL)
        *seq_out = seq;
    MaybeLogTsCadence(dec, sys, false, seq, ts, sys->ts_count);
    return ts;
}

static bool BufferHasStartCode(const uint8_t *buf, size_t len)
{
    if (len < 3)
        return false;

    for (size_t i = 0; i + 3 <= len; ++i)
    {
        if (buf[i] == 0 && buf[i + 1] == 0)
        {
            if (buf[i + 2] == 1)
                return true;
            if (i + 4 <= len && buf[i + 2] == 0 && buf[i + 3] == 1)
                return true;
        }
    }

    return false;
}

static size_t StartCodeLen(const uint8_t *p, const uint8_t *end)
{
    if (end - p >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
        return 4;
    if (end - p >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1)
        return 3;
    return 0;
}

static uint32_t ReadBE(const uint8_t *p, uint8_t n)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < n; ++i)
        v = (v << 8) | p[i];
    return v;
}

static void AvccResetPending(edge264mvc_sys_t *sys)
{
    sys->avcc_pending_len = 0;
}

static int AvccEnsurePendingCapacity(edge264mvc_sys_t *sys, size_t need)
{
    if (need <= sys->avcc_pending_cap)
        return VLCDEC_SUCCESS;

    size_t new_cap = sys->avcc_pending_cap;
    if (new_cap < 4096)
        new_cap = 4096;
    while (new_cap < need)
    {
        if (new_cap > (SIZE_MAX / 2))
        {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }

    uint8_t *new_buf = realloc(sys->avcc_pending, new_cap);
    if (new_buf == NULL)
        return VLCDEC_ECRITICAL;

    sys->avcc_pending = new_buf;
    sys->avcc_pending_cap = new_cap;
    return VLCDEC_SUCCESS;
}

static int AvccAppendPending(decoder_t *dec, edge264mvc_sys_t *sys,
                             const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return VLCDEC_SUCCESS;

    if (sys->avcc_pending_len > SIZE_MAX - len)
    {
        msg_Err(dec, "edge264mvc AVCC pending buffer size overflow");
        return VLCDEC_ECRITICAL;
    }

    size_t need = sys->avcc_pending_len + len;
    if (need > EDGE264MVC_AVCC_PENDING_MAX)
    {
        if (!sys->warned_avcc_length)
        {
            msg_Warn(dec,
                     "edge264mvc AVCC pending buffer exceeded %u bytes; dropping pending data",
                     EDGE264MVC_AVCC_PENDING_MAX);
            sys->warned_avcc_length = true;
        }
        AvccResetPending(sys);
        return VLCDEC_SUCCESS;
    }

    int ret = AvccEnsurePendingCapacity(sys, need);
    if (ret != VLCDEC_SUCCESS)
    {
        msg_Err(dec, "edge264mvc failed to grow AVCC pending buffer");
        return ret;
    }

    memcpy(sys->avcc_pending + sys->avcc_pending_len, buf, len);
    sys->avcc_pending_len += len;
    return VLCDEC_SUCCESS;
}

static void AnnexBResetPending(edge264mvc_sys_t *sys)
{
    sys->annexb_pending_len = 0;
}

static int AnnexBEnsurePendingCapacity(edge264mvc_sys_t *sys, size_t need)
{
    if (need <= sys->annexb_pending_cap)
        return VLCDEC_SUCCESS;

    size_t new_cap = sys->annexb_pending_cap;
    if (new_cap < 4096)
        new_cap = 4096;
    while (new_cap < need)
    {
        if (new_cap > (SIZE_MAX / 2))
        {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }

    uint8_t *new_buf = realloc(sys->annexb_pending, new_cap);
    if (new_buf == NULL)
        return VLCDEC_ECRITICAL;

    sys->annexb_pending = new_buf;
    sys->annexb_pending_cap = new_cap;
    return VLCDEC_SUCCESS;
}

static int AnnexBAppendPending(decoder_t *dec, edge264mvc_sys_t *sys,
                               const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return VLCDEC_SUCCESS;

    if (sys->annexb_pending_len > SIZE_MAX - len)
    {
        msg_Err(dec, "edge264mvc Annex-B pending buffer size overflow");
        return VLCDEC_ECRITICAL;
    }

    size_t need = sys->annexb_pending_len + len;
    if (need > EDGE264MVC_AVCC_PENDING_MAX)
    {
        if (!sys->warned_annexb_length)
        {
            msg_Warn(dec,
                     "edge264mvc Annex-B pending buffer exceeded %u bytes; dropping pending data",
                     EDGE264MVC_AVCC_PENDING_MAX);
            sys->warned_annexb_length = true;
        }
        AnnexBResetPending(sys);
        return VLCDEC_SUCCESS;
    }

    int ret = AnnexBEnsurePendingCapacity(sys, need);
    if (ret != VLCDEC_SUCCESS)
    {
        msg_Err(dec, "edge264mvc failed to grow Annex-B pending buffer");
        return ret;
    }

    memcpy(sys->annexb_pending + sys->annexb_pending_len, buf, len);
    sys->annexb_pending_len += len;
    return VLCDEC_SUCCESS;
}

static void DumpAnnexBNAL(decoder_t *dec, edge264mvc_sys_t *sys,
                          const uint8_t *nal, const uint8_t *end)
{
    if (sys->dump_annexb_fp == NULL || nal == NULL || end == NULL || nal >= end)
        return;

    static const uint8_t start_code[4] = { 0, 0, 0, 1 };
    size_t len = (size_t)(end - nal);

    if (fwrite(start_code, 1, sizeof(start_code), sys->dump_annexb_fp) != sizeof(start_code) ||
        fwrite(nal, 1, len, sys->dump_annexb_fp) != len)
    {
        if (!sys->warned_dump_io)
        {
            msg_Warn(dec, "edge264mvc dump write failed; disabling dump output");
            sys->warned_dump_io = true;
        }
        fclose(sys->dump_annexb_fp);
        sys->dump_annexb_fp = NULL;
        return;
    }

    sys->dump_nals++;
}

static int LoadEdge264Api(decoder_t *dec, edge264mvc_sys_t *sys)
{
    char *override_path = var_InheritString(dec, "edge264mvc-lib");
    if (override_path != NULL && *override_path == '\0')
    {
        free(override_path);
        override_path = NULL;
    }
    if (override_path == NULL)
    {
        const char *env_path = getenv("EDGE264MVC_LIB");
        if (env_path != NULL && *env_path != '\0')
            override_path = strdup(env_path);
    }

    const char *candidates[4] = { NULL, "libedge264.so.1", "libedge264.so", NULL };
    unsigned candidate_count = 3;

    if (override_path != NULL && *override_path != '\0')
    {
        candidates[0] = override_path;
        candidate_count = 4;
    }

    for (unsigned i = 0; i < candidate_count; ++i)
    {
        if (candidates[i] == NULL)
            continue;

        sys->api.dl_handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (sys->api.dl_handle != NULL)
            break;
    }

    if (sys->api.dl_handle == NULL)
    {
        msg_Err(dec, "unable to load edge264 shared library: %s", dlerror());
        free(override_path);
        return VLC_EGENERIC;
    }

#define LOAD_EDGE264_SYM(field, symbol) \
    do { \
        sys->api.field = dlsym(sys->api.dl_handle, symbol); \
        if (sys->api.field == NULL) { \
            msg_Err(dec, "missing symbol in edge264 library: %s", symbol); \
            dlclose(sys->api.dl_handle); \
            sys->api.dl_handle = NULL; \
            free(override_path); \
            return VLC_EGENERIC; \
        } \
    } while (0)

    LOAD_EDGE264_SYM(find_start_code, "edge264_find_start_code");
    LOAD_EDGE264_SYM(alloc, "edge264_alloc");
    LOAD_EDGE264_SYM(flush, "edge264_flush");
    LOAD_EDGE264_SYM(free_decoder, "edge264_free");
    LOAD_EDGE264_SYM(decode_NAL, "edge264_decode_NAL");
    LOAD_EDGE264_SYM(get_frame, "edge264_get_frame");
    LOAD_EDGE264_SYM(return_frame, "edge264_return_frame");

#undef LOAD_EDGE264_SYM

    free(override_path);
    return VLC_SUCCESS;
}

static void UnloadEdge264Api(edge264mvc_sys_t *sys)
{
    if (sys->api.dl_handle != NULL)
    {
        dlclose(sys->api.dl_handle);
        sys->api.dl_handle = NULL;
    }
}

static void CopyPlaneRows(uint8_t *dst, ptrdiff_t dst_pitch,
                          const uint8_t *src, ptrdiff_t src_pitch,
                          unsigned width, unsigned height)
{
    for (unsigned y = 0; y < height; ++y)
    {
        memcpy(dst, src, width);
        dst += dst_pitch;
        src += src_pitch;
    }
}

static bool EnsurePlaneCapacity(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return true;

    uint8_t *next = realloc(*buf, need);
    if (next == NULL)
        return false;
    *buf = next;
    *cap = need;
    return true;
}

static void RightHistoryReset(edge264mvc_sys_t *sys, bool free_buffers)
{
    sys->right_hist_head = 0;
    sys->selected_right_last_frameid = -1;
    for (unsigned i = 0; i < EDGE264MVC_RIGHT_HISTORY_MAX; ++i)
    {
        right_hist_entry_t *e = &sys->right_hist[i];
        e->valid = false;
        e->frame_id = -1;
        if (!free_buffers)
            continue;
        free(e->y);
        free(e->u);
        free(e->v);
        e->y = e->u = e->v = NULL;
        e->y_cap = e->u_cap = e->v_cap = 0;
    }
}

static void RightHistoryStore(decoder_t *dec, edge264mvc_sys_t *sys,
                              const Edge264Frame *frm,
                              unsigned wY, unsigned hY,
                              unsigned wC, unsigned hC)
{
    if (sys->right_hist_depth == 0 || frm->FrameId_mvc < 0)
        return;
    if (frm->samples_mvc[0] == NULL || frm->samples_mvc[1] == NULL || frm->samples_mvc[2] == NULL)
        return;

    if (wY == 0 || hY == 0 || wC == 0 || hC == 0)
        return;

    const size_t y_need = (size_t)wY * (size_t)hY;
    const size_t c_need = (size_t)wC * (size_t)hC;
    if (y_need == 0 || c_need == 0)
        return;

    right_hist_entry_t *e = &sys->right_hist[sys->right_hist_head % sys->right_hist_depth];
    if (!EnsurePlaneCapacity(&e->y, &e->y_cap, y_need) ||
        !EnsurePlaneCapacity(&e->u, &e->u_cap, c_need) ||
        !EnsurePlaneCapacity(&e->v, &e->v_cap, c_need))
    {
        msg_Warn(dec, "edge264mvc right-history allocation failed");
        return;
    }

    CopyPlaneRows(e->y, (ptrdiff_t)wY, frm->samples_mvc[0], frm->stride_Y, wY, hY);
    CopyPlaneRows(e->u, (ptrdiff_t)wC, frm->samples_mvc[1], frm->stride_C, wC, hC);
    CopyPlaneRows(e->v, (ptrdiff_t)wC, frm->samples_mvc[2], frm->stride_C, wC, hC);
    e->frame_id = frm->FrameId_mvc;
    e->valid = true;

    sys->right_hist_head = (sys->right_hist_head + 1u) % sys->right_hist_depth;
}

static right_pick_t RightHistoryPick(edge264mvc_sys_t *sys,
                                     const Edge264Frame *frm,
                                     unsigned wY, unsigned wC)
{
    right_pick_t pick = {
        .y = frm->samples_mvc[0],
        .u = frm->samples_mvc[1],
        .v = frm->samples_mvc[2],
        .stride_y = frm->stride_Y,
        .stride_c = frm->stride_C,
        .frame_id = frm->FrameId_mvc,
        .from_history = false,
    };

    if (sys->right_hist_depth == 0 || frm->FrameId < 0 || frm->FrameId_mvc < 0)
        return pick;

    sys->right_hist_selects++;

    const int32_t min_allowed = sys->selected_right_last_frameid;
    const bool have_min_allowed = (min_allowed >= 0);
    const bool min_exceeds_base = (have_min_allowed && min_allowed > frm->FrameId);

    const right_hist_entry_t *best = NULL;
    int32_t best_id = min_exceeds_base ? INT32_MAX : INT32_MIN;
    for (unsigned i = 0; i < sys->right_hist_depth; ++i)
    {
        const right_hist_entry_t *e = &sys->right_hist[i];
        if (!e->valid)
            continue;
        if (have_min_allowed && e->frame_id < min_allowed)
            continue;
        if (!min_exceeds_base && e->frame_id > frm->FrameId)
            continue; /* avoid picking a known-future right-eye frame */
        if (best == NULL)
        {
            best = e;
            best_id = e->frame_id;
            continue;
        }
        if (min_exceeds_base)
        {
            if (e->frame_id < best_id)
            {
                best = e;
                best_id = e->frame_id;
            }
        }
        else if (e->frame_id > best_id)
        {
            best = e;
            best_id = e->frame_id;
        }
    }

    if (best == NULL)
    {
        sys->right_hist_select_no_past++;
    }
    else
    {
        pick.y = best->y;
        pick.u = best->u;
        pick.v = best->v;
        pick.stride_y = (ptrdiff_t)wY;
        pick.stride_c = (ptrdiff_t)wC;
        pick.frame_id = best->frame_id;
        pick.from_history = true;

        sys->right_hist_select_hits++;
        if (best->frame_id == frm->FrameId)
            sys->right_hist_select_exact++;
    }

    if (have_min_allowed && pick.frame_id >= 0 && pick.frame_id < min_allowed)
    {
        if (frm->FrameId_mvc >= min_allowed)
        {
            /* Keep right-eye frame-id monotonic when possible to reduce visible jitter. */
            pick.y = frm->samples_mvc[0];
            pick.u = frm->samples_mvc[1];
            pick.v = frm->samples_mvc[2];
            pick.stride_y = frm->stride_Y;
            pick.stride_c = frm->stride_C;
            pick.frame_id = frm->FrameId_mvc;
            pick.from_history = false;
            sys->right_hist_monotonic_forced_current++;
        }
        else
        {
            sys->right_hist_monotonic_unresolved++;
        }
    }

    if (sys->hold_right_on_backstep &&
        sys->selected_right_last_frameid >= 0 &&
        pick.frame_id >= 0 &&
        pick.frame_id < sys->selected_right_last_frameid)
    {
        const int32_t hold_id = sys->selected_right_last_frameid;
        const right_hist_entry_t *hold = NULL;
        for (unsigned i = 0; i < sys->right_hist_depth; ++i)
        {
            const right_hist_entry_t *e = &sys->right_hist[i];
            if (e->valid && e->frame_id == hold_id)
            {
                hold = e;
                break;
            }
        }

        if (hold != NULL)
        {
            pick.y = hold->y;
            pick.u = hold->u;
            pick.v = hold->v;
            pick.stride_y = (ptrdiff_t)wY;
            pick.stride_c = (ptrdiff_t)wC;
            pick.frame_id = hold->frame_id;
            pick.from_history = true;
            sys->right_hist_monotonic_forced_hold++;
        }
        else if (frm->FrameId_mvc >= hold_id)
        {
            pick.y = frm->samples_mvc[0];
            pick.u = frm->samples_mvc[1];
            pick.v = frm->samples_mvc[2];
            pick.stride_y = frm->stride_Y;
            pick.stride_c = frm->stride_C;
            pick.frame_id = frm->FrameId_mvc;
            pick.from_history = false;
            sys->right_hist_monotonic_forced_current++;
        }
        else
        {
            sys->right_hist_monotonic_unresolved++;
        }
    }

    return pick;
}

static void RecordSelectedFrameIdTelemetry(edge264mvc_sys_t *sys,
                                           const Edge264Frame *frm,
                                           const right_pick_t *pick)
{
    if (frm->FrameId < 0 || pick->frame_id < 0)
        return;

    const int64_t delta64 = (int64_t)pick->frame_id - (int64_t)frm->FrameId;
    int32_t delta = 0;
    if (delta64 > INT32_MAX)
        delta = INT32_MAX;
    else if (delta64 < INT32_MIN)
        delta = INT32_MIN;
    else
        delta = (int32_t)delta64;

    int32_t abs_delta = delta >= 0 ? delta : (delta == INT32_MIN ? INT32_MAX : -delta);
    if (abs_delta > sys->selected_frameid_max_abs_delta)
        sys->selected_frameid_max_abs_delta = abs_delta;

    sys->selected_frameid_samples++;
    sys->selected_frameid_last_delta = delta;
    if (delta != 0)
        sys->selected_frameid_nonzero_delta++;
    if (delta > 0)
        sys->selected_frameid_right_ahead++;
    else if (delta < 0)
        sys->selected_frameid_left_ahead++;
}

static void LogFrameIdPairTelemetry(decoder_t *dec, edge264mvc_sys_t *sys,
                                    const Edge264Frame *frm, const right_pick_t *pick,
                                    vlc_tick_t out_ts, uint64_t out_seq)
{
    if (!sys->log_frameids)
        return;
    if (frm->FrameId < 0 || frm->FrameId_mvc < 0 || pick->frame_id < 0)
        return;

    const int32_t raw_delta = (int32_t)((int64_t)frm->FrameId_mvc - (int64_t)frm->FrameId);
    const int32_t sel_delta = (int32_t)((int64_t)pick->frame_id - (int64_t)frm->FrameId);
    const int32_t prev_sel = sys->selected_right_last_frameid;
    const bool backstep = (prev_sel >= 0 && pick->frame_id < prev_sel);
    const bool periodic = (sys->frameid_log_every > 0 &&
                           (sys->frames_queued % sys->frameid_log_every) == 0);
    if (backstep || raw_delta != 0 || sel_delta != 0 || periodic)
    {
        msg_Dbg(dec,
                "edge264mvc framepair: base=%d raw_ext=%d raw_delta=%d sel_ext=%d sel_delta=%d src=%c prev_sel=%d backstep=%d au=%d/%d poc=%d/%d frame_num=%d/%d view=%d/%d ts_seq=%" PRIu64 " ts=%" PRId64,
                frm->FrameId, frm->FrameId_mvc, raw_delta,
                pick->frame_id, sel_delta,
                pick->from_history ? 'H' : 'C',
                prev_sel, backstep ? 1 : 0,
                frm->AccessUnitId, frm->AccessUnitId_mvc,
                frm->PicOrderCnt, frm->PicOrderCnt_mvc,
                frm->FrameNum_abs, frm->FrameNum_abs_mvc,
                frm->view_id, frm->view_id_mvc,
                out_seq, (int64_t)out_ts);
    }
}

static void RecordPostSeekFrameTelemetry(decoder_t *dec, edge264mvc_sys_t *sys,
                                         const Edge264Frame *frm, bool has_stereo,
                                         vlc_tick_t out_ts, uint64_t ts_seq)
{
    if (sys->post_seek_debug_frames_left == 0)
        return;

    const bool repeat_base = (sys->post_seek_last_base_frameid >= 0 &&
                              frm->FrameId == sys->post_seek_last_base_frameid);
    const bool repeat_right = (sys->post_seek_last_right_frameid >= 0 &&
                               frm->FrameId_mvc == sys->post_seek_last_right_frameid);

    sys->post_seek_frames_queued++;
    if (has_stereo)
        sys->post_seek_stereo_frames++;
    else
        sys->post_seek_mono_frames++;

    if (out_ts <= VLC_TICK_INVALID)
        sys->post_seek_invalid_date_frames++;
    else if (sys->post_seek_last_date > VLC_TICK_INVALID && out_ts < sys->post_seek_last_date)
        sys->post_seek_ts_backsteps++;

    if (repeat_base)
        sys->post_seek_same_base_frame_repeats++;
    if (repeat_right)
        sys->post_seek_same_right_frame_repeats++;

    sys->post_seek_last_date = out_ts;
    sys->post_seek_last_base_frameid = frm->FrameId;
    sys->post_seek_last_right_frameid = frm->FrameId_mvc;

    msg_Dbg(dec,
            "edge264mvc post-seek frame[%u]: stereo=%d ts_seq=%" PRIu64 " ts=%" PRId64
            " base(frame=%d au=%d poc=%d fn=%d view=%d)"
            " ext(frame=%d au=%d poc=%d fn=%d view=%d)"
            " repeat(base=%d,right=%d)",
            EDGE264MVC_POST_SEEK_DEBUG_FRAMES - sys->post_seek_debug_frames_left,
            has_stereo ? 1 : 0, ts_seq, (int64_t)out_ts,
            frm->FrameId, frm->AccessUnitId, frm->PicOrderCnt, frm->FrameNum_abs, frm->view_id,
            frm->FrameId_mvc, frm->AccessUnitId_mvc, frm->PicOrderCnt_mvc,
            frm->FrameNum_abs_mvc, frm->view_id_mvc,
            repeat_base ? 1 : 0, repeat_right ? 1 : 0);

    sys->post_seek_debug_frames_left--;
}

static void LogPostSeekDecodeTelemetry(decoder_t *dec, edge264mvc_sys_t *sys,
                                       const char *stage, uint64_t nal_idx,
                                       unsigned nal_type, unsigned nal_ref_idc,
                                       int ret_code)
{
    if (sys->post_seek_decode_debug_left == 0)
        return;

    msg_Dbg(dec,
            "edge264mvc post-seek decode[%u]: stage=%s nal=%" PRIu64
            " type=%u nri=%u ret=%d wait_idr=%d paramset=%d subset_sps=%d"
            " post_discont=%u tsq=%u frames(popped=%" PRIu64 ",queued=%" PRIu64 ")",
            (EDGE264MVC_POST_SEEK_DEBUG_FRAMES * 4u) - sys->post_seek_decode_debug_left,
            stage != NULL ? stage : "?",
            nal_idx, nal_type, nal_ref_idc, ret_code,
            sys->wait_idr_after_flush ? 1 : 0,
            sys->wait_idr_have_paramset ? 1 : 0,
            sys->seen_subset_sps ? 1 : 0,
            sys->post_discontinuity_blocks, sys->ts_count,
            sys->frames_popped, sys->frames_queued);

    sys->post_seek_decode_debug_left--;
}

static bool EnsureOutputFormat(decoder_t *dec, edge264mvc_sys_t *sys,
                               const Edge264Frame *frm, bool has_stereo)
{
    if (frm->width_Y <= 0 || frm->height_Y <= 0 || frm->width_C <= 0 || frm->height_C <= 0)
        return false;

    unsigned src_width = (unsigned)frm->width_Y;
    unsigned src_height = (unsigned)frm->height_Y;
    unsigned out_width = has_stereo ? src_width * 2u : src_width;
    unsigned out_height = src_height;

    if (sys->out_configured &&
        sys->out_width == out_width &&
        sys->out_height == out_height &&
        sys->out_stereo == has_stereo)
    {
        return true;
    }

    dec->fmt_out.i_cat = VIDEO_ES;
    dec->fmt_out.i_codec = VLC_CODEC_I420;

    video_format_t *v = &dec->fmt_out.video;
    v->i_chroma = VLC_CODEC_I420;
    v->i_width = out_width;
    v->i_visible_width = out_width;
    v->i_height = out_height;
    v->i_visible_height = out_height;
    v->i_x_offset = 0;
    v->i_y_offset = 0;

    if (dec->fmt_in.video.i_sar_num > 0 && dec->fmt_in.video.i_sar_den > 0)
    {
        v->i_sar_num = dec->fmt_in.video.i_sar_num;
        v->i_sar_den = dec->fmt_in.video.i_sar_den;
    }
    else
    {
        v->i_sar_num = 1;
        v->i_sar_den = 1;
    }

    if (dec->fmt_in.video.i_frame_rate > 0 && dec->fmt_in.video.i_frame_rate_base > 0)
    {
        v->i_frame_rate = dec->fmt_in.video.i_frame_rate;
        v->i_frame_rate_base = dec->fmt_in.video.i_frame_rate_base;
    }

    if (has_stereo && sys->advertise_multiview)
        v->multiview_mode = MULTIVIEW_STEREO_SBS;
    else
        v->multiview_mode = MULTIVIEW_2D;

    if (decoder_UpdateVideoFormat(dec) != 0)
    {
        msg_Err(dec, "edge264mvc failed to update output format (%ux%u)", out_width, out_height);
        return false;
    }

    sys->out_width = out_width;
    sys->out_height = out_height;
    sys->out_stereo = has_stereo;
    sys->out_configured = true;
    RightHistoryReset(sys, false);

    msg_Dbg(dec,
            "edge264mvc output format updated: %ux%u stereo=%d fps=%u/%u sar=%u:%u",
            out_width, out_height, (int)has_stereo,
            v->i_frame_rate, v->i_frame_rate_base,
            v->i_sar_num, v->i_sar_den);
    return true;
}

static bool CopyFrameToPicture(picture_t *pic, const Edge264Frame *frm, bool has_stereo,
                               const right_pick_t *right_pick)
{
    if (pic->i_planes < 3)
        return false;

    unsigned wY = (unsigned)frm->width_Y;
    unsigned hY = (unsigned)frm->height_Y;
    unsigned wC = (unsigned)frm->width_C;
    unsigned hC = (unsigned)frm->height_C;

    if (!has_stereo)
    {
        if ((unsigned)pic->p[0].i_visible_pitch < wY ||
            (unsigned)pic->p[1].i_visible_pitch < wC ||
            (unsigned)pic->p[2].i_visible_pitch < wC)
            return false;

        CopyPlaneRows(pic->p[0].p_pixels, pic->p[0].i_pitch,
                      frm->samples[0], frm->stride_Y, wY, hY);
        CopyPlaneRows(pic->p[1].p_pixels, pic->p[1].i_pitch,
                      frm->samples[1], frm->stride_C, wC, hC);
        CopyPlaneRows(pic->p[2].p_pixels, pic->p[2].i_pitch,
                      frm->samples[2], frm->stride_C, wC, hC);
        return true;
    }

    if ((unsigned)pic->p[0].i_visible_pitch < (wY * 2u) ||
        (unsigned)pic->p[1].i_visible_pitch < (wC * 2u) ||
        (unsigned)pic->p[2].i_visible_pitch < (wC * 2u))
        return false;

    uint8_t *dstY = pic->p[0].p_pixels;
    const uint8_t *srcLY = frm->samples[0];
    const uint8_t *srcRY = right_pick != NULL ? right_pick->y : frm->samples_mvc[0];
    const ptrdiff_t strideRY = right_pick != NULL ? right_pick->stride_y : frm->stride_Y;
    for (unsigned y = 0; y < hY; ++y)
    {
        memcpy(dstY, srcLY, wY);
        memcpy(dstY + wY, srcRY, wY);
        dstY += pic->p[0].i_pitch;
        srcLY += frm->stride_Y;
        srcRY += strideRY;
    }

    uint8_t *dstU = pic->p[1].p_pixels;
    const uint8_t *srcLU = frm->samples[1];
    const uint8_t *srcRU = right_pick != NULL ? right_pick->u : frm->samples_mvc[1];
    const ptrdiff_t strideRC = right_pick != NULL ? right_pick->stride_c : frm->stride_C;
    for (unsigned y = 0; y < hC; ++y)
    {
        memcpy(dstU, srcLU, wC);
        memcpy(dstU + wC, srcRU, wC);
        dstU += pic->p[1].i_pitch;
        srcLU += frm->stride_C;
        srcRU += strideRC;
    }

    uint8_t *dstV = pic->p[2].p_pixels;
    const uint8_t *srcLV = frm->samples[2];
    const uint8_t *srcRV = right_pick != NULL ? right_pick->v : frm->samples_mvc[2];
    for (unsigned y = 0; y < hC; ++y)
    {
        memcpy(dstV, srcLV, wC);
        memcpy(dstV + wC, srcRV, wC);
        dstV += pic->p[2].i_pitch;
        srcLV += frm->stride_C;
        srcRV += strideRC;
    }

    return true;
}

static void RecordFrameIdTelemetry(decoder_t *dec, edge264mvc_sys_t *sys,
                                   const Edge264Frame *frm, bool has_stereo,
                                   vlc_tick_t out_ts, uint64_t out_seq)
{
    if (!has_stereo)
        return;

    if (frm->FrameId < 0 || frm->FrameId_mvc < 0)
        return;

    const int64_t delta64 = (int64_t)frm->FrameId_mvc - (int64_t)frm->FrameId;
    int32_t delta = 0;
    if (delta64 > INT32_MAX)
        delta = INT32_MAX;
    else if (delta64 < INT32_MIN)
        delta = INT32_MIN;
    else
        delta = (int32_t)delta64;

    int32_t abs_delta = delta >= 0 ? delta : (delta == INT32_MIN ? INT32_MAX : -delta);
    if (abs_delta > sys->frameid_max_abs_delta)
        sys->frameid_max_abs_delta = abs_delta;

    sys->frameid_samples++;
    sys->frameid_last_delta = delta;
    if (delta != 0)
        sys->frameid_nonzero_delta++;
    if (delta > 0)
        sys->frameid_right_ahead++;
    else if (delta < 0)
        sys->frameid_left_ahead++;

    if (!sys->log_frameids)
        return;

    const bool periodic = (sys->frameid_log_every > 0 &&
                           (sys->frames_queued % sys->frameid_log_every) == 0);
    if (delta != 0 || periodic)
    {
        msg_Dbg(dec,
                "edge264mvc frameids: base=%d ext=%d delta=%d max_abs=%d au=%d/%d poc=%d/%d frame_num=%d/%d view=%d/%d ts_seq=%" PRIu64 " ts=%" PRId64,
                frm->FrameId, frm->FrameId_mvc, delta, sys->frameid_max_abs_delta,
                frm->AccessUnitId, frm->AccessUnitId_mvc,
                frm->PicOrderCnt, frm->PicOrderCnt_mvc,
                frm->FrameNum_abs, frm->FrameNum_abs_mvc,
                frm->view_id, frm->view_id_mvc,
                out_seq, (int64_t)out_ts);
    }
}

static int QueueDecodedFrames(decoder_t *dec, edge264mvc_sys_t *sys)
{
    Edge264Frame frm;
    sys->queue_calls++;
    int ret;
    while ((ret = sys->api.get_frame(sys->decoder, &frm, 0)) == 0)
    {
        sys->frames_popped++;

        if (frm.bit_depth_Y != 8 || frm.bit_depth_C != 8)
        {
            msg_Warn(dec, "edge264mvc only supports 8-bit output currently (Y=%d C=%d)",
                     frm.bit_depth_Y, frm.bit_depth_C);
            continue;
        }

        bool has_stereo = frm.samples_mvc[0] != NULL && frm.samples_mvc[1] != NULL && frm.samples_mvc[2] != NULL;
        if (!has_stereo && sys->post_seek_require_stereo_drops_left > 0)
        {
            sys->post_seek_mono_frames_dropped++;
            if (sys->post_seek_debug_frames_left > 0)
            {
                msg_Dbg(dec,
                        "edge264mvc post-seek mono drop[%u]: drops_left=%u"
                        " base(frame=%d au=%d poc=%d fn=%d view=%d)"
                        " ext(frame=%d au=%d poc=%d fn=%d view=%d)",
                        EDGE264MVC_POST_SEEK_REQUIRE_STEREO_DROPS -
                            sys->post_seek_require_stereo_drops_left,
                        sys->post_seek_require_stereo_drops_left,
                        frm.FrameId, frm.AccessUnitId, frm.PicOrderCnt,
                        frm.FrameNum_abs, frm.view_id,
                        frm.FrameId_mvc, frm.AccessUnitId_mvc, frm.PicOrderCnt_mvc,
                        frm.FrameNum_abs_mvc, frm.view_id_mvc);
            }
            sys->post_seek_require_stereo_drops_left--;
            continue;
        }
        if (has_stereo)
            sys->post_seek_require_stereo_drops_left = 0;
        if (!EnsureOutputFormat(dec, sys, &frm, has_stereo))
        {
            continue;
        }

        right_pick_t right_pick = {
            .y = frm.samples_mvc[0],
            .u = frm.samples_mvc[1],
            .v = frm.samples_mvc[2],
            .stride_y = frm.stride_Y,
            .stride_c = frm.stride_C,
            .frame_id = frm.FrameId_mvc,
            .from_history = false,
        };
        if (has_stereo)
        {
            const unsigned wY = (unsigned)frm.width_Y;
            const unsigned hY = (unsigned)frm.height_Y;
            const unsigned wC = (unsigned)frm.width_C;
            const unsigned hC = (unsigned)frm.height_C;
            RightHistoryStore(dec, sys, &frm, wY, hY, wC, hC);
            right_pick = RightHistoryPick(sys, &frm, wY, wC);
            RecordSelectedFrameIdTelemetry(sys, &frm, &right_pick);
        }

        picture_t *pic = decoder_NewPicture(dec);
        if (pic == NULL)
        {
            continue;
        }

        if (!CopyFrameToPicture(pic, &frm, has_stereo, has_stereo ? &right_pick : NULL))
        {
            picture_Release(pic);
            continue;
        }

        uint64_t ts_seq = 0;
        pic->date = TsQueuePop(dec, sys, &ts_seq);
        pic->b_progressive = true;
        pic->i_nb_fields = 2;

        RecordPostSeekFrameTelemetry(dec, sys, &frm, has_stereo, pic->date, ts_seq);
        RecordFrameIdTelemetry(dec, sys, &frm, has_stereo, pic->date, ts_seq);
        if (has_stereo)
        {
            if (sys->selected_right_last_frameid >= 0)
            {
                if (right_pick.frame_id < sys->selected_right_last_frameid)
                    sys->selected_frameid_backsteps++;
                else if (right_pick.frame_id == sys->selected_right_last_frameid)
                    sys->selected_frameid_repeats++;
            }
            LogFrameIdPairTelemetry(dec, sys, &frm, &right_pick, pic->date, ts_seq);
            if (right_pick.frame_id >= 0)
                sys->selected_right_last_frameid = right_pick.frame_id;
        }
        decoder_QueueVideo(dec, pic);
        sys->frames_queued++;
    }

    if (ret == ENOMSG)
    {
        sys->queue_nomsg++;
        if (sys->post_seek_debug_frames_left > 0)
        {
            sys->post_seek_getframe_nomsg++;
            msg_Dbg(dec,
                    "edge264mvc post-seek get_frame: ENOMSG frames_popped=%" PRIu64
                    " frames_queued=%" PRIu64 " tsq=%u drops_left=%u",
                    sys->frames_popped, sys->frames_queued, sys->ts_count,
                    sys->post_seek_require_stereo_drops_left);
        }
        return VLCDEC_SUCCESS;
    }

    if (ret == EINVAL)
    {
        sys->queue_einval++;
        msg_Err(dec, "edge264_get_frame returned EINVAL");
        return VLCDEC_ECRITICAL;
    }

    return VLCDEC_SUCCESS;
}

static void RecordDecodeRet(edge264mvc_sys_t *sys, int ret)
{
    switch (ret)
    {
        case 0:
            sys->decode_ret_zero++;
            break;
        case ENODATA:
            sys->decode_ret_enodata++;
            break;
        case ENOBUFS:
            sys->decode_ret_enobufs++;
            break;
        case ENOTSUP:
            sys->decode_ret_enotsup++;
            break;
        case EBADMSG:
            sys->decode_ret_ebadmsg++;
            break;
        case EINVAL:
            sys->decode_ret_einval++;
            break;
        case ENOMEM:
            sys->decode_ret_enomem++;
            break;
        default:
            sys->decode_ret_other++;
            break;
    }
}

static int DrainOnEnobufs(decoder_t *dec, edge264mvc_sys_t *sys, uint64_t *frames_snapshot)
{
    sys->enobufs_drain_pulses++;

    int qret = QueueDecodedFrames(dec, sys);
    if (qret != VLCDEC_SUCCESS)
        return qret;

    if (sys->frames_popped > *frames_snapshot)
    {
        *frames_snapshot = sys->frames_popped;
        sys->enobufs_drain_progress++;
    }

    return VLCDEC_SUCCESS;
}

static int DrainDecoder(decoder_t *dec, edge264mvc_sys_t *sys, unsigned max_loops)
{
    unsigned idle_loops = 0;
    uint64_t last_frames = sys->frames_popped;

    for (unsigned i = 0; i < max_loops; ++i)
    {
        if (vlc_killed())
        {
            sys->killed_short_circuit++;
            return VLCDEC_SUCCESS;
        }

        int qret = QueueDecodedFrames(dec, sys);
        if (qret != VLCDEC_SUCCESS)
            return qret;

        bool progressed = (sys->frames_popped > last_frames);
        if (progressed)
        {
            last_frames = sys->frames_popped;
            idle_loops = 0;
        }
        else
        {
            idle_loops++;
            if (sys->post_seek_debug_frames_left > 0)
            {
                sys->post_seek_drain_idle_loops++;
                msg_Dbg(dec,
                        "edge264mvc post-seek drain idle[%u]: frames_popped=%" PRIu64
                        " frames_queued=%" PRIu64 " tsq=%u drops_left=%u",
                        idle_loops, sys->frames_popped, sys->frames_queued,
                        sys->ts_count, sys->post_seek_require_stereo_drops_left);
            }
        }

        if (idle_loops >= 16)
            return VLCDEC_SUCCESS;
    }

    return VLCDEC_SUCCESS;
}

typedef struct
{
    unsigned refcount;
    size_t size;
    uint8_t storage[];
} edge264_nal_buf_t;

#define EDGE264MVC_NAL_GUARD_BYTES 32u

static edge264_nal_buf_t *Edge264NALBufAlloc(const uint8_t *nal, size_t size)
{
    if (nal == NULL || size == 0)
        return NULL;
    if (size > SIZE_MAX - sizeof(edge264_nal_buf_t) - 2u * EDGE264MVC_NAL_GUARD_BYTES)
        return NULL;

    size_t total = size + 2u * EDGE264MVC_NAL_GUARD_BYTES;
    edge264_nal_buf_t *buf = malloc(sizeof(*buf) + total);
    if (buf == NULL)
        return NULL;

    buf->refcount = 1;
    buf->size = size;
    memset(buf->storage, 0, total);
    memcpy(buf->storage + EDGE264MVC_NAL_GUARD_BYTES, nal, size);
    buf->storage[EDGE264MVC_NAL_GUARD_BYTES - 3] = 0;
    buf->storage[EDGE264MVC_NAL_GUARD_BYTES - 2] = 0;
    buf->storage[EDGE264MVC_NAL_GUARD_BYTES - 1] = 1;
    buf->storage[EDGE264MVC_NAL_GUARD_BYTES + size + 0] = 0;
    buf->storage[EDGE264MVC_NAL_GUARD_BYTES + size + 1] = 0;
    buf->storage[EDGE264MVC_NAL_GUARD_BYTES + size + 2] = 1;
    return buf;
}

static void Edge264NALBufRetain(edge264_nal_buf_t *buf)
{
    if (buf == NULL)
        return;
    __atomic_add_fetch(&buf->refcount, 1u, __ATOMIC_ACQ_REL);
}

static void Edge264NALBufRelease(edge264_nal_buf_t *buf)
{
    if (buf == NULL)
        return;
    if (__atomic_sub_fetch(&buf->refcount, 1u, __ATOMIC_ACQ_REL) == 0u)
        free(buf);
}

static void Edge264NALUnref(int ret, void *arg)
{
    (void)ret;
    Edge264NALBufRelease((edge264_nal_buf_t *)arg);
}

static bool RecoverAfterSoftError(decoder_t *dec, edge264mvc_sys_t *sys,
                                  const char *cause)
{
    const bool early_post_seek_error =
        (sys->post_discontinuity_blocks > 0 &&
         sys->post_seek_last_date <= VLC_TICK_INVALID);

    if (early_post_seek_error && sys->post_seek_soft_restart_budget > 0)
    {
        sys->post_seek_soft_restart_budget--;
        FlushDecoderLocked(sys);
        if (ReopenDecoderLocked(dec, sys,
                                cause != NULL ? cause : "post_seek_soft_error",
                                0u) != VLCDEC_SUCCESS)
            return false;
        sys->skip_next_discontinuity_reopen = false;
        ArmPostSeekRecovery(sys, false);
        sys->recover_flushes++;
        sys->last_recover_decode_call = sys->decode_calls;
        msg_Dbg(dec,
                "edge264mvc post-seek soft-error restart: cause=%s decode_calls=%" PRIu64,
                cause != NULL ? cause : "post_seek_soft_error",
                sys->decode_calls);
        return true;
    }
    else if (early_post_seek_error && sys->post_seek_decode_debug_left > 0)
    {
        msg_Dbg(dec,
                "edge264mvc post-seek soft-error restart skipped: cause=%s budget=%u decode_calls=%" PRIu64,
                cause != NULL ? cause : "post_seek_soft_error",
                sys->post_seek_soft_restart_budget, sys->decode_calls);
    }

    if (!sys->wait_idr_after_flush &&
        sys->last_recover_decode_call != 0 &&
        (sys->decode_calls - sys->last_recover_decode_call) < sys->recover_flush_min_decode_gap)
    {
        sys->recover_flush_skips++;
        return false;
    }

    sys->api.flush(sys->decoder);
    TsQueueReset(sys);
    sys->wait_idr_after_flush = sys->single_thread_mode;
    sys->wait_idr_have_paramset = false;
    sys->wait_idr_allow_non_idr_once = false;
    sys->recover_flushes++;
    sys->last_recover_decode_call = sys->decode_calls;
    return true;
}

static void RecordSoftTrace(edge264mvc_sys_t *sys, uint64_t nal_idx,
                            unsigned nal_type, unsigned prev_nal_type,
                            unsigned nal_ref_idc, int ret_code,
                            unsigned recover_action)
{
    if (sys->soft_trace_count >= EDGE264MVC_SOFT_TRACE_MAX)
    {
        sys->soft_trace_dropped++;
        return;
    }

    edge264_soft_trace_t *t = &sys->soft_trace[sys->soft_trace_count++];
    t->decode_call = sys->decode_calls;
    t->nal_idx = nal_idx;
    t->frames_popped = sys->frames_popped;
    t->frames_queued = sys->frames_queued;
    t->nal_type = nal_type;
    t->prev_nal_type = prev_nal_type;
    t->nal_ref_idc = nal_ref_idc;
    t->ret_code = ret_code;
    t->recover_action = recover_action;
    t->wait_idr_after_flush = sys->wait_idr_after_flush;
    t->wait_idr_have_paramset = sys->wait_idr_have_paramset;
    t->seen_subset_sps = sys->seen_subset_sps;
}

static int DecodeOneNAL(decoder_t *dec, edge264mvc_sys_t *sys,
                        const uint8_t *nal, const uint8_t *end,
                        bool is_terminal_in_block, bool eos_hint)
{
    if (nal == NULL || end == NULL || nal >= end)
        return VLCDEC_SUCCESS;

    const unsigned nal_type = nal[0] & 0x1f;
    const unsigned nal_ref_idc = (unsigned)((nal[0] >> 5) & 0x03);
    if (sys->nals_seen < 128)
    {
        msg_Dbg(dec,
                "edge264mvc startup_nal idx=%" PRIu64 " type=%u ref=%u wait_idr=%d seen_subset=%d have_param=%d",
                sys->nals_seen + 1, nal_type, nal_ref_idc,
                sys->wait_idr_after_flush, sys->seen_subset_sps, sys->wait_idr_have_paramset);
    }
    const unsigned prev_nal_type = sys->last_nal_type;
    sys->last_nal_type = nal_type;
    uint64_t nal_idx = ++sys->nals_seen;
    if (sys->post_seek_decode_debug_left > 0)
        LogPostSeekDecodeTelemetry(dec, sys, "enter", nal_idx, nal_type, nal_ref_idc, 0);
    if (nal_type == 15)
    {
        sys->seen_subset_sps = true;
        sys->startup_seen_subset_sps++;
    }
    if (sys->gate_type20_until_subset_sps &&
        !sys->seen_subset_sps &&
        nal_type == 20)
    {
        if (sys->startup_gated_type20 < sys->gate_type20_max_skips)
        {
            sys->startup_gated_type20++;
            LogPostSeekDecodeTelemetry(dec, sys, "skip_gate_type20", nal_idx,
                                       nal_type, nal_ref_idc, 0);
            return VLCDEC_SUCCESS;
        }
        if (!sys->warned_gate_limit)
        {
            msg_Warn(dec,
                     "edge264mvc startup type-20 gate limit reached without subset SPS; disabling gate");
            sys->warned_gate_limit = true;
        }
        sys->seen_subset_sps = true;
    }
    if (sys->wait_idr_after_flush)
    {
        if (nal_type == 7 || nal_type == 8 || nal_type == 15)
            sys->wait_idr_have_paramset = true;
        const bool vcl_or_viewext = ((nal_type >= 1 && nal_type <= 5) || nal_type == 20);
        if (nal_type == 5)
        {
            if (sys->wait_idr_have_paramset)
            {
                sys->wait_idr_after_flush = false;
                sys->wait_idr_have_paramset = false;
                sys->wait_idr_allow_non_idr_once = false;
            }
            else
            {
                sys->skipped_wait_idr_vcl++;
                LogPostSeekDecodeTelemetry(dec, sys, "skip_wait_idr_idr_no_paramset",
                                           nal_idx, nal_type, nal_ref_idc, 0);
                return VLCDEC_SUCCESS;
            }
        }
        else if (vcl_or_viewext)
        {
            if (sys->wait_idr_allow_non_idr_once &&
                sys->wait_idr_have_paramset &&
                sys->seen_subset_sps &&
                sys->post_discontinuity_blocks > 0)
            {
                sys->wait_idr_after_flush = false;
                sys->wait_idr_have_paramset = false;
                sys->wait_idr_allow_non_idr_once = false;
                LogPostSeekDecodeTelemetry(dec, sys, "allow_wait_idr_non_idr_once",
                                           nal_idx, nal_type, nal_ref_idc, 0);
            }
            else
            {
                sys->skipped_wait_idr_vcl++;
                LogPostSeekDecodeTelemetry(dec, sys, "skip_wait_idr_vcl",
                                           nal_idx, nal_type, nal_ref_idc, 0);
                return VLCDEC_SUCCESS;
            }
        }
    }
    if (sys->skip_type20_nri0 && nal_type == 20 && (nal[0] & 0x60) == 0)
    {
        sys->skipped_type20_nri0++;
        LogPostSeekDecodeTelemetry(dec, sys, "skip_type20_nri0",
                                   nal_idx, nal_type, nal_ref_idc, 0);
        return VLCDEC_SUCCESS;
    }
    if (nal_type == 0 || nal_type >= 24)
    {
        sys->skipped_reserved_nals++;
        if (!sys->warned_reserved_nal)
        {
            msg_Warn(dec, "edge264mvc skipping reserved/unsupported NAL type(s) 0 or 24..31");
            sys->warned_reserved_nal = true;
        }
        LogPostSeekDecodeTelemetry(dec, sys, "skip_reserved",
                                   nal_idx, nal_type, nal_ref_idc, 0);
        return VLCDEC_SUCCESS;
    }

    if (eos_hint && is_terminal_in_block && nal_type == 20)
    {
        /*
         * Guardrail: skip final MVC extension NAL at EOS to avoid known crash case.
         */
        sys->skipped_terminal_ext_nals++;
        LogPostSeekDecodeTelemetry(dec, sys, "skip_terminal_ext",
                                   nal_idx, nal_type, nal_ref_idc, 0);
        return VLCDEC_SUCCESS;
    }

    DumpAnnexBNAL(dec, sys, nal, end);
    MaybeLogSeiPayloadProbe(dec, sys, nal, end);

    const size_t nal_size = (size_t)(end - nal);
    edge264_nal_buf_t *nal_buf = Edge264NALBufAlloc(nal, nal_size);
    if (nal_buf == NULL)
    {
        msg_Err(dec, "edge264mvc failed to allocate owned NAL buffer (%zu bytes)", nal_size);
        return VLCDEC_ECRITICAL;
    }

    const uint8_t *nal_owned = nal_buf->storage + EDGE264MVC_NAL_GUARD_BYTES;
    const uint8_t *end_owned = nal_owned + nal_buf->size;

    unsigned enobufs_loop_max = sys->enobufs_loop_max;
    if (sys->post_discontinuity_blocks > 0 &&
        enobufs_loop_max > EDGE264MVC_POST_DISCONT_ENOBUFS_LOOP_MAX)
    {
        enobufs_loop_max = EDGE264MVC_POST_DISCONT_ENOBUFS_LOOP_MAX;
    }

    unsigned enobufs_spins = 0;
    unsigned enobufs_total_spins = 0;
    unsigned nal_loop_guard = 0;
    uint64_t frames_snapshot = sys->frames_popped;
    int decode_status = VLCDEC_SUCCESS;

    for (;;)
    {
        if (vlc_killed())
        {
            sys->killed_short_circuit++;
            decode_status = VLCDEC_SUCCESS;
            break;
        }

        nal_loop_guard++;
        if (nal_loop_guard > EDGE264MVC_NAL_LOOP_MAX)
        {
            msg_Warn(dec, "edge264mvc NAL loop guard triggered, dropping one NAL to avoid stall");
            decode_status = VLCDEC_SUCCESS;
            break;
        }

        /*
         * Threaded edge264 may continue parsing a slice after decode_NAL returns.
         * Use owned NAL storage and async unref callback so worker threads never
         * read from freed VLC block memory.
         */
        Edge264NALBufRetain(nal_buf);
        int ret = sys->api.decode_NAL(sys->decoder, nal_owned, end_owned,
                                      Edge264NALUnref, nal_buf);
        sys->decode_calls++;
        RecordDecodeRet(sys, ret);
        if (sys->post_seek_decode_debug_left > 0)
            LogPostSeekDecodeTelemetry(dec, sys, "decode_ret", nal_idx, nal_type,
                                       nal_ref_idc, ret);
        if (ret != 0)
            Edge264NALBufRelease(nal_buf);

        int qret = QueueDecodedFrames(dec, sys);
        if (qret != VLCDEC_SUCCESS)
        {
            decode_status = qret;
            break;
        }

        if (ret == 0)
        {
            decode_status = VLCDEC_SUCCESS;
            break;
        }

        if (ret == ENODATA)
        {
            decode_status = VLCDEC_SUCCESS;
            break;
        }

        if (ret == ENOBUFS)
        {
            if (sys->first_enobufs_nal == 0)
            {
                sys->first_enobufs_nal = nal_idx;
                sys->first_enobufs_type = nal_type;
            }
            if (nal_type == 20)
                sys->enobufs_nal20++;
            else if (nal_type == 14)
                sys->enobufs_nal14++;
            else
                sys->enobufs_other++;

            if (sys->single_thread_mode && nal_type == 5)
            {
                sys->api.flush(sys->decoder);
                TsQueueReset(sys);
                sys->wait_idr_after_flush = true;
                sys->wait_idr_have_paramset = false;
                sys->recover_flushes++;
                sys->last_recover_decode_call = sys->decode_calls;
                decode_status = VLCDEC_SUCCESS;
                break;
            }

            enobufs_spins++;
            enobufs_total_spins++;

            if (sys->frames_popped > frames_snapshot)
            {
                frames_snapshot = sys->frames_popped;
                enobufs_spins = 0;
                continue;
            }

            if ((enobufs_spins % sys->enobufs_drain_period) == 0)
            {
                uint64_t before = sys->frames_popped;
                int dr = DrainOnEnobufs(dec, sys, &frames_snapshot);
                if (dr != VLCDEC_SUCCESS)
                {
                    decode_status = dr;
                    break;
                }
                if (sys->frames_popped > before)
                {
                    enobufs_spins = 0;
                    continue;
                }
            }

            if (sys->enobufs_retry_sleep_us > 0)
                msleep((vlc_tick_t)sys->enobufs_retry_sleep_us);

            const unsigned enobufs_total_max =
                enobufs_loop_max > (UINT_MAX / 8u) ?
                UINT_MAX : (enobufs_loop_max * 8u);
            if (enobufs_spins > enobufs_loop_max ||
                enobufs_total_spins > enobufs_total_max)
            {
                const bool aux_nal = (nal_type == 9 || nal_type == 10 ||
                                      nal_type == 11 || nal_type == 12);
                if (aux_nal)
                {
                    msg_Dbg(dec,
                            "edge264mvc ENOBUFS guard: dropping aux NAL type %u after spin=%u total=%u",
                            nal_type, enobufs_spins, enobufs_total_spins);
                    sys->enobufs_guard_skipped_aux++;
                    decode_status = VLCDEC_SUCCESS;
                    break;
                }

                if (!sys->warned_enobufs_guard)
                {
                    msg_Warn(dec,
                             "edge264mvc ENOBUFS starvation guard: flush+resync after spin=%u total=%u",
                             enobufs_spins, enobufs_total_spins);
                    sys->warned_enobufs_guard = true;
                }
                sys->api.flush(sys->decoder);
                TsQueueReset(sys);
                sys->wait_idr_after_flush = sys->single_thread_mode;
                sys->wait_idr_have_paramset = false;
                sys->wait_idr_allow_non_idr_once = false;
                sys->recover_flushes++;
                sys->last_recover_decode_call = sys->decode_calls;
                sys->enobufs_guard_flushes++;
                decode_status = VLCDEC_SUCCESS;
                break;
            }
            continue;
        }

        if (ret == ENOTSUP)
        {
            if (sys->first_enotsup_nal == 0)
            {
                sys->first_enotsup_nal = nal_idx;
                sys->first_enotsup_type = nal_type;
            }
            if (nal_type == 20)
                sys->enotsup_nal20++;
            else if (nal_type == 14)
                sys->enotsup_nal14++;
            else
                sys->enotsup_other++;
            const bool viewext_soft = (nal_type == 20 || nal_type == 14);
            if (!sys->warned_unsupported)
            {
                msg_Warn(dec, "edge264mvc stream contains unsupported features; continuing");
                sys->warned_unsupported = true;
            }
            if (viewext_soft && !sys->recover_viewext_errors)
            {
                RecordSoftTrace(sys, nal_idx, nal_type, prev_nal_type, nal_ref_idc, ret,
                                EDGE264MVC_SOFT_ACTION_SKIP_VIEWEXT);
                sys->recover_flush_skips++;
                decode_status = VLCDEC_SUCCESS;
                break;
            }
            const bool vcl_or_viewext = ((nal_type >= 1 && nal_type <= 5) || viewext_soft);
            if (!sys->single_thread_mode && !vcl_or_viewext)
            {
                RecordSoftTrace(sys, nal_idx, nal_type, prev_nal_type, nal_ref_idc, ret,
                                EDGE264MVC_SOFT_ACTION_SKIP_NONVCL);
                sys->recover_flush_skips++;
                decode_status = VLCDEC_SUCCESS;
                break;
            }
            /* Recover decoder state with cooldown to avoid reset thrash. */
            const bool did_flush = RecoverAfterSoftError(dec, sys, "soft_enotsup");
            RecordSoftTrace(sys, nal_idx, nal_type, prev_nal_type, nal_ref_idc, ret,
                            did_flush ? EDGE264MVC_SOFT_ACTION_FLUSH
                                      : EDGE264MVC_SOFT_ACTION_SKIP_COOLDOWN);
            decode_status = VLCDEC_SUCCESS;
            break;
        }

        if (ret == EBADMSG)
        {
            if (sys->first_ebadmsg_nal == 0)
            {
                sys->first_ebadmsg_nal = nal_idx;
                sys->first_ebadmsg_type = nal_type;
            }
            if (nal_type == 20)
                sys->ebadmsg_nal20++;
            else if (nal_type == 14)
                sys->ebadmsg_nal14++;
            else
                sys->ebadmsg_other++;
            const bool viewext_soft = (nal_type == 20 || nal_type == 14);
            if (!sys->warned_invalid)
            {
                msg_Warn(dec, "edge264mvc detected invalid/corrupted NAL units; continuing");
                sys->warned_invalid = true;
            }
            if (viewext_soft && !sys->recover_viewext_errors)
            {
                RecordSoftTrace(sys, nal_idx, nal_type, prev_nal_type, nal_ref_idc, ret,
                                EDGE264MVC_SOFT_ACTION_SKIP_VIEWEXT);
                sys->recover_flush_skips++;
                decode_status = VLCDEC_SUCCESS;
                break;
            }
            const bool vcl_or_viewext = ((nal_type >= 1 && nal_type <= 5) || viewext_soft);
            if (!sys->single_thread_mode && !vcl_or_viewext)
            {
                RecordSoftTrace(sys, nal_idx, nal_type, prev_nal_type, nal_ref_idc, ret,
                                EDGE264MVC_SOFT_ACTION_SKIP_NONVCL);
                sys->recover_flush_skips++;
                decode_status = VLCDEC_SUCCESS;
                break;
            }
            /* Recover decoder state with cooldown to avoid reset thrash. */
            const bool did_flush = RecoverAfterSoftError(dec, sys, "soft_ebadmsg");
            RecordSoftTrace(sys, nal_idx, nal_type, prev_nal_type, nal_ref_idc, ret,
                            did_flush ? EDGE264MVC_SOFT_ACTION_FLUSH
                                      : EDGE264MVC_SOFT_ACTION_SKIP_COOLDOWN);
            decode_status = VLCDEC_SUCCESS;
            break;
        }

        if (ret == ENOMEM || ret == EINVAL)
        {
            msg_Err(dec, "edge264_decode_NAL failed critically: %d", ret);
            decode_status = VLCDEC_ECRITICAL;
            break;
        }

        msg_Dbg(dec, "edge264_decode_NAL returned %d", ret);
        decode_status = VLCDEC_SUCCESS;
        break;
    }

    Edge264NALBufRelease(nal_buf);
    return decode_status;
}

static int DecodeAnnexBBuffer(decoder_t *dec, edge264mvc_sys_t *sys,
                              const uint8_t *buf, size_t len, bool eos_hint)
{
    int append_ret = AnnexBAppendPending(dec, sys, buf, len);
    if (append_ret != VLCDEC_SUCCESS)
        return append_ret;

    size_t consume = 0;
    if (sys->annexb_pending_len > 0)
    {
        const uint8_t *base = sys->annexb_pending;
        const uint8_t *end = base + sys->annexb_pending_len;
        const uint8_t *sc = sys->api.find_start_code(base, end, 0);
        if (sc < end)
        {
            if (sc > base)
                consume = (size_t)(sc - base);

            const uint8_t *nal_sc = sc;
            while (nal_sc < end)
            {
                if (vlc_killed())
                {
                    sys->killed_short_circuit++;
                    return VLCDEC_SUCCESS;
                }

                size_t prefix = StartCodeLen(nal_sc, end);
                if (prefix == 0)
                    break;

                const uint8_t *nal = nal_sc + prefix;
                const uint8_t *next_sc = sys->api.find_start_code(nal, end, 0);
                if (next_sc >= end)
                    break;

                if (next_sc > nal)
                {
                    int ret = DecodeOneNAL(dec, sys, nal, next_sc, false, false);
                    if (ret != VLCDEC_SUCCESS)
                        return ret;
                }

                consume = (size_t)(next_sc - base);
                nal_sc = next_sc;
            }
        }
    }

    if (consume > 0)
    {
        if (consume < sys->annexb_pending_len)
        {
            size_t tail = sys->annexb_pending_len - consume;
            memmove(sys->annexb_pending, sys->annexb_pending + consume, tail);
            sys->annexb_pending_len = tail;
        }
        else
        {
            AnnexBResetPending(sys);
        }
    }

    if (!eos_hint || sys->annexb_pending_len == 0)
        return VLCDEC_SUCCESS;

    const uint8_t *base = sys->annexb_pending;
    const uint8_t *end = base + sys->annexb_pending_len;
    const uint8_t *sc = sys->api.find_start_code(base, end, 0);
    if (sc < end)
    {
        size_t prefix = StartCodeLen(sc, end);
        const uint8_t *nal = sc + prefix;
        if (nal < end)
        {
            int ret = DecodeOneNAL(dec, sys, nal, end, true, true);
            if (ret != VLCDEC_SUCCESS)
                return ret;
        }
    }
    else
    {
        int ret = DecodeOneNAL(dec, sys, base, end, true, true);
        if (ret != VLCDEC_SUCCESS)
            return ret;
    }

    AnnexBResetPending(sys);
    return VLCDEC_SUCCESS;
}

static int DecodeAvccBuffer(decoder_t *dec, edge264mvc_sys_t *sys,
                            const uint8_t *buf, size_t len, bool eos_hint)
{
    if (buf == NULL || len == 0)
        return VLCDEC_SUCCESS;

    int append_ret = AvccAppendPending(dec, sys, buf, len);
    if (append_ret != VLCDEC_SUCCESS)
        return append_ret;

    size_t pos = 0;
    while ((sys->avcc_pending_len - pos) >= sys->avcc_length_size)
    {
        if (vlc_killed())
        {
            sys->killed_short_circuit++;
            return VLCDEC_SUCCESS;
        }

        const uint8_t *p = sys->avcc_pending + pos;
        uint32_t nal_size = ReadBE(p, sys->avcc_length_size);

        if (nal_size == 0)
        {
            pos += sys->avcc_length_size;
            continue;
        }

        if (nal_size > EDGE264MVC_AVCC_NAL_MAX)
        {
            size_t tail_len = sys->avcc_pending_len - pos;
            const uint8_t *tail = sys->avcc_pending + pos;
            if (BufferHasStartCode(tail, tail_len))
            {
                int ret = DecodeAnnexBBuffer(dec, sys, tail, tail_len, eos_hint);
                AvccResetPending(sys);
                return ret;
            }

            if (!sys->warned_avcc_length)
            {
                msg_Warn(dec, "edge264mvc invalid AVCC NAL length: %u", nal_size);
                sys->warned_avcc_length = true;
            }
            AvccResetPending(sys);
            return VLCDEC_SUCCESS;
        }

        size_t avail = sys->avcc_pending_len - pos - sys->avcc_length_size;
        if (nal_size > avail)
            break;

        const uint8_t *nal = p + sys->avcc_length_size;
        const uint8_t *nal_end = nal + nal_size;
        const bool terminal = ((size_t)(nal_end - sys->avcc_pending) == sys->avcc_pending_len);
        int ret = DecodeOneNAL(dec, sys, nal, nal_end, terminal, eos_hint);
        if (ret != VLCDEC_SUCCESS)
        {
            if (pos > 0)
            {
                size_t tail = sys->avcc_pending_len - pos;
                memmove(sys->avcc_pending, sys->avcc_pending + pos, tail);
                sys->avcc_pending_len = tail;
            }
            return ret;
        }

        pos += (size_t)sys->avcc_length_size + nal_size;
    }

    if (pos > 0)
    {
        if (pos < sys->avcc_pending_len)
        {
            size_t tail = sys->avcc_pending_len - pos;
            memmove(sys->avcc_pending, sys->avcc_pending + pos, tail);
            sys->avcc_pending_len = tail;
        }
        else
        {
            AvccResetPending(sys);
        }
    }

    if (eos_hint && sys->avcc_pending_len != 0)
    {
        if (!sys->warned_avcc_length)
        {
            msg_Warn(dec, "edge264mvc dropping %zu trailing AVCC byte(s) at EOS",
                     sys->avcc_pending_len);
            sys->warned_avcc_length = true;
        }
        AvccResetPending(sys);
    }

    return VLCDEC_SUCCESS;
}

static int FeedExtradata(decoder_t *dec, edge264mvc_sys_t *sys)
{
    if (dec->fmt_in.p_extra == NULL || dec->fmt_in.i_extra == 0)
        return VLCDEC_SUCCESS;

    const uint8_t *extra = dec->fmt_in.p_extra;
    size_t extra_len = dec->fmt_in.i_extra;

    if (extra_len >= 7 && extra[0] == 1)
    {
        sys->avcc_length_size = (extra[4] & 0x03) + 1;
        sys->avcc_announced = true;

        size_t pos = 5;
        if (pos >= extra_len)
            return VLCDEC_SUCCESS;

        uint8_t sps_count = extra[pos++] & 0x1f;
        for (uint8_t i = 0; i < sps_count; ++i)
        {
            if (pos + 2 > extra_len)
                return VLCDEC_SUCCESS;
            uint16_t n = (uint16_t)((extra[pos] << 8) | extra[pos + 1]);
            pos += 2;
            if (pos + n > extra_len)
                return VLCDEC_SUCCESS;
            int ret = DecodeOneNAL(dec, sys, &extra[pos], &extra[pos + n], false, false);
            if (ret != VLCDEC_SUCCESS)
                return ret;
            pos += n;
        }

        if (pos >= extra_len)
            return VLCDEC_SUCCESS;

        uint8_t pps_count = extra[pos++];
        for (uint8_t i = 0; i < pps_count; ++i)
        {
            if (pos + 2 > extra_len)
                return VLCDEC_SUCCESS;
            uint16_t n = (uint16_t)((extra[pos] << 8) | extra[pos + 1]);
            pos += 2;
            if (pos + n > extra_len)
                return VLCDEC_SUCCESS;
            int ret = DecodeOneNAL(dec, sys, &extra[pos], &extra[pos + n], false, false);
            if (ret != VLCDEC_SUCCESS)
                return ret;
            pos += n;
        }

        return VLCDEC_SUCCESS;
    }

    if (BufferHasStartCode(extra, extra_len))
        return DecodeAnnexBBuffer(dec, sys, extra, extra_len, false);

    return VLCDEC_SUCCESS;
}

static void FlushDecoder(decoder_t *dec)
{
    edge264mvc_sys_t *sys = (edge264mvc_sys_t *)dec->p_sys;
    if (sys == NULL)
        return;

    vlc_mutex_lock(&sys->lock);
    FlushDecoderLocked(sys);
    if (ReopenDecoderLocked(dec, sys, "pf_flush", 0u) != VLCDEC_SUCCESS)
        msg_Err(dec, "edge264mvc hard reopen after flush failed");
    else
        sys->skip_next_discontinuity_reopen = true;
    ArmPostSeekRecovery(sys, true);
    vlc_mutex_unlock(&sys->lock);
}

static void FlushDecoderLocked(edge264mvc_sys_t *sys)
{
    if (sys->decoder != NULL)
        sys->api.flush(sys->decoder);

    TsQueueReset(sys);
    AvccResetPending(sys);
    AnnexBResetPending(sys);
    RightHistoryReset(sys, false);
    sys->seen_subset_sps = false;
    sys->wait_idr_after_flush = sys->single_thread_mode;
    sys->wait_idr_have_paramset = false;
    sys->wait_idr_allow_non_idr_once = false;
    sys->post_seek_debug_frames_left = 0;
    sys->post_seek_decode_debug_left = 0;
    sys->post_seek_require_stereo_drops_left = 0;
    sys->post_seek_soft_restart_budget = 0;
    sys->post_seek_last_date = VLC_TICK_INVALID;
    sys->post_seek_last_base_frameid = -1;
    sys->post_seek_last_right_frameid = -1;
    sys->warned_gate_limit = false;
    sys->annexb_announced = false;
    sys->skip_next_discontinuity_reopen = false;
}

static void ArmPostSeekRecovery(edge264mvc_sys_t *sys, bool reset_soft_budget)
{
    sys->post_discontinuity_blocks = EDGE264MVC_POST_DISCONT_BLOCKS;
    sys->wait_idr_after_flush = true;
    sys->wait_idr_have_paramset = sys->extradata_seeded;
    sys->wait_idr_allow_non_idr_once = true;
    if (sys->extradata_seeded)
        sys->seen_subset_sps = true;
    sys->post_seek_debug_frames_left = EDGE264MVC_POST_SEEK_DEBUG_FRAMES;
    sys->post_seek_decode_debug_left = EDGE264MVC_POST_SEEK_DEBUG_FRAMES * 4u;
    sys->post_seek_require_stereo_drops_left =
        sys->out_stereo ? EDGE264MVC_POST_SEEK_REQUIRE_STEREO_DROPS : 0;
    if (reset_soft_budget)
        sys->post_seek_soft_restart_budget = 1;
    sys->post_seek_last_date = VLC_TICK_INVALID;
    sys->post_seek_last_base_frameid = -1;
    sys->post_seek_last_right_frameid = -1;
}

static Edge264Decoder *AllocEdge264Decoder(decoder_t *dec, edge264mvc_sys_t *sys,
                                           int thread_count)
{
    Edge264Decoder *decoder =
        sys->api.alloc(thread_count,
                       (sys->log_frameids || sys->log_sei) ? Edge264CoreLogCb : NULL,
                       (sys->log_frameids || sys->log_sei) ? dec : NULL,
                       0, NULL, NULL, NULL);
    if (decoder == NULL && (sys->log_frameids || sys->log_sei))
    {
        msg_Warn(dec,
                 "edge264_alloc failed with core logger enabled; retrying without core logger");
        decoder = sys->api.alloc(thread_count, NULL, NULL, 0, NULL, NULL, NULL);
    }
    return decoder;
}

static int ReopenDecoderLocked(decoder_t *dec, edge264mvc_sys_t *sys,
                               const char *cause, uint32_t block_flags)
{
    int reopen_thread_count = sys->current_thread_count;

    Edge264Decoder *new_decoder = AllocEdge264Decoder(dec, sys, reopen_thread_count);
    if (new_decoder == NULL)
        return VLCDEC_ECRITICAL;

    Edge264Decoder *old_decoder = sys->decoder;
    sys->decoder = new_decoder;

    int ret = FeedExtradata(dec, sys);
    if (ret != VLCDEC_SUCCESS)
    {
        sys->api.free_decoder(&sys->decoder);
        sys->decoder = old_decoder;
        return ret;
    }
    sys->extradata_seeded = true;

    if (old_decoder != NULL)
        sys->api.free_decoder(&old_decoder);

    sys->current_thread_count = reopen_thread_count;
    sys->single_thread_mode = (reopen_thread_count == 0);

    sys->hard_reopen_calls++;
    sys->last_hard_reopen_flags = block_flags;
    if (cause != NULL)
    {
        if (strcmp(cause, "pf_flush") == 0)
            sys->hard_reopen_from_flush++;
        else if (strstr(cause, "discont") != NULL)
            sys->hard_reopen_from_decode_discont++;
        else if (strstr(cause, "corrupt") != NULL)
            sys->hard_reopen_from_decode_corrupt++;
        else
            sys->hard_reopen_from_decode_other++;
    }
    else
    {
        sys->hard_reopen_from_decode_other++;
    }

    msg_Dbg(dec,
            "edge264mvc decoder hard-reopened: cause=%s flags=0x%x tsq=%u out_stereo=%d post_blocks=%u threads=%d force_single=%d",
            cause != NULL ? cause : "unknown", block_flags, sys->ts_count,
            sys->out_stereo ? 1 : 0, sys->post_discontinuity_blocks,
            reopen_thread_count, 0);
    return VLCDEC_SUCCESS;
}

static int OpenDecoder(vlc_object_t *obj)
{
    decoder_t *dec = (decoder_t *)obj;

    if (dec->fmt_in.i_codec != VLC_CODEC_H264 &&
        dec->fmt_in.i_codec != OPEN3DBLURAYMVC_CODEC_MVC &&
        dec->fmt_in.i_codec != OPEN3DMKV_CODEC_H264)
        return VLC_EGENERIC;

    msg_Dbg(dec,
            "edge264mvc open request: codec=0x%8.8x id=%d width=%u height=%u packetized=%d fps=%u/%u sar=%u:%u",
            dec->fmt_in.i_codec,
            dec->fmt_in.i_id,
            dec->fmt_in.video.i_width,
            dec->fmt_in.video.i_height,
            dec->fmt_in.b_packetized ? 1 : 0,
            dec->fmt_in.video.i_frame_rate,
            dec->fmt_in.video.i_frame_rate_base,
            dec->fmt_in.video.i_sar_num,
            dec->fmt_in.video.i_sar_den);

    /*
     * Keep plain H.264 launcher-opt-in only, but allow our internal Matroska AVC
     * tag and the Blu-ray MVC codec tag to route here automatically.
     */
    if (!IsExplicitOptIn(dec))
        return VLC_EGENERIC;

    edge264mvc_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;
    vlc_mutex_init(&sys->lock);
    ClearOfmdExtractedState(&sys->ofmd_extracted);
    Open3DSubtitleDepthStateClear(&sys->ofmd_selected_depth);
    sys->ofmd_selected_entry = -1;

    int ret = LoadEdge264Api(dec, sys);
    if (ret != VLC_SUCCESS)
    {
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        return ret;
    }

    char *dump_path = var_InheritString(dec, "edge264mvc-dump-annexb");
    if (dump_path != NULL && *dump_path != '\0')
    {
        sys->dump_annexb_fp = fopen(dump_path, "wb");
        if (sys->dump_annexb_fp == NULL)
        {
            msg_Warn(dec, "edge264mvc failed to open dump path %s: %s", dump_path, vlc_strerror_c(errno));
        }
        else
        {
            setvbuf(sys->dump_annexb_fp, NULL, _IONBF, 0);
            msg_Info(dec, "edge264mvc dump enabled: %s", dump_path);
        }
    }
    free(dump_path);

    int thread_count = var_InheritInteger(dec, "edge264mvc-threads");
    const char *env_threads = getenv("EDGE264MVC_THREADS");
    if (env_threads != NULL && *env_threads != '\0')
    {
        errno = 0;
        char *end = NULL;
        long parsed = strtol(env_threads, &end, 10);
        if (errno == 0 && end != env_threads && *end == '\0' &&
            parsed >= INT_MIN && parsed <= INT_MAX)
        {
            thread_count = (int)parsed;
        }
        else
        {
            msg_Warn(dec, "invalid EDGE264MVC_THREADS=%s ignored", env_threads);
        }
    }
    if (thread_count < -1)
        thread_count = 0;
    sys->alloc_thread_count = thread_count;
    sys->current_thread_count = thread_count;
    sys->single_thread_mode = (thread_count == 0);
    sys->last_nal_type = 0xffu;

    int recover_gap = var_InheritInteger(dec, "edge264mvc-recover-gap");
    if (recover_gap < 0)
        recover_gap = EDGE264MVC_DEFAULT_RECOVER_FLUSH_MIN_DECODE_GAP;
    if (thread_count != 0 &&
        recover_gap == EDGE264MVC_DEFAULT_RECOVER_FLUSH_MIN_DECODE_GAP)
    {
        recover_gap = EDGE264MVC_DEFAULT_RECOVER_FLUSH_MIN_DECODE_GAP_THREADED;
    }
    sys->recover_flush_min_decode_gap = (unsigned)recover_gap;

    int enobufs_loop_max = var_InheritInteger(dec, "edge264mvc-enobufs-loop-max");
    if (enobufs_loop_max < 1)
        enobufs_loop_max = EDGE264MVC_DEFAULT_ENOBUFS_LOOP_MAX;
    /*
     * Threaded decode benefits from a larger ENOBUFS retry budget; single-thread
     * mode keeps the tighter limit to avoid long busy-loop stalls.
     */
    if (thread_count != 0 && enobufs_loop_max == EDGE264MVC_DEFAULT_ENOBUFS_LOOP_MAX)
        enobufs_loop_max = EDGE264MVC_DEFAULT_ENOBUFS_LOOP_MAX_THREADED;
    sys->enobufs_loop_max = (unsigned)enobufs_loop_max;

    int enobufs_drain_period = var_InheritInteger(dec, "edge264mvc-enobufs-drain-period");
    if (enobufs_drain_period < 1)
        enobufs_drain_period = EDGE264MVC_DEFAULT_ENOBUFS_DRAIN_PERIOD;
    sys->enobufs_drain_period = (unsigned)enobufs_drain_period;

    int enobufs_sleep_us = var_InheritInteger(dec, "edge264mvc-enobufs-sleep-us");
    if (enobufs_sleep_us < 0)
        enobufs_sleep_us = EDGE264MVC_DEFAULT_ENOBUFS_SLEEP_US;
    sys->enobufs_retry_sleep_us = (unsigned)enobufs_sleep_us;

    sys->recover_viewext_errors = var_InheritBool(dec, "edge264mvc-recover-viewext-errors");
    sys->advertise_multiview = var_InheritBool(dec, "edge264mvc-advertise-multiview");
    sys->log_frameids = var_InheritBool(dec, "edge264mvc-log-frameids");
    sys->log_sei = var_InheritBool(dec, "edge264mvc-log-sei");
    int frameid_log_every = var_InheritInteger(dec, "edge264mvc-frameid-log-every");
    if (frameid_log_every < 0)
        frameid_log_every = 0;
    sys->frameid_log_every = (unsigned)frameid_log_every;
    int right_hist_depth = var_InheritInteger(dec, "edge264mvc-right-history-depth");
    if (right_hist_depth < 0)
        right_hist_depth = 0;
    if (right_hist_depth > EDGE264MVC_RIGHT_HISTORY_MAX)
        right_hist_depth = EDGE264MVC_RIGHT_HISTORY_MAX;
    sys->right_hist_depth = (unsigned)right_hist_depth;
    sys->skip_type20_nri0 = var_InheritBool(dec, "edge264mvc-skip-type20-nri0");
    sys->gate_type20_until_subset_sps =
        var_InheritBool(dec, "edge264mvc-gate-type20-until-subset-sps");
    int gate_max = var_InheritInteger(dec, "edge264mvc-gate-type20-max-skips");
    if (gate_max < 0)
        gate_max = 0;
    sys->gate_type20_max_skips = (unsigned)gate_max;
    sys->hold_right_on_backstep =
        var_InheritBool(dec, "edge264mvc-hold-right-on-backstep");
    sys->use_pts_dates =
        var_InheritBool(dec, "edge264mvc-use-pts-dates");
    int ts_log_every = var_InheritInteger(dec, "edge264mvc-ts-log-every");
    if (ts_log_every < 0)
        ts_log_every = 0;
    sys->ts_log_every = (unsigned)ts_log_every;
    sys->normalize_ts_cadence =
        var_InheritBool(dec, "edge264mvc-normalize-ts-cadence");
    if (!sys->normalize_ts_cadence &&
        dec->fmt_in.i_codec == OPEN3DMKV_CODEC_H264)
    {
        sys->normalize_ts_cadence = true;
        msg_Dbg(dec,
                "edge264mvc auto-enabled MKV timestamp normalization for internal mkv route");
    }
    bool mkv_bridge_present = false;
    ReadMkvSubtitleMapping(dec, &mkv_bridge_present,
                           &sys->mkv_subtitle_plane,
                           &sys->mkv_subtitle_source_id);

    if (sys->mkv_subtitle_plane >= 0 || sys->mkv_subtitle_source_id >= 0)
    {
        msg_Dbg(dec,
                "edge264mvc selected MKV subtitle mapping plane=%d source_id=%d via=%s",
                sys->mkv_subtitle_plane,
                sys->mkv_subtitle_source_id,
                mkv_bridge_present ? "bridge" : "inherited");
        PublishSelectedMkvOfmdDepth(dec, sys);
    }
    sys->seen_subset_sps = false;
    sys->warned_gate_limit = false;
    RightHistoryReset(sys, false);

    msg_Info(dec,
             "edge264mvc open config: threads=%d frameid_log=%d sei_log=%d frameid_every=%u use_pts_dates=%d ts_log_every=%u normalize_ts=%d",
             thread_count, sys->log_frameids ? 1 : 0, sys->log_sei ? 1 : 0, sys->frameid_log_every,
             sys->use_pts_dates ? 1 : 0, sys->ts_log_every,
             sys->normalize_ts_cadence ? 1 : 0);

    sys->decoder = AllocEdge264Decoder(dec, sys, sys->current_thread_count);
    if (sys->decoder == NULL)
    {
        msg_Err(dec, "edge264_alloc failed");
        if (sys->dump_annexb_fp != NULL)
        {
            fclose(sys->dump_annexb_fp);
            sys->dump_annexb_fp = NULL;
        }
        UnloadEdge264Api(sys);
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        return VLC_EGENERIC;
    }

    dec->p_sys = (decoder_sys_t *)sys;
    dec->pf_decode = DecodeBlock;
    dec->pf_flush = FlushDecoder;

    es_format_Copy(&dec->fmt_out, &dec->fmt_in);
    dec->fmt_out.i_cat = VIDEO_ES;
    dec->fmt_out.i_codec = VLC_CODEC_I420;
    dec->fmt_out.video.i_chroma = VLC_CODEC_I420;
    dec->fmt_out.video.multiview_mode = MULTIVIEW_2D;

    TsQueueReset(sys);

    ret = FeedExtradata(dec, sys);
    if (ret != VLCDEC_SUCCESS)
    {
        if (sys->decoder != NULL)
            sys->api.free_decoder(&sys->decoder);
        if (sys->dump_annexb_fp != NULL)
        {
            fclose(sys->dump_annexb_fp);
            sys->dump_annexb_fp = NULL;
        }
        UnloadEdge264Api(sys);
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        dec->p_sys = NULL;
        return VLC_EGENERIC;
    }
    sys->extradata_seeded = true;

    msg_Info(dec,
             "edge264mvc decoder initialized (runtime libedge264 loaded, threads=%d"
             " recover_gap=%u enobufs_loop=%u enobufs_drain=%u enobufs_sleep_us=%u"
             " post_discont(blocks=%u,enobufs_loop=%u)"
             " recover_viewext=%d advertise_multiview=%d"
             " frameid_log=%d frameid_every=%u right_hist_depth=%u"
             " skip_t20_nri0=%d gate_t20_until_sps=%d gate_t20_max=%u hold_right_backstep=%d)",
             thread_count, sys->recover_flush_min_decode_gap,
             sys->enobufs_loop_max, sys->enobufs_drain_period,
             sys->enobufs_retry_sleep_us,
             EDGE264MVC_POST_DISCONT_BLOCKS,
             EDGE264MVC_POST_DISCONT_ENOBUFS_LOOP_MAX,
             sys->recover_viewext_errors ? 1 : 0,
             sys->advertise_multiview ? 1 : 0,
             sys->log_frameids ? 1 : 0, sys->frameid_log_every,
             sys->right_hist_depth, sys->skip_type20_nri0 ? 1 : 0,
             sys->gate_type20_until_subset_sps ? 1 : 0,
             sys->gate_type20_max_skips,
             sys->hold_right_on_backstep ? 1 : 0);
    return VLC_SUCCESS;
}

static int DecodeBlock(decoder_t *dec, block_t *block)
{
    edge264mvc_sys_t *sys = (edge264mvc_sys_t *)dec->p_sys;
    vlc_mutex_lock(&sys->lock);

    if (block == NULL)
    {
        sys->drain_calls++;
        if ((sys->drain_calls % 256u) == 0u || (!sys->warned_drain_poll && sys->drain_calls >= 32))
        {
            msg_Dbg(dec,
                    "edge264mvc drain poll=%" PRIu64 " blocks=%" PRIu64
                    " frames=%" PRIu64 " tsq=%u decode_calls=%" PRIu64
                    " enobufs=%" PRIu64 " enodata=%" PRIu64,
                    sys->drain_calls, sys->blocks_seen, sys->frames_queued,
                    sys->ts_count, sys->decode_calls,
                    sys->decode_ret_enobufs, sys->decode_ret_enodata);
            sys->warned_drain_poll = true;
        }
        int ret = DrainDecoder(dec, sys, EDGE264MVC_DRAIN_LOOP_MAX);
        vlc_mutex_unlock(&sys->lock);
        if (ret != VLCDEC_SUCCESS)
            return ret;
        return VLCDEC_SUCCESS;
    }

    sys->blocks_seen++;

    const bool discontinuity = (block->i_flags & BLOCK_FLAG_DISCONTINUITY) != 0;
    if (block->i_flags & (BLOCK_FLAG_DISCONTINUITY | BLOCK_FLAG_CORRUPTED))
    {
        const bool corrupted = (block->i_flags & BLOCK_FLAG_CORRUPTED) != 0;
        if (discontinuity && !corrupted && sys->skip_next_discontinuity_reopen)
        {
            sys->skip_next_discontinuity_reopen = false;
            ArmPostSeekRecovery(sys, true);
        }
        else
        {
        const char *reopen_cause =
            discontinuity
                ? ((block->i_flags & BLOCK_FLAG_CORRUPTED) != 0
                       ? "decode_block_discont+corrupt"
                       : "decode_block_discont")
                : "decode_block_corrupt";
        FlushDecoderLocked(sys);
        if (ReopenDecoderLocked(dec, sys, reopen_cause, (uint32_t)block->i_flags) != VLCDEC_SUCCESS)
        {
            block_Release(block);
            vlc_mutex_unlock(&sys->lock);
            return VLCDEC_ECRITICAL;
        }
        sys->skip_next_discontinuity_reopen = false;
        if (discontinuity)
            ArmPostSeekRecovery(sys, true);
        }
    }

    if (block->i_flags & BLOCK_FLAG_CORRUPTED)
    {
        block_Release(block);
        if (sys->post_discontinuity_blocks > 0)
            sys->post_discontinuity_blocks--;
        vlc_mutex_unlock(&sys->lock);
        return VLCDEC_SUCCESS;
    }

    /*
     * Default to DTS because this branch emits frames in decode-oriented order.
     * Keep PTS selection as an opt-in diagnostic for native MKV pacing work.
     */
    vlc_tick_t ts;
    if (sys->use_pts_dates && block->i_pts > VLC_TICK_INVALID)
        ts = block->i_pts;
    else if (block->i_dts > VLC_TICK_INVALID)
        ts = block->i_dts;
    else
        ts = block->i_pts;
    TsQueuePush(dec, sys, ts);

    const bool eos_hint = (block->i_flags & BLOCK_FLAG_END_OF_SEQUENCE) != 0;

    if (block->i_buffer == 0)
    {
        block_Release(block);
        if (sys->post_discontinuity_blocks > 0)
            sys->post_discontinuity_blocks--;
        vlc_mutex_unlock(&sys->lock);
        return VLCDEC_SUCCESS;
    }

    int ret;
    const bool has_start_code = BufferHasStartCode(block->p_buffer, block->i_buffer);
    if (sys->avcc_length_size >= 1 && sys->avcc_length_size <= 4)
    {
        ret = DecodeAvccBuffer(dec, sys, block->p_buffer, block->i_buffer, eos_hint);
    }
    else if (has_start_code || sys->annexb_announced || sys->annexb_pending_len > 0)
    {
        if (has_start_code)
            sys->annexb_announced = true;
        ret = DecodeAnnexBBuffer(dec, sys, block->p_buffer, block->i_buffer, eos_hint);
    }
    else
    {
        if (!sys->warned_missing_layout)
        {
            msg_Warn(dec, "edge264mvc cannot determine H.264 NAL layout (no Annex-B start codes and no avcC length size)");
            sys->warned_missing_layout = true;
        }
        ret = VLCDEC_SUCCESS;
    }

    block_Release(block);
    if (ret == VLCDEC_SUCCESS && eos_hint)
    {
        uint8_t dummy = 0;
        int eos_ret = sys->api.decode_NAL(sys->decoder, &dummy, &dummy, NULL, NULL);
        sys->decode_calls++;
        RecordDecodeRet(sys, eos_ret);
        if (eos_ret == ENOMEM || eos_ret == EINVAL)
        {
            vlc_mutex_unlock(&sys->lock);
            return VLCDEC_ECRITICAL;
        }

        int dr = DrainDecoder(dec, sys, EDGE264MVC_DRAIN_LOOP_MAX);
        if (sys->post_discontinuity_blocks > 0)
            sys->post_discontinuity_blocks--;
        vlc_mutex_unlock(&sys->lock);
        if (dr != VLCDEC_SUCCESS)
            return dr;
        return ret;
    }
    if (sys->post_discontinuity_blocks > 0)
        sys->post_discontinuity_blocks--;
    vlc_mutex_unlock(&sys->lock);
    return ret;
}

static void CloseDecoder(vlc_object_t *obj)
{
    decoder_t *dec = (decoder_t *)obj;
    edge264mvc_sys_t *sys = (edge264mvc_sys_t *)dec->p_sys;

    if (sys == NULL)
        return;

    if (HasMkvSubtitleMapping(sys))
    {
        Open3DSubtitleDepthStateClear(&sys->ofmd_selected_depth);
        PublishSelectedMkvOfmdDepth(dec, sys);
    }

    if (sys->decoder != NULL)
        sys->api.free_decoder(&sys->decoder);

    if (sys->dump_annexb_fp != NULL)
    {
        fclose(sys->dump_annexb_fp);
        sys->dump_annexb_fp = NULL;
    }

    UnloadEdge264Api(sys);

    msg_Dbg(dec,
            "edge264mvc stats: blocks=%" PRIu64 " nals=%" PRIu64
            " frames_popped=%" PRIu64 " frames=%" PRIu64
            " drains=%" PRIu64 " decode_calls=%" PRIu64
            " ret0=%" PRIu64 " enodata=%" PRIu64 " enobufs=%" PRIu64
            " enotsup=%" PRIu64 " ebadmsg=%" PRIu64
            " einval=%" PRIu64 " enomem=%" PRIu64 " other=%" PRIu64
            " queue_calls=%" PRIu64 " queue_nomsg=%" PRIu64
            " queue_einval=%" PRIu64 " killed=%" PRIu64
            " recover_flushes=%" PRIu64
            " recover_flush_skips=%" PRIu64
            " enobufs_guard_flushes=%" PRIu64
            " enobufs_guard_skipped_aux=%" PRIu64
            " enobufs_drain_pulses=%" PRIu64
            " enobufs_drain_progress=%" PRIu64
            " enobufs_by_nal(t20=%" PRIu64 ",t14=%" PRIu64 ",other=%" PRIu64 ")"
            " enotsup_by_nal(t20=%" PRIu64 ",t14=%" PRIu64 ",other=%" PRIu64 ")"
            " ebadmsg_by_nal(t20=%" PRIu64 ",t14=%" PRIu64 ",other=%" PRIu64 ")"
            " tuning(recover_gap=%u,enobufs_loop=%u,enobufs_drain=%u,enobufs_sleep_us=%u,recover_viewext=%d,post_discont_left=%u)"
            " first_enobufs=(nal=%" PRIu64 ",type=%u)"
            " first_enotsup=(nal=%" PRIu64 ",type=%u)"
            " first_ebadmsg=(nal=%" PRIu64 ",type=%u)"
            " skipped_terminal_ext=%" PRIu64
            " skipped_reserved=%" PRIu64
            " skipped_wait_idr_vcl=%" PRIu64
            " skipped_type20_nri0=%" PRIu64
            " startup_gated_type20=%" PRIu64
            " startup_seen_subset_sps=%" PRIu64
            " wait_idr_after_flush=%d"
            " post_seek(frames=%" PRIu64 ",mono=%" PRIu64
            ",stereo=%" PRIu64 ",mono_drop=%" PRIu64 ",invalid_ts=%" PRIu64
            ",ts_backsteps=%" PRIu64 ",nomsg=%" PRIu64
            ",idle_loops=%" PRIu64 ",repeat_base=%" PRIu64
            ",repeat_right=%" PRIu64 ")"
            " frameid(samples=%" PRIu64 ",nonzero=%" PRIu64
            ",right_ahead=%" PRIu64 ",left_ahead=%" PRIu64
            ",last_delta=%d,max_abs=%d,log=%d,every=%u)"
            " sel_frameid(samples=%" PRIu64 ",nonzero=%" PRIu64
            ",right_ahead=%" PRIu64 ",left_ahead=%" PRIu64
            ",last_delta=%d,max_abs=%d,backsteps=%" PRIu64 ",repeats=%" PRIu64 ")"
            " right_hist(selects=%" PRIu64 ",hits=%" PRIu64
            ",exact=%" PRIu64 ",no_past=%" PRIu64
            ",mono_forced_hold=%" PRIu64 ",mono_forced_cur=%" PRIu64
            ",mono_unresolved=%" PRIu64
            ",depth=%u,last_sel=%d)"
            " dump_nals=%" PRIu64
            " soft_trace(kept=%u,dropped=%" PRIu64 ",last_nal=%u)",
            sys->blocks_seen, sys->nals_seen, sys->frames_popped, sys->frames_queued,
            sys->drain_calls, sys->decode_calls,
            sys->decode_ret_zero, sys->decode_ret_enodata, sys->decode_ret_enobufs,
            sys->decode_ret_enotsup, sys->decode_ret_ebadmsg,
            sys->decode_ret_einval, sys->decode_ret_enomem, sys->decode_ret_other,
            sys->queue_calls, sys->queue_nomsg, sys->queue_einval,
            sys->killed_short_circuit, sys->recover_flushes,
            sys->recover_flush_skips,
            sys->enobufs_guard_flushes, sys->enobufs_guard_skipped_aux,
            sys->enobufs_drain_pulses,
            sys->enobufs_drain_progress,
            sys->enobufs_nal20, sys->enobufs_nal14, sys->enobufs_other,
            sys->enotsup_nal20, sys->enotsup_nal14, sys->enotsup_other,
            sys->ebadmsg_nal20, sys->ebadmsg_nal14, sys->ebadmsg_other,
            sys->recover_flush_min_decode_gap,
            sys->enobufs_loop_max, sys->enobufs_drain_period, sys->enobufs_retry_sleep_us,
            sys->recover_viewext_errors ? 1 : 0,
            sys->post_discontinuity_blocks,
            sys->first_enobufs_nal, sys->first_enobufs_type,
            sys->first_enotsup_nal, sys->first_enotsup_type,
            sys->first_ebadmsg_nal, sys->first_ebadmsg_type,
            sys->skipped_terminal_ext_nals, sys->skipped_reserved_nals,
            sys->skipped_wait_idr_vcl,
            sys->skipped_type20_nri0,
            sys->startup_gated_type20, sys->startup_seen_subset_sps,
            sys->wait_idr_after_flush ? 1 : 0,
            sys->post_seek_frames_queued, sys->post_seek_mono_frames,
            sys->post_seek_stereo_frames, sys->post_seek_mono_frames_dropped,
            sys->post_seek_invalid_date_frames,
            sys->post_seek_ts_backsteps, sys->post_seek_getframe_nomsg,
            sys->post_seek_drain_idle_loops,
            sys->post_seek_same_base_frame_repeats,
            sys->post_seek_same_right_frame_repeats,
            sys->frameid_samples, sys->frameid_nonzero_delta,
            sys->frameid_right_ahead, sys->frameid_left_ahead,
            sys->frameid_last_delta, sys->frameid_max_abs_delta,
            sys->log_frameids ? 1 : 0, sys->frameid_log_every,
            sys->selected_frameid_samples, sys->selected_frameid_nonzero_delta,
            sys->selected_frameid_right_ahead, sys->selected_frameid_left_ahead,
            sys->selected_frameid_last_delta, sys->selected_frameid_max_abs_delta,
            sys->selected_frameid_backsteps, sys->selected_frameid_repeats,
            sys->right_hist_selects, sys->right_hist_select_hits,
            sys->right_hist_select_exact, sys->right_hist_select_no_past,
            sys->right_hist_monotonic_forced_hold,
            sys->right_hist_monotonic_forced_current,
            sys->right_hist_monotonic_unresolved,
            sys->right_hist_depth, sys->selected_right_last_frameid,
            sys->dump_nals,
            sys->soft_trace_count, sys->soft_trace_dropped, sys->last_nal_type);

    for (unsigned i = 0; i < sys->soft_trace_count; ++i)
    {
        const edge264_soft_trace_t *t = &sys->soft_trace[i];
        msg_Dbg(dec,
                "edge264mvc softtrace[%u]: ret=%d nal=%" PRIu64
                " type=%u prev=%u nri=%u action=%u decode_call=%" PRIu64
                " frames(popped=%" PRIu64 ",queued=%" PRIu64 ")"
                " wait_idr=%d paramset=%d subset_sps=%d",
                i, t->ret_code, t->nal_idx,
                t->nal_type, t->prev_nal_type, t->nal_ref_idc, t->recover_action,
                t->decode_call, t->frames_popped, t->frames_queued,
                t->wait_idr_after_flush ? 1 : 0,
                t->wait_idr_have_paramset ? 1 : 0,
                t->seen_subset_sps ? 1 : 0);
    }

    free(sys->avcc_pending);
    sys->avcc_pending = NULL;
    sys->avcc_pending_cap = 0;
    sys->avcc_pending_len = 0;
    sys->avcc_announced = false;

    free(sys->annexb_pending);
    sys->annexb_pending = NULL;
    sys->annexb_pending_cap = 0;
    sys->annexb_pending_len = 0;
    sys->annexb_announced = false;
    RightHistoryReset(sys, true);

    vlc_mutex_destroy(&sys->lock);
    free(sys);
    dec->p_sys = NULL;
}
