#!/usr/bin/env bash
set -uo pipefail

ROOT=/home/codex/ucm-dramstore-sim-codex-20260728
OUT_DIR="${1:-/tmp/dramstore-long-soak-20260728}"
DURATION_SECONDS="${DURATION_SECONDS:-600}"
MAX_ITERATIONS="${MAX_ITERATIONS:-0}"
CASE_OFFSET="${CASE_OFFSET:-0}"
POOL_CONTROL="${POOL_CONTROL:-110.138.0.3:49400}"
STORE_CONTROL="${STORE_CONTROL:-110.138.0.3:49401}"
POOL_ONE_SIDED="${POOL_ONE_SIDED:-110.138.0.3:49500}"
STORE_ONE_SIDED="${STORE_ONE_SIDED:-110.138.0.3:49501}"
read -r -a POOL_BLOCK_SIZES <<<"${POOL_BLOCK_SIZES:-64 4096 65536}"
read -r -a POOL_BLOCK_PROPORTIONS <<<"${POOL_BLOCK_PROPORTIONS:-1 7 2}"
CONFIG="${OUT_DIR}/phase1-mixed.yaml"
POOL_LOG="${OUT_DIR}/phase1-pool.log"
ITERATIONS="${OUT_DIR}/phase1-iterations.tsv"
SUMMARY="${OUT_DIR}/phase1-summary.txt"
POOL_BIN="${ROOT}/build-dramstore-sim-cann851/ucm/store/dram/drampool"
SIM_BIN="${ROOT}/ucm/store/dram/build/dramstore_sim"
RUNNER="${ROOT}/ucm/store/dram/scripts/run_dramstore_sim.sh"

mkdir -p "${OUT_DIR}"
: >"${POOL_LOG}"
printf 'iteration\tstart\tend\tstatus\tcase\tpool_rss_kb\tpool_threads\tpool_fds\tcritical_lines\tlog\n' >"${ITERATIONS}"

phase_start_text="$(date '+%F %T %z')"
phase_start_epoch="$(date +%s)"
deadline=$((phase_start_epoch + DURATION_SECONDS))
pool_pid=""
pool_shutdown_forced=0

