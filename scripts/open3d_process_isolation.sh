#!/usr/bin/env bash

open3d_isolation_log() {
  printf '%s\n' "$*"
}

open3d_isolation_expand_cpuset() {
  local cpuset="$1"
  local token start end cpu
  local -a expanded=()
  local joined=""

  IFS=',' read -r -a tokens <<<"${cpuset}"
  for token in "${tokens[@]}"; do
    [[ -n "${token}" ]] || continue
    if [[ "${token}" == *-* ]]; then
      start="${token%%-*}"
      end="${token##*-}"
      if [[ ! "${start}" =~ ^[0-9]+$ || ! "${end}" =~ ^[0-9]+$ ]]; then
        return 1
      fi
      if (( end < start )); then
        return 1
      fi
      for ((cpu = start; cpu <= end; ++cpu)); do
        expanded+=("${cpu}")
      done
    elif [[ "${token}" =~ ^[0-9]+$ ]]; then
      expanded+=("${token}")
    else
      return 1
    fi
  done

  if (( ${#expanded[@]} == 0 )); then
    return 1
  fi

  joined="$(IFS=,; echo "${expanded[*]}")"
  printf '%s\n' "${joined}"
}

open3d_isolation_systemd_probe_cpuset() {
  local cpuset="$1"
  local probed=""

  probed="$(
    systemd-run --user --scope --quiet -p "AllowedCPUs=${cpuset}" \
      /bin/bash -lc \
      'awk -F":" '"'"'/^Cpus_allowed_list:/ { gsub(/^[ \t]+/, "", $2); print $2; exit }'"'"' /proc/self/status' \
      2>/dev/null || true
  )"

  printf '%s\n' "${probed}"
}

open3d_isolation_systemd_cpuset_supported() {
  local cpuset="$1"
  local requested_normalized=""
  local probed=""
  local probed_normalized=""

  requested_normalized="$(open3d_isolation_expand_cpuset "${cpuset}" 2>/dev/null || true)"
  [[ -n "${requested_normalized}" ]] || return 1

  probed="$(open3d_isolation_systemd_probe_cpuset "${cpuset}")"
  [[ -n "${probed}" ]] || return 1

  probed_normalized="$(open3d_isolation_expand_cpuset "${probed}" 2>/dev/null || true)"
  [[ -n "${probed_normalized}" ]] || return 1

  [[ "${requested_normalized}" == "${probed_normalized}" ]]
}

open3d_isolation_user_has_presenter_affinity_arg() {
  local token
  local -a args=("$@")
  local i
  for ((i = 0; i < ${#args[@]}; ++i)); do
    token="${args[i]}"
    if [[ "${token}" == --open3d-presenter-affinity-cpu=* ]]; then
      return 0
    fi
    if [[ "${token}" == "--open3d-presenter-affinity-cpu" ]] &&
       (( i + 1 < ${#args[@]} )); then
      return 0
    fi
  done
  return 1
}

open3d_isolation_first_cpu_from_cpuset() {
  local cpuset="$1"
  local token start
  IFS=',' read -r -a tokens <<<"${cpuset}"
  for token in "${tokens[@]}"; do
    [[ -n "${token}" ]] || continue
    if [[ "${token}" == *-* ]]; then
      start="${token%%-*}"
      if [[ "${start}" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "${start}"
        return 0
      fi
    elif [[ "${token}" =~ ^[0-9]+$ ]]; then
      printf '%s\n' "${token}"
      return 0
    fi
  done
  return 1
}

open3d_isolation_append_presenter_args() {
  local out_name="$1"
  shift
  local -n out_ref="${out_name}"
  local preferred_cpu="${OPEN3D_PRESENTER_PREFERRED_CPU:-}"

  [[ -n "${preferred_cpu}" ]] || return 0

  if open3d_isolation_user_has_presenter_affinity_arg "$@"; then
    open3d_isolation_log "open3d isolation: presenter preferred cpu ignored because affinity cpu was already supplied"
    return 0
  fi

  if [[ "${preferred_cpu}" == "auto" ]]; then
    if [[ -z "${OPEN3D_PROCESS_CPUSET:-}" ]]; then
      open3d_isolation_log "open3d isolation: presenter preferred cpu auto requested without OPEN3D_PROCESS_CPUSET, skipping"
      return 0
    fi
    preferred_cpu="$(open3d_isolation_first_cpu_from_cpuset "${OPEN3D_PROCESS_CPUSET}" || true)"
    if [[ -z "${preferred_cpu}" ]]; then
      open3d_isolation_log "open3d isolation: failed to derive presenter cpu from OPEN3D_PROCESS_CPUSET='${OPEN3D_PROCESS_CPUSET}', skipping"
      return 0
    fi
  fi

  if [[ ! "${preferred_cpu}" =~ ^[0-9]+$ ]]; then
    open3d_isolation_log "open3d isolation: invalid presenter preferred cpu '${preferred_cpu}', skipping"
    return 0
  fi

  out_ref+=("--open3d-presenter-affinity-cpu=${preferred_cpu}")
  open3d_isolation_log "open3d isolation: presenter preferred cpu=${preferred_cpu}"
}

open3d_isolation_build_exec_prefix() {
  local out_name="$1"
  local -n out_ref="${out_name}"
  local cpuset="${OPEN3D_PROCESS_CPUSET:-}"
  local cpu_backend="${OPEN3D_PROCESS_CPU_BACKEND:-auto}"
  local sched_policy="${OPEN3D_PROCESS_SCHED_POLICY:-}"
  local rt_priority="${OPEN3D_PROCESS_RT_PRIORITY:-}"
  local io_class="${OPEN3D_PROCESS_IO_CLASS:-}"
  local io_priority="${OPEN3D_PROCESS_IO_PRIORITY:-}"
  out_ref=()

  if [[ -n "${cpuset}" ]]; then
    case "${cpu_backend}" in
      auto|systemd)
        if command -v systemd-run >/dev/null 2>&1 &&
           open3d_isolation_systemd_cpuset_supported "${cpuset}"; then
          out_ref=(systemd-run --user --scope --quiet -p "AllowedCPUs=${cpuset}")
          open3d_isolation_log "open3d isolation: process cpuset=${cpuset} backend=systemd"
        elif command -v systemd-run >/dev/null 2>&1 &&
             systemd-run --user --scope --quiet -p "AllowedCPUs=${cpuset}" /bin/true >/dev/null 2>&1; then
          open3d_isolation_log "open3d isolation: systemd AllowedCPUs probe did not apply cpuset='${cpuset}', falling back to taskset"
        elif [[ "${cpu_backend}" == "systemd" ]]; then
          open3d_isolation_log "open3d isolation: systemd AllowedCPUs backend unavailable for cpuset='${cpuset}', falling back to taskset"
        fi
        ;;&
      auto|taskset|systemd)
        if (( ${#out_ref[@]} == 0 )); then
          if ! command -v taskset >/dev/null 2>&1; then
            open3d_isolation_log "open3d isolation: taskset not available, falling back to in-process presenter tuning only"
          elif ! taskset --cpu-list "${cpuset}" /bin/true >/dev/null 2>&1; then
            open3d_isolation_log "open3d isolation: invalid or unsupported OPEN3D_PROCESS_CPUSET='${cpuset}', falling back to in-process presenter tuning only"
          else
            out_ref=(taskset --cpu-list "${cpuset}")
            open3d_isolation_log "open3d isolation: process cpuset=${cpuset} backend=taskset"
          fi
        fi
        ;;
      *)
        open3d_isolation_log "open3d isolation: unsupported OPEN3D_PROCESS_CPU_BACKEND='${cpu_backend}', falling back to current in-process presenter tuning"
        ;;
    esac
  fi

  if [[ -n "${sched_policy}" ]]; then
    if ! command -v chrt >/dev/null 2>&1; then
      open3d_isolation_log "open3d isolation: chrt not available, falling back to in-process presenter tuning only"
    else
      if [[ -z "${rt_priority}" ]]; then
        rt_priority=10
      fi

      if ! [[ "${rt_priority}" =~ ^[0-9]+$ ]]; then
        open3d_isolation_log "open3d isolation: invalid OPEN3D_PROCESS_RT_PRIORITY='${rt_priority}', skipping scheduler policy"
      else
        case "${sched_policy}" in
          fifo)
            if ! chrt --fifo "${rt_priority}" /bin/true >/dev/null 2>&1; then
              open3d_isolation_log "open3d isolation: fifo priority=${rt_priority} denied, falling back to current in-process presenter tuning"
            else
              out_ref+=(chrt --fifo "${rt_priority}")
              open3d_isolation_log "open3d isolation: process scheduler=fifo priority=${rt_priority}"
            fi
            ;;
          rr)
            if ! chrt --rr "${rt_priority}" /bin/true >/dev/null 2>&1; then
              open3d_isolation_log "open3d isolation: rr priority=${rt_priority} denied, falling back to current in-process presenter tuning"
            else
              out_ref+=(chrt --rr "${rt_priority}")
              open3d_isolation_log "open3d isolation: process scheduler=rr priority=${rt_priority}"
            fi
            ;;
          *)
            open3d_isolation_log "open3d isolation: unsupported OPEN3D_PROCESS_SCHED_POLICY='${sched_policy}', skipping scheduler policy"
            ;;
        esac
      fi
    fi
  fi

  if [[ -z "${io_class}" ]]; then
    return 0
  fi

  if ! command -v ionice >/dev/null 2>&1; then
    open3d_isolation_log "open3d isolation: ionice not available, falling back to current process priority"
    return 0
  fi

  case "${io_class}" in
    realtime)
      if [[ -z "${io_priority}" ]]; then
        io_priority=0
      fi
      if ! [[ "${io_priority}" =~ ^[0-7]$ ]]; then
        open3d_isolation_log "open3d isolation: invalid OPEN3D_PROCESS_IO_PRIORITY='${io_priority}' for realtime class, skipping I/O policy"
        return 0
      fi
      if ! ionice -c 1 -n "${io_priority}" /bin/true >/dev/null 2>&1; then
        open3d_isolation_log "open3d isolation: realtime ionice priority=${io_priority} denied, falling back to current process I/O priority"
        return 0
      fi
      out_ref+=(ionice -c 1 -n "${io_priority}")
      open3d_isolation_log "open3d isolation: process ionice class=realtime priority=${io_priority}"
      ;;
    best-effort|besteffort)
      if [[ -z "${io_priority}" ]]; then
        io_priority=0
      fi
      if ! [[ "${io_priority}" =~ ^[0-7]$ ]]; then
        open3d_isolation_log "open3d isolation: invalid OPEN3D_PROCESS_IO_PRIORITY='${io_priority}' for best-effort class, skipping I/O policy"
        return 0
      fi
      if ! ionice -c 2 -n "${io_priority}" /bin/true >/dev/null 2>&1; then
        open3d_isolation_log "open3d isolation: best-effort ionice priority=${io_priority} denied, falling back to current process I/O priority"
        return 0
      fi
      out_ref+=(ionice -c 2 -n "${io_priority}")
      open3d_isolation_log "open3d isolation: process ionice class=best-effort priority=${io_priority}"
      ;;
    idle)
      if ! ionice -c 3 /bin/true >/dev/null 2>&1; then
        open3d_isolation_log "open3d isolation: idle ionice denied, falling back to current process I/O priority"
        return 0
      fi
      out_ref+=(ionice -c 3)
      open3d_isolation_log "open3d isolation: process ionice class=idle"
      ;;
    *)
      open3d_isolation_log "open3d isolation: unsupported OPEN3D_PROCESS_IO_CLASS='${io_class}', skipping I/O policy"
      ;;
  esac
}
