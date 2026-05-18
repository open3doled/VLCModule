#!/bin/bash
set -euo pipefail

open3d_exec_host_tool() {
  env -u LD_LIBRARY_PATH -u VLC_PLUGIN_PATH "$@"
}

if command -v prlimit >/dev/null 2>&1; then
  rt_rttime_soft_us="${OPEN3D_PROCESS_RT_RTTIME_US:-500000}"
  rt_rttime_hard_us="${OPEN3D_PROCESS_RT_RTTIME_HARD_US:-1000000}"

  if [[ "${rt_rttime_soft_us}" =~ ^[0-9]+$ && "${rt_rttime_hard_us}" =~ ^[0-9]+$ ]]; then
    if (( rt_rttime_soft_us > 0 && rt_rttime_hard_us > 0 )); then
      if (( rt_rttime_hard_us < rt_rttime_soft_us )); then
        rt_rttime_hard_us="${rt_rttime_soft_us}"
      fi
      if open3d_exec_host_tool prlimit --pid "$$" --rttime="${rt_rttime_soft_us}:${rt_rttime_hard_us}" >/dev/null 2>&1; then
        printf '%s\n' "open3d isolation applied: RT runtime guard soft=${rt_rttime_soft_us}us hard=${rt_rttime_hard_us}us"
      else
        printf '%s\n' "open3d isolation applied: RT runtime guard request failed soft=${rt_rttime_soft_us}us hard=${rt_rttime_hard_us}us"
      fi
    fi
  else
    printf '%s\n' "open3d isolation applied: ignoring invalid RT runtime guard soft='${rt_rttime_soft_us}' hard='${rt_rttime_hard_us}'"
  fi
fi

if [[ "${OPEN3D_LOG_APPLIED_ISOLATION_STATE:-1}" == "1" ]]; then
  printf '%s\n' "open3d isolation applied: pid=$$ ppid=$PPID"

  if [[ -r /proc/self/status ]]; then
    open3d_exec_host_tool awk '
      /^Cpus_allowed_list:/ { print "open3d isolation applied: " $0 }
      /^Mems_allowed_list:/ { print "open3d isolation applied: " $0 }
    ' /proc/self/status
  fi

  if command -v taskset >/dev/null 2>&1; then
    open3d_exec_host_tool taskset -pc "$$" 2>/dev/null | open3d_exec_host_tool sed 's/^/open3d isolation applied: /' || true
  fi

  if command -v ionice >/dev/null 2>&1; then
    open3d_exec_host_tool ionice -p "$$" 2>/dev/null | open3d_exec_host_tool sed 's/^/open3d isolation applied: /' || true
  fi

  if command -v chrt >/dev/null 2>&1; then
    open3d_exec_host_tool chrt -p "$$" 2>/dev/null | open3d_exec_host_tool sed 's/^/open3d isolation applied: /' || true
  fi
fi

exec "$@"
