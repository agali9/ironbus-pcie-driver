// demo.cpp — basic factorial check
#include "edu_device.hpp"
#include <iostream>

int main()
{
    try {
        EduDevice dev("/dev/edu_pci");
        std::cout << "Opened /dev/edu_pci  fd=" << dev.fd() << '\n';
        for (std::uint32_t n : {0u, 1u, 5u, 10u, 12u})
            std::cout << "  " << n << "! = " << dev.factorial(n) << '\n';
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
