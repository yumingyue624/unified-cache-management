#!/usr/bin/env bash
set -uo pipefail

ROOT=/home/codex/ucm-dramstore-sim-codex-20260728
OUT_DIR="${1:-/tmp/dramstore-long-soak-20260728/phase4-faults}"
DURATION_SECONDS="${DURATION_SECONDS:-1800}"
MAX_ITERATIONS="${MAX_ITERATIONS:-0}"
POOL_CONTROL=110.138.0.3:49400
STORE_CONTROL=110.138.0.3:49401
POOL_ONE_SIDED=110.138.0.3:49500
STORE_ONE_SIDED=110.138.0.3:49501
POOL_BIN="${ROOT}/build-dramstore-sim-cann851/ucm/store/dram/drampool"
SIM_BIN="${ROOT}/ucm/store/dram/build/dramstore_sim"
RUNNER="${ROOT}/ucm/store/dram/scripts/run_dramstore_sim.sh"
CONFIG="${OUT_DIR}/phase4.yaml"
RESULTS="${OUT_DIR}/phase4-iterations.tsv"
SUMMARY="${OUT_DIR}/phase4-summary.txt"

mkdir -p "${OUT_DIR}"
printf 'iteration\tstart\tend\tscenario\tprimary_status\tprimary_ok\trecovery_status\trecovery_ok\tremaining_sim\tforced_cleanup\tprimary_log\trecovery_log\tpool_log\n' >"${RESULTS}"

source /usr/local/Ascend/cann-8.5.1/set_env.sh
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-8.5.1
export ASCEND_TOOLKIT_HOME=/usr/local/Ascend/cann-8.5.1
export HIXL_HOME=/usr/local/Ascend/cann-8.5.1/aarch64-linux
export DRAMSTORE_SIM_BIN="${SIM_BIN}"

cat >"${CONFIG}" <<EOF
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
  timeout_ms: 10000
logger:
  level: info
  dir: "${OUT_DIR}/runtime-logs"
  max_files: 30
  max_size_mb: 10
EOF

phase_start_text="$(date '+%F %T %z')"
phase_start_epoch="$(date +%s)"
deadline=$((phase_start_epoch + DURATION_SECONDS))
iteration=0
passes=0
unexpected_failures=0
intentional_pool_crashes=0
forced_cleanups=0
pool_pid=""
listener_pid=""
wrapper_pid=""

stop_listener()
{
    if [[ -n "${listener_pid}" ]] && kill -0 "${listener_pid}" 2>/dev/null; then
        kill -TERM "${listener_pid}" 2>/dev/null || true
        wait "${listener_pid}" 2>/dev/null || true
    fi
    listener_pid=""
}

stop_pool()
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
            forced_cleanups=$((forced_cleanups + 1))
            kill -KILL "${pool_pid}" 2>/dev/null || true
        fi
        wait "${pool_pid}" 2>/dev/null || true
    fi
    pool_pid=""
    return "${forced}"
}

cleanup()
{
    stop_listener
    if [[ -n "${wrapper_pid}" ]] && kill -0 "${wrapper_pid}" 2>/dev/null; then
        kill -TERM "${wrapper_pid}" 2>/dev/null || true
        wait "${wrapper_pid}" 2>/dev/null || true
    fi
    wrapper_pid=""
    stop_pool || true
}
trap cleanup EXIT INT TERM

