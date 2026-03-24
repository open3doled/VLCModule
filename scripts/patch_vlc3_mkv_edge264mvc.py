#!/usr/bin/env python3
from pathlib import Path
import sys


CODEC_DEFINE = "#define OPEN3DMKV_CODEC_H264 VLC_FOURCC('m','k','v','h')\n"
ALIAS_BLOCK = """#if defined(OPEN3D_VLC_ABI_ALIAS_T64)
/*
 * Host distro VLC 3.0.x builds may look for the Debian/Ubuntu t64 module
 * entry symbol even when building against the upstream 3.0.23 source tree.
 * Export both entry names so the staged mkv plugin can be loaded by the host
 * launcher without affecting the AppImage runtime.
 */
#ifdef __cplusplus
extern "C" {
#endif
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
#ifdef __cplusplus
}
#endif
#endif
"""

OLD_AVC_CASE = """        S_CASE("V_MPEG4/ISO/AVC") {\n            vars.p_fmt->i_codec = VLC_FOURCC( 'a','v','c','1' );\n            fill_extra_data( vars.p_tk, 0 );\n        }\n"""
NEW_AVC_CASE = """        S_CASE("V_MPEG4/ISO/AVC") {\n            vars.p_fmt->i_codec = OPEN3DMKV_CODEC_H264;\n            fill_extra_data( vars.p_tk, 0 );\n        }\n"""
OLD_CAPABILITY = '    set_capability( "demux", 50 )\n'
NEW_CAPABILITY = '    set_capability( "demux", 60 )\n'

OLD_TRACK_NUMBER_CASE = """        E_CASE( KaxTrackNumber, tnum )\n        {\n            vars.tk->i_number = static_cast<uint32>( tnum );\n            debug( vars, "Track Number=%u", vars.tk->i_number );\n        }\n"""
NEW_TRACK_NUMBER_CASE = """        E_CASE( KaxTrackNumber, tnum )\n        {\n            vars.tk->i_number = static_cast<uint32>( tnum );\n            vars.tk->fmt.i_id = static_cast<int>( vars.tk->i_number );\n            debug( vars, "Track Number=%u", vars.tk->i_number );\n        }\n"""
OLD_TRACK_UID_CASE = """        E_CASE( KaxTrackUID, tuid )\n        {\n            debug( vars, "Track UID=%x", static_cast<uint32>( tuid ) );\n        }\n"""
NEW_TRACK_UID_CASE = """        E_CASE( KaxTrackUID, tuid )\n        {\n            vars.tk->i_uid = static_cast<uint64>( tuid );\n            debug( vars, "Track UID=%" PRIu64, vars.tk->i_uid );\n        }\n"""

MKV_HPP_FIELDS_OLD = """        bool         b_default;\n        bool         b_enabled;\n        bool         b_forced;\n        track_id_t   i_number;\n\n        unsigned int i_extra_data;\n"""
MKV_HPP_FIELDS_NEW = """        bool         b_default;\n        bool         b_enabled;\n        bool         b_forced;\n        track_id_t   i_number;\n        uint64_t     i_uid;\n\n        bool         b_open3d_mkv_subtitle_metadata;\n        int          i_open3d_mkv_subtitle_plane;\n        int          i_open3d_mkv_subtitle_source_id;\n        int          i_open3d_mkv_subtitle_static_offset_units;\n\n        unsigned int i_extra_data;\n"""

MKT_TRACK_CTOR_OLD = """    b_default(true)\n  ,b_enabled(true)\n  ,b_forced(false)\n  ,i_number(0)\n  ,i_extra_data(0)\n"""
MKT_TRACK_CTOR_NEW = """    b_default(true)\n  ,b_enabled(true)\n  ,b_forced(false)\n  ,i_number(0)\n  ,i_uid(0)\n  ,b_open3d_mkv_subtitle_metadata(false)\n  ,i_open3d_mkv_subtitle_plane(-1)\n  ,i_open3d_mkv_subtitle_source_id(-1)\n  ,i_open3d_mkv_subtitle_static_offset_units(0)\n  ,i_extra_data(0)\n"""

MKV_CPP_INCLUDE_ANCHOR = "#include <vlc_url.h>\n"
MKV_CPP_EXTRA_INCLUDES = "#include <vlc_variables.h>\n#include \"../../video_output/open3d_subtitle_bridge.h\"\n"

