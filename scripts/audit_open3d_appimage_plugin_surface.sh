#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: audit_open3d_appimage_plugin_surface.sh <AppDir-or-AppImage-extract-root>

Audits an assembled Open3DOLED AppDir/AppImage extract for the first known
playback-critical VLC plugin surface needed for desktop playback parity.

The audit currently checks for:
  - access/libfilesystem_plugin.so
  - demux/libmkv_plugin.so
  - codec/libavcodec_plugin.so
  - packetizer/libpacketizer_dts_plugin.so
  - packetizer/libpacketizer_copy_plugin.so
  - audio_output/libpulse_plugin.so
  - audio_mixer/libfloat_mixer_plugin.so
  - audio_filter/libaudio_format_plugin.so
  - audio_filter/libscaletempo_plugin.so
  - audio_filter/libsamplerate_plugin.so
  - text_renderer/libfreetype_plugin.so
  - video_chroma/libswscale_plugin.so
  - video_chroma/libyuvp_plugin.so
  - video_output/libgl_plugin.so
  - video_output/libegl_x11_plugin.so
  - misc/liblogger_plugin.so
  - logger/libconsole_logger_plugin.so
  - logger/libfile_logger_plugin.so

When `OPEN3D_APPIMAGE_EXTENDED_MODULE_SET=1` (the default), it also checks:
  - control/libdbus_plugin.so
  - packetizer/libpacketizer_a52_plugin.so
  - control/liboldrc_plugin.so
  - demux/libdirectory_demux_plugin.so
  - audio_filter/libequalizer_plugin.so
  - video_filter/libdeinterlace_plugin.so
  - spu/libsubsdelay_plugin.so
  - lua/liblua_plugin.so
  - misc/libexport_plugin.so
  - misc/libxml_plugin.so
  - audio_output/libalsa_plugin.so
  - access/libhttp_plugin.so
  - access/libhttps_plugin.so
  - access/libftp_plugin.so
  - access/libtcp_plugin.so
  - access/libudp_plugin.so
  - access/librtp_plugin.so
  - access/libsdp_plugin.so
  - demux/libmp4_plugin.so
  - demux/libavi_plugin.so
  - demux/libogg_plugin.so
  - demux/libwav_plugin.so
  - demux/libsubtitle_plugin.so
  - codec/libsubsdec_plugin.so
  - codec/libdvbsub_plugin.so
  - codec/libspudec_plugin.so
  - codec/libwebvtt_plugin.so
  - video_output/libegl_wl_plugin.so
  - video_output/libwl_shm_plugin.so
  - video_output/libxdg_shell_plugin.so
  - video_output/libxcb_window_plugin.so
  - video_output/libglx_plugin.so

Exit status:
  0 if all required plugins are present
  1 if one or more required plugins are missing
  2 for usage or path errors
EOF
}

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 2
fi

ROOT_PATH="$1"
if [[ ! -d "${ROOT_PATH}" ]]; then
  echo "Error: root path not found: ${ROOT_PATH}" >&2
  exit 2
fi

PLUGIN_ROOT="${ROOT_PATH}/usr/lib/vlc/plugins"
if [[ ! -d "${PLUGIN_ROOT}" ]]; then
  echo "Error: plugin root not found under ${ROOT_PATH}: ${PLUGIN_ROOT}" >&2
  exit 2
fi

required_plugins=(
  "access/libfilesystem_plugin.so|filesystem access for local media"
  "demux/libmkv_plugin.so|Matroska demux for MVC MKV playback"
  "codec/libavcodec_plugin.so|FFmpeg-backed audio/SPU decode and fallback decode paths"
  "packetizer/libpacketizer_dts_plugin.so|DTS packetizer needed for current Blu-ray audio path"
  "packetizer/libpacketizer_copy_plugin.so|copy packetizer needed for current Blu-ray subtitle/SPU path"
  "audio_output/libpulse_plugin.so|desktop audio output on the current host baseline"
  "audio_mixer/libfloat_mixer_plugin.so|audio volume/mixer module used by the host baseline"
  "audio_filter/libaudio_format_plugin.so|audio converter module used for s32l->f32l conversion"
  "audio_filter/libscaletempo_plugin.so|audio filter module used by the maintained playback defaults"
  "audio_filter/libsamplerate_plugin.so|audio resampler module used by the host baseline"
  "text_renderer/libfreetype_plugin.so|subtitle/text renderer support"
  "video_chroma/libswscale_plugin.so|video converter module used by the host subtitle-composition baseline"
  "video_chroma/libyuvp_plugin.so|YUVA subtitle chroma-converter module used by the host baseline"
  "video_output/libgl_plugin.so|core OpenGL video-output helper path"
  "video_output/libegl_x11_plugin.so|X11 EGL helper for desktop GL/vout startup"
  "misc/liblogger_plugin.so|core VLC logging frontend"
  "logger/libconsole_logger_plugin.so|console logger backend for stderr/stdout diagnostics"
  "logger/libfile_logger_plugin.so|file logger backend for packaged-runtime diagnostics"
)

