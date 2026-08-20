// edu_device.hpp — C++ RAII wrapper for /dev/edu_pci

#pragma once

#include <cstdint>
#include <optional>
#include <string>

extern "C" {
#include <linux/ioctl.h>
}

// -------------------------------------------------------
// Ring geometry — must match kernel edu_pci.c
// -------------------------------------------------------
constexpr std::uint32_t RING_SIZE      = 64;
constexpr std::uint32_t RING_MASK      = RING_SIZE - 1;
constexpr std::uint32_t SQ_ENTRY_BYTES = 16;
constexpr std::uint32_t CQ_ENTRY_BYTES = 16;
constexpr std::uint32_t SQ_SIZE        = RING_SIZE * SQ_ENTRY_BYTES;  // 1024
constexpr std::uint32_t CQ_SIZE        = RING_SIZE * CQ_ENTRY_BYTES;  // 1024
constexpr std::uint32_t DMA_BUF_TOTAL  = 4096;

constexpr std::uint8_t EDU_OP_FACTORIAL = 0x01;
constexpr std::uint8_t EDU_OP_DMA_TEST  = 0x02;

struct sq_entry {
    std::uint16_t tag;
    std::uint8_t  opcode;
    std::uint8_t  flags;
    std::uint32_t operand;
    std::uint64_t rsvd;
} __attribute__((packed));

struct cq_entry {
    std::uint16_t tag;
    std::uint8_t  status;
    std::uint8_t  flags;
    std::uint32_t result;
    std::uint64_t rsvd;
} __attribute__((packed));

static_assert(sizeof(sq_entry) == 16, "sq_entry layout drift vs kernel");
static_assert(sizeof(cq_entry) == 16, "cq_entry layout drift vs kernel");

// -------------------------------------------------------
// IOCTL structs — mirror kernel definitions
// -------------------------------------------------------
struct edu_fact_req {
    std::uint32_t input;
    std::uint32_t output;
    std::uint32_t irq_status;
};

struct edu_state_req {
    std::uint32_t last_irq;
    std::uint32_t last_fact_in;
    std::uint32_t last_fact_out;
    std::uint32_t dma_ok;
};

struct edu_submit_req {
    std::uint16_t tag;
    std::uint8_t  opcode;
    std::uint8_t  pad;
    std::uint32_t operand;
};

struct edu_completion_req {
    std::uint16_t tag;
    std::uint8_t  status;
    std::uint8_t  pad;
    std::uint32_t result;
};

#define EDU_IOC_MAGIC 'E'
#define EDU_IOC_RUN_FACTORIAL _IOWR(EDU_IOC_MAGIC, 1, struct edu_fact_req)
#define EDU_IOC_RUN_DMA_TEST  _IO(EDU_IOC_MAGIC, 2)
#define EDU_IOC_GET_STATE     _IOR(EDU_IOC_MAGIC, 3, struct edu_state_req)
#define EDU_IOC_SUBMIT        _IOW(EDU_IOC_MAGIC, 4, struct edu_submit_req)
#define EDU_IOC_POLL_CQ       _IOR(EDU_IOC_MAGIC, 5, struct edu_completion_req)
#define EDU_IOC_SET_EVENTFD   _IOW(EDU_IOC_MAGIC, 6, int)

// -------------------------------------------------------
// Completion value type returned to callers
// -------------------------------------------------------
struct Completion {
    std::uint16_t tag    { 0 };
    std::uint8_t  status { 0 };
    std::uint32_t result { 0 };

    bool ok() const { return status == 0; }
};

// -------------------------------------------------------
// EduDevice — userspace API
// -------------------------------------------------------
class EduDevice {
public:
    explicit EduDevice(const std::string& path = "/dev/edu_pci");
    ~EduDevice();

    EduDevice(const EduDevice&)            = delete;
    EduDevice& operator=(const EduDevice&) = delete;
    EduDevice(EduDevice&& other) noexcept;
    EduDevice& operator=(EduDevice&& other) noexcept;

    int fd() const { return fd_; }

    // Legacy blocking ioctls
    std::uint32_t factorial(std::uint32_t n);
    void dma_test();
    edu_state_req state();

    // Async ring (ioctl copy path)
    void submit(std::uint16_t tag, std::uint8_t opcode, std::uint32_t operand = 0);
    void submit_factorial(std::uint16_t tag, std::uint32_t n);
    void submit_dma_test(std::uint16_t tag);

    std::optional<Completion> poll_completion();
    std::optional<Completion> wait_completion(int timeout_ms = 1000);

    // mmap zero-copy ring access
    void map_rings();
    void unmap_rings();
    void submit_zc(std::uint16_t tag, std::uint8_t opcode, std::uint32_t operand = 0);
    void submit_factorial_zc(std::uint16_t tag, std::uint32_t n);
    void submit_dma_test_zc(std::uint16_t tag);

    const sq_entry* sq_ptr() const;
    const cq_entry* cq_ptr() const;
    std::uint32_t sq_tail() const { return sq_tail_; }

private:
    void register_eventfd();
    void ioctl_submit(std::uint16_t tag, std::uint8_t opcode, std::uint32_t operand);

    int         fd_            { -1 };
    int         evtfd_         { -1 };
    void*       ring_map_      { nullptr };
    bool        rings_mapped_  { false };
    std::uint32_t sq_tail_     { 0 };
};