MKV_CPP_HELPERS = """
static void Open3DMkvSubtitlePublishSelection(demux_sys_t *p_sys,
                                              bool available,
                                              bool force,
                                              int static_units,
                                              int plane,
                                              int source_id)
{
    if (p_sys == NULL || p_sys->demuxer.obj.libvlc == NULL)
        return;

    Open3DMkvSubtitleBridgePublishToObject(
        VLC_OBJECT(p_sys->demuxer.obj.libvlc),
        available, force, static_units, plane, source_id);
}

static bool Open3DMkvSubtitleResolveTrackByEsId(const demux_sys_t *p_sys,
                                                int i_spu_es_id,
                                                int *plane_out,
                                                int *source_id_out,
                                                int *static_units_out)
{
    if (plane_out != NULL)
        *plane_out = -1;
    if (source_id_out != NULL)
        *source_id_out = -1;
    if (static_units_out != NULL)
        *static_units_out = 0;

    if (p_sys == NULL || i_spu_es_id < 0)
        return false;

    for (size_t i_stream = 0; i_stream < p_sys->streams.size(); ++i_stream)
    {
        const matroska_stream_c *stream = p_sys->streams[i_stream];
        if (stream == NULL)
            continue;

        for (size_t i_segment = 0; i_segment < stream->segments.size(); ++i_segment)
        {
            const matroska_segment_c *segment = stream->segments[i_segment];
            if (segment == NULL)
                continue;

            for (matroska_segment_c::tracks_map_t::const_iterator it = segment->tracks.begin();
                 it != segment->tracks.end(); ++it)
            {
                const mkv_track_t &track = *it->second;
                if (track.fmt.i_cat != SPU_ES || track.fmt.i_id != i_spu_es_id)
                    continue;

                if (!track.b_open3d_mkv_subtitle_metadata)
                    return false;

                if (plane_out != NULL)
                    *plane_out = track.i_open3d_mkv_subtitle_plane;
                if (source_id_out != NULL)
                    *source_id_out = track.i_open3d_mkv_subtitle_source_id;
                if (static_units_out != NULL)
                    *static_units_out = track.i_open3d_mkv_subtitle_static_offset_units;
                return true;
            }
        }
    }

    return false;
}

static bool Open3DMkvSubtitleResolveTrackByStartupPrefs(
    const demux_sys_t *p_sys,
    int *plane_out,
    int *source_id_out,
    int *static_units_out,
    int *track_id_out,
    int *track_index_out)
{
    if (plane_out != NULL)
        *plane_out = -1;
    if (source_id_out != NULL)
        *source_id_out = -1;
    if (static_units_out != NULL)
        *static_units_out = 0;
    if (track_id_out != NULL)
        *track_id_out = -1;
    if (track_index_out != NULL)
        *track_index_out = -1;

    if (p_sys == NULL)
        return false;

    const int sub_track_id = var_InheritInteger(&p_sys->demuxer, "sub-track-id");
    if (sub_track_id >= 0)
    {
        int plane = -1;
        int source_id = -1;
        int static_units = 0;
        if (Open3DMkvSubtitleResolveTrackByEsId(p_sys, sub_track_id,
                                                &plane, &source_id, &static_units))
        {
            if (plane_out != NULL)
                *plane_out = plane;
            if (source_id_out != NULL)
                *source_id_out = source_id;
            if (static_units_out != NULL)
                *static_units_out = static_units;
            if (track_id_out != NULL)
                *track_id_out = sub_track_id;
            return true;
        }
    }

    const int sub_track = var_InheritInteger(&p_sys->demuxer, "sub-track");
    if (sub_track < 0)
        return false;

    int subtitle_index = 0;
    for (size_t i_stream = 0; i_stream < p_sys->streams.size(); ++i_stream)
    {
        const matroska_stream_c *stream = p_sys->streams[i_stream];
        if (stream == NULL)
            continue;

        for (size_t i_segment = 0; i_segment < stream->segments.size(); ++i_segment)
        {
            const matroska_segment_c *segment = stream->segments[i_segment];
            if (segment == NULL)
                continue;

            for (matroska_segment_c::tracks_map_t::const_iterator it = segment->tracks.begin();
                 it != segment->tracks.end(); ++it)
            {
                const mkv_track_t &track = *it->second;
                if (track.fmt.i_cat != SPU_ES)
                    continue;
                if (subtitle_index++ != sub_track)
                    continue;

                if (!track.b_open3d_mkv_subtitle_metadata)
                    return false;

                if (plane_out != NULL)
                    *plane_out = track.i_open3d_mkv_subtitle_plane;
                if (source_id_out != NULL)
                    *source_id_out = track.i_open3d_mkv_subtitle_source_id;
                if (static_units_out != NULL)
                    *static_units_out = track.i_open3d_mkv_subtitle_static_offset_units;
                if (track_id_out != NULL)
                    *track_id_out = track.fmt.i_id;
                if (track_index_out != NULL)
                    *track_index_out = sub_track;
                return true;
            }
        }
    }

    return false;
}

static void Open3DMkvSubtitleSyncSelection(demux_sys_t *p_sys)
{
    if (p_sys == NULL)
        return;

    int plane = -1;
    int source_id = -1;
    int static_units = 0;
    if (p_sys->p_input != NULL)
    {
        const int i_spu_es_id = var_GetInteger(p_sys->p_input, "spu-es");
        if (Open3DMkvSubtitleResolveTrackByEsId(p_sys, i_spu_es_id,
                                                &plane, &source_id, &static_units))
        {
            Open3DMkvSubtitlePublishSelection(p_sys, true, true,
                                             static_units, plane, source_id);
            msg_Dbg(&p_sys->demuxer,
                    "open3d mkv subtitle mapping synced: spu-es=%d plane=%d source_id=%d static_units=%d",
                    i_spu_es_id, plane, source_id, static_units);
            return;
        }
    }

    int startup_track_id = -1;
    int startup_track_index = -1;
    if (Open3DMkvSubtitleResolveTrackByStartupPrefs(
            p_sys, &plane, &source_id, &static_units,
            &startup_track_id, &startup_track_index))
    {
        Open3DMkvSubtitlePublishSelection(p_sys, true, true,
                                         static_units, plane, source_id);
        if (startup_track_id >= 0)
        {
            msg_Dbg(&p_sys->demuxer,
                    "open3d mkv startup subtitle preference: sub-track-id=%d plane=%d source_id=%d static_units=%d",
                    startup_track_id, plane, source_id, static_units);
        }
        else
        {
            msg_Dbg(&p_sys->demuxer,
                    "open3d mkv startup subtitle preference: sub-track=%d plane=%d source_id=%d static_units=%d",
                    startup_track_index, plane, source_id, static_units);
        }
        return;
    }

    Open3DMkvSubtitlePublishSelection(p_sys, true, false, 0, -1, -1);
    if (p_sys->p_input != NULL)
    {
        const int i_spu_es_id = var_GetInteger(p_sys->p_input, "spu-es");
        if (i_spu_es_id >= 0)
        {
            msg_Dbg(&p_sys->demuxer,
                    "open3d mkv subtitle mapping cleared for spu-es=%d",
                    i_spu_es_id);
        }
    }
}

static int Open3DMkvSubtitleTrackCallback(vlc_object_t *obj, char const *name,
                                          vlc_value_t oldval, vlc_value_t newval,
                                          void *opaque)
{
    VLC_UNUSED(obj);
    VLC_UNUSED(name);
    VLC_UNUSED(oldval);
    VLC_UNUSED(newval);

    Open3DMkvSubtitleSyncSelection(static_cast<demux_sys_t *>(opaque));
    return VLC_SUCCESS;
}

"""

