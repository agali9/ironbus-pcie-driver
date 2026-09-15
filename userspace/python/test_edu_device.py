# python/test_edu_device.py
# pytest suite for the Python EduDevice wrapper.
#
# Run (inside VM, after building):
#   cd ~/pcie_prototype_async/userspace
#   sudo python3 -m pytest python/test_edu_device.py -v
#
# Tests that need real hardware are skipped automatically
# if /dev/edu_pci is not present.

import os
import sys
import ctypes
import pytest

# Allow running from repo root or userspace/
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

# -------------------------------------------------------
# Skip marker — applied to all tests that need the device
# -------------------------------------------------------

DEVICE_PATH = "/dev/edu_pci"

def device_present() -> bool:
    return os.path.exists(DEVICE_PATH) and os.access(DEVICE_PATH, os.R_OK | os.W_OK)

needs_device = pytest.mark.skipif(
    not device_present(),
    reason=f"{DEVICE_PATH} not accessible — run inside VM as root"
)

# -------------------------------------------------------
# Import the wrapper (may fail if .so not built yet)
# -------------------------------------------------------

try:
    from python.edu_device import (
        EduDevice, EduStateReq, Completion,
        EDU_OP_FACTORIAL, EDU_OP_DMA_TEST,
    )
    _import_ok = True
except OSError as e:
    _import_ok = False
    _import_err = str(e)

pytestmark = pytest.mark.skipif(
    not _import_ok,
    reason=f"libedu_device.so not found: {'' if _import_ok else _import_err}"
)

# -------------------------------------------------------
# Fixtures
# -------------------------------------------------------

@pytest.fixture
def dev():
    """Open EduDevice, yield it, close on teardown."""
    d = EduDevice(DEVICE_PATH)
    yield d
    d.close()

# -------------------------------------------------------
# Struct / constant sanity — no device needed
# -------------------------------------------------------

class TestConstants:
    def test_opcode_factorial(self):
        assert EDU_OP_FACTORIAL == 0x01

    def test_opcode_dma_test(self):
        assert EDU_OP_DMA_TEST == 0x02

    def test_completion_ok(self):
        c = Completion()
        c.tag = 1; c.status = 0; c.result = 42
        assert c.ok()

    def test_completion_not_ok(self):
        c = Completion()
        c.status = 1
        assert not c.ok()

    def test_completion_repr(self):
        c = Completion()
        c.tag = 7; c.status = 0; c.result = 5040
        assert "5040" in repr(c)
        assert "ok=True" in repr(c)

# -------------------------------------------------------
# Open / close
# -------------------------------------------------------

class TestOpen:
    def test_bad_path_raises(self):
        with pytest.raises(OSError):
            EduDevice("/dev/does_not_exist_edu")

    @needs_device
    def test_open_close(self):
        d = EduDevice(DEVICE_PATH)
        d.close()

    @needs_device
    def test_context_manager(self):
        with EduDevice(DEVICE_PATH) as d:
            assert d._handle is not None

    @needs_device
    def test_double_close_safe(self):
        d = EduDevice(DEVICE_PATH)
        d.close()
        d.close()   # should not raise

# -------------------------------------------------------
# Legacy factorial
# -------------------------------------------------------

class TestFactorial:
    @needs_device
    def test_zero(self, dev):
        assert dev.factorial(0) == 1

    @needs_device
    def test_one(self, dev):
        assert dev.factorial(1) == 1

    @needs_device
    def test_five(self, dev):
        assert dev.factorial(5) == 120

    @needs_device
    def test_ten(self, dev):
        assert dev.factorial(10) == 3_628_800

    @needs_device
    def test_twelve(self, dev):
        assert dev.factorial(12) == 479_001_600

    @needs_device
    @pytest.mark.parametrize("n,expected", [
        (0, 1), (1, 1), (2, 2), (3, 6),
        (4, 24), (6, 720), (8, 40320),
    ])
    def test_parametrized(self, dev, n, expected):
        assert dev.factorial(n) == expected

# -------------------------------------------------------
# Legacy DMA test
# -------------------------------------------------------

class TestDmaTest:
    @needs_device
    def test_passes(self, dev):
        dev.dma_test()   # must not raise

    @needs_device
    def test_state_dma_ok(self, dev):
        dev.dma_test()
        s = dev.state()
        assert s.dma_ok == 1

# -------------------------------------------------------
# State ioctl
# -------------------------------------------------------

class TestState:
    @needs_device
    def test_state_after_factorial(self, dev):
        dev.factorial(9)
        s = dev.state()
        assert s.last_fact_in  == 9
        assert s.last_fact_out == 362880   # 9!

# -------------------------------------------------------
# Async ring: submit + wait
# -------------------------------------------------------

