#pragma once
#include <cstdint>
#include <string>
extern "C" { #include <linux/ioctl.h> }
#define EDU_IOC_MAGIC 'E'
struct edu_fact_req { std::uint32_t input, output, irq_status; };
#define EDU_IOC_RUN_FACTORIAL _IOWR(EDU_IOC_MAGIC, 1, struct edu_fact_req)
class EduDevice {
public:
    explicit EduDevice(const std::string& path = "/dev/edu_pci");
    ~EduDevice();
    std::uint32_t factorial(std::uint32_t n);
    int fd() const { return fd_; }
private:
    int fd_ { -1 };
};