MKV_OPEN_REGISTER_OLD = """    /* Set the demux function */\n    p_demux->pf_demux   = Demux;\n    p_demux->pf_control = Control;\n    p_demux->p_sys      = p_sys = new demux_sys_t( *p_demux );\n"""
MKV_OPEN_REGISTER_NEW = """    /* Set the demux function */\n    p_demux->pf_demux   = Demux;\n    p_demux->pf_control = Control;\n    p_demux->p_sys      = p_sys = new demux_sys_t( *p_demux );\n\n    p_sys->p_input = p_demux->p_input;\n    Open3DMkvSubtitlePublishSelection(p_sys, false, false, 0, -1, -1);\n    if (p_sys->p_input != NULL)\n        var_AddCallback(p_sys->p_input, \"spu-es\",\n                        Open3DMkvSubtitleTrackCallback, p_sys);\n"""
LEGACY_MKV_OPEN_REGISTER_BLOCK = """\n    p_sys->p_input = p_demux->p_input;\n    if (p_sys->p_input != NULL)\n    {\n        Open3DMkvSubtitleEnsureInputVars(p_sys->p_input);\n        Open3DMkvSubtitleSetInputVars(p_sys->p_input, false, 0, -1, -1);\n        var_AddCallback(p_sys->p_input, \"spu-es\",\n                        Open3DMkvSubtitleTrackCallback, p_sys);\n    }\n"""

