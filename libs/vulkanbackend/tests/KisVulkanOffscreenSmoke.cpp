/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define VK_ENABLE_BETA_EXTENSIONS
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include "KisVulkanFoundationShaderAssets.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t ValueCount = 256;
constexpr std::uint64_t WaitTimeoutNanoseconds = 5'000'000'000ULL;

struct Context
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkBuffer inputBuffer = VK_NULL_HANDLE;
    VmaAllocation inputAllocation = VK_NULL_HANDLE;
    VkBuffer outputBuffer = VK_NULL_HANDLE;
    VmaAllocation outputAllocation = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;

    ~Context()
    {
        if (device != VK_NULL_HANDLE) {
            if (timeline != VK_NULL_HANDLE) vkDestroySemaphore(device, timeline, nullptr);
            if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
            if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
            if (shaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, shaderModule, nullptr);
            if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
        }
        if (allocator != VK_NULL_HANDLE) {
            if (outputBuffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator, outputBuffer, outputAllocation);
            }
            if (inputBuffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator, inputBuffer, inputAllocation);
            }
            vmaDestroyAllocator(allocator);
        }
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }
};

bool hasExtension(const std::vector<VkExtensionProperties> &extensions, const char *name)
{
    return std::any_of(extensions.cbegin(), extensions.cend(), [name](const auto &extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

bool check(VkResult result, const char *operation)
{
    if (result == VK_SUCCESS) return true;
    std::cerr << operation << " failed with VkResult " << static_cast<int>(result) << '\n';
    return false;
}

std::vector<VkExtensionProperties> enumerateInstanceExtensions()
{
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) return {};
    extensions.resize(count);
    return extensions;
}

std::vector<VkExtensionProperties> enumerateDeviceExtensions(VkPhysicalDevice physicalDevice)
{
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS) return {};
    extensions.resize(count);
    return extensions;
}

int runSmoke()
{
    Context context;

    const auto instanceExtensions = enumerateInstanceExtensions();
    std::vector<const char *> enabledInstanceExtensions;
    VkInstanceCreateFlags instanceFlags = 0;
    if (hasExtension(instanceExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabledInstanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "Krita Vulkan BR0 offscreen smoke";
    applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    applicationInfo.pEngineName = "Krita";
    applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instanceCreateInfo{};
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.flags = instanceFlags;
    instanceCreateInfo.pApplicationInfo = &applicationInfo;
    instanceCreateInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(enabledInstanceExtensions.size());
    instanceCreateInfo.ppEnabledExtensionNames = enabledInstanceExtensions.data();
    if (!check(vkCreateInstance(&instanceCreateInfo, nullptr, &context.instance), "vkCreateInstance")) {
        return 1;
    }

    std::uint32_t physicalDeviceCount = 0;
    if (!check(vkEnumeratePhysicalDevices(context.instance, &physicalDeviceCount, nullptr),
               "vkEnumeratePhysicalDevices(count)") || physicalDeviceCount == 0) {
        std::cerr << "No Vulkan physical device is available\n";
        return 1;
    }
    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    if (!check(vkEnumeratePhysicalDevices(
                   context.instance, &physicalDeviceCount, physicalDevices.data()),
               "vkEnumeratePhysicalDevices(list)")) {
        return 1;
    }

    std::uint32_t queueFamilyIndex = 0;
    std::vector<VkExtensionProperties> selectedDeviceExtensions;
    VkPhysicalDeviceProperties selectedProperties{};
    for (VkPhysicalDevice candidate : physicalDevices) {
        VkPhysicalDeviceSynchronization2Features synchronization2{};
        synchronization2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
        timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        timeline.pNext = &synchronization2;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &timeline;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!timeline.timelineSemaphore || !synchronization2.synchronization2) continue;

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_2) continue;

        std::uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, queueFamilies.data());
        const auto queueFamily = std::find_if(
            queueFamilies.cbegin(), queueFamilies.cend(), [](const auto &properties) {
                return properties.queueCount > 0 &&
                       (properties.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            });
        if (queueFamily == queueFamilies.cend()) continue;

        const auto extensions = enumerateDeviceExtensions(candidate);
        if (properties.apiVersion < VK_API_VERSION_1_3 &&
            !hasExtension(extensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
            continue;
        }

        context.physicalDevice = candidate;
        selectedProperties = properties;
        selectedDeviceExtensions = extensions;
        queueFamilyIndex = static_cast<std::uint32_t>(queueFamily - queueFamilies.cbegin());
        break;
    }
    if (context.physicalDevice == VK_NULL_HANDLE) {
        std::cerr << "No Vulkan 1.2 compute device with timeline semaphores and synchronization2 is available\n";
        return 1;
    }

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    std::vector<const char *> enabledDeviceExtensions;
    if (hasExtension(selectedDeviceExtensions, "VK_KHR_portability_subset")) {
        enabledDeviceExtensions.push_back("VK_KHR_portability_subset");
    }
    if (selectedProperties.apiVersion < VK_API_VERSION_1_3) {
        enabledDeviceExtensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    }

    VkPhysicalDeviceSynchronization2Features synchronization2{};
    synchronization2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    synchronization2.synchronization2 = VK_TRUE;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.pNext = &synchronization2;
    timeline.timelineSemaphore = VK_TRUE;

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &timeline;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(enabledDeviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();
    if (!check(vkCreateDevice(context.physicalDevice, &deviceCreateInfo, nullptr, &context.device),
               "vkCreateDevice")) {
        return 1;
    }
    vkGetDeviceQueue(context.device, queueFamilyIndex, 0, &context.queue);

    VmaAllocatorCreateInfo allocatorCreateInfo{};
    allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    allocatorCreateInfo.instance = context.instance;
    allocatorCreateInfo.physicalDevice = context.physicalDevice;
    allocatorCreateInfo.device = context.device;
    if (!check(vmaCreateAllocator(&allocatorCreateInfo, &context.allocator),
               "vmaCreateAllocator")) {
        return 1;
    }

    VkBufferCreateInfo inputBufferCreateInfo{};
    inputBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    inputBufferCreateInfo.size = ValueCount * sizeof(std::uint32_t);
    inputBufferCreateInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    inputBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo hostAllocationCreateInfo{};
    hostAllocationCreateInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    hostAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    VmaAllocationInfo inputAllocationInfo{};
    if (!check(vmaCreateBuffer(context.allocator,
                               &inputBufferCreateInfo,
                               &hostAllocationCreateInfo,
                               &context.inputBuffer,
                               &context.inputAllocation,
                               &inputAllocationInfo),
               "vmaCreateBuffer(input)")) {
        return 1;
    }

    VkBufferCreateInfo outputBufferCreateInfo{};
    outputBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    outputBufferCreateInfo.size = sizeof(std::uint32_t);
    outputBufferCreateInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    outputBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationInfo outputAllocationInfo{};
    if (!check(vmaCreateBuffer(context.allocator,
                               &outputBufferCreateInfo,
                               &hostAllocationCreateInfo,
                               &context.outputBuffer,
                               &context.outputAllocation,
                               &outputAllocationInfo),
               "vmaCreateBuffer(output)")) {
        return 1;
    }

    auto *input = static_cast<std::uint32_t *>(inputAllocationInfo.pMappedData);
    auto *output = static_cast<std::uint32_t *>(outputAllocationInfo.pMappedData);
    if (!input || !output) {
        std::cerr << "VMA did not return mapped host pointers\n";
        return 1;
    }
    std::iota(input, input + ValueCount, 1U);
    *output = 0;
    if (!check(vmaFlushAllocation(context.allocator, context.inputAllocation, 0, VK_WHOLE_SIZE),
               "vmaFlushAllocation(input)") ||
        !check(vmaFlushAllocation(context.allocator, context.outputAllocation, 0, VK_WHOLE_SIZE),
               "vmaFlushAllocation(output)")) {
        return 1;
    }

    const VkDescriptorSetLayoutBinding descriptorBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
    };
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo{};
    descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutCreateInfo.bindingCount = 2;
    descriptorSetLayoutCreateInfo.pBindings = descriptorBindings;
    if (!check(vkCreateDescriptorSetLayout(context.device,
                                            &descriptorSetLayoutCreateInfo,
                                            nullptr,
                                            &context.descriptorSetLayout),
               "vkCreateDescriptorSetLayout")) {
        return 1;
    }

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.size = sizeof(std::uint32_t);
    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
    pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutCreateInfo.setLayoutCount = 1;
    pipelineLayoutCreateInfo.pSetLayouts = &context.descriptorSetLayout;
    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
    if (!check(vkCreatePipelineLayout(context.device,
                                      &pipelineLayoutCreateInfo,
                                      nullptr,
                                      &context.pipelineLayout),
               "vkCreatePipelineLayout")) {
        return 1;
    }

    VkShaderModuleCreateInfo shaderModuleCreateInfo{};
    shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleCreateInfo.codeSize =
        KisVulkanFoundationShaderAssets::checksumSpirvWordCount * sizeof(std::uint32_t);
    shaderModuleCreateInfo.pCode = KisVulkanFoundationShaderAssets::checksumSpirv;
    if (!check(vkCreateShaderModule(context.device,
                                    &shaderModuleCreateInfo,
                                    nullptr,
                                    &context.shaderModule),
               "vkCreateShaderModule")) {
        return 1;
    }

    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = context.shaderModule;
    shaderStage.pName = "main";
    VkComputePipelineCreateInfo pipelineCreateInfo{};
    pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.stage = shaderStage;
    pipelineCreateInfo.layout = context.pipelineLayout;
    if (!check(vkCreateComputePipelines(context.device,
                                        VK_NULL_HANDLE,
                                        1,
                                        &pipelineCreateInfo,
                                        nullptr,
                                        &context.pipeline),
               "vkCreateComputePipelines")) {
        return 1;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo descriptorPoolCreateInfo{};
    descriptorPoolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolCreateInfo.maxSets = 1;
    descriptorPoolCreateInfo.poolSizeCount = 1;
    descriptorPoolCreateInfo.pPoolSizes = &poolSize;
    if (!check(vkCreateDescriptorPool(context.device,
                                      &descriptorPoolCreateInfo,
                                      nullptr,
                                      &context.descriptorPool),
               "vkCreateDescriptorPool")) {
        return 1;
    }
    VkDescriptorSetAllocateInfo descriptorSetAllocateInfo{};
    descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocateInfo.descriptorPool = context.descriptorPool;
    descriptorSetAllocateInfo.descriptorSetCount = 1;
    descriptorSetAllocateInfo.pSetLayouts = &context.descriptorSetLayout;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (!check(vkAllocateDescriptorSets(context.device,
                                        &descriptorSetAllocateInfo,
                                        &descriptorSet),
               "vkAllocateDescriptorSets")) {
        return 1;
    }

    const VkDescriptorBufferInfo bufferInfos[] = {
        {context.inputBuffer, 0, inputBufferCreateInfo.size},
        {context.outputBuffer, 0, outputBufferCreateInfo.size}
    };
    VkWriteDescriptorSet descriptorWrites[2]{};
    for (std::uint32_t binding = 0; binding < 2; ++binding) {
        descriptorWrites[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[binding].dstSet = descriptorSet;
        descriptorWrites[binding].dstBinding = binding;
        descriptorWrites[binding].descriptorCount = 1;
        descriptorWrites[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptorWrites[binding].pBufferInfo = &bufferInfos[binding];
    }
    vkUpdateDescriptorSets(context.device, 2, descriptorWrites, 0, nullptr);

    VkCommandPoolCreateInfo commandPoolCreateInfo{};
    commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolCreateInfo.queueFamilyIndex = queueFamilyIndex;
    if (!check(vkCreateCommandPool(context.device,
                                   &commandPoolCreateInfo,
                                   nullptr,
                                   &context.commandPool),
               "vkCreateCommandPool")) {
        return 1;
    }
    VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
    commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocateInfo.commandPool = context.commandPool;
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferAllocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (!check(vkAllocateCommandBuffers(context.device,
                                        &commandBufferAllocateInfo,
                                        &commandBuffer),
               "vkAllocateCommandBuffers")) {
        return 1;
    }

    VkCommandBufferBeginInfo commandBufferBeginInfo{};
    commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo),
               "vkBeginCommandBuffer")) {
        return 1;
    }
    VkMemoryBarrier uploadBarrier{};
    uploadBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    uploadBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    uploadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         1,
                         &uploadBarrier,
                         0,
                         nullptr,
                         0,
                         nullptr);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipeline);
    vkCmdBindDescriptorSets(commandBuffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            context.pipelineLayout,
                            0,
                            1,
                            &descriptorSet,
                            0,
                            nullptr);
    vkCmdPushConstants(commandBuffer,
                       context.pipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(ValueCount),
                       &ValueCount);
    vkCmdDispatch(commandBuffer, (ValueCount + 63U) / 64U, 1, 1);
    VkMemoryBarrier downloadBarrier{};
    downloadBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    downloadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    downloadBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         1,
                         &downloadBarrier,
                         0,
                         nullptr,
                         0,
                         nullptr);
    if (!check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer")) return 1;

    VkSemaphoreTypeCreateInfo semaphoreTypeCreateInfo{};
    semaphoreTypeCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    semaphoreTypeCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphoreCreateInfo{};
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreCreateInfo.pNext = &semaphoreTypeCreateInfo;
    if (!check(vkCreateSemaphore(context.device,
                                 &semaphoreCreateInfo,
                                 nullptr,
                                 &context.timeline),
               "vkCreateSemaphore(timeline)")) {
        return 1;
    }

    const std::uint64_t signalValue = 1;
    VkTimelineSemaphoreSubmitInfo timelineSubmitInfo{};
    timelineSubmitInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineSubmitInfo.signalSemaphoreValueCount = 1;
    timelineSubmitInfo.pSignalSemaphoreValues = &signalValue;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = &timelineSubmitInfo;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &context.timeline;
    if (!check(vkQueueSubmit(context.queue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit")) {
        return 1;
    }

    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &context.timeline;
    waitInfo.pValues = &signalValue;
    const VkResult waitResult = vkWaitSemaphores(
        context.device, &waitInfo, WaitTimeoutNanoseconds);
    if (!check(waitResult, "vkWaitSemaphores(5 second timeout)")) return 1;
    if (!check(vmaInvalidateAllocation(
                   context.allocator, context.outputAllocation, 0, VK_WHOLE_SIZE),
               "vmaInvalidateAllocation(output)")) {
        return 1;
    }

    const std::uint32_t expected = (ValueCount * (ValueCount + 1U)) / 2U;
    if (*output != expected) {
        std::cerr << "Checksum mismatch: expected " << expected << ", got " << *output << '\n';
        return 1;
    }

    std::cout << "Vulkan BR0 offscreen smoke passed on " << selectedProperties.deviceName
              << "; checksum=" << *output
              << "; shader=" << KisVulkanFoundationShaderAssets::checksumSpirvSha256
              << '\n';
    return 0;
}

} // namespace

int main()
{
    return runSmoke();
}
