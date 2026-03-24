#ifndef VLC_OPEN3D_SUBTITLE_BRIDGE_H
#define VLC_OPEN3D_SUBTITLE_BRIDGE_H 1

#include <vlc_common.h>
#include <vlc_subpicture.h>
#include <vlc_variables.h>

#define OPEN3D_SUBTITLE_BRIDGE_VAR "open3d-subtitle-bridge"
#define OPEN3D_SUBTITLE_BRIDGE_ENABLE_VAR "open3d-subtitle-bridge-enable"

#define OPEN3D_SUBTITLE_OFFSET_VALID_VAR "open3d-subtitle-offset-valid"
#define OPEN3D_SUBTITLE_OFFSET_SIGNED_VAR "open3d-subtitle-offset-signed"
#define OPEN3D_SUBTITLE_OFFSET_RAW_VAR "open3d-subtitle-offset-raw"
#define OPEN3D_SUBTITLE_OFFSET_SEQ_VAR "open3d-subtitle-offset-seq"
#define OPEN3D_SUBTITLE_OFFSET_FRAME_VAR "open3d-subtitle-offset-frame"

#define OPEN3D_MKV_SUBTITLE_BRIDGE_VALID_VAR "open3d-mkv-subtitle-bridge-valid"
#define OPEN3D_MKV_SUBTITLE_FORCE_VAR "open3d-mkv-subtitle-force"
#define OPEN3D_MKV_SUBTITLE_STATIC_UNITS_VAR "open3d-mkv-subtitle-static-offset-units"
#define OPEN3D_MKV_SUBTITLE_PLANE_VAR "open3d-mkv-subtitle-plane"
#define OPEN3D_MKV_SUBTITLE_SOURCE_ID_VAR "open3d-mkv-subtitle-source-id"

typedef struct
{
    bool valid;
    uint8_t sequence;
    uint8_t raw;
    int8_t signed_offset;
    int frame_index;
} open3d_subtitle_depth_state_t;

typedef struct
{
    vlc_mutex_t lock;
    bool active;
    unsigned width;
    unsigned height;
    vlc_tick_t pts;
    open3d_subtitle_depth_state_t depth;
    subpicture_region_t *regions;
} open3d_subtitle_bridge_t;

static inline void Open3DSubtitleDepthStateClear(open3d_subtitle_depth_state_t *state)
{
    if (state == NULL)
        return;

    state->valid = false;
    state->sequence = 0xff;
    state->raw = 0;
    state->signed_offset = 0;
    state->frame_index = -1;
}

static inline void Open3DSubtitleDepthStateSet(open3d_subtitle_depth_state_t *state,
                                               bool valid,
                                               uint8_t sequence,
                                               uint8_t raw,
                                               int8_t signed_offset,
                                               int frame_index)
{
    if (state == NULL)
        return;

    state->valid = valid;
    state->sequence = sequence;
    state->raw = raw;
    state->signed_offset = signed_offset;
    state->frame_index = frame_index;
}

static inline int Open3DMkvSubtitleDepthResolveEntryIndex(size_t entry_count,
                                                          int plane)
{
    if (plane < 0 || entry_count == 0)
        return -1;

    /*
     * The tracked MakeMKV OFMD layout exposes one leading reserved/default
     * slot followed by plane-indexed entries. Prefer that layout when present,
     * but keep a direct-index fallback for shallower tables.
     */
    if ((size_t)(plane + 1) < entry_count)
        return plane + 1;
    if ((size_t)plane < entry_count)
        return plane;
    return -1;
}

static inline bool Open3DMkvSubtitleDepthSelectPlane(
    const open3d_subtitle_depth_state_t *candidates,
    size_t entry_count,
    int plane,
    open3d_subtitle_depth_state_t *selected,
    int *selected_index)
{
    if (selected != NULL)
        Open3DSubtitleDepthStateClear(selected);
    if (selected_index != NULL)
        *selected_index = -1;

    const int index = Open3DMkvSubtitleDepthResolveEntryIndex(entry_count, plane);
    if (index < 0 || candidates == NULL)
        return false;

    if (selected != NULL)
        *selected = candidates[index];
    if (selected_index != NULL)
        *selected_index = index;
    return true;
}

static inline void Open3DSubtitleBridgeClearLocked(open3d_subtitle_bridge_t *bridge)
{
    if (bridge->regions != NULL)
    {
        subpicture_region_ChainDelete(bridge->regions);
        bridge->regions = NULL;
    }

    bridge->active = false;
    bridge->width = 0;
    bridge->height = 0;
    bridge->pts = VLC_TICK_INVALID;
    Open3DSubtitleDepthStateClear(&bridge->depth);
}