MKV_OPEN_SYNC_ANCHOR = "    p_sys->InitUi();\n\n    return VLC_SUCCESS;\n"
MKV_OPEN_SYNC_INSERT = "    Open3DMkvSubtitleSyncSelection(p_sys);\n    p_sys->InitUi();\n\n    return VLC_SUCCESS;\n"

MKV_CLOSE_OLD = """static void Close( vlc_object_t *p_this )\n{\n    demux_t     *p_demux = reinterpret_cast<demux_t*>( p_this );\n    demux_sys_t *p_sys   = p_demux->p_sys;\n"""
MKV_CLOSE_NEW = """static void Close( vlc_object_t *p_this )\n{\n    demux_t     *p_demux = reinterpret_cast<demux_t*>( p_this );\n    demux_sys_t *p_sys   = p_demux->p_sys;\n    if (p_sys != NULL && p_sys->p_input != NULL)\n        var_DelCallback(p_sys->p_input, \"spu-es\",\n                        Open3DMkvSubtitleTrackCallback, p_sys);\n    Open3DMkvSubtitlePublishSelection(p_sys, false, false, 0, -1, -1);\n"""

MATROSKA_SEGMENT_CPP_INCLUDE_ANCHOR = "#include <limits>\n"
MATROSKA_SEGMENT_CPP_EXTRA_INCLUDES = [
    "#include <cctype>\n",
    "#include <cerrno>\n",
    "#include <cstdlib>\n",
]

MATROSKA_SEGMENT_CPP_HELPERS = """
static std::string Open3DMkvTagUpper(const std::string &input)
{
    std::string out = input;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    return out;
}

static bool Open3DMkvTagPlaneKey(const std::string &key)
{
    const std::string upper = Open3DMkvTagUpper(key);
    if (upper == "3D-PLANE")
        return true;
    return upper.size() > 9 && upper.compare(0, 9, "3D-PLANE-") == 0;
}

static bool Open3DMkvTagSourceIdKey(const std::string &key)
{
    const std::string upper = Open3DMkvTagUpper(key);
    if (upper == "SOURCE_ID")
        return true;
    return upper.size() > 10 && upper.compare(0, 10, "SOURCE_ID-") == 0;
}

static bool Open3DMkvParseInt(const std::string &value, int *out)
{
    if (out == NULL || value.empty())
        return false;

    char *end = NULL;
    errno = 0;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\\0' ||
        parsed < INT_MIN || parsed > INT_MAX)
        return false;

    *out = static_cast<int>(parsed);
    return true;
}

static void Open3DMkvCollectTrackMetadataFromSimpleTag(
    const SimpleTag &simple,
    bool *have_plane,
    int *plane,
    bool *have_source_id,
    int *source_id)
{
    if (Open3DMkvTagPlaneKey(simple.tag_name))
    {
        int parsed_plane = -1;
        if (Open3DMkvParseInt(simple.value, &parsed_plane))
        {
            if (plane != NULL)
                *plane = parsed_plane;
            if (have_plane != NULL)
                *have_plane = true;
        }
    }
    else if (Open3DMkvTagSourceIdKey(simple.tag_name))
    {
        int parsed_source_id = -1;
        if (Open3DMkvParseInt(simple.value, &parsed_source_id))
        {
            if (source_id != NULL)
                *source_id = parsed_source_id;
            if (have_source_id != NULL)
                *have_source_id = true;
        }
    }

    for (size_t i = 0; i < simple.sub_tags.size(); ++i)
    {
        Open3DMkvCollectTrackMetadataFromSimpleTag(simple.sub_tags[i],
                                                   have_plane, plane,
                                                   have_source_id, source_id);
    }
}

static mkv_track_t *Open3DMkvFindTrackByUid(matroska_segment_c::tracks_map_t &tracks,
                                            uint64_t uid)
{
    if (uid == 0)
        return NULL;

    for (matroska_segment_c::tracks_map_t::iterator it = tracks.begin();
         it != tracks.end(); ++it)
    {
        mkv_track_t *track = it->second.get();
        if (track != NULL && track->i_uid == uid)
            return track;
    }
    return NULL;
}

static void Open3DMkvApplyTrackSubtitleTag(matroska_segment_c &segment,
                                           const Tag &tag)
{
    if (tag.i_tag_type != TRACK_UID)
        return;

    mkv_track_t *track = Open3DMkvFindTrackByUid(segment.tracks, tag.i_uid);
    if (track == NULL || track->fmt.i_cat != SPU_ES)
        return;

    bool have_plane = false;
    bool have_source_id = false;
    int plane = -1;
    int source_id = -1;
    for (size_t i = 0; i < tag.simple_tags.size(); ++i)
    {
        Open3DMkvCollectTrackMetadataFromSimpleTag(tag.simple_tags[i],
                                                   &have_plane, &plane,
                                                   &have_source_id, &source_id);
    }
    if (!have_plane && !have_source_id)
        return;

    track->b_open3d_mkv_subtitle_metadata = true;
    track->i_open3d_mkv_subtitle_plane = have_plane ? plane : -1;
    track->i_open3d_mkv_subtitle_source_id = have_source_id ? source_id : -1;
    track->i_open3d_mkv_subtitle_static_offset_units = have_plane ? plane : 0;

    msg_Dbg(&segment.sys.demuxer,
            "open3d mkv subtitle metadata track=%u uid=%" PRIu64 " plane=%d source_id=%d static_units=%d",
            track->i_number, track->i_uid,
            track->i_open3d_mkv_subtitle_plane,
            track->i_open3d_mkv_subtitle_source_id,
            track->i_open3d_mkv_subtitle_static_offset_units);
}

static void Open3DMkvApplyTrackSubtitleTags(matroska_segment_c &segment)
{
    for (size_t i = 0; i < segment.tags.size(); ++i)
        Open3DMkvApplyTrackSubtitleTag(segment, segment.tags[i]);
}

"""

