// edu_device.cpp — EduDevice implementation

#include "edu_device.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unistd.h>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

namespace {

void throw_errno(const char* what)
{
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

}  // namespace

// -------------------------------------------------------
// Lifecycle
// -------------------------------------------------------

EduDevice::EduDevice(const std::string& path)
{
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
        throw_errno("open " + path);

    evtfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd_ < 0) {
        ::close(fd_);
        fd_ = -1;
        throw_errno("eventfd");
    }

    register_eventfd();
}

EduDevice::~EduDevice()
{
    unmap_rings();
    if (evtfd_ >= 0)
        ::close(evtfd_);
    if (fd_ >= 0)
        ::close(fd_);
}

EduDevice::EduDevice(EduDevice&& other) noexcept
    : fd_(other.fd_),
      evtfd_(other.evtfd_),
      ring_map_(other.ring_map_),
      rings_mapped_(other.rings_mapped_),
      sq_tail_(other.sq_tail_)
{
    other.fd_           = -1;
    other.evtfd_        = -1;
    other.ring_map_     = nullptr;
    other.rings_mapped_ = false;
    other.sq_tail_      = 0;
}

EduDevice& EduDevice::operator=(EduDevice&& other) noexcept
{
    if (this != &other) {
        unmap_rings();
        if (evtfd_ >= 0)
            ::close(evtfd_);
        if (fd_ >= 0)
            ::close(fd_);

        fd_           = other.fd_;
        evtfd_        = other.evtfd_;
        ring_map_     = other.ring_map_;
        rings_mapped_ = other.rings_mapped_;
        sq_tail_      = other.sq_tail_;

        other.fd_           = -1;
        other.evtfd_        = -1;
        other.ring_map_     = nullptr;
        other.rings_mapped_ = false;
        other.sq_tail_      = 0;
    }
    return *this;
}

void EduDevice::register_eventfd()
{
    if (::ioctl(fd_, EDU_IOC_SET_EVENTFD, &evtfd_) != 0)
        throw_errno("EDU_IOC_SET_EVENTFD");
}

// -------------------------------------------------------
// Legacy blocking ioctls
// -------------------------------------------------------

std::uint32_t EduDevice::factorial(std::uint32_t n)
{
    edu_fact_req req{};
    req.input = n;
    if (::ioctl(fd_, EDU_IOC_RUN_FACTORIAL, &req) != 0)
        throw_errno("EDU_IOC_RUN_FACTORIAL");
    return req.output;
}

void EduDevice::dma_test()
{
    if (::ioctl(fd_, EDU_IOC_RUN_DMA_TEST) != 0)
        throw_errno("EDU_IOC_RUN_DMA_TEST");
}

edu_state_req EduDevice::state()
{
    edu_state_req s{};
    if (::ioctl(fd_, EDU_IOC_GET_STATE, &s) != 0)
        throw_errno("EDU_IOC_GET_STATE");
    return s;
}

// -------------------------------------------------------
// Async ring — ioctl copy path
// -------------------------------------------------------

void EduDevice::ioctl_submit(std::uint16_t tag, std::uint8_t opcode, std::uint32_t operand)
{
    edu_submit_req req{};
    req.tag     = tag;
    req.opcode  = opcode;
    req.operand = operand;

    if (::ioctl(fd_, EDU_IOC_SUBMIT, &req) != 0) {
        if (errno == ENOSPC)
            throw std::runtime_error("ring full");
        if (errno == EINVAL)
            throw std::runtime_error("bad opcode");
        throw_errno("EDU_IOC_SUBMIT");
    }

    ++sq_tail_;
}

void EduDevice::submit(std::uint16_t tag, std::uint8_t opcode, std::uint32_t operand)
{
    ioctl_submit(tag, opcode, operand);
}

void EduDevice::submit_factorial(std::uint16_t tag, std::uint32_t n)
{
    submit(tag, EDU_OP_FACTORIAL, n);
}

void EduDevice::submit_dma_test(std::uint16_t tag)
{
    submit(tag, EDU_OP_DMA_TEST, 0);
}

std::optional<Completion> EduDevice::poll_completion()
{
    edu_completion_req c{};
    if (::ioctl(fd_, EDU_IOC_POLL_CQ, &c) != 0) {
        if (errno == EAGAIN)
            return std::nullopt;
        throw_errno("EDU_IOC_POLL_CQ");
    }

    Completion out;
    out.tag    = c.tag;
    out.status = c.status;
    out.result = c.result;
    return out;
}

std::optional<Completion> EduDevice::wait_completion(int timeout_ms)
{
    /*
     * EDU is a single-command device; a simple poll loop is reliable
     * even though eventfd is registered for future epoll-based callers.
     */
    const int step_us = 500;
    int waited_us = 0;
    const int limit_us = timeout_ms * 1000;

    while (waited_us < limit_us) {
        if (auto c = poll_completion())
            return c;
        ::usleep(step_us);
        waited_us += step_us;
    }
    return std::nullopt;
}

// -------------------------------------------------------
// mmap zero-copy path
// -------------------------------------------------------

void EduDevice::map_rings()
{
    if (rings_mapped_)
        return;

    void* p = ::mmap(nullptr, DMA_BUF_TOTAL, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED)
        throw_errno("mmap rings");

    ring_map_     = p;
    rings_mapped_ = true;
}

void EduDevice::unmap_rings()
{
    if (!rings_mapped_)
        return;

    ::munmap(ring_map_, DMA_BUF_TOTAL);
    ring_map_     = nullptr;
    rings_mapped_ = false;
}

const sq_entry* EduDevice::sq_ptr() const
{
    if (!rings_mapped_)
        throw std::runtime_error("map_rings() not called");
    return reinterpret_cast<const sq_entry*>(ring_map_);
}

const cq_entry* EduDevice::cq_ptr() const
{
    if (!rings_mapped_)
        throw std::runtime_error("map_rings() not called");
    auto* base = reinterpret_cast<const std::uint8_t*>(ring_map_);
    return reinterpret_cast<const cq_entry*>(base + SQ_SIZE);
}

void EduDevice::submit_zc(std::uint16_t tag, std::uint8_t opcode, std::uint32_t operand)
{
    if (!rings_mapped_)
        throw std::runtime_error("map_rings() required for zero-copy submit");

    const std::uint32_t slot = sq_tail_ & RING_MASK;
    auto* sq = const_cast<sq_entry*>(sq_ptr());

    sq[slot].tag     = tag;
    sq[slot].opcode  = opcode;
    sq[slot].flags   = 0;
    sq[slot].operand = operand;
    sq[slot].rsvd    = 0;

    __sync_synchronize();

    ioctl_submit(tag, opcode, operand);
}

void EduDevice::submit_factorial_zc(std::uint16_t tag, std::uint32_t n)
{
    submit_zc(tag, EDU_OP_FACTORIAL, n);
}

void EduDevice::submit_dma_test_zc(std::uint16_t tag)
{
    submit_zc(tag, EDU_OP_DMA_TEST, 0);
}
