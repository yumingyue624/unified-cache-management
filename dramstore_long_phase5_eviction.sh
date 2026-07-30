#!/usr/bin/env bash
set -uo pipefail

ROOT=/home/codex/ucm-dramstore-sim-codex-20260728
OUT_DIR="${1:-/tmp/dramstore-long-soak-20260728/phase5-eviction}"
DURATION_SECONDS="${DURATION_SECONDS:-2700}"
MAX_ITERATIONS="${MAX_ITERATIONS:-0}"
MEDIUM_ONLY="${MEDIUM_ONLY:-0}"
read -r -a POOL_PROPORTIONS <<<"${POOL_PROPORTIONS:-1 1}"
A2_INTER_ITERATION_GAP_SECONDS="${A2_INTER_ITERATION_GAP_SECONDS:-0}"
POOL_CONTROL=110.138.0.3:49400
STORE_CONTROL=110.138.0.3:49401
POOL_ONE_SIDED=110.138.0.3:49500
STORE_ONE_SIDED=110.138.0.3:49501
POOL_BIN="${POOL_BIN:-${ROOT}/build-dramstore-sim-cann851/ucm/store/dram/drampool}"
SIM_BIN="${SIM_BIN:-${ROOT}/ucm/store/dram/build/dramstore_sim}"
RUNNER="${ROOT}/ucm/store/dram/scripts/run_dramstore_sim.sh"
CONFIG="${OUT_DIR}/phase5.yaml"
POOL_LOG="${OUT_DIR}/phase5-pool.log"
RESULTS="${OUT_DIR}/phase5-iterations.tsv"
SUMMARY="${OUT_DIR}/phase5-summary.txt"

mkdir -p "${OUT_DIR}"
: >"${POOL_LOG}"
printf 'iteration\tstart\tend\tcase\tstatus\tcritical_lines\tpool_errors_total\trss_kb\tthreads\tfds\tsim_log\n' >"${RESULTS}"

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
  lease_time_ms: 10000
  default_evict_ratio: 0.2
  evict_period_ms: 31536000000
operation:
  timeout_ms: 10000
logger:
  level: info
  dir: "${OUT_DIR}/runtime-logs"
  max_files: 50
  max_size_mb: 20
EOF

phase_start_text="$(date '+%F %T %z')"
phase_start_epoch="$(date +%s)"
deadline=$((phase_start_epoch + DURATION_SECONDS))
pool_pid=""
forced_cleanup=0

