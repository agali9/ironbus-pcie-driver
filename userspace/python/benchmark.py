#!/usr/bin/env python3
# python/benchmark.py — EduDevice latency + throughput + zero-copy benchmark
#
# Measures:
#   - Latency percentiles (QD=1) for ioctl submit
#   - Real software-pipeline QD sweep: 1, 4, 16, 64
#   - ioctl submit vs true doorbell/mmap zero-copy submit
#   - Userspace CPU time via time.process_time()
#
# EDU hardware is single-command; the driver serializes MMIO kicks.
# QD = number of outstanding SQ entries (software pipeline depth).
#
# Run (inside VM as root):
#   cd ~/pcie_prototype_async/userspace
#   sudo PYTHONPATH=. python3 python/benchmark.py

import os
import sys
import time
import statistics
import argparse
import errno

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from python.edu_device import EduDevice

# -------------------------------------------------------
# Config
# -------------------------------------------------------

DEVICE_PATH   = "/dev/edu_pci"
WARMUP_OPS    = 32
MEASURE_OPS   = 256
LATENCY_OPS   = 512
TIMEOUT_MS    = 5000
# Ring holds 64 entries; ring_full leaves one empty → max outstanding 63.
# QD=64 exercises fill-until-ENOSPC then reclaim.
QUEUE_DEPTHS  = [1, 4, 16, 64]
RING_MAX_OUTSTANDING = 63


def now_us() -> float:
    return time.perf_counter() * 1e6


def pct(samples: list, p: float) -> float:
    if not samples:
        return 0.0
    idx = min(len(samples) - 1, int(len(samples) * p))
    return samples[idx]


def latency_stats(samples: list) -> dict:
    s = sorted(samples)
    return {
        "min_us":    s[0],
        "mean_us":   statistics.mean(s),
        "p50_us":    statistics.median(s),
        "p95_us":    pct(s, 0.95),
        "p99_us":    pct(s, 0.99),
        "max_us":    s[-1],
        "stddev_us": statistics.stdev(s) if len(s) > 1 else 0.0,
        "ops":       len(s),
        "ops_per_sec": 1e6 / statistics.mean(s) if statistics.mean(s) > 0 else 0.0,
    }


def drain(dev: EduDevice, count: int):
    for i in range(count):
        c = dev.wait_completion(TIMEOUT_MS)
        if c is None:
            raise RuntimeError(f"Timeout draining completion {i}/{count}")
        if not c.ok():
            raise RuntimeError(f"Completion error: tag={c.tag} status={c.status}")


def submit_one(dev: EduDevice, tag: int, use_zc: bool):
    n = (tag % 12) + 1
    t = tag % 65535
    if use_zc:
        dev.submit_factorial_zc(t, n)
    else:
        dev.submit_factorial(t, n)


def run_pipeline(dev: EduDevice, qd: int, total_ops: int, use_zc: bool = False,
                 collect_latency: bool = False):
    """
    Keep up to `qd` outstanding commands on the SQ/CQ.
    Returns (wall_us, cpu_s, latency_samples).
    """
    effective_qd = min(qd, RING_MAX_OUTSTANDING)
    submit_ts = {}
    samples = []
    submitted = 0
    completed = 0
    outstanding = 0
    tag = 0

    wall0 = now_us()
    cpu0 = time.process_time()

    while completed < total_ops:
        while outstanding < effective_qd and submitted < total_ops:
            try:
                if collect_latency:
                    submit_ts[tag % 65535] = now_us()
                submit_one(dev, tag, use_zc)
                submitted += 1
                outstanding += 1
                tag += 1
            except OSError as e:
                # Ring full at high QD — reclaim then continue
                if e.errno == errno.ENOSPC and outstanding > 0:
                    break
                raise

        c = dev.wait_completion(TIMEOUT_MS)
        if c is None:
            raise RuntimeError(
                f"Timeout waiting completion "
                f"(completed={completed}/{total_ops}, outstanding={outstanding})"
            )
        if not c.ok():
            raise RuntimeError(f"Completion error: tag={c.tag} status={c.status}")

        if collect_latency and c.tag in submit_ts:
            samples.append(now_us() - submit_ts.pop(c.tag))
        outstanding -= 1
        completed += 1

    wall_us = now_us() - wall0
    cpu_s = time.process_time() - cpu0
    return wall_us, cpu_s, samples


