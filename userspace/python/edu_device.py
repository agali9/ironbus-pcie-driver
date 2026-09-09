# python/edu_device.py — Python ctypes bindings for libedu_device.so
# Wraps both legacy blocking ioctls and async ring path.

import ctypes
import ctypes.util
import os
import pathlib

# -------------------------------------------------------
# Load the shared library
# -------------------------------------------------------

def _load_lib() -> ctypes.CDLL:
    # Look next to this file first, then LD_LIBRARY_PATH
    here = pathlib.Path(__file__).parent
    candidates = [
        here.parent / "libedu_device.so",
        here / "libedu_device.so",
    ]
    for p in candidates:
        if p.exists():
            return ctypes.CDLL(str(p), use_errno=True)
    name = ctypes.util.find_library("edu_device")
    if name:
        return ctypes.CDLL(name, use_errno=True)
    raise OSError("libedu_device.so not found — run 'make' in userspace/")

_lib = _load_lib()

# -------------------------------------------------------
# Mirrored C structs
# -------------------------------------------------------

class EduStateReq(ctypes.Structure):
    _fields_ = [
        ("last_irq",      ctypes.c_uint32),
        ("last_fact_in",  ctypes.c_uint32),
        ("last_fact_out", ctypes.c_uint32),
        ("dma_ok",        ctypes.c_uint32),
    ]

class Completion(ctypes.Structure):
    _fields_ = [
        ("tag",    ctypes.c_uint16),
        ("status", ctypes.c_uint8),
        ("result", ctypes.c_uint32),
    ]
    def ok(self) -> bool:
        return self.status == 0
    def __repr__(self) -> str:
        return (f"Completion(tag={self.tag}, status={self.status}, "
                f"result={self.result}, ok={self.ok()})")

# -------------------------------------------------------
# C function signatures
# -------------------------------------------------------
# We expose a thin C shim (edu_device_c.cpp) that wraps the C++ class.
# See edu_device_c.hpp / edu_device_c.cpp below.
# Functions follow the pattern:  edu_XXX(handle, ...) -> int / uint32

_lib.edu_open.restype  = ctypes.c_void_p
_lib.edu_open.argtypes = [ctypes.c_char_p]

_lib.edu_close.restype  = None
_lib.edu_close.argtypes = [ctypes.c_void_p]

_lib.edu_factorial.restype  = ctypes.c_int   # 0=ok, -1=error
_lib.edu_factorial.argtypes = [ctypes.c_void_p,
                                ctypes.c_uint32,
                                ctypes.POINTER(ctypes.c_uint32)]

_lib.edu_dma_test.restype  = ctypes.c_int
_lib.edu_dma_test.argtypes = [ctypes.c_void_p]

_lib.edu_state.restype  = ctypes.c_int
_lib.edu_state.argtypes = [ctypes.c_void_p, ctypes.POINTER(EduStateReq)]

_lib.edu_submit.restype  = ctypes.c_int
_lib.edu_submit.argtypes = [ctypes.c_void_p,
                             ctypes.c_uint16,   # tag
                             ctypes.c_uint8,    # opcode
                             ctypes.c_uint32]   # operand

_lib.edu_poll_completion.restype  = ctypes.c_int   # 0=got one, -1=empty/err
_lib.edu_poll_completion.argtypes = [ctypes.c_void_p,
                                      ctypes.POINTER(Completion)]

_lib.edu_wait_completion.restype  = ctypes.c_int
_lib.edu_wait_completion.argtypes = [ctypes.c_void_p,
                                      ctypes.c_int,  # timeout_ms
                                      ctypes.POINTER(Completion)]

_lib.edu_map_rings.restype  = ctypes.c_int
_lib.edu_map_rings.argtypes = [ctypes.c_void_p]

_lib.edu_unmap_rings.restype  = None
_lib.edu_unmap_rings.argtypes = [ctypes.c_void_p]