static inline void Open3DSubtitleBridgeInit(open3d_subtitle_bridge_t *bridge)
{
    vlc_mutex_init(&bridge->lock);
    bridge->regions = NULL;
    Open3DSubtitleBridgeClearLocked(bridge);
}

static inline void Open3DSubtitleBridgeDestroy(open3d_subtitle_bridge_t *bridge)
{
    vlc_mutex_lock(&bridge->lock);
    Open3DSubtitleBridgeClearLocked(bridge);
    vlc_mutex_unlock(&bridge->lock);
    vlc_mutex_destroy(&bridge->lock);
}

static inline int Open3DSubtitleBridgeCopyRegions(subpicture_region_t **dst_head,
                                                  const subpicture_region_t *src_head)
{
    for (const subpicture_region_t *src = src_head; src != NULL; src = src->p_next)
    {
        subpicture_region_t *copy = subpicture_region_Copy((subpicture_region_t *)src);
        if (copy == NULL)
        {
            subpicture_region_ChainDelete(*dst_head);
            *dst_head = NULL;
            return VLC_ENOMEM;
        }

        copy->p_next = NULL;
        if (*dst_head == NULL)
        {
            *dst_head = copy;
        }
        else
        {
            subpicture_region_t *tail = *dst_head;
            while (tail->p_next != NULL)
                tail = tail->p_next;
            tail->p_next = copy;
        }
    }

    return VLC_SUCCESS;
}

static inline int Open3DSubtitleBridgeUpdate(open3d_subtitle_bridge_t *bridge,
                                             unsigned width, unsigned height,
                                             const subpicture_region_t *regions,
                                             vlc_tick_t pts,
                                             const open3d_subtitle_depth_state_t *depth_state)
{
    subpicture_region_t *copy = NULL;
    if (regions != NULL &&
        Open3DSubtitleBridgeCopyRegions(&copy, regions) != VLC_SUCCESS)
        return VLC_ENOMEM;

    vlc_mutex_lock(&bridge->lock);
    Open3DSubtitleBridgeClearLocked(bridge);
    bridge->regions = copy;
    bridge->active = copy != NULL;
    bridge->width = width;
    bridge->height = height;
    bridge->pts = pts;
    if (depth_state != NULL)
        bridge->depth = *depth_state;
    vlc_mutex_unlock(&bridge->lock);
    return VLC_SUCCESS;
}

static inline void Open3DSubtitleBridgeClear(open3d_subtitle_bridge_t *bridge)
{
    vlc_mutex_lock(&bridge->lock);
    Open3DSubtitleBridgeClearLocked(bridge);
    vlc_mutex_unlock(&bridge->lock);
}

static inline subpicture_t *Open3DSubtitleBridgeCloneSubpicture(open3d_subtitle_bridge_t *bridge)
{
    subpicture_t *subpicture = NULL;

    vlc_mutex_lock(&bridge->lock);
    if (bridge->active && bridge->regions != NULL)
    {
        subpicture = subpicture_New(NULL);
        if (subpicture != NULL)
        {
            subpicture->b_absolute = true;
            subpicture->b_ephemer = true;
            subpicture->b_subtitle = true;
            subpicture->i_original_picture_width = (int)bridge->width;
            subpicture->i_original_picture_height = (int)bridge->height;
            subpicture->i_start = bridge->pts;
            subpicture->i_stop = bridge->pts;

            if (Open3DSubtitleBridgeCopyRegions(&subpicture->p_region,
                                                bridge->regions) != VLC_SUCCESS)
            {
                subpicture_Delete(subpicture);
                subpicture = NULL;
            }
        }
    }
    vlc_mutex_unlock(&bridge->lock);

    return subpicture;
}

static inline void Open3DSubtitleBridgeAttachToObject(vlc_object_t *obj,
                                                      open3d_subtitle_bridge_t *bridge)
{
    if (obj == NULL)
        return;
    var_Create(obj, OPEN3D_SUBTITLE_BRIDGE_VAR, VLC_VAR_ADDRESS);
    var_SetAddress(obj, OPEN3D_SUBTITLE_BRIDGE_VAR, bridge);
    var_Create(obj, OPEN3D_SUBTITLE_BRIDGE_ENABLE_VAR, VLC_VAR_BOOL);
}

