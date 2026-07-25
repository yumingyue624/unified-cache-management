#!/usr/bin/env bash

set -euo pipefail

DRAM_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRAMSTORE_SIM_BIN="${DRAMSTORE_SIM_BIN:-${DRAM_ROOT}/build/dramstore_sim}"

usage()
{
    cat >&2 <<EOF
Usage: $0 --config PATH --pool-control IP:PORT --store-control IP:PORT \
--devices ID[,ID...] [--store-index-base N] [--key-seed N] [dramstore_sim options]

Starts one dramstore_sim process per device. The first process uses the given
--store-control port, and each following process uses the next port.

Example:
  $0 --config /tmp/drampool_e2e.yaml \
    --pool-control 110.3.233.61:9000 \
    --store-control 110.3.233.51:9001 \
    --devices 2,3 \
    --store-index-base 0 \
    --block-size 4096 --block-num 1 --rounds 10 \
    --put 4 --get 4 --lookup-exist 4 --lookup-miss 4
EOF
}

if [[ ! -x "${DRAMSTORE_SIM_BIN}" ]]; then
    echo "dramstore_sim is not executable: ${DRAMSTORE_SIM_BIN}" >&2
    exit 1
fi

devices_value=""
store_control=""
store_index_base=0
key_seed=""
common_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --devices)
            [[ $# -ge 2 ]] || { echo "missing value for --devices" >&2; usage; exit 2; }
            devices_value="$2"
            shift 2
            ;;
        --store-control)
            [[ $# -ge 2 ]] ||
                { echo "missing value for --store-control" >&2; usage; exit 2; }
            store_control="$2"
            shift 2
            ;;
        --store-index-base)
            [[ $# -ge 2 ]] ||
                { echo "missing value for --store-index-base" >&2; usage; exit 2; }
            store_index_base="$2"
            shift 2
            ;;
        --key-seed)
            [[ $# -ge 2 ]] ||
                { echo "missing value for --key-seed" >&2; usage; exit 2; }
            key_seed="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            [[ $# -ge 2 ]] || { echo "missing value for $1" >&2; usage; exit 2; }
            common_args+=("$1" "$2")
            shift 2
            ;;
    esac
done

if [[ -z "${devices_value}" || -z "${store_control}" ]]; then
    echo "--devices and --store-control are required" >&2
    usage
    exit 2
fi
if [[ ! "${store_index_base}" =~ ^[0-9]+$ ]]; then
    echo "invalid --store-index-base value: ${store_index_base}" >&2
    exit 2
fi
if [[ -z "${key_seed}" ]]; then
    key_seed="$(date +%s%N 2>/dev/null || date +%s)"
    key_seed="${key_seed:0:19}"
fi
if [[ ! "${key_seed}" =~ ^[0-9]+$ ]]; then
    echo "invalid --key-seed value: ${key_seed}" >&2
    exit 2
fi
echo "DramStore simulation key_seed=${key_seed}"

if [[ "${store_control}" =~ ^(\[[^]]+\]|[^:]+):([0-9]+)$ ]]; then
    store_host="${BASH_REMATCH[1]}"
    first_store_port="${BASH_REMATCH[2]}"
else
    echo "invalid --store-control value: ${store_control}" >&2
    exit 2
fi

if ((first_store_port < 1 || first_store_port > 65535)); then
    echo "invalid --store-control port: ${first_store_port}" >&2
    exit 2
fi

IFS=',' read -r -a devices <<< "${devices_value}"
if [[ ${#devices[@]} -eq 0 ]]; then
    echo "--devices must not be empty" >&2
    exit 2
fi

pids=()
cleanup()
{
    local pid
    trap - INT TERM
    for pid in "${pids[@]}"; do
        kill -TERM "${pid}" 2>/dev/null || true
    done
    for pid in "${pids[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
}
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

for index in "${!devices[@]}"; do
    device="${devices[index]}"
    if [[ ! "${device}" =~ ^[0-9]+$ ]]; then
        echo "invalid device id: ${device}" >&2
        cleanup
        exit 2
    fi

    store_port=$((first_store_port + index))
    if ((store_port > 65535)); then
        echo "store control port range exceeds 65535" >&2
        cleanup
        exit 2
    fi
    endpoint="${store_host}:${store_port}"
    store_index=$((store_index_base + index))

    echo "Starting DramStore process: store=${store_index} device=${device} control=${endpoint}"
    "${DRAMSTORE_SIM_BIN}" \
        "${common_args[@]}" \
        --store-control "${endpoint}" \
        --store-index "${store_index}" \
        --key-seed "${key_seed}" \
        --devices "${device}" &
    pids+=("$!")
done

result=0
for pid in "${pids[@]}"; do
    if wait "${pid}"; then
        continue
    else
        child_status=$?
        if ((result == 0)); then
            result="${child_status}"
        fi
        echo "DramStore process failed: pid=${pid} status=${child_status}" >&2
    fi
done

trap - INT TERM
exit "${result}"
