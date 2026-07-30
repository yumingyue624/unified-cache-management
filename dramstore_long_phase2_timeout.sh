#!/usr/bin/env bash
set -uo pipefail

ROOT=/home/codex/ucm-dramstore-sim-codex-20260728
OUT_DIR="${1:-/tmp/dramstore-long-soak-20260728/phase2-timeout}"
DURATION_SECONDS="${DURATION_SECONDS:-900}"
POOL_CONTROL=110.138.0.3:49400
STORE_CONTROL=110.138.0.3:49401
POOL_ONE_SIDED=110.138.0.3:49500
STORE_ONE_SIDED=110.138.0.3:49501
POOL_BIN="${ROOT}/build-dramstore-sim-cann851/ucm/store/dram/drampool"
SIM_BIN="${ROOT}/ucm/store/dram/build/dramstore_sim"
RUNNER="${ROOT}/ucm/store/dram/scripts/run_dramstore_sim.sh"
RESULTS="${OUT_DIR}/phase2-iterations.tsv"
SUMMARY="${OUT_DIR}/phase2-summary.txt"

mkdir -p "${OUT_DIR}"
printf 'iteration\tstart\tend\ttimeout_ms\tstatus\ttimeout_lines\tvalidation_lines\tgetstatus_lines\tpool_error_lines\tforced_pool_kill\tverdict\tsim_log\tpool_log\n' >"${RESULTS}"

source /usr/local/Ascend/cann-8.5.1/set_env.sh
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-8.5.1
export ASCEND_TOOLKIT_HOME=/usr/local/Ascend/cann-8.5.1
export HIXL_HOME=/usr/local/Ascend/cann-8.5.1/aarch64-linux
export DRAMSTORE_SIM_BIN="${SIM_BIN}"

phase_start_text="$(date '+%F %T %z')"
phase_start_epoch="$(date +%s)"
deadline=$((phase_start_epoch + DURATION_SECONDS))
iteration=0
passes=0
expected_timeout_failures=0
unexpected_failures=0
forced_pool_kills=0
pool_pid=""

cleanup_pool()
{
    local forced=0
    if [[ -n "${pool_pid}" ]] && kill -0 "${pool_pid}" 2>/dev/null; then
        kill -TERM "${pool_pid}" 2>/dev/null || true
        for _ in $(seq 1 300); do
            kill -0 "${pool_pid}" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "${pool_pid}" 2>/dev/null; then
            forced=1
            forced_pool_kills=$((forced_pool_kills + 1))
            kill -KILL "${pool_pid}" 2>/dev/null || true
        fi
        wait "${pool_pid}" 2>/dev/null || true
    fi
    pool_pid=""
    return "${forced}"
}
trap 'cleanup_pool || true' EXIT INT TERM

while [[ "${iteration}" -eq 0 || "$(date +%s)" -lt "${deadline}" ]]; do
    iteration=$((iteration + 1))
    case "${iteration}" in
        1) timeout_ms=5000 ;;
        2) timeout_ms=6000 ;;
        3) timeout_ms=7000 ;;
        4) timeout_ms=8000 ;;
        5) timeout_ms=9000 ;;
        6) timeout_ms=10000 ;;
        *)
            if ((iteration % 2 == 1)); then timeout_ms=5000; else timeout_ms=10000; fi
            ;;
    esac

    config="${OUT_DIR}/phase2-${iteration}.yaml"
    pool_log="${OUT_DIR}/phase2-pool-$(printf '%03d' "${iteration}").log"
    sim_log="${OUT_DIR}/phase2-sim-$(printf '%03d' "${iteration}")-${timeout_ms}ms.log"

    cat >"${config}" <<EOF
transport:
  device_ids: [4]
  endpoints:
    - two_sided: "${POOL_CONTROL}"
      one_sided: "${POOL_ONE_SIDED}"
    - two_sided: "${STORE_CONTROL}"
      one_sided: "${STORE_ONE_SIDED}"
queue:
  request_depth: 65536
  completion_depth: 65536
request_receiver:
  idle_wait_us: 100
poller:
  pending_depth: 64
flag_buffer:
  capacity_mb: 64
  slot_size_bytes: 64
gc:
  enabled: true
  interval_ms: 1000
metadata:
  periodic_eviction_policy: TTL
  deep_eviction_policy: POSITION
  lease_time_ms: 5000
  default_evict_ratio: 0.0
  evict_period_ms: 31536000000