def measure_latency_qd1(dev: EduDevice, use_zc: bool = False) -> dict:
    for i in range(WARMUP_OPS):
        submit_one(dev, i, use_zc)
        drain(dev, 1)

    samples = []
    cpu0 = time.process_time()
    for i in range(LATENCY_OPS):
        t0 = now_us()
        submit_one(dev, i, use_zc)
        drain(dev, 1)
        samples.append(now_us() - t0)
    cpu_s = time.process_time() - cpu0

    stats = latency_stats(samples)
    stats["cpu_s"] = cpu_s
    stats["cpu_pct"] = (cpu_s / (sum(samples) / 1e6)) * 100.0 if samples else 0.0
    return stats


def measure_qd_sweep(dev: EduDevice, use_zc: bool = False) -> list:
    results = []
    for qd in QUEUE_DEPTHS:
        run_pipeline(dev, qd, WARMUP_OPS, use_zc=use_zc, collect_latency=False)
        wall_us, cpu_s, samples = run_pipeline(
            dev, qd, MEASURE_OPS, use_zc=use_zc, collect_latency=True
        )
        ops_per_sec = MEASURE_OPS / (wall_us / 1e6)
        stats = latency_stats(samples) if samples else {
            "mean_us": wall_us / MEASURE_OPS,
            "p50_us": 0, "p95_us": 0, "p99_us": 0,
            "min_us": 0, "max_us": 0, "stddev_us": 0, "ops": 0,
            "ops_per_sec": ops_per_sec,
        }
        results.append({
            "qd": qd,
            "ops_per_sec": ops_per_sec,
            "wall_us": wall_us,
            "cpu_s": cpu_s,
            "cpu_pct": (cpu_s / (wall_us / 1e6)) * 100.0 if wall_us > 0 else 0.0,
            **{k: stats[k] for k in (
                "mean_us", "p50_us", "p95_us", "p99_us", "min_us", "max_us"
            )},
        })
    return results


def measure_zc_vs_ioctl(dev: EduDevice) -> dict:
    dev.map_rings()

    ioctl = measure_latency_qd1(dev, use_zc=False)
    zc = measure_latency_qd1(dev, use_zc=True)

    # Also compare pipeline throughput at QD=1 for submissions/sec
    run_pipeline(dev, 1, WARMUP_OPS, use_zc=False)
    ioctl_wall, ioctl_cpu, _ = run_pipeline(dev, 1, MEASURE_OPS, use_zc=False)
    run_pipeline(dev, 1, WARMUP_OPS, use_zc=True)
    zc_wall, zc_cpu, _ = run_pipeline(dev, 1, MEASURE_OPS, use_zc=True)

    ioctl_sps = MEASURE_OPS / (ioctl_wall / 1e6)
    zc_sps = MEASURE_OPS / (zc_wall / 1e6)

    lat_improve = (
        (ioctl["mean_us"] - zc["mean_us"]) / ioctl["mean_us"] * 100.0
        if ioctl["mean_us"] > 0 else 0.0
    )
    tput_improve = (
        (zc_sps - ioctl_sps) / ioctl_sps * 100.0 if ioctl_sps > 0 else 0.0
    )

    return {
        "ioctl": ioctl,
        "zc": zc,
        "ioctl_ops_per_sec": ioctl_sps,
        "zc_ops_per_sec": zc_sps,
        "ioctl_cpu_s": ioctl_cpu,
        "zc_cpu_s": zc_cpu,
        "latency_improvement_pct": lat_improve,
        "throughput_improvement_pct": tput_improve,
    }


def fmt_section(title: str) -> str:
    bar = "=" * 60
    return f"\n{bar}\n  {title}\n{bar}"


def format_latency(r: dict, label: str) -> str:
    return "\n".join([
        f"  path         : {label}",
        f"  ops measured : {r['ops']}",
        f"  submissions/s: {r['ops_per_sec']:,.1f}",
        f"  min          : {r['min_us']:.1f} µs",
        f"  mean         : {r['mean_us']:.1f} µs",
        f"  p50          : {r['p50_us']:.1f} µs",
        f"  p95          : {r['p95_us']:.1f} µs",
        f"  p99          : {r['p99_us']:.1f} µs",
        f"  max          : {r['max_us']:.1f} µs",
        f"  stddev       : {r['stddev_us']:.1f} µs",
        f"  CPU time     : {r.get('cpu_s', 0):.4f} s  ({r.get('cpu_pct', 0):.1f}% of wall)",
    ])


