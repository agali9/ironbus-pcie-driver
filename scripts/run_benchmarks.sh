#!/usr/bin/env bash
# Run edu_pci benchmarks and save results.
# Must execute on Linux inside a VM with QEMU -device edu and edu_pci.ko loaded.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_DIR="${REPO_ROOT}/kernel"
USER_DIR="${REPO_ROOT}/userspace"
RESULTS_DIR="${REPO_ROOT}/results"
DEVICE="${DEVICE:-/dev/edu_pci}"
OUTPUT="${OUTPUT:-${RESULTS_DIR}/benchmark_results.txt}"

die() { echo "ERROR: $*" >&2; exit 1; }

require_root() {
    if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
        die "Run as root (sudo) — need access to ${DEVICE} and insmod"
    fi
}

check_edu_pci() {
    if ! lspci -nn 2>/dev/null | grep -q '1234:11e8'; then
        die "EDU PCI device (1234:11e8) not visible — boot QEMU with: -device edu"
    fi
}

build_all() {
    echo "=== Building kernel module ==="
    make -C "${KERNEL_DIR}" clean
    make -C "${KERNEL_DIR}" -j"$(nproc)"

    echo "=== Building userspace ==="
    make -C "${USER_DIR}" clean
    make -C "${USER_DIR}" -j"$(nproc)"
}

load_driver() {
    echo "=== Loading edu_pci.ko ==="
    rmmod edu_pci 2>/dev/null || true
    insmod "${KERNEL_DIR}/edu_pci.ko"
    [[ -c "${DEVICE}" ]] || die "${DEVICE} not created after insmod"
    dmesg | tail -5
}

run_python_benchmark() {
    mkdir -p "${RESULTS_DIR}"
    echo "=== Running python/benchmark.py ==="
    cd "${USER_DIR}"
    PYTHONPATH=. python3 python/benchmark.py \
        --device "${DEVICE}" \
        --output "${OUTPUT}"
}

run_demo_throughput() {
    echo "=== Running demo (includes throughput section) ==="
    cd "${USER_DIR}"
    LD_LIBRARY_PATH=. ./demo 2>&1 | tee "${RESULTS_DIR}/demo_output.txt"
}

collect_debugfs() {
    if [[ -r /sys/kernel/debug/edu_pci/stats ]]; then
        cat /sys/kernel/debug/edu_pci/stats > "${RESULTS_DIR}/debugfs_stats.txt"
        echo "debugfs stats -> ${RESULTS_DIR}/debugfs_stats.txt"
    fi
}

main() {
    require_root
    check_edu_pci
    build_all
    load_driver
    run_python_benchmark
    run_demo_throughput
    collect_debugfs
    echo ""
    echo "=== Done ==="
    echo "Benchmark results: ${OUTPUT}"
    echo "Demo output:       ${RESULTS_DIR}/demo_output.txt"
}

main "$@"
