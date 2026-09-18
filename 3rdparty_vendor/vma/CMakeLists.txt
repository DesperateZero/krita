##
## Vulkan Memory Allocator (VMA) from GPUOpen-LibrariesAndSDKs
## https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
##
## VMA is a single-header C++ library for Vulkan memory allocation.
## Version: 3.1.0 (or later)
##

if(HAVE_VULKAN)
    add_library(VulkanMemoryAllocator INTERFACE)
    # VMA is an upstream single-header implementation. Treat it as a system
    # include so Krita's warning policy does not report vendor-internal code.
    target_include_directories(VulkanMemoryAllocator SYSTEM INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(VulkanMemoryAllocator INTERFACE Vulkan::Vulkan)
    add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
endif()