MATROSKA_SEGMENT_CPP_TAGS_DONE_OLD = '    msg_Dbg( &sys.demuxer, "loading tags done." );\n}\n'
MATROSKA_SEGMENT_CPP_TAGS_DONE_NEW = '    Open3DMkvApplyTrackSubtitleTags(*this);\n    msg_Dbg( &sys.demuxer, "loading tags done." );\n}\n'


def ensure_replace(text: str, old: str, new: str, label: str) -> str:
    if old in text:
        return text.replace(old, new, 1)
    if new in text:
        return text
    raise SystemExit(label)


def dedupe_exact_block(text: str, block: str) -> str:
    first = text.find(block)
    if first < 0:
        return text
    head = text[:first + len(block)]
    tail = text[first + len(block):].replace(block, "")
    return head + tail


def patch_matroska_segment_parse(parse_path: Path) -> None:
    text = parse_path.read_text()

    if CODEC_DEFINE not in text:
        anchor = "#include <vlc_codecs.h>\n"
        if anchor not in text:
            raise SystemExit(f"{parse_path}: codec define anchor not found")
        text = text.replace(anchor, anchor + CODEC_DEFINE, 1)

    text = ensure_replace(
        text, OLD_AVC_CASE, NEW_AVC_CASE,
        f"{parse_path}: AVC case not found",
    )
    text = ensure_replace(
        text, OLD_TRACK_NUMBER_CASE, NEW_TRACK_NUMBER_CASE,
        f"{parse_path}: track number case not found",
    )
    text = ensure_replace(
        text, OLD_TRACK_UID_CASE, NEW_TRACK_UID_CASE,
        f"{parse_path}: track UID case not found",
    )

    parse_path.write_text(text)


def patch_mkv_hpp(path: Path) -> None:
    text = path.read_text()
    text = ensure_replace(
        text, MKV_HPP_FIELDS_OLD, MKV_HPP_FIELDS_NEW,
        f"{path}: mkv_track_t field anchor not found",
    )
    path.write_text(text)