class TestAsyncRing:
    @needs_device
    def test_submit_factorial_single(self, dev):
        dev.submit_factorial(tag=1, n=6)
        c = dev.wait_completion(timeout_ms=2000)
        assert c is not None, "Timeout waiting for completion"
        assert c.tag    == 1
        assert c.ok()
        assert c.result == 720   # 6!

    @needs_device
    def test_submit_dma_test(self, dev):
        dev.submit_dma_test(tag=0xAB)
        c = dev.wait_completion(timeout_ms=2000)
        assert c is not None
        assert c.tag == 0xAB
        assert c.ok()

    @needs_device
    def test_poll_empty_returns_none(self, dev):
        result = dev.poll_completion()
        assert result is None

    @needs_device
    def test_wait_timeout_returns_none(self, dev):
        result = dev.wait_completion(timeout_ms=100)
        assert result is None

    @needs_device
    def test_batch_completions(self, dev):
        """Pipeline: submit all, then drain — real outstanding depth."""
        batch = 8
        for i in range(batch):
            dev.submit_factorial(tag=i, n=i + 1)
        for i in range(batch):
            c = dev.wait_completion(timeout_ms=2000)
            assert c is not None, f"Timeout draining batch at {i}"
            assert c.ok()
        # Tags may complete in order (FIFO kick); accept any permutation of 0..7
        # Re-run ordered check via second pipeline collecting tags
        tags = []
        for i in range(batch):
            dev.submit_factorial(tag=i, n=3)
        for _ in range(batch):
            c = dev.wait_completion(timeout_ms=2000)
            assert c is not None and c.ok()
            tags.append(c.tag)
        assert sorted(tags) == list(range(batch))

    @needs_device
    @pytest.mark.parametrize("n,expected", [(3,6),(5,120),(7,5040)])
    def test_async_factorial_values(self, dev, n, expected):
        dev.submit_factorial(tag=n, n=n)
        c = dev.wait_completion(2000)
        assert c is not None
        assert c.result == expected

# -------------------------------------------------------
# mmap
# -------------------------------------------------------

class TestMmap:
    @needs_device
    def test_map_unmap(self, dev):
        dev.map_rings()
        dev.unmap_rings()   # must not raise

    @needs_device
    def test_map_idempotent(self, dev):
        dev.map_rings()
        dev.map_rings()     # second call is a no-op

    @needs_device
    def test_unmap_without_map(self, dev):
        dev.unmap_rings()   # must not raise or crash

# -------------------------------------------------------
# mmap + true doorbell zero-copy
# -------------------------------------------------------

class TestZeroCopyDoorbell:
    @needs_device
    def test_zc_factorial(self, dev):
        dev.map_rings()
        try:
            dev.submit_factorial_zc(tag=99, n=5)
            c = dev.wait_completion(timeout_ms=2000)
            assert c is not None
            assert c.tag == 99
            assert c.ok()
            assert c.result == 120
        finally:
            dev.unmap_rings()

    @needs_device
    def test_zc_requires_map(self, dev):
        with pytest.raises(OSError):
            dev.submit_factorial_zc(tag=1, n=3)


# -------------------------------------------------------
# Software pipeline — multiple outstanding commands
# -------------------------------------------------------

class TestPipelineOutstanding:
    @needs_device
    def test_pipeline_qd4(self, dev):
        """Submit 4 before draining — exercises driver hw_busy queue."""
        qd = 4
        for i in range(qd):
            dev.submit_factorial(tag=i, n=i + 1)
        seen = {}
        for _ in range(qd):
            c = dev.wait_completion(timeout_ms=2000)
            assert c is not None
            assert c.ok()
            seen[c.tag] = c.result
        assert set(seen.keys()) == set(range(qd))
        assert seen[0] == 1
        assert seen[1] == 2
        assert seen[2] == 6
        assert seen[3] == 24

    @needs_device
    def test_pipeline_qd16(self, dev):
        qd = 16
        for i in range(qd):
            dev.submit_factorial(tag=i, n=(i % 12) + 1)
        tags = []
        for _ in range(qd):
            c = dev.wait_completion(timeout_ms=5000)
            assert c is not None and c.ok()
            tags.append(c.tag)
        assert sorted(tags) == list(range(qd))

    @needs_device
    def test_zc_pipeline_qd4(self, dev):
        dev.map_rings()
        try:
            qd = 4
            for i in range(qd):
                dev.submit_factorial_zc(tag=i + 100, n=4)
            for _ in range(qd):
                c = dev.wait_completion(timeout_ms=2000)
                assert c is not None and c.ok()
                assert c.result == 24
        finally:
            dev.unmap_rings()


# -------------------------------------------------------
# Error handling
# -------------------------------------------------------

class TestErrors:
    @needs_device
    def test_submit_bad_opcode(self, dev):
        with pytest.raises(OSError):
            dev.submit(tag=1, opcode=0xFF, operand=0)