cleanup()
{
    if [[ -n "${pool_pid}" ]] && kill -0 "${pool_pid}" 2>/dev/null; then
        kill -TERM "${pool_pid}" 2>/dev/null || true
        for _ in $(seq 1 300); do
            kill -0 "${pool_pid}" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "${pool_pid}" 2>/dev/null; then
            forced_cleanup=1
            kill -KILL "${pool_pid}" 2>/dev/null || true
        fi
        wait "${pool_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

"${POOL_BIN}" \
    --addr "${POOL_CONTROL}" \
    --nics mlx5_0 \
    --pool-size-gb 1 \
    --kvcache-block-sizes 4096 65536 \
    --kvcache-block-proportions "${POOL_PROPORTIONS[@]}" \
    --ttl-minutes 120 \
    --config "${CONFIG}" >"${POOL_LOG}" 2>&1 &
pool_pid=$!

ready=0
for _ in $(seq 1 300); do
    if grep -q 'DramPool service ready' "${POOL_LOG}"; then ready=1; break; fi
    kill -0 "${pool_pid}" 2>/dev/null || break
    sleep 0.1
done
if [[ "${ready}" -ne 1 ]]; then
    printf 'phase=phase5_eviction\npool_ready=0\n' >"${SUMMARY}"
    exit 2
fi

initial_rss_kb="$(awk '/VmRSS:/ {print $2}' "/proc/${pool_pid}/status")"
max_rss_kb="${initial_rss_kb}"
max_threads=0
max_fds=0
iteration=0
passes=0
unexpected_failures=0

while [[ "${iteration}" -eq 0 || "$(date +%s)" -lt "${deadline}" ]]; do
    iteration=$((iteration + 1))
    if [[ "${MEDIUM_ONLY}" -eq 1 ]]; then
        case $(((iteration - 1) % 3)) in
            0)
                case_name=medium_put_pressure
                args=(--block-size 4096 --block-num 4 --rounds 1
                      --put 1000 --get 0 --lookup-exist 0 --lookup-miss 0)
                ;;
            1)
                case_name=medium_mixed_leased
                args=(--block-size 4096 --block-num 4 --rounds 2
                      --put 300 --get 300 --lookup-exist 100 --lookup-miss 100)
                ;;
            *)
                case_name=lookup_heavy
                args=(--block-size 4096 --block-num 1 --rounds 3
                      --put 100 --get 100 --lookup-exist 1000 --lookup-miss 1000)
                ;;
        esac
    else
      case $(((iteration - 1) % 5)) in
        0)
            case_name=large_put_pressure
            args=(--block-size 65536 --block-num 4 --rounds 1
                  --put 300 --get 0 --lookup-exist 0 --lookup-miss 0)
            ;;
        1)
            case_name=large_mixed_leased
            args=(--block-size 65536 --block-num 4 --rounds 2
                  --put 50 --get 50 --lookup-exist 20 --lookup-miss 20)
            ;;
        2)
            case_name=medium_put_pressure
            args=(--block-size 4096 --block-num 4 --rounds 1
                  --put 1000 --get 0 --lookup-exist 0 --lookup-miss 0)
            ;;
        3)
            case_name=medium_mixed_leased
            args=(--block-size 4096 --block-num 4 --rounds 2
                  --put 300 --get 300 --lookup-exist 100 --lookup-miss 100)
            ;;
        *)
            case_name=lookup_heavy
            args=(--block-size 4096 --block-num 1 --rounds 3
                  --put 100 --get 100 --lookup-exist 1000 --lookup-miss 1000)
            ;;
      esac
    fi

    iter_start="$(date '+%F %T')"
    sim_log="${OUT_DIR}/phase5-sim-$(printf '%04d' "${iteration}")-${case_name}.log"
    timeout 180 bash "${RUNNER}" \
        --config "${CONFIG}" \
        --pool-control "${POOL_CONTROL}" \
        --store-control "${STORE_CONTROL}" \
        --devices 5 \
        --key-seed "$((202608010000 + iteration))" \
        --eviction-aware 1 \
        "${args[@]}" >"${sim_log}" 2>&1
    status=$?
    iter_end="$(date '+%F %T')"
    sleep 0.1
    critical_lines="$(grep -Eic \
        'timed out|response validation failed|GetStatus failed|TransportManager::Shutdown failed|Segmentation fault|double free|use-after-free' \
        "${sim_log}" || true)"
    pool_errors_total="$(grep -Eic \
        '\\[UC\\]\\[E\\]|StoreBegin.*failed|no free slots|server error|Segmentation fault' \
        "${POOL_LOG}" || true)"

    if kill -0 "${pool_pid}" 2>/dev/null; then
        rss_kb="$(awk '/VmRSS:/ {print $2}' "/proc/${pool_pid}/status")"
        threads="$(awk '/Threads:/ {print $2}' "/proc/${pool_pid}/status")"
        fds="$(find "/proc/${pool_pid}/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
    else
        rss_kb=0
        threads=0
        fds=0
        status=125
    fi
    ((rss_kb > max_rss_kb)) && max_rss_kb="${rss_kb}"
    ((threads > max_threads)) && max_threads="${threads}"
    ((fds > max_fds)) && max_fds="${fds}"

    if [[ "${status}" -eq 0 && "${critical_lines}" -eq 0 &&
          "${pool_errors_total}" -eq 0 ]] &&
        grep -q '^dramstore simulation passed$' "${sim_log}"; then
        passes=$((passes + 1))
    else
        unexpected_failures=$((unexpected_failures + 1))
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${iteration}" "${iter_start}" "${iter_end}" "${case_name}" "${status}" \
        "${critical_lines}" "${pool_errors_total}" "${rss_kb}" "${threads}" "${fds}" \
        "${sim_log}" >>"${RESULTS}"
    if [[ "${unexpected_failures}" -gt 0 ]]; then break; fi
    if [[ "${MAX_ITERATIONS}" -gt 0 && "${iteration}" -ge "${MAX_ITERATIONS}" ]]; then break; fi
    if [[ "${A2_INTER_ITERATION_GAP_SECONDS}" != "0" ]]; then
        sleep "${A2_INTER_ITERATION_GAP_SECONDS}"
    fi
done

final_rss_kb=0
final_threads=0
final_fds=0
if kill -0 "${pool_pid}" 2>/dev/null; then
    final_rss_kb="$(awk '/VmRSS:/ {print $2}' "/proc/${pool_pid}/status")"
    final_threads="$(awk '/Threads:/ {print $2}' "/proc/${pool_pid}/status")"
    final_fds="$(find "/proc/${pool_pid}/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
fi

cat >"${SUMMARY}" <<EOF
phase=phase5_eviction
phase_start=${phase_start_text}
phase_end=$(date '+%F %T %z')
requested_duration_seconds=${DURATION_SECONDS}
actual_duration_seconds=$(( $(date +%s) - phase_start_epoch ))
iterations=${iteration}
passes=${passes}
unexpected_failures=${unexpected_failures}
initial_rss_kb=${initial_rss_kb}
max_rss_kb=${max_rss_kb}
final_rss_kb=${final_rss_kb}
max_threads=${max_threads}
final_threads=${final_threads}
max_fds=${max_fds}
final_fds=${final_fds}
forced_cleanup=${forced_cleanup}
results=${RESULTS}
pool_log=${POOL_LOG}
EOF

if [[ "${unexpected_failures}" -gt 0 ]]; then exit 1; fi
exit 0
