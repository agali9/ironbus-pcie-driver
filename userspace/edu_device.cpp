#include "edu_device.hpp"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdexcept>

EduDevice::EduDevice(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR);
    if (fd_ < 0) throw std::runtime_error("open failed");
}
EduDevice::~EduDevice() { if (fd_ >= 0) ::close(fd_); }

std::uint32_t EduDevice::factorial(std::uint32_t n) {
    edu_fact_req req{}; req.input = n;
    if (::ioctl(fd_, EDU_IOC_RUN_FACTORIAL, &req) != 0)
        throw std::runtime_error("factorial ioctl failed");
    return req.output;
}