_lib.edu_submit_zc.restype  = ctypes.c_int
_lib.edu_submit_zc.argtypes = [ctypes.c_void_p,
                                ctypes.c_uint16,
                                ctypes.c_uint8,
                                ctypes.c_uint32]

# Opcode constants (mirrors edu_device.hpp)
EDU_OP_FACTORIAL = 0x01
EDU_OP_DMA_TEST  = 0x02

# -------------------------------------------------------
# Python class
# -------------------------------------------------------

class EduDevice:
    """Python wrapper around libedu_device.so."""

    def __init__(self, path: str = "/dev/edu_pci"):
        self._handle = None  # initialize first so __del__ is safe on failure
        handle = _lib.edu_open(path.encode())
        if not handle:
            raise OSError(f"edu_open({path}) failed")
        self._handle = handle

    def close(self):
        if self._handle:
            _lib.edu_close(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        self.close()

    # ---- Legacy blocking ----

    def factorial(self, n: int) -> int:
        """Compute n! via the EDU device (blocking)."""
        out = ctypes.c_uint32(0)
        if _lib.edu_factorial(self._handle, ctypes.c_uint32(n),
                               ctypes.byref(out)) != 0:
            raise OSError("edu_factorial failed")
        return out.value

    def dma_test(self):
        """Run the built-in DMA round-trip test."""
        if _lib.edu_dma_test(self._handle) != 0:
            raise OSError("edu_dma_test failed")

    def state(self) -> EduStateReq:
        s = EduStateReq()
        if _lib.edu_state(self._handle, ctypes.byref(s)) != 0:
            raise OSError("edu_state failed")
        return s

    # ---- Async ring ----

    def submit(self, tag: int, opcode: int, operand: int = 0):
        """Submit a command to the SQ ring (non-blocking)."""
        ret = _lib.edu_submit(self._handle,
                               ctypes.c_uint16(tag),
                               ctypes.c_uint8(opcode),
                               ctypes.c_uint32(operand))
        if ret != 0:
            err = ctypes.get_errno()
            raise OSError(err, f"edu_submit failed (tag={tag})")

    def submit_factorial(self, tag: int, n: int):
        self.submit(tag, EDU_OP_FACTORIAL, n)

    def submit_dma_test(self, tag: int):
        self.submit(tag, EDU_OP_DMA_TEST, 0)

    def submit_zc(self, tag: int, opcode: int, operand: int = 0):
        """Zero-copy submit: write mmap'd SQ + EDU_IOC_DOORBELL (no descriptor copy)."""
        ret = _lib.edu_submit_zc(self._handle,
                                  ctypes.c_uint16(tag),
                                  ctypes.c_uint8(opcode),
                                  ctypes.c_uint32(operand))
        if ret != 0:
            err = ctypes.get_errno()
            raise OSError(err, f"edu_submit_zc failed (tag={tag})")

    def submit_factorial_zc(self, tag: int, n: int):
        self.submit_zc(tag, EDU_OP_FACTORIAL, n)

    def submit_dma_test_zc(self, tag: int):
        self.submit_zc(tag, EDU_OP_DMA_TEST, 0)

    def poll_completion(self) -> "Completion | None":
        """Non-blocking CQ poll. Returns None if empty."""
        c = Completion()
        ret = _lib.edu_poll_completion(self._handle, ctypes.byref(c))
        return c if ret == 0 else None

    def wait_completion(self, timeout_ms: int = 1000) -> "Completion | None":
        """Block until a completion arrives or timeout_ms elapses."""
        c = Completion()
        ret = _lib.edu_wait_completion(self._handle,
                                        ctypes.c_int(timeout_ms),
                                        ctypes.byref(c))
        return c if ret == 0 else None

    # ---- mmap ----

    def map_rings(self):
        if _lib.edu_map_rings(self._handle) != 0:
            raise OSError("edu_map_rings failed")

    def unmap_rings(self):
        _lib.edu_unmap_rings(self._handle)
