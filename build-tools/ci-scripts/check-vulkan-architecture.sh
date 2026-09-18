#!/usr/bin/env bash

set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repository_root}"

failed=0

require_absent() {
    local pattern="$1"
    shift
    local description="$1"
    shift

    if rg -n --glob '!build-tools/ci-scripts/check-vulkan-architecture.sh' \
        "${pattern}" "$@"; then
        echo "architecture violation: ${description}" >&2
        failed=1
    fi
}

require_present() {
    local pattern="$1"
    shift
    local description="$1"
    shift

    if ! rg -n "${pattern}" "$@" >/dev/null; then
        echo "architecture violation: missing ${description}" >&2
        failed=1
    fi
}

# BR0 production graph: the legacy mixed image/UI runtime must be unreachable.
if rg --files libs/image_vulkan 2>/dev/null | rg -q '.'; then
    echo 'architecture violation: archived image_vulkan runtime still exists in the source tree' >&2
    failed=1
fi
if rg --files libs/ui/vulkan 2>/dev/null | \
    rg -q '/KisVulkan(Canvas(_macOS)?\.(cpp|h|mm)|DeviceManager\.(cpp|h)|MemoryPool\.(cpp|h)|Swapchain\.(cpp|h)|\.(cpp|h))$'; then
    echo 'architecture violation: archived Vulkan UI runtime still exists in the source tree' >&2
    failed=1
fi
require_absent 'add_subdirectory[[:space:]]*\([[:space:]]*image_vulkan' \
    'legacy image_vulkan is still in the production build graph' \
    libs/CMakeLists.txt
require_absent '^[[:space:]]*kritaimagevulkan([[:space:]]|$)' \
    'kritaui still links the legacy Vulkan runtime' \
    libs/ui/CMakeLists.txt
require_absent 'vulkan/KisVulkan' \
    'legacy Vulkan canvas sources are still compiled by kritaui' \
    libs/ui/CMakeLists.txt
require_absent 'useVulkanCanvas|VULKAN_STARTED|KisVulkanCanvas' \
    'an unverified Vulkan product entry remains reachable' \
    libs/ui \
    --glob '!**/vulkan/**'
require_absent 'KisVulkanPaintRouter|activeInstance[[:space:]]*\(' \
    'global active canvas/router state remains in active UI code' \
    libs/ui/KisPresetShadowUpdater.cpp libs/ui/canvas
require_absent 'KisVulkanPaintRouter|KisVulkanUpdateInfo|kritaimagevulkan' \
    'a deleted legacy runtime symbol was reintroduced' \
    libs

# New boundary: backend depends down on kritaimage and never up on kritaui.
require_present 'kis_add_library[[:space:]]*\([[:space:]]*kritavulkanbackend' \
    'kritavulkanbackend target' \
    libs/vulkanbackend/CMakeLists.txt
require_present 'add_library[[:space:]]*\([[:space:]]*kritavulkanbase[[:space:]]+STATIC' \
    'internal kritavulkanbase target' \
    libs/vulkanbackend/CMakeLists.txt
require_present 'add_custom_target[[:space:]]*\([[:space:]]*krita_vulkan_shader_assets' \
    'explicit Vulkan shader asset target' \
    libs/vulkanbackend/CMakeLists.txt
require_present 'option[[:space:]]*\([[:space:]]*BUILD_VULKAN_FOUNDATION[^)]*OFF[[:space:]]*\)' \
    'default-off Vulkan foundation build option' \
    CMakeLists.txt
require_present 'option[[:space:]]*\([[:space:]]*BUILD_VULKAN_UI_ADAPTER[^)]*OFF[[:space:]]*\)' \
    'default-off Vulkan UI adapter build option' \
    CMakeLists.txt
require_present 'if[[:space:]]*\([[:space:]]*BUILD_VULKAN_FOUNDATION[[:space:]]*\)' \
    'independent Vulkan foundation target gate' \
    libs/CMakeLists.txt
require_present 'if[[:space:]]*\([[:space:]]*BUILD_VULKAN_UI_ADAPTER[[:space:]]*\)' \
    'independent Vulkan UI adapter target gate' \
    libs/CMakeLists.txt
require_present 'kis_add_library[[:space:]]*\([[:space:]]*kritavulkanuiadapter' \
    'disabled kritavulkanuiadapter target' \
    libs/ui/vulkan/CMakeLists.txt
require_absent '^[[:space:]]*kritaui([[:space:]]|$)' \
    'kritavulkanbackend links kritaui' \
    libs/vulkanbackend/CMakeLists.txt
require_absent 'QWidget|QWindow|KisCanvas' \
    'UI/window types leaked into the backend boundary' \
    libs/vulkanbackend
