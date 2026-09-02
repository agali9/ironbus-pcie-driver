// edu_device_c.hpp — plain-C shim so Python ctypes can call the C++ class
// Every function takes an opaque handle returned by edu_open().

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Matches EduStateReq in edu_device.hpp
typedef struct {
    uint32_t last_irq;
    uint32_t last_fact_in;
    uint32_t last_fact_out;
    uint32_t dma_ok;
} edu_state_t;

// Matches Completion in edu_device.hpp
typedef struct {
    uint16_t tag;
    uint8_t  status;
    uint32_t result;
} edu_completion_t;

// Open /dev/edu_pci (or custom path). Returns NULL on error.
void* edu_open(const char* path);

// Close and free the handle.
void  edu_close(void* handle);

// Legacy blocking
// Returns 0 on success, -1 on error.
int   edu_factorial(void* handle, uint32_t n, uint32_t* out);
int   edu_dma_test(void* handle);
int   edu_state(void* handle, edu_state_t* out);

// Async ring submit (non-blocking)
// Returns 0 on success, -1 on error (errno: ENOSPC=ring full, EINVAL=bad opcode)
int   edu_submit(void* handle, uint16_t tag, uint8_t opcode, uint32_t operand);

// Zero-copy mmap submit — requires edu_map_rings() first
int   edu_submit_zc(void* handle, uint16_t tag, uint8_t opcode, uint32_t operand);

// Non-blocking CQ poll. Returns 0 if entry dequeued, -1 if empty.
int   edu_poll_completion(void* handle, edu_completion_t* out);

// Blocking wait. Returns 0 if entry received, -1 on timeout/error.
int   edu_wait_completion(void* handle, int timeout_ms, edu_completion_t* out);

// mmap ring buffer into this process
int   edu_map_rings(void* handle);
void  edu_unmap_rings(void* handle);

#ifdef __cplusplus
}
#endif
