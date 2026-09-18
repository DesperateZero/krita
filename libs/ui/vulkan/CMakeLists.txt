if(NOT HAVE_VULKAN)
    return()
endif()

set(kritavulkanuiadapter_LIB_SRCS
    KisVulkanCanvasAdapter.cpp
    KisVulkanPresenter.cpp
    KisVulkanViewAttachment.cpp
)

kis_add_library(kritavulkanuiadapter SHARED ${kritavulkanuiadapter_LIB_SRCS})

generate_export_header(kritavulkanuiadapter BASE_NAME kritavulkanuiadapter)

target_include_directories(kritavulkanuiadapter
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

# This downstream adapter may consume both interfaces. Neither kritaui nor the
# backend links back to this target, so it cannot form a dependency cycle.
target_link_libraries(kritavulkanuiadapter
    PUBLIC
        kritavulkanbackend
    PRIVATE
        kritaui
        Qt${QT_MAJOR_VERSION}::Core
)

set_target_properties(kritavulkanuiadapter
    PROPERTIES
        VERSION ${GENERIC_KRITA_LIB_VERSION}
        SOVERSION ${GENERIC_KRITA_LIB_SOVERSION}
)

# Internal until BR6: no installed/public ABI and no product registration.