require_absent '#include[[:space:]]*[<"](vulkan|vk_mem_alloc)' \
    'Vulkan native types leaked into backend-neutral PageStore contracts' \
    libs/image/pagestore libs/image/gpustroke
require_present 'class[[:space:]]+.*KisVulkanDeviceService' \
    'device service foundation contract' \
    libs/vulkanbackend/KisVulkanDeviceService.h
require_present 'class[[:space:]]+.*KisVulkanShaderCatalog' \
    'shader catalog and ABI contract' \
    libs/vulkanbackend/KisVulkanShaderCatalog.h
require_present 'class[[:space:]]+.*KisVulkanResourceRetirementQueue' \
    'ticket-qualified resource retirement contract' \
    libs/vulkanbackend/KisVulkanResourceRetirementQueue.h
require_present 'class[[:space:]]+.*KisVulkanCompositeExecutor' \
    'composite executor contract' \
    libs/vulkanbackend/KisVulkanCompositeExecutor.h
require_present 'class[[:space:]]+.*KisVulkanDisplayExecutor' \
    'display executor contract' \
    libs/vulkanbackend/KisVulkanDisplayExecutor.h
require_present 'class[[:space:]]+.*KisVulkanWsiService' \
    'backend WSI ownership contract' \
    libs/vulkanbackend/KisVulkanWsiService.h
require_present 'KisVulkanFoundationContractsTest' \
    'foundation contract test target' \
    libs/vulkanbackend/tests/CMakeLists.txt
require_present 'KisPageStoreFoundationTest' \
    'backend-neutral PageStore foundation test target' \
    libs/image/tests/CMakeLists.txt
require_present 'struct[[:space:]]+.*KisEvaluationGraphSnapshot' \
    'backend-neutral immutable evaluation graph contract' \
    libs/image/pagestore/KisEvaluationGraphTypes.h
require_present 'struct[[:space:]]+.*KisEvaluationValueType' \
    'open semantic value type contract' \
    libs/image/pagestore/KisEvaluationGraphTypes.h
require_present 'class[[:space:]]+.*KisEvaluationGraphLowerer' \
    'authoring-model to EvalGraphIR lowering boundary' \
    libs/image/pagestore/KisEvaluationGraphLowerer.h
require_present 'class[[:space:]]+.*KisEvaluationPlanner' \
    'EvalGraphIR to task-DAG planning boundary' \
    libs/image/pagestore/KisEvaluationPlanner.h
require_present 'class[[:space:]]+.*KisDerivedSurfaceCache' \
    'dependency-qualified derived surface cache boundary' \
    libs/image/pagestore/KisDerivedSurfaceCache.h
require_absent 'KisLayerSegmentPlanner|KisGraphEpoch|KisDerivedCompositeCache|KisCompositeOperatorClass|KisEvaluationValueKind' \
    'layer-stack semantics leaked into the generic evaluation foundation' \
    libs/image/pagestore libs/image/gpustroke libs/vulkanbackend
require_present 'add_executable[[:space:]]*\([[:space:]]*kritavulkanoffscreen' \
    'real offscreen Vulkan smoke target' \
    libs/vulkanbackend/tests/CMakeLists.txt
require_present 'foundation_checksum\.comp' \
    'compiled foundation shader asset' \
    libs/vulkanbackend/CMakeLists.txt
require_absent 'install[[:space:]]*\([[:space:]]*TARGETS[[:space:]]+kritavulkanuiadapter' \
    'pre-BR6 UI adapter is installed as a public ABI' \
    libs/ui/vulkan/CMakeLists.txt

# Queue APIs are reserved for SubmissionCoordinator; blocking waits are not a
# runtime implementation option even there.
require_absent 'vkQueueSubmit|vkQueueSubmit2|vkQueueBindSparse|vkQueuePresentKHR' \
    'a backend component bypasses SubmissionCoordinator queue ownership' \
    libs/vulkanbackend \
    --glob '!KisVulkanSubmissionCoordinator.cpp' \
    --glob '!**/tests/**'
require_absent 'vkQueueWaitIdle|vkDeviceWaitIdle|UINT64_MAX' \
    'an unbounded Vulkan wait exists in the new backend' \
    libs/vulkanbackend \
    --glob '!**/tests/**'
require_absent 'vkQueueSubmit|vkQueueSubmit2|vkQueueBindSparse|vkQueuePresentKHR|vkQueueWaitIdle|vkDeviceWaitIdle|UINT64_MAX' \
    'the UI adapter owns queue operations or an unbounded wait' \
    libs/ui/vulkan

if ((failed)); then
    exit 1
fi

echo 'Vulkan architecture checks passed.'
