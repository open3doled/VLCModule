/**
 * @file open3dannexb.c
 * @brief Minimal Open3D MVC Annex-B passthrough demux for VLC 3.0.23
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <inttypes.h>
#include <string.h>
#include <strings.h>

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_demux.h>
#include <vlc_plugin.h>
#include <vlc_stream.h>

#define OPEN3DANNEXB_PASSTHROUGH_BLOCK (64u * 1024u)

typedef struct
{
    es_out_id_t *es;
    es_format_t fmt;
    bool have_output_time;
    bool mark_next_discontinuity;
    vlc_tick_t next_pts;
    vlc_tick_t first_output_time;
    vlc_tick_t current_output_time;
    uint64_t passthrough_blocks;
    uint64_t output_discontinuity_marks;
    uint64_t ctrl_get_time;
    uint64_t ctrl_get_length;
    uint64_t ctrl_get_pos;
    uint64_t ctrl_set_time;
    uint64_t ctrl_set_pos;
    uint64_t ctrl_other;
} open3dannexb_sys_t;

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);
static int Demux(demux_t *);
static int Control(demux_t *, int, va_list);

vlc_module_begin()
    set_shortname(N_("Open3D Annex-B"))
    set_description(N_("Open3D MVC Annex-B passthrough demux"))
    set_capability("demux", 7)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_DEMUX)
    add_shortcut("open3dannexb")
    set_callbacks(Open, Close)
vlc_module_end()

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

static bool has_annexb_extension(const char *psz_file)
{
    if (psz_file == NULL)
        return false;

    const char *ext = strrchr(psz_file, '.');
    if (ext == NULL || ext[1] == '\0')
        return false;

    ext++;
    return !strcasecmp(ext, "264") ||
           !strcasecmp(ext, "h264") ||
           !strcasecmp(ext, "mvc");
}

static int64_t tick_to_ms_or_neg1(vlc_tick_t tick)
{
    return (tick == VLC_TICK_INVALID) ? -1 : MS_FROM_VLC_TICK(tick);
}

static void arm_output_discontinuity(open3dannexb_sys_t *sys)
{
    if (sys != NULL)
        sys->mark_next_discontinuity = true;
}

static void apply_output_discontinuity(open3dannexb_sys_t *sys, block_t *out)
{
    if (sys == NULL || out == NULL || !sys->mark_next_discontinuity)
        return;

    out->i_flags |= BLOCK_FLAG_DISCONTINUITY;
    sys->mark_next_discontinuity = false;
    sys->output_discontinuity_marks++;
}

static void seed_seek_target_clock(demux_t *demux, open3dannexb_sys_t *sys, vlc_tick_t target_time)
{
    if (demux == NULL || sys == NULL || target_time == VLC_TICK_INVALID || target_time < 0)
        return;

    es_out_SetPCR(demux->out, target_time);
    sys->have_output_time = true;
    sys->first_output_time = 0;
    sys->current_output_time = target_time;
    sys->next_pts = target_time;
    arm_output_discontinuity(sys);

    msg_Info(demux, "open3dannexb seek clock preseed: target_ms=%" PRId64,
             tick_to_ms_or_neg1(target_time));
}

static void reset_runtime_state(open3dannexb_sys_t *sys)
{
    if (sys == NULL)
        return;

    sys->have_output_time = false;
    sys->first_output_time = VLC_TICK_INVALID;
    sys->current_output_time = VLC_TICK_INVALID;
    sys->next_pts = 0;
    arm_output_discontinuity(sys);
}

static int Open(vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;

    if (demux->s == NULL)
        return VLC_EGENERIC;

    if (!demux->obj.force && !has_annexb_extension(demux->psz_file))
        return VLC_EGENERIC;

    open3dannexb_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;

    demux->p_sys = (demux_sys_t *)sys;
    sys->next_pts = 0;
    sys->first_output_time = VLC_TICK_INVALID;
    sys->current_output_time = VLC_TICK_INVALID;

    es_format_Init(&sys->fmt, VIDEO_ES, VLC_CODEC_H264);
    sys->fmt.i_original_fourcc = VLC_CODEC_H264;
    /* Preserve MVC extension NAL units for edge264mvc by bypassing h264 packetizer. */
    sys->fmt.b_packetized = true;
    sys->es = es_out_Add(demux->out, &sys->fmt);
    if (sys->es == NULL)
        goto error;

    demux->pf_demux = Demux;
    demux->pf_control = Control;

    msg_Info(demux, "open3dannexb Annex-B passthrough mode enabled");
    return VLC_SUCCESS;