def patch_mkv_cpp(path: Path) -> None:
    text = path.read_text()
    text = ensure_replace(
        text, OLD_CAPABILITY, NEW_CAPABILITY,
        f"{path}: mkv demux capability not found",
    )
    if MKV_CPP_EXTRA_INCLUDES not in text:
        if MKV_CPP_INCLUDE_ANCHOR not in text:
            raise SystemExit(f"{path}: mkv.cpp include anchor not found")
        text = text.replace(MKV_CPP_INCLUDE_ANCHOR,
                            MKV_CPP_INCLUDE_ANCHOR + MKV_CPP_EXTRA_INCLUDES, 1)
    anchor = "/*****************************************************************************\n * Module descriptor\n *****************************************************************************/\n"
    if anchor not in text:
        raise SystemExit(f"{path}: mkv helper anchor not found")
    for marker in (
        "static void Open3DMkvSubtitleEnsureInputVars(",
        "static void Open3DMkvSubtitlePublishSelection(",
    ):
        if marker in text:
            start = text.index(marker)
            end = text.index(anchor)
            text = text[:start] + text[end:]
            break
    text = text.replace(anchor, MKV_CPP_HELPERS + anchor, 1)
    if "vlc_entry__3_0_0ft64" not in text:
        if MKV_CPP_INCLUDE_ANCHOR not in text:
            raise SystemExit(f"{path}: mkv alias anchor not found")
        text = text.replace(MKV_CPP_INCLUDE_ANCHOR,
                            MKV_CPP_INCLUDE_ANCHOR + "\n" + ALIAS_BLOCK + "\n", 1)

    text = ensure_replace(
        text, MKV_OPEN_REGISTER_OLD, MKV_OPEN_REGISTER_NEW,
        f"{path}: mkv Open registration block not found",
    )
    text = ensure_replace(
        text, MKV_OPEN_SYNC_ANCHOR, MKV_OPEN_SYNC_INSERT,
        f"{path}: mkv Open sync anchor not found",
    )
    text = ensure_replace(
        text, MKV_CLOSE_OLD, MKV_CLOSE_NEW,
        f"{path}: mkv Close block not found",
    )
    text = ensure_replace(
        text, MKT_TRACK_CTOR_OLD, MKT_TRACK_CTOR_NEW,
        f"{path}: mkv_track_t constructor block not found",
    )
    while LEGACY_MKV_OPEN_REGISTER_BLOCK in text:
        text = text.replace(LEGACY_MKV_OPEN_REGISTER_BLOCK, "")
    text = text.replace("    Open3DMkvSubtitleSyncInputVars(p_sys);\n", "")
    text = dedupe_exact_block(text, MKV_OPEN_REGISTER_NEW)
    text = dedupe_exact_block(text, "    Open3DMkvSubtitleSyncSelection(p_sys);\n")
    text = dedupe_exact_block(text, "    if (p_sys != NULL && p_sys->p_input != NULL)\n        var_DelCallback(p_sys->p_input, \"spu-es\",\n                        Open3DMkvSubtitleTrackCallback, p_sys);\n")

    path.write_text(text)


def patch_matroska_segment_cpp(path: Path) -> None:
    text = path.read_text()
    if MATROSKA_SEGMENT_CPP_INCLUDE_ANCHOR not in text:
        raise SystemExit(f"{path}: matroska_segment.cpp include anchor not found")
    for include in MATROSKA_SEGMENT_CPP_EXTRA_INCLUDES:
        if include not in text:
            text = text.replace(
                MATROSKA_SEGMENT_CPP_INCLUDE_ANCHOR,
                MATROSKA_SEGMENT_CPP_INCLUDE_ANCHOR + include,
                1,
            )
    if "Open3DMkvApplyTrackSubtitleTags" not in text:
        anchor = "\nmatroska_segment_c::matroska_segment_c("
        if anchor not in text:
            raise SystemExit(f"{path}: matroska_segment helper anchor not found")
        text = text.replace(anchor, "\n" + MATROSKA_SEGMENT_CPP_HELPERS + anchor, 1)
    text = ensure_replace(
        text, MATROSKA_SEGMENT_CPP_TAGS_DONE_OLD, MATROSKA_SEGMENT_CPP_TAGS_DONE_NEW,
        f"{path}: tag completion block not found",
    )
    path.write_text(text)


def patch_tree(tree_root: Path) -> None:
    patch_matroska_segment_parse(
        tree_root / "modules/demux/mkv/matroska_segment_parse.cpp"
    )
    patch_mkv_hpp(tree_root / "modules/demux/mkv/mkv.hpp")
    patch_matroska_segment_cpp(tree_root / "modules/demux/mkv/matroska_segment.cpp")
    patch_mkv_cpp(tree_root / "modules/demux/mkv/mkv.cpp")


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: patch_vlc3_mkv_edge264mvc.py /path/to/vlc-source", file=sys.stderr)
        return 2

    patch_tree(Path(sys.argv[1]).resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
