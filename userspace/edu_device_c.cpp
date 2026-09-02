// edu_device_c.cpp — C shim implementation

#include "edu_device_c.hpp"
#include "edu_device.hpp"

#include <cerrno>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

// -------------------------------------------------------
// Helpers
// -------------------------------------------------------

static EduDevice* as_dev(void* h) { return static_cast<EduDevice*>(h); }

// -------------------------------------------------------
// Lifecycle
// -------------------------------------------------------

void* edu_open(const char* path)
{
    try {
        return new EduDevice(path ? path : "/dev/edu_pci");
    } catch (...) {
        errno = ENODEV;
        return nullptr;
    }
}

void edu_close(void* handle)
{
    delete as_dev(handle);
}

// -------------------------------------------------------
// Legacy blocking
// -------------------------------------------------------

int edu_factorial(void* handle, uint32_t n, uint32_t* out)
{
    try {
        *out = as_dev(handle)->factorial(n);
        return 0;
    } catch (...) { errno = EIO; return -1; }
}

int edu_dma_test(void* handle)
{
    try {
        as_dev(handle)->dma_test();
        return 0;
    } catch (...) { errno = EIO; return -1; }
}

int edu_state(void* handle, edu_state_t* out)
{
    try {
        auto s  = as_dev(handle)->state();
        out->last_irq      = s.last_irq;
        out->last_fact_in  = s.last_fact_in;
        out->last_fact_out = s.last_fact_out;
        out->dma_ok        = s.dma_ok;
        return 0;
    } catch (...) { errno = EIO; return -1; }
}

// -------------------------------------------------------
// Async ring
// -------------------------------------------------------

int edu_submit(void* handle, uint16_t tag, uint8_t opcode, uint32_t operand)
{
    try {
        as_dev(handle)->submit(tag, opcode, operand);
        return 0;
    } catch (const std::runtime_error& ex) {
        if (std::string(ex.what()).find("ring full") != std::string::npos)
            errno = ENOSPC;
        else if (std::string(ex.what()).find("bad opcode") != std::string::npos)
            errno = EINVAL;
        else
            errno = EIO;
        return -1;
    }
}

int edu_submit_zc(void* handle, uint16_t tag, uint8_t opcode, uint32_t operand)
{
    try {
        as_dev(handle)->submit_zc(tag, opcode, operand);
        return 0;
    } catch (const std::runtime_error& ex) {
        if (std::string(ex.what()).find("ring full") != std::string::npos)
            errno = ENOSPC;
        else if (std::string(ex.what()).find("bad opcode") != std::string::npos)
            errno = EINVAL;
        else if (std::string(ex.what()).find("map_rings") != std::string::npos)
            errno = EINVAL;
        else
            errno = EIO;
        return -1;
    }
}

int edu_poll_completion(void* handle, edu_completion_t* out)
{
    try {
        auto c = as_dev(handle)->poll_completion();
        if (!c) { errno = EAGAIN; return -1; }
        out->tag    = c->tag;
        out->status = c->status;
        out->result = c->result;
        return 0;
    } catch (...) { errno = EIO; return -1; }
}

int edu_wait_completion(void* handle, int timeout_ms, edu_completion_t* out)
{
    try {
        auto c = as_dev(handle)->wait_completion(timeout_ms);
        if (!c) { errno = ETIMEDOUT; return -1; }
        out->tag    = c->tag;
        out->status = c->status;
        out->result = c->result;
        return 0;
    } catch (...) { errno = EIO; return -1; }
}

// -------------------------------------------------------
// mmap
// -------------------------------------------------------

int edu_map_rings(void* handle)
{
    try {
        as_dev(handle)->map_rings();
        return 0;
    } catch (...) { errno = EIO; return -1; }
}

void edu_unmap_rings(void* handle)
{
    as_dev(handle)->unmap_rings();
}
