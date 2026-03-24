/**
 * @file open3d_display.c
 * @brief Open3DOLED page-flipping video output module for VLC 3.0.23
 */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <ctype.h>
#include <assert.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <glob.h>
#include <alloca.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#ifdef __linux__
# include <pthread.h>
# include <sched.h>
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_subpicture.h>
#include <vlc_vout_display.h>
#include <vlc_opengl.h>
#include <vlc_actions.h>

#include "opengl/vout_helper.h"
#include "open3d_font8x16_basic.h"
#include "open3d_subtitle_bridge.h"

typedef enum
{
    OPEN3D_HOTKEY_PROFILE_SAFE = 0,
    OPEN3D_HOTKEY_PROFILE_LEGACY,
    OPEN3D_HOTKEY_PROFILE_CUSTOM,
} open3d_hotkey_profile_t;

/* Plugin callbacks */
static int Open(vlc_object_t *);
static void Close(vlc_object_t *);

/* Display callbacks */
static picture_pool_t *Pool(vout_display_t *, unsigned);
static void PictureRender(vout_display_t *, picture_t *, subpicture_t *);
static void PictureDisplay(vout_display_t *, picture_t *, subpicture_t *);
static int Control(vout_display_t *, int, va_list);
static int Open3DEmitterVarCallback(vlc_object_t *, char const *,
                                    vlc_value_t, vlc_value_t, void *);
static int Open3DConfigVarCallback(vlc_object_t *, char const *,
                                   vlc_value_t, vlc_value_t, void *);
static int Open3DHotkeyVarCallback(vlc_object_t *, char const *,
                                   vlc_value_t, vlc_value_t, void *);
static void Open3DRegionChainAppend(subpicture_region_t **, subpicture_region_t *);
static int Open3DCopyRegionChain(subpicture_region_t **, const subpicture_region_t *);
static int Open3DCloneRegionChainShallow(subpicture_region_t **, const subpicture_region_t *);
static subpicture_region_t *Open3DCreateBitmapTextRegion(const char *,
                                                         int, int,
                                                         int, int,
                                                         int,
                                                         uint32_t,
                                                         uint8_t,
                                                         double,
                                                         bool);
static void Open3DEmitterClose(vout_display_sys_t *);
static bool Open3DEmitterQueueCommand(vout_display_t *, vout_display_sys_t *,
                                      const char *, bool, vlc_tick_t, const char *);
static void Open3DEmitterQueueApplySettings(vout_display_t *, vout_display_sys_t *);
static void Open3DEmitterMaybeReconnect(vout_display_t *, vout_display_sys_t *, vlc_tick_t);
static void Open3DEmitterUpdateDirtyState(vout_display_t *, vout_display_sys_t *, const char *);
static void Open3DEmitterWake(vout_display_sys_t *);
static void Open3DEmitterQueueEye(vout_display_sys_t *, bool, vlc_tick_t);
static void Open3DEmitterRequestEyeReset(vout_display_sys_t *);
static void *Open3DEmitterThread(void *);
static open3d_hotkey_profile_t Open3DParseHotkeysProfile(const char *);
static void Open3DPublishHotkeyDefaults(vout_display_t *, open3d_hotkey_profile_t);
static void Open3DReloadHotkeys(vout_display_t *, vout_display_sys_t *);
static void Open3DControlWake(vout_display_sys_t *);
static void *Open3DControlThread(void *);
static void *Open3DPresenterThread(void *);
static void Open3DPresenterWake(vout_display_sys_t *);
static void Open3DStatusSetMessage(vout_display_t *, vout_display_sys_t *, vlc_tick_t,
                                   const char *, ...);

#define OPEN3D_GL_TEXT N_("OpenGL extension")
#define OPEN3D_PROVIDER_LONGTEXT N_( \
    "Extension through which to use the Open Graphics Library (OpenGL).")

#define OPEN3D_ENABLE_TEXT N_("Enable Open3D page-flipping")
#define OPEN3D_ENABLE_LONGTEXT N_( \
    "Enable eye-alternating rendering for packed stereo input.")

#define OPEN3D_LAYOUT_TEXT N_("Stereo layout")
#define OPEN3D_LAYOUT_LONGTEXT N_( \
    "Packed layout selection. Use auto/sbs/tb for inferred full-or-half, or select explicit sbs-full/sbs-half/tb-full/tb-half.")

#define OPEN3D_HALF_LAYOUT_TEXT N_("Default half-layout in auto mode")
#define OPEN3D_HALF_LAYOUT_LONGTEXT N_( \
    "When auto detection is ambiguous (common 16:9), choose whether half-packed content defaults to side-by-side or top-bottom.")

#define OPEN3D_DRIVE_MODE_TEXT N_("Trigger drive mode")
#define OPEN3D_DRIVE_MODE_LONGTEXT N_( \
    "Top-level drive mode selector. Optical mode draws trigger boxes for emitter sensing; serial mode drives eyes via serial commands.")

#define OPEN3D_FLIP_EYES_TEXT N_("Swap eyes")
#define OPEN3D_FLIP_EYES_LONGTEXT N_( \
    "Swap left and right eye output.")

#define OPEN3D_TARGET_FLIP_HZ_TEXT N_("Target flip rate (Hz)")
#define OPEN3D_TARGET_FLIP_HZ_LONGTEXT N_( \
    "Requested eye toggle cadence. Set to 0 to toggle once per displayed frame.")

#define OPEN3D_PRESENTER_HZ_TEXT N_("Presenter cadence (Hz)")
#define OPEN3D_PRESENTER_HZ_LONGTEXT N_( \
    "Redisplay cadence for the presenter thread. Set to your display refresh rate (for example 120).")

#define OPEN3D_GPU_OVERLAY_TEXT N_("Use GPU rectangle overlay path")
#define OPEN3D_GPU_OVERLAY_LONGTEXT N_( \
    "Render trigger/calibration rectangles directly with OpenGL, bypassing VLC subpicture uploads for those overlays.")

#define OPEN3D_DEBUG_STATUS_TEXT N_("Debug status logs")
#define OPEN3D_DEBUG_STATUS_LONGTEXT N_( \
    "Log periodic Open3D eye/layout/scheduler status to the VLC debug log.")

#define OPEN3D_PRESENTER_TELEMETRY_TEXT N_("Presenter telemetry")
#define OPEN3D_PRESENTER_TELEMETRY_LONGTEXT N_( \
    "Log low-overhead periodic presenter telemetry and exact miss/over clocks without enabling the heavier per-frame debug-status path.")

#define OPEN3D_PRESENTER_TELEMETRY_INTERVAL_TEXT N_("Presenter telemetry interval (frames)")
#define OPEN3D_PRESENTER_TELEMETRY_INTERVAL_LONGTEXT N_( \
    "How many presenter frames to skip between periodic telemetry lines when presenter telemetry is enabled.")

#define OPEN3D_STAGE_PROFILE_TEXT N_("Presenter stage profiling")
#define OPEN3D_STAGE_PROFILE_LONGTEXT N_( \
    "Measure presenter-thread stage wall time, thread CPU time, and scheduler delay for Prepare/Display/Swap. Disabled by default and intended only for investigation.")

#define OPEN3D_PRESENTER_RT_ENABLE_TEXT N_("Presenter realtime scheduling")
#define OPEN3D_PRESENTER_RT_ENABLE_LONGTEXT N_( \
    "Attempt to raise the presenter thread to SCHED_FIFO on Linux. Enabled by default, but may fall back automatically if the host does not permit realtime scheduling.")

#define OPEN3D_PRESENTER_RT_PRIORITY_TEXT N_("Presenter realtime priority")
#define OPEN3D_PRESENTER_RT_PRIORITY_LONGTEXT N_( \
    "Requested SCHED_FIFO priority for the presenter thread on Linux. Set to -1 to use max-1, which preserves the previous default behavior.")

#define OPEN3D_PRESENTER_AFFINITY_ENABLE_TEXT N_("Presenter CPU pinning")
#define OPEN3D_PRESENTER_AFFINITY_ENABLE_LONGTEXT N_( \
    "Attempt to pin the presenter thread to a single allowed CPU on Linux. Enabled by default, but may fall back automatically if affinity changes are unavailable.")

#define OPEN3D_PRESENTER_AFFINITY_CPU_TEXT N_("Presenter pinned CPU")
#define OPEN3D_PRESENTER_AFFINITY_CPU_LONGTEXT N_( \
    "Requested CPU for presenter-thread pinning on Linux. Set to -1 to auto-select the current CPU or the first allowed CPU.")

#define OPEN3D_PRESENTER_MLOCKALL_TEXT N_("Presenter memory lock")
#define OPEN3D_PRESENTER_MLOCKALL_LONGTEXT N_( \
    "Attempt to lock current and future process mappings with mlockall() on Linux to reduce page-fault jitter. Disabled by default because it may fail without elevated limits.")

#define OPEN3D_PRESENTER_PREFAULT_KB_TEXT N_("Presenter prefault buffer (KiB)")
#define OPEN3D_PRESENTER_PREFAULT_KB_LONGTEXT N_( \
    "Touched heap+stack warmup size for the presenter thread on Linux when memory lock is enabled. Set to 0 to disable prefault warmup.")

#define OPEN3D_PRESENTER_LEAD_US_TEXT N_("Presenter render lead (us)")
#define OPEN3D_PRESENTER_LEAD_US_LONGTEXT N_( \
    "Start presenter Prepare/Display/Swap this many microseconds before the nominal present tick so swap and scheduler jitter are absorbed before the deadline. Set to 0 to disable this mitigation.")

#define OPEN3D_SUBTITLE_DEPTH_ENABLE_TEXT N_("Enable 3D subtitle placement")
#define OPEN3D_SUBTITLE_DEPTH_ENABLE_LONGTEXT N_( \
    "Apply eye-specific horizontal subtitle placement when Blu-ray subtitle depth metadata or MKV static subtitle fallback metadata is available.")

#define OPEN3D_SUBTITLE_DEPTH_UNIT_TEXT N_("3D subtitle unit shift (px)")
#define OPEN3D_SUBTITLE_DEPTH_UNIT_LONGTEXT N_( \
    "Horizontal pixel shift per subtitle depth unit at 1920 eye width. Negative values invert the disparity direction for both Blu-ray depth metadata and MKV static subtitle fallback.")

#define OPEN3D_MKV_SUBTITLE_STATIC_OFFSET_TEXT N_("MKV static subtitle offset units")
#define OPEN3D_MKV_SUBTITLE_STATIC_OFFSET_LONGTEXT N_( \
    "Fallback static subtitle offset units for MKV subtitle tracks when only per-track plane metadata is available. Used only when Blu-ray subtitle offset metadata is absent. Set to 0 to disable this MKV fallback.")
#define OPEN3D_MKV_SUBTITLE_FORCE_TEXT N_("Force MKV bitmap subtitle ownership")
#define OPEN3D_MKV_SUBTITLE_FORCE_LONGTEXT N_( \
    "Treat incoming bitmap subtitle subpictures as Open3D subtitle content for MKV playback when the selected MKV subtitle track has known stereo metadata. This keeps PF/SBS subtitle ownership even if VLC core does not mark the incoming composed subpicture with b_subtitle.")
#define OPEN3D_MKV_SUBTITLE_PLANE_TEXT N_("MKV subtitle plane id")
#define OPEN3D_MKV_SUBTITLE_PLANE_LONGTEXT N_( \
    "Selected MKV subtitle plane identifier derived from Matroska 3d-plane metadata. Used to associate the selected subtitle track with future MVC-side dynamic depth metadata while keeping the current static fallback path.")
#define OPEN3D_MKV_SUBTITLE_SOURCE_ID_TEXT N_("MKV subtitle source id")
#define OPEN3D_MKV_SUBTITLE_SOURCE_ID_LONGTEXT N_( \
    "Selected MKV subtitle source identifier derived from Matroska SOURCE_ID metadata. Used only for MKV subtitle-depth association and ignored when unset.")

#define OPEN3D_TRIGGER_ENABLE_TEXT N_("Enable optical trigger boxes")
#define OPEN3D_TRIGGER_ENABLE_LONGTEXT N_( \
    "Draw alternating optical trigger boxes for left/right eye frames.")

#define OPEN3D_TRIGGER_SIZE_TEXT N_("Trigger box size (px)")
#define OPEN3D_TRIGGER_SIZE_LONGTEXT N_( \
    "Logical trigger box size in eye-space pixels.")

#define OPEN3D_TRIGGER_PADDING_TEXT N_("Trigger box padding (px)")
#define OPEN3D_TRIGGER_PADDING_LONGTEXT N_( \
    "Logical trigger box edge margin in eye-space pixels.")

#define OPEN3D_TRIGGER_SPACING_TEXT N_("Trigger box spacing (px)")
#define OPEN3D_TRIGGER_SPACING_LONGTEXT N_( \
    "Logical spacing between primary and secondary trigger boxes in eye-space pixels.")

#define OPEN3D_TRIGGER_CORNER_TEXT N_("Trigger box corner")
#define OPEN3D_TRIGGER_CORNER_LONGTEXT N_( \
    "Anchor corner for trigger box placement.")

#define OPEN3D_TRIGGER_OFFSET_X_TEXT N_("Trigger horizontal offset (px)")
#define OPEN3D_TRIGGER_OFFSET_X_LONGTEXT N_( \
    "Horizontal trigger offset from the selected corner anchor in eye-space pixels.")

#define OPEN3D_TRIGGER_OFFSET_Y_TEXT N_("Trigger vertical offset (px)")
#define OPEN3D_TRIGGER_OFFSET_Y_LONGTEXT N_( \
    "Vertical trigger offset from the selected corner anchor in eye-space pixels.")

#define OPEN3D_TRIGGER_ALPHA_TEXT N_("Trigger box alpha")
#define OPEN3D_TRIGGER_ALPHA_LONGTEXT N_( \
    "Trigger box opacity from 0.0 (transparent) to 1.0 (opaque).")

#define OPEN3D_TRIGGER_BRIGHTNESS_TEXT N_("Trigger white-box brightness")
#define OPEN3D_TRIGGER_BRIGHTNESS_LONGTEXT N_( \
    "White trigger luminance value from 0 to 255.")

#define OPEN3D_TRIGGER_BLACK_BORDER_TEXT N_("Trigger black border (px)")
#define OPEN3D_TRIGGER_BLACK_BORDER_LONGTEXT N_( \
    "Black border thickness around each trigger box in eye-space pixels.")

#define OPEN3D_TRIGGER_INVERT_TEXT N_("Invert trigger pattern")
#define OPEN3D_TRIGGER_INVERT_LONGTEXT N_( \
    "Invert white/black trigger assignment for left/right eyes.")

#define OPEN3D_CALIBRATION_ENABLE_TEXT N_("Enable calibration reticle")
#define OPEN3D_CALIBRATION_ENABLE_LONGTEXT N_( \
    "Draw a center calibration crosshair overlay.")

#define OPEN3D_CALIBRATION_SIZE_TEXT N_("Calibration size (px)")
#define OPEN3D_CALIBRATION_SIZE_LONGTEXT N_( \
    "Logical half-size of the calibration crosshair in eye-space pixels.")

#define OPEN3D_CALIBRATION_THICKNESS_TEXT N_("Calibration thickness (px)")
#define OPEN3D_CALIBRATION_THICKNESS_LONGTEXT N_( \
    "Logical line thickness of the calibration crosshair in eye-space pixels.")

#define OPEN3D_CALIBRATION_ALPHA_TEXT N_("Calibration alpha")
#define OPEN3D_CALIBRATION_ALPHA_LONGTEXT N_( \
    "Calibration reticle opacity from 0.0 (transparent) to 1.0 (opaque).")

#define OPEN3D_HOTKEYS_ENABLE_TEXT N_("Enable Open3D hotkeys")
#define OPEN3D_HOTKEYS_ENABLE_LONGTEXT N_( \
    "Enable plugin-only Open3D focused-window hotkeys.")

#define OPEN3D_HOTKEYS_PROFILE_TEXT N_("Open3D hotkey profile")
#define OPEN3D_HOTKEYS_PROFILE_LONGTEXT N_( \
    "Select conflict-safe defaults, MPC legacy defaults, or custom key strings.")

#define OPEN3D_KEY_TOGGLE_ENABLE_TEXT N_("Hotkey: toggle Open3D")
#define OPEN3D_KEY_TOGGLE_ENABLE_LONGTEXT N_( \
    "Key binding string for toggling Open3D rendering.")

#define OPEN3D_KEY_TOGGLE_TRIGGER_TEXT N_("Hotkey: toggle status OSD")
#define OPEN3D_KEY_TOGGLE_TRIGGER_LONGTEXT N_( \
    "Key binding string for toggling the persistent Open3D status OSD.")

#define OPEN3D_KEY_TOGGLE_CALIB_TEXT N_("Hotkey: toggle calibration")
#define OPEN3D_KEY_TOGGLE_CALIB_LONGTEXT N_( \
    "Key binding string for toggling calibration overlay.")

#define OPEN3D_KEY_HELP_TEXT N_("Hotkey: show status message")
#define OPEN3D_KEY_HELP_LONGTEXT N_( \
    "Key binding string for showing a short Open3D status message.")

#define OPEN3D_KEY_FLIP_EYES_TEXT N_("Hotkey: flip eyes")
#define OPEN3D_KEY_FLIP_EYES_LONGTEXT N_( \
    "Key binding string for toggling eye swap.")

#define OPEN3D_KEY_EMITTER_READ_TEXT N_("Hotkey: emitter read")
#define OPEN3D_KEY_EMITTER_READ_LONGTEXT N_( \
    "Key binding string for manual emitter read command.")

#define OPEN3D_KEY_EMITTER_APPLY_TEXT N_("Hotkey: emitter apply")
#define OPEN3D_KEY_EMITTER_APPLY_LONGTEXT N_( \
    "Key binding string for manual emitter apply command.")

#define OPEN3D_KEY_EMITTER_SAVE_TEXT N_("Hotkey: emitter save EEPROM")
#define OPEN3D_KEY_EMITTER_SAVE_LONGTEXT N_( \
    "Key binding string for manual emitter save command.")

#define OPEN3D_KEY_EMITTER_RECONNECT_TEXT N_("Hotkey: emitter reconnect")
#define OPEN3D_KEY_EMITTER_RECONNECT_LONGTEXT N_( \
    "Key binding string for manual emitter reconnect command.")

#define OPEN3D_KEY_EMITTER_FW_TEXT N_("Hotkey: emitter firmware update")
#define OPEN3D_KEY_EMITTER_FW_LONGTEXT N_( \
    "Key binding string for emitter firmware update command.")

#define OPEN3D_KEY_CALIB_G_TEXT N_("Hotkey: calibration G")
#define OPEN3D_KEY_CALIB_G_LONGTEXT N_("Calibration command key for calibration-help toggle.")
#define OPEN3D_KEY_CALIB_T_TEXT N_("Hotkey: calibration T")
#define OPEN3D_KEY_CALIB_T_LONGTEXT N_("Calibration command key for trigger drive-mode toggle.")
#define OPEN3D_KEY_CALIB_B_TEXT N_("Hotkey: calibration B")
#define OPEN3D_KEY_CALIB_B_LONGTEXT N_("Calibration command key for emitter EEPROM save.")
#define OPEN3D_KEY_CALIB_P_TEXT N_("Hotkey: calibration P")
#define OPEN3D_KEY_CALIB_P_LONGTEXT N_("Calibration command key for optical debug logging toggle.")
#define OPEN3D_KEY_CALIB_W_TEXT N_("Hotkey: calibration W")
#define OPEN3D_KEY_CALIB_W_LONGTEXT N_("Calibration command key for trigger Y adjust.")
#define OPEN3D_KEY_CALIB_S_TEXT N_("Hotkey: calibration S")
#define OPEN3D_KEY_CALIB_S_LONGTEXT N_("Calibration command key for trigger Y adjust.")
#define OPEN3D_KEY_CALIB_A_TEXT N_("Hotkey: calibration A")
#define OPEN3D_KEY_CALIB_A_LONGTEXT N_("Calibration command key for trigger X adjust.")
#define OPEN3D_KEY_CALIB_D_TEXT N_("Hotkey: calibration D")
#define OPEN3D_KEY_CALIB_D_LONGTEXT N_("Calibration command key for trigger X adjust.")
#define OPEN3D_KEY_CALIB_Q_TEXT N_("Hotkey: calibration Q")
#define OPEN3D_KEY_CALIB_Q_LONGTEXT N_("Calibration command key for trigger spacing down.")
#define OPEN3D_KEY_CALIB_E_TEXT N_("Hotkey: calibration E")
#define OPEN3D_KEY_CALIB_E_LONGTEXT N_("Calibration command key for trigger spacing up.")
#define OPEN3D_KEY_CALIB_N_TEXT N_("Hotkey: calibration N")
#define OPEN3D_KEY_CALIB_N_LONGTEXT N_("Calibration command key for trigger black-border down.")
#define OPEN3D_KEY_CALIB_M_TEXT N_("Hotkey: calibration M")
#define OPEN3D_KEY_CALIB_M_LONGTEXT N_("Calibration command key for trigger black-border up.")
#define OPEN3D_KEY_CALIB_Z_TEXT N_("Hotkey: calibration Z")
#define OPEN3D_KEY_CALIB_Z_LONGTEXT N_("Calibration command key for trigger size down.")
#define OPEN3D_KEY_CALIB_X_TEXT N_("Hotkey: calibration X")
#define OPEN3D_KEY_CALIB_X_LONGTEXT N_("Calibration command key for trigger size up.")
#define OPEN3D_KEY_CALIB_I_TEXT N_("Hotkey: calibration I")
#define OPEN3D_KEY_CALIB_I_LONGTEXT N_("Calibration command key for emitter IR frame-delay down.")
#define OPEN3D_KEY_CALIB_K_TEXT N_("Hotkey: calibration K")
#define OPEN3D_KEY_CALIB_K_LONGTEXT N_("Calibration command key for emitter IR frame-delay up.")
#define OPEN3D_KEY_CALIB_O_TEXT N_("Hotkey: calibration O")
#define OPEN3D_KEY_CALIB_O_LONGTEXT N_("Calibration command key for emitter IR frame-duration down.")
#define OPEN3D_KEY_CALIB_L_TEXT N_("Hotkey: calibration L")
#define OPEN3D_KEY_CALIB_L_LONGTEXT N_("Calibration command key for emitter IR frame-duration up.")

#define OPEN3D_STATUS_ENABLE_TEXT N_("Enable Open3D status/help OSD")
#define OPEN3D_STATUS_ENABLE_LONGTEXT N_( \
    "Draw plugin-side status/help text overlays for hotkeys and emitter state.")

#define OPEN3D_STATUS_DURATION_MS_TEXT N_("Open3D status OSD duration (ms)")
#define OPEN3D_STATUS_DURATION_MS_LONGTEXT N_( \
    "Duration for short status messages shown by Open3D hotkeys/events.")

#define OPEN3D_STATUS_HELP_MS_TEXT N_("Open3D help OSD duration (ms)")
#define OPEN3D_STATUS_HELP_MS_LONGTEXT N_( \
    "Duration for Open3D hotkey help overlay.")

#define OPEN3D_EMITTER_ENABLE_TEXT N_("Enable serial emitter output")
#define OPEN3D_EMITTER_ENABLE_LONGTEXT N_( \
    "Send eye state commands to an Open3D emitter over a Linux tty device.")

#define OPEN3D_EMITTER_TTY_TEXT N_("Emitter tty device")
#define OPEN3D_EMITTER_TTY_LONGTEXT N_( \
    "Emitter serial device path (for example /dev/ttyACM0).")

#define OPEN3D_EMITTER_BAUD_TEXT N_("Emitter baud rate")
#define OPEN3D_EMITTER_BAUD_LONGTEXT N_( \
    "Emitter serial baud rate.")

#define OPEN3D_EMITTER_AUTO_RECONNECT_TEXT N_("Emitter auto reconnect")
#define OPEN3D_EMITTER_AUTO_RECONNECT_LONGTEXT N_( \
    "Retry opening emitter tty if disconnected.")

#define OPEN3D_EMITTER_RECONNECT_MS_TEXT N_("Emitter reconnect interval (ms)")
#define OPEN3D_EMITTER_RECONNECT_MS_LONGTEXT N_( \
    "Delay between emitter reconnect attempts.")

#define OPEN3D_EMITTER_LOG_IO_TEXT N_("Emitter IO debug logs")
#define OPEN3D_EMITTER_LOG_IO_LONGTEXT N_( \
    "Log emitter connect/disconnect and eye command writes.")

#define OPEN3D_EMITTER_READ_ON_CONNECT_TEXT N_("Read emitter settings on connect")
#define OPEN3D_EMITTER_READ_ON_CONNECT_LONGTEXT N_( \
    "Queue command 0 on connect and parse emitter parameters/firmware response.")

#define OPEN3D_EMITTER_APPLY_ON_CONNECT_TEXT N_("Apply emitter settings on connect")
#define OPEN3D_EMITTER_APPLY_ON_CONNECT_LONGTEXT N_( \
    "Queue emitter settings apply command (0,<params>) when connection is established.")

#define OPEN3D_EMITTER_SAVE_ON_APPLY_TEXT N_("Save EEPROM after apply")
#define OPEN3D_EMITTER_SAVE_ON_APPLY_LONGTEXT N_( \
    "Queue command 8 after apply-on-connect to persist settings to EEPROM.")

#define OPEN3D_EMITTER_SETTINGS_JSON_TEXT N_("Emitter settings JSON path")
#define OPEN3D_EMITTER_SETTINGS_JSON_LONGTEXT N_( \
    "Path to emitter settings JSON file. Use \"auto\" for " \
    "$XDG_CONFIG_HOME/open3doled/vlc/local_emitter_settings.json.")

#define OPEN3D_EMITTER_LOAD_JSON_TEXT N_("Load emitter settings JSON on start")
#define OPEN3D_EMITTER_LOAD_JSON_LONGTEXT N_( \
    "Load emitter command 0,<...> fields from JSON during module initialization.")

#define OPEN3D_EMITTER_SAVE_JSON_TEXT N_("Save emitter settings JSON on updates")
#define OPEN3D_EMITTER_SAVE_JSON_LONGTEXT N_( \
    "Write current desired/device emitter settings and dirty state to JSON after updates.")

#define OPEN3D_EMITTER_CMD_READ_TEXT N_("Emitter command: read now")
#define OPEN3D_EMITTER_CMD_READ_LONGTEXT N_( \
    "One-shot trigger. When set to true at runtime, queue command 0 and then reset to false.")

#define OPEN3D_EMITTER_CMD_APPLY_TEXT N_("Emitter command: apply now")
#define OPEN3D_EMITTER_CMD_APPLY_LONGTEXT N_( \
    "One-shot trigger. When set to true at runtime, queue command 0,<params> and then reset to false.")

#define OPEN3D_EMITTER_CMD_SAVE_TEXT N_("Emitter command: save EEPROM now")
#define OPEN3D_EMITTER_CMD_SAVE_LONGTEXT N_( \
    "One-shot trigger. When set to true at runtime, queue command 8 and then reset to false.")

#define OPEN3D_EMITTER_CMD_RECONNECT_TEXT N_("Emitter command: reconnect now")
#define OPEN3D_EMITTER_CMD_RECONNECT_LONGTEXT N_( \
    "One-shot trigger. When set to true at runtime, close/reopen the tty and then reset to false.")

#define OPEN3D_EMITTER_CMD_FW_UPDATE_TEXT N_("Emitter command: firmware update now")
#define OPEN3D_EMITTER_CMD_FW_UPDATE_LONGTEXT N_( \
    "One-shot trigger. Runs firmware helper workflow with preflight and reconnect handling.")

#define OPEN3D_EMITTER_OPT_CSV_ENABLE_TEXT N_("Emitter optical debug CSV")
#define OPEN3D_EMITTER_OPT_CSV_ENABLE_LONGTEXT N_( \
    "Log optical debug stream lines (prefix '+') to a CSV file.")

#define OPEN3D_EMITTER_OPT_CSV_PATH_TEXT N_("Emitter optical debug CSV path")
#define OPEN3D_EMITTER_OPT_CSV_PATH_LONGTEXT N_( \
    "CSV output path. Use \"auto\" for $XDG_STATE_HOME/open3doled/vlc/optical_debug.csv.")

#define OPEN3D_EMITTER_OPT_CSV_FLUSH_TEXT N_("Emitter optical debug CSV flush")
#define OPEN3D_EMITTER_OPT_CSV_FLUSH_LONGTEXT N_( \
    "Flush CSV output on every optical debug line.")

#define OPEN3D_EMITTER_FW_HELPER_TEXT N_("Emitter firmware helper command")
#define OPEN3D_EMITTER_FW_HELPER_LONGTEXT N_( \
    "Path to firmware update helper script/binary.")

#define OPEN3D_EMITTER_FW_HEX_TEXT N_("Emitter firmware image path")
#define OPEN3D_EMITTER_FW_HEX_LONGTEXT N_( \
    "Path to firmware hex image used by firmware helper command.")

#define OPEN3D_EMITTER_FW_BACKUP_JSON_TEXT N_("Emitter firmware backup JSON path")
#define OPEN3D_EMITTER_FW_BACKUP_JSON_LONGTEXT N_( \
    "Backup JSON path before firmware update. Use \"auto\" for $XDG_CONFIG_HOME/open3doled/vlc/firmware_backup_settings.json.")

#define OPEN3D_EMITTER_FW_REAPPLY_TEXT N_("Emitter firmware reapply settings")
#define OPEN3D_EMITTER_FW_REAPPLY_LONGTEXT N_( \
    "Re-apply desired/backup settings after firmware update success.")

#define OPEN3D_EMITTER_IR_PROTOCOL_TEXT N_("Emitter IR protocol")
#define OPEN3D_EMITTER_IR_PROTOCOL_LONGTEXT N_("Emitter IR protocol field in command 0,<...>.")

#define OPEN3D_EMITTER_IR_FRAME_DELAY_TEXT N_("Emitter IR frame delay")
#define OPEN3D_EMITTER_IR_FRAME_DELAY_LONGTEXT N_("Emitter IR frame delay field in command 0,<...>.")

#define OPEN3D_EMITTER_IR_FRAME_DURATION_TEXT N_("Emitter IR frame duration")
#define OPEN3D_EMITTER_IR_FRAME_DURATION_LONGTEXT N_("Emitter IR frame duration field in command 0,<...>.")

#define OPEN3D_EMITTER_IR_SIGNAL_SPACING_TEXT N_("Emitter IR signal spacing")
#define OPEN3D_EMITTER_IR_SIGNAL_SPACING_LONGTEXT N_("Emitter IR signal spacing field in command 0,<...>.")

#define OPEN3D_EMITTER_OPT_BLOCK_DELAY_TEXT N_("Emitter opt block-detection delay")
#define OPEN3D_EMITTER_OPT_BLOCK_DELAY_LONGTEXT N_("Emitter opt block-detection delay field in command 0,<...>.")

#define OPEN3D_EMITTER_OPT_MIN_THRESHOLD_TEXT N_("Emitter opt min threshold")
#define OPEN3D_EMITTER_OPT_MIN_THRESHOLD_LONGTEXT N_("Emitter opt min threshold field in command 0,<...>.")

#define OPEN3D_EMITTER_OPT_THRESHOLD_HIGH_TEXT N_("Emitter opt high threshold")
#define OPEN3D_EMITTER_OPT_THRESHOLD_HIGH_LONGTEXT N_("Emitter opt high threshold field in command 0,<...>.")

#define OPEN3D_EMITTER_OPT_THRESHOLD_LOW_TEXT N_("Emitter opt low threshold")
#define OPEN3D_EMITTER_OPT_THRESHOLD_LOW_LONGTEXT N_("Emitter opt low threshold field in command 0,<...>.")

#define OPEN3D_EMITTER_OPT_IGNORE_DURING_IR_TEXT N_("Emitter opt ignore during IR")
#define OPEN3D_EMITTER_OPT_IGNORE_DURING_IR_LONGTEXT N_("Emitter opt ignore-during-IR field in command 0,<...>.")

#define OPEN3D_EMITTER_OPT_DUP_REALTIME_TEXT N_("Emitter opt duplicate realtime")
#define OPEN3D_EMITTER_OPT_DUP_REALTIME_LONGTEXT N_("Emitter duplicate realtime reporting field in command 0,<...>.")

#define OPEN3D_EMITTER_OPT_OUTPUT_STATS_TEXT N_("Emitter opt output stats")
#define OPEN3D_EMITTER_OPT_OUTPUT_STATS_LONGTEXT N_("Emitter output stats field in command 0,<...>.")

#define OPEN3D_EMITTER_OPT_IGNORE_DUP_TEXT N_("Emitter opt ignore duplicates")
#define OPEN3D_EMITTER_OPT_IGNORE_DUP_LONGTEXT N_("Emitter ignore-duplicates field in command 0,<...>.")

#define OPEN3D_EMITTER_OPT_SENSOR_FILTER_TEXT N_("Emitter opt sensor filter mode")
#define OPEN3D_EMITTER_OPT_SENSOR_FILTER_LONGTEXT N_("Emitter sensor filter mode field in command 0,<...>.")

#define OPEN3D_EMITTER_IR_FLIP_EYES_TEXT N_("Emitter IR flip eyes")
#define OPEN3D_EMITTER_IR_FLIP_EYES_LONGTEXT N_("Emitter IR flip-eyes field in command 0,<...>.")

#define OPEN3D_EMITTER_IR_AVG_TIMING_TEXT N_("Emitter IR average timing mode")
#define OPEN3D_EMITTER_IR_AVG_TIMING_LONGTEXT N_("Emitter IR average-timing field in command 0,<...>.")

#define OPEN3D_EMITTER_TARGET_FRAMETIME_TEXT N_("Emitter target frametime")
#define OPEN3D_EMITTER_TARGET_FRAMETIME_LONGTEXT N_("Emitter target frametime field in command 0,<...>.")

#define OPEN3D_EMITTER_DRIVE_MODE_TEXT N_("Emitter drive mode")
#define OPEN3D_EMITTER_DRIVE_MODE_LONGTEXT N_("Emitter drive mode field in command 0,<...> (0=optical, 1=serial).")

static const char *const open3d_layout_values[] = {
    "auto",
    "sbs",
    "tb",
    "sbs-full",
    "sbs-half",
    "tb-full",
    "tb-half",
};

static const char *const open3d_layout_text[] = {
    N_("Auto"),
    N_("Side-by-Side (Infer Full/Half)"),
    N_("Top-Bottom (Infer Full/Half)"),
    N_("Side-by-Side Full"),
    N_("Side-by-Side Half"),
    N_("Top-Bottom Full"),
    N_("Top-Bottom Half"),
};

static const char *const open3d_half_layout_values[] = {
    "sbs",
    "tb",
};

static const char *const open3d_half_layout_text[] = {
    N_("Side-by-Side"),
    N_("Top-Bottom"),
};

static const char *const open3d_drive_mode_values[] = {
    "optical",
    "serial",
};

static const char *const open3d_drive_mode_text[] = {
    N_("Optical"),
    N_("Serial (PC)"),
};

static const char *const open3d_trigger_corner_values[] = {
    "top-left",
    "top-right",
    "bottom-left",
    "bottom-right",
};

static const char *const open3d_trigger_corner_text[] = {
    N_("Top Left"),
    N_("Top Right"),
    N_("Bottom Left"),
    N_("Bottom Right"),
};

static const char *const open3d_hotkey_profile_values[] = {
    "safe",
    "legacy",
    "custom",
};

static const char *const open3d_hotkey_profile_text[] = {
    N_("Conflict-Safe"),
    N_("MPC Legacy"),
    N_("Custom"),
};

static const char open3d_emitter_cmd_read_var[] = "open3d-emitter-cmd-read";
static const char open3d_emitter_cmd_apply_var[] = "open3d-emitter-cmd-apply";
static const char open3d_emitter_cmd_save_var[] = "open3d-emitter-cmd-save";
static const char open3d_emitter_cmd_reconnect_var[] = "open3d-emitter-cmd-reconnect";
static const char open3d_emitter_cmd_fw_update_var[] = "open3d-emitter-cmd-firmware-update";
vlc_module_begin()
    set_shortname(N_("Open3D"))
    set_description(N_("Open3DOLED page-flipping OpenGL video output"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout display", 271)
    set_callbacks(Open, Close)
    add_shortcut("open3d", "open3d-gl")

    add_module("gl", "opengl", NULL,
               OPEN3D_GL_TEXT, OPEN3D_PROVIDER_LONGTEXT, true)
    add_glopts()

    set_section(N_("Open3D Core"), NULL)
    add_bool("open3d-enable", true,
             OPEN3D_ENABLE_TEXT, OPEN3D_ENABLE_LONGTEXT, true)
    add_string("open3d-layout", "auto",
               OPEN3D_LAYOUT_TEXT, OPEN3D_LAYOUT_LONGTEXT, true)
        change_string_list(open3d_layout_values, open3d_layout_text)
    add_string("open3d-default-half-layout", "sbs",
               OPEN3D_HALF_LAYOUT_TEXT, OPEN3D_HALF_LAYOUT_LONGTEXT, true)
        change_string_list(open3d_half_layout_values, open3d_half_layout_text)
    add_string("open3d-trigger-drive-mode", "optical",
               OPEN3D_DRIVE_MODE_TEXT, OPEN3D_DRIVE_MODE_LONGTEXT, true)
        change_string_list(open3d_drive_mode_values, open3d_drive_mode_text)
    add_bool("open3d-flip-eyes", false,
             OPEN3D_FLIP_EYES_TEXT, OPEN3D_FLIP_EYES_LONGTEXT, true)
    add_float("open3d-target-flip-hz", 0.0,
              OPEN3D_TARGET_FLIP_HZ_TEXT, OPEN3D_TARGET_FLIP_HZ_LONGTEXT, true)
    add_float("open3d-presenter-hz", 120.0,
              OPEN3D_PRESENTER_HZ_TEXT, OPEN3D_PRESENTER_HZ_LONGTEXT, true)
        change_float_range(1.0, 1000.0)
    add_bool("open3d-gpu-overlay-enable", true,
             OPEN3D_GPU_OVERLAY_TEXT, OPEN3D_GPU_OVERLAY_LONGTEXT, true)
    add_bool("open3d-debug-status", false,
             OPEN3D_DEBUG_STATUS_TEXT, OPEN3D_DEBUG_STATUS_LONGTEXT, true)
    add_bool("open3d-presenter-telemetry", false,
             OPEN3D_PRESENTER_TELEMETRY_TEXT, OPEN3D_PRESENTER_TELEMETRY_LONGTEXT, true)
    add_integer("open3d-presenter-telemetry-interval", 120,
                OPEN3D_PRESENTER_TELEMETRY_INTERVAL_TEXT, OPEN3D_PRESENTER_TELEMETRY_INTERVAL_LONGTEXT, true)
        change_integer_range(1, 3600)
    add_bool("open3d-presenter-stage-profile", false,
             OPEN3D_STAGE_PROFILE_TEXT, OPEN3D_STAGE_PROFILE_LONGTEXT, true)
    add_bool("open3d-presenter-rt-enable", true,
             OPEN3D_PRESENTER_RT_ENABLE_TEXT, OPEN3D_PRESENTER_RT_ENABLE_LONGTEXT, true)
    add_integer("open3d-presenter-rt-priority", -1,
                OPEN3D_PRESENTER_RT_PRIORITY_TEXT, OPEN3D_PRESENTER_RT_PRIORITY_LONGTEXT, true)
        change_integer_range(-1, 99)
    add_bool("open3d-presenter-affinity-enable", true,
             OPEN3D_PRESENTER_AFFINITY_ENABLE_TEXT, OPEN3D_PRESENTER_AFFINITY_ENABLE_LONGTEXT, true)
    add_integer("open3d-presenter-affinity-cpu", -1,
                OPEN3D_PRESENTER_AFFINITY_CPU_TEXT, OPEN3D_PRESENTER_AFFINITY_CPU_LONGTEXT, true)
        change_integer_range(-1, 4096)
    add_bool("open3d-presenter-memory-lock", false,
             OPEN3D_PRESENTER_MLOCKALL_TEXT, OPEN3D_PRESENTER_MLOCKALL_LONGTEXT, true)
    add_integer("open3d-presenter-prefault-kb", 256,
                OPEN3D_PRESENTER_PREFAULT_KB_TEXT, OPEN3D_PRESENTER_PREFAULT_KB_LONGTEXT, true)
        change_integer_range(0, 65536)
    add_integer("open3d-presenter-lead-us", 0,
                OPEN3D_PRESENTER_LEAD_US_TEXT, OPEN3D_PRESENTER_LEAD_US_LONGTEXT, true)
        change_integer_range(0, 20000)
    add_bool("open3d-subtitle-depth-enable", true,
             OPEN3D_SUBTITLE_DEPTH_ENABLE_TEXT, OPEN3D_SUBTITLE_DEPTH_ENABLE_LONGTEXT, true)
    add_integer("open3d-subtitle-depth-unit-px", 2,
                OPEN3D_SUBTITLE_DEPTH_UNIT_TEXT, OPEN3D_SUBTITLE_DEPTH_UNIT_LONGTEXT, true)
        change_integer_range(-128, 128)
    add_integer("open3d-mkv-subtitle-static-offset-units", 0,
                OPEN3D_MKV_SUBTITLE_STATIC_OFFSET_TEXT, OPEN3D_MKV_SUBTITLE_STATIC_OFFSET_LONGTEXT, true)
        change_integer_range(-128, 128)
    add_bool("open3d-mkv-subtitle-force", false,
             OPEN3D_MKV_SUBTITLE_FORCE_TEXT, OPEN3D_MKV_SUBTITLE_FORCE_LONGTEXT, true)
    add_integer("open3d-mkv-subtitle-plane", -1,
                OPEN3D_MKV_SUBTITLE_PLANE_TEXT, OPEN3D_MKV_SUBTITLE_PLANE_LONGTEXT, true)
        change_integer_range(-1, 255)
    add_integer("open3d-mkv-subtitle-source-id", -1,
                OPEN3D_MKV_SUBTITLE_SOURCE_ID_TEXT, OPEN3D_MKV_SUBTITLE_SOURCE_ID_LONGTEXT, true)
        change_integer_range(-1, 65535)

    set_section(N_("Trigger Overlay"), NULL)
    add_bool("open3d-trigger-enable", true,
             OPEN3D_TRIGGER_ENABLE_TEXT, OPEN3D_TRIGGER_ENABLE_LONGTEXT, true)
    add_integer("open3d-trigger-size", 13,
                OPEN3D_TRIGGER_SIZE_TEXT, OPEN3D_TRIGGER_SIZE_LONGTEXT, true)
        change_integer_range(1, 512)
    add_integer("open3d-trigger-padding", 10,
                OPEN3D_TRIGGER_PADDING_TEXT, OPEN3D_TRIGGER_PADDING_LONGTEXT, true)
        change_integer_range(0, 2048)
    add_integer("open3d-trigger-spacing", 23,
                OPEN3D_TRIGGER_SPACING_TEXT, OPEN3D_TRIGGER_SPACING_LONGTEXT, true)
        change_integer_range(0, 2048)
    add_string("open3d-trigger-corner", "top-left",
               OPEN3D_TRIGGER_CORNER_TEXT, OPEN3D_TRIGGER_CORNER_LONGTEXT, true)
        change_string_list(open3d_trigger_corner_values, open3d_trigger_corner_text)
    add_integer("open3d-trigger-offset-x", 0,
                OPEN3D_TRIGGER_OFFSET_X_TEXT, OPEN3D_TRIGGER_OFFSET_X_LONGTEXT, true)
        change_integer_range(-8192, 8192)
    add_integer("open3d-trigger-offset-y", 0,
                OPEN3D_TRIGGER_OFFSET_Y_TEXT, OPEN3D_TRIGGER_OFFSET_Y_LONGTEXT, true)
        change_integer_range(-8192, 8192)
    add_float("open3d-trigger-alpha", 1.0,
              OPEN3D_TRIGGER_ALPHA_TEXT, OPEN3D_TRIGGER_ALPHA_LONGTEXT, true)
        change_float_range(0.0, 1.0)
    add_integer("open3d-trigger-brightness", 255,
                OPEN3D_TRIGGER_BRIGHTNESS_TEXT, OPEN3D_TRIGGER_BRIGHTNESS_LONGTEXT, true)
        change_integer_range(0, 255)
    add_integer("open3d-trigger-black-border", 10,
                OPEN3D_TRIGGER_BLACK_BORDER_TEXT, OPEN3D_TRIGGER_BLACK_BORDER_LONGTEXT, true)
        change_integer_range(0, 1024)
    add_bool("open3d-trigger-invert", false,
             OPEN3D_TRIGGER_INVERT_TEXT, OPEN3D_TRIGGER_INVERT_LONGTEXT, true)

    set_section(N_("Calibration"), NULL)
    add_bool("open3d-calibration-enable", false,
             OPEN3D_CALIBRATION_ENABLE_TEXT, OPEN3D_CALIBRATION_ENABLE_LONGTEXT, true)

    set_section(N_("Hotkeys"), NULL)
    add_bool("open3d-hotkeys-enable", true,
             OPEN3D_HOTKEYS_ENABLE_TEXT, OPEN3D_HOTKEYS_ENABLE_LONGTEXT, true)
    add_string("open3d-hotkeys-profile", "safe",
               OPEN3D_HOTKEYS_PROFILE_TEXT, OPEN3D_HOTKEYS_PROFILE_LONGTEXT, true)
        change_string_list(open3d_hotkey_profile_values, open3d_hotkey_profile_text)
    add_string("open3d-key-toggle-enabled", "Ctrl+Shift+F8",
               OPEN3D_KEY_TOGGLE_ENABLE_TEXT, OPEN3D_KEY_TOGGLE_ENABLE_LONGTEXT, true)
    add_string("open3d-key-toggle-trigger", "Ctrl+Shift+F9",
               OPEN3D_KEY_TOGGLE_TRIGGER_TEXT, OPEN3D_KEY_TOGGLE_TRIGGER_LONGTEXT, true)
    add_string("open3d-key-toggle-calibration", "Ctrl+Shift+F10",
               OPEN3D_KEY_TOGGLE_CALIB_TEXT, OPEN3D_KEY_TOGGLE_CALIB_LONGTEXT, true)
    add_string("open3d-key-help", "Ctrl+Shift+F11",
               OPEN3D_KEY_HELP_TEXT, OPEN3D_KEY_HELP_LONGTEXT, true)
    add_string("open3d-key-flip-eyes", "Ctrl+Shift+F12",
               OPEN3D_KEY_FLIP_EYES_TEXT, OPEN3D_KEY_FLIP_EYES_LONGTEXT, true)
    add_string("open3d-key-emitter-read", "Ctrl+Alt+R",
               OPEN3D_KEY_EMITTER_READ_TEXT, OPEN3D_KEY_EMITTER_READ_LONGTEXT, true)
    add_string("open3d-key-emitter-apply", "Ctrl+Alt+Y",
               OPEN3D_KEY_EMITTER_APPLY_TEXT, OPEN3D_KEY_EMITTER_APPLY_LONGTEXT, true)
    add_string("open3d-key-emitter-save", "Ctrl+Alt+U",
               OPEN3D_KEY_EMITTER_SAVE_TEXT, OPEN3D_KEY_EMITTER_SAVE_LONGTEXT, true)
    add_string("open3d-key-emitter-reconnect", "Ctrl+Alt+J",
               OPEN3D_KEY_EMITTER_RECONNECT_TEXT, OPEN3D_KEY_EMITTER_RECONNECT_LONGTEXT, true)
    add_string("open3d-key-emitter-firmware-update", "",
               OPEN3D_KEY_EMITTER_FW_TEXT, OPEN3D_KEY_EMITTER_FW_LONGTEXT, true)

    set_section(N_("Hotkeys Calibration"), NULL)
    add_string("open3d-key-calib-g", "Ctrl+Alt+G",
               OPEN3D_KEY_CALIB_G_TEXT, OPEN3D_KEY_CALIB_G_LONGTEXT, true)
    add_string("open3d-key-calib-t", "Ctrl+Alt+T",
               OPEN3D_KEY_CALIB_T_TEXT, OPEN3D_KEY_CALIB_T_LONGTEXT, true)
    add_string("open3d-key-calib-b", "Ctrl+Alt+B",
               OPEN3D_KEY_CALIB_B_TEXT, OPEN3D_KEY_CALIB_B_LONGTEXT, true)
    add_string("open3d-key-calib-p", "Ctrl+Alt+P",
               OPEN3D_KEY_CALIB_P_TEXT, OPEN3D_KEY_CALIB_P_LONGTEXT, true)
    add_string("open3d-key-calib-w", "Ctrl+Alt+W",
               OPEN3D_KEY_CALIB_W_TEXT, OPEN3D_KEY_CALIB_W_LONGTEXT, true)
    add_string("open3d-key-calib-s", "Ctrl+Alt+S",
               OPEN3D_KEY_CALIB_S_TEXT, OPEN3D_KEY_CALIB_S_LONGTEXT, true)
    add_string("open3d-key-calib-a", "Ctrl+Alt+A",
               OPEN3D_KEY_CALIB_A_TEXT, OPEN3D_KEY_CALIB_A_LONGTEXT, true)
    add_string("open3d-key-calib-d", "Ctrl+Alt+D",
               OPEN3D_KEY_CALIB_D_TEXT, OPEN3D_KEY_CALIB_D_LONGTEXT, true)
    add_string("open3d-key-calib-q", "Ctrl+Alt+Q",
               OPEN3D_KEY_CALIB_Q_TEXT, OPEN3D_KEY_CALIB_Q_LONGTEXT, true)
    add_string("open3d-key-calib-e", "Ctrl+Alt+E",
               OPEN3D_KEY_CALIB_E_TEXT, OPEN3D_KEY_CALIB_E_LONGTEXT, true)
    add_string("open3d-key-calib-n", "Ctrl+Alt+N",
               OPEN3D_KEY_CALIB_N_TEXT, OPEN3D_KEY_CALIB_N_LONGTEXT, true)
    add_string("open3d-key-calib-m", "Ctrl+Alt+M",
               OPEN3D_KEY_CALIB_M_TEXT, OPEN3D_KEY_CALIB_M_LONGTEXT, true)
    add_string("open3d-key-calib-z", "Ctrl+Alt+Z",
               OPEN3D_KEY_CALIB_Z_TEXT, OPEN3D_KEY_CALIB_Z_LONGTEXT, true)
    add_string("open3d-key-calib-x", "Ctrl+Alt+X",
               OPEN3D_KEY_CALIB_X_TEXT, OPEN3D_KEY_CALIB_X_LONGTEXT, true)
    add_string("open3d-key-calib-i", "Ctrl+Alt+I",
               OPEN3D_KEY_CALIB_I_TEXT, OPEN3D_KEY_CALIB_I_LONGTEXT, true)
    add_string("open3d-key-calib-k", "Ctrl+Alt+K",
               OPEN3D_KEY_CALIB_K_TEXT, OPEN3D_KEY_CALIB_K_LONGTEXT, true)
    add_string("open3d-key-calib-o", "Ctrl+Alt+O",
               OPEN3D_KEY_CALIB_O_TEXT, OPEN3D_KEY_CALIB_O_LONGTEXT, true)
    add_string("open3d-key-calib-l", "Ctrl+Alt+L",
               OPEN3D_KEY_CALIB_L_TEXT, OPEN3D_KEY_CALIB_L_LONGTEXT, true)

    set_section(N_("Status OSD"), NULL)
    add_bool("open3d-status-osd-enable", true,
             OPEN3D_STATUS_ENABLE_TEXT, OPEN3D_STATUS_ENABLE_LONGTEXT, true)
    add_integer("open3d-status-osd-duration-ms", 2200,
                OPEN3D_STATUS_DURATION_MS_TEXT, OPEN3D_STATUS_DURATION_MS_LONGTEXT, true)
        change_integer_range(250, 30000)
    add_integer("open3d-status-help-duration-ms", 8000,
                OPEN3D_STATUS_HELP_MS_TEXT, OPEN3D_STATUS_HELP_MS_LONGTEXT, true)
        change_integer_range(500, 120000)

    set_section(N_("Emitter Connection"), NULL)
    add_bool("open3d-emitter-enable", false,
             OPEN3D_EMITTER_ENABLE_TEXT, OPEN3D_EMITTER_ENABLE_LONGTEXT, true)
    add_string("open3d-emitter-tty", "auto",
               OPEN3D_EMITTER_TTY_TEXT, OPEN3D_EMITTER_TTY_LONGTEXT, true)
    add_integer("open3d-emitter-baud", 115200,
                OPEN3D_EMITTER_BAUD_TEXT, OPEN3D_EMITTER_BAUD_LONGTEXT, true)
        change_integer_range(1200, 4000000)
    add_bool("open3d-emitter-auto-reconnect", true,
             OPEN3D_EMITTER_AUTO_RECONNECT_TEXT, OPEN3D_EMITTER_AUTO_RECONNECT_LONGTEXT, true)
    add_integer("open3d-emitter-reconnect-ms", 500,
                OPEN3D_EMITTER_RECONNECT_MS_TEXT, OPEN3D_EMITTER_RECONNECT_MS_LONGTEXT, true)
        change_integer_range(10, 10000)
    add_bool("open3d-emitter-log-io", false,
             OPEN3D_EMITTER_LOG_IO_TEXT, OPEN3D_EMITTER_LOG_IO_LONGTEXT, true)
    add_bool("open3d-emitter-read-on-connect", true,
             OPEN3D_EMITTER_READ_ON_CONNECT_TEXT, OPEN3D_EMITTER_READ_ON_CONNECT_LONGTEXT, true)
    add_bool("open3d-emitter-apply-on-connect", false,
             OPEN3D_EMITTER_APPLY_ON_CONNECT_TEXT, OPEN3D_EMITTER_APPLY_ON_CONNECT_LONGTEXT, true)
    add_bool("open3d-emitter-save-on-apply", false,
             OPEN3D_EMITTER_SAVE_ON_APPLY_TEXT, OPEN3D_EMITTER_SAVE_ON_APPLY_LONGTEXT, true)
    add_string("open3d-emitter-settings-json", "auto",
               OPEN3D_EMITTER_SETTINGS_JSON_TEXT, OPEN3D_EMITTER_SETTINGS_JSON_LONGTEXT, true)
    add_bool("open3d-emitter-load-json", true,
             OPEN3D_EMITTER_LOAD_JSON_TEXT, OPEN3D_EMITTER_LOAD_JSON_LONGTEXT, true)
    add_bool("open3d-emitter-save-json", true,
             OPEN3D_EMITTER_SAVE_JSON_TEXT, OPEN3D_EMITTER_SAVE_JSON_LONGTEXT, true)
    add_string("open3d-emitter-fw-helper", "",
               OPEN3D_EMITTER_FW_HELPER_TEXT, OPEN3D_EMITTER_FW_HELPER_LONGTEXT, true)
    add_string("open3d-emitter-fw-hex", "",
               OPEN3D_EMITTER_FW_HEX_TEXT, OPEN3D_EMITTER_FW_HEX_LONGTEXT, true)
    add_string("open3d-emitter-fw-backup-json", "auto",
               OPEN3D_EMITTER_FW_BACKUP_JSON_TEXT, OPEN3D_EMITTER_FW_BACKUP_JSON_LONGTEXT, true)
    add_bool("open3d-emitter-fw-reapply", true,
             OPEN3D_EMITTER_FW_REAPPLY_TEXT, OPEN3D_EMITTER_FW_REAPPLY_LONGTEXT, true)

    set_section(N_("Emitter Actions (One-Shot)"), NULL)
    add_bool("open3d-emitter-cmd-read", false,
             OPEN3D_EMITTER_CMD_READ_TEXT, OPEN3D_EMITTER_CMD_READ_LONGTEXT, true)
    add_bool("open3d-emitter-cmd-apply", false,
             OPEN3D_EMITTER_CMD_APPLY_TEXT, OPEN3D_EMITTER_CMD_APPLY_LONGTEXT, true)
    add_bool("open3d-emitter-cmd-save", false,
             OPEN3D_EMITTER_CMD_SAVE_TEXT, OPEN3D_EMITTER_CMD_SAVE_LONGTEXT, true)
    add_bool("open3d-emitter-cmd-reconnect", false,
             OPEN3D_EMITTER_CMD_RECONNECT_TEXT, OPEN3D_EMITTER_CMD_RECONNECT_LONGTEXT, true)
    add_bool("open3d-emitter-cmd-firmware-update", false,
             OPEN3D_EMITTER_CMD_FW_UPDATE_TEXT, OPEN3D_EMITTER_CMD_FW_UPDATE_LONGTEXT, true)
    add_bool("open3d-emitter-opt-csv-enable", false,
             OPEN3D_EMITTER_OPT_CSV_ENABLE_TEXT, OPEN3D_EMITTER_OPT_CSV_ENABLE_LONGTEXT, true)
    add_string("open3d-emitter-opt-csv-path", "auto",
               OPEN3D_EMITTER_OPT_CSV_PATH_TEXT, OPEN3D_EMITTER_OPT_CSV_PATH_LONGTEXT, true)
    add_bool("open3d-emitter-opt-csv-flush", false,
             OPEN3D_EMITTER_OPT_CSV_FLUSH_TEXT, OPEN3D_EMITTER_OPT_CSV_FLUSH_LONGTEXT, true)

    set_section(N_("Emitter IR Parameters"), NULL)
    add_integer("open3d-emitter-ir-protocol", 6,
                OPEN3D_EMITTER_IR_PROTOCOL_TEXT, OPEN3D_EMITTER_IR_PROTOCOL_LONGTEXT, true)
        change_integer_range(0, 255)
    add_integer("open3d-emitter-ir-frame-delay", 500,
                OPEN3D_EMITTER_IR_FRAME_DELAY_TEXT, OPEN3D_EMITTER_IR_FRAME_DELAY_LONGTEXT, true)
        change_integer_range(-100000, 100000)
    add_integer("open3d-emitter-ir-frame-duration", 7000,
                OPEN3D_EMITTER_IR_FRAME_DURATION_TEXT, OPEN3D_EMITTER_IR_FRAME_DURATION_LONGTEXT, true)
        change_integer_range(-100000, 100000)
    add_integer("open3d-emitter-ir-signal-spacing", 1000,
                OPEN3D_EMITTER_IR_SIGNAL_SPACING_TEXT, OPEN3D_EMITTER_IR_SIGNAL_SPACING_LONGTEXT, true)
        change_integer_range(-100000, 100000)

    set_section(N_("Emitter Optical Parameters"), NULL)
    add_integer("open3d-emitter-opt-block-delay", 1000,
                OPEN3D_EMITTER_OPT_BLOCK_DELAY_TEXT, OPEN3D_EMITTER_OPT_BLOCK_DELAY_LONGTEXT, true)
        change_integer_range(-100000, 100000)
    add_integer("open3d-emitter-opt-min-threshold", 20,
                OPEN3D_EMITTER_OPT_MIN_THRESHOLD_TEXT, OPEN3D_EMITTER_OPT_MIN_THRESHOLD_LONGTEXT, true)
        change_integer_range(0, 255)
    add_integer("open3d-emitter-opt-threshold-high", 128,
                OPEN3D_EMITTER_OPT_THRESHOLD_HIGH_TEXT, OPEN3D_EMITTER_OPT_THRESHOLD_HIGH_LONGTEXT, true)
        change_integer_range(0, 255)
    add_integer("open3d-emitter-opt-threshold-low", 32,
                OPEN3D_EMITTER_OPT_THRESHOLD_LOW_TEXT, OPEN3D_EMITTER_OPT_THRESHOLD_LOW_LONGTEXT, true)
        change_integer_range(0, 255)
    add_integer("open3d-emitter-opt-ignore-during-ir", 1,
                OPEN3D_EMITTER_OPT_IGNORE_DURING_IR_TEXT, OPEN3D_EMITTER_OPT_IGNORE_DURING_IR_LONGTEXT, true)
        change_integer_range(0, 1)
    add_integer("open3d-emitter-opt-dup-realtime", 0,
                OPEN3D_EMITTER_OPT_DUP_REALTIME_TEXT, OPEN3D_EMITTER_OPT_DUP_REALTIME_LONGTEXT, true)
        change_integer_range(0, 1)
    add_integer("open3d-emitter-opt-output-stats", 0,
                OPEN3D_EMITTER_OPT_OUTPUT_STATS_TEXT, OPEN3D_EMITTER_OPT_OUTPUT_STATS_LONGTEXT, true)
        change_integer_range(0, 1)
    add_integer("open3d-emitter-opt-ignore-duplicates", 1,
                OPEN3D_EMITTER_OPT_IGNORE_DUP_TEXT, OPEN3D_EMITTER_OPT_IGNORE_DUP_LONGTEXT, true)
        change_integer_range(0, 1)
    add_integer("open3d-emitter-opt-sensor-filter", 0,
                OPEN3D_EMITTER_OPT_SENSOR_FILTER_TEXT, OPEN3D_EMITTER_OPT_SENSOR_FILTER_LONGTEXT, true)
        change_integer_range(0, 16)

    set_section(N_("Emitter Sync/Mode"), NULL)
    add_integer("open3d-emitter-ir-flip-eyes", 0,
                OPEN3D_EMITTER_IR_FLIP_EYES_TEXT, OPEN3D_EMITTER_IR_FLIP_EYES_LONGTEXT, true)
        change_integer_range(0, 1)
    add_integer("open3d-emitter-ir-avg-timing", 0,
                OPEN3D_EMITTER_IR_AVG_TIMING_TEXT, OPEN3D_EMITTER_IR_AVG_TIMING_LONGTEXT, true)
        change_integer_range(0, 1)
    add_integer("open3d-emitter-target-frametime", 6670,
                OPEN3D_EMITTER_TARGET_FRAMETIME_TEXT, OPEN3D_EMITTER_TARGET_FRAMETIME_LONGTEXT, true)
        change_integer_range(1000, 100000)
    add_integer("open3d-emitter-drive-mode", 1,
                OPEN3D_EMITTER_DRIVE_MODE_TEXT, OPEN3D_EMITTER_DRIVE_MODE_LONGTEXT, true)
        change_integer_range(0, 1)
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

typedef enum
{
    OPEN3D_LAYOUT_AUTO = 0,
    OPEN3D_LAYOUT_SBS,
    OPEN3D_LAYOUT_TB,
    OPEN3D_LAYOUT_SBS_FULL,
    OPEN3D_LAYOUT_SBS_HALF,
    OPEN3D_LAYOUT_TB_FULL,
    OPEN3D_LAYOUT_TB_HALF,
} open3d_layout_t;

typedef enum
{
    OPEN3D_PACK_NONE = 0,
    OPEN3D_PACK_SBS_FULL,
    OPEN3D_PACK_SBS_HALF,
    OPEN3D_PACK_TB_FULL,
    OPEN3D_PACK_TB_HALF,
} open3d_pack_t;

static void Open3DApplySubtitleStereoShift(vout_display_t *, vout_display_sys_t *,
                                           subpicture_t *, unsigned, bool);
static subpicture_t *Open3DBuildPackedStereoSubtitleSubpicture(vout_display_t *,
                                                               vout_display_sys_t *,
                                                               const subpicture_t *,
                                                               open3d_pack_t);

typedef enum
{
    OPEN3D_HALF_LAYOUT_SBS = 0,
    OPEN3D_HALF_LAYOUT_TB,
} open3d_half_layout_t;

typedef enum
{
    OPEN3D_CORNER_TOP_LEFT = 0,
    OPEN3D_CORNER_TOP_RIGHT,
    OPEN3D_CORNER_BOTTOM_LEFT,
    OPEN3D_CORNER_BOTTOM_RIGHT,
} open3d_corner_t;

typedef struct
{
    int ir_protocol;
    int ir_frame_delay;
    int ir_frame_duration;
    int ir_signal_spacing;
    int opt_block_signal_detection_delay;
    int opt_min_threshold_value_to_activate;
    int opt_detection_threshold_high;
    int opt_enable_ignore_during_ir;
    int opt_enable_duplicate_realtime_reporting;
    int opt_output_stats;
    int opt_ignore_all_duplicates;
    int opt_sensor_filter_mode;
    int ir_flip_eyes;
    int opt_detection_threshold_low;
    int ir_average_timing_mode;
    int target_frametime;
    int ir_drive_mode;
} open3d_emitter_settings_t;

#define OPEN3D_EMITTER_CMD_MAX   320
#define OPEN3D_EMITTER_CMD_QUEUE 32

typedef struct
{
    char command[OPEN3D_EMITTER_CMD_MAX];
    size_t len;
    bool wait_ok;
    vlc_tick_t timeout;
    const char *tag;
} open3d_emitter_cmd_t;

typedef struct
{
    int x;
    int y;
    unsigned w;
    unsigned h;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} open3d_overlay_rect_t;

typedef struct
{
    subpicture_region_t *region_template;
    open3d_overlay_rect_t *gpu_rects;
    size_t gpu_rect_count;
    char text[4096];
    int max_width;
    int max_height;
    int font_size;
    uint32_t color;
    uint8_t background_alpha;
    int pixel_aspect_milli;
    bool center_lines;
    bool valid;
} open3d_text_region_cache_t;

typedef struct
{
    bool enabled;
    bool swap_eyes;
    open3d_layout_t forced_layout;
    open3d_half_layout_t default_half_layout;
} open3d_presenter_state_snapshot_t;

typedef struct
{
    bool valid;
    uint64_t state_epoch;
    unsigned base_width;
    unsigned base_height;
    int pixel_aspect_milli;
    open3d_pack_t pack_mode;
    bool show_right_eye;
    bool message_active;
    subpicture_t *subpicture;
} open3d_overlay_subpicture_cache_t;

typedef struct
{
    bool valid;
    uint64_t signature;
    open3d_pack_t pack_mode;
    unsigned packed_width;
    unsigned packed_height;
    bool subtitle_depth_enable;
    int subtitle_depth_unit_px;
    int offset_signed;
    int offset_seq;
    subpicture_t *pf_left;
    subpicture_t *pf_right;
    subpicture_t *packed;
} open3d_bluray_subtitle_cache_t;

#define OPEN3D_BLURAY_SUBTITLE_OFFSET_HISTORY_CAP 64

typedef struct
{
    uint64_t wall_ns;
    uint64_t cpu_ns;
    uint64_t runq_ns;
    bool runq_valid;
} open3d_presenter_stage_stamp_t;

typedef struct
{
    uint64_t wall_ns;
    uint64_t cpu_ns;
    uint64_t runq_ns;
    bool runq_valid;
} open3d_presenter_stage_metric_t;

typedef struct
{
    bool valid;
    bool explicit_swap;
    open3d_presenter_stage_metric_t prelude;
    open3d_presenter_stage_metric_t emitter_queue;
    open3d_presenter_stage_metric_t status_log;
    open3d_presenter_stage_metric_t compose;
    open3d_presenter_stage_metric_t gl_lock;
    open3d_presenter_stage_metric_t make_current;
    open3d_presenter_stage_metric_t total;
    open3d_presenter_stage_metric_t prepare;
    open3d_presenter_stage_metric_t display;
    open3d_presenter_stage_metric_t swap;
    open3d_presenter_stage_metric_t cleanup;
} open3d_presenter_stage_profile_t;

typedef struct
{
    bool valid;
    vlc_tick_t wallclock;
    int seq;
    int signed_offset;
    int frame_index;
} open3d_bluray_subtitle_offset_sample_t;

typedef struct
{
    bool valid;
    uint64_t content_signature;
    int seq;
    int signed_offset;
    int frame_index;
    vlc_tick_t latched_at;
} open3d_bluray_subtitle_offset_latch_t;

#define OPEN3D_GL_BLEND               0x0BE2u
#define OPEN3D_GL_SCISSOR_TEST        0x0C11u
#define OPEN3D_GL_SRC_ALPHA           0x0302u
#define OPEN3D_GL_ONE_MINUS_SRC_ALPHA 0x0303u
#define OPEN3D_GL_COLOR_BUFFER_BIT    0x00004000u
#define OPEN3D_GL_COLOR_WRITEMASK     0x0C23u
#define OPEN3D_GL_COLOR_CLEAR_VALUE   0x0C22u
#define OPEN3D_GL_SCISSOR_BOX         0x0C10u

typedef void (*open3d_gl_enable_fn)(unsigned);
typedef void (*open3d_gl_disable_fn)(unsigned);
typedef void (*open3d_gl_blendfunc_fn)(unsigned, unsigned);
typedef void (*open3d_gl_scissor_fn)(int, int, int, int);
typedef void (*open3d_gl_clearcolor_fn)(float, float, float, float);
typedef void (*open3d_gl_clear_fn)(unsigned);
typedef void (*open3d_gl_getbooleanv_fn)(unsigned, unsigned char *);
typedef void (*open3d_gl_getfloatv_fn)(unsigned, float *);
typedef void (*open3d_gl_getintegerv_fn)(unsigned, int *);
typedef void (*open3d_gl_colormask_fn)(unsigned char, unsigned char,
                                        unsigned char, unsigned char);

typedef struct
{
    bool ready;
    open3d_gl_enable_fn Enable;
    open3d_gl_disable_fn Disable;
    open3d_gl_blendfunc_fn BlendFunc;
    open3d_gl_scissor_fn Scissor;
    open3d_gl_clearcolor_fn ClearColor;
    open3d_gl_clear_fn Clear;
    open3d_gl_getbooleanv_fn GetBooleanv;
    open3d_gl_getfloatv_fn GetFloatv;
    open3d_gl_getintegerv_fn GetIntegerv;
    open3d_gl_colormask_fn ColorMask;
} open3d_gl_overlay_api_t;

typedef struct
{
    bool valid;
    unsigned canvas_width;
    unsigned canvas_height;
    int x_primary;
    int x_secondary;
    int y;
    unsigned size_x;
    unsigned size_y;
    unsigned black_border_x;
    unsigned black_border_y;
    uint8_t alpha;
    uint8_t brightness;
    subpicture_region_t *pattern_regions[2];
} open3d_trigger_region_cache_t;

typedef struct
{
    bool valid;
    unsigned canvas_width;
    unsigned canvas_height;
    int area_left;
    int area_top;
    int area_right;
    int area_bottom;
    int center_x;
    int center_y;
    unsigned thick_x;
    unsigned thick_y;
    bool include_black;
    uint8_t alpha;
    subpicture_region_t *region_chain;
} open3d_calibration_region_cache_t;

struct vout_display_sys_t
{
    vout_display_opengl_t *vgl;
    vlc_gl_t *gl;
    picture_pool_t *pool;

    bool enabled;
    bool swap_eyes;
    open3d_layout_t forced_layout;
    open3d_half_layout_t default_half_layout;

    double target_flip_hz;
    vlc_tick_t flip_period;
    vlc_tick_t next_flip_deadline;
    bool show_right_eye;
    _Atomic uint64_t presenter_state_seq;
    open3d_presenter_state_snapshot_t presenter_state_snapshot;
    _Atomic uint64_t overlay_state_epoch;

    bool presenter_enable;
    double presenter_hz;
    vlc_tick_t presenter_period;
    bool gpu_overlay_enable;
    bool gpu_overlay_ready;
    bool presenter_telemetry;
    int presenter_telemetry_interval;
    bool presenter_stage_profile;
    bool presenter_rt_request;
    int presenter_rt_priority_request;
    bool presenter_affinity_request;
    int presenter_affinity_cpu_request;
    bool presenter_memory_lock_request;
    int presenter_prefault_kb_request;
    int presenter_lead_us_request;
    vlc_tick_t presenter_lead;
    int presenter_schedstat_fd;
    open3d_gl_overlay_api_t gl_overlay;
    bool presenter_stop;
    _Atomic bool presenter_stop_atomic;
    bool presenter_started;
    vlc_thread_t presenter_thread;
    vlc_cond_t presenter_cond;
    vlc_mutex_t presenter_lock;
    bool control_stop;
    bool control_started;
    vlc_thread_t control_thread;
    vlc_cond_t control_cond;
    vlc_mutex_t control_lock;
    vlc_mutex_t gl_lock;
    picture_t *presenter_picture;
    subpicture_t *presenter_subpicture;
    uint64_t presenter_generation;
    bool place_valid;
    bool place_force;
    vout_display_place_t last_place;

    bool debug_status;
    bool warned_missing_pack;
    bool warned_overlay_alloc;
    bool warned_flip_rate_limited;
    bool subtitle_depth_enable;
    int subtitle_depth_unit_px;
    int mkv_subtitle_static_offset_units;
    bool mkv_subtitle_force;
    int mkv_subtitle_plane;
    int mkv_subtitle_source_id;
    uint64_t late_flip_events;
    uint64_t late_flip_steps_total;
    uint64_t presenter_early_wake_events;
    uint64_t presenter_late_events;
    uint64_t presenter_late_steps_total;
    uint64_t presenter_tick_count;
    uint64_t presenter_deadline_miss_events;
    uint64_t presenter_deadline_miss_steps_total;
    vlc_tick_t presenter_deadline_miss_max;
    uint64_t presenter_profiled_frame_count;
    uint64_t presenter_render_total;
    vlc_tick_t presenter_render_max;
    uint64_t presenter_render_over_budget_events;
    vlc_tick_t presenter_render_over_budget_max;
    uint64_t presenter_prelude_wall_total_ns;
    uint64_t presenter_prelude_cpu_total_ns;
    uint64_t presenter_emitter_queue_wall_total_ns;
    uint64_t presenter_emitter_queue_cpu_total_ns;
    uint64_t presenter_status_log_wall_total_ns;
    uint64_t presenter_status_log_cpu_total_ns;
    uint64_t presenter_compose_wall_total_ns;
    uint64_t presenter_compose_cpu_total_ns;
    uint64_t presenter_gl_lock_wall_total_ns;
    uint64_t presenter_gl_lock_cpu_total_ns;
    uint64_t presenter_make_current_wall_total_ns;
    uint64_t presenter_make_current_cpu_total_ns;
    uint64_t presenter_prepare_wall_total_ns;
    uint64_t presenter_prepare_cpu_total_ns;
    uint64_t presenter_display_wall_total_ns;
    uint64_t presenter_display_cpu_total_ns;
    uint64_t presenter_swap_wall_total_ns;
    uint64_t presenter_swap_cpu_total_ns;
    uint64_t presenter_cleanup_wall_total_ns;
    uint64_t presenter_cleanup_cpu_total_ns;
    uint64_t presenter_prelude_wall_max_ns;
    uint64_t presenter_prelude_cpu_max_ns;
    uint64_t presenter_emitter_queue_wall_max_ns;
    uint64_t presenter_emitter_queue_cpu_max_ns;
    uint64_t presenter_status_log_wall_max_ns;
    uint64_t presenter_status_log_cpu_max_ns;
    uint64_t presenter_compose_wall_max_ns;
    uint64_t presenter_compose_cpu_max_ns;
    uint64_t presenter_gl_lock_wall_max_ns;
    uint64_t presenter_gl_lock_cpu_max_ns;
    uint64_t presenter_make_current_wall_max_ns;
    uint64_t presenter_make_current_cpu_max_ns;
    uint64_t presenter_prepare_wall_max_ns;
    uint64_t presenter_prepare_cpu_max_ns;
    uint64_t presenter_display_wall_max_ns;
    uint64_t presenter_display_cpu_max_ns;
    uint64_t presenter_swap_wall_max_ns;
    uint64_t presenter_swap_cpu_max_ns;
    uint64_t presenter_cleanup_wall_max_ns;
    uint64_t presenter_cleanup_cpu_max_ns;
    uint64_t presenter_sleep_overshoot_events;
    uint64_t presenter_sleep_overshoot_total;
    vlc_tick_t presenter_sleep_overshoot_max;
    bool presenter_rt_enabled;
    bool presenter_affinity_enabled;
    int presenter_affinity_cpu;
    bool presenter_mlockall_enabled;
    size_t presenter_prefault_bytes;
    uint8_t *presenter_prefault_buffer;
    open3d_pack_t last_logged_pack;
    bool last_logged_eye;
    uint64_t frame_count;
    uint64_t bluray_subtitle_cache_hits;
    uint64_t bluray_subtitle_cache_misses;
    open3d_bluray_subtitle_offset_sample_t bluray_subtitle_offset_history[OPEN3D_BLURAY_SUBTITLE_OFFSET_HISTORY_CAP];
    unsigned bluray_subtitle_offset_history_head;
    bool bluray_subtitle_offset_last_valid;
    int bluray_subtitle_offset_last_seq;
    int bluray_subtitle_offset_last_signed;
    int bluray_subtitle_offset_last_frame;
    open3d_bluray_subtitle_offset_latch_t bluray_subtitle_offset_latch;
    bool debug_bluray_subtitle_active;
    bool debug_bluray_subtitle_cache_hit;
    int debug_bluray_subtitle_offset_seq;
    int debug_bluray_subtitle_offset_signed;
    uint64_t debug_bluray_subtitle_signature;
    vlc_tick_t debug_bluray_subtitle_pts;
    vlc_tick_t debug_bluray_subtitle_last_change;

    bool trigger_enable;
    unsigned trigger_size;
    unsigned trigger_padding;
    unsigned trigger_spacing;
    open3d_corner_t trigger_corner;
    int trigger_offset_x;
    int trigger_offset_y;
    uint8_t trigger_alpha;
    uint8_t trigger_brightness;
    unsigned trigger_black_border;
    bool trigger_invert;

    bool calibration_enable;
    unsigned calibration_size;
    unsigned calibration_thickness;
    uint8_t calibration_alpha;

    bool status_osd_enable;
    bool status_overlay_visible;
    vlc_tick_t status_osd_duration;
    vlc_tick_t status_help_duration;
    char status_message[768];
    vlc_tick_t status_message_until;
    bool status_help_active;
    vlc_tick_t status_help_until;
    bool status_calibration_help;
    bool status_prev_emitter_connected_valid;
    bool status_prev_emitter_connected;
    bool status_prev_emitter_dirty_valid;
    bool status_prev_emitter_dirty;
    bool status_prev_firmware_busy_valid;
    bool status_prev_firmware_busy;
    open3d_text_region_cache_t status_main_cache;
    open3d_text_region_cache_t status_serial_cache;
    open3d_text_region_cache_t status_calib_main_cache;
    open3d_text_region_cache_t status_calib_serial_cache;
    open3d_trigger_region_cache_t trigger_region_cache;
    open3d_calibration_region_cache_t calibration_region_cache;
    open3d_overlay_subpicture_cache_t presenter_overlay_cache[2];
    open3d_bluray_subtitle_cache_t bluray_subtitle_cache;
    open3d_presenter_stage_profile_t presenter_stage_profile_last;

    bool hotkeys_enable;
    bool hotkeys_registered;
    open3d_hotkey_profile_t hotkeys_profile;
    uint_fast32_t hotkey_toggle_enabled;
    uint_fast32_t hotkey_toggle_trigger;
    uint_fast32_t hotkey_toggle_calibration;
    uint_fast32_t hotkey_help;
    uint_fast32_t hotkey_flip_eyes;
    uint_fast32_t hotkey_emitter_read;
    uint_fast32_t hotkey_emitter_apply;
    uint_fast32_t hotkey_emitter_save;
    uint_fast32_t hotkey_emitter_reconnect;
    uint_fast32_t hotkey_emitter_fw_update;
    uint_fast32_t hotkey_calib_g;
    uint_fast32_t hotkey_calib_t;
    uint_fast32_t hotkey_calib_b;
    uint_fast32_t hotkey_calib_p;
    uint_fast32_t hotkey_calib_w;
    uint_fast32_t hotkey_calib_s;
    uint_fast32_t hotkey_calib_a;
    uint_fast32_t hotkey_calib_d;
    uint_fast32_t hotkey_calib_q;
    uint_fast32_t hotkey_calib_e;
    uint_fast32_t hotkey_calib_n;
    uint_fast32_t hotkey_calib_m;
    uint_fast32_t hotkey_calib_z;
    uint_fast32_t hotkey_calib_x;
    uint_fast32_t hotkey_calib_i;
    uint_fast32_t hotkey_calib_k;
    uint_fast32_t hotkey_calib_o;
    uint_fast32_t hotkey_calib_l;

    bool emitter_enable;
    char *emitter_tty;
    bool emitter_tty_auto;
    char *emitter_tty_selected;
    int emitter_baud;
    bool emitter_auto_reconnect;
    vlc_tick_t emitter_reconnect_interval;
    bool emitter_log_io;
    bool emitter_read_on_connect;
    bool emitter_apply_on_connect;
    bool emitter_save_on_apply;
    char *emitter_settings_json_path;
    bool emitter_load_json;
    bool emitter_save_json;
    bool emitter_opt_csv_enable;
    char *emitter_opt_csv_path;
    bool emitter_opt_csv_flush;
    FILE *emitter_opt_csv_file;
    bool emitter_opt_csv_header_written;
    char *emitter_fw_helper;
    char *emitter_fw_hex;
    char *emitter_fw_backup_json_path;
    bool emitter_fw_reapply;
    pid_t emitter_fw_pid;
    bool emitter_fw_in_progress;
    bool emitter_fw_reapply_pending;
    vlc_tick_t emitter_fw_started;
    open3d_emitter_settings_t emitter_settings;
    open3d_emitter_settings_t emitter_device_settings;
    bool emitter_device_settings_valid;
    bool emitter_settings_dirty;
    int emitter_fd;
    bool emitter_stop;
    bool emitter_started;
    vlc_thread_t emitter_thread;
    vlc_cond_t emitter_cond;
    bool emitter_eye_pending;
    bool emitter_eye;
    vlc_tick_t emitter_eye_clock;
    bool emitter_eye_reset;
    vlc_tick_t emitter_next_reconnect;
    vlc_tick_t emitter_next_service;
    vlc_tick_t emitter_service_interval;
    bool emitter_last_eye_valid;
    bool emitter_last_eye;
    bool warned_emitter_open;
    bool warned_emitter_detect;
    bool warned_emitter_queue;
    int emitter_firmware_version;

    open3d_emitter_cmd_t emitter_cmd_queue[OPEN3D_EMITTER_CMD_QUEUE];
    unsigned emitter_cmd_head;
    unsigned emitter_cmd_tail;
    bool emitter_cmd_waiting_ok;
    open3d_emitter_cmd_t emitter_cmd_inflight;
    vlc_tick_t emitter_cmd_deadline;

    char emitter_rx_line[512];
    size_t emitter_rx_len;

    vlc_mutex_t emitter_control_lock;
    bool emitter_req_read;
    bool emitter_req_apply;
    bool emitter_req_save;
    bool emitter_req_reconnect;
    bool emitter_req_fw_update;
    bool emitter_pending_read;
    bool emitter_pending_apply;
    bool emitter_pending_save;

    vlc_mutex_t hotkey_lock;
    bool hotkey_req_toggle_enabled;
    bool hotkey_req_toggle_trigger;
    bool hotkey_req_toggle_calibration;
    bool hotkey_req_help;
    bool hotkey_req_flip_eyes;
    bool hotkey_req_emitter_read;
    bool hotkey_req_emitter_apply;
    bool hotkey_req_emitter_save;
    bool hotkey_req_emitter_reconnect;
    bool hotkey_req_emitter_fw_update;
    bool hotkey_req_calib_help_toggle;
    bool hotkey_req_calib_drive_toggle;
    bool hotkey_req_calib_save;
    bool hotkey_req_calib_optlog_toggle;
    int hotkey_req_calib_border_delta;
    int hotkey_req_offset_x_delta;
    int hotkey_req_offset_y_delta;
    int hotkey_req_trigger_size_delta;
    int hotkey_req_trigger_spacing_delta;
    int hotkey_req_trigger_brightness_delta;
    int hotkey_req_ir_frame_delay_delta;
    int hotkey_req_ir_frame_duration_delta;

    video_format_t prepared_eye_source;
    subpicture_t *prepared_owned_subpicture;
};

static open3d_layout_t Open3DParseLayout(const char *value)
{
    if (value == NULL)
        return OPEN3D_LAYOUT_AUTO;
    if (!strcmp(value, "sbs"))
        return OPEN3D_LAYOUT_SBS;
    if (!strcmp(value, "tb"))
        return OPEN3D_LAYOUT_TB;
    if (!strcmp(value, "sbs-full"))
        return OPEN3D_LAYOUT_SBS_FULL;
    if (!strcmp(value, "sbs-half"))
        return OPEN3D_LAYOUT_SBS_HALF;
    if (!strcmp(value, "tb-full"))
        return OPEN3D_LAYOUT_TB_FULL;
    if (!strcmp(value, "tb-half"))
        return OPEN3D_LAYOUT_TB_HALF;
    return OPEN3D_LAYOUT_AUTO;
}

static open3d_half_layout_t Open3DParseDefaultHalfLayout(const char *value)
{
    if (value == NULL)
        return OPEN3D_HALF_LAYOUT_SBS;
    if (!strcmp(value, "tb"))
        return OPEN3D_HALF_LAYOUT_TB;
    return OPEN3D_HALF_LAYOUT_SBS;
}

static int Open3DParseDriveMode(const char *value)
{
    if (value == NULL)
        return 0;
    return !strcmp(value, "optical") ? 0 : 1;
}

static void Open3DEnsureStringVar(vlc_object_t *obj, const char *name, const char *value)
{
    if (obj == NULL || name == NULL || name[0] == '\0')
        return;

    if (var_Type(obj, name) == 0)
        var_Create(obj, name, VLC_VAR_STRING);

    if (value != NULL)
        var_SetString(obj, name, value);
}

static void Open3DEnsureBoolVar(vlc_object_t *obj, const char *name, bool value)
{
    if (obj == NULL || name == NULL || name[0] == '\0')
        return;

    if (var_Type(obj, name) == 0)
        var_Create(obj, name, VLC_VAR_BOOL);

    var_SetBool(obj, name, value);
}

static void Open3DPresenterStatePublish(vout_display_sys_t *sys)
{
    if (sys == NULL)
        return;

    atomic_fetch_add_explicit(&sys->presenter_state_seq, 1, memory_order_acq_rel);
    sys->presenter_state_snapshot.enabled = sys->enabled;
    sys->presenter_state_snapshot.swap_eyes = sys->swap_eyes;
    sys->presenter_state_snapshot.forced_layout = sys->forced_layout;
    sys->presenter_state_snapshot.default_half_layout = sys->default_half_layout;
    atomic_fetch_add_explicit(&sys->presenter_state_seq, 1, memory_order_release);
}

static void Open3DPresenterStateRead(vout_display_sys_t *sys,
                                     open3d_presenter_state_snapshot_t *snapshot)
{
    if (snapshot == NULL)
        return;
    if (sys == NULL)
    {
        memset(snapshot, 0, sizeof(*snapshot));
        return;
    }

    for (;;)
    {
        const uint64_t seq_before =
            atomic_load_explicit(&sys->presenter_state_seq, memory_order_acquire);
        if (seq_before & 1u)
            continue;

        *snapshot = sys->presenter_state_snapshot;

        const uint64_t seq_after =
            atomic_load_explicit(&sys->presenter_state_seq, memory_order_acquire);
        if (seq_before == seq_after)
            break;
    }
}

static void Open3DOverlayStateBump(vout_display_sys_t *sys)
{
    if (sys == NULL)
        return;
    atomic_fetch_add_explicit(&sys->overlay_state_epoch, 1, memory_order_relaxed);
    Open3DPresenterWake(sys);
}

static void Open3DOverlayCacheEntryClear(open3d_overlay_subpicture_cache_t *entry)
{
    if (entry == NULL)
        return;
    if (entry->subpicture != NULL)
        subpicture_Delete(entry->subpicture);
    memset(entry, 0, sizeof(*entry));
}

static void Open3DOverlayCacheClear(vout_display_sys_t *sys)
{
    if (sys == NULL)
        return;
    Open3DOverlayCacheEntryClear(&sys->presenter_overlay_cache[0]);
    Open3DOverlayCacheEntryClear(&sys->presenter_overlay_cache[1]);
}

static void Open3DBluraySubtitleCacheClear(open3d_bluray_subtitle_cache_t *cache)
{
    if (cache == NULL)
        return;
    if (cache->pf_left != NULL)
        subpicture_Delete(cache->pf_left);
    if (cache->pf_right != NULL)
        subpicture_Delete(cache->pf_right);
    if (cache->packed != NULL)
        subpicture_Delete(cache->packed);
    memset(cache, 0, sizeof(*cache));
}

#ifdef __linux__
static bool Open3DReadPresenterSchedstatRunqNs(int fd, uint64_t *runq_ns_out)
{
    if (runq_ns_out == NULL || fd < 0)
        return false;

    char buf[128];
    if (lseek(fd, 0, SEEK_SET) < 0)
        return false;

    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return false;
    buf[n] = '\0';

    uint64_t exec_ns = 0;
    uint64_t runq_ns = 0;
    if (sscanf(buf, "%" SCNu64 " %" SCNu64, &exec_ns, &runq_ns) < 2)
        return false;

    *runq_ns_out = runq_ns;
    return true;
}
#endif

static void Open3DPresenterStageStampNow(vout_display_sys_t *sys,
                                         open3d_presenter_stage_stamp_t *stamp)
{
    if (stamp == NULL)
        return;

    memset(stamp, 0, sizeof(*stamp));

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        stamp->wall_ns = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        stamp->cpu_ns = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;

#ifdef __linux__
    stamp->runq_valid = Open3DReadPresenterSchedstatRunqNs(sys != NULL ? sys->presenter_schedstat_fd : -1,
                                                           &stamp->runq_ns);
#else
    VLC_UNUSED(sys);
#endif
}

static open3d_presenter_stage_metric_t
Open3DPresenterStageMetricDiff(const open3d_presenter_stage_stamp_t *start,
                               const open3d_presenter_stage_stamp_t *end)
{
    open3d_presenter_stage_metric_t metric;
    memset(&metric, 0, sizeof(metric));

    if (start == NULL || end == NULL)
        return metric;

    metric.wall_ns = end->wall_ns >= start->wall_ns ? end->wall_ns - start->wall_ns : 0;
    metric.cpu_ns = end->cpu_ns >= start->cpu_ns ? end->cpu_ns - start->cpu_ns : 0;
    metric.runq_valid = start->runq_valid && end->runq_valid;
    if (metric.runq_valid)
        metric.runq_ns = end->runq_ns >= start->runq_ns ? end->runq_ns - start->runq_ns : 0;

    return metric;
}

static uint64_t Open3DPresenterStageMetricWaitNs(const open3d_presenter_stage_metric_t *metric)
{
    if (metric == NULL || metric->wall_ns <= metric->cpu_ns)
        return 0;
    return metric->wall_ns - metric->cpu_ns;
}

static void Open3DLogPresenterStageProfile(vout_display_t *vd,
                                           const char *reason,
                                           const open3d_presenter_stage_profile_t *profile)
{
    if (vd == NULL || reason == NULL || profile == NULL || !profile->valid)
        return;

    msg_Warn(vd,
             "open3d presenter stage profile reason=%s prelude(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us) "
             "emitter_queue(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us) "
             "status_log(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us) "
             "compose(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us) "
             "gl_lock(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us) "
             "make_current(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us) "
             "total(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us) "
             "prepare(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us) "
             "display(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us) "
             "swap_mode=%s swap(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us) "
             "cleanup(w=%" PRIu64 "us cpu=%" PRIu64 "us wait=%" PRIu64 "us rq=%" PRIu64 "us)",
             reason,
             profile->prelude.wall_ns / 1000, profile->prelude.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->prelude) / 1000,
             profile->prelude.runq_valid ? profile->prelude.runq_ns / 1000 : 0,
             profile->emitter_queue.wall_ns / 1000, profile->emitter_queue.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->emitter_queue) / 1000,
             profile->emitter_queue.runq_valid ? profile->emitter_queue.runq_ns / 1000 : 0,
             profile->status_log.wall_ns / 1000, profile->status_log.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->status_log) / 1000,
             profile->status_log.runq_valid ? profile->status_log.runq_ns / 1000 : 0,
             profile->compose.wall_ns / 1000, profile->compose.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->compose) / 1000,
             profile->compose.runq_valid ? profile->compose.runq_ns / 1000 : 0,
             profile->gl_lock.wall_ns / 1000, profile->gl_lock.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->gl_lock) / 1000,
             profile->gl_lock.runq_valid ? profile->gl_lock.runq_ns / 1000 : 0,
             profile->make_current.wall_ns / 1000, profile->make_current.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->make_current) / 1000,
             profile->make_current.runq_valid ? profile->make_current.runq_ns / 1000 : 0,
             profile->total.wall_ns / 1000, profile->total.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->total) / 1000,
             profile->total.runq_valid ? profile->total.runq_ns / 1000 : 0,
             profile->prepare.wall_ns / 1000, profile->prepare.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->prepare) / 1000,
             profile->prepare.runq_valid ? profile->prepare.runq_ns / 1000 : 0,
             profile->display.wall_ns / 1000, profile->display.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->display) / 1000,
             profile->display.runq_valid ? profile->display.runq_ns / 1000 : 0,
             profile->explicit_swap ? "explicit" : "embedded",
             profile->swap.wall_ns / 1000, profile->swap.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->swap) / 1000,
             profile->swap.runq_valid ? profile->swap.runq_ns / 1000 : 0,
             profile->cleanup.wall_ns / 1000, profile->cleanup.cpu_ns / 1000,
             Open3DPresenterStageMetricWaitNs(&profile->cleanup) / 1000,
             profile->cleanup.runq_valid ? profile->cleanup.runq_ns / 1000 : 0);
}

static int Open3DConfigVarCallback(vlc_object_t *obj, char const *varname,
                                   vlc_value_t oldval, vlc_value_t newval,
                                   void *userdata)
{
    VLC_UNUSED(oldval);
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = userdata;
    bool drive_mode_changed = false;
    bool presenter_state_changed = false;
    bool overlay_state_changed = false;

    if (sys == NULL || varname == NULL)
        return VLC_SUCCESS;

    vlc_mutex_lock(&sys->hotkey_lock);
    if (!strcmp(varname, "open3d-layout"))
    {
        sys->forced_layout = Open3DParseLayout(newval.psz_string);
        presenter_state_changed = true;
        overlay_state_changed = true;
    }
    else if (!strcmp(varname, "open3d-default-half-layout"))
    {
        sys->default_half_layout = Open3DParseDefaultHalfLayout(newval.psz_string);
        presenter_state_changed = true;
        overlay_state_changed = true;
    }
    else if (!strcmp(varname, "open3d-hotkeys-profile"))
    {
        sys->hotkeys_profile = Open3DParseHotkeysProfile(newval.psz_string);
        overlay_state_changed = true;
    }
    else if (!strcmp(varname, "open3d-calibration-enable"))
    {
        sys->calibration_enable = newval.b_bool;
        overlay_state_changed = true;
    }
    else if (!strcmp(varname, "open3d-trigger-drive-mode"))
    {
        const int mode = Open3DParseDriveMode(newval.psz_string);
        drive_mode_changed = (sys->emitter_settings.ir_drive_mode != mode);
        sys->emitter_settings.ir_drive_mode = mode;
        overlay_state_changed = true;
    }
    vlc_mutex_unlock(&sys->hotkey_lock);

    if (!strcmp(varname, "open3d-trigger-drive-mode"))
    {
        Open3DEmitterUpdateDirtyState(vd, sys, "ui-drive-mode");
        if (drive_mode_changed)
        {
            vlc_mutex_lock(&sys->emitter_control_lock);
            sys->emitter_req_apply = true;
            if (sys->emitter_save_on_apply)
                sys->emitter_req_save = true;
            vlc_mutex_unlock(&sys->emitter_control_lock);
            Open3DEmitterWake(sys);
            Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                   "Drive mode set to %s (apply queued)",
                                   sys->emitter_settings.ir_drive_mode == 1 ?
                                   "serial" : "optical");
        }
    }
    else if (!strcmp(varname, "open3d-hotkeys-profile"))
    {
        Open3DPublishHotkeyDefaults(vd, sys->hotkeys_profile);
        Open3DReloadHotkeys(vd, sys);
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               "Hotkey profile: %s",
                               sys->hotkeys_profile == OPEN3D_HOTKEY_PROFILE_LEGACY
                                   ? "MPC legacy"
                                   : (sys->hotkeys_profile == OPEN3D_HOTKEY_PROFILE_CUSTOM
                                          ? "custom" : "conflict-safe"));
    }
    else if (!strcmp(varname, "open3d-calibration-enable"))
    {
        if (sys->calibration_enable)
            sys->status_calibration_help = true;
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               "Calibration mode %s",
                               sys->calibration_enable ? "on" : "off");
    }

    if (presenter_state_changed)
        Open3DPresenterStatePublish(sys);
    if (overlay_state_changed)
        Open3DOverlayStateBump(sys);

    Open3DControlWake(sys);
    Open3DPresenterWake(sys);

    return VLC_SUCCESS;
}

static open3d_corner_t Open3DParseTriggerCorner(const char *value)
{
    if (value == NULL)
        return OPEN3D_CORNER_TOP_LEFT;
    if (!strcmp(value, "top-right"))
        return OPEN3D_CORNER_TOP_RIGHT;
    if (!strcmp(value, "bottom-left"))
        return OPEN3D_CORNER_BOTTOM_LEFT;
    if (!strcmp(value, "bottom-right"))
        return OPEN3D_CORNER_BOTTOM_RIGHT;
    return OPEN3D_CORNER_TOP_LEFT;
}

static open3d_hotkey_profile_t Open3DParseHotkeysProfile(const char *value)
{
    if (value == NULL || value[0] == '\0')
        return OPEN3D_HOTKEY_PROFILE_SAFE;
    if (!strcasecmp(value, "legacy"))
        return OPEN3D_HOTKEY_PROFILE_LEGACY;
    if (!strcasecmp(value, "custom"))
        return OPEN3D_HOTKEY_PROFILE_CUSTOM;
    return OPEN3D_HOTKEY_PROFILE_SAFE;
}

static uint_fast32_t Open3DLoadHotkey(vout_display_t *vd, const char *varname,
                                      open3d_hotkey_profile_t profile,
                                      const char *safe_default,
                                      const char *legacy_default)
{
    char *raw = var_InheritString(vd, varname);
    const char *selected = NULL;

    if (profile == OPEN3D_HOTKEY_PROFILE_SAFE)
        selected = safe_default;
    else if (profile == OPEN3D_HOTKEY_PROFILE_LEGACY)
        selected = legacy_default;
    else if (raw != NULL && raw[0] != '\0')
        selected = raw;
    else
        selected = safe_default;

    uint_fast32_t keycode = KEY_UNSET;
    if (selected != NULL && selected[0] != '\0')
    {
        keycode = vlc_str2keycode(selected);
        if (keycode == KEY_UNSET)
            msg_Warn(vd, "invalid hotkey string for %s: %s", varname, selected);
    }
    free(raw);
    return keycode;
}

static void Open3DPublishHotkeyDefaults(vout_display_t *vd,
                                        open3d_hotkey_profile_t profile)
{
    if (profile == OPEN3D_HOTKEY_PROFILE_CUSTOM)
        return;

    const bool legacy = profile == OPEN3D_HOTKEY_PROFILE_LEGACY;
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-toggle-enabled", "Ctrl+Shift+F8");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-toggle-trigger", "Ctrl+Shift+F9");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-toggle-calibration", "Ctrl+Shift+F10");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-help", "Ctrl+Shift+F11");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-flip-eyes", "Ctrl+Shift+F12");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-emitter-read", "Ctrl+Alt+R");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-emitter-apply", "Ctrl+Alt+Y");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-emitter-save", "Ctrl+Alt+U");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-emitter-reconnect", "Ctrl+Alt+J");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-emitter-firmware-update", "");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-g", legacy ? "g" : "Ctrl+Alt+G");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-t", legacy ? "t" : "Ctrl+Alt+T");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-b", legacy ? "b" : "Ctrl+Alt+B");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-p", legacy ? "p" : "Ctrl+Alt+P");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-w", legacy ? "w" : "Ctrl+Alt+W");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-s", legacy ? "s" : "Ctrl+Alt+S");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-a", legacy ? "a" : "Ctrl+Alt+A");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-d", legacy ? "d" : "Ctrl+Alt+D");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-q", legacy ? "q" : "Ctrl+Alt+Q");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-e", legacy ? "e" : "Ctrl+Alt+E");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-n", legacy ? "n" : "Ctrl+Alt+N");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-m", legacy ? "m" : "Ctrl+Alt+M");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-z", legacy ? "z" : "Ctrl+Alt+Z");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-x", legacy ? "x" : "Ctrl+Alt+X");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-i", legacy ? "i" : "Ctrl+Alt+I");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-k", legacy ? "k" : "Ctrl+Alt+K");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-o", legacy ? "o" : "Ctrl+Alt+O");
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-key-calib-l", legacy ? "l" : "Ctrl+Alt+L");
}

static void Open3DReloadHotkeys(vout_display_t *vd, vout_display_sys_t *sys)
{
    const open3d_hotkey_profile_t profile = sys->hotkeys_profile;
    sys->hotkey_toggle_enabled =
        Open3DLoadHotkey(vd, "open3d-key-toggle-enabled",
                         profile, "Ctrl+Shift+F8", "Ctrl+Shift+F8");
    sys->hotkey_toggle_trigger =
        Open3DLoadHotkey(vd, "open3d-key-toggle-trigger",
                         profile, "Ctrl+Shift+F9", "Ctrl+Shift+F9");
    sys->hotkey_toggle_calibration =
        Open3DLoadHotkey(vd, "open3d-key-toggle-calibration",
                         profile, "Ctrl+Shift+F10", "Ctrl+Shift+F10");
    sys->hotkey_help =
        Open3DLoadHotkey(vd, "open3d-key-help",
                         profile, "Ctrl+Shift+F11", "Ctrl+Shift+F11");
    sys->hotkey_flip_eyes =
        Open3DLoadHotkey(vd, "open3d-key-flip-eyes",
                         profile, "Ctrl+Shift+F12", "Ctrl+Shift+F12");
    sys->hotkey_emitter_read =
        Open3DLoadHotkey(vd, "open3d-key-emitter-read",
                         profile, "Ctrl+Alt+R", "Ctrl+Alt+R");
    sys->hotkey_emitter_apply =
        Open3DLoadHotkey(vd, "open3d-key-emitter-apply",
                         profile, "Ctrl+Alt+Y", "Ctrl+Alt+Y");
    sys->hotkey_emitter_save =
        Open3DLoadHotkey(vd, "open3d-key-emitter-save",
                         profile, "Ctrl+Alt+U", "Ctrl+Alt+U");
    sys->hotkey_emitter_reconnect =
        Open3DLoadHotkey(vd, "open3d-key-emitter-reconnect",
                         profile, "Ctrl+Alt+J", "Ctrl+Alt+J");
    sys->hotkey_emitter_fw_update =
        Open3DLoadHotkey(vd, "open3d-key-emitter-firmware-update",
                         profile, "", "");
    sys->hotkey_calib_g =
        Open3DLoadHotkey(vd, "open3d-key-calib-g",
                         profile, "Ctrl+Alt+G", "g");
    sys->hotkey_calib_t =
        Open3DLoadHotkey(vd, "open3d-key-calib-t",
                         profile, "Ctrl+Alt+T", "t");
    sys->hotkey_calib_b =
        Open3DLoadHotkey(vd, "open3d-key-calib-b",
                         profile, "Ctrl+Alt+B", "b");
    sys->hotkey_calib_p =
        Open3DLoadHotkey(vd, "open3d-key-calib-p",
                         profile, "Ctrl+Alt+P", "p");
    sys->hotkey_calib_w =
        Open3DLoadHotkey(vd, "open3d-key-calib-w",
                         profile, "Ctrl+Alt+W", "w");
    sys->hotkey_calib_s =
        Open3DLoadHotkey(vd, "open3d-key-calib-s",
                         profile, "Ctrl+Alt+S", "s");
    sys->hotkey_calib_a =
        Open3DLoadHotkey(vd, "open3d-key-calib-a",
                         profile, "Ctrl+Alt+A", "a");
    sys->hotkey_calib_d =
        Open3DLoadHotkey(vd, "open3d-key-calib-d",
                         profile, "Ctrl+Alt+D", "d");
    sys->hotkey_calib_q =
        Open3DLoadHotkey(vd, "open3d-key-calib-q",
                         profile, "Ctrl+Alt+Q", "q");
    sys->hotkey_calib_e =
        Open3DLoadHotkey(vd, "open3d-key-calib-e",
                         profile, "Ctrl+Alt+E", "e");
    sys->hotkey_calib_n =
        Open3DLoadHotkey(vd, "open3d-key-calib-n",
                         profile, "Ctrl+Alt+N", "n");
    sys->hotkey_calib_m =
        Open3DLoadHotkey(vd, "open3d-key-calib-m",
                         profile, "Ctrl+Alt+M", "m");
    sys->hotkey_calib_z =
        Open3DLoadHotkey(vd, "open3d-key-calib-z",
                         profile, "Ctrl+Alt+Z", "z");
    sys->hotkey_calib_x =
        Open3DLoadHotkey(vd, "open3d-key-calib-x",
                         profile, "Ctrl+Alt+X", "x");
    sys->hotkey_calib_i =
        Open3DLoadHotkey(vd, "open3d-key-calib-i",
                         profile, "Ctrl+Alt+I", "i");
    sys->hotkey_calib_k =
        Open3DLoadHotkey(vd, "open3d-key-calib-k",
                         profile, "Ctrl+Alt+K", "k");
    sys->hotkey_calib_o =
        Open3DLoadHotkey(vd, "open3d-key-calib-o",
                         profile, "Ctrl+Alt+O", "o");
    sys->hotkey_calib_l =
        Open3DLoadHotkey(vd, "open3d-key-calib-l",
                         profile, "Ctrl+Alt+L", "l");
}

static uint_fast32_t Open3DNormalizeHotkeyCode(uint_fast32_t keycode)
{
    uint_fast32_t base = keycode & ~KEY_MODIFIER;
    uint_fast32_t mods = keycode & KEY_MODIFIER;

    if (base >= 'A' && base <= 'Z')
        base = (base - 'A') + 'a';

    return mods | base;
}

static bool Open3DHotkeyMatchExact(uint_fast32_t key, uint_fast32_t binding)
{
    if (binding == KEY_UNSET)
        return false;
    return Open3DNormalizeHotkeyCode(key) == Open3DNormalizeHotkeyCode(binding);
}

static bool Open3DHotkeyMatchShiftStep(uint_fast32_t key, uint_fast32_t binding, int *step)
{
    if (binding == KEY_UNSET)
        return false;
    uint_fast32_t binding_norm = Open3DNormalizeHotkeyCode(binding);
    uint_fast32_t key_norm = Open3DNormalizeHotkeyCode(key);

    if (binding & KEY_MODIFIER_SHIFT)
    {
        if (key_norm != binding_norm)
            return false;
        if (step != NULL)
            *step = 1;
        return true;
    }

    uint_fast32_t key_no_shift = key_norm & ~KEY_MODIFIER_SHIFT;
    if (key_no_shift != binding_norm)
        return false;
    if (step != NULL)
        *step = (key & KEY_MODIFIER_SHIFT) ? 10 : 1;
    return true;
}

static int Open3DClampInt(int value, int min_value, int max_value)
{
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static int Open3DPixelAspectMilli(double pixel_aspect)
{
    if (pixel_aspect < 0.05 || pixel_aspect > 20.0)
        pixel_aspect = 1.0;
    return (int)(pixel_aspect * 1000.0 + 0.5);
}

static void Open3DTextRegionCacheClear(open3d_text_region_cache_t *cache)
{
    if (cache == NULL)
        return;
    free(cache->gpu_rects);
    cache->gpu_rects = NULL;
    cache->gpu_rect_count = 0;
    if (cache->region_template != NULL)
    {
        subpicture_region_Delete(cache->region_template);
        cache->region_template = NULL;
    }
    cache->valid = false;
    cache->text[0] = '\0';
}

static int Open3DTextRegionCacheBuildGpuRects(open3d_text_region_cache_t *cache)
{
    if (cache == NULL || cache->region_template == NULL ||
        cache->region_template->p_picture == NULL ||
        cache->region_template->p_picture->i_planes == 0)
    {
        return VLC_EGENERIC;
    }

    free(cache->gpu_rects);
    cache->gpu_rects = NULL;
    cache->gpu_rect_count = 0;

    const picture_t *pic = cache->region_template->p_picture;
    const plane_t *plane = &pic->p[0];
    if (plane->i_pixel_pitch < 4)
        return VLC_EGENERIC;

    const unsigned width = cache->region_template->fmt.i_visible_width;
    const unsigned height = cache->region_template->fmt.i_visible_height;
    size_t cap = 0;

    for (unsigned y = 0; y < height; ++y)
    {
        const uint8_t *row = plane->p_pixels + y * plane->i_pitch;
        unsigned x = 0;
        while (x < width)
        {
            const uint8_t *px = row + x * plane->i_pixel_pitch;
            const uint8_t r = px[0];
            const uint8_t g = px[1];
            const uint8_t b = px[2];
            const uint8_t a = px[3];
            if (a == 0)
            {
                x++;
                continue;
            }

            unsigned run_end = x + 1;
            while (run_end < width)
            {
                const uint8_t *npx = row + run_end * plane->i_pixel_pitch;
                if (npx[0] != r || npx[1] != g || npx[2] != b || npx[3] != a)
                    break;
                run_end++;
            }

            if (cache->gpu_rect_count == cap)
            {
                size_t new_cap = cap == 0 ? 128 : cap * 2;
                open3d_overlay_rect_t *new_rects =
                    realloc(cache->gpu_rects, new_cap * sizeof(*new_rects));
                if (new_rects == NULL)
                {
                    free(cache->gpu_rects);
                    cache->gpu_rects = NULL;
                    cache->gpu_rect_count = 0;
                    return VLC_ENOMEM;
                }
                cache->gpu_rects = new_rects;
                cap = new_cap;
            }

            open3d_overlay_rect_t *dst = &cache->gpu_rects[cache->gpu_rect_count++];
            dst->x = (int)x;
            dst->y = (int)y;
            dst->w = run_end - x;
            dst->h = 1;
            dst->r = r;
            dst->g = g;
            dst->b = b;
            dst->a = a;
            x = run_end;
        }
    }

    return VLC_SUCCESS;
}

static subpicture_region_t *Open3DCloneRegionShallow(const subpicture_region_t *src)
{
    if (src == NULL)
        return NULL;

    subpicture_region_t *copy = subpicture_region_New(&src->fmt);
    if (copy == NULL)
        return NULL;

    copy->i_x = src->i_x;
    copy->i_y = src->i_y;
    copy->i_align = src->i_align;
    copy->i_alpha = src->i_alpha;
    copy->i_text_align = src->i_text_align;
    copy->b_noregionbg = src->b_noregionbg;
    copy->b_gridmode = src->b_gridmode;
    copy->b_balanced_text = src->b_balanced_text;
    copy->i_max_width = src->i_max_width;
    copy->i_max_height = src->i_max_height;
    copy->p_next = NULL;

    if (src->p_text != NULL)
    {
        copy->p_text = text_segment_Copy(src->p_text);
        if (copy->p_text == NULL)
        {
            subpicture_region_Delete(copy);
            return NULL;
        }
    }

    if (copy->p_picture != NULL)
    {
        picture_Release(copy->p_picture);
        copy->p_picture = NULL;
    }
    if (src->p_picture != NULL)
    {
        copy->p_picture = picture_Hold(src->p_picture);
        if (copy->p_picture == NULL)
        {
            subpicture_region_Delete(copy);
            return NULL;
        }
    }

    return copy;
}

static int Open3DTextRegionCacheGetCopy(open3d_text_region_cache_t *cache,
                                        const char *text,
                                        int max_width,
                                        int max_height,
                                        int font_size,
                                        uint32_t color,
                                        uint8_t background_alpha,
                                        double pixel_aspect,
                                        bool center_lines,
                                        subpicture_region_t **out_region)
{
    if (out_region != NULL)
        *out_region = NULL;

    if (text == NULL || text[0] == '\0')
        return VLC_SUCCESS;

    const int pa_milli = Open3DPixelAspectMilli(pixel_aspect);
    const bool rebuild =
        cache == NULL ||
        !cache->valid ||
        cache->region_template == NULL ||
        strcmp(cache->text, text) ||
        cache->max_width != max_width ||
        cache->max_height != max_height ||
        cache->font_size != font_size ||
        cache->color != color ||
        cache->background_alpha != background_alpha ||
        cache->pixel_aspect_milli != pa_milli ||
        cache->center_lines != center_lines;

    if (rebuild)
    {
        subpicture_region_t *tmpl =
            Open3DCreateBitmapTextRegion(text, 0, 0,
                                         max_width, max_height,
                                         font_size,
                                         color, background_alpha,
                                         pixel_aspect,
                                         center_lines);
        if (tmpl == NULL)
            return VLC_ENOMEM;

        if (cache != NULL)
        {
            Open3DTextRegionCacheClear(cache);
            cache->region_template = tmpl;
            cache->max_width = max_width;
            cache->max_height = max_height;
            cache->font_size = font_size;
            cache->color = color;
            cache->background_alpha = background_alpha;
            cache->pixel_aspect_milli = pa_milli;
            cache->center_lines = center_lines;
            snprintf(cache->text, sizeof(cache->text), "%s", text);
            cache->valid = true;
            Open3DTextRegionCacheBuildGpuRects(cache);
        }
        else
        {
            if (out_region != NULL)
            {
                *out_region = tmpl;
                return VLC_SUCCESS;
            }
            subpicture_region_Delete(tmpl);
            return VLC_SUCCESS;
        }
    }

    if (cache->region_template == NULL)
        return VLC_ENOMEM;

    if (out_region == NULL)
        return VLC_SUCCESS;

    subpicture_region_t *copy = Open3DCloneRegionShallow(cache->region_template);
    if (copy == NULL)
        return VLC_ENOMEM;

    *out_region = copy;
    return VLC_SUCCESS;
}

static bool Open3DStatusOverlayActive(const vout_display_sys_t *sys, vlc_tick_t now)
{
    if (!sys->status_osd_enable)
        return false;
    if (now == VLC_TICK_INVALID)
        now = mdate();

    const bool general_visible = sys->enabled && sys->status_overlay_visible;
    const bool calibration_visible = sys->enabled && sys->calibration_enable;
    const bool message_active = (sys->status_message_until != VLC_TICK_INVALID &&
                                 now < sys->status_message_until &&
                                 sys->status_message[0] != '\0');

    if (general_visible)
        return true;
    if (calibration_visible && sys->status_calibration_help)
        return true;
    if (message_active && (general_visible || calibration_visible))
        return true;
    if (sys->emitter_enable && sys->emitter_settings_dirty &&
        (general_visible || calibration_visible))
        return true;
    return false;
}

static void Open3DStatusSetMessage(vout_display_t *vd, vout_display_sys_t *sys,
                                   vlc_tick_t duration, const char *fmt, ...)
{
    if (sys == NULL)
        return;
    if (!sys->status_osd_enable || fmt == NULL)
        return;
    if (duration <= 0)
        duration = sys->status_osd_duration;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sys->status_message, sizeof(sys->status_message), fmt, ap);
    va_end(ap);

    vlc_tick_t now = mdate();
    sys->status_message_until = now + duration;
    msg_Dbg(vd, "open3d osd: %s", sys->status_message);
    Open3DOverlayStateBump(sys);
}

static void Open3DStatusShowHelp(vout_display_sys_t *sys, bool enable, vlc_tick_t now)
{
    if (sys == NULL)
        return;
    if (!sys->status_osd_enable)
        return;
    if (now == VLC_TICK_INVALID)
        now = mdate();

    sys->status_help_active = enable;
    if (enable)
        sys->status_help_until = now + sys->status_help_duration;
    else
        sys->status_help_until = VLC_TICK_INVALID;
    Open3DOverlayStateBump(sys);
}

static const char *Open3DHotkeyName(uint_fast32_t keycode, char *buf, size_t buflen)
{
    if (buflen == 0)
        return "";
    if (keycode == KEY_UNSET)
    {
        snprintf(buf, buflen, "unset");
        return buf;
    }

    char *name = vlc_keycode2str(keycode, false);
    if (name == NULL || name[0] == '\0')
    {
        free(name);
        snprintf(buf, buflen, "0x%08" PRIxFAST32, keycode);
        return buf;
    }

    snprintf(buf, buflen, "%s", name);
    free(name);
    return buf;
}

static void Open3DAppendLine(char *dst, size_t dst_size, const char *fmt, ...)
{
    if (dst == NULL || dst_size == 0 || fmt == NULL)
        return;

    size_t len = strnlen(dst, dst_size);
    if (len >= dst_size - 1)
        return;

    if (len > 0)
    {
        dst[len++] = '\n';
        dst[len] = '\0';
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst + len, dst_size - len, fmt, ap);
    va_end(ap);
}

static void Open3DFillRgbaRect(picture_t *pic, int x, int y,
                               unsigned width, unsigned height,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (pic == NULL || pic->i_planes == 0 || width == 0 || height == 0)
        return;
    if (pic->p[0].i_pixel_pitch < 4)
        return;

    const int max_w = pic->p[0].i_visible_pitch / pic->p[0].i_pixel_pitch;
    const int max_h = pic->p[0].i_visible_lines;
    if (max_w <= 0 || max_h <= 0)
        return;

    int x0 = x;
    int y0 = y;
    int x1 = x + (int)width;
    int y1 = y + (int)height;
    if (x1 <= 0 || y1 <= 0 || x0 >= max_w || y0 >= max_h)
        return;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > max_w)
        x1 = max_w;
    if (y1 > max_h)
        y1 = max_h;
    if (x1 <= x0 || y1 <= y0)
        return;

    plane_t *plane = &pic->p[0];
    for (int py = y0; py < y1; ++py)
    {
        uint8_t *row = plane->p_pixels + py * plane->i_pitch;
        for (int px = x0; px < x1; ++px)
        {
            uint8_t *dst = row + px * plane->i_pixel_pitch;
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst[3] = a;
        }
    }
}

#define OPEN3D_BITMAP_FONT_W 8
#define OPEN3D_BITMAP_FONT_H 16
#define OPEN3D_OSD_FONT_SCALE 1.0

static void Open3DDrawGlyph8x16(picture_t *pic, int x, int y,
                                int glyph_w, int glyph_h,
                                const uint8_t glyph[OPEN3D_BITMAP_FONT_H],
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (glyph_w <= 0 || glyph_h <= 0 || glyph == NULL)
        return;

    for (int gy = 0; gy < OPEN3D_BITMAP_FONT_H; ++gy)
    {
        const int y0 = (gy * glyph_h) / OPEN3D_BITMAP_FONT_H;
        const int y1 = ((gy + 1) * glyph_h) / OPEN3D_BITMAP_FONT_H;
        if (y1 <= y0)
            continue;

        const uint8_t row = glyph[gy];
        for (int gx = 0; gx < OPEN3D_BITMAP_FONT_W; ++gx)
        {
            if ((row & (0x80u >> gx)) == 0)
                continue;

            const int x0 = (gx * glyph_w) / OPEN3D_BITMAP_FONT_W;
            const int x1 = ((gx + 1) * glyph_w) / OPEN3D_BITMAP_FONT_W;
            if (x1 <= x0)
                continue;

            Open3DFillRgbaRect(pic,
                               x + x0,
                               y + y0,
                               (unsigned)(x1 - x0), (unsigned)(y1 - y0),
                               r, g, b, a);
        }
    }
}

#define OPEN3D_BITMAP_TEXT_MAX_LINES 128
#define OPEN3D_BITMAP_TEXT_MAX_COLS  192

typedef struct
{
    unsigned line_count;
    unsigned max_cols;
    unsigned line_cols[OPEN3D_BITMAP_TEXT_MAX_LINES];
    char lines[OPEN3D_BITMAP_TEXT_MAX_LINES][OPEN3D_BITMAP_TEXT_MAX_COLS + 1];
} open3d_bitmap_text_layout_t;

static void Open3DBitmapTextLayout(const char *text, unsigned cols_limit,
                                   open3d_bitmap_text_layout_t *layout)
{
    memset(layout, 0, sizeof(*layout));
    if (cols_limit == 0)
        cols_limit = 1;
    if (cols_limit > OPEN3D_BITMAP_TEXT_MAX_COLS)
        cols_limit = OPEN3D_BITMAP_TEXT_MAX_COLS;

    unsigned line = 0;
    unsigned col = 0;
    layout->line_count = 1;

    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p)
    {
        unsigned char ch = *p;
        if (ch == '\r')
            continue;

        if (ch == '\n')
        {
            layout->line_cols[line] = col;
            layout->lines[line][col] = '\0';
            if (col > layout->max_cols)
                layout->max_cols = col;

            if (line + 1 >= OPEN3D_BITMAP_TEXT_MAX_LINES)
                break;

            line++;
            layout->line_count = line + 1;
            col = 0;
            continue;
        }

        if (col >= cols_limit)
        {
            layout->line_cols[line] = col;
            layout->lines[line][col] = '\0';
            if (col > layout->max_cols)
                layout->max_cols = col;

            if (line + 1 >= OPEN3D_BITMAP_TEXT_MAX_LINES)
                break;

            line++;
            layout->line_count = line + 1;
            col = 0;
        }

        if (ch < 32)
            ch = ' ';
        else if (ch >= 127)
            ch = '?';

        layout->lines[line][col++] = (char)ch;
    }

    layout->line_cols[line] = col;
    layout->lines[line][col] = '\0';
    if (col > layout->max_cols)
        layout->max_cols = col;

    if (layout->line_count == 0)
        layout->line_count = 1;
    if (layout->max_cols == 0)
        layout->max_cols = 1;
}

static subpicture_region_t *Open3DCreateBitmapTextRegion(const char *text,
                                                         int x, int y,
                                                         int max_width,
                                                         int max_height,
                                                         int font_size,
                                                         uint32_t color,
                                                         uint8_t background_alpha,
                                                         double pixel_aspect,
                                                         bool center_lines)
{
    if (text == NULL || text[0] == '\0')
        return NULL;

    if (pixel_aspect < 0.05 || pixel_aspect > 20.0)
        pixel_aspect = 1.0;
    /*
     * Compensate glyph width by the inverse sample-aspect ratio so OSD text
     * remains visually square after VLC applies video SAR scaling.
     */
    double x_comp = 1.0 / pixel_aspect;
    x_comp *= 2.0;
    if (x_comp < 0.35)
        x_comp = 0.35;
    if (x_comp > 1.50)
        x_comp = 1.50;

    const int desired_glyph_h = Open3DClampInt(font_size, OPEN3D_BITMAP_FONT_H, 96);
    int chosen_glyph_w = 1;
    int chosen_glyph_h = OPEN3D_BITMAP_FONT_H;
    int chosen_char_gap = 1;
    int chosen_line_gap = 1;
    int chosen_padding_x = 2;
    int chosen_padding_y = 2;
    unsigned chosen_width = 0;
    unsigned chosen_height = 0;
    open3d_bitmap_text_layout_t chosen_layout;
    memset(&chosen_layout, 0, sizeof(chosen_layout));
    bool have_choice = false;

    for (int glyph_h = desired_glyph_h;
         glyph_h >= OPEN3D_BITMAP_FONT_H && !have_choice;
         --glyph_h)
    {
        const int glyph_w = Open3DClampInt(
            (int)((glyph_h * OPEN3D_BITMAP_FONT_W * x_comp) /
                  (double)OPEN3D_BITMAP_FONT_H + 0.5),
            1, 96);
        const int char_gap = Open3DClampInt((glyph_w + 23) / 24, 0, 4);
        const int line_gap = Open3DClampInt((glyph_h + 31) / 32, 1, 4);
        const int padding_x = Open3DClampInt((glyph_w + 3) / 4, 2, 24);
        const int padding_y = Open3DClampInt((glyph_h + 7) / 8, 2, 24);

        unsigned cols_limit = OPEN3D_BITMAP_TEXT_MAX_COLS;
        if (max_width > 0)
        {
            const int avail_w = max_width - 2 * padding_x;
            if (avail_w < glyph_w)
                continue;
            cols_limit = (unsigned)((avail_w + char_gap) / (glyph_w + char_gap));
            if (cols_limit == 0)
                cols_limit = 1;
            if (cols_limit > OPEN3D_BITMAP_TEXT_MAX_COLS)
                cols_limit = OPEN3D_BITMAP_TEXT_MAX_COLS;
        }

        open3d_bitmap_text_layout_t layout;
        Open3DBitmapTextLayout(text, cols_limit, &layout);

        const unsigned width = (unsigned)(2 * padding_x +
            (int)layout.max_cols * glyph_w +
            ((layout.max_cols > 0 ? (int)layout.max_cols - 1 : 0) * char_gap));
        const unsigned height = (unsigned)(2 * padding_y +
            (int)layout.line_count * glyph_h +
            ((layout.line_count > 0 ? (int)layout.line_count - 1 : 0) * line_gap));

        if (max_width > 0 && (int)width > max_width)
            continue;
        if (max_height > 0 && (int)height > max_height)
            continue;

        chosen_glyph_w = glyph_w;
        chosen_glyph_h = glyph_h;
        chosen_char_gap = char_gap;
        chosen_line_gap = line_gap;
        chosen_padding_x = padding_x;
        chosen_padding_y = padding_y;
        chosen_width = width;
        chosen_height = height;
        chosen_layout = layout;
        have_choice = true;
    }

    if (!have_choice)
    {
        Open3DBitmapTextLayout(text, OPEN3D_BITMAP_TEXT_MAX_COLS, &chosen_layout);
        chosen_glyph_h = OPEN3D_BITMAP_FONT_H;
        chosen_glyph_w = Open3DClampInt(
            (int)((chosen_glyph_h * OPEN3D_BITMAP_FONT_W * x_comp) /
                  (double)OPEN3D_BITMAP_FONT_H + 0.5),
            1, 96);
        chosen_char_gap = Open3DClampInt((chosen_glyph_w + 23) / 24, 0, 4);
        chosen_line_gap = 1;
        chosen_padding_x = Open3DClampInt((chosen_glyph_w + 3) / 4, 2, 24);
        chosen_padding_y = Open3DClampInt((chosen_glyph_h + 7) / 8, 2, 24);
        const int glyph_w = chosen_glyph_w;
        const int glyph_h = chosen_glyph_h;
        chosen_width = (unsigned)(2 * chosen_padding_x +
            (int)chosen_layout.max_cols * glyph_w +
            ((chosen_layout.max_cols > 0 ? (int)chosen_layout.max_cols - 1 : 0) * chosen_char_gap));
        chosen_height = (unsigned)(2 * chosen_padding_y +
            (int)chosen_layout.line_count * glyph_h +
            ((chosen_layout.line_count > 0 ? (int)chosen_layout.line_count - 1 : 0) * chosen_line_gap));
    }

    if (chosen_width == 0 || chosen_height == 0)
        return NULL;

    video_format_t fmt;
    video_format_Init(&fmt, VLC_CODEC_RGBA);
    video_format_Setup(&fmt, VLC_CODEC_RGBA,
                       (int)chosen_width, (int)chosen_height,
                       (int)chosen_width, (int)chosen_height,
                       1, 1);

    subpicture_region_t *region = subpicture_region_New(&fmt);
    video_format_Clean(&fmt);
    if (region == NULL || region->p_picture == NULL || region->p_picture->i_planes == 0)
    {
        if (region != NULL)
            subpicture_region_Delete(region);
        return NULL;
    }

    region->i_align = 0;
    region->i_x = x;
    region->i_y = y;
    region->i_alpha = 0xFF;

    picture_t *pic = region->p_picture;
    Open3DFillRgbaRect(pic, 0, 0, chosen_width, chosen_height, 0, 0, 0, 0);
    if (background_alpha > 0)
        Open3DFillRgbaRect(pic, 0, 0, chosen_width, chosen_height, 0, 0, 0, background_alpha);

    const uint8_t fr = (uint8_t)((color >> 16) & 0xFF);
    const uint8_t fg = (uint8_t)((color >> 8) & 0xFF);
    const uint8_t fb = (uint8_t)(color & 0xFF);
    const int glyph_w = chosen_glyph_w;
    const int glyph_h = chosen_glyph_h;
    const int text_area_width = (int)chosen_width - 2 * chosen_padding_x;

    int pen_y = chosen_padding_y;
    for (unsigned li = 0; li < chosen_layout.line_count; ++li)
    {
        int pen_x = chosen_padding_x;
        const unsigned cols = chosen_layout.line_cols[li];
        if (center_lines && cols > 0)
        {
            const int line_px = (int)cols * glyph_w + ((int)cols - 1) * chosen_char_gap;
            if (line_px < text_area_width)
                pen_x += (text_area_width - line_px) / 2;
        }

        for (unsigned ci = 0; ci < cols; ++ci)
        {
            unsigned char ch = (unsigned char)chosen_layout.lines[li][ci];
            if (ch >= 128)
                ch = '?';
            if (ch < 32)
                ch = ' ';

            Open3DDrawGlyph8x16(pic, pen_x, pen_y,
                                chosen_glyph_w, chosen_glyph_h,
                                open3d_font8x16_basic[ch], fr, fg, fb, 0xFF);
            pen_x += glyph_w + chosen_char_gap;
        }
        pen_y += glyph_h + chosen_line_gap;
    }

    return region;
}

static int Open3DAppendStatusOverlay(vout_display_t *vd, vout_display_sys_t *sys,
                                     subpicture_t *merged,
                                     unsigned base_width, unsigned base_height,
                                     vlc_tick_t now,
                                     double pixel_aspect)
{
    if (!Open3DStatusOverlayActive(sys, now))
        return VLC_SUCCESS;

    const bool message_active = (sys->status_message_until != VLC_TICK_INVALID &&
                                 now < sys->status_message_until &&
                                 sys->status_message[0] != '\0');
    const bool general_visible = sys->enabled && sys->status_overlay_visible;
    const bool calibration_visible = sys->enabled && sys->calibration_enable;
    const bool dirty = sys->emitter_enable && sys->emitter_settings_dirty;
    const bool emitter_connected = sys->emitter_enable && sys->emitter_fd >= 0;
    int drive_mode = 0;
    if (emitter_connected && sys->emitter_device_settings_valid)
        drive_mode = sys->emitter_device_settings.ir_drive_mode == 1 ? 1 : 0;
    else if (emitter_connected)
        drive_mode = sys->emitter_settings.ir_drive_mode == 1 ? 1 : 0;
    const bool serial_mode = drive_mode == 1;
    const char *emitter_link = !sys->emitter_enable ? "disabled"
                            : (emitter_connected ? "connected" : "disconnected");
    const char *tty_display = "auto";
    if (sys->emitter_tty_selected != NULL && sys->emitter_tty_selected[0] != '\0')
        tty_display = sys->emitter_tty_selected;
    else if (!sys->emitter_tty_auto && sys->emitter_tty != NULL && sys->emitter_tty[0] != '\0')
        tty_display = sys->emitter_tty;
    else if (!sys->emitter_tty_auto)
        tty_display = "(unset)";

    const int margin_x = Open3DClampInt((int)(base_width / 80), 12, 48);
    const int margin_y = Open3DClampInt((int)(base_height / 80), 12, 36);
    const int status_font_size =
        Open3DClampInt((int)((base_height / 41.0) * OPEN3D_OSD_FONT_SCALE + 0.5), 16, 28);
    const int calib_font_size =
        Open3DClampInt((int)((base_height / 37.0) * OPEN3D_OSD_FONT_SCALE + 0.5), 18, 30);
    const int max_width = (int)base_width - margin_x * 2;
    const int status_max_height = (int)base_height / 2;
    const int calib_max_height = (int)base_height - margin_y * 2;

    char status_block[4096];
    char status_serial_block[2048];
    char calibration_block[4096];
    char calibration_serial_block[2048];
    status_block[0] = '\0';
    status_serial_block[0] = '\0';
    calibration_block[0] = '\0';
    calibration_serial_block[0] = '\0';

    char k_toggle[96], k_osd[96], k_calib[96], k_help[96], k_flip[96];
    char k_g[96], k_t[96], k_b[96], k_p[96];
    char k_w[96], k_s[96], k_a[96], k_d[96];
    char k_q[96], k_e[96], k_n[96], k_m[96];
    char k_z[96], k_x[96], k_i[96], k_k[96], k_o[96], k_l[96];
    if (general_visible || calibration_visible)
    {
        Open3DHotkeyName(sys->hotkey_toggle_enabled, k_toggle, sizeof(k_toggle));
        Open3DHotkeyName(sys->hotkey_toggle_trigger, k_osd, sizeof(k_osd));
        Open3DHotkeyName(sys->hotkey_toggle_calibration, k_calib, sizeof(k_calib));
        Open3DHotkeyName(sys->hotkey_help, k_help, sizeof(k_help));
        Open3DHotkeyName(sys->hotkey_flip_eyes, k_flip, sizeof(k_flip));
        Open3DHotkeyName(sys->hotkey_calib_g, k_g, sizeof(k_g));
        Open3DHotkeyName(sys->hotkey_calib_t, k_t, sizeof(k_t));
        Open3DHotkeyName(sys->hotkey_calib_b, k_b, sizeof(k_b));
        Open3DHotkeyName(sys->hotkey_calib_p, k_p, sizeof(k_p));
        Open3DHotkeyName(sys->hotkey_calib_w, k_w, sizeof(k_w));
        Open3DHotkeyName(sys->hotkey_calib_s, k_s, sizeof(k_s));
        Open3DHotkeyName(sys->hotkey_calib_a, k_a, sizeof(k_a));
        Open3DHotkeyName(sys->hotkey_calib_d, k_d, sizeof(k_d));
        Open3DHotkeyName(sys->hotkey_calib_q, k_q, sizeof(k_q));
        Open3DHotkeyName(sys->hotkey_calib_e, k_e, sizeof(k_e));
        Open3DHotkeyName(sys->hotkey_calib_n, k_n, sizeof(k_n));
        Open3DHotkeyName(sys->hotkey_calib_m, k_m, sizeof(k_m));
        Open3DHotkeyName(sys->hotkey_calib_z, k_z, sizeof(k_z));
        Open3DHotkeyName(sys->hotkey_calib_x, k_x, sizeof(k_x));
        Open3DHotkeyName(sys->hotkey_calib_i, k_i, sizeof(k_i));
        Open3DHotkeyName(sys->hotkey_calib_k, k_k, sizeof(k_k));
        Open3DHotkeyName(sys->hotkey_calib_o, k_o, sizeof(k_o));
        Open3DHotkeyName(sys->hotkey_calib_l, k_l, sizeof(k_l));
    }

    if (general_visible)
    {
        Open3DAppendLine(status_block, sizeof(status_block),
                         "Pageflip %s drive=%s emitter=%s trig=%s calib=%s",
                         sys->enabled ? "on" : "off",
                         serial_mode ? "serial" : "optical",
                         emitter_link,
                         sys->trigger_enable ? "on" : "off",
                         sys->calibration_enable ? "on" : "off");
        Open3DAppendLine(status_block, sizeof(status_block),
                         "Emitter tty: %s", tty_display);

        Open3DAppendLine(status_block, sizeof(status_block),
                         "Hotkeys: %s=2d/3d  %s=osd  %s=calibration",
                         k_toggle, k_osd, k_calib);
        Open3DAppendLine(status_block, sizeof(status_block),
                         "Hotkeys: %s=status  %s=flip",
                         k_help, k_flip);
    }

    if (message_active && (general_visible || calibration_visible))
    {
        if (general_visible)
            Open3DAppendLine(status_block, sizeof(status_block), "%s", sys->status_message);
        else
            Open3DAppendLine(calibration_block, sizeof(calibration_block), "%s", sys->status_message);
    }

    if (dirty && (general_visible || calibration_visible))
    {
        if (general_visible)
            Open3DAppendLine(status_block, sizeof(status_block),
                             "Emitter settings not saved to EEPROM (press %s to save)", k_b);
        else
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Emitter settings not saved to EEPROM (press %s to save)", k_b);
    }

    if (calibration_visible)
    {
        if (sys->status_calibration_help)
        {
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Calibration mode %s", serial_mode ? "serial" : "optical");
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "%s: Toggle calibration help overlay.", k_g);
            Open3DAppendLine(calibration_serial_block, sizeof(calibration_serial_block),
                             "%s: Toggle drive mode (0=optical, 1=serial).", k_t);
            Open3DAppendLine(calibration_serial_block, sizeof(calibration_serial_block),
                             "%s: Save current emitter settings to EEPROM.", k_b);
            Open3DAppendLine(calibration_serial_block, sizeof(calibration_serial_block),
                             "%s/%s: (us) Delay after signal before activating glasses.", k_i, k_k);
            Open3DAppendLine(calibration_serial_block, sizeof(calibration_serial_block),
                             "%s/%s: (us) Duration to keep glasses active after activation.", k_o, k_l);
            if (!emitter_connected)
                Open3DAppendLine(calibration_serial_block, sizeof(calibration_serial_block),
                                 "Serial emitter disconnected; serial hotkeys are unavailable.");
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Shift: Use larger step sizes for adjustments.");
            if (!serial_mode)
            {
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s/%s: Whitebox vertical position.", k_w, k_s);
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s/%s: Whitebox horizontal position.", k_a, k_d);
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s/%s: Spacing between the two whiteboxes.", k_q, k_e);
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s/%s: Whitebox size.", k_z, k_x);
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s/%s: Width of black border around trigger boxes.", k_n, k_m);
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s: Toggle optical debug logging.", k_p);
            }
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Drive mode: %s  emitter: %s",
                             serial_mode ? "serial" : "optical",
                             emitter_link);
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Emitter tty: %s", tty_display);
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Opt debug logging: %s",
                             serial_mode ? "N/A (serial mode)"
                                         : (sys->emitter_opt_csv_enable ? "ON" : "OFF"));
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Top-level: %s/%s/%s/%s/%s = 2d/3d, osd, calibration, help, flip.",
                             k_toggle, k_osd, k_calib, k_help, k_flip);
        }
        else
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Calibration mode ON (press %s for calibration help)", k_g);
    }

    int status_region_y = 0;
    bool status_region_visible = false;
    if (status_block[0] != '\0')
    {
        subpicture_region_t *status = NULL;
        if (Open3DTextRegionCacheGetCopy(&sys->status_main_cache,
                                         status_block,
                                         max_width, status_max_height,
                                         status_font_size,
                                         0xD6F8FF, 120,
                                         pixel_aspect,
                                         false,
                                         &status) != VLC_SUCCESS)
            return VLC_ENOMEM;
        if (status == NULL)
            return VLC_ENOMEM;

        int h = (int)status->fmt.i_visible_height;
        if (h <= 0)
            h = (int)status->fmt.i_height;
        int y = (int)base_height - margin_y - h;
        if (y < margin_y)
            y = margin_y;
        status->i_y = y;
        status_region_y = y;
        status_region_visible = true;
        Open3DRegionChainAppend(&merged->p_region, status);
    }

    if (status_serial_block[0] != '\0')
    {
        subpicture_region_t *status_serial = NULL;
        if (Open3DTextRegionCacheGetCopy(&sys->status_serial_cache,
                                         status_serial_block,
                                         max_width, status_max_height,
                                         status_font_size,
                                         emitter_connected ? 0xD6F8FF : 0x8A8A8A, 120,
                                         pixel_aspect,
                                         false,
                                         &status_serial) != VLC_SUCCESS)
            return VLC_ENOMEM;
        if (status_serial == NULL)
            return VLC_ENOMEM;

        int h = (int)status_serial->fmt.i_visible_height;
        if (h <= 0)
            h = (int)status_serial->fmt.i_height;

        int y;
        if (status_region_visible)
        {
            const int gap = Open3DClampInt(status_font_size / 3, 4, 12);
            y = status_region_y - h - gap;
        }
        else
        {
            y = (int)base_height - margin_y - h;
        }
        if (y < margin_y)
            y = margin_y;
        status_serial->i_y = y;
        Open3DRegionChainAppend(&merged->p_region, status_serial);
    }

    int calib_region_x = 0;
    int calib_region_y = 0;
    int calib_region_h = 0;
    bool calib_region_visible = false;
    if (calibration_block[0] != '\0')
    {
        subpicture_region_t *calib = NULL;
        if (Open3DTextRegionCacheGetCopy(&sys->status_calib_main_cache,
                                         calibration_block,
                                         max_width, calib_max_height,
                                         calib_font_size,
                                         0xFFE8B0, 112,
                                         pixel_aspect,
                                         true,
                                         &calib) != VLC_SUCCESS)
            return VLC_ENOMEM;
        if (calib == NULL)
            return VLC_ENOMEM;

        int w = (int)calib->fmt.i_visible_width;
        int h = (int)calib->fmt.i_visible_height;
        if (w <= 0)
            w = (int)calib->fmt.i_width;
        if (h <= 0)
            h = (int)calib->fmt.i_height;

        int x = ((int)base_width - w) / 2;
        int y = ((int)base_height - h) / 2;
        if (x < margin_x)
            x = margin_x;
        if (y < margin_y)
            y = margin_y;
        if (x + w > (int)base_width - margin_x)
            x = (int)base_width - margin_x - w;
        if (x < margin_x)
            x = margin_x;
        calib->i_x = x;
        calib->i_y = y;
        calib_region_x = x;
        calib_region_y = y;
        calib_region_h = h;
        calib_region_visible = true;
        Open3DRegionChainAppend(&merged->p_region, calib);
    }

    if (calibration_serial_block[0] != '\0')
    {
        subpicture_region_t *calib_serial = NULL;
        if (Open3DTextRegionCacheGetCopy(&sys->status_calib_serial_cache,
                                         calibration_serial_block,
                                         max_width, calib_max_height,
                                         calib_font_size,
                                         emitter_connected ? 0xFFE8B0 : 0x8A8A8A, 112,
                                         pixel_aspect,
                                         true,
                                         &calib_serial) != VLC_SUCCESS)
            return VLC_ENOMEM;
        if (calib_serial == NULL)
            return VLC_ENOMEM;

        int w = (int)calib_serial->fmt.i_visible_width;
        int h = (int)calib_serial->fmt.i_visible_height;
        if (w <= 0)
            w = (int)calib_serial->fmt.i_width;
        if (h <= 0)
            h = (int)calib_serial->fmt.i_height;

        int x = ((int)base_width - w) / 2;
        int y = ((int)base_height - h) / 2;
        if (calib_region_visible)
        {
            const int gap = Open3DClampInt(calib_font_size / 3, 4, 14);
            y = calib_region_y + calib_region_h + gap;
            if (y + h > (int)base_height - margin_y)
                y = calib_region_y - h - gap;
            x = calib_region_x;
        }

        if (x < margin_x)
            x = margin_x;
        if (x + w > (int)base_width - margin_x)
            x = (int)base_width - margin_x - w;
        if (x < margin_x)
            x = margin_x;

        if (y < margin_y)
            y = margin_y;
        if (y + h > (int)base_height - margin_y)
            y = (int)base_height - margin_y - h;
        if (y < margin_y)
            y = margin_y;

        calib_serial->i_x = x;
        calib_serial->i_y = y;
        Open3DRegionChainAppend(&merged->p_region, calib_serial);
    }

    VLC_UNUSED(vd);
    return VLC_SUCCESS;
}

static void Open3DUpdateEmitterStatusWarnings(vout_display_t *vd, vout_display_sys_t *sys)
{
    const bool connected = sys->emitter_fd >= 0;
    if (!sys->status_prev_emitter_connected_valid)
    {
        sys->status_prev_emitter_connected = connected;
        sys->status_prev_emitter_connected_valid = true;
    }
    else if (connected != sys->status_prev_emitter_connected)
    {
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               connected ? "Emitter connected" : "Emitter disconnected");
        sys->status_prev_emitter_connected = connected;
        Open3DOverlayStateBump(sys);
    }

    const bool dirty = sys->emitter_settings_dirty;
    if (!sys->status_prev_emitter_dirty_valid)
    {
        sys->status_prev_emitter_dirty = dirty;
        sys->status_prev_emitter_dirty_valid = true;
        if (dirty)
            Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                   "Emitter settings dirty");
    }
    else if (dirty != sys->status_prev_emitter_dirty)
    {
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               dirty ? "Emitter settings dirty" : "Emitter settings synced");
        sys->status_prev_emitter_dirty = dirty;
        Open3DOverlayStateBump(sys);
    }

    const bool firmware_busy = sys->emitter_fw_in_progress;
    if (!sys->status_prev_firmware_busy_valid)
    {
        sys->status_prev_firmware_busy = firmware_busy;
        sys->status_prev_firmware_busy_valid = true;
    }
    else if (firmware_busy != sys->status_prev_firmware_busy)
    {
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               firmware_busy ? "Emitter firmware update started"
                                             : "Emitter firmware update finished");
        sys->status_prev_firmware_busy = firmware_busy;
        Open3DOverlayStateBump(sys);
    }
}

static unsigned Open3DSaturatingMulUnsigned(unsigned value, unsigned factor)
{
    if (value == 0 || factor == 0)
        return 0;
    if (value > UINT_MAX / factor)
        return UINT_MAX;
    return value * factor;
}

static unsigned Open3DGCDUnsigned(unsigned a, unsigned b)
{
    while (b != 0)
    {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    return a == 0 ? 1 : a;
}

static void Open3DNormalizeSar(video_format_t *fmt)
{
    if (fmt->i_sar_num == 0 || fmt->i_sar_den == 0)
    {
        fmt->i_sar_num = 1;
        fmt->i_sar_den = 1;
        return;
    }

    unsigned gcd = Open3DGCDUnsigned(fmt->i_sar_num, fmt->i_sar_den);
    fmt->i_sar_num /= gcd;
    fmt->i_sar_den /= gcd;
}

static open3d_pack_t Open3DInferHalfPackedFromAspect(const video_format_t *source,
                                                      bool prefer_sbs)
{
    if (source == NULL || source->i_visible_width == 0 || source->i_visible_height == 0)
        return OPEN3D_PACK_NONE;

    const double aspect = (double)source->i_visible_width / (double)source->i_visible_height;
    if (prefer_sbs)
        return aspect > 2.39 ? OPEN3D_PACK_SBS_FULL : OPEN3D_PACK_SBS_HALF;
    return aspect < (4.0 / 3.0) ? OPEN3D_PACK_TB_FULL : OPEN3D_PACK_TB_HALF;
}

static open3d_pack_t Open3DDetectPackModeWithState(vout_display_t *vd,
                                                   open3d_layout_t forced_layout,
                                                   open3d_half_layout_t default_half_layout)
{
    switch (forced_layout)
    {
        case OPEN3D_LAYOUT_SBS_FULL:
            return OPEN3D_PACK_SBS_FULL;
        case OPEN3D_LAYOUT_SBS_HALF:
            return OPEN3D_PACK_SBS_HALF;
        case OPEN3D_LAYOUT_TB_FULL:
            return OPEN3D_PACK_TB_FULL;
        case OPEN3D_LAYOUT_TB_HALF:
            return OPEN3D_PACK_TB_HALF;
        case OPEN3D_LAYOUT_SBS:
            return Open3DInferHalfPackedFromAspect(&vd->source, true);
        case OPEN3D_LAYOUT_TB:
            return Open3DInferHalfPackedFromAspect(&vd->source, false);
        case OPEN3D_LAYOUT_AUTO:
        default:
            break;
    }

    if (vd->source.i_visible_width == 0 || vd->source.i_visible_height == 0)
        return OPEN3D_PACK_NONE;

    bool have_orientation_hint = false;
    bool prefer_sbs = false;

    switch (vd->source.multiview_mode)
    {
        case MULTIVIEW_STEREO_SBS:
        case MULTIVIEW_STEREO_COL:
            have_orientation_hint = true;
            prefer_sbs = true;
            break;

        case MULTIVIEW_STEREO_TB:
        case MULTIVIEW_STEREO_ROW:
            have_orientation_hint = true;
            prefer_sbs = false;
            break;

        default:
            break;
    }

    if (have_orientation_hint)
        return Open3DInferHalfPackedFromAspect(&vd->source, prefer_sbs);

    const double aspect = (double)vd->source.i_visible_width / (double)vd->source.i_visible_height;
    if (aspect > 2.39)
        return OPEN3D_PACK_SBS_FULL;
    if (aspect < (4.0 / 3.0))
        return OPEN3D_PACK_TB_FULL;
    return default_half_layout == OPEN3D_HALF_LAYOUT_SBS
         ? OPEN3D_PACK_SBS_HALF
         : OPEN3D_PACK_TB_HALF;
}

static open3d_pack_t Open3DDetectPackMode(vout_display_t *vd, vout_display_sys_t *sys)
{
    open3d_presenter_state_snapshot_t snapshot;
    Open3DPresenterStateRead(sys, &snapshot);
    return Open3DDetectPackModeWithState(vd, snapshot.forced_layout,
                                         snapshot.default_half_layout);
}

static vlc_tick_t Open3DFrameClock(const picture_t *pic)
{
    if (pic != NULL && pic->date != VLC_TICK_INVALID)
        return pic->date;
    return mdate();
}

static const char *Open3DPackName(open3d_pack_t pack_mode)
{
    switch (pack_mode)
    {
        case OPEN3D_PACK_SBS_FULL:
            return "sbs-full";
        case OPEN3D_PACK_SBS_HALF:
            return "sbs-half";
        case OPEN3D_PACK_TB_FULL:
            return "tb-full";
        case OPEN3D_PACK_TB_HALF:
            return "tb-half";
        case OPEN3D_PACK_NONE:
        default:
            return "none";
    }
}

static unsigned Open3DAdvanceEye(vout_display_sys_t *sys, bool enabled,
                                 vlc_tick_t frame_clock)
{
    if (!enabled)
        return 0;

    if (sys->presenter_enable &&
        sys->flip_period <= 0 &&
        sys->presenter_period > 0)
    {
        /* In presenter mode (target-flip-hz=0), cadence is driven by the
         * presenter thread itself. Toggle once for each displayed presenter
         * frame so condition wakeups cannot cause same-eye repeats. */
        if (sys->frame_count > 0)
            sys->show_right_eye = !sys->show_right_eye;
        return 1;
    }

    vlc_tick_t period = sys->flip_period;
    if (period <= 0)
    {
        if (sys->frame_count > 0)
        {
            sys->show_right_eye = !sys->show_right_eye;
            return 1;
        }
        return 0;
    }

    if (frame_clock == VLC_TICK_INVALID)
        frame_clock = mdate();

    if (sys->next_flip_deadline == VLC_TICK_INVALID)
    {
        sys->next_flip_deadline = frame_clock + period;
        return 0;
    }

    if (frame_clock < sys->next_flip_deadline)
        return 0;

    const vlc_tick_t late = frame_clock - sys->next_flip_deadline;
    uint64_t steps = 1 + (uint64_t)(late / period);

    if (steps > UINT32_MAX)
        steps = UINT32_MAX;

    if (steps & 1)
        sys->show_right_eye = !sys->show_right_eye;

    sys->next_flip_deadline += (vlc_tick_t)(steps * (uint64_t)period);
    return (unsigned)steps;
}

static void Open3DMaybeLogStatus(vout_display_t *vd, vout_display_sys_t *sys,
                                 open3d_pack_t pack_mode, bool show_right_eye,
                                 vlc_tick_t frame_clock, unsigned flips,
                                 const video_format_t *eye_source)
{
    if (!sys->debug_status)
        return;

    const bool pack_changed = pack_mode != sys->last_logged_pack;
    const bool eye_changed = show_right_eye != sys->last_logged_eye;
    const bool late = flips > 1;
    const bool periodic = (sys->frame_count % 120) == 0;

    if (!(pack_changed || eye_changed || late || periodic))
        return;

    const uint64_t presenter_ticks = sys->presenter_tick_count;
    const uint64_t avg_render_us = presenter_ticks > 0
                                 ? (sys->presenter_render_total / presenter_ticks)
                                 : 0;
    const uint64_t avg_sleep_overshoot_us = presenter_ticks > 0
                                          ? (sys->presenter_sleep_overshoot_total / presenter_ticks)
                                          : 0;
    const uint64_t profiled_frames = sys->presenter_profiled_frame_count;
    const uint64_t prelude_avg_wall_us = profiled_frames > 0
                                       ? (sys->presenter_prelude_wall_total_ns / profiled_frames) / 1000
                                       : 0;
    const uint64_t prelude_avg_cpu_us = profiled_frames > 0
                                      ? (sys->presenter_prelude_cpu_total_ns / profiled_frames) / 1000
                                      : 0;
    const uint64_t emitter_queue_avg_wall_us = profiled_frames > 0
                                             ? (sys->presenter_emitter_queue_wall_total_ns / profiled_frames) / 1000
                                             : 0;
    const uint64_t emitter_queue_avg_cpu_us = profiled_frames > 0
                                            ? (sys->presenter_emitter_queue_cpu_total_ns / profiled_frames) / 1000
                                            : 0;
    const uint64_t status_log_avg_wall_us = profiled_frames > 0
                                          ? (sys->presenter_status_log_wall_total_ns / profiled_frames) / 1000
                                          : 0;
    const uint64_t status_log_avg_cpu_us = profiled_frames > 0
                                         ? (sys->presenter_status_log_cpu_total_ns / profiled_frames) / 1000
                                         : 0;
    const uint64_t compose_avg_wall_us = profiled_frames > 0
                                       ? (sys->presenter_compose_wall_total_ns / profiled_frames) / 1000
                                       : 0;
    const uint64_t compose_avg_cpu_us = profiled_frames > 0
                                      ? (sys->presenter_compose_cpu_total_ns / profiled_frames) / 1000
                                      : 0;
    const uint64_t gl_lock_avg_wall_us = profiled_frames > 0
                                       ? (sys->presenter_gl_lock_wall_total_ns / profiled_frames) / 1000
                                       : 0;
    const uint64_t gl_lock_avg_cpu_us = profiled_frames > 0
                                      ? (sys->presenter_gl_lock_cpu_total_ns / profiled_frames) / 1000
                                      : 0;
    const uint64_t make_current_avg_wall_us = profiled_frames > 0
                                            ? (sys->presenter_make_current_wall_total_ns / profiled_frames) / 1000
                                            : 0;
    const uint64_t make_current_avg_cpu_us = profiled_frames > 0
                                           ? (sys->presenter_make_current_cpu_total_ns / profiled_frames) / 1000
                                           : 0;
    const uint64_t prepare_avg_wall_us = profiled_frames > 0
                                       ? (sys->presenter_prepare_wall_total_ns / profiled_frames) / 1000
                                       : 0;
    const uint64_t prepare_avg_cpu_us = profiled_frames > 0
                                      ? (sys->presenter_prepare_cpu_total_ns / profiled_frames) / 1000
                                      : 0;
    const uint64_t display_avg_wall_us = profiled_frames > 0
                                       ? (sys->presenter_display_wall_total_ns / profiled_frames) / 1000
                                       : 0;
    const uint64_t display_avg_cpu_us = profiled_frames > 0
                                      ? (sys->presenter_display_cpu_total_ns / profiled_frames) / 1000
                                      : 0;
    const uint64_t swap_avg_wall_us = profiled_frames > 0
                                    ? (sys->presenter_swap_wall_total_ns / profiled_frames) / 1000
                                    : 0;
    const uint64_t swap_avg_cpu_us = profiled_frames > 0
                                   ? (sys->presenter_swap_cpu_total_ns / profiled_frames) / 1000
                                   : 0;
    const uint64_t cleanup_avg_wall_us = profiled_frames > 0
                                       ? (sys->presenter_cleanup_wall_total_ns / profiled_frames) / 1000
                                       : 0;
    const uint64_t cleanup_avg_cpu_us = profiled_frames > 0
                                      ? (sys->presenter_cleanup_cpu_total_ns / profiled_frames) / 1000
                                      : 0;

    if (eye_source != NULL)
    {
        msg_Dbg(vd,
                "open3d state frame=%" PRIu64 " pack=%s eye=%s flips=%u late_events=%" PRIu64 " late_steps=%" PRIu64 " clock=%" PRId64 " eye=%ux%u off=%u,%u sar=%u:%u",
                sys->frame_count,
                Open3DPackName(pack_mode),
                show_right_eye ? "right" : "left",
                flips,
                sys->late_flip_events,
                sys->late_flip_steps_total,
                frame_clock,
                eye_source->i_visible_width,
                eye_source->i_visible_height,
                eye_source->i_x_offset,
                eye_source->i_y_offset,
                eye_source->i_sar_num,
                eye_source->i_sar_den);
        if (sys->presenter_enable)
        {
            msg_Dbg(vd,
                    "open3d presenter stats ticks=%" PRIu64 " late_events=%" PRIu64 " late_steps=%" PRIu64 " miss_events=%" PRIu64 " miss_steps=%" PRIu64 " miss_max=%" PRId64 "us render_avg=%" PRIu64 "us render_max=%" PRId64 "us sleep_overshoot_avg=%" PRIu64 "us sleep_overshoot_max=%" PRId64 "us rt=%s affinity=%s cpu=%d",
                    presenter_ticks,
                    sys->presenter_late_events,
                    sys->presenter_late_steps_total,
                    sys->presenter_deadline_miss_events,
                    sys->presenter_deadline_miss_steps_total,
                    sys->presenter_deadline_miss_max,
                    avg_render_us,
                    sys->presenter_render_max,
                    avg_sleep_overshoot_us,
                    sys->presenter_sleep_overshoot_max,
                    sys->presenter_rt_enabled ? "on" : "off",
                    sys->presenter_affinity_enabled ? "on" : "off",
                    sys->presenter_affinity_cpu);
            if (profiled_frames > 0)
                msg_Dbg(vd,
                        "open3d presenter outer stage stats profiled=%" PRIu64 " prelude_avg=%" PRIu64 "us prelude_cpu_avg=%" PRIu64 "us prelude_max=%" PRIu64 "us cleanup_avg=%" PRIu64 "us cleanup_cpu_avg=%" PRIu64 "us cleanup_max=%" PRIu64 "us",
                        profiled_frames,
                        prelude_avg_wall_us,
                        prelude_avg_cpu_us,
                        sys->presenter_prelude_wall_max_ns / 1000,
                        cleanup_avg_wall_us,
                        cleanup_avg_cpu_us,
                        sys->presenter_cleanup_wall_max_ns / 1000);
            if (profiled_frames > 0)
                msg_Dbg(vd,
                        "open3d presenter prelude stage stats profiled=%" PRIu64 " emitter_queue_avg=%" PRIu64 "us emitter_queue_cpu_avg=%" PRIu64 "us emitter_queue_max=%" PRIu64 "us status_log_avg=%" PRIu64 "us status_log_cpu_avg=%" PRIu64 "us status_log_max=%" PRIu64 "us",
                        profiled_frames,
                        emitter_queue_avg_wall_us,
                        emitter_queue_avg_cpu_us,
                        sys->presenter_emitter_queue_wall_max_ns / 1000,
                        status_log_avg_wall_us,
                        status_log_avg_cpu_us,
                        sys->presenter_status_log_wall_max_ns / 1000);
            if (profiled_frames > 0)
                msg_Dbg(vd,
                        "open3d presenter pregl stage stats profiled=%" PRIu64 " compose_avg=%" PRIu64 "us compose_cpu_avg=%" PRIu64 "us compose_max=%" PRIu64 "us gl_lock_avg=%" PRIu64 "us gl_lock_cpu_avg=%" PRIu64 "us gl_lock_max=%" PRIu64 "us make_current_avg=%" PRIu64 "us make_current_cpu_avg=%" PRIu64 "us make_current_max=%" PRIu64 "us",
                        profiled_frames,
                        compose_avg_wall_us,
                        compose_avg_cpu_us,
                        sys->presenter_compose_wall_max_ns / 1000,
                        gl_lock_avg_wall_us,
                        gl_lock_avg_cpu_us,
                        sys->presenter_gl_lock_wall_max_ns / 1000,
                        make_current_avg_wall_us,
                        make_current_avg_cpu_us,
                        sys->presenter_make_current_wall_max_ns / 1000);
            if (profiled_frames > 0)
                msg_Dbg(vd,
                        "open3d presenter stage stats profiled=%" PRIu64 " prepare_avg=%" PRIu64 "us prepare_cpu_avg=%" PRIu64 "us prepare_max=%" PRIu64 "us display_avg=%" PRIu64 "us display_cpu_avg=%" PRIu64 "us display_max=%" PRIu64 "us swap_avg=%" PRIu64 "us swap_cpu_avg=%" PRIu64 "us swap_max=%" PRIu64 "us",
                        profiled_frames,
                        prepare_avg_wall_us,
                        prepare_avg_cpu_us,
                        sys->presenter_prepare_wall_max_ns / 1000,
                        display_avg_wall_us,
                        display_avg_cpu_us,
                        sys->presenter_display_wall_max_ns / 1000,
                        swap_avg_wall_us,
                        swap_avg_cpu_us,
                        sys->presenter_swap_wall_max_ns / 1000);
        }
    }
    else
    {
        msg_Dbg(vd,
                "open3d state frame=%" PRIu64 " pack=%s eye=%s flips=%u late_events=%" PRIu64 " late_steps=%" PRIu64 " clock=%" PRId64,
                sys->frame_count,
                Open3DPackName(pack_mode),
                show_right_eye ? "right" : "left",
                flips,
                sys->late_flip_events,
                sys->late_flip_steps_total,
                frame_clock);
        if (sys->presenter_enable)
        {
            msg_Dbg(vd,
                    "open3d presenter stats ticks=%" PRIu64 " late_events=%" PRIu64 " late_steps=%" PRIu64 " miss_events=%" PRIu64 " miss_steps=%" PRIu64 " miss_max=%" PRId64 "us render_avg=%" PRIu64 "us render_max=%" PRId64 "us sleep_overshoot_avg=%" PRIu64 "us sleep_overshoot_max=%" PRId64 "us rt=%s affinity=%s cpu=%d",
                    presenter_ticks,
                    sys->presenter_late_events,
                    sys->presenter_late_steps_total,
                    sys->presenter_deadline_miss_events,
                    sys->presenter_deadline_miss_steps_total,
                    sys->presenter_deadline_miss_max,
                    avg_render_us,
                    sys->presenter_render_max,
                    avg_sleep_overshoot_us,
                    sys->presenter_sleep_overshoot_max,
                    sys->presenter_rt_enabled ? "on" : "off",
                    sys->presenter_affinity_enabled ? "on" : "off",
                    sys->presenter_affinity_cpu);
            if (profiled_frames > 0)
                msg_Dbg(vd,
                        "open3d presenter outer stage stats profiled=%" PRIu64 " prelude_avg=%" PRIu64 "us prelude_cpu_avg=%" PRIu64 "us prelude_max=%" PRIu64 "us cleanup_avg=%" PRIu64 "us cleanup_cpu_avg=%" PRIu64 "us cleanup_max=%" PRIu64 "us",
                        profiled_frames,
                        prelude_avg_wall_us,
                        prelude_avg_cpu_us,
                        sys->presenter_prelude_wall_max_ns / 1000,
                        cleanup_avg_wall_us,
                        cleanup_avg_cpu_us,
                        sys->presenter_cleanup_wall_max_ns / 1000);
            if (profiled_frames > 0)
                msg_Dbg(vd,
                        "open3d presenter prelude stage stats profiled=%" PRIu64 " emitter_queue_avg=%" PRIu64 "us emitter_queue_cpu_avg=%" PRIu64 "us emitter_queue_max=%" PRIu64 "us status_log_avg=%" PRIu64 "us status_log_cpu_avg=%" PRIu64 "us status_log_max=%" PRIu64 "us",
                        profiled_frames,
                        emitter_queue_avg_wall_us,
                        emitter_queue_avg_cpu_us,
                        sys->presenter_emitter_queue_wall_max_ns / 1000,
                        status_log_avg_wall_us,
                        status_log_avg_cpu_us,
                        sys->presenter_status_log_wall_max_ns / 1000);
            if (profiled_frames > 0)
                msg_Dbg(vd,
                        "open3d presenter pregl stage stats profiled=%" PRIu64 " compose_avg=%" PRIu64 "us compose_cpu_avg=%" PRIu64 "us compose_max=%" PRIu64 "us gl_lock_avg=%" PRIu64 "us gl_lock_cpu_avg=%" PRIu64 "us gl_lock_max=%" PRIu64 "us make_current_avg=%" PRIu64 "us make_current_cpu_avg=%" PRIu64 "us make_current_max=%" PRIu64 "us",
                        profiled_frames,
                        compose_avg_wall_us,
                        compose_avg_cpu_us,
                        sys->presenter_compose_wall_max_ns / 1000,
                        gl_lock_avg_wall_us,
                        gl_lock_avg_cpu_us,
                        sys->presenter_gl_lock_wall_max_ns / 1000,
                        make_current_avg_wall_us,
                        make_current_avg_cpu_us,
                        sys->presenter_make_current_wall_max_ns / 1000);
            if (profiled_frames > 0)
                msg_Dbg(vd,
                        "open3d presenter stage stats profiled=%" PRIu64 " prepare_avg=%" PRIu64 "us prepare_cpu_avg=%" PRIu64 "us prepare_max=%" PRIu64 "us display_avg=%" PRIu64 "us display_cpu_avg=%" PRIu64 "us display_max=%" PRIu64 "us swap_avg=%" PRIu64 "us swap_cpu_avg=%" PRIu64 "us swap_max=%" PRIu64 "us",
                        profiled_frames,
                        prepare_avg_wall_us,
                        prepare_avg_cpu_us,
                        sys->presenter_prepare_wall_max_ns / 1000,
                        display_avg_wall_us,
                        display_avg_cpu_us,
                        sys->presenter_display_wall_max_ns / 1000,
                        swap_avg_wall_us,
                        swap_avg_cpu_us,
                        sys->presenter_swap_wall_max_ns / 1000);
        }
    }

    sys->last_logged_pack = pack_mode;
    sys->last_logged_eye = show_right_eye;
}

static bool Open3DShouldLogPresenterEvents(const vout_display_sys_t *sys)
{
    return sys != NULL &&
           (sys->debug_status || sys->presenter_telemetry || sys->presenter_stage_profile);
}

static void Open3DMaybeLogPresenterTelemetry(vout_display_t *vd, vout_display_sys_t *sys,
                                             open3d_pack_t pack_mode, bool show_right_eye,
                                             vlc_tick_t frame_clock, unsigned flips)
{
    if (!sys->presenter_telemetry)
        return;

    const unsigned interval = sys->presenter_telemetry_interval > 0
                            ? (unsigned)sys->presenter_telemetry_interval
                            : 120U;
    const bool periodic = (sys->frame_count % interval) == 0;
    const bool late = flips > 1;
    const bool first = sys->frame_count == 0;

    if (!(periodic || late || first))
        return;

    const uint64_t presenter_ticks = sys->presenter_tick_count;
    const uint64_t avg_render_us = presenter_ticks > 0
                                 ? (sys->presenter_render_total / presenter_ticks)
                                 : 0;
    const uint64_t avg_sleep_overshoot_us = presenter_ticks > 0
                                          ? (sys->presenter_sleep_overshoot_total / presenter_ticks)
                                          : 0;

    msg_Dbg(vd,
            "open3d presenter telemetry frame=%" PRIu64 " pack=%s eye=%s flips=%u clock=%" PRId64
            " ticks=%" PRIu64 " miss_events=%" PRIu64 " over_budget=%" PRIu64
            " render_avg=%" PRIu64 "us sleep_overshoot_avg=%" PRIu64 "us rt=%s affinity=%s cpu=%d",
            sys->frame_count,
            Open3DPackName(pack_mode),
            show_right_eye ? "right" : "left",
            flips,
            frame_clock,
            presenter_ticks,
            sys->presenter_deadline_miss_events,
            sys->presenter_render_over_budget_events,
            avg_render_us,
            avg_sleep_overshoot_us,
            sys->presenter_rt_enabled ? "on" : "off",
            sys->presenter_affinity_enabled ? "on" : "off",
            sys->presenter_affinity_cpu);
}

static void Open3DApplyEyeCrop(const video_format_t *source,
                               open3d_pack_t pack_mode,
                               bool show_right_eye,
                               video_format_t *eye_source)
{
    *eye_source = *source;
    eye_source->multiview_mode = MULTIVIEW_2D;

    if (eye_source->i_sar_num == 0 || eye_source->i_sar_den == 0)
    {
        eye_source->i_sar_num = 1;
        eye_source->i_sar_den = 1;
    }

    if (pack_mode == OPEN3D_PACK_SBS_FULL || pack_mode == OPEN3D_PACK_SBS_HALF)
    {
        const unsigned half = source->i_visible_width / 2;
        if (half > 0)
        {
            eye_source->i_visible_width = half;
            if (source->i_width > 0)
                eye_source->i_width = source->i_width / 2;
            if (show_right_eye)
                eye_source->i_x_offset = source->i_x_offset + half;
        }
    }
    else if (pack_mode == OPEN3D_PACK_TB_FULL || pack_mode == OPEN3D_PACK_TB_HALF)
    {
        const unsigned half = source->i_visible_height / 2;
        if (half > 0)
        {
            eye_source->i_visible_height = half;
            if (source->i_height > 0)
                eye_source->i_height = source->i_height / 2;
            if (show_right_eye)
                eye_source->i_y_offset = source->i_y_offset + half;
        }
    }

    if (pack_mode == OPEN3D_PACK_SBS_HALF)
    {
        eye_source->i_sar_num = Open3DSaturatingMulUnsigned(eye_source->i_sar_num, 2);
    }
    else if (pack_mode == OPEN3D_PACK_TB_HALF)
    {
        eye_source->i_sar_den = Open3DSaturatingMulUnsigned(eye_source->i_sar_den, 2);
    }

    Open3DNormalizeSar(eye_source);
}

static uint8_t Open3DAlphaFromFloat(double alpha)
{
    if (alpha <= 0.0)
        return 0;
    if (alpha >= 1.0)
        return 255;
    return (uint8_t)(alpha * 255.0 + 0.5);
}

static unsigned Open3DScalePx(unsigned value, double scale)
{
    if (value == 0)
        return 0;
    if (scale <= 0.0)
        scale = 1.0;

    unsigned scaled = (unsigned)(value * scale + 0.5);
    if (scaled == 0)
        scaled = 1;
    return scaled;
}

static int Open3DScaleSignedPx(int value, double scale)
{
    if (value == 0)
        return 0;
    if (scale <= 0.0)
        scale = 1.0;

    double scaled = value * scale;
    if (scaled >= 0.0)
        return (int)(scaled + 0.5);
    return (int)(scaled - 0.5);
}

static int Open3DCurrentDriveMode(vout_display_sys_t *sys)
{
    if (!sys->emitter_enable || sys->emitter_fd < 0)
        return 0; /* disconnected behaves like optical in MPC */

    /* When connected, prefer polled device mode to mirror MPC behavior. */
    if (sys->emitter_device_settings_valid)
        return sys->emitter_device_settings.ir_drive_mode == 1 ? 1 : 0;

    /* Before first parameter readback, stay in optical-safe mode. */
    return 0;
}

static void Open3DReleasePreparedSubpicture(vout_display_sys_t *sys)
{
    if (sys->prepared_owned_subpicture != NULL)
    {
        subpicture_Delete(sys->prepared_owned_subpicture);
        sys->prepared_owned_subpicture = NULL;
    }
}

static void Open3DEmitterWake(vout_display_sys_t *sys)
{
    if (sys == NULL || !sys->emitter_started)
        return;

    vlc_mutex_lock(&sys->emitter_control_lock);
    vlc_cond_signal(&sys->emitter_cond);
    vlc_mutex_unlock(&sys->emitter_control_lock);
}

static void Open3DEmitterQueueEye(vout_display_sys_t *sys,
                                  bool show_right_eye,
                                  vlc_tick_t frame_clock)
{
    if (sys == NULL || !sys->emitter_enable || !sys->emitter_started)
        return;
    if (frame_clock == VLC_TICK_INVALID)
        frame_clock = mdate();

    vlc_mutex_lock(&sys->emitter_control_lock);
    sys->emitter_eye_pending = true;
    sys->emitter_eye = show_right_eye;
    sys->emitter_eye_clock = frame_clock;
    vlc_cond_signal(&sys->emitter_cond);
    vlc_mutex_unlock(&sys->emitter_control_lock);
}

static void Open3DEmitterRequestEyeReset(vout_display_sys_t *sys)
{
    if (sys == NULL || !sys->emitter_enable || !sys->emitter_started)
        return;

    vlc_mutex_lock(&sys->emitter_control_lock);
    sys->emitter_eye_reset = true;
    vlc_cond_signal(&sys->emitter_cond);
    vlc_mutex_unlock(&sys->emitter_control_lock);
}

static void Open3DControlWake(vout_display_sys_t *sys)
{
    if (sys == NULL || !sys->control_started)
        return;

    vlc_mutex_lock(&sys->control_lock);
    vlc_cond_signal(&sys->control_cond);
    vlc_mutex_unlock(&sys->control_lock);
}

static void Open3DPresenterWake(vout_display_sys_t *sys)
{
    if (sys == NULL || !sys->presenter_started)
        return;
    vlc_mutex_lock(&sys->presenter_lock);
    vlc_cond_signal(&sys->presenter_cond);
    vlc_mutex_unlock(&sys->presenter_lock);
}

static subpicture_t *Open3DCloneSubpicture(const subpicture_t *base_subpicture)
{
    if (base_subpicture == NULL)
        return NULL;

    subpicture_t *copy = subpicture_New(NULL);
    if (copy == NULL)
        return NULL;

    copy->i_channel = base_subpicture->i_channel;
    copy->i_order = base_subpicture->i_order;
    copy->i_start = base_subpicture->i_start;
    copy->i_stop = base_subpicture->i_stop;
    copy->b_ephemer = base_subpicture->b_ephemer;
    copy->b_fade = base_subpicture->b_fade;
    copy->b_subtitle = base_subpicture->b_subtitle;
    copy->b_absolute = base_subpicture->b_absolute;
    copy->i_original_picture_width = base_subpicture->i_original_picture_width;
    copy->i_original_picture_height = base_subpicture->i_original_picture_height;
    copy->i_alpha = base_subpicture->i_alpha;

    if (Open3DCopyRegionChain(&copy->p_region, base_subpicture->p_region) != VLC_SUCCESS)
    {
        subpicture_Delete(copy);
        return NULL;
    }
    return copy;
}

static subpicture_t *Open3DCloneSubpictureShallow(const subpicture_t *base_subpicture)
{
    if (base_subpicture == NULL)
        return NULL;

    subpicture_t *copy = subpicture_New(NULL);
    if (copy == NULL)
        return NULL;

    copy->i_channel = base_subpicture->i_channel;
    copy->i_order = base_subpicture->i_order;
    copy->i_start = base_subpicture->i_start;
    copy->i_stop = base_subpicture->i_stop;
    copy->b_ephemer = base_subpicture->b_ephemer;
    copy->b_fade = base_subpicture->b_fade;
    copy->b_subtitle = base_subpicture->b_subtitle;
    copy->b_absolute = base_subpicture->b_absolute;
    copy->i_original_picture_width = base_subpicture->i_original_picture_width;
    copy->i_original_picture_height = base_subpicture->i_original_picture_height;
    copy->i_alpha = base_subpicture->i_alpha;

    if (Open3DCloneRegionChainShallow(&copy->p_region, base_subpicture->p_region) != VLC_SUCCESS)
    {
        subpicture_Delete(copy);
        return NULL;
    }
    return copy;
}

static vout_thread_t *Open3DGetParentVout(vout_display_t *vd)
{
    if (vd == NULL || vd->obj.parent == NULL)
        return NULL;
    return (vout_thread_t *)vd->obj.parent;
}

static void Open3DSetDirectBluraySubtitleBridge(vout_display_t *vd, bool enabled)
{
    vout_thread_t *p_vout = Open3DGetParentVout(vd);
    Open3DSubtitleBridgeSetEnabledOnObject(VLC_OBJECT(p_vout), enabled);
}

static bool Open3DGetBluraySubtitleOffsetState(vout_display_t *vd,
                                               bool *valid_out,
                                               int *signed_out,
                                               int *seq_out,
                                               int *frame_out)
{
    vout_thread_t *p_vout = Open3DGetParentVout(vd);
    if (valid_out != NULL)
        *valid_out = false;
    if (signed_out != NULL)
        *signed_out = 0;
    if (seq_out != NULL)
        *seq_out = -1;
    if (frame_out != NULL)
        *frame_out = -1;

    if (p_vout == NULL)
        return false;

    open3d_subtitle_depth_state_t depth_state;
    if (!Open3DSubtitleDepthReadFromObjectTree(VLC_OBJECT(p_vout), &depth_state))
    {
        static vlc_tick_t last_missing_log = VLC_TICK_INVALID;
        const vlc_tick_t now = mdate();
        if (last_missing_log == VLC_TICK_INVALID || now - last_missing_log >= CLOCK_FREQ * 2)
        {
            const int valid_type = var_Type(p_vout, OPEN3D_SUBTITLE_OFFSET_VALID_VAR);
            const int signed_type = var_Type(p_vout, OPEN3D_SUBTITLE_OFFSET_SIGNED_VAR);
            const int seq_type = var_Type(p_vout, OPEN3D_SUBTITLE_OFFSET_SEQ_VAR);
            msg_Dbg(vd,
                    "open3d subtitle offset vars missing on vout tree valid=%d signed=%d seq=%d",
                    valid_type, signed_type, seq_type);
            last_missing_log = now;
        }
        return false;
    }
    if (valid_out != NULL)
        *valid_out = depth_state.valid;
    if (signed_out != NULL)
        *signed_out = depth_state.signed_offset;
    if (seq_out != NULL)
        *seq_out = depth_state.valid ? depth_state.sequence : -1;
    if (frame_out != NULL)
        *frame_out = depth_state.frame_index;

    static vlc_tick_t last_state_log = VLC_TICK_INVALID;
    const vlc_tick_t now = mdate();
    if (last_state_log == VLC_TICK_INVALID || now - last_state_log >= CLOCK_FREQ * 2)
    {
        msg_Dbg(vd,
                "open3d subtitle offset state valid=%d signed=%d seq=%d frame=%d",
                valid_out != NULL ? *valid_out : depth_state.valid,
                signed_out != NULL ? *signed_out : depth_state.signed_offset,
                seq_out != NULL ? *seq_out : (depth_state.valid ? depth_state.sequence : -1),
                frame_out != NULL ? *frame_out : depth_state.frame_index);
        last_state_log = now;
    }
    return true;
}

static void Open3DRecordBluraySubtitleOffsetSample(vout_display_sys_t *sys,
                                                   bool valid,
                                                   int seq,
                                                   int signed_offset,
                                                   int frame_index)
{
    if (sys == NULL)
        return;

    if (!valid || seq < 0)
    {
        sys->bluray_subtitle_offset_last_valid = false;
        return;
    }

    if (sys->bluray_subtitle_offset_last_valid &&
        sys->bluray_subtitle_offset_last_seq == seq &&
        sys->bluray_subtitle_offset_last_signed == signed_offset &&
        sys->bluray_subtitle_offset_last_frame == frame_index)
        return;

    open3d_bluray_subtitle_offset_sample_t *sample =
        &sys->bluray_subtitle_offset_history[sys->bluray_subtitle_offset_history_head];
    sample->valid = true;
    sample->wallclock = mdate();
    sample->seq = seq;
    sample->signed_offset = signed_offset;
    sample->frame_index = frame_index;

    sys->bluray_subtitle_offset_history_head =
        (sys->bluray_subtitle_offset_history_head + 1) %
        OPEN3D_BLURAY_SUBTITLE_OFFSET_HISTORY_CAP;
    sys->bluray_subtitle_offset_last_valid = true;
    sys->bluray_subtitle_offset_last_seq = seq;
    sys->bluray_subtitle_offset_last_signed = signed_offset;
    sys->bluray_subtitle_offset_last_frame = frame_index;
}

static bool Open3DFindRecentBluraySubtitleOffset(vout_display_sys_t *sys,
                                                 int seq,
                                                 vlc_tick_t now,
                                                 int *signed_out,
                                                 int *frame_out,
                                                 vlc_tick_t *age_out)
{
    if (sys == NULL || seq < 0)
        return false;

    for (unsigned i = 0; i < OPEN3D_BLURAY_SUBTITLE_OFFSET_HISTORY_CAP; ++i)
    {
        unsigned idx = (sys->bluray_subtitle_offset_history_head +
                        OPEN3D_BLURAY_SUBTITLE_OFFSET_HISTORY_CAP - 1 - i) %
                       OPEN3D_BLURAY_SUBTITLE_OFFSET_HISTORY_CAP;
        const open3d_bluray_subtitle_offset_sample_t *sample =
            &sys->bluray_subtitle_offset_history[idx];
        if (!sample->valid || sample->seq != seq)
            continue;

        const vlc_tick_t age = now >= sample->wallclock ? now - sample->wallclock : 0;
        if (age > VLC_TICK_FROM_MS(250))
            continue;

        if (signed_out != NULL)
            *signed_out = sample->signed_offset;
        if (frame_out != NULL)
            *frame_out = sample->frame_index;
        if (age_out != NULL)
            *age_out = age;
        return true;
    }

    return false;
}

static bool Open3DResolveBluraySubtitleOffset(vout_display_t *vd,
                                              vout_display_sys_t *sys,
                                              uint64_t content_signature,
                                              bool live_valid,
                                              int live_seq,
                                              int live_signed,
                                              int live_frame,
                                              int *resolved_seq_out,
                                              int *resolved_signed_out)
{
    if (vd == NULL || sys == NULL || !live_valid || live_seq < 0)
    {
        if (sys != NULL)
            sys->bluray_subtitle_offset_latch.valid = false;
        return false;
    }

    Open3DRecordBluraySubtitleOffsetSample(sys, live_valid, live_seq, live_signed, live_frame);

    if (sys->bluray_subtitle_offset_latch.valid &&
        sys->bluray_subtitle_offset_latch.content_signature == content_signature &&
        sys->bluray_subtitle_offset_latch.seq == live_seq)
    {
        if (resolved_seq_out != NULL)
            *resolved_seq_out = sys->bluray_subtitle_offset_latch.seq;
        if (resolved_signed_out != NULL)
            *resolved_signed_out = sys->bluray_subtitle_offset_latch.signed_offset;
        return true;
    }

    const vlc_tick_t now = mdate();
    int chosen_signed = live_signed;
    int chosen_frame = live_frame;
    vlc_tick_t history_age = VLC_TICK_INVALID;
    const bool from_history =
        Open3DFindRecentBluraySubtitleOffset(sys, live_seq, now,
                                            &chosen_signed, &chosen_frame,
                                            &history_age);

    sys->bluray_subtitle_offset_latch.valid = true;
    sys->bluray_subtitle_offset_latch.content_signature = content_signature;
    sys->bluray_subtitle_offset_latch.seq = live_seq;
    sys->bluray_subtitle_offset_latch.signed_offset = chosen_signed;
    sys->bluray_subtitle_offset_latch.frame_index = chosen_frame;
    sys->bluray_subtitle_offset_latch.latched_at = now;

    if (sys->debug_status)
    {
        msg_Dbg(vd,
                "open3d subtitle offset latch sig=%" PRIx64
                " seq=%d live=%d frame=%d chosen=%d source=%s age_ms=%" PRId64,
                content_signature,
                live_seq, live_signed, live_frame, chosen_signed,
                from_history ? "history" : "live",
                history_age != VLC_TICK_INVALID ? MS_FROM_VLC_TICK(history_age) : INT64_C(-1));
    }

    if (resolved_seq_out != NULL)
        *resolved_seq_out = live_seq;
    if (resolved_signed_out != NULL)
        *resolved_signed_out = chosen_signed;
    return true;
}

static subpicture_t *Open3DCloneDirectBluraySubtitleSubpicture(vout_display_t *vd)
{
    vout_thread_t *p_vout = Open3DGetParentVout(vd);
    if (p_vout == NULL)
        return NULL;

    open3d_subtitle_bridge_t *bridge =
        Open3DSubtitleBridgeGetFromObject(VLC_OBJECT(p_vout));
    if (bridge == NULL)
        return NULL;

    return Open3DSubtitleBridgeCloneSubpicture(bridge);
}

static subpicture_t *Open3DMergeSubpictures(const subpicture_t *base_subpicture,
                                            const subpicture_t *extra_subpicture)
{
    if (base_subpicture == NULL && extra_subpicture == NULL)
        return NULL;

    subpicture_t *merged = subpicture_New(NULL);
    if (merged == NULL)
        return NULL;

    const subpicture_t *seed = base_subpicture != NULL ? base_subpicture
                                                       : extra_subpicture;
    merged->i_channel = seed->i_channel;
    merged->i_order = seed->i_order;
    merged->i_start = seed->i_start;
    merged->i_stop = seed->i_stop;
    merged->b_ephemer = seed->b_ephemer;
    merged->b_fade = seed->b_fade;
    merged->b_subtitle = (base_subpicture != NULL && base_subpicture->b_subtitle) ||
                         (extra_subpicture != NULL && extra_subpicture->b_subtitle);
    merged->b_absolute = seed->b_absolute;
    merged->i_original_picture_width = seed->i_original_picture_width;
    merged->i_original_picture_height = seed->i_original_picture_height;
    merged->i_alpha = seed->i_alpha;

    if (base_subpicture != NULL &&
        Open3DCopyRegionChain(&merged->p_region, base_subpicture->p_region) != VLC_SUCCESS)
    {
        subpicture_Delete(merged);
        return NULL;
    }

    if (extra_subpicture != NULL)
    {
        subpicture_region_t *extra_chain = NULL;
        if (Open3DCopyRegionChain(&extra_chain, extra_subpicture->p_region) != VLC_SUCCESS)
        {
            subpicture_Delete(merged);
            return NULL;
        }
        Open3DRegionChainAppend(&merged->p_region, extra_chain);

        if (merged->i_original_picture_width <= 0)
            merged->i_original_picture_width = extra_subpicture->i_original_picture_width;
        if (merged->i_original_picture_height <= 0)
            merged->i_original_picture_height = extra_subpicture->i_original_picture_height;
        if (merged->i_start == VLC_TICK_INVALID || merged->i_start == 0)
            merged->i_start = extra_subpicture->i_start;
        if (merged->i_stop == VLC_TICK_INVALID || merged->i_stop == 0)
            merged->i_stop = extra_subpicture->i_stop;
        if (extra_subpicture->b_subtitle)
            merged->b_subtitle = true;
    }

    return merged;
}

static uint64_t Open3DSubtitleCacheHashMix(uint64_t hash, uint64_t value)
{
    hash ^= value;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static uint64_t Open3DSubtitleCacheHashPlaneSamples(uint64_t hash, const plane_t *plane)
{
    if (plane == NULL || plane->p_pixels == NULL || plane->i_pitch <= 0 || plane->i_lines <= 0)
        return Open3DSubtitleCacheHashMix(hash, 0);

    const size_t span = (size_t)plane->i_pitch * (size_t)plane->i_lines;
    hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)plane->i_pitch);
    hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)plane->i_lines);

    const unsigned sample_count = span < 16 ? (unsigned)span : 16u;
    for (unsigned i = 0; i < sample_count; ++i)
    {
        const size_t pos = sample_count == 1
                         ? 0
                         : ((span - 1) * (size_t)i) / (size_t)(sample_count - 1);
        hash = Open3DSubtitleCacheHashMix(hash, plane->p_pixels[pos]);
    }

    return hash;
}

static uint64_t Open3DSubtitleCacheSignature(const subpicture_t *subpicture)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    if (subpicture == NULL)
        return 0;

    hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)subpicture->i_channel);
    hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)subpicture->i_order);
    hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)subpicture->i_original_picture_width);
    hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)subpicture->i_original_picture_height);
    hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)subpicture->i_alpha);
    hash = Open3DSubtitleCacheHashMix(hash, subpicture->b_absolute ? 1u : 0u);
    hash = Open3DSubtitleCacheHashMix(hash, subpicture->b_subtitle ? 1u : 0u);

    for (const subpicture_region_t *region = subpicture->p_region; region != NULL; region = region->p_next)
    {
        hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)region->i_x);
        hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)region->i_y);
        hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)region->i_align);
        hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)region->i_alpha);
        hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)region->fmt.i_visible_width);
        hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)region->fmt.i_visible_height);
        hash = Open3DSubtitleCacheHashMix(hash, region->p_picture != NULL ? 1u : 0u);
        hash = Open3DSubtitleCacheHashMix(hash, region->p_text != NULL ? 1u : 0u);

        if (region->p_picture != NULL)
        {
            hash = Open3DSubtitleCacheHashMix(hash, (uint64_t)(uint32_t)region->p_picture->i_planes);
            for (int i = 0; i < region->p_picture->i_planes; ++i)
                hash = Open3DSubtitleCacheHashPlaneSamples(hash, &region->p_picture->p[i]);
        }
    }

    return hash;
}

static subpicture_t *Open3DCloneBluraySubtitleFallback(vout_display_t *vd,
                                                       const subpicture_t *base_subpicture)
{
    if (vd == NULL || base_subpicture == NULL)
        return NULL;
    vout_display_sys_t *sys = vd->sys;
    if (sys == NULL)
        return NULL;
    sys->debug_bluray_subtitle_active = false;
    sys->debug_bluray_subtitle_cache_hit = false;
    if (base_subpicture->b_subtitle)
    {
        static vlc_tick_t last_bsubtitle_log = VLC_TICK_INVALID;
        const vlc_tick_t now = mdate();
        if (last_bsubtitle_log == VLC_TICK_INVALID || now - last_bsubtitle_log >= CLOCK_FREQ * 2)
        {
            msg_Dbg(vd, "open3d fallback skipped because incoming subpicture already has b_subtitle=1");
            last_bsubtitle_log = now;
        }
        return NULL;
    }
    if (base_subpicture->p_region == NULL)
    {
        static vlc_tick_t last_regionless_log = VLC_TICK_INVALID;
        const vlc_tick_t now = mdate();
        if (last_regionless_log == VLC_TICK_INVALID || now - last_regionless_log >= CLOCK_FREQ * 2)
        {
            msg_Dbg(vd, "open3d fallback skipped because incoming subpicture has no regions");
            last_regionless_log = now;
        }
        return NULL;
    }
    unsigned packed_width = vd->source.i_visible_width > 0
                          ? vd->source.i_visible_width
                          : vd->source.i_width;
    unsigned packed_height = vd->source.i_visible_height > 0
                           ? vd->source.i_visible_height
                           : vd->source.i_height;
    open3d_pack_t pack_mode = Open3DDetectPackMode(vd, sys);
    const uint64_t content_signature = Open3DSubtitleCacheSignature(base_subpicture);
    bool offset_valid = false;
    int live_offset_units = 0;
    int offset_units = 0;
    int live_offset_seq = -1;
    int offset_seq = -1;
    int live_offset_frame = -1;
    if (!Open3DGetBluraySubtitleOffsetState(vd, &offset_valid,
                                            &live_offset_units,
                                            &live_offset_seq,
                                            &live_offset_frame) ||
        !Open3DResolveBluraySubtitleOffset(vd, sys, content_signature,
                                           offset_valid,
                                           live_offset_seq,
                                           live_offset_units,
                                           live_offset_frame,
                                           &offset_seq,
                                           &offset_units))
    {
        static vlc_tick_t last_no_offset_log = VLC_TICK_INVALID;
        const vlc_tick_t now = mdate();
        if (last_no_offset_log == VLC_TICK_INVALID || now - last_no_offset_log >= CLOCK_FREQ * 2)
        {
            msg_Dbg(vd,
                    "open3d fallback skipped because subtitle offset state unavailable seq=%d",
                    live_offset_seq);
            last_no_offset_log = now;
        }
        return NULL;
    }
    uint64_t signature = content_signature;
    signature = Open3DSubtitleCacheHashMix(signature, (uint64_t)(uint32_t)offset_seq);
    signature = Open3DSubtitleCacheHashMix(signature, (uint64_t)(uint32_t)(uint8_t)offset_units);
    signature = Open3DSubtitleCacheHashMix(signature, (uint64_t)(uint32_t)pack_mode);
    signature = Open3DSubtitleCacheHashMix(signature, (uint64_t)packed_width);
    signature = Open3DSubtitleCacheHashMix(signature, (uint64_t)packed_height);
    signature = Open3DSubtitleCacheHashMix(signature, sys->subtitle_depth_enable ? 1u : 0u);
    signature = Open3DSubtitleCacheHashMix(signature, (uint64_t)(uint32_t)sys->subtitle_depth_unit_px);

    open3d_bluray_subtitle_cache_t *cache = &sys->bluray_subtitle_cache;
    const bool cache_hit = cache->valid &&
                           cache->signature == signature &&
                           cache->pack_mode == pack_mode &&
                           cache->packed_width == packed_width &&
                           cache->packed_height == packed_height &&
                           cache->subtitle_depth_enable == sys->subtitle_depth_enable &&
                           cache->subtitle_depth_unit_px == sys->subtitle_depth_unit_px &&
                           cache->offset_signed == offset_units &&
                           cache->offset_seq == offset_seq;
    const vlc_tick_t now = mdate();

    if (!cache_hit)
    {
        const bool depth_refresh = cache->valid &&
                                   sys->debug_bluray_subtitle_signature == content_signature;
        const int old_offset = cache->offset_signed;
        const int old_seq = cache->offset_seq;
        Open3DBluraySubtitleCacheClear(cache);
        cache->signature = signature;
        cache->pack_mode = pack_mode;
        cache->packed_width = packed_width;
        cache->packed_height = packed_height;
        cache->subtitle_depth_enable = sys->subtitle_depth_enable;
        cache->subtitle_depth_unit_px = sys->subtitle_depth_unit_px;
        cache->offset_signed = offset_units;
        cache->offset_seq = offset_seq;

        subpicture_t *left = Open3DCloneSubpictureShallow(base_subpicture);
        subpicture_t *right = Open3DCloneSubpictureShallow(base_subpicture);
        if (left != NULL)
            left->b_subtitle = true;
        if (right != NULL)
            right->b_subtitle = true;

        if (left == NULL || right == NULL)
        {
            if (left != NULL)
                subpicture_Delete(left);
            if (right != NULL)
                subpicture_Delete(right);
            Open3DBluraySubtitleCacheClear(cache);
            return NULL;
        }

        if (pack_mode != OPEN3D_PACK_NONE)
        {
            unsigned eye_width = packed_width;
            switch (pack_mode)
            {
                case OPEN3D_PACK_SBS_FULL:
                case OPEN3D_PACK_SBS_HALF:
                    eye_width = packed_width / 2;
                    break;
                default:
                    eye_width = packed_width;
                    break;
            }
            Open3DApplySubtitleStereoShift(vd, sys, left, eye_width, false);
            Open3DApplySubtitleStereoShift(vd, sys, right, eye_width, true);

            subpicture_t *packed_source = Open3DCloneSubpictureShallow(base_subpicture);
            if (packed_source != NULL)
            {
                packed_source->b_subtitle = true;
                cache->packed = Open3DBuildPackedStereoSubtitleSubpicture(vd, sys,
                                                                          packed_source,
                                                                          pack_mode);
                subpicture_Delete(packed_source);
            }
        }

        cache->pf_left = left;
        cache->pf_right = right;
        cache->valid = true;
        sys->bluray_subtitle_cache_misses++;
        sys->debug_bluray_subtitle_last_change = now;
        if (sys->debug_status)
        {
            const int64_t pts = base_subpicture->i_start;
            if (depth_refresh)
            {
                msg_Dbg(vd,
                        "open3d subtitle depth refresh sig=%" PRIx64
                        " seq=%d->%d signed=%d->%d pts=%" PRId64 " age_ms=%" PRId64,
                        content_signature, old_seq, offset_seq, old_offset, offset_units,
                        pts,
                        pts != VLC_TICK_INVALID ? MS_FROM_VLC_TICK(now - pts) : INT64_C(-1));
            }
            else
            {
                msg_Dbg(vd,
                        "open3d subtitle cache rebuild sig=%" PRIx64
                        " seq=%d signed=%d pts=%" PRId64 " pack=%d",
                        content_signature, offset_seq, offset_units,
                        pts, (int)pack_mode);
            }
        }
    }
    else
    {
        sys->bluray_subtitle_cache_hits++;
    }

    static vlc_tick_t last_log = VLC_TICK_INVALID;
    if (last_log == VLC_TICK_INVALID || now - last_log >= CLOCK_FREQ * 2)
    {
        int region_count = 0;
        for (const subpicture_region_t *region = base_subpicture->p_region; region != NULL; region = region->p_next)
            region_count++;
        msg_Dbg(vd,
                "open3d forcing Blu-ray subtitle treatment on composed subpicture "
                "regions=%d orig=%dx%d seq=%d",
                region_count,
                base_subpicture->i_original_picture_width,
                base_subpicture->i_original_picture_height,
                offset_seq);
        last_log = now;
    }

    sys->debug_bluray_subtitle_active = true;
    sys->debug_bluray_subtitle_cache_hit = cache_hit;
    sys->debug_bluray_subtitle_offset_seq = offset_seq;
    sys->debug_bluray_subtitle_offset_signed = offset_units;
    sys->debug_bluray_subtitle_signature = content_signature;
    sys->debug_bluray_subtitle_pts = base_subpicture->i_start;

    subpicture_t *result = NULL;
    if (!sys->enabled && pack_mode != OPEN3D_PACK_NONE && cache->packed != NULL)
        result = Open3DCloneSubpictureShallow(cache->packed);
    else if (sys->enabled && cache->pf_right != NULL && cache->pf_left != NULL)
        result = Open3DCloneSubpictureShallow(sys->show_right_eye ? cache->pf_right
                                                                  : cache->pf_left);
    else if (cache->pf_left != NULL)
        result = Open3DCloneSubpictureShallow(cache->pf_left);

    return result;
}

static bool Open3DSubpictureHasBitmapRegions(const subpicture_t *subpicture)
{
    if (subpicture == NULL)
        return false;

    for (const subpicture_region_t *region = subpicture->p_region;
         region != NULL; region = region->p_next)
    {
        if (region->p_picture != NULL)
            return true;
    }
    return false;
}

static bool Open3DResolvePreferredSubtitleOffset(vout_display_t *vd,
                                                 vout_display_sys_t *sys,
                                                 bool *offset_valid_out,
                                                 int *offset_units_out,
                                                 int *offset_seq_out);

static void Open3DRefreshMkvSubtitleSettings(vout_display_t *vd,
                                             vout_display_sys_t *sys)
{
    if (vd == NULL || sys == NULL)
        return;

    bool bridge_present = false;
    bool mkv_subtitle_force = false;
    int mkv_subtitle_static_offset_units = 0;
    int mkv_subtitle_plane = -1;
    int mkv_subtitle_source_id = -1;
    vlc_object_t *bridge_owner =
        vd->obj.libvlc != NULL ? VLC_OBJECT(vd->obj.libvlc) : NULL;
    if (Open3DMkvSubtitleBridgeReadFromObject(bridge_owner, &bridge_present,
                                              &mkv_subtitle_force,
                                              &mkv_subtitle_static_offset_units,
                                              &mkv_subtitle_plane,
                                              &mkv_subtitle_source_id) &&
        bridge_present)
    {
        if (!mkv_subtitle_force)
        {
            mkv_subtitle_static_offset_units = 0;
            mkv_subtitle_plane = -1;
            mkv_subtitle_source_id = -1;
        }
    }
    else
    {
        mkv_subtitle_force = var_InheritBool(vd, "open3d-mkv-subtitle-force");
        mkv_subtitle_static_offset_units =
            var_InheritInteger(vd, "open3d-mkv-subtitle-static-offset-units");
        mkv_subtitle_plane =
            var_InheritInteger(vd, "open3d-mkv-subtitle-plane");
        mkv_subtitle_source_id =
            var_InheritInteger(vd, "open3d-mkv-subtitle-source-id");
    }

    if (mkv_subtitle_force == sys->mkv_subtitle_force &&
        mkv_subtitle_static_offset_units == sys->mkv_subtitle_static_offset_units &&
        mkv_subtitle_plane == sys->mkv_subtitle_plane &&
        mkv_subtitle_source_id == sys->mkv_subtitle_source_id)
        return;

    sys->mkv_subtitle_force = mkv_subtitle_force;
    sys->mkv_subtitle_static_offset_units = mkv_subtitle_static_offset_units;
    sys->mkv_subtitle_plane = mkv_subtitle_plane;
    sys->mkv_subtitle_source_id = mkv_subtitle_source_id;

    msg_Dbg(vd,
            "open3d refreshed MKV subtitle settings force=%d static_units=%d plane=%d source_id=%d via=%s",
            sys->mkv_subtitle_force ? 1 : 0,
            sys->mkv_subtitle_static_offset_units,
            sys->mkv_subtitle_plane,
            sys->mkv_subtitle_source_id,
            bridge_present ? "bridge" : "inherited");
}

static subpicture_t *Open3DCloneMkvSubtitleFallback(vout_display_t *vd,
                                                    const subpicture_t *base_subpicture)
{
    if (vd == NULL || base_subpicture == NULL)
        return NULL;

    vout_display_sys_t *sys = vd->sys;
    Open3DRefreshMkvSubtitleSettings(vd, sys);
    if (sys == NULL || !sys->mkv_subtitle_force)
        return NULL;

    if (base_subpicture->b_subtitle || base_subpicture->p_region == NULL)
        return NULL;

    if (!Open3DSubpictureHasBitmapRegions(base_subpicture))
        return NULL;

    subpicture_t *copy = Open3DCloneSubpictureShallow(base_subpicture);
    if (copy == NULL)
        return NULL;

    copy->b_subtitle = true;
    sys->debug_bluray_subtitle_active = true;
    sys->debug_bluray_subtitle_cache_hit = false;
    bool preferred_offset_valid = false;
    int preferred_offset_units = sys->mkv_subtitle_static_offset_units;
    int preferred_offset_seq = -1;
    Open3DResolvePreferredSubtitleOffset(vd, sys,
                                         &preferred_offset_valid,
                                         &preferred_offset_units,
                                         &preferred_offset_seq);
    sys->debug_bluray_subtitle_offset_seq = preferred_offset_valid ? preferred_offset_seq : -1;
    sys->debug_bluray_subtitle_offset_signed = preferred_offset_units;
    sys->debug_bluray_subtitle_signature = Open3DSubtitleCacheSignature(base_subpicture);
    sys->debug_bluray_subtitle_pts = base_subpicture->i_start;

    static vlc_tick_t last_log = VLC_TICK_INVALID;
    const vlc_tick_t now = mdate();
    if (last_log == VLC_TICK_INVALID || now - last_log >= CLOCK_FREQ * 2)
    {
        int region_count = 0;
        for (const subpicture_region_t *region = base_subpicture->p_region; region != NULL; region = region->p_next)
            region_count++;
        msg_Dbg(vd,
                "open3d forcing MKV subtitle treatment on composed subpicture "
                "regions=%d orig=%dx%d static_units=%d preferred_units=%d seq=%d plane=%d source_id=%d",
                region_count,
                base_subpicture->i_original_picture_width,
                base_subpicture->i_original_picture_height,
                sys->mkv_subtitle_static_offset_units,
                preferred_offset_units,
                preferred_offset_valid ? preferred_offset_seq : -1,
                sys->mkv_subtitle_plane,
                sys->mkv_subtitle_source_id);
        last_log = now;
    }

    return copy;
}

static void Open3DShiftRegionChain(subpicture_region_t *chain, int dx, int dy)
{
    for (subpicture_region_t *region = chain; region != NULL; region = region->p_next)
    {
        region->i_x += dx;
        region->i_y += dy;
    }
}

static bool Open3DResolvePreferredSubtitleOffset(vout_display_t *vd,
                                                 vout_display_sys_t *sys,
                                                 bool *offset_valid_out,
                                                 int *offset_units_out,
                                                 int *offset_seq_out)
{
    if (offset_valid_out != NULL)
        *offset_valid_out = false;
    if (offset_units_out != NULL)
        *offset_units_out = 0;
    if (offset_seq_out != NULL)
        *offset_seq_out = -1;

    if (vd == NULL || sys == NULL)
        return false;

    Open3DRefreshMkvSubtitleSettings(vd, sys);

    if (sys->bluray_subtitle_offset_latch.valid)
    {
        if (offset_valid_out != NULL)
            *offset_valid_out = true;
        if (offset_units_out != NULL)
            *offset_units_out = sys->bluray_subtitle_offset_latch.signed_offset;
        if (offset_seq_out != NULL)
            *offset_seq_out = sys->bluray_subtitle_offset_latch.seq;
        return true;
    }

    bool bridge_valid = false;
    int bridge_units = 0;
    int bridge_seq = -1;
    if (Open3DGetBluraySubtitleOffsetState(vd, &bridge_valid, &bridge_units,
                                           &bridge_seq, NULL) &&
        bridge_valid)
    {
        if (offset_valid_out != NULL)
            *offset_valid_out = true;
        if (offset_units_out != NULL)
            *offset_units_out = bridge_units;
        if (offset_seq_out != NULL)
            *offset_seq_out = bridge_seq;
        return true;
    }

    if (sys->mkv_subtitle_static_offset_units != 0)
    {
        if (offset_valid_out != NULL)
            *offset_valid_out = true;
        if (offset_units_out != NULL)
            *offset_units_out = sys->mkv_subtitle_static_offset_units;
        return true;
    }

    return false;
}

static bool Open3DComputeSubtitleStereoDelta(vout_display_t *vd,
                                             vout_display_sys_t *sys,
                                             unsigned eye_width,
                                             int *delta_px_out)
{
    if (delta_px_out == NULL)
        return false;

    *delta_px_out = 0;

    if (vd == NULL || sys == NULL || !sys->subtitle_depth_enable || eye_width == 0)
        return false;

    bool offset_valid = false;
    int offset_units = 0;
    int offset_seq = -1;
    if (!Open3DResolvePreferredSubtitleOffset(vd, sys,
                                              &offset_valid,
                                              &offset_units,
                                              &offset_seq) ||
        !offset_valid)
        return false;

    sys->debug_bluray_subtitle_offset_seq = offset_seq;
    sys->debug_bluray_subtitle_offset_signed = offset_units;

    if (offset_units == 0 || sys->subtitle_depth_unit_px == 0)
        return false;

    const double width_scale = (double)eye_width / 1920.0;
    const int delta_px = (int)lround((double)offset_units *
                                     (double)sys->subtitle_depth_unit_px *
                                     width_scale);
    if (delta_px == 0)
        return false;

    *delta_px_out = delta_px;
    return true;
}

static void Open3DApplySubtitleStereoShiftToChain(vout_display_t *vd,
                                                  vout_display_sys_t *sys,
                                                  subpicture_region_t *chain,
                                                  unsigned eye_width,
                                                  bool show_right_eye)
{
    int delta_px = 0;
    if (!Open3DComputeSubtitleStereoDelta(vd, sys, eye_width, &delta_px))
        return;

    int eye_shift = show_right_eye ? delta_px / 2 : -delta_px / 2;
    if ((delta_px & 1) && show_right_eye)
        eye_shift += (delta_px > 0) ? 1 : -1;

    Open3DShiftRegionChain(chain, eye_shift, 0);
}

static bool Open3DSubtitleUsesPackedCoordinates(const subpicture_t *subpicture,
                                                open3d_pack_t pack_mode,
                                                unsigned packed_width,
                                                unsigned packed_height)
{
    if (subpicture == NULL || packed_width == 0 || packed_height == 0)
        return false;

    switch (pack_mode)
    {
        case OPEN3D_PACK_SBS_FULL:
        case OPEN3D_PACK_SBS_HALF:
            return subpicture->i_original_picture_width > 0 &&
                   (unsigned)subpicture->i_original_picture_width == packed_width;
        case OPEN3D_PACK_TB_FULL:
        case OPEN3D_PACK_TB_HALF:
            return subpicture->i_original_picture_height > 0 &&
                   (unsigned)subpicture->i_original_picture_height == packed_height;
        default:
            return false;
    }
}

static subpicture_t *Open3DBuildPackedStereoSubtitleSubpicture(vout_display_t *vd,
                                                               vout_display_sys_t *sys,
                                                               const subpicture_t *base_subpicture,
                                                               open3d_pack_t pack_mode)
{
    if (vd == NULL || sys == NULL || base_subpicture == NULL ||
        !base_subpicture->b_subtitle || pack_mode == OPEN3D_PACK_NONE)
        return NULL;

    unsigned packed_width = vd->source.i_visible_width > 0
                          ? vd->source.i_visible_width
                          : vd->source.i_width;
    unsigned packed_height = vd->source.i_visible_height > 0
                           ? vd->source.i_visible_height
                           : vd->source.i_height;
    unsigned eye_width = packed_width;
    unsigned eye_height = packed_height;
    unsigned output_orig_width = packed_width;
    unsigned output_orig_height = packed_height;
    int right_dx = 0;
    int right_dy = 0;
    const bool packed_relative =
        Open3DSubtitleUsesPackedCoordinates(base_subpicture, pack_mode,
                                            packed_width, packed_height);

    switch (pack_mode)
    {
        case OPEN3D_PACK_SBS_FULL:
        case OPEN3D_PACK_SBS_HALF:
            eye_width = packed_width / 2;
            right_dx = (int)eye_width;
            if (packed_relative)
            {
                output_orig_width = packed_width * 2;
                right_dx = (int)packed_width;
            }
            break;
        case OPEN3D_PACK_TB_FULL:
        case OPEN3D_PACK_TB_HALF:
            eye_height = packed_height / 2;
            right_dy = (int)eye_height;
            if (packed_relative)
            {
                output_orig_height = packed_height * 2;
                right_dy = (int)packed_height;
            }
            break;
        default:
            return NULL;
    }

    if (packed_width == 0 || packed_height == 0 || eye_width == 0 || eye_height == 0)
        return NULL;

    subpicture_t *packed = subpicture_New(NULL);
    if (packed == NULL)
        return NULL;

    packed->i_channel = base_subpicture->i_channel;
    packed->i_order = base_subpicture->i_order;
    packed->i_start = base_subpicture->i_start;
    packed->i_stop = base_subpicture->i_stop;
    packed->b_ephemer = base_subpicture->b_ephemer;
    packed->b_fade = base_subpicture->b_fade;
    packed->b_subtitle = base_subpicture->b_subtitle;
    packed->b_absolute = base_subpicture->b_absolute;
    packed->i_original_picture_width = (int)output_orig_width;
    packed->i_original_picture_height = (int)output_orig_height;
    packed->i_alpha = base_subpicture->i_alpha;

    subpicture_region_t *left_chain = NULL;
    subpicture_region_t *right_chain = NULL;

    if (Open3DCloneRegionChainShallow(&left_chain, base_subpicture->p_region) != VLC_SUCCESS ||
        Open3DCloneRegionChainShallow(&right_chain, base_subpicture->p_region) != VLC_SUCCESS)
    {
        subpicture_region_ChainDelete(left_chain);
        subpicture_region_ChainDelete(right_chain);
        subpicture_Delete(packed);
        return NULL;
    }

    Open3DApplySubtitleStereoShiftToChain(vd, sys, left_chain, eye_width, false);
    Open3DShiftRegionChain(right_chain, right_dx, right_dy);
    Open3DApplySubtitleStereoShiftToChain(vd, sys, right_chain, eye_width, true);

    static vlc_tick_t last_pack_log = VLC_TICK_INVALID;
    const vlc_tick_t now = mdate();
    if (last_pack_log == VLC_TICK_INVALID || now - last_pack_log >= CLOCK_FREQ * 2)
    {
        msg_Dbg(vd,
                "open3d packed subtitle normalize packed_relative=%d pack=%d "
                "base_orig=%dx%d out_orig=%ux%u right=(%d,%d)",
                packed_relative ? 1 : 0, (int)pack_mode,
                base_subpicture->i_original_picture_width,
                base_subpicture->i_original_picture_height,
                output_orig_width, output_orig_height,
                right_dx, right_dy);
        last_pack_log = now;
    }

    packed->p_region = left_chain;
    Open3DRegionChainAppend(&packed->p_region, right_chain);
    return packed;
}

static void Open3DPresenterStoreFrame(vout_display_sys_t *sys, picture_t *pic,
                                      const subpicture_t *subpicture)
{
    if (sys == NULL || pic == NULL)
        return;

    /* Presenter ownership only needs retained region/picture references here.
     * Avoid deep-copying subtitle bitmap planes on every stored frame. */
    subpicture_t *sub_copy = Open3DCloneSubpictureShallow(subpicture);
    picture_t *old_pic = NULL;
    subpicture_t *old_sub = NULL;
    vlc_mutex_lock(&sys->presenter_lock);
    old_pic = sys->presenter_picture;
    old_sub = sys->presenter_subpicture;
    sys->presenter_picture = picture_Hold(pic);
    sys->presenter_subpicture = sub_copy;
    sys->presenter_generation++;
    vlc_cond_signal(&sys->presenter_cond);
    vlc_mutex_unlock(&sys->presenter_lock);

    if (old_pic != NULL)
        picture_Release(old_pic);
    if (old_sub != NULL)
        subpicture_Delete(old_sub);
}

static void Open3DPresenterGetFrame(vout_display_sys_t *sys,
                                    uint64_t last_generation,
                                    uint64_t *generation_out,
                                    picture_t **pic_out,
                                    subpicture_t **sub_out)
{
    if (pic_out != NULL)
        *pic_out = NULL;
    if (sub_out != NULL)
        *sub_out = NULL;
    if (sys == NULL)
        return;

    vlc_mutex_lock(&sys->presenter_lock);
    const uint64_t generation = sys->presenter_generation;
    if (generation_out != NULL)
        *generation_out = generation;
    if (pic_out != NULL &&
        generation != last_generation &&
        sys->presenter_picture != NULL)
        *pic_out = picture_Hold(sys->presenter_picture);
    if (sub_out != NULL &&
        generation != last_generation &&
        sys->presenter_picture != NULL &&
        sys->presenter_subpicture != NULL)
        *sub_out = Open3DCloneSubpictureShallow(sys->presenter_subpicture);
    vlc_mutex_unlock(&sys->presenter_lock);
}

static void Open3DPresenterClearFrame(vout_display_sys_t *sys)
{
    picture_t *old_pic = NULL;
    subpicture_t *old_sub = NULL;

    if (sys == NULL)
        return;

    vlc_mutex_lock(&sys->presenter_lock);
    old_pic = sys->presenter_picture;
    old_sub = sys->presenter_subpicture;
    sys->presenter_picture = NULL;
    sys->presenter_subpicture = NULL;
    sys->presenter_generation++;
    vlc_mutex_unlock(&sys->presenter_lock);

    if (old_pic != NULL)
        picture_Release(old_pic);
    if (old_sub != NULL)
        subpicture_Delete(old_sub);
}

static void Open3DRegionChainAppend(subpicture_region_t **head,
                                    subpicture_region_t *region)
{
    if (*head == NULL)
    {
        *head = region;
        return;
    }

    subpicture_region_t *tail = *head;
    while (tail->p_next != NULL)
        tail = tail->p_next;
    tail->p_next = region;
}

static void Open3DRegionChainPrependChain(subpicture_region_t **head,
                                          subpicture_region_t *chain)
{
    if (head == NULL || chain == NULL)
        return;

    subpicture_region_t *tail = chain;
    while (tail->p_next != NULL)
        tail = tail->p_next;
    tail->p_next = *head;
    *head = chain;
}

static void Open3DApplySubtitleStereoShift(vout_display_t *vd,
                                           vout_display_sys_t *sys,
                                           subpicture_t *subpicture,
                                           unsigned base_width,
                                           bool show_right_eye)
{
    if (vd == NULL || sys == NULL || subpicture == NULL ||
        !sys->subtitle_depth_enable || !subpicture->b_subtitle) {
        return;
    }
    Open3DApplySubtitleStereoShiftToChain(vd, sys, subpicture->p_region,
                                          base_width, show_right_eye);
}

static subpicture_region_t *Open3DCreateSolidRegion(unsigned width, unsigned height,
                                                    int x, int y,
                                                    uint8_t r, uint8_t g, uint8_t b,
                                                    uint8_t alpha)
{
    if (width == 0 || height == 0)
        return NULL;

    video_format_t fmt;
    video_format_Init(&fmt, VLC_CODEC_RGBA);
    video_format_Setup(&fmt, VLC_CODEC_RGBA,
                       (int)width, (int)height,
                       (int)width, (int)height,
                       1, 1);

    subpicture_region_t *region = subpicture_region_New(&fmt);
    video_format_Clean(&fmt);
    if (region == NULL)
        return NULL;

    region->i_x = x;
    region->i_y = y;
    region->i_align = 0;
    region->i_alpha = alpha;

    picture_t *pic = region->p_picture;
    if (pic == NULL || pic->i_planes == 0 || pic->p[0].i_pixel_pitch < 4)
    {
        subpicture_region_Delete(region);
        return NULL;
    }

    plane_t *plane = &pic->p[0];
    for (unsigned line = 0; line < height; ++line)
    {
        uint8_t *dst = plane->p_pixels + line * plane->i_pitch;
        for (unsigned col = 0; col < width; ++col)
        {
            uint8_t *px = dst + col * plane->i_pixel_pitch;
            px[0] = r;
            px[1] = g;
            px[2] = b;
            px[3] = 0xFF;
        }
    }

    return region;
}

static subpicture_region_t *Open3DCreateSolidRegionClamped(unsigned canvas_width,
                                                           unsigned canvas_height,
                                                           int x, int y,
                                                           unsigned width, unsigned height,
                                                           uint8_t r, uint8_t g, uint8_t b,
                                                           uint8_t alpha,
                                                           bool *visible)
{
    int64_t x0 = x;
    int64_t y0 = y;
    int64_t x1 = x0 + width;
    int64_t y1 = y0 + height;

    if (x1 <= 0 || y1 <= 0 || x0 >= (int64_t)canvas_width || y0 >= (int64_t)canvas_height)
    {
        *visible = false;
        return NULL;
    }

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (int64_t)canvas_width)
        x1 = canvas_width;
    if (y1 > (int64_t)canvas_height)
        y1 = canvas_height;

    if (x1 <= x0 || y1 <= y0)
    {
        *visible = false;
        return NULL;
    }

    *visible = true;
    return Open3DCreateSolidRegion((unsigned)(x1 - x0), (unsigned)(y1 - y0),
                                   (int)x0, (int)y0, r, g, b, alpha);
}

typedef struct
{
    bool valid;
    int x_primary;
    int x_secondary;
    int y;
    unsigned size_x;
    unsigned size_y;
    unsigned spacing_x;
    unsigned black_border_x;
    unsigned black_border_y;
} open3d_trigger_geometry_t;

static void Open3DComputeTriggerGeometry(vout_display_sys_t *sys,
                                         unsigned base_width, unsigned base_height,
                                         double scale_x, double scale_y,
                                         open3d_trigger_geometry_t *geo)
{
    if (geo == NULL)
        return;
    memset(geo, 0, sizeof(*geo));

    if (sys == NULL || base_width == 0 || base_height == 0)
        return;

    unsigned size_x = Open3DScalePx(sys->trigger_size, scale_x);
    unsigned size_y = Open3DScalePx(sys->trigger_size, scale_y);
    unsigned margin_x = Open3DScalePx(sys->trigger_padding, scale_x);
    unsigned margin_y = Open3DScalePx(sys->trigger_padding, scale_y);
    unsigned spacing_x = Open3DScalePx(sys->trigger_spacing, scale_x);
    unsigned black_border_x = Open3DScalePx(sys->trigger_black_border, scale_x);
    unsigned black_border_y = Open3DScalePx(sys->trigger_black_border, scale_y);
    int offset_x = Open3DScaleSignedPx(sys->trigger_offset_x, scale_x);
    int offset_y = Open3DScaleSignedPx(sys->trigger_offset_y, scale_y);

    if (size_x > base_width)
        size_x = base_width;
    if (size_y > base_height)
        size_y = base_height;
    if (size_x == 0 || size_y == 0)
        return;

    int x_primary;
    int y_primary;
    int direction = 1;

    switch (sys->trigger_corner)
    {
        case OPEN3D_CORNER_TOP_RIGHT:
            x_primary = (int)base_width - (int)margin_x - (int)size_x;
            y_primary = (int)margin_y;
            direction = -1;
            break;
        case OPEN3D_CORNER_BOTTOM_LEFT:
            x_primary = (int)margin_x;
            y_primary = (int)base_height - (int)margin_y - (int)size_y;
            direction = 1;
            break;
        case OPEN3D_CORNER_BOTTOM_RIGHT:
            x_primary = (int)base_width - (int)margin_x - (int)size_x;
            y_primary = (int)base_height - (int)margin_y - (int)size_y;
            direction = -1;
            break;
        case OPEN3D_CORNER_TOP_LEFT:
        default:
            x_primary = (int)margin_x;
            y_primary = (int)margin_y;
            direction = 1;
            break;
    }

    x_primary += offset_x;
    y_primary += offset_y;

    if (x_primary < 0)
        x_primary = 0;
    if (y_primary < 0)
        y_primary = 0;
    if ((unsigned)x_primary + size_x > base_width)
        x_primary = base_width > size_x ? (int)(base_width - size_x) : 0;
    if ((unsigned)y_primary + size_y > base_height)
        y_primary = base_height > size_y ? (int)(base_height - size_y) : 0;

    const int step = (int)size_x + (int)spacing_x;
    int x_secondary = x_primary + direction * step;
    if (x_secondary < 0 || (unsigned)x_secondary + size_x > base_width)
    {
        const int opposite = x_primary - direction * step;
        if (opposite >= 0 && (unsigned)opposite + size_x <= base_width)
            x_secondary = opposite;
    }
    if (x_secondary < 0)
        x_secondary = 0;
    if ((unsigned)x_secondary + size_x > base_width)
        x_secondary = base_width > size_x ? (int)(base_width - size_x) : 0;

    geo->valid = true;
    geo->x_primary = x_primary;
    geo->x_secondary = x_secondary;
    geo->y = y_primary;
    geo->size_x = size_x;
    geo->size_y = size_y;
    geo->spacing_x = spacing_x;
    geo->black_border_x = black_border_x;
    geo->black_border_y = black_border_y;
}

static int Open3DCopyRegionChain(subpicture_region_t **dst_head,
                                 const subpicture_region_t *src_head)
{
    for (const subpicture_region_t *src = src_head; src != NULL; src = src->p_next)
    {
        subpicture_region_t *copy = subpicture_region_New(&src->fmt);
        if (copy == NULL)
            return VLC_ENOMEM;

        copy->i_x = src->i_x;
        copy->i_y = src->i_y;
        copy->i_align = src->i_align;
        copy->i_alpha = src->i_alpha;
        copy->i_text_align = src->i_text_align;
        copy->b_noregionbg = src->b_noregionbg;
        copy->b_gridmode = src->b_gridmode;
        copy->b_balanced_text = src->b_balanced_text;
        copy->i_max_width = src->i_max_width;
        copy->i_max_height = src->i_max_height;

        if (src->p_text != NULL)
        {
            copy->p_text = text_segment_Copy(src->p_text);
            if (copy->p_text == NULL)
            {
                subpicture_region_Delete(copy);
                return VLC_ENOMEM;
            }
        }

        if (src->p_picture != NULL && copy->p_picture != NULL)
        {
            const int planes = src->p_picture->i_planes;
            for (int i = 0; i < planes; ++i)
            {
                const int lines = src->p_picture->p[i].i_lines;
                const int pitch = src->p_picture->p[i].i_pitch;
                const int max_lines = copy->p_picture->p[i].i_lines;
                const int max_pitch = copy->p_picture->p[i].i_pitch;
                const int copy_lines = lines < max_lines ? lines : max_lines;
                const int copy_pitch = pitch < max_pitch ? pitch : max_pitch;

                if (copy_lines > 0 && copy_pitch > 0)
                {
                    for (int l = 0; l < copy_lines; ++l)
                    {
                        memcpy(copy->p_picture->p[i].p_pixels + l * copy->p_picture->p[i].i_pitch,
                               src->p_picture->p[i].p_pixels + l * src->p_picture->p[i].i_pitch,
                               (size_t)copy_pitch);
                    }
                }
            }
        }
        Open3DRegionChainAppend(dst_head, copy);
    }
    return VLC_SUCCESS;
}

static int Open3DCloneRegionChainShallow(subpicture_region_t **dst_head,
                                         const subpicture_region_t *src_head)
{
    for (const subpicture_region_t *src = src_head; src != NULL; src = src->p_next)
    {
        subpicture_region_t *copy = Open3DCloneRegionShallow(src);
        if (copy == NULL)
            return VLC_ENOMEM;
        Open3DRegionChainAppend(dst_head, copy);
    }
    return VLC_SUCCESS;
}

static void Open3DTriggerRegionCacheClear(open3d_trigger_region_cache_t *cache)
{
    if (cache == NULL)
        return;

    for (size_t i = 0; i < ARRAY_SIZE(cache->pattern_regions); ++i)
    {
        if (cache->pattern_regions[i] != NULL)
        {
            subpicture_region_Delete(cache->pattern_regions[i]);
            cache->pattern_regions[i] = NULL;
        }
    }
    cache->valid = false;
}

static void Open3DCalibrationRegionCacheClear(open3d_calibration_region_cache_t *cache)
{
    if (cache == NULL)
        return;
    if (cache->region_chain != NULL)
    {
        subpicture_region_Delete(cache->region_chain);
        cache->region_chain = NULL;
    }
    cache->valid = false;
}

static int Open3DBuildTriggerPatternTemplate(unsigned canvas_width,
                                             unsigned canvas_height,
                                             const open3d_trigger_geometry_t *geo,
                                             uint8_t alpha,
                                             uint8_t brightness,
                                             bool primary_white,
                                             subpicture_region_t **out_head)
{
    if (out_head == NULL || geo == NULL)
        return VLC_EGENERIC;
    *out_head = NULL;

    bool region_visible = false;
    subpicture_region_t *head = NULL;

    if (geo->black_border_x > 0 || geo->black_border_y > 0)
    {
        const int x_left = (geo->x_primary < geo->x_secondary)
                           ? geo->x_primary : geo->x_secondary;
        const int x_right = (geo->x_primary > geo->x_secondary)
                            ? geo->x_primary : geo->x_secondary;

        subpicture_region_t *border_union =
            Open3DCreateSolidRegionClamped(canvas_width, canvas_height,
                                           x_left - (int)geo->black_border_x,
                                           geo->y - (int)geo->black_border_y,
                                           (unsigned)(x_right - x_left) +
                                               geo->size_x + 2 * geo->black_border_x,
                                           geo->size_y + 2 * geo->black_border_y,
                                           0x00, 0x00, 0x00,
                                           alpha,
                                           &region_visible);
        if (region_visible)
        {
            if (border_union == NULL)
                goto error;
            Open3DRegionChainAppend(&head, border_union);
        }
    }

    const uint8_t primary_level = primary_white ? brightness : 0x00;
    const uint8_t secondary_level = primary_white ? 0x00 : brightness;

    subpicture_region_t *primary =
        Open3DCreateSolidRegionClamped(canvas_width, canvas_height,
                                       geo->x_primary, geo->y,
                                       geo->size_x, geo->size_y,
                                       primary_level, primary_level, primary_level,
                                       alpha,
                                       &region_visible);
    if (region_visible)
    {
        if (primary == NULL)
            goto error;
        Open3DRegionChainAppend(&head, primary);
    }

    subpicture_region_t *secondary =
        Open3DCreateSolidRegionClamped(canvas_width, canvas_height,
                                       geo->x_secondary, geo->y,
                                       geo->size_x, geo->size_y,
                                       secondary_level, secondary_level, secondary_level,
                                       alpha,
                                       &region_visible);
    if (region_visible)
    {
        if (secondary == NULL)
            goto error;
        Open3DRegionChainAppend(&head, secondary);
    }

    *out_head = head;
    return VLC_SUCCESS;

error:
    if (head != NULL)
        subpicture_region_Delete(head);
    return VLC_ENOMEM;
}

static int Open3DEnsureTriggerRegionCache(vout_display_sys_t *sys,
                                          unsigned canvas_width,
                                          unsigned canvas_height,
                                          const open3d_trigger_geometry_t *geo)
{
    if (sys == NULL || geo == NULL)
        return VLC_EGENERIC;

    open3d_trigger_region_cache_t *cache = &sys->trigger_region_cache;
    const bool key_match = cache->valid &&
                           cache->canvas_width == canvas_width &&
                           cache->canvas_height == canvas_height &&
                           cache->x_primary == geo->x_primary &&
                           cache->x_secondary == geo->x_secondary &&
                           cache->y == geo->y &&
                           cache->size_x == geo->size_x &&
                           cache->size_y == geo->size_y &&
                           cache->black_border_x == geo->black_border_x &&
                           cache->black_border_y == geo->black_border_y &&
                           cache->alpha == sys->trigger_alpha &&
                           cache->brightness == sys->trigger_brightness;
    if (key_match)
        return VLC_SUCCESS;

    Open3DTriggerRegionCacheClear(cache);

    if (Open3DBuildTriggerPatternTemplate(canvas_width, canvas_height, geo,
                                          sys->trigger_alpha, sys->trigger_brightness,
                                          false, &cache->pattern_regions[0]) != VLC_SUCCESS)
        return VLC_ENOMEM;

    if (Open3DBuildTriggerPatternTemplate(canvas_width, canvas_height, geo,
                                          sys->trigger_alpha, sys->trigger_brightness,
                                          true, &cache->pattern_regions[1]) != VLC_SUCCESS)
    {
        Open3DTriggerRegionCacheClear(cache);
        return VLC_ENOMEM;
    }

    cache->canvas_width = canvas_width;
    cache->canvas_height = canvas_height;
    cache->x_primary = geo->x_primary;
    cache->x_secondary = geo->x_secondary;
    cache->y = geo->y;
    cache->size_x = geo->size_x;
    cache->size_y = geo->size_y;
    cache->black_border_x = geo->black_border_x;
    cache->black_border_y = geo->black_border_y;
    cache->alpha = sys->trigger_alpha;
    cache->brightness = sys->trigger_brightness;
    cache->valid = true;
    return VLC_SUCCESS;
}

static int Open3DEnsureCalibrationRegionCache(vout_display_sys_t *sys,
                                              unsigned canvas_width,
                                              unsigned canvas_height,
                                              int area_left,
                                              int area_top,
                                              int area_right,
                                              int area_bottom,
                                              int center_x,
                                              int center_y,
                                              unsigned thick_x,
                                              unsigned thick_y,
                                              bool include_black,
                                              uint8_t alpha)
{
    if (sys == NULL)
        return VLC_EGENERIC;

    open3d_calibration_region_cache_t *cache = &sys->calibration_region_cache;
    const bool key_match = cache->valid &&
                           cache->canvas_width == canvas_width &&
                           cache->canvas_height == canvas_height &&
                           cache->area_left == area_left &&
                           cache->area_top == area_top &&
                           cache->area_right == area_right &&
                           cache->area_bottom == area_bottom &&
                           cache->center_x == center_x &&
                           cache->center_y == center_y &&
                           cache->thick_x == thick_x &&
                           cache->thick_y == thick_y &&
                           cache->include_black == include_black &&
                           cache->alpha == alpha;
    if (key_match)
        return VLC_SUCCESS;

    Open3DCalibrationRegionCacheClear(cache);

    subpicture_region_t *chain = NULL;
    bool region_visible = false;
    const unsigned horiz_w = (unsigned)((area_right - area_left) > 1 ? (area_right - area_left) : 1);
    const unsigned vert_h = (unsigned)((area_bottom - area_top) > 1 ? (area_bottom - area_top) : 1);
    const int horiz_x = area_left;
    const int horiz_y = center_y - (int)(thick_y / 2);
    const int vert_x = center_x - (int)(thick_x / 2);
    const int vert_y = area_top;

    if (include_black)
    {
        subpicture_region_t *black =
            Open3DCreateSolidRegionClamped(canvas_width, canvas_height,
                                           area_left, area_top,
                                           horiz_w, vert_h,
                                           0x00, 0x00, 0x00,
                                           0xFF, &region_visible);
        if (region_visible)
        {
            if (black == NULL)
                goto error;
            Open3DRegionChainAppend(&chain, black);
        }
    }

    subpicture_region_t *horiz =
        Open3DCreateSolidRegionClamped(canvas_width, canvas_height,
                                       horiz_x, horiz_y, horiz_w, thick_y,
                                       0x00, 0xFF, 0x00,
                                       alpha, &region_visible);
    if (region_visible)
    {
        if (horiz == NULL)
            goto error;
        Open3DRegionChainAppend(&chain, horiz);
    }

    subpicture_region_t *vert =
        Open3DCreateSolidRegionClamped(canvas_width, canvas_height,
                                       vert_x, vert_y, thick_x, vert_h,
                                       0x00, 0xFF, 0x00,
                                       alpha, &region_visible);
    if (region_visible)
    {
        if (vert == NULL)
            goto error;
        Open3DRegionChainAppend(&chain, vert);
    }

    cache->canvas_width = canvas_width;
    cache->canvas_height = canvas_height;
    cache->area_left = area_left;
    cache->area_top = area_top;
    cache->area_right = area_right;
    cache->area_bottom = area_bottom;
    cache->center_x = center_x;
    cache->center_y = center_y;
    cache->thick_x = thick_x;
    cache->thick_y = thick_y;
    cache->include_black = include_black;
    cache->alpha = alpha;
    cache->region_chain = chain;
    cache->valid = true;
    return VLC_SUCCESS;

error:
    if (chain != NULL)
        subpicture_region_Delete(chain);
    return VLC_ENOMEM;
}

static void Open3DOverlayRectPush(open3d_overlay_rect_t *rects, size_t max_rects,
                                  size_t *count,
                                  int x, int y, unsigned w, unsigned h,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (rects == NULL || count == NULL || *count >= max_rects || w == 0 || h == 0 || a == 0)
        return;
    open3d_overlay_rect_t *dst = &rects[*count];
    dst->x = x;
    dst->y = y;
    dst->w = w;
    dst->h = h;
    dst->r = r;
    dst->g = g;
    dst->b = b;
    dst->a = a;
    (*count)++;
}

static bool Open3DMapOverlayRectToScissor(const vout_display_place_t *place,
                                          unsigned base_width, unsigned base_height,
                                          unsigned display_height,
                                          const open3d_overlay_rect_t *rect,
                                          int *sx, int *sy, int *sw, int *sh)
{
    if (place == NULL || rect == NULL || sx == NULL || sy == NULL || sw == NULL || sh == NULL)
        return false;
    if (base_width == 0 || base_height == 0 || place->width <= 0 || place->height <= 0)
        return false;

    int64_t rx0 = rect->x;
    int64_t ry0 = rect->y;
    int64_t rx1 = rx0 + rect->w;
    int64_t ry1 = ry0 + rect->h;

    if (rx1 <= 0 || ry1 <= 0 || rx0 >= (int64_t)base_width || ry0 >= (int64_t)base_height)
        return false;

    if (rx0 < 0)
        rx0 = 0;
    if (ry0 < 0)
        ry0 = 0;
    if (rx1 > (int64_t)base_width)
        rx1 = base_width;
    if (ry1 > (int64_t)base_height)
        ry1 = base_height;

    if (rx1 <= rx0 || ry1 <= ry0)
        return false;

    const double sx_scale = (double)place->width / (double)base_width;
    const double sy_scale = (double)place->height / (double)base_height;
    int x0 = place->x + (int)(rx0 * sx_scale + 0.5);
    int x1 = place->x + (int)(rx1 * sx_scale + 0.5);
    int y0 = place->y + (int)(ry0 * sy_scale + 0.5);
    int y1 = place->y + (int)(ry1 * sy_scale + 0.5);
    const int place_right = place->x + (int)place->width;
    const int place_bottom = place->y + (int)place->height;

    if (x1 <= x0 || y1 <= y0)
        return false;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > place_right)
        x1 = place_right;
    if (y1 > place_bottom)
        y1 = place_bottom;
    if (x1 <= x0 || y1 <= y0)
        return false;

    *sx = x0;
    *sw = x1 - x0;
    *sh = y1 - y0;
    *sy = (int)display_height - y1;
    if (*sy < 0)
        *sy = 0;
    return *sw > 0 && *sh > 0;
}

static bool Open3DLoadGpuOverlayApi(vout_display_t *vd, vout_display_sys_t *sys)
{
    if (sys == NULL || sys->gl == NULL)
        return false;

    open3d_gl_overlay_api_t *api = &sys->gl_overlay;
    memset(api, 0, sizeof(*api));

#define OPEN3D_GL_LOAD(field, type, name) \
    api->field = (type)vlc_gl_GetProcAddress(sys->gl, name)
    OPEN3D_GL_LOAD(Enable, open3d_gl_enable_fn, "glEnable");
    OPEN3D_GL_LOAD(Disable, open3d_gl_disable_fn, "glDisable");
    OPEN3D_GL_LOAD(BlendFunc, open3d_gl_blendfunc_fn, "glBlendFunc");
    OPEN3D_GL_LOAD(Scissor, open3d_gl_scissor_fn, "glScissor");
    OPEN3D_GL_LOAD(ClearColor, open3d_gl_clearcolor_fn, "glClearColor");
    OPEN3D_GL_LOAD(Clear, open3d_gl_clear_fn, "glClear");
    OPEN3D_GL_LOAD(GetBooleanv, open3d_gl_getbooleanv_fn, "glGetBooleanv");
    OPEN3D_GL_LOAD(GetFloatv, open3d_gl_getfloatv_fn, "glGetFloatv");
    OPEN3D_GL_LOAD(GetIntegerv, open3d_gl_getintegerv_fn, "glGetIntegerv");
    OPEN3D_GL_LOAD(ColorMask, open3d_gl_colormask_fn, "glColorMask");
#undef OPEN3D_GL_LOAD

    if (api->Enable == NULL || api->Disable == NULL || api->BlendFunc == NULL ||
        api->Scissor == NULL || api->ClearColor == NULL || api->Clear == NULL ||
        api->GetBooleanv == NULL || api->GetFloatv == NULL || api->GetIntegerv == NULL ||
        api->ColorMask == NULL)
    {
        msg_Warn(vd, "open3d gpu overlay path unavailable; using subpicture overlays");
        memset(api, 0, sizeof(*api));
        return false;
    }

    api->ready = true;
    msg_Dbg(vd, "open3d gpu overlay path enabled");
    return true;
}

static void Open3DDrawOverlayRectsGLOffset(vout_display_t *vd, vout_display_sys_t *sys,
                                           const vout_display_place_t *place,
                                           unsigned base_width, unsigned base_height,
                                           const open3d_overlay_rect_t *rects,
                                           size_t rect_count,
                                           int offset_x, int offset_y)
{
    VLC_UNUSED(vd);
    if (sys == NULL || rects == NULL || rect_count == 0 || place == NULL)
        return;
    if (!sys->gpu_overlay_ready || !sys->gl_overlay.ready)
        return;

    open3d_gl_overlay_api_t *api = &sys->gl_overlay;
    unsigned char prev_blend = 0;
    unsigned char prev_scissor = 0;
    unsigned char prev_mask[4] = { 1, 1, 1, 1 };
    float prev_clear[4] = { 0.f, 0.f, 0.f, 0.f };
    int prev_scissor_box[4] = { 0, 0, 0, 0 };

    api->GetBooleanv(OPEN3D_GL_BLEND, &prev_blend);
    api->GetBooleanv(OPEN3D_GL_SCISSOR_TEST, &prev_scissor);
    api->GetBooleanv(OPEN3D_GL_COLOR_WRITEMASK, prev_mask);
    api->GetFloatv(OPEN3D_GL_COLOR_CLEAR_VALUE, prev_clear);
    if (prev_scissor)
        api->GetIntegerv(OPEN3D_GL_SCISSOR_BOX, prev_scissor_box);

    api->Enable(OPEN3D_GL_BLEND);
    api->BlendFunc(OPEN3D_GL_SRC_ALPHA, OPEN3D_GL_ONE_MINUS_SRC_ALPHA);
    api->Enable(OPEN3D_GL_SCISSOR_TEST);
    api->ColorMask(1, 1, 1, 1);

    for (size_t i = 0; i < rect_count; ++i)
    {
        int sx = 0, sy = 0, sw = 0, sh = 0;
        open3d_overlay_rect_t rect = rects[i];
        rect.x += offset_x;
        rect.y += offset_y;
        if (!Open3DMapOverlayRectToScissor(place, base_width, base_height,
                                           vd->cfg->display.height, &rect,
                                           &sx, &sy, &sw, &sh))
            continue;

        api->Scissor(sx, sy, sw, sh);
        api->ClearColor(rect.r / 255.0f,
                        rect.g / 255.0f,
                        rect.b / 255.0f,
                        rect.a / 255.0f);
        api->Clear(OPEN3D_GL_COLOR_BUFFER_BIT);
    }

    api->ClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]);
    api->ColorMask(prev_mask[0], prev_mask[1], prev_mask[2], prev_mask[3]);

    if (prev_scissor)
    {
        api->Scissor(prev_scissor_box[0], prev_scissor_box[1],
                     prev_scissor_box[2], prev_scissor_box[3]);
        api->Enable(OPEN3D_GL_SCISSOR_TEST);
    }
    else
    {
        api->Disable(OPEN3D_GL_SCISSOR_TEST);
    }

    if (prev_blend)
        api->Enable(OPEN3D_GL_BLEND);
    else
        api->Disable(OPEN3D_GL_BLEND);
}

static void Open3DDrawOverlayRectsGL(vout_display_t *vd, vout_display_sys_t *sys,
                                     const vout_display_place_t *place,
                                     unsigned base_width, unsigned base_height,
                                     const open3d_overlay_rect_t *rects,
                                     size_t rect_count)
{
    Open3DDrawOverlayRectsGLOffset(vd, sys, place, base_width, base_height,
                                   rects, rect_count, 0, 0);
}

static bool Open3DTextCacheDimensions(const open3d_text_region_cache_t *cache,
                                      int *w, int *h)
{
    if (cache == NULL || cache->region_template == NULL)
        return false;

    int width = (int)cache->region_template->fmt.i_visible_width;
    int height = (int)cache->region_template->fmt.i_visible_height;
    if (width <= 0)
        width = (int)cache->region_template->fmt.i_width;
    if (height <= 0)
        height = (int)cache->region_template->fmt.i_height;
    if (width <= 0 || height <= 0)
        return false;

    if (w != NULL)
        *w = width;
    if (h != NULL)
        *h = height;
    return true;
}

static void Open3DDrawStatusOverlayGpu(vout_display_t *vd, vout_display_sys_t *sys,
                                       const vout_display_place_t *place,
                                       unsigned base_width, unsigned base_height,
                                       vlc_tick_t now, double pixel_aspect)
{
    if (sys == NULL || place == NULL)
        return;
    if (!Open3DStatusOverlayActive(sys, now))
        return;

    const bool message_active = (sys->status_message_until != VLC_TICK_INVALID &&
                                 now < sys->status_message_until &&
                                 sys->status_message[0] != '\0');
    const bool general_visible = sys->enabled && sys->status_overlay_visible;
    const bool calibration_visible = sys->enabled && sys->calibration_enable;
    const bool dirty = sys->emitter_enable && sys->emitter_settings_dirty;
    const bool emitter_connected = sys->emitter_enable && sys->emitter_fd >= 0;
    int drive_mode = 0;
    if (emitter_connected && sys->emitter_device_settings_valid)
        drive_mode = sys->emitter_device_settings.ir_drive_mode == 1 ? 1 : 0;
    else if (emitter_connected)
        drive_mode = sys->emitter_settings.ir_drive_mode == 1 ? 1 : 0;
    const bool serial_mode = drive_mode == 1;
    const char *emitter_link = !sys->emitter_enable ? "disabled"
                            : (emitter_connected ? "connected" : "disconnected");
    const char *tty_display = "auto";
    if (sys->emitter_tty_selected != NULL && sys->emitter_tty_selected[0] != '\0')
        tty_display = sys->emitter_tty_selected;
    else if (!sys->emitter_tty_auto && sys->emitter_tty != NULL && sys->emitter_tty[0] != '\0')
        tty_display = sys->emitter_tty;
    else if (!sys->emitter_tty_auto)
        tty_display = "(unset)";

    const int margin_x = Open3DClampInt((int)(base_width / 80), 12, 48);
    const int margin_y = Open3DClampInt((int)(base_height / 80), 12, 36);
    const int status_font_size =
        Open3DClampInt((int)((base_height / 41.0) * OPEN3D_OSD_FONT_SCALE + 0.5), 16, 28);
    const int calib_font_size =
        Open3DClampInt((int)((base_height / 37.0) * OPEN3D_OSD_FONT_SCALE + 0.5), 18, 30);
    const int max_width = (int)base_width - margin_x * 2;
    const int status_max_height = (int)base_height / 2;
    const int calib_max_height = (int)base_height - margin_y * 2;

    char status_block[4096];
    char status_serial_block[2048];
    char calibration_block[4096];
    char calibration_serial_block[2048];
    status_block[0] = '\0';
    status_serial_block[0] = '\0';
    calibration_block[0] = '\0';
    calibration_serial_block[0] = '\0';

    char k_toggle[96], k_osd[96], k_calib[96], k_help[96], k_flip[96];
    char k_g[96], k_t[96], k_b[96], k_p[96];
    char k_w[96], k_s[96], k_a[96], k_d[96];
    char k_q[96], k_e[96], k_n[96], k_m[96];
    char k_z[96], k_x[96], k_i[96], k_k[96], k_o[96], k_l[96];
    if (general_visible || calibration_visible)
    {
        Open3DHotkeyName(sys->hotkey_toggle_enabled, k_toggle, sizeof(k_toggle));
        Open3DHotkeyName(sys->hotkey_toggle_trigger, k_osd, sizeof(k_osd));
        Open3DHotkeyName(sys->hotkey_toggle_calibration, k_calib, sizeof(k_calib));
        Open3DHotkeyName(sys->hotkey_help, k_help, sizeof(k_help));
        Open3DHotkeyName(sys->hotkey_flip_eyes, k_flip, sizeof(k_flip));
        Open3DHotkeyName(sys->hotkey_calib_g, k_g, sizeof(k_g));
        Open3DHotkeyName(sys->hotkey_calib_t, k_t, sizeof(k_t));
        Open3DHotkeyName(sys->hotkey_calib_b, k_b, sizeof(k_b));
        Open3DHotkeyName(sys->hotkey_calib_p, k_p, sizeof(k_p));
        Open3DHotkeyName(sys->hotkey_calib_w, k_w, sizeof(k_w));
        Open3DHotkeyName(sys->hotkey_calib_s, k_s, sizeof(k_s));
        Open3DHotkeyName(sys->hotkey_calib_a, k_a, sizeof(k_a));
        Open3DHotkeyName(sys->hotkey_calib_d, k_d, sizeof(k_d));
        Open3DHotkeyName(sys->hotkey_calib_q, k_q, sizeof(k_q));
        Open3DHotkeyName(sys->hotkey_calib_e, k_e, sizeof(k_e));
        Open3DHotkeyName(sys->hotkey_calib_n, k_n, sizeof(k_n));
        Open3DHotkeyName(sys->hotkey_calib_m, k_m, sizeof(k_m));
        Open3DHotkeyName(sys->hotkey_calib_z, k_z, sizeof(k_z));
        Open3DHotkeyName(sys->hotkey_calib_x, k_x, sizeof(k_x));
        Open3DHotkeyName(sys->hotkey_calib_i, k_i, sizeof(k_i));
        Open3DHotkeyName(sys->hotkey_calib_k, k_k, sizeof(k_k));
        Open3DHotkeyName(sys->hotkey_calib_o, k_o, sizeof(k_o));
        Open3DHotkeyName(sys->hotkey_calib_l, k_l, sizeof(k_l));
    }

    if (general_visible)
    {
        Open3DAppendLine(status_block, sizeof(status_block),
                         "Pageflip %s drive=%s emitter=%s trig=%s calib=%s",
                         sys->enabled ? "on" : "off",
                         serial_mode ? "serial" : "optical",
                         emitter_link,
                         sys->trigger_enable ? "on" : "off",
                         sys->calibration_enable ? "on" : "off");
        Open3DAppendLine(status_block, sizeof(status_block),
                         "Emitter tty: %s", tty_display);
        Open3DAppendLine(status_block, sizeof(status_block),
                         "Hotkeys: %s=2d/3d  %s=osd  %s=calibration",
                         k_toggle, k_osd, k_calib);
        Open3DAppendLine(status_block, sizeof(status_block),
                         "Hotkeys: %s=status  %s=flip",
                         k_help, k_flip);
    }

    if (message_active && (general_visible || calibration_visible))
    {
        if (general_visible)
            Open3DAppendLine(status_block, sizeof(status_block), "%s", sys->status_message);
        else
            Open3DAppendLine(calibration_block, sizeof(calibration_block), "%s", sys->status_message);
    }

    if (dirty && (general_visible || calibration_visible))
    {
        if (general_visible)
            Open3DAppendLine(status_block, sizeof(status_block),
                             "Emitter settings not saved to EEPROM (press %s to save)", k_b);
        else
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Emitter settings not saved to EEPROM (press %s to save)", k_b);
    }

    if (calibration_visible)
    {
        if (sys->status_calibration_help)
        {
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Calibration mode %s", serial_mode ? "serial" : "optical");
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "%s: Toggle calibration help overlay.", k_g);
            Open3DAppendLine(calibration_serial_block, sizeof(calibration_serial_block),
                             "%s: Toggle drive mode (0=optical, 1=serial).", k_t);
            Open3DAppendLine(calibration_serial_block, sizeof(calibration_serial_block),
                             "%s: Save current emitter settings to EEPROM.", k_b);
            Open3DAppendLine(calibration_serial_block, sizeof(calibration_serial_block),
                             "%s/%s: (us) Delay after signal before activating glasses.", k_i, k_k);
            Open3DAppendLine(calibration_serial_block, sizeof(calibration_serial_block),
                             "%s/%s: (us) Duration to keep glasses active after activation.", k_o, k_l);
            if (!emitter_connected)
                Open3DAppendLine(calibration_serial_block, sizeof(calibration_serial_block),
                                 "Serial emitter disconnected; serial hotkeys are unavailable.");
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Shift: Use larger step sizes for adjustments.");
            if (!serial_mode)
            {
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s/%s: Whitebox vertical position.", k_w, k_s);
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s/%s: Whitebox horizontal position.", k_a, k_d);
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s/%s: Spacing between the two whiteboxes.", k_q, k_e);
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s/%s: Whitebox size.", k_z, k_x);
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s/%s: Width of black border around trigger boxes.", k_n, k_m);
                Open3DAppendLine(calibration_block, sizeof(calibration_block),
                                 "%s: Toggle optical debug logging.", k_p);
            }
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Drive mode: %s  emitter: %s",
                             serial_mode ? "serial" : "optical",
                             emitter_link);
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Emitter tty: %s", tty_display);
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Opt debug logging: %s",
                             serial_mode ? "N/A (serial mode)"
                                         : (sys->emitter_opt_csv_enable ? "ON" : "OFF"));
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Top-level: %s/%s/%s/%s/%s = 2d/3d, osd, calibration, help, flip.",
                             k_toggle, k_osd, k_calib, k_help, k_flip);
        }
        else
            Open3DAppendLine(calibration_block, sizeof(calibration_block),
                             "Calibration mode ON (press %s for calibration help)", k_g);
    }

    int status_region_y = 0;
    bool status_region_visible = false;
    if (status_block[0] != '\0' &&
        Open3DTextRegionCacheGetCopy(&sys->status_main_cache, status_block,
                                     max_width, status_max_height,
                                     status_font_size,
                                     0xD6F8FF, 120, pixel_aspect, false,
                                     NULL) == VLC_SUCCESS)
    {
        int w = 0, h = 0;
        if (Open3DTextCacheDimensions(&sys->status_main_cache, &w, &h))
        {
            int y = (int)base_height - margin_y - h;
            if (y < margin_y)
                y = margin_y;
            status_region_y = y;
            status_region_visible = true;
            Open3DDrawOverlayRectsGLOffset(vd, sys, place, base_width, base_height,
                                           sys->status_main_cache.gpu_rects,
                                           sys->status_main_cache.gpu_rect_count,
                                           0, y);
        }
    }

    if (status_serial_block[0] != '\0' &&
        Open3DTextRegionCacheGetCopy(&sys->status_serial_cache, status_serial_block,
                                     max_width, status_max_height,
                                     status_font_size,
                                     emitter_connected ? 0xD6F8FF : 0x8A8A8A, 120,
                                     pixel_aspect, false, NULL) == VLC_SUCCESS)
    {
        int h = 0;
        if (Open3DTextCacheDimensions(&sys->status_serial_cache, NULL, &h))
        {
            int y;
            if (status_region_visible)
            {
                const int gap = Open3DClampInt(status_font_size / 3, 4, 12);
                y = status_region_y - h - gap;
            }
            else
            {
                y = (int)base_height - margin_y - h;
            }
            if (y < margin_y)
                y = margin_y;
            Open3DDrawOverlayRectsGLOffset(vd, sys, place, base_width, base_height,
                                           sys->status_serial_cache.gpu_rects,
                                           sys->status_serial_cache.gpu_rect_count,
                                           0, y);
        }
    }

    int calib_region_x = 0;
    int calib_region_y = 0;
    int calib_region_h = 0;
    bool calib_region_visible = false;
    if (calibration_block[0] != '\0' &&
        Open3DTextRegionCacheGetCopy(&sys->status_calib_main_cache, calibration_block,
                                     max_width, calib_max_height,
                                     calib_font_size,
                                     0xFFE8B0, 112, pixel_aspect, true, NULL) == VLC_SUCCESS)
    {
        int w = 0, h = 0;
        if (Open3DTextCacheDimensions(&sys->status_calib_main_cache, &w, &h))
        {
            int x = ((int)base_width - w) / 2;
            int y = ((int)base_height - h) / 2;
            if (x < margin_x)
                x = margin_x;
            if (y < margin_y)
                y = margin_y;
            if (x + w > (int)base_width - margin_x)
                x = (int)base_width - margin_x - w;
            if (x < margin_x)
                x = margin_x;
            calib_region_x = x;
            calib_region_y = y;
            calib_region_h = h;
            calib_region_visible = true;
            Open3DDrawOverlayRectsGLOffset(vd, sys, place, base_width, base_height,
                                           sys->status_calib_main_cache.gpu_rects,
                                           sys->status_calib_main_cache.gpu_rect_count,
                                           x, y);
        }
    }

    if (calibration_serial_block[0] != '\0' &&
        Open3DTextRegionCacheGetCopy(&sys->status_calib_serial_cache, calibration_serial_block,
                                     max_width, calib_max_height,
                                     calib_font_size,
                                     emitter_connected ? 0xFFE8B0 : 0x8A8A8A, 112,
                                     pixel_aspect, true, NULL) == VLC_SUCCESS)
    {
        int w = 0, h = 0;
        if (Open3DTextCacheDimensions(&sys->status_calib_serial_cache, &w, &h))
        {
            int x = ((int)base_width - w) / 2;
            int y = ((int)base_height - h) / 2;
            if (calib_region_visible)
            {
                const int gap = Open3DClampInt(calib_font_size / 3, 4, 14);
                y = calib_region_y + calib_region_h + gap;
                if (y + h > (int)base_height - margin_y)
                    y = calib_region_y - h - gap;
                x = calib_region_x;
            }

            if (x < margin_x)
                x = margin_x;
            if (x + w > (int)base_width - margin_x)
                x = (int)base_width - margin_x - w;
            if (x < margin_x)
                x = margin_x;
            if (y < margin_y)
                y = margin_y;
            if (y + h > (int)base_height - margin_y)
                y = (int)base_height - margin_y - h;
            if (y < margin_y)
                y = margin_y;

            Open3DDrawOverlayRectsGLOffset(vd, sys, place, base_width, base_height,
                                           sys->status_calib_serial_cache.gpu_rects,
                                           sys->status_calib_serial_cache.gpu_rect_count,
                                           x, y);
        }
    }
}

static void Open3DDrawGpuOverlay(vout_display_t *vd, vout_display_sys_t *sys,
                                 const video_format_t *eye_source,
                                 open3d_pack_t pack_mode, bool show_right_eye,
                                 const vout_display_place_t *place)
{
    if (sys == NULL || place == NULL || eye_source == NULL)
        return;
    if (!sys->enabled || !sys->gpu_overlay_enable || !sys->gpu_overlay_ready)
        return;
    const bool optical_mode = Open3DCurrentDriveMode(sys) == 0;

    unsigned base_width = vd->source.i_visible_width > 0 ? vd->source.i_visible_width : vd->source.i_width;
    unsigned base_height = vd->source.i_visible_height > 0 ? vd->source.i_visible_height : vd->source.i_height;
    if (base_width == 0 || base_height == 0)
        return;

    const unsigned eye_width = eye_source->i_visible_width > 0 ? eye_source->i_visible_width : base_width;
    const unsigned eye_height = eye_source->i_visible_height > 0 ? eye_source->i_visible_height : base_height;
    const double scale_x = eye_width > 0 ? (double)base_width / eye_width : 1.0;
    const double scale_y = eye_height > 0 ? (double)base_height / eye_height : 1.0;

    open3d_overlay_rect_t calibration_rects[8];
    size_t calibration_rect_count = 0;
    open3d_overlay_rect_t trigger_rects[8];
    size_t trigger_rect_count = 0;

    open3d_trigger_geometry_t trigger_geo;
    Open3DComputeTriggerGeometry(sys, base_width, base_height, scale_x, scale_y, &trigger_geo);

    if (sys->trigger_enable &&
        sys->trigger_size > 0 &&
        sys->trigger_alpha > 0 &&
        optical_mode &&
        pack_mode != OPEN3D_PACK_NONE &&
        trigger_geo.valid)
    {
        bool primary_white = !show_right_eye;
        if (sys->trigger_invert)
            primary_white = !primary_white;

        const uint8_t white = sys->trigger_brightness;
        const uint8_t primary_level = primary_white ? white : 0x00;
        const uint8_t secondary_level = primary_white ? 0x00 : white;

        if (trigger_geo.black_border_x > 0 || trigger_geo.black_border_y > 0)
        {
            const int x_left = (trigger_geo.x_primary < trigger_geo.x_secondary)
                               ? trigger_geo.x_primary : trigger_geo.x_secondary;
            const int x_right = (trigger_geo.x_primary > trigger_geo.x_secondary)
                                ? trigger_geo.x_primary : trigger_geo.x_secondary;
            Open3DOverlayRectPush(trigger_rects, ARRAY_SIZE(trigger_rects), &trigger_rect_count,
                                  x_left - (int)trigger_geo.black_border_x,
                                  trigger_geo.y - (int)trigger_geo.black_border_y,
                                  (unsigned)(x_right - x_left) + trigger_geo.size_x +
                                      2 * trigger_geo.black_border_x,
                                  trigger_geo.size_y + 2 * trigger_geo.black_border_y,
                                  0x00, 0x00, 0x00, sys->trigger_alpha);
        }

        Open3DOverlayRectPush(trigger_rects, ARRAY_SIZE(trigger_rects), &trigger_rect_count,
                              trigger_geo.x_primary, trigger_geo.y,
                              trigger_geo.size_x, trigger_geo.size_y,
                              primary_level, primary_level, primary_level,
                              sys->trigger_alpha);
        Open3DOverlayRectPush(trigger_rects, ARRAY_SIZE(trigger_rects), &trigger_rect_count,
                              trigger_geo.x_secondary, trigger_geo.y,
                              trigger_geo.size_x, trigger_geo.size_y,
                              secondary_level, secondary_level, secondary_level,
                              sys->trigger_alpha);
    }

    if (sys->calibration_enable &&
        sys->calibration_size > 0 &&
        sys->calibration_thickness > 0 &&
        sys->calibration_alpha > 0 &&
        optical_mode)
    {
        unsigned thick_x = Open3DScalePx(sys->calibration_thickness, scale_x);
        unsigned thick_y = Open3DScalePx(sys->calibration_thickness, scale_y);
        if (thick_x == 0)
            thick_x = 1;
        if (thick_y == 0)
            thick_y = 1;

        int center_x = (int)(base_width / 2);
        int center_y = (int)(base_height / 2);
        int area_left = 0;
        int area_top = 0;
        int area_right = (int)base_width;
        int area_bottom = (int)base_height;

        if (trigger_geo.valid && sys->trigger_enable)
        {
            unsigned extra_x = Open3DScalePx(sys->calibration_size, scale_x);
            unsigned extra_y = Open3DScalePx(sys->calibration_size, scale_y);
            if (extra_x < 1)
                extra_x = 1;
            if (extra_y < 1)
                extra_y = 1;

            const int trigger_left = (trigger_geo.x_primary < trigger_geo.x_secondary
                                      ? trigger_geo.x_primary
                                      : trigger_geo.x_secondary) -
                                     (int)trigger_geo.black_border_x;
            const int trigger_right = (trigger_geo.x_primary > trigger_geo.x_secondary
                                       ? trigger_geo.x_primary
                                       : trigger_geo.x_secondary) +
                                      (int)trigger_geo.size_x +
                                      (int)trigger_geo.black_border_x;
            const int trigger_top = trigger_geo.y - (int)trigger_geo.black_border_y;
            const int trigger_bottom = trigger_geo.y + (int)trigger_geo.size_y +
                                       (int)trigger_geo.black_border_y;

            int extra_left = (int)(extra_x / 2);
            int extra_right = (int)extra_x - extra_left;
            int extra_top = (int)(extra_y / 2);
            int extra_bottom = (int)extra_y - extra_top;

            area_left = trigger_left - extra_left;
            area_right = trigger_right + extra_right;
            area_top = trigger_top - extra_top;
            area_bottom = trigger_bottom + extra_bottom;

            Open3DOverlayRectPush(calibration_rects, ARRAY_SIZE(calibration_rects), &calibration_rect_count,
                                  area_left, area_top,
                                  (unsigned)((area_right - area_left) > 1 ? (area_right - area_left) : 1),
                                  (unsigned)((area_bottom - area_top) > 1 ? (area_bottom - area_top) : 1),
                                  0x00, 0x00, 0x00, 0xFF);

            center_x = (trigger_geo.x_primary + trigger_geo.x_secondary + (int)trigger_geo.size_x) / 2;
            center_y = trigger_geo.y + (int)trigger_geo.size_y / 2;
        }
        else
        {
            unsigned half_x = Open3DScalePx(sys->calibration_size, scale_x);
            unsigned half_y = Open3DScalePx(sys->calibration_size, scale_y);
            area_left = center_x - (int)half_x;
            area_right = center_x + (int)half_x;
            area_top = center_y - (int)half_y;
            area_bottom = center_y + (int)half_y;
        }

        const int horiz_x = area_left;
        const int horiz_y = center_y - (int)(thick_y / 2);
        const int vert_x = center_x - (int)(thick_x / 2);
        const int vert_y = area_top;
        const unsigned horiz_w = (unsigned)((area_right - area_left) > 1 ? (area_right - area_left) : 1);
        const unsigned vert_h = (unsigned)((area_bottom - area_top) > 1 ? (area_bottom - area_top) : 1);
        Open3DOverlayRectPush(calibration_rects, ARRAY_SIZE(calibration_rects), &calibration_rect_count,
                              horiz_x, horiz_y, horiz_w, thick_y,
                              0x00, 0xFF, 0x00, sys->calibration_alpha);
        Open3DOverlayRectPush(calibration_rects, ARRAY_SIZE(calibration_rects), &calibration_rect_count,
                              vert_x, vert_y, thick_x, vert_h,
                              0x00, 0xFF, 0x00, sys->calibration_alpha);
    }

    /* Keep calibration behind trigger boxes so the white trigger pattern remains visible. */
    if (calibration_rect_count > 0)
        Open3DDrawOverlayRectsGL(vd, sys, place, base_width, base_height,
                                 calibration_rects, calibration_rect_count);
    if (trigger_rect_count > 0)
        Open3DDrawOverlayRectsGL(vd, sys, place, base_width, base_height,
                                 trigger_rects, trigger_rect_count);
}

static bool Open3DOverlayActive(vout_display_sys_t *sys, open3d_pack_t pack_mode)
{
    const vlc_tick_t now = mdate();
    const bool optical_mode = Open3DCurrentDriveMode(sys) == 0;
    const bool use_gpu_overlay = sys->gpu_overlay_enable && sys->gpu_overlay_ready;
    const bool trigger_active = !use_gpu_overlay &&
                                sys->enabled &&
                                sys->trigger_enable &&
                                sys->trigger_size > 0 &&
                                sys->trigger_alpha > 0 &&
                                optical_mode &&
                                pack_mode != OPEN3D_PACK_NONE;

    const bool calibration_active = !use_gpu_overlay &&
                                    sys->enabled &&
                                    sys->calibration_enable &&
                                    sys->calibration_size > 0 &&
                                    sys->calibration_thickness > 0 &&
                                    sys->calibration_alpha > 0 &&
                                    optical_mode;
    /* Keep status/calibration text in the subpicture pipeline for proper alpha blending. */
    const bool status_active = Open3DStatusOverlayActive(sys, now);

    return trigger_active || calibration_active || status_active;
}

static subpicture_t *Open3DBuildOverlaySubpicture(vout_display_t *vd,
                                                  vout_display_sys_t *sys,
                                                  const subpicture_t *base_subpicture,
                                                  bool base_subpicture_shifted,
                                                  const subpicture_t *extra_subpicture,
                                                  bool extra_subpicture_shifted,
                                                  const video_format_t *eye_source,
                                                  open3d_pack_t pack_mode,
                                                  bool show_right_eye,
                                                  vlc_tick_t now)
{
    subpicture_t *merged = subpicture_New(NULL);
    if (merged == NULL)
        return NULL;

    subpicture_region_t *base_chain = NULL;
    subpicture_region_t *extra_chain = NULL;

    if (base_subpicture != NULL)
    {
        merged->i_channel = base_subpicture->i_channel;
        merged->i_order = base_subpicture->i_order;
        merged->i_start = base_subpicture->i_start;
        merged->i_stop = base_subpicture->i_stop;
        merged->b_ephemer = base_subpicture->b_ephemer;
        merged->b_fade = base_subpicture->b_fade;
        merged->b_subtitle = base_subpicture->b_subtitle;
        merged->b_absolute = base_subpicture->b_absolute;
        merged->i_original_picture_width = base_subpicture->i_original_picture_width;
        merged->i_original_picture_height = base_subpicture->i_original_picture_height;
        merged->i_alpha = base_subpicture->i_alpha;

        if (Open3DCopyRegionChain(&merged->p_region, base_subpicture->p_region) != VLC_SUCCESS)
        {
            subpicture_Delete(merged);
            return NULL;
        }
        base_chain = merged->p_region;
    }

    if (extra_subpicture != NULL)
    {
        if (Open3DCopyRegionChain(&extra_chain, extra_subpicture->p_region) != VLC_SUCCESS)
        {
            subpicture_Delete(merged);
            return NULL;
        }
        Open3DRegionChainAppend(&merged->p_region, extra_chain);

        if (merged->i_original_picture_width <= 0)
            merged->i_original_picture_width = extra_subpicture->i_original_picture_width;
        if (merged->i_original_picture_height <= 0)
            merged->i_original_picture_height = extra_subpicture->i_original_picture_height;
        if (extra_subpicture->b_subtitle)
            merged->b_subtitle = true;
    }

    unsigned base_width = merged->i_original_picture_width > 0
                        ? (unsigned)merged->i_original_picture_width
                        : vd->source.i_visible_width;
    unsigned base_height = merged->i_original_picture_height > 0
                         ? (unsigned)merged->i_original_picture_height
                         : vd->source.i_visible_height;

    if (base_width == 0)
        base_width = vd->source.i_width;
    if (base_height == 0)
        base_height = vd->source.i_height;

    if (merged->i_original_picture_width <= 0)
        merged->i_original_picture_width = (int)base_width;
    if (merged->i_original_picture_height <= 0)
        merged->i_original_picture_height = (int)base_height;
    if (sys->enabled && pack_mode != OPEN3D_PACK_NONE)
    {
        const unsigned shift_width = eye_source->i_visible_width > 0
                                   ? eye_source->i_visible_width
                                   : (eye_source->i_width > 0 ? eye_source->i_width
                                                              : base_width);
        if (base_chain != NULL &&
            base_subpicture != NULL &&
            base_subpicture->b_subtitle &&
            !base_subpicture_shifted)
            Open3DApplySubtitleStereoShiftToChain(vd, sys, base_chain,
                                                  shift_width, show_right_eye);
        if (extra_chain != NULL &&
            extra_subpicture != NULL &&
            extra_subpicture->b_subtitle &&
            !extra_subpicture_shifted)
            Open3DApplySubtitleStereoShiftToChain(vd, sys, extra_chain,
                                                  shift_width, show_right_eye);
    }

    const unsigned eye_width = eye_source->i_visible_width > 0
                             ? eye_source->i_visible_width
                             : base_width;
    const unsigned eye_height = eye_source->i_visible_height > 0
                              ? eye_source->i_visible_height
                              : base_height;

    const double scale_x = eye_width > 0 ? (double)base_width / eye_width : 1.0;
    const double scale_y = eye_height > 0 ? (double)base_height / eye_height : 1.0;
    double pixel_aspect = 1.0;
    if (eye_source->i_sar_num > 0 && eye_source->i_sar_den > 0)
        pixel_aspect = (double)eye_source->i_sar_num / (double)eye_source->i_sar_den;
    open3d_trigger_geometry_t trigger_geo;
    Open3DComputeTriggerGeometry(sys, base_width, base_height, scale_x, scale_y, &trigger_geo);
    const bool optical_mode = Open3DCurrentDriveMode(sys) == 0;
    const bool use_gpu_overlay = sys->gpu_overlay_enable && sys->gpu_overlay_ready;

    if (!use_gpu_overlay)
    {
        if (sys->enabled &&
            sys->trigger_enable &&
            sys->trigger_size > 0 &&
            sys->trigger_alpha > 0 &&
            optical_mode &&
            pack_mode != OPEN3D_PACK_NONE &&
            trigger_geo.valid)
        {
            bool primary_white = !show_right_eye;
            if (sys->trigger_invert)
                primary_white = !primary_white;
            if (Open3DEnsureTriggerRegionCache(sys, base_width, base_height,
                                               &trigger_geo) != VLC_SUCCESS)
                goto overlay_error;

            const size_t pattern_idx = primary_white ? 1 : 0;
            if (sys->trigger_region_cache.pattern_regions[pattern_idx] != NULL)
            {
                if (Open3DCloneRegionChainShallow(&merged->p_region,
                                                  sys->trigger_region_cache.pattern_regions[pattern_idx]) != VLC_SUCCESS)
                    goto overlay_error;
            }
        }

        if (sys->enabled &&
            sys->calibration_enable &&
            sys->calibration_size > 0 &&
            sys->calibration_thickness > 0 &&
            sys->calibration_alpha > 0 &&
            optical_mode &&
            base_width > 0 &&
            base_height > 0)
        {
            unsigned thick_x = Open3DScalePx(sys->calibration_thickness, scale_x);
            unsigned thick_y = Open3DScalePx(sys->calibration_thickness, scale_y);
            if (thick_x == 0)
                thick_x = 1;
            if (thick_y == 0)
                thick_y = 1;

            int center_x = (int)(base_width / 2);
            int center_y = (int)(base_height / 2);
            int area_left = 0;
            int area_top = 0;
            int area_right = (int)base_width;
            int area_bottom = (int)base_height;
            bool include_black = false;

            if (trigger_geo.valid &&
                sys->trigger_enable)
            {
                unsigned extra_x = Open3DScalePx(sys->calibration_size, scale_x);
                unsigned extra_y = Open3DScalePx(sys->calibration_size, scale_y);
                if (extra_x < 1)
                    extra_x = 1;
                if (extra_y < 1)
                    extra_y = 1;

                const int trigger_left = (trigger_geo.x_primary < trigger_geo.x_secondary
                                          ? trigger_geo.x_primary
                                          : trigger_geo.x_secondary) -
                                         (int)trigger_geo.black_border_x;
                const int trigger_right = (trigger_geo.x_primary > trigger_geo.x_secondary
                                           ? trigger_geo.x_primary
                                           : trigger_geo.x_secondary) +
                                          (int)trigger_geo.size_x +
                                          (int)trigger_geo.black_border_x;
                const int trigger_top = trigger_geo.y - (int)trigger_geo.black_border_y;
                const int trigger_bottom = trigger_geo.y + (int)trigger_geo.size_y +
                                           (int)trigger_geo.black_border_y;

                int extra_left = (int)(extra_x / 2);
                int extra_right = (int)extra_x - extra_left;
                int extra_top = (int)(extra_y / 2);
                int extra_bottom = (int)extra_y - extra_top;

                area_left = trigger_left - extra_left;
                area_right = trigger_right + extra_right;
                area_top = trigger_top - extra_top;
                area_bottom = trigger_bottom + extra_bottom;
                include_black = true;

                center_x = (trigger_geo.x_primary + trigger_geo.x_secondary + (int)trigger_geo.size_x) / 2;
                center_y = trigger_geo.y + (int)trigger_geo.size_y / 2;
            }
            else
            {
                unsigned half_x = Open3DScalePx(sys->calibration_size, scale_x);
                unsigned half_y = Open3DScalePx(sys->calibration_size, scale_y);
                area_left = center_x - (int)half_x;
                area_right = center_x + (int)half_x;
                area_top = center_y - (int)half_y;
                area_bottom = center_y + (int)half_y;
            }

            if (Open3DEnsureCalibrationRegionCache(sys,
                                                   base_width, base_height,
                                                   area_left, area_top,
                                                   area_right, area_bottom,
                                                   center_x, center_y,
                                                   thick_x, thick_y,
                                                   include_black,
                                                   sys->calibration_alpha) != VLC_SUCCESS)
                goto overlay_error;

            if (sys->calibration_region_cache.region_chain != NULL)
            {
                subpicture_region_t *calib_copy = NULL;
                if (Open3DCloneRegionChainShallow(&calib_copy,
                                                  sys->calibration_region_cache.region_chain) != VLC_SUCCESS)
                    goto overlay_error;
                Open3DRegionChainPrependChain(&merged->p_region, calib_copy);
            }
        }
    }

    if (Open3DAppendStatusOverlay(vd, sys, merged, base_width, base_height, now,
                                  pixel_aspect) != VLC_SUCCESS)
        goto overlay_error;

    return merged;

overlay_error:
    subpicture_Delete(merged);
    return NULL;
}

static subpicture_t *Open3DPresenterOverlayCacheGet(vout_display_t *vd,
                                                    vout_display_sys_t *sys,
                                                    const video_format_t *eye_source,
                                                    open3d_pack_t pack_mode,
                                                    bool show_right_eye,
                                                    vlc_tick_t now)
{
    if (vd == NULL || sys == NULL || eye_source == NULL)
        return NULL;

    const int idx = show_right_eye ? 1 : 0;
    open3d_overlay_subpicture_cache_t *entry = &sys->presenter_overlay_cache[idx];

    unsigned base_width = vd->source.i_visible_width;
    unsigned base_height = vd->source.i_visible_height;
    if (base_width == 0)
        base_width = vd->source.i_width;
    if (base_height == 0)
        base_height = vd->source.i_height;

    double pixel_aspect = 1.0;
    if (eye_source->i_sar_num > 0 && eye_source->i_sar_den > 0)
        pixel_aspect = (double)eye_source->i_sar_num / (double)eye_source->i_sar_den;
    const int pixel_aspect_milli = Open3DPixelAspectMilli(pixel_aspect);
    const bool message_active = (sys->status_message_until != VLC_TICK_INVALID &&
                                 now < sys->status_message_until &&
                                 sys->status_message[0] != '\0');
    const uint64_t state_epoch =
        atomic_load_explicit(&sys->overlay_state_epoch, memory_order_relaxed);

    if (entry->valid &&
        entry->subpicture != NULL &&
        entry->state_epoch == state_epoch &&
        entry->base_width == base_width &&
        entry->base_height == base_height &&
        entry->pixel_aspect_milli == pixel_aspect_milli &&
        entry->pack_mode == pack_mode &&
        entry->show_right_eye == show_right_eye &&
        entry->message_active == message_active)
    {
        return entry->subpicture;
    }

    subpicture_t *fresh = Open3DBuildOverlaySubpicture(vd, sys, NULL, false,
                                                       NULL, false,
                                                       eye_source,
                                                       pack_mode,
                                                       show_right_eye,
                                                       now);
    if (fresh == NULL)
        return entry->subpicture;

    Open3DOverlayCacheEntryClear(entry);
    entry->valid = true;
    entry->state_epoch = state_epoch;
    entry->base_width = base_width;
    entry->base_height = base_height;
    entry->pixel_aspect_milli = pixel_aspect_milli;
    entry->pack_mode = pack_mode;
    entry->show_right_eye = show_right_eye;
    entry->message_active = message_active;
    entry->subpicture = fresh;
    return fresh;
}

static speed_t Open3DEmitterSpeedFromBaud(int baud)
{
    switch (baud)
    {
        case 1200:
            return B1200;
        case 2400:
            return B2400;
        case 4800:
            return B4800;
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
#ifdef B230400
        case 230400:
            return B230400;
#endif
#ifdef B460800
        case 460800:
            return B460800;
#endif
#ifdef B921600
        case 921600:
            return B921600;
#endif
        default:
            return B0;
    }
}

static bool Open3DEmitterWouldBlock(int io_errno)
{
    if (io_errno == EAGAIN)
        return true;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (io_errno == EWOULDBLOCK)
        return true;
#endif
    return false;
}

static bool Open3DContainsNoCase(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL)
        return false;

    const size_t needle_len = strlen(needle);
    if (needle_len == 0)
        return true;

    for (const char *p = haystack; *p != '\0'; ++p)
    {
        if (strncasecmp(p, needle, needle_len) == 0)
            return true;
    }
    return false;
}

static int Open3DEmitterPortScore(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return INT_MIN / 2;

    int score = 0;
    if (Open3DContainsNoCase(path, "/dev/serial/by-id/"))
        score += 30;
    if (Open3DContainsNoCase(path, "/dev/serial/by-path/"))
        score += 24;
    if (Open3DContainsNoCase(path, "sparkfun"))
        score += 60;
    if (Open3DContainsNoCase(path, "pro_micro"))
        score += 50;
    if (Open3DContainsNoCase(path, "open3d"))
        score += 40;
    if (Open3DContainsNoCase(path, "1b4f"))
        score += 30;
    if (Open3DContainsNoCase(path, "9206"))
        score += 20;
    if (Open3DContainsNoCase(path, "ttyACM"))
        score += 20;
    if (Open3DContainsNoCase(path, "ttyUSB"))
        score += 16;
    if (Open3DContainsNoCase(path, "arduino"))
        score += 8;
    if (Open3DContainsNoCase(path, "ch340") ||
        Open3DContainsNoCase(path, "1a86"))
        score += 8;
    if (Open3DContainsNoCase(path, "cp210") ||
        Open3DContainsNoCase(path, "10c4"))
        score += 8;
    if (Open3DContainsNoCase(path, "ftdi") ||
        Open3DContainsNoCase(path, "0403"))
        score += 8;
    if (Open3DContainsNoCase(path, "bluetooth"))
        score -= 80;
    if (Open3DContainsNoCase(path, "ttyS") || Open3DContainsNoCase(path, "ttyAMA"))
        score -= 30;

    char *resolved = realpath(path, NULL);
    if (resolved != NULL)
    {
        if (strcmp(resolved, path))
            score += Open3DEmitterPortScore(resolved) / 2;
        free(resolved);
    }

    return score;
}

static int Open3DEmitterProbePortScore(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        if (errno == EACCES || errno == EBUSY || errno == EPERM)
            return 4;
        return -400;
    }

    struct termios tio;
    int score = tcgetattr(fd, &tio) == 0 ? 14 : 2;
    close(fd);
    return score;
}

static char *Open3DEmitterDetectAutoTty(vout_display_t *vd, vout_display_sys_t *sys)
{
    const char *const patterns[] = {
        "/dev/serial/by-id/*",
        "/dev/serial/by-path/*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
    };

    int best_score = -1;
    char *best_path = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(patterns); ++i)
    {
        glob_t matches;
        memset(&matches, 0, sizeof(matches));
        if (glob(patterns[i], 0, NULL, &matches) != 0)
        {
            globfree(&matches);
            continue;
        }

        for (size_t idx = 0; idx < matches.gl_pathc; ++idx)
        {
            const char *path = matches.gl_pathv[idx];
            if (path == NULL)
                continue;

            int score = Open3DEmitterPortScore(path);
            const int probe_score = Open3DEmitterProbePortScore(path);
            if (probe_score < 0)
                continue;
            score += probe_score;
            if (sys->emitter_tty_selected != NULL &&
                !strcmp(path, sys->emitter_tty_selected))
            {
                score += 8;
            }
            if (score > best_score || (best_path == NULL && score == best_score))
            {
                char *candidate = strdup(path);
                if (candidate == NULL)
                    continue;
                free(best_path);
                best_path = candidate;
                best_score = score;
            }
        }
        globfree(&matches);
    }

    if (best_path != NULL)
    {
        msg_Dbg(vd, "auto-selected emitter tty: %s (score=%d)", best_path, best_score);
    }

    return best_path;
}

static bool Open3DEmitterSettingsEqual(const open3d_emitter_settings_t *a,
                                       const open3d_emitter_settings_t *b)
{
    return a->ir_protocol == b->ir_protocol &&
           a->ir_frame_delay == b->ir_frame_delay &&
           a->ir_frame_duration == b->ir_frame_duration &&
           a->ir_signal_spacing == b->ir_signal_spacing &&
           a->opt_block_signal_detection_delay == b->opt_block_signal_detection_delay &&
           a->opt_min_threshold_value_to_activate == b->opt_min_threshold_value_to_activate &&
           a->opt_detection_threshold_high == b->opt_detection_threshold_high &&
           a->opt_enable_ignore_during_ir == b->opt_enable_ignore_during_ir &&
           a->opt_enable_duplicate_realtime_reporting == b->opt_enable_duplicate_realtime_reporting &&
           a->opt_output_stats == b->opt_output_stats &&
           a->opt_ignore_all_duplicates == b->opt_ignore_all_duplicates &&
           a->opt_sensor_filter_mode == b->opt_sensor_filter_mode &&
           a->ir_flip_eyes == b->ir_flip_eyes &&
           a->opt_detection_threshold_low == b->opt_detection_threshold_low &&
           a->ir_average_timing_mode == b->ir_average_timing_mode &&
           a->target_frametime == b->target_frametime &&
           a->ir_drive_mode == b->ir_drive_mode;
}

static void Open3DEmitterSettingsCopy(open3d_emitter_settings_t *dst,
                                      const open3d_emitter_settings_t *src)
{
    *dst = *src;
}

static bool Open3DEnsureParentDirectory(const char *filepath)
{
    if (filepath == NULL || filepath[0] == '\0')
        return false;

    char *scratch = strdup(filepath);
    if (scratch == NULL)
        return false;

    for (char *p = scratch + 1; *p != '\0'; ++p)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(scratch, 0755) != 0 && errno != EEXIST)
        {
            free(scratch);
            return false;
        }
        *p = '/';
    }

    free(scratch);
    return true;
}

static char *Open3DResolveEmitterSettingsPath(const char *raw)
{
    if (raw == NULL || raw[0] == '\0' || !strcasecmp(raw, "auto"))
    {
        const char *config_home = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        const char *base = config_home;
        const char *suffix = "/open3doled/vlc/local_emitter_settings.json";
        if (base == NULL || base[0] == '\0')
        {
            if (home == NULL || home[0] == '\0')
                return NULL;
            suffix = "/.config/open3doled/vlc/local_emitter_settings.json";
            base = home;
        }

        size_t len = strlen(base) + strlen(suffix) + 1;
        char *resolved = malloc(len);
        if (resolved == NULL)
            return NULL;
        snprintf(resolved, len, "%s%s", base, suffix);
        return resolved;
    }

    if (raw[0] == '~' && raw[1] == '/')
    {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            return strdup(raw);

        size_t len = strlen(home) + strlen(raw + 1) + 1;
        char *resolved = malloc(len);
        if (resolved == NULL)
            return NULL;
        snprintf(resolved, len, "%s%s", home, raw + 1);
        return resolved;
    }

    return strdup(raw);
}

static char *Open3DResolveEmitterOptCsvPath(const char *raw)
{
    if (raw == NULL || raw[0] == '\0' || !strcasecmp(raw, "auto"))
    {
        const char *state_home = getenv("XDG_STATE_HOME");
        const char *home = getenv("HOME");
        const char *base = state_home;
        const char *suffix = "/open3doled/vlc/optical_debug.csv";
        if (base == NULL || base[0] == '\0')
        {
            if (home == NULL || home[0] == '\0')
                return NULL;
            suffix = "/.local/state/open3doled/vlc/optical_debug.csv";
            base = home;
        }

        size_t len = strlen(base) + strlen(suffix) + 1;
        char *resolved = malloc(len);
        if (resolved == NULL)
            return NULL;
        snprintf(resolved, len, "%s%s", base, suffix);
        return resolved;
    }

    if (raw[0] == '~' && raw[1] == '/')
    {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            return strdup(raw);

        size_t len = strlen(home) + strlen(raw + 1) + 1;
        char *resolved = malloc(len);
        if (resolved == NULL)
            return NULL;
        snprintf(resolved, len, "%s%s", home, raw + 1);
        return resolved;
    }

    return strdup(raw);
}

static char *Open3DResolveEmitterFwBackupPath(const char *raw)
{
    if (raw == NULL || raw[0] == '\0' || !strcasecmp(raw, "auto"))
    {
        const char *config_home = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        const char *base = config_home;
        const char *suffix = "/open3doled/vlc/firmware_backup_settings.json";
        if (base == NULL || base[0] == '\0')
        {
            if (home == NULL || home[0] == '\0')
                return NULL;
            suffix = "/.config/open3doled/vlc/firmware_backup_settings.json";
            base = home;
        }

        size_t len = strlen(base) + strlen(suffix) + 1;
        char *resolved = malloc(len);
        if (resolved == NULL)
            return NULL;
        snprintf(resolved, len, "%s%s", base, suffix);
        return resolved;
    }

    if (raw[0] == '~' && raw[1] == '/')
    {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
            return strdup(raw);

        size_t len = strlen(home) + strlen(raw + 1) + 1;
        char *resolved = malloc(len);
        if (resolved == NULL)
            return NULL;
        snprintf(resolved, len, "%s%s", home, raw + 1);
        return resolved;
    }

    return strdup(raw);
}

static bool Open3DWriteAll(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static char *Open3DReadSmallTextFile(const char *path, size_t max_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;

    size_t cap = max_size + 1;
    char *buf = malloc(cap);
    if (buf == NULL)
    {
        close(fd);
        return NULL;
    }

    size_t used = 0;
    while (used < max_size)
    {
        ssize_t n = read(fd, buf + used, max_size - used);
        if (n == 0)
            break;
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        used += (size_t)n;
    }
    close(fd);
    buf[used] = '\0';
    return buf;
}

static bool Open3DJsonGetInt(const char *json, const char *key, int *value)
{
    char needle[96];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return false;

    const char *p = strstr(json, needle);
    if (p == NULL)
        return false;
    p += n;
    while (*p != '\0' && *p != ':')
        ++p;
    if (*p != ':')
        return false;
    ++p;
    while (*p != '\0' && isspace((unsigned char)*p))
        ++p;

    errno = 0;
    char *end = NULL;
    long parsed = strtol(p, &end, 10);
    if (end == p || errno != 0)
        return false;

    *value = (int)parsed;
    return true;
}

static bool Open3DEmitterSettingsFromJson(const char *json, open3d_emitter_settings_t *settings)
{
    open3d_emitter_settings_t parsed;
    if (!Open3DJsonGetInt(json, "ir_protocol", &parsed.ir_protocol) ||
        !Open3DJsonGetInt(json, "ir_frame_delay", &parsed.ir_frame_delay) ||
        !Open3DJsonGetInt(json, "ir_frame_duration", &parsed.ir_frame_duration) ||
        !Open3DJsonGetInt(json, "ir_signal_spacing", &parsed.ir_signal_spacing) ||
        !Open3DJsonGetInt(json, "opt_block_signal_detection_delay", &parsed.opt_block_signal_detection_delay) ||
        !Open3DJsonGetInt(json, "opt_min_threshold_value_to_activate", &parsed.opt_min_threshold_value_to_activate) ||
        !Open3DJsonGetInt(json, "opt_detection_threshold_high", &parsed.opt_detection_threshold_high) ||
        !Open3DJsonGetInt(json, "opt_enable_ignore_during_ir", &parsed.opt_enable_ignore_during_ir) ||
        !Open3DJsonGetInt(json, "opt_enable_duplicate_realtime_reporting", &parsed.opt_enable_duplicate_realtime_reporting) ||
        !Open3DJsonGetInt(json, "opt_output_stats", &parsed.opt_output_stats) ||
        !Open3DJsonGetInt(json, "opt_ignore_all_duplicates", &parsed.opt_ignore_all_duplicates) ||
        !Open3DJsonGetInt(json, "opt_sensor_filter_mode", &parsed.opt_sensor_filter_mode) ||
        !Open3DJsonGetInt(json, "ir_flip_eyes", &parsed.ir_flip_eyes) ||
        !Open3DJsonGetInt(json, "opt_detection_threshold_low", &parsed.opt_detection_threshold_low) ||
        !Open3DJsonGetInt(json, "ir_average_timing_mode", &parsed.ir_average_timing_mode) ||
        !Open3DJsonGetInt(json, "target_frametime", &parsed.target_frametime) ||
        !Open3DJsonGetInt(json, "ir_drive_mode", &parsed.ir_drive_mode))
        return false;

    Open3DEmitterSettingsCopy(settings, &parsed);
    return true;
}

static bool Open3DEmitterWriteSettingsJson(vout_display_t *vd, vout_display_sys_t *sys)
{
    if (!sys->emitter_save_json || sys->emitter_settings_json_path == NULL)
        return false;
    if (!Open3DEnsureParentDirectory(sys->emitter_settings_json_path))
        return false;

    char temp_path[1024];
    int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                     sys->emitter_settings_json_path);
    if (n < 0 || (size_t)n >= sizeof(temp_path))
        return false;

    char body[4096];
    const char *device_block = "null";
    char device_json[1024];
    if (sys->emitter_device_settings_valid)
    {
        int dn = snprintf(device_json, sizeof(device_json),
                          "{\n"
                          "    \"ir_protocol\": %d,\n"
                          "    \"ir_frame_delay\": %d,\n"
                          "    \"ir_frame_duration\": %d,\n"
                          "    \"ir_signal_spacing\": %d,\n"
                          "    \"opt_block_signal_detection_delay\": %d,\n"
                          "    \"opt_min_threshold_value_to_activate\": %d,\n"
                          "    \"opt_detection_threshold_high\": %d,\n"
                          "    \"opt_enable_ignore_during_ir\": %d,\n"
                          "    \"opt_enable_duplicate_realtime_reporting\": %d,\n"
                          "    \"opt_output_stats\": %d,\n"
                          "    \"opt_ignore_all_duplicates\": %d,\n"
                          "    \"opt_sensor_filter_mode\": %d,\n"
                          "    \"ir_flip_eyes\": %d,\n"
                          "    \"opt_detection_threshold_low\": %d,\n"
                          "    \"ir_average_timing_mode\": %d,\n"
                          "    \"target_frametime\": %d,\n"
                          "    \"ir_drive_mode\": %d\n"
                          "  }",
                          sys->emitter_device_settings.ir_protocol,
                          sys->emitter_device_settings.ir_frame_delay,
                          sys->emitter_device_settings.ir_frame_duration,
                          sys->emitter_device_settings.ir_signal_spacing,
                          sys->emitter_device_settings.opt_block_signal_detection_delay,
                          sys->emitter_device_settings.opt_min_threshold_value_to_activate,
                          sys->emitter_device_settings.opt_detection_threshold_high,
                          sys->emitter_device_settings.opt_enable_ignore_during_ir,
                          sys->emitter_device_settings.opt_enable_duplicate_realtime_reporting,
                          sys->emitter_device_settings.opt_output_stats,
                          sys->emitter_device_settings.opt_ignore_all_duplicates,
                          sys->emitter_device_settings.opt_sensor_filter_mode,
                          sys->emitter_device_settings.ir_flip_eyes,
                          sys->emitter_device_settings.opt_detection_threshold_low,
                          sys->emitter_device_settings.ir_average_timing_mode,
                          sys->emitter_device_settings.target_frametime,
                          sys->emitter_device_settings.ir_drive_mode);
        if (dn < 0 || (size_t)dn >= sizeof(device_json))
            return false;
        device_block = device_json;
    }

    int bn = snprintf(body, sizeof(body),
                      "{\n"
                      "  \"schema\": 1,\n"
                      "  \"firmware_version\": %d,\n"
                      "  \"dirty\": %s,\n"
                      "  \"desired\": {\n"
                      "    \"ir_protocol\": %d,\n"
                      "    \"ir_frame_delay\": %d,\n"
                      "    \"ir_frame_duration\": %d,\n"
                      "    \"ir_signal_spacing\": %d,\n"
                      "    \"opt_block_signal_detection_delay\": %d,\n"
                      "    \"opt_min_threshold_value_to_activate\": %d,\n"
                      "    \"opt_detection_threshold_high\": %d,\n"
                      "    \"opt_enable_ignore_during_ir\": %d,\n"
                      "    \"opt_enable_duplicate_realtime_reporting\": %d,\n"
                      "    \"opt_output_stats\": %d,\n"
                      "    \"opt_ignore_all_duplicates\": %d,\n"
                      "    \"opt_sensor_filter_mode\": %d,\n"
                      "    \"ir_flip_eyes\": %d,\n"
                      "    \"opt_detection_threshold_low\": %d,\n"
                      "    \"ir_average_timing_mode\": %d,\n"
                      "    \"target_frametime\": %d,\n"
                      "    \"ir_drive_mode\": %d\n"
                      "  },\n"
                      "  \"device\": %s\n"
                      "}\n",
                      sys->emitter_firmware_version,
                      sys->emitter_settings_dirty ? "true" : "false",
                      sys->emitter_settings.ir_protocol,
                      sys->emitter_settings.ir_frame_delay,
                      sys->emitter_settings.ir_frame_duration,
                      sys->emitter_settings.ir_signal_spacing,
                      sys->emitter_settings.opt_block_signal_detection_delay,
                      sys->emitter_settings.opt_min_threshold_value_to_activate,
                      sys->emitter_settings.opt_detection_threshold_high,
                      sys->emitter_settings.opt_enable_ignore_during_ir,
                      sys->emitter_settings.opt_enable_duplicate_realtime_reporting,
                      sys->emitter_settings.opt_output_stats,
                      sys->emitter_settings.opt_ignore_all_duplicates,
                      sys->emitter_settings.opt_sensor_filter_mode,
                      sys->emitter_settings.ir_flip_eyes,
                      sys->emitter_settings.opt_detection_threshold_low,
                      sys->emitter_settings.ir_average_timing_mode,
                      sys->emitter_settings.target_frametime,
                      sys->emitter_settings.ir_drive_mode,
                      device_block);
    if (bn < 0 || (size_t)bn >= sizeof(body))
        return false;

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    bool ok = Open3DWriteAll(fd, body, (size_t)bn);
    if (close(fd) != 0)
        ok = false;
    if (!ok)
    {
        unlink(temp_path);
        return false;
    }
    if (rename(temp_path, sys->emitter_settings_json_path) != 0)
    {
        unlink(temp_path);
        return false;
    }

    if (sys->emitter_log_io)
        msg_Dbg(vd, "emitter settings JSON updated: %s", sys->emitter_settings_json_path);
    return true;
}

static bool Open3DEmitterLoadSettingsJson(vout_display_t *vd, vout_display_sys_t *sys)
{
    if (!sys->emitter_load_json || sys->emitter_settings_json_path == NULL)
        return false;

    char *json = Open3DReadSmallTextFile(sys->emitter_settings_json_path, 65536);
    if (json == NULL)
        return false;

    open3d_emitter_settings_t parsed;
    bool ok = Open3DEmitterSettingsFromJson(json, &parsed);
    free(json);
    if (!ok)
    {
        msg_Warn(vd, "failed to parse emitter settings JSON: %s",
                 sys->emitter_settings_json_path);
        return false;
    }

    Open3DEmitterSettingsCopy(&sys->emitter_settings, &parsed);
    msg_Dbg(vd, "loaded emitter settings JSON: %s", sys->emitter_settings_json_path);
    return true;
}

static void Open3DEmitterUpdateDirtyState(vout_display_t *vd, vout_display_sys_t *sys,
                                          const char *reason)
{
    bool dirty = false;
    if (sys->emitter_device_settings_valid)
    {
        dirty = !Open3DEmitterSettingsEqual(&sys->emitter_settings,
                                            &sys->emitter_device_settings);
    }

    if (dirty != sys->emitter_settings_dirty)
    {
        msg_Dbg(vd, "emitter settings dirty=%s (%s)",
                dirty ? "yes" : "no",
                reason != NULL ? reason : "unknown");
        Open3DOverlayStateBump(sys);
    }

    sys->emitter_settings_dirty = dirty;
    Open3DEmitterWriteSettingsJson(vd, sys);
}

static bool Open3DEmitterParseParameters(const char *payload,
                                         int *firmware_version,
                                         open3d_emitter_settings_t *out_settings)
{
    if (payload == NULL || payload[0] == '\0')
        return false;

    char *copy = strdup(payload);
    if (copy == NULL)
        return false;

    int values[18];
    unsigned count = 0;
    char *saveptr = NULL;
    for (char *token = strtok_r(copy, ",", &saveptr);
         token != NULL && count < ARRAY_SIZE(values);
         token = strtok_r(NULL, ",", &saveptr))
    {
        while (*token != '\0' && isspace((unsigned char)*token))
            token++;

        errno = 0;
        char *end = NULL;
        long parsed = strtol(token, &end, 10);
        if (end == token || errno != 0)
            break;
        values[count++] = (int)parsed;
    }
    free(copy);

    if (count < 18)
        return false;

    if (firmware_version != NULL)
        *firmware_version = values[0];

    if (out_settings != NULL)
    {
        out_settings->ir_protocol = values[1];
        out_settings->ir_frame_delay = values[2];
        out_settings->ir_frame_duration = values[3];
        out_settings->ir_signal_spacing = values[4];
        out_settings->opt_block_signal_detection_delay = values[5];
        out_settings->opt_min_threshold_value_to_activate = values[6];
        out_settings->opt_detection_threshold_high = values[7];
        out_settings->opt_enable_ignore_during_ir = values[8];
        out_settings->opt_enable_duplicate_realtime_reporting = values[9];
        out_settings->opt_output_stats = values[10];
        out_settings->opt_ignore_all_duplicates = values[11];
        out_settings->opt_sensor_filter_mode = values[12];
        out_settings->ir_flip_eyes = values[13];
        out_settings->opt_detection_threshold_low = values[14];
        out_settings->ir_average_timing_mode = values[15];
        out_settings->target_frametime = values[16];
        out_settings->ir_drive_mode = values[17];
    }
    return true;
}

static void Open3DEmitterCloseOptCsv(vout_display_sys_t *sys)
{
    if (sys->emitter_opt_csv_file != NULL)
    {
        fclose(sys->emitter_opt_csv_file);
        sys->emitter_opt_csv_file = NULL;
    }
}

static bool Open3DEmitterEnsureOptCsv(vout_display_t *vd, vout_display_sys_t *sys)
{
    if (!sys->emitter_opt_csv_enable || sys->emitter_opt_csv_path == NULL)
        return false;
    if (sys->emitter_opt_csv_file != NULL)
        return true;
    if (!Open3DEnsureParentDirectory(sys->emitter_opt_csv_path))
        return false;

    bool write_header = true;
    struct stat st;
    if (stat(sys->emitter_opt_csv_path, &st) == 0 && st.st_size > 0)
        write_header = false;

    FILE *fp = fopen(sys->emitter_opt_csv_path, "a");
    if (fp == NULL)
    {
        msg_Warn(vd, "failed to open emitter optical CSV %s: %s",
                 sys->emitter_opt_csv_path, vlc_strerror_c(errno));
        return false;
    }

    sys->emitter_opt_csv_file = fp;
    if (write_header && !sys->emitter_opt_csv_header_written)
    {
        fputs("mono_us,line\n", fp);
        if (sys->emitter_opt_csv_flush)
            fflush(fp);
        sys->emitter_opt_csv_header_written = true;
    }
    return true;
}

static void Open3DEmitterLogOptCsv(vout_display_t *vd, vout_display_sys_t *sys, const char *line)
{
    if (!sys->emitter_opt_csv_enable || line == NULL || line[0] == '\0')
        return;
    if (!Open3DEmitterEnsureOptCsv(vd, sys))
        return;

    char escaped[1024];
    size_t out = 0;
    escaped[out++] = '"';
    for (const char *p = line; *p != '\0' && out + 3 < sizeof(escaped); ++p)
    {
        if (*p == '"')
            escaped[out++] = '"';
        escaped[out++] = *p;
    }
    escaped[out++] = '"';
    escaped[out] = '\0';

    const vlc_tick_t now = mdate();
    fprintf(sys->emitter_opt_csv_file, "%"PRId64",%s\n", (int64_t)now, escaped);
    if (sys->emitter_opt_csv_flush)
        fflush(sys->emitter_opt_csv_file);
}

static bool Open3DEmitterWriteFirmwareBackup(vout_display_t *vd, vout_display_sys_t *sys,
                                             const char *backup_path)
{
    if (backup_path == NULL || backup_path[0] == '\0')
        return false;
    if (!Open3DEnsureParentDirectory(backup_path))
        return false;

    const open3d_emitter_settings_t *source = &sys->emitter_settings;
    if (sys->emitter_device_settings_valid)
        source = &sys->emitter_device_settings;

    char temp_path[1024];
    int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp", backup_path);
    if (n < 0 || (size_t)n >= sizeof(temp_path))
        return false;

    char body[2048];
    int bn = snprintf(body, sizeof(body),
                      "{\n"
                      "  \"schema\": 1,\n"
                      "  \"timestamp_mono_us\": %"PRId64",\n"
                      "  \"firmware_version\": %d,\n"
                      "  \"settings\": {\n"
                      "    \"ir_protocol\": %d,\n"
                      "    \"ir_frame_delay\": %d,\n"
                      "    \"ir_frame_duration\": %d,\n"
                      "    \"ir_signal_spacing\": %d,\n"
                      "    \"opt_block_signal_detection_delay\": %d,\n"
                      "    \"opt_min_threshold_value_to_activate\": %d,\n"
                      "    \"opt_detection_threshold_high\": %d,\n"
                      "    \"opt_enable_ignore_during_ir\": %d,\n"
                      "    \"opt_enable_duplicate_realtime_reporting\": %d,\n"
                      "    \"opt_output_stats\": %d,\n"
                      "    \"opt_ignore_all_duplicates\": %d,\n"
                      "    \"opt_sensor_filter_mode\": %d,\n"
                      "    \"ir_flip_eyes\": %d,\n"
                      "    \"opt_detection_threshold_low\": %d,\n"
                      "    \"ir_average_timing_mode\": %d,\n"
                      "    \"target_frametime\": %d,\n"
                      "    \"ir_drive_mode\": %d\n"
                      "  }\n"
                      "}\n",
                      (int64_t)mdate(),
                      sys->emitter_firmware_version,
                      source->ir_protocol,
                      source->ir_frame_delay,
                      source->ir_frame_duration,
                      source->ir_signal_spacing,
                      source->opt_block_signal_detection_delay,
                      source->opt_min_threshold_value_to_activate,
                      source->opt_detection_threshold_high,
                      source->opt_enable_ignore_during_ir,
                      source->opt_enable_duplicate_realtime_reporting,
                      source->opt_output_stats,
                      source->opt_ignore_all_duplicates,
                      source->opt_sensor_filter_mode,
                      source->ir_flip_eyes,
                      source->opt_detection_threshold_low,
                      source->ir_average_timing_mode,
                      source->target_frametime,
                      source->ir_drive_mode);
    if (bn < 0 || (size_t)bn >= sizeof(body))
        return false;

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    bool ok = Open3DWriteAll(fd, body, (size_t)bn);
    if (close(fd) != 0)
        ok = false;
    if (!ok)
    {
        unlink(temp_path);
        return false;
    }
    if (rename(temp_path, backup_path) != 0)
    {
        unlink(temp_path);
        return false;
    }

    msg_Dbg(vd, "emitter firmware preflight backup saved: %s", backup_path);
    return true;
}

static void Open3DEmitterStartFirmwareUpdate(vout_display_t *vd, vout_display_sys_t *sys,
                                             vlc_tick_t now)
{
    if (sys->emitter_fw_in_progress)
    {
        msg_Warn(vd, "firmware update already running");
        return;
    }
    if (sys->emitter_fw_helper == NULL || sys->emitter_fw_helper[0] == '\0')
    {
        msg_Warn(vd, "firmware update helper path is empty");
        return;
    }
    if (sys->emitter_fw_hex == NULL || sys->emitter_fw_hex[0] == '\0')
    {
        msg_Warn(vd, "firmware hex path is empty");
        return;
    }

    char *port = NULL;
    if (sys->emitter_tty_selected != NULL && sys->emitter_tty_selected[0] != '\0')
        port = strdup(sys->emitter_tty_selected);
    else if (!sys->emitter_tty_auto && sys->emitter_tty != NULL && sys->emitter_tty[0] != '\0')
        port = strdup(sys->emitter_tty);
    else
        port = Open3DEmitterDetectAutoTty(vd, sys);
    if (port == NULL || port[0] == '\0')
    {
        msg_Warn(vd, "firmware update preflight failed: no tty selected/detected");
        free(port);
        return;
    }

    if (sys->emitter_settings_dirty)
    {
        msg_Warn(vd, "firmware update preflight: settings are dirty (device != desired)");
    }
    if (!Open3DEmitterWriteFirmwareBackup(vd, sys, sys->emitter_fw_backup_json_path))
    {
        msg_Warn(vd, "firmware update preflight: failed to write backup JSON %s",
                 sys->emitter_fw_backup_json_path != NULL ?
                 sys->emitter_fw_backup_json_path : "(null)");
    }

    if (sys->emitter_device_settings_valid)
    {
        Open3DEmitterSettingsCopy(&sys->emitter_settings, &sys->emitter_device_settings);
        Open3DEmitterWriteSettingsJson(vd, sys);
    }

    Open3DEmitterClose(sys);

    pid_t pid = fork();
    if (pid < 0)
    {
        msg_Err(vd, "failed to fork firmware helper: %s", vlc_strerror_c(errno));
        free(port);
        return;
    }
    if (pid == 0)
    {
        execlp(sys->emitter_fw_helper, sys->emitter_fw_helper,
               "--port", port,
               "--hex", sys->emitter_fw_hex,
               (char *)NULL);
        _exit(127);
    }

    sys->emitter_fw_pid = pid;
    sys->emitter_fw_in_progress = true;
    sys->emitter_fw_started = now == VLC_TICK_INVALID ? mdate() : now;
    sys->emitter_fw_reapply_pending = false;
    msg_Dbg(vd, "firmware helper started: pid=%ld port=%s helper=%s",
            (long)pid, port, sys->emitter_fw_helper);
    free(port);
}

static void Open3DEmitterPollFirmwareUpdate(vout_display_t *vd, vout_display_sys_t *sys,
                                            vlc_tick_t now)
{
    if (!sys->emitter_fw_in_progress || sys->emitter_fw_pid <= 0)
        return;

    int status = 0;
    pid_t waited = waitpid(sys->emitter_fw_pid, &status, WNOHANG);
    if (waited == 0)
        return;
    if (waited < 0)
    {
        if (errno == EINTR)
            return;
        msg_Warn(vd, "firmware helper wait failed: %s", vlc_strerror_c(errno));
        sys->emitter_fw_in_progress = false;
        sys->emitter_fw_pid = 0;
        sys->emitter_next_reconnect = VLC_TICK_INVALID;
        return;
    }

    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok)
    {
        msg_Dbg(vd, "firmware helper finished successfully");
        if (sys->emitter_fw_reapply)
            sys->emitter_fw_reapply_pending = true;
    }
    else
    {
        if (WIFEXITED(status))
            msg_Warn(vd, "firmware helper failed with exit code %d", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            msg_Warn(vd, "firmware helper terminated by signal %d", WTERMSIG(status));
        else
            msg_Warn(vd, "firmware helper failed");
    }

    sys->emitter_fw_in_progress = false;
    sys->emitter_fw_pid = 0;
    sys->emitter_next_reconnect = VLC_TICK_INVALID;
    Open3DEmitterMaybeReconnect(vd, sys, now);
}

static void Open3DEmitterDrainControlRequests(vout_display_t *vd, vout_display_sys_t *sys,
                                              vlc_tick_t now)
{
    bool req_read = false;
    bool req_apply = false;
    bool req_save = false;
    bool req_reconnect = false;
    bool req_fw_update = false;

    vlc_mutex_lock(&sys->emitter_control_lock);
    req_read = sys->emitter_req_read;
    req_apply = sys->emitter_req_apply;
    req_save = sys->emitter_req_save;
    req_reconnect = sys->emitter_req_reconnect;
    req_fw_update = sys->emitter_req_fw_update;
    sys->emitter_req_read = false;
    sys->emitter_req_apply = false;
    sys->emitter_req_save = false;
    sys->emitter_req_reconnect = false;
    sys->emitter_req_fw_update = false;
    vlc_mutex_unlock(&sys->emitter_control_lock);

    if (req_reconnect)
    {
        if (sys->emitter_fw_in_progress)
            msg_Warn(vd, "reconnect request ignored: firmware update is running");
        else
        {
            Open3DEmitterClose(sys);
            sys->emitter_next_reconnect = VLC_TICK_INVALID;
            Open3DEmitterMaybeReconnect(vd, sys, now);
        }
    }

    if (req_read)
    {
        if (sys->emitter_fd >= 0)
            Open3DEmitterQueueCommand(vd, sys, "0", true, VLC_TICK_FROM_MS(5000), "read-manual");
        else
            sys->emitter_pending_read = true;
    }

    if (req_apply)
    {
        if (sys->emitter_fd >= 0)
        {
            Open3DEmitterQueueApplySettings(vd, sys);
            if (sys->emitter_save_on_apply)
                Open3DEmitterQueueCommand(vd, sys, "8", true, VLC_TICK_FROM_MS(5000), "save-after-apply");
        }
        else
        {
            sys->emitter_pending_apply = true;
            if (sys->emitter_save_on_apply)
                sys->emitter_pending_save = true;
        }
    }

    if (req_save)
    {
        if (sys->emitter_fd >= 0)
            Open3DEmitterQueueCommand(vd, sys, "8", true, VLC_TICK_FROM_MS(5000), "save-manual");
        else
            sys->emitter_pending_save = true;
    }
    if (req_fw_update)
        Open3DEmitterStartFirmwareUpdate(vd, sys, now);
}

static void Open3DEmitterResetQueue(vout_display_sys_t *sys)
{
    sys->emitter_cmd_head = 0;
    sys->emitter_cmd_tail = 0;
    sys->emitter_cmd_waiting_ok = false;
    memset(&sys->emitter_cmd_inflight, 0, sizeof(sys->emitter_cmd_inflight));
    sys->emitter_cmd_deadline = VLC_TICK_INVALID;
    sys->emitter_rx_len = 0;
    memset(sys->emitter_rx_line, 0, sizeof(sys->emitter_rx_line));
}

static void Open3DEmitterClose(vout_display_sys_t *sys)
{
    bool was_connected = false;
    if (sys->emitter_fd >= 0)
    {
        was_connected = true;
        close(sys->emitter_fd);
        sys->emitter_fd = -1;
    }
    sys->emitter_last_eye_valid = false;
    Open3DEmitterResetQueue(sys);
    if (was_connected)
        Open3DOverlayStateBump(sys);
}

static bool Open3DEmitterQueueCommand(vout_display_t *vd, vout_display_sys_t *sys,
                                      const char *command, bool wait_ok,
                                      vlc_tick_t timeout, const char *tag)
{
    if (command == NULL || command[0] == '\0')
        return false;

    unsigned next = (sys->emitter_cmd_head + 1) % OPEN3D_EMITTER_CMD_QUEUE;
    if (next == sys->emitter_cmd_tail)
    {
        if (!sys->warned_emitter_queue)
        {
            msg_Warn(vd, "emitter command queue full, dropping oldest command");
            sys->warned_emitter_queue = true;
        }
        sys->emitter_cmd_tail = (sys->emitter_cmd_tail + 1) % OPEN3D_EMITTER_CMD_QUEUE;
    }
    else
    {
        sys->warned_emitter_queue = false;
    }

    open3d_emitter_cmd_t *slot = &sys->emitter_cmd_queue[sys->emitter_cmd_head];
    memset(slot, 0, sizeof(*slot));
    slot->wait_ok = wait_ok;
    slot->timeout = timeout;
    slot->tag = tag;
    int written = snprintf(slot->command, sizeof(slot->command), "%s", command);
    if (written < 0)
        return false;
    slot->len = (size_t)written;
    if (slot->len == 0 || slot->len >= sizeof(slot->command))
        return false;
    if (slot->command[slot->len - 1] != '\n')
    {
        if (slot->len + 1 >= sizeof(slot->command))
            return false;
        slot->command[slot->len++] = '\n';
        slot->command[slot->len] = '\0';
    }

    sys->emitter_cmd_head = next;
    return true;
}

static bool Open3DEmitterPopCommand(vout_display_sys_t *sys, open3d_emitter_cmd_t *cmd)
{
    if (sys->emitter_cmd_tail == sys->emitter_cmd_head)
        return false;

    *cmd = sys->emitter_cmd_queue[sys->emitter_cmd_tail];
    sys->emitter_cmd_tail = (sys->emitter_cmd_tail + 1) % OPEN3D_EMITTER_CMD_QUEUE;
    return true;
}

static void Open3DEmitterQueueApplySettings(vout_display_t *vd, vout_display_sys_t *sys)
{
    char command[OPEN3D_EMITTER_CMD_MAX];
    const open3d_emitter_settings_t *s = &sys->emitter_settings;
    int written = snprintf(command, sizeof(command),
                           "0,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                           s->ir_protocol,
                           s->ir_frame_delay,
                           s->ir_frame_duration,
                           s->ir_signal_spacing,
                           s->opt_block_signal_detection_delay,
                           s->opt_min_threshold_value_to_activate,
                           s->opt_detection_threshold_high,
                           s->opt_enable_ignore_during_ir,
                           s->opt_enable_duplicate_realtime_reporting,
                           s->opt_output_stats,
                           s->opt_ignore_all_duplicates,
                           s->opt_sensor_filter_mode,
                           s->ir_flip_eyes,
                           s->opt_detection_threshold_low,
                           s->ir_average_timing_mode,
                           s->target_frametime,
                           s->ir_drive_mode);
    if (written < 0 || (size_t)written >= sizeof(command))
        return;

    Open3DEmitterQueueCommand(vd, sys, command, true, VLC_TICK_FROM_MS(5000), "apply");
    if (sys->emitter_device_settings_valid)
    {
        sys->emitter_settings_dirty = !Open3DEmitterSettingsEqual(&sys->emitter_settings,
                                                                   &sys->emitter_device_settings);
    }
    Open3DEmitterWriteSettingsJson(vd, sys);
}

static void Open3DEmitterQueueStartupCommands(vout_display_t *vd, vout_display_sys_t *sys)
{
    bool need_read = sys->emitter_read_on_connect || sys->emitter_pending_read;
    bool need_apply = sys->emitter_apply_on_connect || sys->emitter_pending_apply;
    bool need_save = sys->emitter_pending_save;

    if (need_read)
        Open3DEmitterQueueCommand(vd, sys, "0", true, VLC_TICK_FROM_MS(5000), "read");
    if (need_apply)
    {
        Open3DEmitterQueueApplySettings(vd, sys);
        if (sys->emitter_save_on_apply)
            need_save = true;
    }
    if (need_save)
        Open3DEmitterQueueCommand(vd, sys, "8", true, VLC_TICK_FROM_MS(5000), "save");

    sys->emitter_pending_read = false;
    sys->emitter_pending_apply = false;
    sys->emitter_pending_save = false;
}

static int Open3DEmitterOpen(vout_display_t *vd, vout_display_sys_t *sys, const char *tty_path)
{
    if (tty_path == NULL || tty_path[0] == '\0')
        return VLC_EGENERIC;

    speed_t speed = Open3DEmitterSpeedFromBaud(sys->emitter_baud);
    if (speed == B0)
    {
        msg_Err(vd, "unsupported emitter baud rate: %d", sys->emitter_baud);
        return VLC_EGENERIC;
    }

    int fd = open(tty_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        msg_Warn(vd, "failed to open emitter tty %s: %s", tty_path, vlc_strerror_c(errno));
        return VLC_EGENERIC;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0)
    {
        msg_Warn(vd, "tcgetattr failed on emitter tty %s: %s", tty_path, vlc_strerror_c(errno));
        close(fd);
        return VLC_EGENERIC;
    }

    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;
#endif
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0)
    {
        msg_Warn(vd, "failed to set emitter tty speed %d on %s: %s",
                 sys->emitter_baud, tty_path, vlc_strerror_c(errno));
        close(fd);
        return VLC_EGENERIC;
    }

    if (tcsetattr(fd, TCSANOW, &tio) != 0)
    {
        msg_Warn(vd, "tcsetattr failed on emitter tty %s: %s", tty_path, vlc_strerror_c(errno));
        close(fd);
        return VLC_EGENERIC;
    }

    tcflush(fd, TCIOFLUSH);
    sys->emitter_fd = fd;
    free(sys->emitter_tty_selected);
    sys->emitter_tty_selected = strdup(tty_path);
    sys->warned_emitter_open = false;
    sys->warned_emitter_detect = false;
    sys->emitter_next_reconnect = VLC_TICK_INVALID;
    sys->emitter_last_eye_valid = false;
    Open3DEmitterResetQueue(sys);
    Open3DEmitterQueueStartupCommands(vd, sys);
    if (sys->emitter_fw_reapply_pending)
    {
        Open3DEmitterQueueApplySettings(vd, sys);
        Open3DEmitterQueueCommand(vd, sys, "8", true, VLC_TICK_FROM_MS(5000),
                                  "save-after-firmware");
        Open3DEmitterQueueCommand(vd, sys, "0", true, VLC_TICK_FROM_MS(5000),
                                  "read-after-firmware");
        sys->emitter_fw_reapply_pending = false;
    }

    if (sys->emitter_log_io)
        msg_Dbg(vd, "emitter connected: tty=%s baud=%d", tty_path, sys->emitter_baud);
    Open3DOverlayStateBump(sys);
    return VLC_SUCCESS;
}

static void Open3DEmitterScheduleReconnect(vout_display_sys_t *sys, vlc_tick_t now)
{
    if (!sys->emitter_auto_reconnect)
        return;
    if (now == VLC_TICK_INVALID)
        now = mdate();
    sys->emitter_next_reconnect = now + sys->emitter_reconnect_interval;
}

static void Open3DEmitterHandleWriteFailure(vout_display_t *vd, vout_display_sys_t *sys,
                                            const char *what, int io_errno, vlc_tick_t now)
{
    msg_Warn(vd, "emitter %s failed on %s: %s",
             what,
             sys->emitter_tty_selected != NULL ? sys->emitter_tty_selected : "(null)",
             vlc_strerror_c(io_errno));
    Open3DEmitterClose(sys);
    Open3DEmitterScheduleReconnect(sys, now);
}

static void Open3DEmitterMaybeReconnect(vout_display_t *vd, vout_display_sys_t *sys,
                                        vlc_tick_t now)
{
    if (!sys->emitter_enable || sys->emitter_fd >= 0 || sys->emitter_fw_in_progress)
        return;

    if (now == VLC_TICK_INVALID)
        now = mdate();
    if (sys->emitter_next_reconnect != VLC_TICK_INVALID && now < sys->emitter_next_reconnect)
        return;

    char *candidate = NULL;
    if (sys->emitter_tty_auto)
        candidate = Open3DEmitterDetectAutoTty(vd, sys);
    else if (sys->emitter_tty != NULL && sys->emitter_tty[0] != '\0')
        candidate = strdup(sys->emitter_tty);

    if (candidate == NULL || candidate[0] == '\0')
    {
        if (!sys->warned_emitter_detect)
        {
            msg_Warn(vd, "no emitter tty detected");
            sys->warned_emitter_detect = true;
        }
        free(candidate);
        Open3DEmitterScheduleReconnect(sys, now);
        return;
    }

    if (Open3DEmitterOpen(vd, sys, candidate) != VLC_SUCCESS)
    {
        if (!sys->warned_emitter_open)
        {
            msg_Warn(vd, "unable to open emitter tty %s (baud %d)", candidate, sys->emitter_baud);
            sys->warned_emitter_open = true;
        }
        Open3DEmitterScheduleReconnect(sys, now);
    }
    free(candidate);
}

static void Open3DEmitterHandleLine(vout_display_t *vd, vout_display_sys_t *sys, const char *line)
{
    if (line == NULL || line[0] == '\0')
        return;

    if (sys->emitter_log_io)
        msg_Dbg(vd, "emitter rx: %s", line);

    if (!strcmp(line, "OK"))
    {
        if (sys->emitter_cmd_waiting_ok)
        {
            const char *tag = sys->emitter_cmd_inflight.tag;
            if (sys->emitter_log_io)
            {
                msg_Dbg(vd, "emitter cmd OK: %s",
                        sys->emitter_cmd_inflight.tag != NULL ?
                        sys->emitter_cmd_inflight.tag : "unnamed");
            }
            sys->emitter_cmd_waiting_ok = false;
            sys->emitter_cmd_deadline = VLC_TICK_INVALID;

            if (tag != NULL && !strcmp(tag, "apply"))
            {
                Open3DEmitterQueueCommand(vd, sys, "0", true, VLC_TICK_FROM_MS(5000),
                                          "read-after-apply");
            }
        }
        return;
    }

    if (line[0] == '+')
    {
        Open3DEmitterLogOptCsv(vd, sys, line);
        return;
    }

    if (!strncasecmp(line, "parameters", 10))
    {
        const char *payload = line + 10;
        while (*payload == ' ' || *payload == '\t' || *payload == ':')
            payload++;

        open3d_emitter_settings_t parsed;
        int firmware = 0;
        if (Open3DEmitterParseParameters(payload, &firmware, &parsed))
        {
            if (firmware > 0 && firmware != sys->emitter_firmware_version)
            {
                sys->emitter_firmware_version = firmware;
                msg_Dbg(vd, "emitter firmware detected: %d", firmware);
            }
            Open3DEmitterSettingsCopy(&sys->emitter_device_settings, &parsed);
            sys->emitter_device_settings_valid = true;
            Open3DEmitterUpdateDirtyState(vd, sys, "parameters");
            Open3DOverlayStateBump(sys);
        }
        else
        {
            errno = 0;
            char *end = NULL;
            long fw = strtol(payload, &end, 10);
            if (end != payload && errno == 0)
            {
                int fallback_firmware = (int)fw;
                if (fallback_firmware > 0 &&
                    fallback_firmware != sys->emitter_firmware_version)
                {
                    sys->emitter_firmware_version = fallback_firmware;
                    msg_Dbg(vd, "emitter firmware detected: %d", fallback_firmware);
                }
            }
        }
        return;
    }
}

static void Open3DEmitterPumpRx(vout_display_t *vd, vout_display_sys_t *sys, vlc_tick_t now)
{
    if (sys->emitter_fd < 0)
        return;

    char buf[128];
    for (int iter = 0; iter < 2; ++iter)
    {
        ssize_t n = read(sys->emitter_fd, buf, sizeof(buf));
        if (n == 0)
            return;

        if (n < 0)
        {
            if (Open3DEmitterWouldBlock(errno))
                return;
            Open3DEmitterHandleWriteFailure(vd, sys, "read", errno, now);
            return;
        }

        for (ssize_t i = 0; i < n; ++i)
        {
            char c = buf[i];
            if (c == '\r')
                continue;
            if (c == '\n')
            {
                sys->emitter_rx_line[sys->emitter_rx_len] = '\0';
                Open3DEmitterHandleLine(vd, sys, sys->emitter_rx_line);
                sys->emitter_rx_len = 0;
                continue;
            }

            if (sys->emitter_rx_len + 1 < sizeof(sys->emitter_rx_line))
                sys->emitter_rx_line[sys->emitter_rx_len++] = c;
            else
                sys->emitter_rx_len = 0;
        }
    }
}

static void Open3DEmitterPumpCommandQueue(vout_display_t *vd, vout_display_sys_t *sys,
                                          vlc_tick_t now)
{
    if (sys->emitter_fd < 0)
        return;

    if (now == VLC_TICK_INVALID)
        now = mdate();

    if (sys->emitter_cmd_waiting_ok)
    {
        if (sys->emitter_cmd_deadline != VLC_TICK_INVALID && now >= sys->emitter_cmd_deadline)
        {
            msg_Warn(vd, "emitter command timeout: %s",
                     sys->emitter_cmd_inflight.tag != NULL ?
                     sys->emitter_cmd_inflight.tag : "unnamed");
            sys->emitter_cmd_waiting_ok = false;
            sys->emitter_cmd_deadline = VLC_TICK_INVALID;
        }
        return;
    }

    open3d_emitter_cmd_t cmd;
    if (!Open3DEmitterPopCommand(sys, &cmd))
        return;

    ssize_t written = write(sys->emitter_fd, cmd.command, cmd.len);
    if (written != (ssize_t)cmd.len)
    {
        if (written < 0 && Open3DEmitterWouldBlock(errno))
        {
            /* Requeue command at head-equivalent position by rolling tail back. */
            sys->emitter_cmd_tail = (sys->emitter_cmd_tail + OPEN3D_EMITTER_CMD_QUEUE - 1)
                                    % OPEN3D_EMITTER_CMD_QUEUE;
            sys->emitter_cmd_queue[sys->emitter_cmd_tail] = cmd;
            return;
        }

        int io_errno = written < 0 ? errno : EIO;
        Open3DEmitterHandleWriteFailure(vd, sys, "command write", io_errno, now);
        return;
    }

    if (sys->emitter_log_io)
        msg_Dbg(vd, "emitter tx: %s", cmd.command);

    if (cmd.wait_ok)
    {
        sys->emitter_cmd_waiting_ok = true;
        sys->emitter_cmd_inflight = cmd;
        sys->emitter_cmd_deadline = now + cmd.timeout;
    }
}

static void Open3DEmitterPump(vout_display_t *vd, vout_display_sys_t *sys, vlc_tick_t now)
{
    if (!sys->emitter_enable)
        return;
    if (now == VLC_TICK_INVALID)
        now = mdate();

    bool urgent = false;
    vlc_mutex_lock(&sys->emitter_control_lock);
    urgent = sys->emitter_req_read || sys->emitter_req_apply || sys->emitter_req_save ||
             sys->emitter_req_reconnect || sys->emitter_req_fw_update;
    vlc_mutex_unlock(&sys->emitter_control_lock);

    if (!urgent &&
        sys->emitter_service_interval > 0 &&
        sys->emitter_next_service != VLC_TICK_INVALID &&
        now < sys->emitter_next_service)
    {
        return;
    }

    Open3DEmitterDrainControlRequests(vd, sys, now);
    Open3DEmitterPollFirmwareUpdate(vd, sys, now);
    Open3DEmitterMaybeReconnect(vd, sys, now);
    Open3DEmitterPumpRx(vd, sys, now);
    Open3DEmitterPumpCommandQueue(vd, sys, now);

    if (sys->emitter_service_interval > 0)
        sys->emitter_next_service = now + sys->emitter_service_interval;
    else
        sys->emitter_next_service = VLC_TICK_INVALID;
}

static void Open3DEmitterSendEye(vout_display_t *vd, vout_display_sys_t *sys,
                                 bool show_right_eye, vlc_tick_t now)
{
    if (!sys->emitter_enable || sys->emitter_fd < 0)
        return;
    if (Open3DCurrentDriveMode(sys) != 1)
        return;
    if (sys->emitter_last_eye_valid && sys->emitter_last_eye == show_right_eye)
        return;

    const char cmd[] = "9,0\n";
    char out[sizeof(cmd)];
    memcpy(out, cmd, sizeof(cmd));
    out[2] = show_right_eye ? '1' : '0';

    ssize_t written = write(sys->emitter_fd, out, sizeof(out) - 1);
    if (written != (ssize_t)(sizeof(out) - 1))
    {
        if (written < 0 && Open3DEmitterWouldBlock(errno))
            return;

        int io_errno = written < 0 ? errno : EIO;
        Open3DEmitterHandleWriteFailure(vd, sys, "eye write", io_errno, now);
        return;
    }

    sys->emitter_last_eye = show_right_eye;
    sys->emitter_last_eye_valid = true;
    if (sys->emitter_log_io)
        msg_Dbg(vd, "emitter eye command: %c", show_right_eye ? 'R' : 'L');
}

static void *Open3DEmitterThread(void *data)
{
    vout_display_t *vd = data;
    vout_display_sys_t *sys = vd->sys;

    for (;;)
    {
        bool eye_pending = false;
        bool eye_reset = false;
        bool eye = false;
        vlc_tick_t eye_clock = VLC_TICK_INVALID;

        vlc_mutex_lock(&sys->emitter_control_lock);
        while (!sys->emitter_stop)
        {
            const vlc_tick_t now = mdate();
            const bool urgent = sys->emitter_eye_pending || sys->emitter_eye_reset ||
                                sys->emitter_req_read || sys->emitter_req_apply ||
                                sys->emitter_req_save || sys->emitter_req_reconnect ||
                                sys->emitter_req_fw_update;

            vlc_tick_t deadline = now + VLC_TICK_FROM_MS(1);
            if (sys->emitter_service_interval > 0)
            {
                if (sys->emitter_next_service != VLC_TICK_INVALID)
                    deadline = sys->emitter_next_service;
                else
                    deadline = now;
            }

            if (urgent || now >= deadline)
                break;

            vlc_cond_timedwait(&sys->emitter_cond, &sys->emitter_control_lock, deadline);
        }

        if (sys->emitter_stop)
        {
            vlc_mutex_unlock(&sys->emitter_control_lock);
            break;
        }

        eye_reset = sys->emitter_eye_reset;
        sys->emitter_eye_reset = false;
        eye_pending = sys->emitter_eye_pending;
        eye = sys->emitter_eye;
        eye_clock = sys->emitter_eye_clock;
        sys->emitter_eye_pending = false;
        vlc_mutex_unlock(&sys->emitter_control_lock);

        if (eye_reset)
            sys->emitter_last_eye_valid = false;
        if (eye_pending)
            Open3DEmitterSendEye(vd, sys, eye, eye_clock);

        Open3DEmitterPump(vd, sys, mdate());
    }

    return NULL;
}

static void Open3DApplyHotkeyRequests(vout_display_t *vd, vout_display_sys_t *sys)
{
    const vlc_tick_t now = mdate();
    bool req_toggle_enabled = false;
    bool req_toggle_osd = false;
    bool req_toggle_calibration = false;
    bool req_help = false;
    bool req_flip_eyes = false;
    bool req_emitter_read = false;
    bool req_emitter_apply = false;
    bool req_emitter_save = false;
    bool req_emitter_reconnect = false;
    bool req_emitter_fw_update = false;
    bool req_calib_help_toggle = false;
    bool req_calib_drive_toggle = false;
    bool req_calib_save = false;
    bool req_calib_optlog_toggle = false;
    int req_calib_border_delta = 0;
    int req_offset_x_delta = 0;
    int req_offset_y_delta = 0;
    int req_trigger_size_delta = 0;
    int req_trigger_spacing_delta = 0;
    int req_ir_frame_delay_delta = 0;
    int req_ir_frame_duration_delta = 0;
    bool wakeup_emitter = false;
    bool presenter_state_changed = false;
    bool overlay_state_changed = false;

    vlc_mutex_lock(&sys->hotkey_lock);
    req_toggle_enabled = sys->hotkey_req_toggle_enabled;
    req_toggle_osd = sys->hotkey_req_toggle_trigger;
    req_toggle_calibration = sys->hotkey_req_toggle_calibration;
    req_help = sys->hotkey_req_help;
    req_flip_eyes = sys->hotkey_req_flip_eyes;
    req_emitter_read = sys->hotkey_req_emitter_read;
    req_emitter_apply = sys->hotkey_req_emitter_apply;
    req_emitter_save = sys->hotkey_req_emitter_save;
    req_emitter_reconnect = sys->hotkey_req_emitter_reconnect;
    req_emitter_fw_update = sys->hotkey_req_emitter_fw_update;
    req_calib_help_toggle = sys->hotkey_req_calib_help_toggle;
    req_calib_drive_toggle = sys->hotkey_req_calib_drive_toggle;
    req_calib_save = sys->hotkey_req_calib_save;
    req_calib_optlog_toggle = sys->hotkey_req_calib_optlog_toggle;
    req_calib_border_delta = sys->hotkey_req_calib_border_delta;
    req_offset_x_delta = sys->hotkey_req_offset_x_delta;
    req_offset_y_delta = sys->hotkey_req_offset_y_delta;
    req_trigger_size_delta = sys->hotkey_req_trigger_size_delta;
    req_trigger_spacing_delta = sys->hotkey_req_trigger_spacing_delta;
    req_ir_frame_delay_delta = sys->hotkey_req_ir_frame_delay_delta;
    req_ir_frame_duration_delta = sys->hotkey_req_ir_frame_duration_delta;

    sys->hotkey_req_toggle_enabled = false;
    sys->hotkey_req_toggle_trigger = false;
    sys->hotkey_req_toggle_calibration = false;
    sys->hotkey_req_help = false;
    sys->hotkey_req_flip_eyes = false;
    sys->hotkey_req_emitter_read = false;
    sys->hotkey_req_emitter_apply = false;
    sys->hotkey_req_emitter_save = false;
    sys->hotkey_req_emitter_reconnect = false;
    sys->hotkey_req_emitter_fw_update = false;
    sys->hotkey_req_calib_help_toggle = false;
    sys->hotkey_req_calib_drive_toggle = false;
    sys->hotkey_req_calib_save = false;
    sys->hotkey_req_calib_optlog_toggle = false;
    sys->hotkey_req_calib_border_delta = 0;
    sys->hotkey_req_offset_x_delta = 0;
    sys->hotkey_req_offset_y_delta = 0;
    sys->hotkey_req_trigger_size_delta = 0;
    sys->hotkey_req_trigger_spacing_delta = 0;
    sys->hotkey_req_trigger_brightness_delta = 0;
    sys->hotkey_req_ir_frame_delay_delta = 0;
    sys->hotkey_req_ir_frame_duration_delta = 0;
    vlc_mutex_unlock(&sys->hotkey_lock);

    if (req_toggle_enabled)
    {
        sys->enabled = !sys->enabled;
        presenter_state_changed = true;
        overlay_state_changed = true;
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               "Open3D %s", sys->enabled ? "enabled" : "disabled");
    }
    if (req_toggle_osd)
    {
        sys->status_overlay_visible = !sys->status_overlay_visible;
        overlay_state_changed = true;
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               "OSD %s", sys->status_overlay_visible ? "on" : "off");
    }
    if (req_toggle_calibration)
    {
        sys->calibration_enable = !sys->calibration_enable;
        if (sys->calibration_enable)
            sys->status_calibration_help = true;
        overlay_state_changed = true;
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               "Calibration mode %s", sys->calibration_enable ? "on" : "off");
    }
    if (req_help)
    {
        Open3DStatusShowHelp(sys, false, now);
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               "Status key pressed: use calibration mode for detailed calibration hotkeys");
    }
    if (req_flip_eyes)
    {
        sys->swap_eyes = !sys->swap_eyes;
        presenter_state_changed = true;
        overlay_state_changed = true;
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               "Swap eyes %s", sys->swap_eyes ? "enabled" : "disabled");
    }

    const bool have_calib_action =
        req_calib_help_toggle || req_calib_drive_toggle || req_calib_save || req_calib_optlog_toggle ||
        req_calib_border_delta != 0 || req_offset_x_delta != 0 || req_offset_y_delta != 0 ||
        req_trigger_size_delta != 0 || req_trigger_spacing_delta != 0 ||
        req_ir_frame_delay_delta != 0 || req_ir_frame_duration_delta != 0;
    const bool calibration_mode = sys->calibration_enable;

    if (have_calib_action && !calibration_mode)
    {
        char k_calib[96];
        Open3DHotkeyName(sys->hotkey_toggle_calibration, k_calib, sizeof(k_calib));
        Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                               "Calibration mode off (%s)", k_calib);
    }

    if (calibration_mode)
    {
        if (req_calib_help_toggle)
        {
            sys->status_calibration_help = !sys->status_calibration_help;
            overlay_state_changed = true;
            Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                   "Calibration help %s",
                                   sys->status_calibration_help ? "on" : "off");
        }

        if (req_calib_drive_toggle)
        {
            if (sys->emitter_fd < 0)
            {
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Drive mode toggle requires emitter connection");
            }
            else
            {
                sys->emitter_settings.ir_drive_mode =
                    (sys->emitter_settings.ir_drive_mode == 1) ? 0 : 1;
                Open3DEmitterUpdateDirtyState(vd, sys, "hotkey-drive-toggle");
                overlay_state_changed = true;
                vlc_mutex_lock(&sys->emitter_control_lock);
                sys->emitter_req_apply = true;
                if (sys->emitter_save_on_apply)
                    sys->emitter_req_save = true;
                vlc_mutex_unlock(&sys->emitter_control_lock);
                wakeup_emitter = true;
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Drive mode: %s (apply queued)",
                                       sys->emitter_settings.ir_drive_mode == 1
                                           ? "serial" : "optical");
            }
        }

        if (req_calib_save)
        {
            if (sys->emitter_fd < 0)
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Save EEPROM requires emitter connection");
            else
                req_emitter_save = true;
        }

        const bool serial_mode = Open3DCurrentDriveMode(sys) == 1;
        if (req_calib_optlog_toggle)
        {
            if (serial_mode)
            {
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Opt debug logging unavailable in serial mode");
            }
            else if (sys->emitter_fd < 0)
            {
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Opt debug logging requires emitter connection");
            }
            else
            {
                sys->emitter_opt_csv_enable = !sys->emitter_opt_csv_enable;
                if (!sys->emitter_opt_csv_enable)
                    Open3DEmitterCloseOptCsv(sys);
                else
                    Open3DEmitterEnsureOptCsv(vd, sys);
                overlay_state_changed = true;
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Opt debug logging %s",
                                       sys->emitter_opt_csv_enable ? "on" : "off");
            }
        }

        if (!serial_mode)
        {
            if (req_calib_border_delta != 0)
            {
                int border = (int)sys->trigger_black_border + req_calib_border_delta;
                sys->trigger_black_border = (unsigned)Open3DClampInt(border, 0, 1024);
                overlay_state_changed = true;
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Black border: %u", sys->trigger_black_border);
            }
            if (req_offset_x_delta != 0)
            {
                int val = sys->trigger_offset_x + req_offset_x_delta;
                sys->trigger_offset_x = Open3DClampInt(val, -8192, 8192);
                overlay_state_changed = true;
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Horizontal position: %d", sys->trigger_offset_x);
            }
            if (req_offset_y_delta != 0)
            {
                int val = sys->trigger_offset_y + req_offset_y_delta;
                sys->trigger_offset_y = Open3DClampInt(val, -8192, 8192);
                overlay_state_changed = true;
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Vertical position: %d", sys->trigger_offset_y);
            }
            if (req_trigger_spacing_delta != 0)
            {
                int val = (int)sys->trigger_spacing + req_trigger_spacing_delta;
                sys->trigger_spacing = (unsigned)Open3DClampInt(val, 0, 2048);
                overlay_state_changed = true;
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Spacing: %u", sys->trigger_spacing);
            }
            if (req_trigger_size_delta != 0)
            {
                int val = (int)sys->trigger_size + req_trigger_size_delta;
                sys->trigger_size = (unsigned)Open3DClampInt(val, 1, 512);
                overlay_state_changed = true;
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Whitebox size: %u", sys->trigger_size);
            }
        }

        if (req_ir_frame_delay_delta != 0 || req_ir_frame_duration_delta != 0)
        {
            bool changed = false;
            if (req_ir_frame_delay_delta != 0)
            {
                int val = sys->emitter_settings.ir_frame_delay + req_ir_frame_delay_delta;
                if (val < 0)
                    val = 0;
                sys->emitter_settings.ir_frame_delay = val;
                changed = true;
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Frame delay: %d", sys->emitter_settings.ir_frame_delay);
            }
            if (req_ir_frame_duration_delta != 0)
            {
                int val = sys->emitter_settings.ir_frame_duration + req_ir_frame_duration_delta;
                if (val < 0)
                    val = 0;
                sys->emitter_settings.ir_frame_duration = val;
                changed = true;
                Open3DStatusSetMessage(vd, sys, sys->status_osd_duration,
                                       "Frame duration: %d", sys->emitter_settings.ir_frame_duration);
            }

            if (changed)
            {
                Open3DEmitterUpdateDirtyState(vd, sys, "hotkey-ir-timing");
                if (sys->emitter_fd >= 0)
                {
                    vlc_mutex_lock(&sys->emitter_control_lock);
                    sys->emitter_req_apply = true;
                    vlc_mutex_unlock(&sys->emitter_control_lock);
                    wakeup_emitter = true;
                }
            }
        }
    }

    if (req_emitter_read || req_emitter_apply || req_emitter_save ||
        req_emitter_reconnect || req_emitter_fw_update)
    {
        vlc_mutex_lock(&sys->emitter_control_lock);
        if (req_emitter_read)
            sys->emitter_req_read = true;
        if (req_emitter_apply)
            sys->emitter_req_apply = true;
        if (req_emitter_save)
            sys->emitter_req_save = true;
        if (req_emitter_reconnect)
            sys->emitter_req_reconnect = true;
        if (req_emitter_fw_update)
            sys->emitter_req_fw_update = true;
        vlc_mutex_unlock(&sys->emitter_control_lock);
        wakeup_emitter = true;

        if (req_emitter_read)
            Open3DStatusSetMessage(vd, sys, sys->status_osd_duration, "Emitter read queued");
        if (req_emitter_apply)
            Open3DStatusSetMessage(vd, sys, sys->status_osd_duration, "Emitter apply queued");
        if (req_emitter_save)
            Open3DStatusSetMessage(vd, sys, sys->status_osd_duration, "Emitter save queued");
        if (req_emitter_reconnect)
            Open3DStatusSetMessage(vd, sys, sys->status_osd_duration, "Emitter reconnect queued");
        if (req_emitter_fw_update)
            Open3DStatusSetMessage(vd, sys, sys->status_osd_duration, "Emitter firmware update queued");
    }

    if (wakeup_emitter)
        Open3DEmitterWake(sys);

    if (presenter_state_changed)
        Open3DPresenterStatePublish(sys);
    if (overlay_state_changed)
        Open3DOverlayStateBump(sys);
}

static void *Open3DControlThread(void *data)
{
    vout_display_t *vd = data;
    vout_display_sys_t *sys = vd->sys;

    for (;;)
    {
        Open3DApplyHotkeyRequests(vd, sys);
        if (sys->emitter_enable)
            Open3DUpdateEmitterStatusWarnings(vd, sys);

        const vlc_tick_t deadline = mdate() + VLC_TICK_FROM_MS(20);
        vlc_mutex_lock(&sys->control_lock);
        if (sys->control_stop)
        {
            vlc_mutex_unlock(&sys->control_lock);
            break;
        }
        vlc_cond_timedwait(&sys->control_cond, &sys->control_lock, deadline);
        bool stop = sys->control_stop;
        vlc_mutex_unlock(&sys->control_lock);
        if (stop)
            break;
    }

    return NULL;
}

static int Open3DHotkeyVarCallback(vlc_object_t *obj, char const *name,
                                   vlc_value_t oldval, vlc_value_t newval,
                                   void *data)
{
    VLC_UNUSED(obj);
    VLC_UNUSED(oldval);

    if (name == NULL || strcmp(name, "key-pressed") || data == NULL)
        return VLC_SUCCESS;

    vout_display_sys_t *sys = data;
    if (!sys->hotkeys_enable)
        return VLC_SUCCESS;

    uint_fast32_t key = (uint_fast32_t)newval.i_int;
    if (key == KEY_UNSET)
        return VLC_SUCCESS;

    vlc_mutex_lock(&sys->hotkey_lock);
    if (Open3DHotkeyMatchExact(key, sys->hotkey_toggle_enabled))
        sys->hotkey_req_toggle_enabled = true;
    if (Open3DHotkeyMatchExact(key, sys->hotkey_toggle_trigger))
        sys->hotkey_req_toggle_trigger = true;
    if (Open3DHotkeyMatchExact(key, sys->hotkey_toggle_calibration))
        sys->hotkey_req_toggle_calibration = true;
    if (Open3DHotkeyMatchExact(key, sys->hotkey_help))
        sys->hotkey_req_help = true;
    if (Open3DHotkeyMatchExact(key, sys->hotkey_flip_eyes))
        sys->hotkey_req_flip_eyes = true;
    if (Open3DHotkeyMatchExact(key, sys->hotkey_emitter_read))
        sys->hotkey_req_emitter_read = true;
    if (Open3DHotkeyMatchExact(key, sys->hotkey_emitter_apply))
        sys->hotkey_req_emitter_apply = true;
    if (Open3DHotkeyMatchExact(key, sys->hotkey_emitter_save))
        sys->hotkey_req_emitter_save = true;
    if (Open3DHotkeyMatchExact(key, sys->hotkey_emitter_reconnect))
        sys->hotkey_req_emitter_reconnect = true;
    if (Open3DHotkeyMatchExact(key, sys->hotkey_emitter_fw_update))
        sys->hotkey_req_emitter_fw_update = true;

    int step = 0;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_g, &step))
        sys->hotkey_req_calib_help_toggle = true;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_t, &step))
        sys->hotkey_req_calib_drive_toggle = true;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_b, &step))
        sys->hotkey_req_calib_save = true;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_p, &step))
        sys->hotkey_req_calib_optlog_toggle = true;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_w, &step))
        sys->hotkey_req_offset_y_delta -= step;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_s, &step))
        sys->hotkey_req_offset_y_delta += step;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_a, &step))
        sys->hotkey_req_offset_x_delta -= step;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_d, &step))
        sys->hotkey_req_offset_x_delta += step;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_q, &step))
        sys->hotkey_req_trigger_spacing_delta -= (step >= 10) ? 5 : 1;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_e, &step))
        sys->hotkey_req_trigger_spacing_delta += (step >= 10) ? 5 : 1;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_n, &step))
        sys->hotkey_req_calib_border_delta -= (step >= 10) ? 5 : 1;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_m, &step))
        sys->hotkey_req_calib_border_delta += (step >= 10) ? 5 : 1;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_z, &step))
        sys->hotkey_req_trigger_size_delta -= step;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_x, &step))
        sys->hotkey_req_trigger_size_delta += step;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_i, &step))
        sys->hotkey_req_ir_frame_delay_delta -= (step >= 10) ? 200 : 10;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_k, &step))
        sys->hotkey_req_ir_frame_delay_delta += (step >= 10) ? 200 : 10;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_o, &step))
        sys->hotkey_req_ir_frame_duration_delta -= (step >= 10) ? 200 : 10;
    if (Open3DHotkeyMatchShiftStep(key, sys->hotkey_calib_l, &step))
        sys->hotkey_req_ir_frame_duration_delta += (step >= 10) ? 200 : 10;
    vlc_mutex_unlock(&sys->hotkey_lock);
    Open3DControlWake(sys);
    Open3DPresenterWake(sys);
    return VLC_SUCCESS;
}

static int Open3DEmitterVarCallback(vlc_object_t *obj, char const *name,
                                    vlc_value_t oldval, vlc_value_t newval,
                                    void *data)
{
    VLC_UNUSED(oldval);

    if (!newval.b_bool || name == NULL || data == NULL)
        return VLC_SUCCESS;

    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = data;
    bool matched = false;

    vlc_mutex_lock(&sys->emitter_control_lock);
    if (!strcmp(name, open3d_emitter_cmd_read_var))
    {
        sys->emitter_req_read = true;
        matched = true;
    }
    else if (!strcmp(name, open3d_emitter_cmd_apply_var))
    {
        sys->emitter_req_apply = true;
        matched = true;
    }
    else if (!strcmp(name, open3d_emitter_cmd_save_var))
    {
        sys->emitter_req_save = true;
        matched = true;
    }
    else if (!strcmp(name, open3d_emitter_cmd_reconnect_var))
    {
        sys->emitter_req_reconnect = true;
        matched = true;
    }
    else if (!strcmp(name, open3d_emitter_cmd_fw_update_var))
    {
        sys->emitter_req_fw_update = true;
        matched = true;
    }
    vlc_mutex_unlock(&sys->emitter_control_lock);

    if (matched)
        var_SetBool(vd, name, false);
    if (matched)
        Open3DEmitterWake(sys);
    if (matched)
        Open3DControlWake(sys);
    if (matched)
        Open3DPresenterWake(sys);
    return VLC_SUCCESS;
}

static int Open(vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = calloc(1, sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    sys->gl = NULL;
    sys->pool = NULL;
    sys->next_flip_deadline = VLC_TICK_INVALID;
    sys->presenter_stop = false;
    atomic_init(&sys->presenter_stop_atomic, false);
    sys->presenter_started = false;
    atomic_init(&sys->presenter_state_seq, 0);
    atomic_init(&sys->overlay_state_epoch, 1);
    sys->gpu_overlay_enable = false;
    sys->gpu_overlay_ready = false;
    memset(&sys->gl_overlay, 0, sizeof(sys->gl_overlay));
    sys->control_stop = false;
    sys->control_started = false;
    sys->presenter_picture = NULL;
    sys->presenter_subpicture = NULL;
    sys->presenter_generation = 0;
    sys->presenter_schedstat_fd = -1;
    sys->place_valid = false;
    sys->place_force = true;
    memset(&sys->last_place, 0, sizeof(sys->last_place));
    sys->presenter_rt_priority_request = -1;
    sys->presenter_affinity_cpu_request = -1;
    sys->presenter_prefault_kb_request = 0;
    sys->presenter_affinity_cpu = -1;
    sys->presenter_prefault_buffer = NULL;
    sys->emitter_stop = false;
    sys->emitter_started = false;
    sys->emitter_eye_pending = false;
    sys->emitter_eye = false;
    sys->emitter_eye_clock = VLC_TICK_INVALID;
    sys->emitter_eye_reset = false;
    sys->emitter_fd = -1;
    sys->emitter_tty = NULL;
    sys->emitter_next_reconnect = VLC_TICK_INVALID;
    sys->emitter_next_service = VLC_TICK_INVALID;
    sys->emitter_service_interval = VLC_TICK_FROM_MS(2);
    sys->emitter_settings_json_path = NULL;
    sys->emitter_opt_csv_path = NULL;
    sys->emitter_opt_csv_file = NULL;
    sys->emitter_fw_helper = NULL;
    sys->emitter_fw_hex = NULL;
    sys->emitter_fw_backup_json_path = NULL;
    vlc_mutex_init(&sys->presenter_lock);
    vlc_cond_init(&sys->presenter_cond);
    vlc_mutex_init(&sys->control_lock);
    vlc_cond_init(&sys->control_cond);
    vlc_mutex_init(&sys->gl_lock);
    vlc_cond_init(&sys->emitter_cond);
    vlc_mutex_init(&sys->emitter_control_lock);
    vlc_mutex_init(&sys->hotkey_lock);

    sys->enabled = var_InheritBool(vd, "open3d-enable");
    sys->swap_eyes = var_InheritBool(vd, "open3d-flip-eyes");
    sys->debug_status = var_InheritBool(vd, "open3d-debug-status");
    sys->subtitle_depth_enable = var_InheritBool(vd, "open3d-subtitle-depth-enable");
    sys->subtitle_depth_unit_px = var_InheritInteger(vd, "open3d-subtitle-depth-unit-px");
    sys->mkv_subtitle_static_offset_units = 0;
    sys->mkv_subtitle_force = false;
    sys->mkv_subtitle_plane = -1;
    sys->mkv_subtitle_source_id = -1;
    Open3DRefreshMkvSubtitleSettings(vd, sys);

    sys->trigger_enable = var_InheritBool(vd, "open3d-trigger-enable");
    int trigger_size = var_InheritInteger(vd, "open3d-trigger-size");
    int trigger_padding = var_InheritInteger(vd, "open3d-trigger-padding");
    int trigger_spacing = var_InheritInteger(vd, "open3d-trigger-spacing");
    int trigger_offset_x = var_InheritInteger(vd, "open3d-trigger-offset-x");
    int trigger_offset_y = var_InheritInteger(vd, "open3d-trigger-offset-y");
    int trigger_brightness = var_InheritInteger(vd, "open3d-trigger-brightness");
    int trigger_black_border = var_InheritInteger(vd, "open3d-trigger-black-border");

    char *trigger_corner = var_InheritString(vd, "open3d-trigger-corner");
    sys->trigger_corner = Open3DParseTriggerCorner(trigger_corner);
    free(trigger_corner);

    sys->trigger_size = trigger_size > 0 ? (unsigned)trigger_size : 0;
    sys->trigger_padding = trigger_padding > 0 ? (unsigned)trigger_padding : 0;
    sys->trigger_spacing = trigger_spacing > 0 ? (unsigned)trigger_spacing : 0;
    sys->trigger_offset_x = trigger_offset_x;
    sys->trigger_offset_y = trigger_offset_y;
    if (trigger_brightness < 0)
        trigger_brightness = 0;
    if (trigger_brightness > 255)
        trigger_brightness = 255;
    sys->trigger_brightness = (uint8_t)trigger_brightness;
    sys->trigger_black_border = trigger_black_border > 0 ? (unsigned)trigger_black_border : 0;
    sys->trigger_alpha = Open3DAlphaFromFloat(var_InheritFloat(vd, "open3d-trigger-alpha"));
    sys->trigger_invert = var_InheritBool(vd, "open3d-trigger-invert");

    sys->calibration_enable = var_InheritBool(vd, "open3d-calibration-enable");
    Open3DEnsureBoolVar(VLC_OBJECT(vd), "open3d-calibration-enable",
                        sys->calibration_enable);
    sys->calibration_size = 64;
    sys->calibration_thickness = 2;
    sys->calibration_alpha = Open3DAlphaFromFloat(0.85);

    sys->status_osd_enable = var_InheritBool(vd, "open3d-status-osd-enable");
    int status_osd_ms = var_InheritInteger(vd, "open3d-status-osd-duration-ms");
    if (status_osd_ms < 250)
        status_osd_ms = 250;
    int status_help_ms = var_InheritInteger(vd, "open3d-status-help-duration-ms");
    if (status_help_ms < 500)
        status_help_ms = 500;
    sys->status_osd_duration = VLC_TICK_FROM_MS(status_osd_ms);
    sys->status_help_duration = VLC_TICK_FROM_MS(status_help_ms);
    sys->status_message[0] = '\0';
    sys->status_message_until = VLC_TICK_INVALID;
    sys->status_overlay_visible = true;
    sys->status_help_active = false;
    sys->status_help_until = VLC_TICK_INVALID;
    sys->status_calibration_help = true;
    sys->status_prev_emitter_connected_valid = false;
    sys->status_prev_emitter_dirty_valid = false;
    sys->status_prev_firmware_busy_valid = false;

    sys->hotkeys_enable = var_InheritBool(vd, "open3d-hotkeys-enable");
    sys->hotkeys_registered = false;
    char *hotkey_profile = var_InheritString(vd, "open3d-hotkeys-profile");
    sys->hotkeys_profile = Open3DParseHotkeysProfile(hotkey_profile);
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-hotkeys-profile",
                          hotkey_profile != NULL ? hotkey_profile : "safe");
    free(hotkey_profile);
    Open3DPublishHotkeyDefaults(vd, sys->hotkeys_profile);
    Open3DReloadHotkeys(vd, sys);

    sys->emitter_enable = var_InheritBool(vd, "open3d-emitter-enable");
    sys->emitter_tty = var_InheritString(vd, "open3d-emitter-tty");
    if (sys->emitter_tty == NULL)
        sys->emitter_tty = strdup("");
    sys->emitter_tty_auto = (sys->emitter_tty[0] == '\0' ||
                             !strcasecmp(sys->emitter_tty, "auto"));
    sys->emitter_tty_selected = NULL;
    sys->emitter_baud = var_InheritInteger(vd, "open3d-emitter-baud");
    sys->emitter_auto_reconnect = var_InheritBool(vd, "open3d-emitter-auto-reconnect");
    int emitter_reconnect_ms = var_InheritInteger(vd, "open3d-emitter-reconnect-ms");
    if (emitter_reconnect_ms < 10)
        emitter_reconnect_ms = 10;
    sys->emitter_reconnect_interval = VLC_TICK_FROM_MS(emitter_reconnect_ms);
    sys->emitter_log_io = var_InheritBool(vd, "open3d-emitter-log-io");
    sys->emitter_read_on_connect = var_InheritBool(vd, "open3d-emitter-read-on-connect");
    sys->emitter_apply_on_connect = var_InheritBool(vd, "open3d-emitter-apply-on-connect");
    sys->emitter_save_on_apply = var_InheritBool(vd, "open3d-emitter-save-on-apply");
    sys->emitter_load_json = var_InheritBool(vd, "open3d-emitter-load-json");
    sys->emitter_save_json = var_InheritBool(vd, "open3d-emitter-save-json");
    char *emitter_settings_json = var_InheritString(vd, "open3d-emitter-settings-json");
    sys->emitter_settings_json_path = Open3DResolveEmitterSettingsPath(emitter_settings_json);
    free(emitter_settings_json);
    sys->emitter_opt_csv_enable = var_InheritBool(vd, "open3d-emitter-opt-csv-enable");
    char *emitter_opt_csv_path = var_InheritString(vd, "open3d-emitter-opt-csv-path");
    sys->emitter_opt_csv_path = Open3DResolveEmitterOptCsvPath(emitter_opt_csv_path);
    free(emitter_opt_csv_path);
    sys->emitter_opt_csv_flush = var_InheritBool(vd, "open3d-emitter-opt-csv-flush");
    sys->emitter_fw_helper = var_InheritString(vd, "open3d-emitter-fw-helper");
    if (sys->emitter_fw_helper == NULL)
        sys->emitter_fw_helper = strdup("");
    sys->emitter_fw_hex = var_InheritString(vd, "open3d-emitter-fw-hex");
    if (sys->emitter_fw_hex == NULL)
        sys->emitter_fw_hex = strdup("");
    char *emitter_fw_backup = var_InheritString(vd, "open3d-emitter-fw-backup-json");
    sys->emitter_fw_backup_json_path = Open3DResolveEmitterFwBackupPath(emitter_fw_backup);
    free(emitter_fw_backup);
    sys->emitter_fw_reapply = var_InheritBool(vd, "open3d-emitter-fw-reapply");
    sys->emitter_fw_pid = 0;
    sys->emitter_fw_in_progress = false;
    sys->emitter_fw_reapply_pending = false;
    sys->emitter_fw_started = VLC_TICK_INVALID;
    sys->emitter_req_read = var_InheritBool(vd, open3d_emitter_cmd_read_var);
    sys->emitter_req_apply = var_InheritBool(vd, open3d_emitter_cmd_apply_var);
    sys->emitter_req_save = var_InheritBool(vd, open3d_emitter_cmd_save_var);
    sys->emitter_req_reconnect = var_InheritBool(vd, open3d_emitter_cmd_reconnect_var);
    sys->emitter_req_fw_update = var_InheritBool(vd, open3d_emitter_cmd_fw_update_var);
    Open3DEnsureBoolVar(VLC_OBJECT(vd), open3d_emitter_cmd_read_var, false);
    Open3DEnsureBoolVar(VLC_OBJECT(vd), open3d_emitter_cmd_apply_var, false);
    Open3DEnsureBoolVar(VLC_OBJECT(vd), open3d_emitter_cmd_save_var, false);
    Open3DEnsureBoolVar(VLC_OBJECT(vd), open3d_emitter_cmd_reconnect_var, false);
    Open3DEnsureBoolVar(VLC_OBJECT(vd), open3d_emitter_cmd_fw_update_var, false);

    sys->emitter_settings.ir_protocol = var_InheritInteger(vd, "open3d-emitter-ir-protocol");
    sys->emitter_settings.ir_frame_delay = var_InheritInteger(vd, "open3d-emitter-ir-frame-delay");
    sys->emitter_settings.ir_frame_duration = var_InheritInteger(vd, "open3d-emitter-ir-frame-duration");
    sys->emitter_settings.ir_signal_spacing = var_InheritInteger(vd, "open3d-emitter-ir-signal-spacing");
    sys->emitter_settings.opt_block_signal_detection_delay = var_InheritInteger(vd, "open3d-emitter-opt-block-delay");
    sys->emitter_settings.opt_min_threshold_value_to_activate = var_InheritInteger(vd, "open3d-emitter-opt-min-threshold");
    sys->emitter_settings.opt_detection_threshold_high = var_InheritInteger(vd, "open3d-emitter-opt-threshold-high");
    sys->emitter_settings.opt_enable_ignore_during_ir = var_InheritInteger(vd, "open3d-emitter-opt-ignore-during-ir");
    sys->emitter_settings.opt_enable_duplicate_realtime_reporting = var_InheritInteger(vd, "open3d-emitter-opt-dup-realtime");
    sys->emitter_settings.opt_output_stats = var_InheritInteger(vd, "open3d-emitter-opt-output-stats");
    sys->emitter_settings.opt_ignore_all_duplicates = var_InheritInteger(vd, "open3d-emitter-opt-ignore-duplicates");
    sys->emitter_settings.opt_sensor_filter_mode = var_InheritInteger(vd, "open3d-emitter-opt-sensor-filter");
    sys->emitter_settings.ir_flip_eyes = var_InheritInteger(vd, "open3d-emitter-ir-flip-eyes");
    sys->emitter_settings.opt_detection_threshold_low = var_InheritInteger(vd, "open3d-emitter-opt-threshold-low");
    sys->emitter_settings.ir_average_timing_mode = var_InheritInteger(vd, "open3d-emitter-ir-avg-timing");
    sys->emitter_settings.target_frametime = var_InheritInteger(vd, "open3d-emitter-target-frametime");
    sys->emitter_settings.ir_drive_mode = var_InheritInteger(vd, "open3d-emitter-drive-mode");
    Open3DEmitterLoadSettingsJson(vd, sys);
    char *drive_mode = var_InheritString(vd, "open3d-trigger-drive-mode");
    sys->emitter_settings.ir_drive_mode = Open3DParseDriveMode(drive_mode);
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-trigger-drive-mode",
                          sys->emitter_settings.ir_drive_mode == 1 ? "serial" : "optical");
    free(drive_mode);

    char *layout = var_InheritString(vd, "open3d-layout");
    sys->forced_layout = Open3DParseLayout(layout);
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-layout",
                          layout != NULL ? layout : "auto");
    free(layout);
    char *default_half_layout = var_InheritString(vd, "open3d-default-half-layout");
    sys->default_half_layout = Open3DParseDefaultHalfLayout(default_half_layout);
    Open3DEnsureStringVar(VLC_OBJECT(vd), "open3d-default-half-layout",
                          default_half_layout != NULL ? default_half_layout : "sbs");
    free(default_half_layout);

    sys->target_flip_hz = var_InheritFloat(vd, "open3d-target-flip-hz");
    if (sys->target_flip_hz > 0.0)
        sys->flip_period = CLOCK_FREQ / sys->target_flip_hz;
    sys->presenter_enable = true;
    sys->presenter_hz = var_InheritFloat(vd, "open3d-presenter-hz");
    if (sys->presenter_hz < 1.0)
        sys->presenter_hz = 120.0;
    sys->presenter_period = CLOCK_FREQ / sys->presenter_hz;
    if (sys->presenter_period <= 0)
        sys->presenter_period = CLOCK_FREQ / 120;
    sys->gpu_overlay_enable = var_InheritBool(vd, "open3d-gpu-overlay-enable");
    sys->presenter_telemetry = var_InheritBool(vd, "open3d-presenter-telemetry");
    sys->presenter_telemetry_interval = var_InheritInteger(vd, "open3d-presenter-telemetry-interval");
    if (sys->presenter_telemetry_interval <= 0)
        sys->presenter_telemetry_interval = 120;
    sys->presenter_stage_profile = var_InheritBool(vd, "open3d-presenter-stage-profile");
    sys->presenter_rt_request = var_InheritBool(vd, "open3d-presenter-rt-enable");
    sys->presenter_rt_priority_request = var_InheritInteger(vd, "open3d-presenter-rt-priority");
    sys->presenter_affinity_request = var_InheritBool(vd, "open3d-presenter-affinity-enable");
    sys->presenter_affinity_cpu_request = var_InheritInteger(vd, "open3d-presenter-affinity-cpu");
    sys->presenter_memory_lock_request = var_InheritBool(vd, "open3d-presenter-memory-lock");
    sys->presenter_prefault_kb_request = var_InheritInteger(vd, "open3d-presenter-prefault-kb");
    sys->presenter_lead_us_request = var_InheritInteger(vd, "open3d-presenter-lead-us");
    if (sys->presenter_prefault_kb_request < 0)
        sys->presenter_prefault_kb_request = 0;
    if (sys->presenter_lead_us_request < 0)
        sys->presenter_lead_us_request = 0;
    sys->presenter_lead = sys->presenter_lead_us_request;
    if (sys->presenter_period > 0 && sys->presenter_lead >= sys->presenter_period)
        sys->presenter_lead = sys->presenter_period - 1;
    msg_Dbg(vd,
            "open3d presenter tuning config rt_enable=%d rt_priority=%d affinity_enable=%d affinity_cpu=%d memlock=%d prefault_kb=%d lead_us=%" PRId64,
            sys->presenter_rt_request ? 1 : 0,
            sys->presenter_rt_priority_request,
            sys->presenter_affinity_request ? 1 : 0,
            sys->presenter_affinity_cpu_request,
            sys->presenter_memory_lock_request ? 1 : 0,
            sys->presenter_prefault_kb_request,
            sys->presenter_lead);
    Open3DPresenterStatePublish(sys);

    sys->last_logged_pack = OPEN3D_PACK_NONE;
    sys->prepared_eye_source = vd->source;
    sys->prepared_owned_subpicture = NULL;
    Open3DEmitterResetQueue(sys);
    sys->emitter_cmd_deadline = VLC_TICK_INVALID;
    sys->emitter_firmware_version = 0;
    sys->emitter_device_settings_valid = false;
    sys->emitter_settings_dirty = false;
    Open3DEmitterWriteSettingsJson(vd, sys);

    if (sys->emitter_enable)
    {
        if (sys->emitter_tty_auto)
            msg_Dbg(vd, "open3d emitter tty mode: auto-detect");
        else if (sys->emitter_tty == NULL || sys->emitter_tty[0] == '\0')
            msg_Warn(vd, "open3d emitter enabled but no tty configured");
    }

    vout_window_t *surface = vout_display_NewWindow(vd, VOUT_WINDOW_TYPE_INVALID);
    if (surface == NULL)
    {
        msg_Err(vd, "parent window not available");
        Open3DEmitterClose(sys);
        Open3DEmitterCloseOptCsv(sys);
        Open3DTextRegionCacheClear(&sys->status_main_cache);
        Open3DTextRegionCacheClear(&sys->status_serial_cache);
        Open3DTextRegionCacheClear(&sys->status_calib_main_cache);
        Open3DTextRegionCacheClear(&sys->status_calib_serial_cache);
        Open3DTriggerRegionCacheClear(&sys->trigger_region_cache);
        Open3DCalibrationRegionCacheClear(&sys->calibration_region_cache);
        Open3DBluraySubtitleCacheClear(&sys->bluray_subtitle_cache);
        Open3DBluraySubtitleCacheClear(&sys->bluray_subtitle_cache);
        free(sys->emitter_tty_selected);
        free(sys->emitter_tty);
        free(sys->emitter_settings_json_path);
        free(sys->emitter_opt_csv_path);
        free(sys->emitter_fw_helper);
        free(sys->emitter_fw_hex);
        free(sys->emitter_fw_backup_json_path);
        if (sys->presenter_picture != NULL)
            picture_Release(sys->presenter_picture);
        if (sys->presenter_subpicture != NULL)
            subpicture_Delete(sys->presenter_subpicture);
        vlc_cond_destroy(&sys->emitter_cond);
        vlc_mutex_destroy(&sys->emitter_control_lock);
        vlc_cond_destroy(&sys->control_cond);
        vlc_mutex_destroy(&sys->control_lock);
        vlc_mutex_destroy(&sys->hotkey_lock);
        vlc_mutex_destroy(&sys->gl_lock);
        vlc_cond_destroy(&sys->presenter_cond);
        vlc_mutex_destroy(&sys->presenter_lock);
        free(sys);
        return VLC_EGENERIC;
    }

    sys->gl = vlc_gl_Create(surface, VLC_OPENGL, "$gl");
    if (sys->gl == NULL)
    {
        vout_display_DeleteWindow(vd, surface);
        Open3DEmitterClose(sys);
        Open3DEmitterCloseOptCsv(sys);
        Open3DTextRegionCacheClear(&sys->status_main_cache);
        Open3DTextRegionCacheClear(&sys->status_serial_cache);
        Open3DTextRegionCacheClear(&sys->status_calib_main_cache);
        Open3DTextRegionCacheClear(&sys->status_calib_serial_cache);
        Open3DTriggerRegionCacheClear(&sys->trigger_region_cache);
        Open3DCalibrationRegionCacheClear(&sys->calibration_region_cache);
        Open3DBluraySubtitleCacheClear(&sys->bluray_subtitle_cache);
        free(sys->emitter_tty_selected);
        free(sys->emitter_tty);
        free(sys->emitter_settings_json_path);
        free(sys->emitter_opt_csv_path);
        free(sys->emitter_fw_helper);
        free(sys->emitter_fw_hex);
        free(sys->emitter_fw_backup_json_path);
        if (sys->presenter_picture != NULL)
            picture_Release(sys->presenter_picture);
        if (sys->presenter_subpicture != NULL)
            subpicture_Delete(sys->presenter_subpicture);
        vlc_cond_destroy(&sys->emitter_cond);
        vlc_mutex_destroy(&sys->emitter_control_lock);
        vlc_cond_destroy(&sys->control_cond);
        vlc_mutex_destroy(&sys->control_lock);
        vlc_mutex_destroy(&sys->hotkey_lock);
        vlc_mutex_destroy(&sys->gl_lock);
        vlc_cond_destroy(&sys->presenter_cond);
        vlc_mutex_destroy(&sys->presenter_lock);
        free(sys);
        return VLC_EGENERIC;
    }

    vlc_gl_Resize(sys->gl, vd->cfg->display.width, vd->cfg->display.height);

    if (vlc_gl_MakeCurrent(sys->gl) != VLC_SUCCESS)
    {
        vlc_gl_Release(sys->gl);
        vout_display_DeleteWindow(vd, surface);
        Open3DEmitterClose(sys);
        Open3DEmitterCloseOptCsv(sys);
        Open3DTextRegionCacheClear(&sys->status_main_cache);
        Open3DTextRegionCacheClear(&sys->status_serial_cache);
        Open3DTextRegionCacheClear(&sys->status_calib_main_cache);
        Open3DTextRegionCacheClear(&sys->status_calib_serial_cache);
        Open3DTriggerRegionCacheClear(&sys->trigger_region_cache);
        Open3DCalibrationRegionCacheClear(&sys->calibration_region_cache);
        Open3DBluraySubtitleCacheClear(&sys->bluray_subtitle_cache);
        free(sys->emitter_tty_selected);
        free(sys->emitter_tty);
        free(sys->emitter_settings_json_path);
        free(sys->emitter_opt_csv_path);
        free(sys->emitter_fw_helper);
        free(sys->emitter_fw_hex);
        free(sys->emitter_fw_backup_json_path);
        if (sys->presenter_picture != NULL)
            picture_Release(sys->presenter_picture);
        if (sys->presenter_subpicture != NULL)
            subpicture_Delete(sys->presenter_subpicture);
        vlc_cond_destroy(&sys->emitter_cond);
        vlc_mutex_destroy(&sys->emitter_control_lock);
        vlc_cond_destroy(&sys->control_cond);
        vlc_mutex_destroy(&sys->control_lock);
        vlc_mutex_destroy(&sys->hotkey_lock);
        vlc_mutex_destroy(&sys->gl_lock);
        vlc_cond_destroy(&sys->presenter_cond);
        vlc_mutex_destroy(&sys->presenter_lock);
        free(sys);
        return VLC_EGENERIC;
    }

    const vlc_fourcc_t *spu_chromas;
    if (sys->gpu_overlay_enable)
        sys->gpu_overlay_ready = Open3DLoadGpuOverlayApi(vd, sys);
    sys->vgl = vout_display_opengl_New(&vd->fmt, &spu_chromas, sys->gl,
                                       &vd->cfg->viewpoint);
    vlc_gl_ReleaseCurrent(sys->gl);

    if (sys->vgl == NULL)
    {
        vlc_gl_Release(sys->gl);
        vout_display_DeleteWindow(vd, surface);
        Open3DEmitterClose(sys);
        Open3DEmitterCloseOptCsv(sys);
        Open3DTextRegionCacheClear(&sys->status_main_cache);
        Open3DTextRegionCacheClear(&sys->status_serial_cache);
        Open3DTextRegionCacheClear(&sys->status_calib_main_cache);
        Open3DTextRegionCacheClear(&sys->status_calib_serial_cache);
        Open3DTriggerRegionCacheClear(&sys->trigger_region_cache);
        Open3DCalibrationRegionCacheClear(&sys->calibration_region_cache);
        free(sys->emitter_tty_selected);
        free(sys->emitter_tty);
        free(sys->emitter_settings_json_path);
        free(sys->emitter_opt_csv_path);
        free(sys->emitter_fw_helper);
        free(sys->emitter_fw_hex);
        free(sys->emitter_fw_backup_json_path);
        if (sys->presenter_picture != NULL)
            picture_Release(sys->presenter_picture);
        if (sys->presenter_subpicture != NULL)
            subpicture_Delete(sys->presenter_subpicture);
        vlc_cond_destroy(&sys->emitter_cond);
        vlc_mutex_destroy(&sys->emitter_control_lock);
        vlc_cond_destroy(&sys->control_cond);
        vlc_mutex_destroy(&sys->control_lock);
        vlc_mutex_destroy(&sys->hotkey_lock);
        vlc_mutex_destroy(&sys->gl_lock);
        vlc_cond_destroy(&sys->presenter_cond);
        vlc_mutex_destroy(&sys->presenter_lock);
        free(sys);
        return VLC_EGENERIC;
    }

    vd->sys = sys;
    vd->info.has_pictures_invalid = false;
    vd->info.subpicture_chromas = spu_chromas;
    vd->pool = Pool;
    vd->prepare = PictureRender;
    vd->display = PictureDisplay;
    vd->control = Control;
    Open3DSetDirectBluraySubtitleBridge(vd, true);
    var_AddCallback(vd, "open3d-layout", Open3DConfigVarCallback, sys);
    var_AddCallback(vd, "open3d-default-half-layout", Open3DConfigVarCallback, sys);
    var_AddCallback(vd, "open3d-hotkeys-profile", Open3DConfigVarCallback, sys);
    var_AddCallback(vd, "open3d-calibration-enable", Open3DConfigVarCallback, sys);
    var_AddCallback(vd, "open3d-trigger-drive-mode", Open3DConfigVarCallback, sys);
    var_AddCallback(vd, open3d_emitter_cmd_read_var, Open3DEmitterVarCallback, sys);
    var_AddCallback(vd, open3d_emitter_cmd_apply_var, Open3DEmitterVarCallback, sys);
    var_AddCallback(vd, open3d_emitter_cmd_save_var, Open3DEmitterVarCallback, sys);
    var_AddCallback(vd, open3d_emitter_cmd_reconnect_var, Open3DEmitterVarCallback, sys);
    var_AddCallback(vd, open3d_emitter_cmd_fw_update_var, Open3DEmitterVarCallback, sys);
    if (sys->hotkeys_enable)
    {
        var_AddCallback(vd->obj.libvlc, "key-pressed", Open3DHotkeyVarCallback, sys);
        sys->hotkeys_registered = true;
    }

    if (vlc_clone(&sys->control_thread, Open3DControlThread, vd, 0) == 0)
    {
        sys->control_started = true;
        Open3DControlWake(sys);
        msg_Dbg(vd, "open3d control/state thread started");
    }
    else
    {
        msg_Warn(vd, "open3d control/state thread start failed; falling back to render-thread state updates");
    }

    if (sys->emitter_enable)
    {
        if (vlc_clone(&sys->emitter_thread, Open3DEmitterThread, vd, 0) == 0)
        {
            sys->emitter_started = true;
            Open3DEmitterWake(sys);
            msg_Dbg(vd, "open3d emitter thread started");
        }
        else
        {
            sys->emitter_enable = false;
            msg_Warn(vd, "open3d emitter thread start failed; serial emitter disabled");
        }
    }

    if (vlc_clone(&sys->presenter_thread, Open3DPresenterThread, vd,
                  VLC_THREAD_PRIORITY_OUTPUT) == 0)
    {
        sys->presenter_started = true;
        msg_Dbg(vd, "open3d presenter thread started (cadence %.3f Hz)",
                (double)CLOCK_FREQ / (double)sys->presenter_period);
    }
    else
    {
        msg_Err(vd, "open3d presenter thread start failed");
        Close(VLC_OBJECT(vd));
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = vd->sys;

    Open3DSetDirectBluraySubtitleBridge(vd, false);

    if (sys->presenter_started)
    {
        vlc_mutex_lock(&sys->presenter_lock);
        sys->presenter_stop = true;
        atomic_store_explicit(&sys->presenter_stop_atomic, true, memory_order_relaxed);
        vlc_cond_signal(&sys->presenter_cond);
        vlc_mutex_unlock(&sys->presenter_lock);
        vlc_join(sys->presenter_thread, NULL);
        sys->presenter_started = false;
    }

    if (sys->control_started)
    {
        vlc_mutex_lock(&sys->control_lock);
        sys->control_stop = true;
        vlc_cond_signal(&sys->control_cond);
        vlc_mutex_unlock(&sys->control_lock);
        vlc_join(sys->control_thread, NULL);
        sys->control_started = false;
    }

    if (sys->emitter_started)
    {
        vlc_mutex_lock(&sys->emitter_control_lock);
        sys->emitter_stop = true;
        vlc_cond_signal(&sys->emitter_cond);
        vlc_mutex_unlock(&sys->emitter_control_lock);
        vlc_join(sys->emitter_thread, NULL);
        sys->emitter_started = false;
    }

    if (sys->presenter_schedstat_fd >= 0)
    {
        close(sys->presenter_schedstat_fd);
        sys->presenter_schedstat_fd = -1;
    }
    free(sys->presenter_prefault_buffer);
    sys->presenter_prefault_buffer = NULL;
    sys->presenter_prefault_bytes = 0;

    var_DelCallback(vd, "open3d-layout", Open3DConfigVarCallback, sys);
    var_DelCallback(vd, "open3d-default-half-layout", Open3DConfigVarCallback, sys);
    var_DelCallback(vd, "open3d-hotkeys-profile", Open3DConfigVarCallback, sys);
    var_DelCallback(vd, "open3d-calibration-enable", Open3DConfigVarCallback, sys);
    var_DelCallback(vd, "open3d-trigger-drive-mode", Open3DConfigVarCallback, sys);
    var_DelCallback(vd, open3d_emitter_cmd_read_var, Open3DEmitterVarCallback, sys);
    var_DelCallback(vd, open3d_emitter_cmd_apply_var, Open3DEmitterVarCallback, sys);
    var_DelCallback(vd, open3d_emitter_cmd_save_var, Open3DEmitterVarCallback, sys);
    var_DelCallback(vd, open3d_emitter_cmd_reconnect_var, Open3DEmitterVarCallback, sys);
    var_DelCallback(vd, open3d_emitter_cmd_fw_update_var, Open3DEmitterVarCallback, sys);
    if (sys->hotkeys_registered)
    {
        var_DelCallback(vd->obj.libvlc, "key-pressed", Open3DHotkeyVarCallback, sys);
        sys->hotkeys_registered = false;
    }
    Open3DPresenterClearFrame(sys);
    Open3DReleasePreparedSubpicture(sys);

    if (sys->emitter_fw_in_progress && sys->emitter_fw_pid > 0)
    {
        int status = 0;
        pid_t waited = waitpid(sys->emitter_fw_pid, &status, WNOHANG);
        if (waited == 0)
        {
            msg_Warn(vd, "firmware helper (pid=%ld) still running during close",
                     (long)sys->emitter_fw_pid);
        }
        else if (waited == sys->emitter_fw_pid)
        {
            sys->emitter_fw_in_progress = false;
            sys->emitter_fw_pid = 0;
        }
    }

    Open3DEmitterClose(sys);
    Open3DEmitterCloseOptCsv(sys);
    Open3DTextRegionCacheClear(&sys->status_main_cache);
    Open3DTextRegionCacheClear(&sys->status_serial_cache);
    Open3DTextRegionCacheClear(&sys->status_calib_main_cache);
    Open3DTextRegionCacheClear(&sys->status_calib_serial_cache);
    Open3DTriggerRegionCacheClear(&sys->trigger_region_cache);
    Open3DCalibrationRegionCacheClear(&sys->calibration_region_cache);
    Open3DBluraySubtitleCacheClear(&sys->bluray_subtitle_cache);
    Open3DOverlayCacheClear(sys);
    vlc_gl_t *gl = sys->gl;
    vout_window_t *surface = gl->surface;

    vlc_mutex_lock(&sys->gl_lock);
    vlc_gl_MakeCurrent(gl);
    vout_display_opengl_Delete(sys->vgl);
    vlc_gl_ReleaseCurrent(gl);
    vlc_mutex_unlock(&sys->gl_lock);

    vlc_gl_Release(gl);
    vout_display_DeleteWindow(vd, surface);
    free(sys->emitter_tty_selected);
    free(sys->emitter_tty);
    free(sys->emitter_settings_json_path);
    free(sys->emitter_opt_csv_path);
    free(sys->emitter_fw_helper);
    free(sys->emitter_fw_hex);
    free(sys->emitter_fw_backup_json_path);
    vlc_cond_destroy(&sys->emitter_cond);
    vlc_mutex_destroy(&sys->emitter_control_lock);
    vlc_cond_destroy(&sys->control_cond);
    vlc_mutex_destroy(&sys->control_lock);
    vlc_mutex_destroy(&sys->hotkey_lock);
    vlc_mutex_destroy(&sys->gl_lock);
    vlc_cond_destroy(&sys->presenter_cond);
    vlc_mutex_destroy(&sys->presenter_lock);
    free(sys);
}

static picture_pool_t *Pool(vout_display_t *vd, unsigned count)
{
    vout_display_sys_t *sys = vd->sys;

    vlc_mutex_lock(&sys->gl_lock);
    if (!sys->pool && vlc_gl_MakeCurrent(sys->gl) == VLC_SUCCESS)
    {
        sys->pool = vout_display_opengl_GetPool(sys->vgl, count);
        vlc_gl_ReleaseCurrent(sys->gl);
    }
    vlc_mutex_unlock(&sys->gl_lock);
    return sys->pool;
}

static bool Open3DComputePicturePlace(const video_format_t *source,
                                      const vout_display_cfg_t *cfg_in,
                                      vout_display_place_t *place)
{
    if (source == NULL || cfg_in == NULL || place == NULL)
        return false;

    vout_display_cfg_t cfg = *cfg_in;
    if (cfg.align.vertical == VOUT_DISPLAY_ALIGN_TOP)
        cfg.align.vertical = VOUT_DISPLAY_ALIGN_BOTTOM;
    else if (cfg.align.vertical == VOUT_DISPLAY_ALIGN_BOTTOM)
        cfg.align.vertical = VOUT_DISPLAY_ALIGN_TOP;

    vout_display_PlacePicture(place, source, &cfg, false);
    return place->width > 0 && place->height > 0;
}

static void Open3DApplyPicturePlace(vout_display_sys_t *sys,
                                    const vout_display_place_t *place)
{
    if (place == NULL || place->width == 0 || place->height == 0)
        return;

    if (sys->place_valid &&
        !sys->place_force &&
        sys->last_place.x == place->x &&
        sys->last_place.y == place->y &&
        sys->last_place.width == place->width &&
        sys->last_place.height == place->height)
    {
        return;
    }

    vlc_gl_Resize(sys->gl, place->width, place->height);
    vout_display_opengl_SetWindowAspectRatio(sys->vgl,
                                             (float)place->width / place->height);
    vout_display_opengl_Viewport(sys->vgl,
                                 place->x, place->y,
                                 place->width, place->height);
    sys->last_place = *place;
    sys->place_valid = true;
    sys->place_force = false;
}

static void Open3DPresenterRenderTick(vout_display_t *vd, vout_display_sys_t *sys,
                                      picture_t *pic, const subpicture_t *base_subpicture,
                                      vlc_tick_t frame_clock)
{
    open3d_presenter_state_snapshot_t state;
    video_format_t eye_source = vd->source;
    open3d_pack_t pack_mode = OPEN3D_PACK_NONE;
    open3d_pack_t detected_pack_mode = OPEN3D_PACK_NONE;
    bool show_right_eye = false;
    unsigned flips = 0;
    open3d_presenter_stage_stamp_t prelude_start = {0}, prelude_end = {0};
    open3d_presenter_stage_stamp_t emitter_queue_start = {0}, emitter_queue_end = {0};
    open3d_presenter_stage_stamp_t status_log_start = {0}, status_log_end = {0};
    open3d_presenter_stage_stamp_t compose_start = {0}, compose_end = {0};
    open3d_presenter_stage_stamp_t gl_lock_start = {0}, gl_lock_end = {0};
    open3d_presenter_stage_stamp_t make_current_start = {0}, make_current_end = {0};
    open3d_presenter_stage_stamp_t cleanup_start = {0}, cleanup_end = {0};

    if (frame_clock == VLC_TICK_INVALID)
        frame_clock = mdate();
    if (sys->presenter_stage_profile)
        Open3DPresenterStageStampNow(sys, &prelude_start);

    if (!sys->control_started)
    {
        Open3DApplyHotkeyRequests(vd, sys);
        if (sys->emitter_enable)
            Open3DUpdateEmitterStatusWarnings(vd, sys);
    }

    Open3DPresenterStateRead(sys, &state);
    detected_pack_mode = Open3DDetectPackModeWithState(vd,
                                                       state.forced_layout,
                                                       state.default_half_layout);

    if (state.enabled)
    {
        pack_mode = detected_pack_mode;
        if (pack_mode != OPEN3D_PACK_NONE)
        {
            flips = Open3DAdvanceEye(sys, state.enabled, frame_clock);
            show_right_eye = sys->show_right_eye;

            if (state.swap_eyes)
                show_right_eye = !show_right_eye;

            Open3DApplyEyeCrop(&vd->source, pack_mode, show_right_eye, &eye_source);
            if (sys->presenter_stage_profile)
                Open3DPresenterStageStampNow(sys, &emitter_queue_start);
            Open3DEmitterQueueEye(sys, show_right_eye, frame_clock);
            if (sys->presenter_stage_profile)
                Open3DPresenterStageStampNow(sys, &emitter_queue_end);

            if (flips > 1)
            {
                sys->late_flip_events++;
                sys->late_flip_steps_total += flips - 1;
            }

            sys->frame_count++;
            if (Open3DShouldLogPresenterEvents(sys) && sys->presenter_stage_profile)
                Open3DPresenterStageStampNow(sys, &status_log_start);
            Open3DMaybeLogStatus(vd, sys, pack_mode, show_right_eye, frame_clock, flips,
                                 &eye_source);
            Open3DMaybeLogPresenterTelemetry(vd, sys, pack_mode, show_right_eye, frame_clock,
                                             flips);
            if (Open3DShouldLogPresenterEvents(sys) && sys->presenter_stage_profile)
                Open3DPresenterStageStampNow(sys, &status_log_end);
            sys->warned_missing_pack = false;
        }
        else if (!sys->warned_missing_pack)
        {
            msg_Warn(vd,
                     "open3d enabled but packed stereo could not be inferred from the current source geometry; set --open3d-layout=sbs/tb or explicit --open3d-layout=sbs-full|sbs-half|tb-full|tb-half");
            sys->warned_missing_pack = true;
            Open3DEmitterRequestEyeReset(sys);
        }
        else
        {
            Open3DEmitterRequestEyeReset(sys);
        }
    }
    else
    {
        pack_mode = detected_pack_mode;
        sys->warned_missing_pack = false;
        Open3DEmitterRequestEyeReset(sys);
    }

    if (sys->presenter_stage_profile)
    {
        Open3DPresenterStageStampNow(sys, &prelude_end);
        Open3DPresenterStageStampNow(sys, &compose_start);
    }

    sys->debug_bluray_subtitle_active = false;
    sys->debug_bluray_subtitle_cache_hit = false;
    subpicture_t *bridge_subpicture = Open3DCloneDirectBluraySubtitleSubpicture(vd);
    subpicture_t *forced_bluray_subpicture = NULL;
    subpicture_t *forced_mkv_subpicture = NULL;
    const subpicture_t *working_subpicture = base_subpicture;
    if (base_subpicture == NULL)
        sys->bluray_subtitle_offset_latch.valid = false;
    if (bridge_subpicture == NULL)
    {
        forced_bluray_subpicture = Open3DCloneBluraySubtitleFallback(vd, base_subpicture);
        if (forced_bluray_subpicture != NULL)
            working_subpicture = forced_bluray_subpicture;
        else
        {
            forced_mkv_subpicture = Open3DCloneMkvSubtitleFallback(vd, base_subpicture);
            if (forced_mkv_subpicture != NULL)
                working_subpicture = forced_mkv_subpicture;
        }
    }

    subpicture_t *packed_subpicture = NULL;
    const bool working_subpicture_already_packed =
        !state.enabled && forced_bluray_subpicture != NULL &&
        working_subpicture == forced_bluray_subpicture;
    if (!state.enabled && pack_mode != OPEN3D_PACK_NONE && working_subpicture != NULL &&
        working_subpicture->b_subtitle && !working_subpicture_already_packed)
    {
        packed_subpicture = Open3DBuildPackedStereoSubtitleSubpicture(vd, sys,
                                                                      working_subpicture,
                                                                      pack_mode);
        if (packed_subpicture != NULL)
            working_subpicture = packed_subpicture;
    }

    const bool overlay_active = Open3DOverlayActive(sys, pack_mode);
    if (bridge_subpicture != NULL)
    {
        if (!state.enabled && pack_mode != OPEN3D_PACK_NONE)
        {
            subpicture_t *packed_bridge = Open3DBuildPackedStereoSubtitleSubpicture(vd, sys,
                                                                                    bridge_subpicture,
                                                                                    pack_mode);
            if (packed_bridge != NULL)
            {
                subpicture_Delete(bridge_subpicture);
                bridge_subpicture = packed_bridge;
            }
        }
        else if (!overlay_active && state.enabled && pack_mode != OPEN3D_PACK_NONE)
        {
            unsigned bridge_width = eye_source.i_visible_width > 0
                                  ? eye_source.i_visible_width
                                  : (eye_source.i_width > 0 ? eye_source.i_width
                                                            : vd->source.i_visible_width);
            if (bridge_width == 0)
                bridge_width = vd->source.i_width;
            Open3DApplySubtitleStereoShift(vd, sys, bridge_subpicture,
                                           bridge_width, show_right_eye);
        }
    }

    subpicture_t *overlay_subpicture = NULL;
    bool overlay_cached = false;
    if (overlay_active)
    {
        if (working_subpicture == NULL)
        {
            overlay_subpicture = Open3DPresenterOverlayCacheGet(vd, sys,
                                                                &eye_source,
                                                                pack_mode,
                                                                show_right_eye,
                                                                frame_clock);
            overlay_cached = overlay_subpicture != NULL;
        }
        else
        {
            overlay_subpicture = Open3DBuildOverlaySubpicture(vd, sys, working_subpicture,
                                                              working_subpicture == forced_bluray_subpicture,
                                                              bridge_subpicture,
                                                              false,
                                                              &eye_source,
                                                              pack_mode,
                                                              show_right_eye,
                                                              frame_clock);
        }

        if (overlay_subpicture == NULL && !sys->warned_overlay_alloc)
        {
            msg_Warn(vd,
                     "open3d overlay allocation failed; rendering without trigger/calibration regions");
            sys->warned_overlay_alloc = true;
        }
        else if (overlay_subpicture != NULL)
        {
            sys->warned_overlay_alloc = false;
        }
    }
    else
    {
        sys->warned_overlay_alloc = false;
    }

    subpicture_t *composed_subpicture = NULL;
    subpicture_t *shifted_subpicture = NULL;
    sys->prepared_eye_source = eye_source;
    subpicture_t *render_subpicture = overlay_subpicture != NULL
                                    ? overlay_subpicture
                                    : (subpicture_t *)working_subpicture;
    if (!overlay_active &&
        state.enabled &&
        pack_mode != OPEN3D_PACK_NONE &&
        bridge_subpicture == NULL &&
        forced_bluray_subpicture == NULL &&
        render_subpicture != NULL &&
        render_subpicture->b_subtitle)
    {
        const unsigned shift_width = eye_source.i_visible_width > 0
                                   ? eye_source.i_visible_width
                                   : (eye_source.i_width > 0 ? eye_source.i_width
                                                              : vd->source.i_visible_width);
        shifted_subpicture = Open3DCloneSubpictureShallow(render_subpicture);
        if (shifted_subpicture != NULL)
        {
            Open3DApplySubtitleStereoShift(vd, sys, shifted_subpicture,
                                           shift_width, show_right_eye);
            render_subpicture = shifted_subpicture;
        }
    }
    if (overlay_subpicture == NULL && bridge_subpicture != NULL)
    {
        if (working_subpicture == NULL)
        {
            render_subpicture = bridge_subpicture;
        }
        else
        {
            composed_subpicture = Open3DMergeSubpictures(working_subpicture,
                                                         bridge_subpicture);
            if (composed_subpicture != NULL)
                render_subpicture = composed_subpicture;
        }
    }

    open3d_presenter_stage_profile_t stage_profile;
    memset(&stage_profile, 0, sizeof(stage_profile));
    if (sys->presenter_stage_profile)
    {
        Open3DPresenterStageStampNow(sys, &compose_end);
        memset(&sys->presenter_stage_profile_last, 0, sizeof(sys->presenter_stage_profile_last));
    }

    if (sys->presenter_stage_profile)
        Open3DPresenterStageStampNow(sys, &gl_lock_start);
    vlc_mutex_lock(&sys->gl_lock);
    if (sys->presenter_stage_profile)
        Open3DPresenterStageStampNow(sys, &gl_lock_end);
    if (sys->presenter_stage_profile)
        Open3DPresenterStageStampNow(sys, &make_current_start);
    if (vlc_gl_MakeCurrent(sys->gl) == VLC_SUCCESS)
    {
        open3d_presenter_stage_stamp_t total_start, total_end;
        open3d_presenter_stage_stamp_t prepare_start, prepare_end;
        open3d_presenter_stage_stamp_t display_start, display_end;
        open3d_presenter_stage_stamp_t swap_start, swap_end;
        if (sys->presenter_stage_profile)
        {
            Open3DPresenterStageStampNow(sys, &make_current_end);
            Open3DPresenterStageStampNow(sys, &total_start);
        }

        if (sys->presenter_stage_profile)
            Open3DPresenterStageStampNow(sys, &prepare_start);
        vout_display_opengl_Prepare(sys->vgl, pic, render_subpicture);
        if (sys->presenter_stage_profile)
            Open3DPresenterStageStampNow(sys, &prepare_end);

        vout_display_place_t place;
        const bool have_place = Open3DComputePicturePlace(&eye_source, vd->cfg, &place);
        if (have_place)
            Open3DApplyPicturePlace(sys, &place);
        const bool gpu_overlay_active = sys->gpu_overlay_enable && sys->gpu_overlay_ready;
        if (sys->presenter_stage_profile)
            Open3DPresenterStageStampNow(sys, &display_start);
        if (gpu_overlay_active)
            vout_display_opengl_DisplayNoSwap(sys->vgl, &eye_source);
        else
            vout_display_opengl_Display(sys->vgl, &eye_source);
        if (gpu_overlay_active && have_place)
            Open3DDrawGpuOverlay(vd, sys, &eye_source, pack_mode, show_right_eye, &place);
        if (sys->presenter_stage_profile)
            Open3DPresenterStageStampNow(sys, &display_end);
        if (gpu_overlay_active)
        {
            if (sys->presenter_stage_profile)
                Open3DPresenterStageStampNow(sys, &swap_start);
            vlc_gl_Swap(sys->gl);
            if (sys->presenter_stage_profile)
                Open3DPresenterStageStampNow(sys, &swap_end);
        }

        vlc_gl_ReleaseCurrent(sys->gl);
        if (sys->presenter_stage_profile)
        {
            Open3DPresenterStageStampNow(sys, &total_end);
            stage_profile.valid = true;
            stage_profile.explicit_swap = gpu_overlay_active;
            stage_profile.compose = Open3DPresenterStageMetricDiff(&compose_start, &compose_end);
            stage_profile.gl_lock = Open3DPresenterStageMetricDiff(&gl_lock_start, &gl_lock_end);
            stage_profile.make_current = Open3DPresenterStageMetricDiff(&make_current_start,
                                                                        &make_current_end);
            stage_profile.total = Open3DPresenterStageMetricDiff(&total_start, &total_end);
            stage_profile.prepare = Open3DPresenterStageMetricDiff(&prepare_start, &prepare_end);
            stage_profile.display = Open3DPresenterStageMetricDiff(&display_start, &display_end);
            if (gpu_overlay_active)
                stage_profile.swap = Open3DPresenterStageMetricDiff(&swap_start, &swap_end);
            sys->presenter_stage_profile_last = stage_profile;
            sys->presenter_profiled_frame_count++;
            sys->presenter_compose_wall_total_ns += stage_profile.compose.wall_ns;
            sys->presenter_compose_cpu_total_ns += stage_profile.compose.cpu_ns;
            sys->presenter_gl_lock_wall_total_ns += stage_profile.gl_lock.wall_ns;
            sys->presenter_gl_lock_cpu_total_ns += stage_profile.gl_lock.cpu_ns;
            sys->presenter_make_current_wall_total_ns += stage_profile.make_current.wall_ns;
            sys->presenter_make_current_cpu_total_ns += stage_profile.make_current.cpu_ns;
            sys->presenter_prepare_wall_total_ns += stage_profile.prepare.wall_ns;
            sys->presenter_prepare_cpu_total_ns += stage_profile.prepare.cpu_ns;
            sys->presenter_display_wall_total_ns += stage_profile.display.wall_ns;
            sys->presenter_display_cpu_total_ns += stage_profile.display.cpu_ns;
            sys->presenter_swap_wall_total_ns += stage_profile.swap.wall_ns;
            sys->presenter_swap_cpu_total_ns += stage_profile.swap.cpu_ns;
            if (stage_profile.compose.wall_ns > sys->presenter_compose_wall_max_ns)
                sys->presenter_compose_wall_max_ns = stage_profile.compose.wall_ns;
            if (stage_profile.compose.cpu_ns > sys->presenter_compose_cpu_max_ns)
                sys->presenter_compose_cpu_max_ns = stage_profile.compose.cpu_ns;
            if (stage_profile.gl_lock.wall_ns > sys->presenter_gl_lock_wall_max_ns)
                sys->presenter_gl_lock_wall_max_ns = stage_profile.gl_lock.wall_ns;
            if (stage_profile.gl_lock.cpu_ns > sys->presenter_gl_lock_cpu_max_ns)
                sys->presenter_gl_lock_cpu_max_ns = stage_profile.gl_lock.cpu_ns;
            if (stage_profile.make_current.wall_ns > sys->presenter_make_current_wall_max_ns)
                sys->presenter_make_current_wall_max_ns = stage_profile.make_current.wall_ns;
            if (stage_profile.make_current.cpu_ns > sys->presenter_make_current_cpu_max_ns)
                sys->presenter_make_current_cpu_max_ns = stage_profile.make_current.cpu_ns;
            if (stage_profile.prepare.wall_ns > sys->presenter_prepare_wall_max_ns)
                sys->presenter_prepare_wall_max_ns = stage_profile.prepare.wall_ns;
            if (stage_profile.prepare.cpu_ns > sys->presenter_prepare_cpu_max_ns)
                sys->presenter_prepare_cpu_max_ns = stage_profile.prepare.cpu_ns;
            if (stage_profile.display.wall_ns > sys->presenter_display_wall_max_ns)
                sys->presenter_display_wall_max_ns = stage_profile.display.wall_ns;
            if (stage_profile.display.cpu_ns > sys->presenter_display_cpu_max_ns)
                sys->presenter_display_cpu_max_ns = stage_profile.display.cpu_ns;
            if (stage_profile.swap.wall_ns > sys->presenter_swap_wall_max_ns)
                sys->presenter_swap_wall_max_ns = stage_profile.swap.wall_ns;
            if (stage_profile.swap.cpu_ns > sys->presenter_swap_cpu_max_ns)
                sys->presenter_swap_cpu_max_ns = stage_profile.swap.cpu_ns;
        }
    }
    else if (sys->presenter_stage_profile)
    {
        Open3DPresenterStageStampNow(sys, &make_current_end);
    }
    vlc_mutex_unlock(&sys->gl_lock);

    if (sys->presenter_stage_profile)
        Open3DPresenterStageStampNow(sys, &cleanup_start);

    if (overlay_subpicture != NULL && !overlay_cached)
        subpicture_Delete(overlay_subpicture);
    if (composed_subpicture != NULL)
        subpicture_Delete(composed_subpicture);
    if (bridge_subpicture != NULL)
        subpicture_Delete(bridge_subpicture);
    if (packed_subpicture != NULL)
        subpicture_Delete(packed_subpicture);
    if (shifted_subpicture != NULL)
        subpicture_Delete(shifted_subpicture);
    if (forced_bluray_subpicture != NULL &&
        forced_bluray_subpicture != packed_subpicture &&
        forced_bluray_subpicture != overlay_subpicture &&
        forced_bluray_subpicture != composed_subpicture)
        subpicture_Delete(forced_bluray_subpicture);
    if (forced_mkv_subpicture != NULL &&
        forced_mkv_subpicture != packed_subpicture &&
        forced_mkv_subpicture != overlay_subpicture &&
        forced_mkv_subpicture != composed_subpicture)
        subpicture_Delete(forced_mkv_subpicture);

    if (sys->presenter_stage_profile)
        Open3DPresenterStageStampNow(sys, &cleanup_end);

    if (stage_profile.valid)
    {
        stage_profile.prelude = Open3DPresenterStageMetricDiff(&prelude_start, &prelude_end);
        stage_profile.emitter_queue = Open3DPresenterStageMetricDiff(&emitter_queue_start,
                                                                     &emitter_queue_end);
        stage_profile.status_log = Open3DPresenterStageMetricDiff(&status_log_start,
                                                                  &status_log_end);
        stage_profile.cleanup = Open3DPresenterStageMetricDiff(&cleanup_start, &cleanup_end);
        sys->presenter_stage_profile_last = stage_profile;
        sys->presenter_prelude_wall_total_ns += stage_profile.prelude.wall_ns;
        sys->presenter_prelude_cpu_total_ns += stage_profile.prelude.cpu_ns;
        sys->presenter_emitter_queue_wall_total_ns += stage_profile.emitter_queue.wall_ns;
        sys->presenter_emitter_queue_cpu_total_ns += stage_profile.emitter_queue.cpu_ns;
        sys->presenter_status_log_wall_total_ns += stage_profile.status_log.wall_ns;
        sys->presenter_status_log_cpu_total_ns += stage_profile.status_log.cpu_ns;
        sys->presenter_cleanup_wall_total_ns += stage_profile.cleanup.wall_ns;
        sys->presenter_cleanup_cpu_total_ns += stage_profile.cleanup.cpu_ns;
        if (stage_profile.prelude.wall_ns > sys->presenter_prelude_wall_max_ns)
            sys->presenter_prelude_wall_max_ns = stage_profile.prelude.wall_ns;
        if (stage_profile.prelude.cpu_ns > sys->presenter_prelude_cpu_max_ns)
            sys->presenter_prelude_cpu_max_ns = stage_profile.prelude.cpu_ns;
        if (stage_profile.emitter_queue.wall_ns > sys->presenter_emitter_queue_wall_max_ns)
            sys->presenter_emitter_queue_wall_max_ns = stage_profile.emitter_queue.wall_ns;
        if (stage_profile.emitter_queue.cpu_ns > sys->presenter_emitter_queue_cpu_max_ns)
            sys->presenter_emitter_queue_cpu_max_ns = stage_profile.emitter_queue.cpu_ns;
        if (stage_profile.status_log.wall_ns > sys->presenter_status_log_wall_max_ns)
            sys->presenter_status_log_wall_max_ns = stage_profile.status_log.wall_ns;
        if (stage_profile.status_log.cpu_ns > sys->presenter_status_log_cpu_max_ns)
            sys->presenter_status_log_cpu_max_ns = stage_profile.status_log.cpu_ns;
        if (stage_profile.cleanup.wall_ns > sys->presenter_cleanup_wall_max_ns)
            sys->presenter_cleanup_wall_max_ns = stage_profile.cleanup.wall_ns;
        if (stage_profile.cleanup.cpu_ns > sys->presenter_cleanup_cpu_max_ns)
            sys->presenter_cleanup_cpu_max_ns = stage_profile.cleanup.cpu_ns;
    }
}

static void Open3DPresenterTuneThread(vout_display_t *vd, vout_display_sys_t *sys)
{
#ifdef __linux__
    if (sys->presenter_stage_profile && sys->presenter_schedstat_fd < 0)
    {
        sys->presenter_schedstat_fd = open("/proc/thread-self/schedstat",
                                           O_RDONLY | O_CLOEXEC);
        if (sys->presenter_schedstat_fd >= 0)
            msg_Dbg(vd, "open3d presenter stage profiling enabled (schedstat available)");
        else
            msg_Dbg(vd, "open3d presenter stage profiling enabled (schedstat unavailable: %s)",
                    vlc_strerror_c(errno));
    }

    pthread_t self = pthread_self();
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        page_size = 4096;

    if (sys->presenter_memory_lock_request)
    {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
        {
            sys->presenter_mlockall_enabled = true;
            msg_Dbg(vd, "open3d presenter memory lock enabled");
        }
        else
        {
            msg_Dbg(vd, "open3d presenter memory lock fallback (mlockall failed: %s)",
                    vlc_strerror_c(errno));
        }
    }
    else
    {
        msg_Dbg(vd, "open3d presenter memory lock disabled by config");
    }

    if (sys->presenter_memory_lock_request && sys->presenter_prefault_kb_request > 0)
    {
        size_t bytes = (size_t)sys->presenter_prefault_kb_request * 1024u;
        uint8_t *buffer = malloc(bytes);
        if (buffer != NULL)
        {
            for (size_t off = 0; off < bytes; off += (size_t)page_size)
                buffer[off] = (uint8_t)(off / (size_t)page_size);
            if (bytes > 0)
                buffer[bytes - 1] = 0;
            sys->presenter_prefault_buffer = buffer;
            sys->presenter_prefault_bytes = bytes;
            msg_Dbg(vd, "open3d presenter prefault heap warmup bytes=%zu", bytes);
        }
        else
        {
            msg_Dbg(vd, "open3d presenter prefault heap warmup fallback (alloc failed: %s)",
                    vlc_strerror_c(errno));
        }

        size_t stack_bytes = bytes < (size_t)(64 * 1024) ? bytes : (size_t)(64 * 1024);
        if (stack_bytes > 0)
        {
            volatile uint8_t *stack_buffer = alloca(stack_bytes);
            for (size_t off = 0; off < stack_bytes; off += (size_t)page_size)
                stack_buffer[off] = (uint8_t)(off / (size_t)page_size);
            stack_buffer[stack_bytes - 1] = 0;
            msg_Dbg(vd, "open3d presenter prefault stack warmup bytes=%zu", stack_bytes);
        }
    }

    int fifo_priority_max = sched_get_priority_max(SCHED_FIFO);
    int fifo_priority_min = sched_get_priority_min(SCHED_FIFO);
    if (sys->presenter_rt_request && fifo_priority_max >= fifo_priority_min)
    {
        bool can_attempt_rt = true;
        int requested_priority = sys->presenter_rt_priority_request;
        int max_allowed_priority = fifo_priority_max;
        int desired_priority = requested_priority < 0 ? fifo_priority_max - 1
                                                      : requested_priority;
#ifdef RLIMIT_RTPRIO
        struct rlimit rtprio_rlimit;
        if (getrlimit(RLIMIT_RTPRIO, &rtprio_rlimit) == 0 &&
            rtprio_rlimit.rlim_cur != RLIM_INFINITY)
        {
            if (rtprio_rlimit.rlim_cur < (rlim_t)max_allowed_priority)
                max_allowed_priority = (int)rtprio_rlimit.rlim_cur;
            if (max_allowed_priority < fifo_priority_min)
            {
                msg_Dbg(vd,
                        "open3d presenter scheduler fallback (RLIMIT_RTPRIO=%ju too low for SCHED_FIFO)",
                        (uintmax_t)rtprio_rlimit.rlim_cur);
                can_attempt_rt = false;
            }
        }
#endif
        if (can_attempt_rt && desired_priority < fifo_priority_min)
            desired_priority = fifo_priority_min;
        if (can_attempt_rt && desired_priority > max_allowed_priority)
        {
            msg_Dbg(vd,
                    "open3d presenter scheduler clamp priority %d -> %d due to RT limit",
                    desired_priority, max_allowed_priority);
            desired_priority = max_allowed_priority;
        }
        if (can_attempt_rt)
        {
            struct sched_param sp = { .sched_priority = desired_priority };
            int rc = pthread_setschedparam(self, SCHED_FIFO, &sp);
            if (rc == 0)
            {
                sys->presenter_rt_enabled = true;
                msg_Dbg(vd, "open3d presenter scheduler: SCHED_FIFO priority=%d",
                        sp.sched_priority);
            }
            else
            {
                msg_Dbg(vd, "open3d presenter scheduler fallback (SCHED_FIFO failed: %s)",
                        vlc_strerror_c(rc));
            }
        }
    }
    else if (!sys->presenter_rt_request)
    {
        msg_Dbg(vd, "open3d presenter scheduler disabled by config");
    }

    if (!sys->presenter_affinity_request)
    {
        msg_Dbg(vd, "open3d presenter affinity disabled by config");
    }
    else
    {
        cpu_set_t process_mask;
        CPU_ZERO(&process_mask);
        if (sched_getaffinity(0, sizeof(process_mask), &process_mask) != 0)
            return;

        int cpu = -1;
        if (sys->presenter_affinity_cpu_request >= 0)
        {
            int requested_cpu = sys->presenter_affinity_cpu_request;
            if (requested_cpu < CPU_SETSIZE && CPU_ISSET(requested_cpu, &process_mask))
                cpu = requested_cpu;
            else
                msg_Dbg(vd, "open3d presenter affinity fallback (requested cpu=%d unavailable)",
                        requested_cpu);
        }
#ifdef HAVE_SCHED_GETCPU
        if (cpu < 0)
        {
            int current_cpu = sched_getcpu();
            if (current_cpu >= 0 && CPU_ISSET(current_cpu, &process_mask))
                cpu = current_cpu;
        }
#endif
        if (cpu < 0)
        {
            for (int i = 0; i < CPU_SETSIZE; ++i)
            {
                if (CPU_ISSET(i, &process_mask))
                {
                    cpu = i;
                    break;
                }
            }
        }

        if (cpu >= 0)
        {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(cpu, &mask);
            int rc = pthread_setaffinity_np(self, sizeof(mask), &mask);
            if (rc == 0)
            {
                sys->presenter_affinity_enabled = true;
                sys->presenter_affinity_cpu = cpu;
                msg_Dbg(vd, "open3d presenter affinity pinned to cpu=%d", cpu);
            }
            else
            {
                msg_Dbg(vd, "open3d presenter affinity fallback (pin failed: %s)",
                        vlc_strerror_c(rc));
            }
        }
    }
#else
    VLC_UNUSED(vd);
    VLC_UNUSED(sys);
#endif
}

static void *Open3DPresenterThread(void *data)
{
    vout_display_t *vd = data;
    vout_display_sys_t *sys = vd->sys;
    vlc_tick_t next_present_tick = mdate();
    uint64_t presenter_generation = UINT64_MAX;
    picture_t *cached_pic = NULL;
    subpicture_t *cached_sub = NULL;

    Open3DPresenterTuneThread(vd, sys);

    for (;;)
    {
        if (sys->presenter_period > 0)
        {
            vlc_tick_t wake_tick = next_present_tick;
            if (next_present_tick == VLC_TICK_INVALID)
                next_present_tick = mdate();
            if (sys->presenter_lead > 0 && wake_tick != VLC_TICK_INVALID)
                wake_tick -= sys->presenter_lead;

            mwait(wake_tick);
            const vlc_tick_t wake = mdate();
            if (wake > wake_tick)
            {
                const vlc_tick_t overshoot = wake - wake_tick;
                sys->presenter_sleep_overshoot_events++;
                sys->presenter_sleep_overshoot_total += overshoot;
                if (overshoot > sys->presenter_sleep_overshoot_max)
                    sys->presenter_sleep_overshoot_max = overshoot;
            }
        }

        if (atomic_load_explicit(&sys->presenter_stop_atomic, memory_order_relaxed))
            break;

        const vlc_tick_t frame_clock = next_present_tick;
        uint64_t generation = presenter_generation;
        picture_t *pic = NULL;
        subpicture_t *sub = NULL;
        Open3DPresenterGetFrame(sys, presenter_generation, &generation, &pic, &sub);
        if (generation != presenter_generation)
        {
            if (cached_pic != NULL)
                picture_Release(cached_pic);
            if (cached_sub != NULL)
                subpicture_Delete(cached_sub);
            cached_pic = pic;
            cached_sub = sub;
            presenter_generation = generation;
        }
        else if (sub != NULL)
        {
            subpicture_Delete(sub);
        }
        else if (pic != NULL)
        {
            picture_Release(pic);
        }

        if (cached_pic != NULL)
        {
            const vlc_tick_t render_start = mdate();
            Open3DPresenterRenderTick(vd, sys, cached_pic, cached_sub, frame_clock);
            const vlc_tick_t render_end = mdate();
            const vlc_tick_t render_cost = render_end - render_start;
            sys->presenter_render_total += (uint64_t)render_cost;
            if (render_cost > sys->presenter_render_max)
                sys->presenter_render_max = render_cost;
            if (sys->presenter_period > 0 && render_cost > sys->presenter_period)
            {
                const vlc_tick_t over = render_cost - sys->presenter_period;
                sys->presenter_render_over_budget_events++;
                if (over > sys->presenter_render_over_budget_max)
                    sys->presenter_render_over_budget_max = over;
                if (Open3DShouldLogPresenterEvents(sys))
                {
                    msg_Warn(vd,
                             "open3d presenter render over budget cost=%" PRId64 "us period=%" PRId64
                             "us over=%" PRId64 "us clock=%" PRId64 " subtitle_active=%d cache_hit=%d seq=%d signed=%d sig=%" PRIx64
                             " subtitle_pts=%" PRId64 " cache_hits=%" PRIu64 " cache_misses=%" PRIu64,
                             render_cost, sys->presenter_period, over, render_end,
                             sys->debug_bluray_subtitle_active ? 1 : 0,
                             sys->debug_bluray_subtitle_cache_hit ? 1 : 0,
                             sys->debug_bluray_subtitle_offset_seq,
                             sys->debug_bluray_subtitle_offset_signed,
                             sys->debug_bluray_subtitle_signature,
                             sys->debug_bluray_subtitle_pts,
                             sys->bluray_subtitle_cache_hits,
                             sys->bluray_subtitle_cache_misses);
                    if (sys->presenter_stage_profile)
                        Open3DLogPresenterStageProfile(vd, "over_budget",
                                                       &sys->presenter_stage_profile_last);
                }
            }
        }
        else
        {
            if (!sys->control_started)
            {
                Open3DApplyHotkeyRequests(vd, sys);
                if (sys->emitter_enable)
                    Open3DUpdateEmitterStatusWarnings(vd, sys);
            }
        }
        sys->presenter_tick_count++;

        if (sys->presenter_period <= 0)
        {
            next_present_tick = mdate();
            continue;
        }

        next_present_tick += sys->presenter_period;
        const vlc_tick_t now = mdate();
        if (next_present_tick < now)
        {
            const vlc_tick_t late = now - next_present_tick;
            const uint64_t steps = (late / sys->presenter_period) + 1;
            sys->presenter_late_events++;
            sys->presenter_late_steps_total += steps;
            sys->presenter_deadline_miss_events++;
            sys->presenter_deadline_miss_steps_total += steps;
            if (late > sys->presenter_deadline_miss_max)
                sys->presenter_deadline_miss_max = late;
            if (Open3DShouldLogPresenterEvents(sys))
            {
                msg_Warn(vd,
                         "open3d presenter deadline miss late=%" PRId64 "us steps=%" PRIu64 " clock=%" PRId64
                         " subtitle_active=%d cache_hit=%d seq=%d signed=%d sig=%" PRIx64
                         " subtitle_pts=%" PRId64 " cache_hits=%" PRIu64 " cache_misses=%" PRIu64,
                         late, steps, now,
                         sys->debug_bluray_subtitle_active ? 1 : 0,
                         sys->debug_bluray_subtitle_cache_hit ? 1 : 0,
                         sys->debug_bluray_subtitle_offset_seq,
                         sys->debug_bluray_subtitle_offset_signed,
                         sys->debug_bluray_subtitle_signature,
                         sys->debug_bluray_subtitle_pts,
                         sys->bluray_subtitle_cache_hits,
                         sys->bluray_subtitle_cache_misses);
                if (sys->presenter_stage_profile)
                    Open3DLogPresenterStageProfile(vd, "deadline_miss",
                                                   &sys->presenter_stage_profile_last);
            }
            next_present_tick += steps * sys->presenter_period;
        }
    }

    if (cached_pic != NULL)
        picture_Release(cached_pic);
    if (cached_sub != NULL)
        subpicture_Delete(cached_sub);

    return NULL;
}

static void PictureRender(vout_display_t *vd, picture_t *pic, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;
    Open3DReleasePreparedSubpicture(sys);

    if (sys->presenter_enable)
    {
        Open3DPresenterStoreFrame(sys, pic, subpicture);
        return;
    }

    video_format_t eye_source = vd->source;
    open3d_pack_t pack_mode = OPEN3D_PACK_NONE;
    const open3d_pack_t detected_pack_mode = Open3DDetectPackMode(vd, sys);
    bool show_right_eye = false;
    vlc_tick_t frame_clock = Open3DFrameClock(pic);
    unsigned flips = 0;

    if (!sys->control_started)
    {
        Open3DApplyHotkeyRequests(vd, sys);
        if (sys->emitter_enable)
            Open3DUpdateEmitterStatusWarnings(vd, sys);
    }

    if (sys->enabled)
    {
        pack_mode = detected_pack_mode;
        if (pack_mode != OPEN3D_PACK_NONE)
        {
            flips = Open3DAdvanceEye(sys, sys->enabled, frame_clock);
            show_right_eye = sys->show_right_eye;

            if (sys->swap_eyes)
                show_right_eye = !show_right_eye;

            Open3DApplyEyeCrop(&vd->source, pack_mode, show_right_eye, &eye_source);
            Open3DEmitterQueueEye(sys, show_right_eye, frame_clock);

            if (flips > 1)
            {
                sys->late_flip_events++;
                sys->late_flip_steps_total += flips - 1;

                if (sys->target_flip_hz > 0.0 &&
                    (flips % 2) == 0 &&
                    !sys->warned_flip_rate_limited)
                {
                    msg_Warn(vd,
                             "open3d target flip rate %.3f Hz is higher than current render cadence; effective eye alternation is present-limited",
                             sys->target_flip_hz);
                    sys->warned_flip_rate_limited = true;
                }
            }

            sys->frame_count++;
            Open3DMaybeLogStatus(vd, sys, pack_mode, show_right_eye, frame_clock, flips,
                                 &eye_source);
            Open3DMaybeLogPresenterTelemetry(vd, sys, pack_mode, show_right_eye, frame_clock,
                                             flips);
            sys->warned_missing_pack = false;
        }
        else if (!sys->warned_missing_pack)
        {
            msg_Warn(vd,
                     "open3d enabled but packed stereo could not be inferred from the current source geometry; set --open3d-layout=sbs/tb or explicit --open3d-layout=sbs-full|sbs-half|tb-full|tb-half");
            sys->warned_missing_pack = true;
            Open3DEmitterRequestEyeReset(sys);
        }
        else
        {
            Open3DEmitterRequestEyeReset(sys);
        }
    }
    else
    {
        pack_mode = detected_pack_mode;
        sys->warned_missing_pack = false;
        Open3DEmitterRequestEyeReset(sys);
    }

    sys->debug_bluray_subtitle_active = false;
    sys->debug_bluray_subtitle_cache_hit = false;
    subpicture_t *bridge_subpicture = Open3DCloneDirectBluraySubtitleSubpicture(vd);
    subpicture_t *forced_bluray_subpicture = NULL;
    subpicture_t *forced_mkv_subpicture = NULL;
    subpicture_t *render_subpicture = subpicture;
    if (subpicture == NULL)
        sys->bluray_subtitle_offset_latch.valid = false;
    if (bridge_subpicture == NULL)
    {
        forced_bluray_subpicture = Open3DCloneBluraySubtitleFallback(vd, subpicture);
        if (forced_bluray_subpicture != NULL)
            render_subpicture = forced_bluray_subpicture;
        else
        {
            forced_mkv_subpicture = Open3DCloneMkvSubtitleFallback(vd, subpicture);
            if (forced_mkv_subpicture != NULL)
                render_subpicture = forced_mkv_subpicture;
        }
    }
    subpicture_t *packed_subpicture = NULL;
    const bool render_subpicture_already_packed =
        !sys->enabled && forced_bluray_subpicture != NULL &&
        render_subpicture == forced_bluray_subpicture;
    if (!sys->enabled && pack_mode != OPEN3D_PACK_NONE &&
        render_subpicture != NULL && render_subpicture->b_subtitle &&
        !render_subpicture_already_packed)
    {
        packed_subpicture = Open3DBuildPackedStereoSubtitleSubpicture(vd, sys,
                                                                      render_subpicture,
                                                                      pack_mode);
        if (packed_subpicture != NULL)
            render_subpicture = packed_subpicture;
    }
    const bool overlay_active = Open3DOverlayActive(sys, pack_mode);
    if (bridge_subpicture != NULL)
    {
        if (!sys->enabled && pack_mode != OPEN3D_PACK_NONE)
        {
            subpicture_t *packed_bridge = Open3DBuildPackedStereoSubtitleSubpicture(vd, sys,
                                                                                    bridge_subpicture,
                                                                                    pack_mode);
            if (packed_bridge != NULL)
            {
                subpicture_Delete(bridge_subpicture);
                bridge_subpicture = packed_bridge;
            }
        }
        else if (!overlay_active && sys->enabled && pack_mode != OPEN3D_PACK_NONE)
        {
            unsigned bridge_width = eye_source.i_visible_width > 0
                                  ? eye_source.i_visible_width
                                  : (eye_source.i_width > 0 ? eye_source.i_width
                                                            : vd->source.i_visible_width);
            if (bridge_width == 0)
                bridge_width = vd->source.i_width;
            Open3DApplySubtitleStereoShift(vd, sys, bridge_subpicture,
                                           bridge_width, show_right_eye);
        }
    }
    subpicture_t *owned_render_subpicture = NULL;
    if (!overlay_active &&
        sys->enabled &&
        pack_mode != OPEN3D_PACK_NONE &&
        bridge_subpicture == NULL &&
        forced_bluray_subpicture == NULL &&
        render_subpicture != NULL &&
        render_subpicture->b_subtitle)
    {
        const unsigned shift_width = eye_source.i_visible_width > 0
                                   ? eye_source.i_visible_width
                                   : (eye_source.i_width > 0 ? eye_source.i_width
                                                              : vd->source.i_visible_width);
        subpicture_t *shifted = Open3DCloneSubpictureShallow(render_subpicture);
        if (shifted != NULL)
        {
            Open3DApplySubtitleStereoShift(vd, sys, shifted, shift_width, show_right_eye);
            render_subpicture = shifted;
            owned_render_subpicture = shifted;
        }
    }

    if (overlay_active)
    {
        subpicture_t *merged = Open3DBuildOverlaySubpicture(vd, sys, render_subpicture,
                                                            render_subpicture == forced_bluray_subpicture,
                                                            bridge_subpicture,
                                                            false,
                                                            &eye_source,
                                                            pack_mode,
                                                            show_right_eye,
                                                            mdate());
        if (merged != NULL)
        {
            if (packed_subpicture != NULL)
            {
                subpicture_Delete(packed_subpicture);
                packed_subpicture = NULL;
            }
            render_subpicture = merged;
            owned_render_subpicture = merged;
            sys->warned_overlay_alloc = false;
        }
        else if (!sys->warned_overlay_alloc)
        {
            msg_Warn(vd,
                     "open3d overlay allocation failed; rendering without trigger/calibration regions");
            sys->warned_overlay_alloc = true;
        }
    }
    else if (bridge_subpicture != NULL)
    {
        if (render_subpicture == NULL)
        {
            render_subpicture = bridge_subpicture;
            owned_render_subpicture = bridge_subpicture;
        }
        else
        {
            subpicture_t *merged = Open3DMergeSubpictures(render_subpicture,
                                                          bridge_subpicture);
            if (merged != NULL)
            {
                render_subpicture = merged;
                owned_render_subpicture = merged;
            }
        }
        sys->warned_overlay_alloc = false;
    }
    else if (packed_subpicture != NULL)
    {
        owned_render_subpicture = packed_subpicture;
        sys->warned_overlay_alloc = false;
    }
    else if (forced_bluray_subpicture != NULL)
    {
        owned_render_subpicture = forced_bluray_subpicture;
        sys->warned_overlay_alloc = false;
    }
    else
    {
        sys->warned_overlay_alloc = false;
    }

    sys->prepared_owned_subpicture = owned_render_subpicture;
    sys->prepared_eye_source = eye_source;

    vlc_mutex_lock(&sys->gl_lock);
    if (vlc_gl_MakeCurrent(sys->gl) == VLC_SUCCESS)
    {
        vout_display_opengl_Prepare(sys->vgl, pic, render_subpicture);
        vlc_gl_ReleaseCurrent(sys->gl);
    }
    vlc_mutex_unlock(&sys->gl_lock);

    if (packed_subpicture != NULL &&
        packed_subpicture != owned_render_subpicture)
        subpicture_Delete(packed_subpicture);
    if (bridge_subpicture != NULL &&
        bridge_subpicture != owned_render_subpicture)
        subpicture_Delete(bridge_subpicture);
    if (forced_bluray_subpicture != NULL &&
        forced_bluray_subpicture != owned_render_subpicture &&
        forced_bluray_subpicture != packed_subpicture)
        subpicture_Delete(forced_bluray_subpicture);
    if (forced_mkv_subpicture != NULL &&
        forced_mkv_subpicture != owned_render_subpicture &&
        forced_mkv_subpicture != packed_subpicture)
        subpicture_Delete(forced_mkv_subpicture);
}

static void PictureDisplay(vout_display_t *vd, picture_t *pic, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;

    if (sys->presenter_enable)
    {
        picture_Release(pic);
        if (subpicture != NULL)
            subpicture_Delete(subpicture);
        return;
    }

    video_format_t eye_source = sys->prepared_eye_source;
    if (eye_source.i_visible_width == 0 || eye_source.i_visible_height == 0)
        eye_source = vd->source;
    open3d_pack_t pack_mode = OPEN3D_PACK_NONE;
    bool show_right_eye = false;
    if (sys->enabled)
    {
        pack_mode = Open3DDetectPackMode(vd, sys);
        show_right_eye = sys->show_right_eye;
        if (sys->swap_eyes)
            show_right_eye = !show_right_eye;
    }

    vlc_mutex_lock(&sys->gl_lock);
    if (vlc_gl_MakeCurrent(sys->gl) == VLC_SUCCESS)
    {
        vout_display_place_t place;
        const bool have_place = Open3DComputePicturePlace(&eye_source, vd->cfg, &place);
        if (have_place)
            Open3DApplyPicturePlace(sys, &place);
        const bool gpu_overlay_active = sys->gpu_overlay_enable && sys->gpu_overlay_ready;
        if (gpu_overlay_active)
            vout_display_opengl_DisplayNoSwap(sys->vgl, &eye_source);
        else
            vout_display_opengl_Display(sys->vgl, &eye_source);
        if (gpu_overlay_active && have_place)
            Open3DDrawGpuOverlay(vd, sys, &eye_source, pack_mode, show_right_eye, &place);
        if (gpu_overlay_active)
            vlc_gl_Swap(sys->gl);
        vlc_gl_ReleaseCurrent(sys->gl);
    }
    vlc_mutex_unlock(&sys->gl_lock);

    Open3DReleasePreparedSubpicture(sys);
    picture_Release(pic);
    if (subpicture != NULL)
        subpicture_Delete(subpicture);
}

static int Control(vout_display_t *vd, int query, va_list ap)
{
    vout_display_sys_t *sys = vd->sys;

    switch (query)
    {
#ifndef NDEBUG
        case VOUT_DISPLAY_RESET_PICTURES:
            vlc_assert_unreachable();
#endif

        case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
        case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
        case VOUT_DISPLAY_CHANGE_ZOOM:
        {
            vout_display_cfg_t cfg = *va_arg(ap, const vout_display_cfg_t *);
            vout_display_place_t place;
            video_format_t src = sys->prepared_eye_source;
            if (src.i_visible_width == 0 || src.i_visible_height == 0)
                src = vd->source;

            if (!Open3DComputePicturePlace(&src, &cfg, &place))
                return VLC_SUCCESS;
            vlc_mutex_lock(&sys->gl_lock);
            if (vlc_gl_MakeCurrent(sys->gl) != VLC_SUCCESS)
            {
                vlc_mutex_unlock(&sys->gl_lock);
                return VLC_EGENERIC;
            }

            sys->place_force = true;
            Open3DApplyPicturePlace(sys, &place);
            vlc_gl_ReleaseCurrent(sys->gl);
            vlc_mutex_unlock(&sys->gl_lock);
            return VLC_SUCCESS;
        }

        case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
        case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
        {
            vout_display_place_t place;
            video_format_t src = sys->prepared_eye_source;
            if (src.i_visible_width == 0 || src.i_visible_height == 0)
                src = vd->source;

            if (!Open3DComputePicturePlace(&src, vd->cfg, &place))
                return VLC_SUCCESS;
            vlc_mutex_lock(&sys->gl_lock);
            if (vlc_gl_MakeCurrent(sys->gl) != VLC_SUCCESS)
            {
                vlc_mutex_unlock(&sys->gl_lock);
                return VLC_EGENERIC;
            }

            sys->place_force = true;
            Open3DApplyPicturePlace(sys, &place);
            vlc_gl_ReleaseCurrent(sys->gl);
            vlc_mutex_unlock(&sys->gl_lock);
            return VLC_SUCCESS;
        }

        case VOUT_DISPLAY_CHANGE_VIEWPOINT:
        {
            vlc_mutex_lock(&sys->gl_lock);
            int ret = vout_display_opengl_SetViewpoint(
                sys->vgl,
                &va_arg(ap, const vout_display_cfg_t *)->viewpoint);
            vlc_mutex_unlock(&sys->gl_lock);
            return ret;
        }

        default:
            msg_Err(vd, "Unknown request %d", query);
            return VLC_EGENERIC;
    }
}
