#!/usr/bin/env bash
set -uo pipefail

ROOT=/home/codex/ucm-dramstore-sim-codex-20260728
OUT_DIR="${1:-/tmp/dramstore-long-soak-20260728/phase3-multistore}"
DURATION_SECONDS="${DURATION_SECONDS:-1200}"
POOL_CONTROL=110.138.0.3:49400
STORE_CONTROL=110.138.0.3:49401
POOL_ONE_SIDED=110.138.0.3:49500
STORE0_ONE_SIDED=110.138.0.3:49501
STORE1_ONE_SIDED=110.138.0.3:49502
POOL_BIN="${ROOT}/build-dramstore-sim-cann851/ucm/store/dram/drampool"
SIM_BIN="${ROOT}/ucm/store/dram/build/dramstore_sim"
RUNNER="${ROOT}/ucm/store/dram/scripts/run_dramstore_sim.sh"
RESULTS="${OUT_DIR}/phase3-iterations.tsv"
SUMMARY="${OUT_DIR}/phase3-summary.txt"

mkdir -p "${OUT_DIR}"
printf 'iteration\tstart\tend\tcase\tstatus\tsim_pass_markers\tround_pass_markers\tcritical_lines\tpool_error_lines\tforced_pool_kill\tsim_log\tpool_log\n' >"${RESULTS}"

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
    case $(((iteration - 1) % 3)) in
        0)
            case_name=tiny_64x16
            block_size=64
            block_num=16
            put_count=300
            get_count=300
            lookup_count=100
            rounds=2
            ;;
        1)
            case_name=medium_4096x4
            block_size=4096
            block_num=4
            put_count=500
            get_count=500
            lookup_count=100
            rounds=2
            ;;
        *)
            case_name=large_65536x2
            block_size=65536
            block_num=2
            put_count=100
            get_count=100
            lookup_count=40
            rounds=2
            ;;
    esac

    config="${OUT_DIR}/phase3-${iteration}.yaml"
    pool_log="${OUT_DIR}/phase3-pool-$(printf '%03d' "${iteration}").log"
    sim_log="${OUT_DIR}/phase3-sim-$(printf '%03d' "${iteration}")-${case_name}.log"
    cat >"${config}" <<EOF
transport:
  device_ids: [4]
  endpoints:
    - two_sided: "${POOL_CONTROL}"
      one_sided: "${POOL_ONE_SIDED}"
    - two_sided: "110.138.0.3:49401"
      one_sided: "${STORE0_ONE_SIDED}"
    - two_sided: "110.138.0.3:49402"
      one_sided: "${STORE1_ONE_SIDED}"
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
  timeout_ms: 10000
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
        --kvcache-block-sizes "${block_size}" \
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
            --devices 5,6 \
            --store-index-base 0 \
            --key-seed "$((202607300000 + iteration))" \
            --block-size "${block_size}" \
            --block-num "${block_num}" \
            --rounds "${rounds}" \
            --put "${put_count}" \
            --get "${get_count}" \
            --lookup-exist "${lookup_count}" \
            --lookup-miss "${lookup_count}" >"${sim_log}" 2>&1
        status=$?
    else
        printf 'DramPool did not become ready\n' >"${sim_log}"
    fi

    sim_pass_markers="$(grep -c '^dramstore simulation passed$' "${sim_log}" || true)"
    round_pass_markers="$(grep -Ec '^store\[[01]\] round [12] passed$' "${sim_log}" || true)"
    critical_lines="$(grep -Eic \
        'timed out|response validation failed|GetStatus failed|TransportManager::Shutdown failed|server error|Segmentation fault|double free|use-after-free' \
        "${sim_log}" || true)"
    pool_error_lines="$(grep -Eic '\\[UC\\]\\[E\\]|server error|Segmentation fault' "${pool_log}" || true)"

    forced=0
    cleanup_pool
    forced=$?
    iter_end="$(date '+%F %T')"

    if [[ "${status}" -eq 0 && "${sim_pass_markers}" -eq 2 &&
          "${round_pass_markers}" -eq 4 && "${critical_lines}" -eq 0 &&
          "${pool_error_lines}" -eq 0 && "${forced}" -eq 0 ]]; then
        passes=$((passes + 1))
    else
        unexpected_failures=$((unexpected_failures + 1))
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${iteration}" "${iter_start}" "${iter_end}" "${case_name}" "${status}" \
        "${sim_pass_markers}" "${round_pass_markers}" "${critical_lines}" \
        "${pool_error_lines}" "${forced}" "${sim_log}" "${pool_log}" >>"${RESULTS}"

    if [[ "${unexpected_failures}" -gt 0 ]]; then break; fi
done

cat >"${SUMMARY}" <<EOF
phase=phase3_multistore
phase_start=${phase_start_text}
phase_end=$(date '+%F %T %z')
requested_duration_seconds=${DURATION_SECONDS}
actual_duration_seconds=$(( $(date +%s) - phase_start_epoch ))
iterations=${iteration}
passes=${passes}
unexpected_failures=${unexpected_failures}
forced_pool_kills=${forced_pool_kills}
results=${RESULTS}
EOF

if [[ "${unexpected_failures}" -gt 0 ]]; then exit 1; fi
exit 0
