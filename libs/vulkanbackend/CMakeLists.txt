if(NOT HAVE_VULKAN)
    return()
endif()

set(KRITA_VULKAN_SHADER_MANIFEST
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/foundation_shader_manifest.json)
set(KRITA_VULKAN_SHADER_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/foundation_checksum.comp)
set(KRITA_VULKAN_SHADER_GENERATOR
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/generate_foundation_shader_assets.py)
set(KRITA_VULKAN_SHADER_OUTPUT_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders)
set(KRITA_VULKAN_SHADER_SPIRV
    ${KRITA_VULKAN_SHADER_OUTPUT_DIR}/foundation_checksum.comp.spv)
set(KRITA_VULKAN_SHADER_REFLECTION_RAW
    ${KRITA_VULKAN_SHADER_OUTPUT_DIR}/foundation_checksum.reflect.raw.json)
set(KRITA_VULKAN_SHADER_REFLECTION
    ${KRITA_VULKAN_SHADER_OUTPUT_DIR}/foundation_checksum.reflect.json)
set(KRITA_VULKAN_SHADER_GENERATED_HEADER
    ${KRITA_VULKAN_SHADER_OUTPUT_DIR}/KisVulkanFoundationShaderAssets.h)
set(KRITA_VULKAN_SHADER_GENERATED_CPP
    ${KRITA_VULKAN_SHADER_OUTPUT_DIR}/KisVulkanFoundationShaderAssets.cpp)
set(KRITA_VULKAN_SHADER_GENERATED_MANIFEST
    ${KRITA_VULKAN_SHADER_OUTPUT_DIR}/foundation_shader_manifest.generated.json)

if(NOT Python_EXECUTABLE)
    message(FATAL_ERROR "BUILD_VULKAN_FOUNDATION requires a Python interpreter")
endif()

add_custom_command(
    OUTPUT ${KRITA_VULKAN_SHADER_SPIRV}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KRITA_VULKAN_SHADER_OUTPUT_DIR}
    COMMAND ${KRITA_GLSLC_EXECUTABLE}
        --target-env=vulkan1.2
        -O
        -o ${KRITA_VULKAN_SHADER_SPIRV}
        ${KRITA_VULKAN_SHADER_SOURCE}
    COMMAND ${KRITA_SPIRV_VAL_EXECUTABLE}
        --target-env vulkan1.2
        ${KRITA_VULKAN_SHADER_SPIRV}
    DEPENDS ${KRITA_VULKAN_SHADER_SOURCE}
    VERBATIM
    COMMENT "Compiling and validating the Vulkan foundation checksum shader"
)

add_custom_command(
    OUTPUT
        ${KRITA_VULKAN_SHADER_GENERATED_HEADER}
        ${KRITA_VULKAN_SHADER_GENERATED_CPP}
        ${KRITA_VULKAN_SHADER_REFLECTION}
        ${KRITA_VULKAN_SHADER_GENERATED_MANIFEST}
    COMMAND ${KRITA_SPIRV_CROSS_EXECUTABLE}
        ${KRITA_VULKAN_SHADER_SPIRV}
        --reflect
        --output ${KRITA_VULKAN_SHADER_REFLECTION_RAW}
    COMMAND ${Python_EXECUTABLE}
        ${KRITA_VULKAN_SHADER_GENERATOR}
        --spirv ${KRITA_VULKAN_SHADER_SPIRV}
        --source ${KRITA_VULKAN_SHADER_SOURCE}
        --reflection ${KRITA_VULKAN_SHADER_REFLECTION_RAW}
        --contract ${KRITA_VULKAN_SHADER_MANIFEST}
        --header ${KRITA_VULKAN_SHADER_GENERATED_HEADER}
        --cpp ${KRITA_VULKAN_SHADER_GENERATED_CPP}
        --canonical-reflection ${KRITA_VULKAN_SHADER_REFLECTION}
        --generated-manifest ${KRITA_VULKAN_SHADER_GENERATED_MANIFEST}
    DEPENDS
        ${KRITA_VULKAN_SHADER_SPIRV}
        ${KRITA_VULKAN_SHADER_GENERATOR}
        ${KRITA_VULKAN_SHADER_MANIFEST}
    VERBATIM
    COMMENT "Reflecting and sealing Vulkan foundation shader assets"
)

