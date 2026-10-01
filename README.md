# Ironbus (pcie-driver)

Ironbus is a Linux kernel driver for QEMU's EDU PCIe device. It maps BAR0, handles MSI and legacy interrupts, does coherent DMA, and exposes a 64-entry submission/completion ring to userspace through ioctl, mmap, and eventfd. A C++17 userspace library, a C ABI shim, and Python ctypes bindings sit on top, and a Jenkins pipeline boots a QEMU VM, loads the driver, and runs the tests.

Everything here runs against QEMU's emulated EDU device. Nothing has been run on physical PCIe hardware.

## What's in the repo

| Path | Contents |
|------|----------|
| `kernel/` | `edu_pci.ko` — BAR0 MMIO, IRQ, DMA, SQ/CQ rings, debugfs, fault injection |
| `userspace/` | C++17 library, C ABI shim, Python bindings, demo, pytest suite |
| `scripts/` | Benchmark runner |
| `qemu-sketch/` | Notes for a custom QEMU device (optional; EDU is used by default) |
| `results/` | Captured demo / benchmark / debugfs output |
| `Jenkinsfile` | CI: build, boot QEMU with `-device edu`, run tests over SSH |

## Driver features

- BAR0 MMIO register access
- MSI and legacy interrupt support
- Coherent DMA buffers
- A miscdevice with a blocking ioctl path (`/dev/edu_pci`)
- A 64-entry submission/completion (SQ/CQ) ring with doorbell submission
- mmap'd SQ/CQ rings, eventfd completion notification, and poll
- debugfs statistics

## Userspace

- A C++17 RAII library over the driver
- A C ABI shim
- Python ctypes bindings

## Running the async driver

You need Linux with kernel headers. I tested it in a QEMU VM with the EDU device.

```bash
cd kernel
make
sudo insmod edu_pci.ko

cd ../userspace
make
sudo LD_LIBRARY_PATH=. ./demo
```

In QEMU, boot with `-device edu`. Should show up as `/dev/edu_pci`.

The async version does factorial math and DMA tests through both the blocking ioctl path and the SQ/CQ ring. In a recorded demo run, the ring pipelined 8 factorial commands at queue depth 8 with correct results, ran a DMA test through the ring, and inspected and unmapped the mmap'd rings.

### Tests

```bash
cd userspace
sudo PYTHONPATH=. python3 -m pytest python/test_edu_device.py -v
```

The pytest suite (36 tests) passes in the QEMU guest with the driver loaded.

### Benchmarks

```bash
sudo ./scripts/run_benchmarks.sh
```

## Results

All results are from a QEMU guest. The EDU device executes one command at a time, so these numbers measure the driver's software path, not hardware concurrency.

**Pipelining through the ring.** Keeping more commands outstanding lets the driver pipeline submission and completion work in software. Each depth ran 256 operations after 32 warmup operations:

| Outstanding commands | Throughput (ops/s) |
|----------------------|--------------------|
| 1 | 1,061 |
| 4 | 2,711 |
| 16 | 2,821 |
| 64 | 2,159 |

Throughput rose 2.66x from 1 to 16 outstanding commands. Median latency rose from 413 µs to 4,505 µs as commands queued, and at 64 outstanding commands the run hit 100% CPU.

**Correctness.** A recorded validation run matched all 2,346 asynchronous submissions to completions with zero DMA errors.

**mmap/doorbell vs ioctl.** Across 20 randomized paired runs at one outstanding command, mmap'd doorbell submission showed no stable advantage over ioctl submission: the median paired difference was -0.23%, with large variance.

## CI

A Jenkins pipeline builds the kernel module and userspace, boots a QEMU VM, loads the driver, runs the tests over SSH, and archives the logs.

## Requirements

- Linux + kernel headers
- g++, make, python3
- QEMU with `-device edu` support