start_pool()
{
    local log=$1
    : >"${log}"
    "${POOL_BIN}" \
        --addr "${POOL_CONTROL}" \
        --nics mlx5_0 \
        --pool-size-gb 1 \
        --kvcache-block-sizes 4096 \
        --kvcache-block-proportions 1 \
        --ttl-minutes 120 \
        --config "${CONFIG}" >"${log}" 2>&1 &
    pool_pid=$!
    for _ in $(seq 1 300); do
        grep -q 'DramPool service ready' "${log}" && return 0
        kill -0 "${pool_pid}" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

run_recovery()
{
    local log=$1
    local seed=$2
    timeout 120 bash "${RUNNER}" \
        --config "${CONFIG}" \
        --pool-control "${POOL_CONTROL}" \
        --store-control "${STORE_CONTROL}" \
        --devices 5 \
        --key-seed "${seed}" \
        --block-size 4096 \
        --block-num 1 \
        --rounds 1 \
        --put 100 \
        --get 100 \
        --lookup-exist 20 \
        --lookup-miss 20 >"${log}" 2>&1
    local status=$?
    if [[ "${status}" -eq 0 ]] &&
        grep -q '^dramstore simulation passed$' "${log}" &&
        ! grep -Eqi 'timed out|response validation failed|GetStatus failed|server error' "${log}"; then
        return 0
    fi
    return "${status:-1}"
}

start_listener()
{
    local port=$1
    local log=$2
    python3 - "${port}" >"${log}" 2>&1 <<'PY' &
import signal
import socket
import sys
import time

sock = socket.socket()
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("110.138.0.3", int(sys.argv[1])))
sock.listen(1)
print("READY", flush=True)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
while True:
    time.sleep(1)
PY
    listener_pid=$!
    for _ in $(seq 1 100); do
        grep -q READY "${log}" && return 0
        kill -0 "${listener_pid}" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

while [[ "${iteration}" -eq 0 || "$(date +%s)" -lt "${deadline}" ]]; do
    iteration=$((iteration + 1))
    scenario_id=$(((iteration - 1) % 5))
    iter_start="$(date '+%F %T')"
    primary_log="${OUT_DIR}/phase4-primary-$(printf '%03d' "${iteration}").log"
    recovery_log="${OUT_DIR}/phase4-recovery-$(printf '%03d' "${iteration}").log"
    pool_log="${OUT_DIR}/phase4-pool-$(printf '%03d' "${iteration}").log"
    primary_status=125
    primary_ok=0
    recovery_status=125
    recovery_ok=0
    remaining_sim=0
    forced=0

    if ! start_pool "${pool_log}"; then
        scenario=pool_start_failure
    else
        case "${scenario_id}" in
            0)
                scenario=pool_sigkill_midflight
                timeout 180 bash "${RUNNER}" \
                    --config "${CONFIG}" \
                    --pool-control "${POOL_CONTROL}" \
                    --store-control "${STORE_CONTROL}" \
                    --devices 5 \
                    --key-seed "$((202607310000 + iteration))" \
                    --block-size 4096 --block-num 1 --rounds 1 \
                    --put 10000 --get 10000 --lookup-exist 0 --lookup-miss 0 \
                    >"${primary_log}" 2>&1 &
                wrapper_pid=$!
                sleep 3
                kill -KILL "${pool_pid}" 2>/dev/null || true
                wait "${pool_pid}" 2>/dev/null || true
                pool_pid=""
                intentional_pool_crashes=$((intentional_pool_crashes + 1))
                wait "${wrapper_pid}"
                primary_status=$?
                wrapper_pid=""
                timeout_lines="$(grep -Eic 'timed out|failed tasks|simulation failed' "${primary_log}" || true)"
                if [[ "${primary_status}" -ne 0 && "${timeout_lines}" -gt 0 ]]; then
                    primary_ok=1
                fi
                if start_pool "${pool_log}.restarted"; then
                    run_recovery "${recovery_log}" "$((202607319000 + iteration))"
                    recovery_status=$?
                    [[ "${recovery_status}" -eq 0 ]] && recovery_ok=1
                fi
                ;;
            1)
                scenario=wrapper_sigterm_midflight
                bash "${RUNNER}" \
                    --config "${CONFIG}" \
                    --pool-control "${POOL_CONTROL}" \
                    --store-control "${STORE_CONTROL}" \
                    --devices 5 \
                    --key-seed "$((202607320000 + iteration))" \
                    --block-size 4096 --block-num 1 --rounds 1 \
                    --put 100 --get 100 --lookup-exist 20 --lookup-miss 20 \
                    >"${primary_log}" 2>&1 &
                wrapper_pid=$!
                for _ in $(seq 1 100); do
                    grep -q 'Starting DramStore process' "${primary_log}" && break
                    sleep 0.05
                done
                sleep 1
                kill -TERM "${wrapper_pid}" 2>/dev/null || true
                wait "${wrapper_pid}"
                primary_status=$?
                wrapper_pid=""
                sleep 2
                remaining_sim="$(pgrep -af '[d]ramstore_sim.*--store-control 110.138.0.3:49401' |
                    wc -l)"
                if [[ "${primary_status}" -eq 143 && "${remaining_sim}" -eq 0 ]]; then
                    primary_ok=1
                fi
                sleep 1
                run_recovery "${recovery_log}" "$((202607329000 + iteration))"
                recovery_status=$?
                [[ "${recovery_status}" -eq 0 ]] && recovery_ok=1
                ;;
            2)
                scenario=store_control_collision
                listener_log="${OUT_DIR}/phase4-listener-$(printf '%03d' "${iteration}").log"
                if start_listener 49401 "${listener_log}"; then
                    timeout 60 bash "${RUNNER}" \
                        --config "${CONFIG}" --pool-control "${POOL_CONTROL}" \
                        --store-control "${STORE_CONTROL}" --devices 5 \
                        --key-seed "$((202607330000 + iteration))" \
                        --block-size 4096 --block-num 1 --rounds 1 \
                        --put 10 --get 10 --lookup-exist 0 --lookup-miss 0 \
                        >"${primary_log}" 2>&1
                    primary_status=$?
                    disconnect_noise="$(grep -Eic 'coordinated disconnect failed|Disconnect failed' \
                        "${primary_log}" || true)"
                    [[ "${primary_status}" -ne 0 && "${disconnect_noise}" -eq 0 ]] &&
                        primary_ok=1
                fi
                stop_listener
                run_recovery "${recovery_log}" "$((202607339000 + iteration))"
                recovery_status=$?
                [[ "${recovery_status}" -eq 0 ]] && recovery_ok=1
                ;;
            3)
                scenario=store_one_sided_collision
                listener_log="${OUT_DIR}/phase4-listener-$(printf '%03d' "${iteration}").log"
                if start_listener 49501 "${listener_log}"; then
                    timeout 60 bash "${RUNNER}" \
                        --config "${CONFIG}" --pool-control "${POOL_CONTROL}" \
                        --store-control "${STORE_CONTROL}" --devices 5 \
                        --key-seed "$((202607340000 + iteration))" \
                        --block-size 4096 --block-num 1 --rounds 1 \
                        --put 10 --get 10 --lookup-exist 0 --lookup-miss 0 \
                        >"${primary_log}" 2>&1
                    primary_status=$?
                    [[ "${primary_status}" -ne 0 ]] && primary_ok=1
                fi
                stop_listener
                run_recovery "${recovery_log}" "$((202607349000 + iteration))"
                recovery_status=$?
                [[ "${recovery_status}" -eq 0 ]] && recovery_ok=1
                ;;
            *)
                scenario=invalid_device
                timeout 60 bash "${RUNNER}" \
                    --config "${CONFIG}" --pool-control "${POOL_CONTROL}" \
                    --store-control "${STORE_CONTROL}" --devices 99 \
                    --key-seed "$((202607350000 + iteration))" \
                    --block-size 4096 --block-num 1 --rounds 1 \
                    --put 10 --get 10 --lookup-exist 0 --lookup-miss 0 \
                    >"${primary_log}" 2>&1
                primary_status=$?
                if [[ "${primary_status}" -ne 0 ]] &&
                    grep -q 'aclrtSetDevice returned 107001' "${primary_log}"; then
                    primary_ok=1
                fi
                run_recovery "${recovery_log}" "$((202607359000 + iteration))"
                recovery_status=$?
                [[ "${recovery_status}" -eq 0 ]] && recovery_ok=1
                ;;
        esac
    fi

    stop_pool
    forced=$?
    iter_end="$(date '+%F %T')"
    if [[ "${primary_ok}" -eq 1 && "${recovery_ok}" -eq 1 && "${forced}" -eq 0 ]]; then
        passes=$((passes + 1))
    else
        unexpected_failures=$((unexpected_failures + 1))
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${iteration}" "${iter_start}" "${iter_end}" "${scenario}" \
        "${primary_status}" "${primary_ok}" "${recovery_status}" "${recovery_ok}" \
        "${remaining_sim}" "${forced}" "${primary_log}" "${recovery_log}" "${pool_log}" \
        >>"${RESULTS}"
    if [[ "${unexpected_failures}" -gt 0 ]]; then break; fi
    if [[ "${MAX_ITERATIONS}" -gt 0 && "${iteration}" -ge "${MAX_ITERATIONS}" ]]; then break; fi
done

cat >"${SUMMARY}" <<EOF
phase=phase4_faults
phase_start=${phase_start_text}
phase_end=$(date '+%F %T %z')
requested_duration_seconds=${DURATION_SECONDS}
actual_duration_seconds=$(( $(date +%s) - phase_start_epoch ))
iterations=${iteration}
passes=${passes}
unexpected_failures=${unexpected_failures}
intentional_pool_crashes=${intentional_pool_crashes}
forced_cleanups=${forced_cleanups}
results=${RESULTS}
EOF

if [[ "${unexpected_failures}" -gt 0 ]]; then exit 1; fi
exit 0