static inline void Open3DSubtitleBridgeDetachFromObject(vlc_object_t *obj)
{
    if (obj == NULL)
        return;
    var_Create(obj, OPEN3D_SUBTITLE_BRIDGE_VAR, VLC_VAR_ADDRESS);
    var_SetAddress(obj, OPEN3D_SUBTITLE_BRIDGE_VAR, NULL);
    var_Create(obj, OPEN3D_SUBTITLE_BRIDGE_ENABLE_VAR, VLC_VAR_BOOL);
    var_SetBool(obj, OPEN3D_SUBTITLE_BRIDGE_ENABLE_VAR, false);
}

static inline void Open3DSubtitleBridgeSetEnabledOnObject(vlc_object_t *obj, bool enabled)
{
    if (obj == NULL)
        return;
    var_Create(obj, OPEN3D_SUBTITLE_BRIDGE_ENABLE_VAR, VLC_VAR_BOOL);
    var_SetBool(obj, OPEN3D_SUBTITLE_BRIDGE_ENABLE_VAR, enabled);
}

static inline bool Open3DSubtitleBridgeGetEnabledFromObject(vlc_object_t *obj)
{
    if (obj == NULL)
        return false;
    var_Create(obj, OPEN3D_SUBTITLE_BRIDGE_ENABLE_VAR, VLC_VAR_BOOL);
    return var_GetBool(obj, OPEN3D_SUBTITLE_BRIDGE_ENABLE_VAR);
}

static inline open3d_subtitle_bridge_t *Open3DSubtitleBridgeGetFromObject(vlc_object_t *obj)
{
    if (obj == NULL || var_Type(obj, OPEN3D_SUBTITLE_BRIDGE_VAR) == 0)
        return NULL;
    return (open3d_subtitle_bridge_t *)var_GetAddress(obj, OPEN3D_SUBTITLE_BRIDGE_VAR);
}

static inline void Open3DSubtitleDepthPublishToObject(vlc_object_t *obj,
                                                      const open3d_subtitle_depth_state_t *state)
{
    open3d_subtitle_depth_state_t local;

    if (obj == NULL)
        return;
    if (state != NULL)
        local = *state;
    else
        Open3DSubtitleDepthStateClear(&local);

    var_Create(obj, OPEN3D_SUBTITLE_OFFSET_VALID_VAR, VLC_VAR_BOOL);
    var_Create(obj, OPEN3D_SUBTITLE_OFFSET_SIGNED_VAR, VLC_VAR_INTEGER);
    var_Create(obj, OPEN3D_SUBTITLE_OFFSET_RAW_VAR, VLC_VAR_INTEGER);
    var_Create(obj, OPEN3D_SUBTITLE_OFFSET_SEQ_VAR, VLC_VAR_INTEGER);
    var_Create(obj, OPEN3D_SUBTITLE_OFFSET_FRAME_VAR, VLC_VAR_INTEGER);

    var_SetBool(obj, OPEN3D_SUBTITLE_OFFSET_VALID_VAR, local.valid);
    var_SetInteger(obj, OPEN3D_SUBTITLE_OFFSET_SIGNED_VAR, local.signed_offset);
    var_SetInteger(obj, OPEN3D_SUBTITLE_OFFSET_RAW_VAR, local.raw);
    var_SetInteger(obj, OPEN3D_SUBTITLE_OFFSET_SEQ_VAR, local.valid ? local.sequence : -1);
    var_SetInteger(obj, OPEN3D_SUBTITLE_OFFSET_FRAME_VAR, local.frame_index);
}

static inline bool Open3DSubtitleDepthReadFromObject(vlc_object_t *obj,
                                                     open3d_subtitle_depth_state_t *state)
{
    if (state != NULL)
        Open3DSubtitleDepthStateClear(state);
    if (obj == NULL)
        return false;

    const int valid_type = var_Type(obj, OPEN3D_SUBTITLE_OFFSET_VALID_VAR);
    const int signed_type = var_Type(obj, OPEN3D_SUBTITLE_OFFSET_SIGNED_VAR);
    const int seq_type = var_Type(obj, OPEN3D_SUBTITLE_OFFSET_SEQ_VAR);
    const int frame_type = var_Type(obj, OPEN3D_SUBTITLE_OFFSET_FRAME_VAR);
    if (valid_type == 0 || signed_type == 0 || seq_type == 0)
        return false;

    if (state != NULL)
    {
        state->valid = var_GetBool(obj, OPEN3D_SUBTITLE_OFFSET_VALID_VAR);
        state->signed_offset = (int8_t)var_GetInteger(obj, OPEN3D_SUBTITLE_OFFSET_SIGNED_VAR);
        state->raw = (uint8_t)var_GetInteger(obj, OPEN3D_SUBTITLE_OFFSET_RAW_VAR);
        state->sequence = (uint8_t)(state->valid ? var_GetInteger(obj, OPEN3D_SUBTITLE_OFFSET_SEQ_VAR) : 0xff);
        state->frame_index = frame_type != 0 ? (int)var_GetInteger(obj, OPEN3D_SUBTITLE_OFFSET_FRAME_VAR) : -1;
    }

    return true;
}

