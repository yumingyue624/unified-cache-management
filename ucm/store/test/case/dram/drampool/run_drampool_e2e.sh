#!/usr/bin/env bash
set -euo pipefail

if [[ $# -gt 1 ]]; then
    echo "usage: $0 [drampool_e2e_binary]" >&2
    exit 2
fi

e2e_binary="${1:-${DRAMPOOL_E2E_BIN:-./build/ucm/transport/kv/dramstore/drampool_e2e}}"
server_device="${DRAMPOOL_E2E_SERVER_DEVICE:-0}"
client_device="${DRAMPOOL_E2E_CLIENT_DEVICE:-1}"
server_control="${DRAMPOOL_E2E_SERVER_CONTROL:-127.0.0.1:19000}"
client_control="${DRAMPOOL_E2E_CLIENT_CONTROL:-127.0.0.1:19001}"
server_one_sided="${DRAMPOOL_E2E_SERVER_ONE_SIDED:-127.0.0.1:19100}"
client_one_sided="${DRAMPOOL_E2E_CLIENT_ONE_SIDED:-127.0.0.1:19101}"

if [[ ! -x "${e2e_binary}" ]]; then
    echo "E2E binary is not executable: ${e2e_binary}" >&2
    exit 2
fi

log_dir="$(mktemp -d -t drampool-e2e.XXXXXX)"
server_log="${log_dir}/server.log"
client_log="${log_dir}/client.log"
server_pid=""

cleanup() {
    if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
        kill -TERM "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

"${e2e_binary}" server "${server_control}" "${client_control}" \
    "${server_one_sided}" "${client_one_sided}" "${server_device}" \
    >"${server_log}" 2>&1 &
server_pid=$!

ready=0
for _ in $(seq 1 300); do
    if grep -q "DRAMPOOL_E2E_SERVER_READY" "${server_log}"; then
        ready=1
        break
    fi
    if ! kill -0 "${server_pid}" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

if [[ "${ready}" -ne 1 ]]; then
    echo "DramPool E2E server did not become ready; log: ${server_log}" >&2
    cat "${server_log}" >&2
    exit 1
fi

if ! timeout 60s "${e2e_binary}" client "${server_control}" "${client_control}" \
    "${server_one_sided}" "${client_one_sided}" "${client_device}" \
    >"${client_log}" 2>&1; then
    echo "DramPool E2E client failed; logs: ${log_dir}" >&2
    cat "${client_log}" >&2
    cat "${server_log}" >&2
    exit 1
fi

if ! grep -q "DRAMPOOL_E2E_PASS" "${client_log}"; then
    echo "DramPool E2E did not report success; logs: ${log_dir}" >&2
    cat "${client_log}" >&2
    cat "${server_log}" >&2
    exit 1
fi

kill -TERM "${server_pid}"
wait "${server_pid}"
server_pid=""
echo "DramPool E2E passed (server device ${server_device}, client device ${client_device})."