add_custom_target(krita_vulkan_shader_assets
    DEPENDS
        ${KRITA_VULKAN_SHADER_GENERATED_HEADER}
        ${KRITA_VULKAN_SHADER_GENERATED_CPP}
        ${KRITA_VULKAN_SHADER_REFLECTION}
        ${KRITA_VULKAN_SHADER_GENERATED_MANIFEST}
    SOURCES
        ${KRITA_VULKAN_SHADER_MANIFEST}
        ${KRITA_VULKAN_SHADER_SOURCE}
        ${KRITA_VULKAN_SHADER_GENERATOR}
)

set(kritavulkanbase_LIB_SRCS
    KisVulkanMemory.cpp
    KisVulkanPipelineRepository.cpp
    KisVulkanResourceRetirementQueue.cpp
    KisVulkanShaderCatalog.cpp
    ${KRITA_VULKAN_SHADER_GENERATED_CPP}
)
set_source_files_properties(
    ${KRITA_VULKAN_SHADER_GENERATED_CPP}
    ${KRITA_VULKAN_SHADER_GENERATED_HEADER}
    PROPERTIES GENERATED TRUE)

# Internal foundation implementation. It is linked into the concrete backend,
# has no installed ABI, and cannot depend on kritaui.
add_library(kritavulkanbase STATIC ${kritavulkanbase_LIB_SRCS})
add_dependencies(kritavulkanbase krita_vulkan_shader_assets)
set_target_properties(kritavulkanbase PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(kritavulkanbase
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${KRITA_VULKAN_SHADER_OUTPUT_DIR}>
)
target_link_libraries(kritavulkanbase
    PRIVATE
        kritaimage
        Vulkan::Vulkan
        GPUOpen::VulkanMemoryAllocator
        Qt${QT_MAJOR_VERSION}::Core
)

set(kritavulkanbackend_LIB_SRCS
    KisVulkanBackendBootstrap.cpp
    KisVulkanCommandArena.cpp
    KisVulkanCompositeExecutor.cpp
    KisVulkanDeviceService.cpp
    KisVulkanDisplayExecutor.cpp
    KisVulkanDocumentGpuSession.cpp
    KisVulkanFoundationTypes.cpp
    KisVulkanPageBindingTable.cpp
    KisVulkanPageReplicaProvider.cpp
    KisVulkanSubmissionCoordinator.cpp
    KisVulkanStrokeExecutor.cpp
    KisVulkanWsiService.cpp
)

kis_add_library(kritavulkanbackend SHARED ${kritavulkanbackend_LIB_SRCS})

generate_export_header(kritavulkanbackend BASE_NAME kritavulkanbackend)

target_include_directories(kritavulkanbackend
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

# Absolute boundary: this target must never link kritaui. UI/view integration is
# reintroduced in BR6 as a downstream adapter after the MVP core is verified.
target_link_libraries(kritavulkanbackend
    PUBLIC
        kritaimage
    PRIVATE
        kritavulkanbase
        Vulkan::Vulkan
        GPUOpen::VulkanMemoryAllocator
        Qt${QT_MAJOR_VERSION}::Core
)

add_dependencies(kritavulkanbackend krita_vulkan_shader_assets)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

set_target_properties(kritavulkanbackend
    PROPERTIES
        VERSION ${GENERIC_KRITA_LIB_VERSION}
        SOVERSION ${GENERIC_KRITA_LIB_SOVERSION}
)

install(TARGETS kritavulkanbackend ${INSTALL_TARGETS_DEFAULT_ARGS})