def format_qd_sweep(results: list) -> str:
    header = (
        f"  {'QD':>4}  {'ops/sec':>10}  {'mean':>8}  {'p50':>8}  "
        f"{'p95':>8}  {'p99':>8}  {'CPU%':>6}"
    )
    sep = "  " + "-" * 62
    rows = [
        f"  {r['qd']:>4}  {r['ops_per_sec']:>10,.1f}  {r['mean_us']:>8.1f}  "
        f"{r['p50_us']:>8.1f}  {r['p95_us']:>8.1f}  {r['p99_us']:>8.1f}  "
        f"{r['cpu_pct']:>5.1f}%"
        for r in results
    ]
    note = (
        "\n  Note: EDU HW is single-command; driver serializes kicks.\n"
        "  QD = software outstanding depth on SQ/CQ (not HW parallelism).\n"
        "  Expect roughly flat ops/sec across QD on this device."
    )
    return "\n".join([header, sep] + rows) + note


def format_zc(r: dict) -> str:
    io, zc = r["ioctl"], r["zc"]
    return "\n".join([
        "  --- ioctl copy submit (QD=1) ---",
        f"  mean / p50 / p95 / p99 : "
        f"{io['mean_us']:.1f} / {io['p50_us']:.1f} / {io['p95_us']:.1f} / {io['p99_us']:.1f} µs",
        f"  submissions/sec        : {r['ioctl_ops_per_sec']:,.1f}",
        f"  CPU (batch)            : {r['ioctl_cpu_s']:.4f} s",
        "  --- mmap + doorbell ZC submit (QD=1) ---",
        f"  mean / p50 / p95 / p99 : "
        f"{zc['mean_us']:.1f} / {zc['p50_us']:.1f} / {zc['p95_us']:.1f} / {zc['p99_us']:.1f} µs",
        f"  submissions/sec        : {r['zc_ops_per_sec']:,.1f}",
        f"  CPU (batch)            : {r['zc_cpu_s']:.4f} s",
        f"  latency improvement    : {r['latency_improvement_pct']:+.2f}%",
        f"  throughput improvement : {r['throughput_improvement_pct']:+.2f}%",
    ])


def main():
    parser = argparse.ArgumentParser(
        description="EduDevice latency + pipeline QD + zero-copy benchmark")
    parser.add_argument("--device", default=DEVICE_PATH)
    parser.add_argument("--output", default="benchmark_results.txt")
    parser.add_argument("--skip-zc", action="store_true",
                        help="Skip zero-copy vs ioctl comparison")
    args = parser.parse_args()

    if not os.path.exists(args.device):
        print(f"ERROR: {args.device} not found — run inside VM as root")
        sys.exit(1)

    lines = []

    def emit(s: str):
        print(s)
        lines.append(s)

    emit(f"EduDevice Benchmark — {args.device}")
    emit(f"Warmup ops: {WARMUP_OPS}  |  Measure ops: {MEASURE_OPS}  |  "
         f"Latency ops: {LATENCY_OPS}")
    emit("QD semantics: software outstanding SQ depth "
         "(EDU HW serialized by driver)")

    with EduDevice(args.device) as dev:
        emit(fmt_section("Latency  (ioctl path, queue depth = 1)"))
        lat = measure_latency_qd1(dev, use_zc=False)
        emit(format_latency(lat, "ioctl EDU_IOC_SUBMIT"))

        emit(fmt_section("Throughput vs real queue depth  (ioctl pipeline)"))
        tput = measure_qd_sweep(dev, use_zc=False)
        emit(format_qd_sweep(tput))

        peak = max(tput, key=lambda x: x["ops_per_sec"])
        emit(f"\n  Peak: {peak['ops_per_sec']:,.1f} ops/sec at QD={peak['qd']}")

        zc = None
        if not args.skip_zc:
            emit(fmt_section("Zero-copy mmap+doorbell vs ioctl submit  (QD=1)"))
            zc = measure_zc_vs_ioctl(dev)
            emit(format_zc(zc))

    emit(fmt_section("Resume-ready summary"))
    summary = (
        f"  {lat['mean_us']:.0f} µs mean latency (ioctl QD=1)  |  "
        f"{peak['ops_per_sec']:,.0f} ops/sec peak (QD={peak['qd']})"
    )
    if zc is not None:
        summary += (
            f"  |  ZC vs ioctl latency {zc['latency_improvement_pct']:+.1f}%  |  "
            f"ZC {zc['zc_ops_per_sec']:,.0f} vs ioctl {zc['ioctl_ops_per_sec']:,.0f} ops/sec"
        )
    emit(summary)
    emit("  Environment: QEMU EDU educational PCI device (single-command HW)")

    with open(args.output, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"\nResults written to {args.output}")


if __name__ == "__main__":
    main()