operation:
  timeout_ms: ${timeout_ms}
logger:
  level: info
  dir: "${OUT_DIR}/runtime-logs-${iteration}"
  max_files: 10
  max_size_mb: 10
EOF

    iter_start="$(date '+%F %T')"
    "${POOL_BIN}" \
        --addr "${POOL_CONTROL}" \
        --nics mlx5_0 \
        --pool-size-gb 1 \
        --kvcache-block-sizes 4096 \
        --kvcache-block-proportions 1 \
        --ttl-minutes 120 \
        --config "${config}" >"${pool_log}" 2>&1 &
    pool_pid=$!

    ready=0
    for _ in $(seq 1 300); do
        if grep -q 'DramPool service ready' "${pool_log}"; then ready=1; break; fi
        kill -0 "${pool_pid}" 2>/dev/null || break
        sleep 0.1
    done

    status=125
    if [[ "${ready}" -eq 1 ]]; then
        timeout 180 bash "${RUNNER}" \
            --config "${config}" \
            --pool-control "${POOL_CONTROL}" \
            --store-control "${STORE_CONTROL}" \
            --devices 5 \
            --key-seed "$((202607290000 + iteration))" \
            --block-size 4096 \
            --block-num 1 \
            --rounds 1 \
            --put 10000 \
            --get 10000 \
            --lookup-exist 0 \
            --lookup-miss 0 >"${sim_log}" 2>&1
        status=$?
    else
        printf 'DramPool did not become ready\n' >"${sim_log}"
    fi

    timeout_lines="$(grep -Eic 'timed out|timeout' "${sim_log}" || true)"
    validation_lines="$(grep -Eic 'response validation failed' "${sim_log}" || true)"
    getstatus_lines="$(grep -Eic 'GetStatus failed|handle.*failed' "${sim_log}" || true)"
    pool_error_lines="$(grep -Eic '\\[UC\\]\\[E\\]|server error|Segmentation fault' "${pool_log}" || true)"

    forced=0
    cleanup_pool
    forced=$?
    iter_end="$(date '+%F %T')"

    verdict=unexpected_failure
    coherent=0
    if [[ "${status}" -eq 0 && "${timeout_lines}" -eq 0 ]]; then
        coherent=1
    elif [[ "${status}" -ne 0 && "${timeout_lines}" -gt 0 ]]; then
        coherent=1
    fi

    if [[ "${validation_lines}" -eq 0 && "${getstatus_lines}" -eq 0 &&
          "${pool_error_lines}" -eq 0 && "${forced}" -eq 0 && "${coherent}" -eq 1 ]]; then
        if [[ "${timeout_ms}" -eq 5000 ]]; then
            if [[ "${status}" -ne 0 && "${timeout_lines}" -gt 0 ]]; then
                verdict=expected_timeout
                expected_timeout_failures=$((expected_timeout_failures + 1))
            fi
        elif [[ "${timeout_ms}" -eq 10000 ]]; then
            if [[ "${status}" -eq 0 ]]; then
                verdict=pass
                passes=$((passes + 1))
            fi
        else
            verdict=boundary_observation
            if [[ "${status}" -eq 0 ]]; then
                passes=$((passes + 1))
            else
                expected_timeout_failures=$((expected_timeout_failures + 1))
            fi
        fi
    fi
    if [[ "${verdict}" == unexpected_failure ]]; then
        unexpected_failures=$((unexpected_failures + 1))
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${iteration}" "${iter_start}" "${iter_end}" "${timeout_ms}" "${status}" \
        "${timeout_lines}" "${validation_lines}" "${getstatus_lines}" \
        "${pool_error_lines}" "${forced}" "${verdict}" "${sim_log}" "${pool_log}" \
        >>"${RESULTS}"

    if [[ "${unexpected_failures}" -gt 0 ]]; then break; fi
done

cat >"${SUMMARY}" <<EOF
phase=phase2_timeout
phase_start=${phase_start_text}
phase_end=$(date '+%F %T %z')
requested_duration_seconds=${DURATION_SECONDS}
actual_duration_seconds=$(( $(date +%s) - phase_start_epoch ))
iterations=${iteration}
passes=${passes}
expected_timeout_failures=${expected_timeout_failures}
unexpected_failures=${unexpected_failures}
forced_pool_kills=${forced_pool_kills}
results=${RESULTS}
EOF

if [[ "${unexpected_failures}" -gt 0 ]]; then exit 1; fi
exit 0