error:
    if (sys->es != NULL)
        es_out_Del(demux->out, sys->es);
    es_format_Clean(&sys->fmt);
    free(sys);
    demux->p_sys = NULL;
    return VLC_EGENERIC;
}

static void Close(vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    open3dannexb_sys_t *sys = (open3dannexb_sys_t *)demux->p_sys;

    if (sys == NULL)
        return;

    msg_Info(demux,
             "open3dannexb summary: passthrough_blocks=%" PRIu64
             " output_discont=%" PRIu64
             " ctrl(get_time=%" PRIu64 ",get_len=%" PRIu64 ",get_pos=%" PRIu64
             ",set_time=%" PRIu64 ",set_pos=%" PRIu64 ",other=%" PRIu64 ")",
             sys->passthrough_blocks,
             sys->output_discontinuity_marks,
             sys->ctrl_get_time, sys->ctrl_get_length, sys->ctrl_get_pos,
             sys->ctrl_set_time, sys->ctrl_set_pos, sys->ctrl_other);

    if (sys->es != NULL)
        es_out_Del(demux->out, sys->es);
    es_format_Clean(&sys->fmt);
    free(sys);
    demux->p_sys = NULL;
}

static int Demux(demux_t *demux)
{
    open3dannexb_sys_t *sys = (open3dannexb_sys_t *)demux->p_sys;

    block_t *chunk = vlc_stream_Block(demux->s, OPEN3DANNEXB_PASSTHROUGH_BLOCK);
    if (chunk == NULL || chunk->i_buffer == 0)
    {
        if (chunk != NULL)
            block_Release(chunk);
        return VLC_DEMUXER_EOF;
    }

    chunk->i_pts = sys->next_pts;
    chunk->i_dts = sys->next_pts;
    chunk->i_length = VLC_TICK_FROM_MS(1);

    apply_output_discontinuity(sys, chunk);
    if (!sys->have_output_time)
    {
        sys->have_output_time = true;
        sys->first_output_time = chunk->i_pts;
    }
    sys->current_output_time = chunk->i_pts;

    es_out_SetPCR(demux->out, chunk->i_pts);
    sys->next_pts += chunk->i_length;
    es_out_Send(demux->out, sys->es, chunk);
    sys->passthrough_blocks++;
    return VLC_DEMUXER_SUCCESS;
}

