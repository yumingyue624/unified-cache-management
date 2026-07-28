#!/usr/bin/env bash
set -euo pipefail

DRAM_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UCM_ROOT="$(cd "${DRAM_ROOT}/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${DRAM_ROOT}/build}"
CXX="${CXX:-g++}"
PYTHON="${PYTHON:-python3}"

find_p2p_root()
{
    local candidate
    local candidates=()
    local selected=""

    shopt -s nullglob
    candidates+=("${UCM_ROOT}"/build/lib.*/ucm/transport/p2p)
    shopt -u nullglob

    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}/libucm_p2p_transport.so" ]] &&
            { [[ -z "${selected}" ]] ||
                [[ "${candidate}/libucm_p2p_transport.so" -nt \
                    "${selected}/libucm_p2p_transport.so" ]]; }; then
            selected="${candidate}"
        fi
    done
    if [[ -n "${selected}" ]]; then
        printf '%s\n' "${selected}"
        return 0
    fi

    candidates=()
    shopt -s nullglob
    candidates+=("${UCM_ROOT}"/build/temp.*/ucm/transport/p2p)
    shopt -u nullglob
    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}/libucm_p2p_transport.so" ]] &&
            { [[ -z "${selected}" ]] ||
                [[ "${candidate}/libucm_p2p_transport.so" -nt \
                    "${selected}/libucm_p2p_transport.so" ]]; }; then
            selected="${candidate}"
        fi
    done
    if [[ -n "${selected}" ]]; then
        printf '%s\n' "${selected}"
        return 0
    fi

    "${PYTHON}" - <<'PY'
from importlib.metadata import distributions
from pathlib import Path

for package in distributions(name="uc-manager"):
    root = Path(package.locate_file("ucm/transport/p2p")).resolve()
    if (root / "libucm_p2p_transport.so").is_file():
        print(root)
        break
PY
}

if [[ -z "${UCM_P2P_ROOT:-}" ]]; then
    UCM_P2P_ROOT="$(find_p2p_root)"
fi

P2P_LIBRARY="${UCM_P2P_ROOT}/libucm_p2p_transport.so"
if [[ ! -f "${P2P_LIBRARY}" ]]; then
    cat >&2 <<EOF
Cannot find libucm_p2p_transport.so.

Checked the configured path:
  ${P2P_LIBRARY}

Also searched:
  ${UCM_ROOT}/build/lib.*/ucm/transport/p2p
  ${UCM_ROOT}/build/temp.*/ucm/transport/p2p
  installed uc-manager distributions

Set UCM_P2P_ROOT to the directory containing libucm_p2p_transport.so.
EOF
    exit 1
fi

P2P_INCLUDE_ROOT="${UCM_P2P_ROOT}"
if [[ ! -f "${P2P_INCLUDE_ROOT}/include/core/transport.h" ]]; then
    P2P_INCLUDE_ROOT="${UCM_ROOT}/ucm/transport/p2p"
fi

ASCEND_ROOT="${ASCEND_ROOT:-${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}}"
ASCEND_INCLUDE="${ASCEND_INCLUDE_DIR:-${ASCEND_ROOT}/include}"
ASCEND_LIB="${ASCEND_LIB_DIR:-${ASCEND_ROOT}/lib64}"
if [[ -d "${ASCEND_ROOT}/aarch64-linux/include" ]]; then
    ASCEND_INCLUDE="${ASCEND_ROOT}/aarch64-linux/include"
fi
if [[ -d "${ASCEND_ROOT}/aarch64-linux/lib64" ]]; then
    ASCEND_LIB="${ASCEND_ROOT}/aarch64-linux/lib64"
fi
HIXL_LIB="${HIXL_LIB_DIR:-${ASCEND_LIB}}"

if [[ ! -f "${HIXL_LIB}/libcann_hixl.so" ]]; then
    echo "Cannot find libcann_hixl.so under HIXL_LIB_DIR=${HIXL_LIB}" >&2
    exit 1
fi

mkdir -p "${BUILD_DIR}"
OUTPUT="${BUILD_DIR}/dramstore_sim"
TEMP_OUTPUT="${BUILD_DIR}/.dramstore_sim.$$"
rm -f "${OUTPUT}" "${TEMP_OUTPUT}"
trap 'rm -f "${TEMP_OUTPUT}"' EXIT

echo "Using P2P library: ${P2P_LIBRARY}"
echo "Using P2P headers: ${P2P_INCLUDE_ROOT}/include"
echo "Using HIXL library: ${HIXL_LIB}/libcann_hixl.so"
"${CXX}" -std=c++17 -Wall -Wextra -Wpedantic ${CXXFLAGS:-} \
    -I"${P2P_INCLUDE_ROOT}/include" \
    -I"${UCM_ROOT}/ucm/shared/infra" \
    -I"${UCM_ROOT}/ucm/store" \
    -I"${UCM_ROOT}/ucm/store/detail" \
    -I"${DRAM_ROOT}/cc" \
    -I"${ASCEND_INCLUDE}" \
    "${DRAM_ROOT}/tests/dramstore_sim.cpp" \
    "${DRAM_ROOT}/cc/kv_protocol.cc" \
    "${DRAM_ROOT}/cc/drampool/drampool_launch_config.cc" \
    "${DRAM_ROOT}/cc/drampool/drampool_yaml_config.cc" \
    -L"${UCM_P2P_ROOT}" -lucm_p2p_transport \
    -L"${HIXL_LIB}" -lcann_hixl \
    -L"${ASCEND_ROOT}" -L"${ASCEND_LIB}" -lascendcl -lmetadef \
    -lfmt -lspdlog -lz -lrt -pthread \
    -Wl,-rpath,"${UCM_P2P_ROOT}" -Wl,-rpath,"${HIXL_LIB}" \
    -Wl,-rpath,"${ASCEND_LIB}" \
    ${LDFLAGS:-} -o "${TEMP_OUTPUT}"

mv -f "${TEMP_OUTPUT}" "${OUTPUT}"
trap - EXIT
echo "Built ${OUTPUT}"