if [[ "${OPEN3D_APPIMAGE_EXTENDED_MODULE_SET:-1}" == "1" ]]; then
  required_plugins+=(
    "control/libdbus_plugin.so|DBus control interface for desktop VLC behavior"
    "packetizer/libpacketizer_a52_plugin.so|A52 packetizer surface for alternate Blu-ray audio tracks"
    "control/liboldrc_plugin.so|legacy RC control interface for richer VLC parity"
    "demux/libdirectory_demux_plugin.so|directory and media-library demux support"
    "audio_filter/libequalizer_plugin.so|equalizer option surface referenced by desktop state"
    "video_filter/libdeinterlace_plugin.so|deinterlace option surface referenced by desktop state"
    "spu/libsubsdelay_plugin.so|subtitle delay/fps option surface referenced by desktop state"
    "lua/liblua_plugin.so|extensions and Lua-backed desktop VLC features"
    "misc/libexport_plugin.so|playlist and media-library export support"
    "misc/libxml_plugin.so|XML reader support for playlist and state loading"
    "audio_output/libalsa_plugin.so|desktop audio fallback when Pulse is unavailable"
    "access/libhttp_plugin.so|common HTTP network access surface"
    "access/libhttps_plugin.so|common HTTPS network access surface"
    "access/libftp_plugin.so|common FTP network access surface"
    "access/libtcp_plugin.so|common TCP network access surface"
    "access/libudp_plugin.so|common UDP network access surface"
    "access/librtp_plugin.so|common RTP network access surface"
    "access/libsdp_plugin.so|common SDP network access surface"
    "demux/libmp4_plugin.so|common MP4 container support"
    "demux/libavi_plugin.so|common AVI container support"
    "demux/libogg_plugin.so|common Ogg container support"
    "demux/libwav_plugin.so|common WAV container support"
    "demux/libsubtitle_plugin.so|subtitle file demux surface"
    "codec/libsubsdec_plugin.so|text subtitle decoder surface"
    "codec/libdvbsub_plugin.so|DVB subtitle decoder surface"
    "codec/libspudec_plugin.so|DVD/Blu-ray bitmap subtitle decoder surface"
    "codec/libwebvtt_plugin.so|WebVTT subtitle decoder surface"
    "video_output/libegl_wl_plugin.so|Wayland EGL helper for later Wayland parity"
    "video_output/libwl_shm_plugin.so|Wayland SHM helper for later Wayland parity"
    "video_output/libxdg_shell_plugin.so|Wayland shell helper for later Wayland parity"
    "video_output/libxcb_window_plugin.so|desktop XCB window helper surface"
    "video_output/libglx_plugin.so|desktop GLX helper surface"
  )
fi

missing_count=0
found_count=0

printf '%s\n' "Open3DOLED AppImage plugin surface audit"
printf '%s\n' "root=${ROOT_PATH}"
printf '%s\n' "plugin_root=${PLUGIN_ROOT}"

for entry in "${required_plugins[@]}"; do
  rel_path="${entry%%|*}"
  description="${entry#*|}"
  abs_path="${PLUGIN_ROOT}/${rel_path}"
  if [[ -f "${abs_path}" ]]; then
    printf 'FOUND   %s  (%s)\n' "${rel_path}" "${description}"
    found_count=$((found_count + 1))
  else
    printf 'MISSING %s  (%s)\n' "${rel_path}" "${description}"
    missing_count=$((missing_count + 1))
  fi
done

printf '%s\n' "found=${found_count}"
printf '%s\n' "missing=${missing_count}"

if (( missing_count > 0 )); then
  exit 1
fi
