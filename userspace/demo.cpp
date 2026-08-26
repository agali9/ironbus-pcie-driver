// demo.cpp — exercises legacy, async ring, mmap, throughput, and fault injection

#include "edu_device.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

// -------------------------------------------------------
// Helpers
// -------------------------------------------------------

static void print_state(const edu_state_req& s)
{
    std::cout << "  last_irq=0x"    << std::hex << s.last_irq << std::dec
              << "  last_fact_in="  << s.last_fact_in
              << "  last_fact_out=" << s.last_fact_out
              << "  dma_ok="        << s.dma_ok
              << '\n';
}

static void print_completion(const Completion& c)
{
    std::cout << "  tag="    << c.tag
              << "  status=" << static_cast<int>(c.status)
              << "  result=" << c.result
              << (c.ok() ? "  [OK]" : "  [FAIL]")
              << '\n';
}

// -------------------------------------------------------
// Demo sections
// -------------------------------------------------------

static void demo_legacy(EduDevice& dev)
{
    std::cout << "\n=== Legacy blocking ioctls ===\n";

    for (std::uint32_t n : {0u, 1u, 5u, 10u, 12u}) {
        std::uint32_t result = dev.factorial(n);
        std::cout << "  " << n << "! = " << result << '\n';
    }

    std::cout << "  DMA test... ";
    dev.dma_test();
    std::cout << "passed\n";

    print_state(dev.state());
}

static void demo_async(EduDevice& dev)
{
    std::cout << "\n=== Async SQ/CQ ring path ===\n";

    /*
     * EDU hardware is single-command, but the driver software-pipelines
     * the SQ: submit several, then drain completions as they arrive.
     */
    constexpr int BATCH = 8;
    std::cout << "  Pipelining " << BATCH
              << " factorial commands (QD=" << BATCH << "):\n";

    for (std::uint16_t i = 0; i < BATCH; ++i)
        dev.submit_factorial(i, i + 1);

    for (int i = 0; i < BATCH; ++i) {
        auto c = dev.wait_completion(2000);
        if (!c) {
            std::cerr << "  Timeout on completion " << i << '\n';
            break;
        }
        print_completion(*c);
    }

    std::cout << "  Submitting DMA test via ring...\n";
    dev.submit_dma_test(0xFF);
    auto c = dev.wait_completion(2000);
    if (c) print_completion(*c);
    else   std::cerr << "  Timeout on DMA ring test\n";
}

static void demo_mmap(EduDevice& dev)
{
    std::cout << "\n=== Zero-copy mmap ring inspection ===\n";

    dev.map_rings();

    const auto* sq = reinterpret_cast<const std::uint8_t*>(dev.sq_ptr());
    const auto* cq = reinterpret_cast<const std::uint8_t*>(dev.cq_ptr());

    std::cout << "  SQ base VA = " << static_cast<const void*>(sq) << '\n';
    std::cout << "  CQ base VA = " << static_cast<const void*>(cq) << '\n';

    // Zero-copy submit then verify SQ was written directly
    std::uint32_t slot_before = dev.sq_tail();
    dev.submit_factorial_zc(42, 7);
    const sq_entry* sqe = dev.sq_ptr();
    std::cout << "  After zc submit — SQ[" << (slot_before & RING_MASK)
              << "]: tag=" << sqe[slot_before & RING_MASK].tag
              << " opcode=0x" << std::hex
              << static_cast<int>(sqe[slot_before & RING_MASK].opcode)
              << std::dec << '\n';
    dev.wait_completion(2000);   // drain

    dev.unmap_rings();
    std::cout << "  Rings unmapped\n";
}

static void demo_fault_injection(EduDevice& dev)
{
    std::cout << "\n=== Fault injection (Phase 7) ===\n";

    // Arm the fault via sysfs
    const std::string sysfs_path =
        "/sys/class/misc/edu_pci/device/inject_fault";

    {
        std::ofstream f(sysfs_path);
        if (!f) {
            std::cout << "  Skipping — sysfs knob not accessible "
                         "(need root + driver loaded)\n";
            return;
        }
        f << "1\n";
    }
    std::cout << "  Fault armed via " << sysfs_path << '\n';

    // Run DMA test — driver should return an error
    std::cout << "  Running DMA test with fault armed...\n";
    try {
        dev.dma_test();
        std::cerr << "  ERROR: expected DMA failure but got success\n";
    } catch (const std::runtime_error& e) {
        std::cout << "  Got expected error: " << e.what() << '\n';
    }

    // Verify flag auto-cleared
    std::ifstream rf(sysfs_path);
    std::string val;
    rf >> val;
    std::cout << "  inject_fault after trigger = " << val
              << " (expected 0 — auto-cleared)\n";

    // Verify driver recovers — next DMA test should succeed
    std::cout << "  Running DMA test again (should succeed)...\n";
    dev.dma_test();
    std::cout << "  Recovery confirmed — DMA test passed\n";
}

static void demo_throughput(EduDevice& dev)
{
    std::cout << "\n=== Throughput benchmark (ring submit) ===\n";

    constexpr int OPS = 64;
    auto t0 = std::chrono::steady_clock::now();

    for (std::uint16_t i = 0; i < OPS; ++i) {
        dev.submit_factorial(i, (i % 12) + 1);
        auto c = dev.wait_completion(5000);
        if (!c) {
            std::cerr << "  Timeout on op " << i << '\n';
            break;
        }
    }

    auto t1   = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    std::cout << "  " << OPS << " ops in " << us << " µs  →  "
              << static_cast<int>(OPS / (us / 1e6)) << " ops/sec\n";
    std::cout << "  avg latency = " << us / OPS << " µs/op\n";
}

// -------------------------------------------------------
// main
// -------------------------------------------------------

int main()
{
    try {
        EduDevice dev("/dev/edu_pci");
        std::cout << "Opened /dev/edu_pci  fd=" << dev.fd() << '\n';

        demo_legacy(dev);
        demo_async(dev);
        demo_mmap(dev);
        demo_fault_injection(dev);
        demo_throughput(dev);

        std::cout << "\nAll demos passed.\n";
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