static inline bool Open3DSubtitleDepthReadFromObjectTree(vlc_object_t *obj,
                                                         open3d_subtitle_depth_state_t *state)
{
    if (state != NULL)
        Open3DSubtitleDepthStateClear(state);

    for (vlc_object_t *current = obj; current != NULL; current = current->obj.parent)
    {
        if (Open3DSubtitleDepthReadFromObject(current, state))
            return true;
    }

    return false;
}

static inline void Open3DMkvSubtitleBridgePublishToObject(vlc_object_t *obj,
                                                          bool available,
                                                          bool force,
                                                          int static_units,
                                                          int plane,
                                                          int source_id)
{
    if (obj == NULL)
        return;

    if (!available)
    {
        force = false;
        static_units = 0;
        plane = -1;
        source_id = -1;
    }

    var_Create(obj, OPEN3D_MKV_SUBTITLE_BRIDGE_VALID_VAR, VLC_VAR_BOOL);
    var_Create(obj, OPEN3D_MKV_SUBTITLE_FORCE_VAR, VLC_VAR_BOOL);
    var_Create(obj, OPEN3D_MKV_SUBTITLE_STATIC_UNITS_VAR, VLC_VAR_INTEGER);
    var_Create(obj, OPEN3D_MKV_SUBTITLE_PLANE_VAR, VLC_VAR_INTEGER);
    var_Create(obj, OPEN3D_MKV_SUBTITLE_SOURCE_ID_VAR, VLC_VAR_INTEGER);

    var_SetBool(obj, OPEN3D_MKV_SUBTITLE_BRIDGE_VALID_VAR, available);
    var_SetBool(obj, OPEN3D_MKV_SUBTITLE_FORCE_VAR, force);
    var_SetInteger(obj, OPEN3D_MKV_SUBTITLE_STATIC_UNITS_VAR, static_units);
    var_SetInteger(obj, OPEN3D_MKV_SUBTITLE_PLANE_VAR, plane);
    var_SetInteger(obj, OPEN3D_MKV_SUBTITLE_SOURCE_ID_VAR, source_id);
}

static inline bool Open3DMkvSubtitleBridgeReadFromObject(vlc_object_t *obj,
                                                         bool *available_out,
                                                         bool *force_out,
                                                         int *static_units_out,
                                                         int *plane_out,
                                                         int *source_id_out)
{
    if (available_out != NULL)
        *available_out = false;
    if (force_out != NULL)
        *force_out = false;
    if (static_units_out != NULL)
        *static_units_out = 0;
    if (plane_out != NULL)
        *plane_out = -1;
    if (source_id_out != NULL)
        *source_id_out = -1;

    if (obj == NULL || var_Type(obj, OPEN3D_MKV_SUBTITLE_BRIDGE_VALID_VAR) == 0)
        return false;

    const bool available = var_GetBool(obj, OPEN3D_MKV_SUBTITLE_BRIDGE_VALID_VAR);
    if (available_out != NULL)
        *available_out = available;
    if (!available)
        return true;

    if (force_out != NULL)
        *force_out = var_GetBool(obj, OPEN3D_MKV_SUBTITLE_FORCE_VAR);
    if (static_units_out != NULL)
        *static_units_out = (int)var_GetInteger(obj, OPEN3D_MKV_SUBTITLE_STATIC_UNITS_VAR);
    if (plane_out != NULL)
        *plane_out = (int)var_GetInteger(obj, OPEN3D_MKV_SUBTITLE_PLANE_VAR);
    if (source_id_out != NULL)
        *source_id_out = (int)var_GetInteger(obj, OPEN3D_MKV_SUBTITLE_SOURCE_ID_VAR);
    return true;
}

#endif