static int Control(demux_t *demux, int query, va_list args)
{
    open3dannexb_sys_t *sys = (open3dannexb_sys_t *)demux->p_sys;

    switch (query)
    {
        case DEMUX_CAN_SEEK:
        {
            bool *pb = va_arg(args, bool *);
            *pb = (stream_Size(demux->s) > 0);
            return VLC_SUCCESS;
        }
        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_CONTROL_PACE:
        {
            bool *pb = va_arg(args, bool *);
            *pb = true;
            return VLC_SUCCESS;
        }
        case DEMUX_GET_TIME:
            sys->ctrl_get_time++;
            if (sys->have_output_time &&
                sys->first_output_time != VLC_TICK_INVALID &&
                sys->current_output_time != VLC_TICK_INVALID &&
                sys->current_output_time >= sys->first_output_time)
            {
                int64_t *pi = va_arg(args, int64_t *);
                *pi = sys->current_output_time - sys->first_output_time;
                return VLC_SUCCESS;
            }
            else
            {
                int64_t *pi = va_arg(args, int64_t *);
                *pi = sys->next_pts;
                return VLC_SUCCESS;
            }
        case DEMUX_GET_LENGTH:
            sys->ctrl_get_length++;
            if (sys->have_output_time &&
                sys->first_output_time != VLC_TICK_INVALID &&
                sys->current_output_time != VLC_TICK_INVALID &&
                sys->current_output_time >= sys->first_output_time)
            {
                const int64_t size = stream_Size(demux->s);
                const uint64_t off = vlc_stream_Tell(demux->s);
                if (size > 0 && off > 0)
                {
                    const vlc_tick_t elapsed = sys->current_output_time - sys->first_output_time;
                    const int64_t estimated = (int64_t)((double)elapsed * ((double)size / (double)off));
                    if (estimated > 0)
                    {
                        int64_t *pi = va_arg(args, int64_t *);
                        *pi = estimated;
                        return VLC_SUCCESS;
                    }
                }
            }
            return VLC_EGENERIC;
        case DEMUX_GET_POSITION:
            sys->ctrl_get_pos++;
        {
            const int64_t size = stream_Size(demux->s);
            if (size <= 0)
                return VLC_EGENERIC;
            double *pf = va_arg(args, double *);
            *pf = (double)((uint64_t)vlc_stream_Tell(demux->s)) / (double)size;
            if (*pf > 1.0)
                *pf = 1.0;
            return VLC_SUCCESS;
        }
        case DEMUX_SET_TIME:
            sys->ctrl_set_time++;
        {
            va_list helper_args;
            va_copy(helper_args, args);

            int64_t target_time = va_arg(args, int64_t);
            if (target_time < 0)
                target_time = 0;

            const int64_t size = stream_Size(demux->s);
            const uint64_t from_off = vlc_stream_Tell(demux->s);
            if (target_time == 0 && vlc_stream_Seek(demux->s, 0) == 0)
            {
                msg_Info(demux, "open3dannexb seek_time: mode=origin from_off=%" PRIu64, from_off);
                seed_seek_target_clock(demux, sys, 0);
                va_end(helper_args);
                return VLC_SUCCESS;
            }

            if (size > 0 && sys->have_output_time &&
                sys->first_output_time != VLC_TICK_INVALID &&
                sys->current_output_time != VLC_TICK_INVALID &&
                sys->current_output_time > sys->first_output_time &&
                from_off > 0)
            {
                const vlc_tick_t elapsed = sys->current_output_time - sys->first_output_time;
                const int64_t estimated_length =
                    (int64_t)((double)elapsed * ((double)size / (double)from_off));
                if (estimated_length > 0)
                {
                    uint64_t target = (uint64_t)((double)target_time * ((double)size / (double)estimated_length));
                    if (target > (uint64_t)size)
                        target = (uint64_t)size;
                    if (vlc_stream_Seek(demux->s, target) == 0)
                    {
                        msg_Info(demux,
                                 "open3dannexb seek_time: mode=ratio from_off=%" PRIu64
                                 " target_off=%" PRIu64 " target_ms=%" PRId64,
                                 from_off, target, MS_FROM_VLC_TICK(target_time));
                        seed_seek_target_clock(demux, sys, (vlc_tick_t)target_time);
                        va_end(helper_args);
                        return VLC_SUCCESS;
                    }
                }
            }

            int ret = demux_vaControlHelper(demux->s, 0, -1, 0, 1, query, helper_args);
            va_end(helper_args);
            if (ret == VLC_SUCCESS)
            {
                msg_Info(demux,
                         "open3dannexb seek_time: mode=helper from_off=%" PRIu64
                         " target_ms=%" PRId64 " landed_off=%" PRIu64,
                         from_off, MS_FROM_VLC_TICK(target_time), vlc_stream_Tell(demux->s));
                seed_seek_target_clock(demux, sys, (vlc_tick_t)target_time);
                return VLC_SUCCESS;
            }
            return ret;
        }
        case DEMUX_SET_POSITION:
            sys->ctrl_set_pos++;
        {
            double f = va_arg(args, double);
            if (f < 0.0)
                f = 0.0;
            if (f > 1.0)
                f = 1.0;

            const int64_t size = stream_Size(demux->s);
            if (size <= 0)
                return VLC_EGENERIC;

            const uint64_t from_off = vlc_stream_Tell(demux->s);
            const uint64_t target = (uint64_t)(f * (double)size);
            vlc_tick_t target_time = VLC_TICK_INVALID;

            if (sys->have_output_time &&
                sys->first_output_time != VLC_TICK_INVALID &&
                sys->current_output_time != VLC_TICK_INVALID &&
                sys->current_output_time >= sys->first_output_time &&
                from_off > 0)
            {
                const vlc_tick_t elapsed = sys->current_output_time - sys->first_output_time;
                const int64_t estimated_length =
                    (int64_t)((double)elapsed * ((double)size / (double)from_off));
                if (estimated_length > 0)
                    target_time = (vlc_tick_t)(f * (double)estimated_length);
            }

            if (vlc_stream_Seek(demux->s, target) != 0)
                return VLC_EGENERIC;

            msg_Info(demux,
                     "open3dannexb seek_pos: from_off=%" PRIu64
                     " target_off=%" PRIu64 " position=%.5f target_ms=%" PRId64,
                     from_off, target, f, tick_to_ms_or_neg1(target_time));

            if (target_time != VLC_TICK_INVALID)
                seed_seek_target_clock(demux, sys, target_time);
            else
                reset_runtime_state(sys);
            return VLC_SUCCESS;
        }
        default:
            sys->ctrl_other++;
            break;
    }

    return demux_vaControlHelper(demux->s, 0, -1, 0, 1, query, args);
}