cleanup()
{
    if [[ -n "${pool_pid}" ]] && kill -0 "${pool_pid}" 2>/dev/null; then
        kill -TERM "${pool_pid}" 2>/dev/null || true
        for _ in $(seq 1 100); do
            kill -0 "${pool_pid}" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "${pool_pid}" 2>/dev/null; then
            pool_shutdown_forced=1
            kill -KILL "${pool_pid}" 2>/dev/null || true
        fi
        wait "${pool_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

source /usr/local/Ascend/cann-8.5.1/set_env.sh
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-8.5.1
export ASCEND_TOOLKIT_HOME=/usr/local/Ascend/cann-8.5.1
export HIXL_HOME=/usr/local/Ascend/cann-8.5.1/aarch64-linux
export DRAMSTORE_SIM_BIN="${SIM_BIN}"

python3 - <<PY
import socket

host = "110.138.0.3"
endpoints = [
    "${POOL_CONTROL}",
    "${STORE_CONTROL}",
    "${POOL_ONE_SIDED}",
    "${STORE_ONE_SIDED}",
]
ports = [int(endpoint.rsplit(":", 1)[1]) for endpoint in endpoints]
sockets = []
try:
    for port in ports:
        sock = socket.socket()
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((host, port))
        sockets.append(sock)
finally:
    for sock in sockets:
        sock.close()
PY
if [[ $? -ne 0 ]]; then
    printf 'phase_start=%s\npreflight=port_bind_failed\n' "${phase_start_text}" >"${SUMMARY}"
    exit 2
fi

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
  dir: "${OUT_DIR}/phase1-runtime-logs"
  max_files: 20
  max_size_mb: 10
EOF

"${POOL_BIN}" \
    --addr "${POOL_CONTROL}" \
    --nics mlx5_0 \
    --pool-size-gb 1 \
    --kvcache-block-sizes "${POOL_BLOCK_SIZES[@]}" \
    --kvcache-block-proportions "${POOL_BLOCK_PROPORTIONS[@]}" \
    --ttl-minutes 120 \
    --config "${CONFIG}" >"${POOL_LOG}" 2>&1 &
pool_pid=$!

ready=0
for _ in $(seq 1 300); do
    if grep -q 'DramPool service ready' "${POOL_LOG}"; then
        ready=1
        break
    fi
    kill -0 "${pool_pid}" 2>/dev/null || break
    sleep 0.1
done
if [[ "${ready}" -ne 1 ]]; then
    printf 'phase_start=%s\npool_ready=0\npool_pid=%s\n' \
        "${phase_start_text}" "${pool_pid}" >"${SUMMARY}"
    exit 3
fi

initial_rss_kb="$(awk '/VmRSS:/ {print $2}' "/proc/${pool_pid}/status")"
iteration=0
passes=0
unexpected_failures=0
total_critical=0
max_rss_kb="${initial_rss_kb:-0}"
max_fds=0
max_threads=0

while [[ "${iteration}" -eq 0 || "$(date +%s)" -lt "${deadline}" ]]; do
    iteration=$((iteration + 1))
    case_id=$(((iteration - 1 + CASE_OFFSET) % 6))
    case "${case_id}" in
        0)
            case_name=baseline_4096
            args=(--block-size 4096 --block-num 1 --rounds 1
                  --put 1000 --get 1000 --lookup-exist 20 --lookup-miss 20)
            ;;
        1)
            case_name=mixed_4block
            args=(--block-size 4096 --block-num 4 --rounds 2
                  --put 300 --get 300 --lookup-exist 100 --lookup-miss 100)
            ;;
        2)
            case_name=tiny_16block
            args=(--block-size 64 --block-num 16 --rounds 2
                  --put 300 --get 300 --lookup-exist 100 --lookup-miss 100)
            ;;
        3)
            case_name=large_4block
            args=(--block-size 65536 --block-num 4 --rounds 2
                  --put 50 --get 50 --lookup-exist 20 --lookup-miss 20)
            ;;
        4)
            case_name=round_reuse
            args=(--block-size 4096 --block-num 2 --rounds 5
                  --put 200 --get 200 --lookup-exist 40 --lookup-miss 40)
            ;;
        *)
            case_name=lookup_heavy
            args=(--block-size 4096 --block-num 1 --rounds 3
                  --put 100 --get 100 --lookup-exist 1000 --lookup-miss 1000)
            ;;
    esac

    iter_start="$(date '+%F %T')"
    iter_log="${OUT_DIR}/phase1-iter-$(printf '%04d' "${iteration}")-${case_name}.log"
    timeout 180 bash "${RUNNER}" \
        --config "${CONFIG}" \
        --pool-control "${POOL_CONTROL}" \
        --store-control "${STORE_CONTROL}" \
        --devices 5 \
        --key-seed "$((202607280000 + iteration))" \
        "${args[@]}" >"${iter_log}" 2>&1
    status=$?
    iter_end="$(date '+%F %T')"

    critical_lines="$(grep -Eic \
        'response validation failed|GetStatus failed|TransportManager::Shutdown failed|AddressSanitizer|runtime error:|server error|Segmentation fault|double free|use-after-free' \
        "${iter_log}" || true)"
    total_critical=$((total_critical + critical_lines))
    if [[ "${status}" -eq 0 ]] &&
        grep -q 'dramstore simulation passed' "${iter_log}" &&
        [[ "${critical_lines}" -eq 0 ]]; then
        passes=$((passes + 1))
    else
        unexpected_failures=$((unexpected_failures + 1))
    fi

    if kill -0 "${pool_pid}" 2>/dev/null; then
        rss_kb="$(awk '/VmRSS:/ {print $2}' "/proc/${pool_pid}/status")"
        threads="$(awk '/Threads:/ {print $2}' "/proc/${pool_pid}/status")"
        fds="$(find "/proc/${pool_pid}/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
    else
        rss_kb=0
        threads=0
        fds=0
        status=125
        unexpected_failures=$((unexpected_failures + 1))
    fi
    ((rss_kb > max_rss_kb)) && max_rss_kb="${rss_kb}"
    ((threads > max_threads)) && max_threads="${threads}"
    ((fds > max_fds)) && max_fds="${fds}"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${iteration}" "${iter_start}" "${iter_end}" "${status}" "${case_name}" \
        "${rss_kb}" "${threads}" "${fds}" "${critical_lines}" "${iter_log}" \
        >>"${ITERATIONS}"

    if [[ "${unexpected_failures}" -gt 0 ]]; then
        break
    fi
    if [[ "${MAX_ITERATIONS}" -gt 0 && "${iteration}" -ge "${MAX_ITERATIONS}" ]]; then
        break
    fi
done

phase_end_text="$(date '+%F %T %z')"
final_rss_kb=0
final_threads=0
final_fds=0
if kill -0 "${pool_pid}" 2>/dev/null; then
    final_rss_kb="$(awk '/VmRSS:/ {print $2}' "/proc/${pool_pid}/status")"
    final_threads="$(awk '/Threads:/ {print $2}' "/proc/${pool_pid}/status")"
    final_fds="$(find "/proc/${pool_pid}/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
fi
pool_critical_lines="$(grep -Eic \
    'GetStatus failed|server error|Segmentation fault|double free|use-after-free|AddressSanitizer|runtime error:' \
    "${POOL_LOG}" || true)"

cat >"${SUMMARY}" <<EOF
phase=phase1_mixed
phase_start=${phase_start_text}
phase_end=${phase_end_text}
requested_duration_seconds=${DURATION_SECONDS}
actual_duration_seconds=$(( $(date +%s) - phase_start_epoch ))
iterations=${iteration}
passes=${passes}
unexpected_failures=${unexpected_failures}
sim_critical_lines=${total_critical}
pool_critical_lines=${pool_critical_lines}
pool_pid=${pool_pid}
initial_rss_kb=${initial_rss_kb}
max_rss_kb=${max_rss_kb}
final_rss_kb=${final_rss_kb}
max_threads=${max_threads}
final_threads=${final_threads}
max_fds=${max_fds}
final_fds=${final_fds}
pool_shutdown_forced=${pool_shutdown_forced}
pool_log=${POOL_LOG}
iterations_file=${ITERATIONS}
EOF

if [[ "${unexpected_failures}" -ne 0 || "${pool_critical_lines}" -ne 0 ]]; then
    exit 1
fi
exit 0
