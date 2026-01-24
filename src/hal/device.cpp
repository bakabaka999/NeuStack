#include "neustack/hal/device.hpp"

#if defined(NEUSTACK_PLATFORM_MACOS)
    #include "neustack/hal/hal_macos.hpp"
#elif defined(NEUSTACK_PLATFORM_LINUX)
    #include "neustack/hal/hal_linux.hpp"
#elif defined(NEUSTACK_PLATFORM_WINDOWS)
    #include "neustack/hal/hal_windows.hpp"
#endif

std::unique_ptr<NetDevice> NetDevice::create() {
#if defined(NEUSTACK_PLATFORM_MACOS)
    return std::make_unique<MacOSDevice>();
#elif defined(NEUSTACK_PLATFORM_LINUX)
    return std::make_unique<LinuxDevice>();
#elif defined(NEUSTACK_PLATFORM_WINDOWS)
    return std::make_unique<WindowsDevice>();
#else
    #error "Unsupported platform"
#endif
}