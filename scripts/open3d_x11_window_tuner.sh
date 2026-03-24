#!/usr/bin/env bash
set -euo pipefail

TARGET_PID="${1:-}"
MODE="${2:-bypass_compositor}"
TIMEOUT_SECONDS="${OPEN3D_X11_TUNER_TIMEOUT_SECONDS:-45}"
POLL_SECONDS="${OPEN3D_X11_TUNER_POLL_SECONDS:-0.2}"

if [[ -z "${TARGET_PID}" ]]; then
  echo "open3d x11 tuner: missing target pid" >&2
  exit 2
fi

if [[ -z "${DISPLAY:-}" ]]; then
  echo "open3d x11 tuner: DISPLAY is not set, skipping" >&2
  exit 0
fi

if ! command -v wmctrl >/dev/null 2>&1; then
  echo "open3d x11 tuner: wmctrl not available, skipping" >&2
  exit 0
fi

if ! command -v xprop >/dev/null 2>&1; then
  echo "open3d x11 tuner: xprop not available, skipping" >&2
  exit 0
fi

apply_mode() {
  local win_hex="$1"
  case "${MODE}" in
    bypass_compositor)
      if xprop -id "${win_hex}" -f _NET_WM_BYPASS_COMPOSITOR 32c -set _NET_WM_BYPASS_COMPOSITOR 1 >/dev/null 2>&1; then
        echo "open3d x11 tuner: applied _NET_WM_BYPASS_COMPOSITOR=1 to ${win_hex}" >&2
        return 0
      fi
      return 1
      ;;
    *)
      echo "open3d x11 tuner: unknown mode ${MODE}" >&2
      return 1
      ;;
  esac
}

deadline=$((SECONDS + TIMEOUT_SECONDS))
last_window=""

while kill -0 "${TARGET_PID}" >/dev/null 2>&1; do
  while IFS= read -r line; do
    [[ -n "${line}" ]] || continue
    read -r win_hex desktop win_pid x y w h host title <<<"${line}"
    [[ "${win_hex}" == 0x* ]] || continue
    [[ "${win_pid}" == "${TARGET_PID}" ]] || continue
    [[ "${win_hex}" != "${last_window}" ]] || continue
    if apply_mode "${win_hex}"; then
      last_window="${win_hex}"
      exit 0
    fi
  done < <(wmctrl -lpG 2>/dev/null || true)

  if (( SECONDS >= deadline )); then
    echo "open3d x11 tuner: timeout waiting for target window" >&2
    break
  fi
  sleep "${POLL_SECONDS}"
done

exit 0
