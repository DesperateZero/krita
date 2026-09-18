kis_add_test(
    KisVulkanFoundationContractsTest.cpp
    LINK_LIBRARIES kritavulkanbackend kritavulkanbase kritatestsdk
)

add_executable(kritavulkanoffscreen KisVulkanOffscreenSmoke.cpp)
target_link_libraries(kritavulkanoffscreen
    PRIVATE
        kritavulkanbase
        Vulkan::Vulkan
        GPUOpen::VulkanMemoryAllocator
)
add_test(NAME libs-vulkanbackend-kritavulkanoffscreen
         COMMAND kritavulkanoffscreen)
set_tests_properties(libs-vulkanbackend-kritavulkanoffscreen
    PROPERTIES
        LABELS "vulkan;foundation;offscreen"
        TIMEOUT 30)

# The macOS Vulkan loader does not search /usr/local (or an unpacked SDK) for
# portability-driver manifests by default. Derive the manifest beside the
# Vulkan library so the offscreen gate exercises MoltenVK deterministically.
if(APPLE)
    get_filename_component(_vulkan_library_dir "${Vulkan_LIBRARY}" DIRECTORY)
    get_filename_component(_vulkan_sdk_prefix "${_vulkan_library_dir}" DIRECTORY)
    set(_moltenvk_icd
        "${_vulkan_sdk_prefix}/share/vulkan/icd.d/MoltenVK_icd.json")
    if(EXISTS "${_moltenvk_icd}")
        set_tests_properties(libs-vulkanbackend-kritavulkanoffscreen
            PROPERTIES ENVIRONMENT "VK_ICD_FILENAMES=${_moltenvk_icd}")
    else()
        message(FATAL_ERROR
            "MoltenVK ICD manifest was not found beside Vulkan_LIBRARY: ${_moltenvk_icd}")
    endif()
endif()
