#pragma once

#include <volk.h>

#include <cstring>

// Per-VkDevice function table. Avoids volkLoadDevice() on the hot path — global
// volk pointers are not safe when the layer switches NVIDIA ↔ Vega across threads.
struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;

    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;

    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;

    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkGetFenceStatus GetFenceStatus = nullptr;

    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;

    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges = nullptr;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges = nullptr;
    PFN_vkGetMemoryHostPointerPropertiesEXT GetMemoryHostPointerPropertiesEXT = nullptr;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR = nullptr;

    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;

    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;

    PFN_vkCreateQueryPool CreateQueryPool = nullptr;
    PFN_vkDestroyQueryPool DestroyQueryPool = nullptr;
    PFN_vkGetQueryPoolResults GetQueryPoolResults = nullptr;
    PFN_vkCmdResetQueryPool CmdResetQueryPool = nullptr;
    PFN_vkCmdWriteTimestamp CmdWriteTimestamp = nullptr;

    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdBlitImage CmdBlitImage = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdDispatch CmdDispatch = nullptr;

    bool loaded = false;

    void load(VkDevice device, PFN_vkGetDeviceProcAddr gdpa) {
        if (!device || !gdpa) return;
        GetDeviceProcAddr = gdpa;
#define LOAD(name) name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name))
        LOAD(DestroyDevice);
        LOAD(GetDeviceQueue);
        LOAD(DeviceWaitIdle);
        LOAD(QueueWaitIdle);
        LOAD(QueueSubmit);
        LOAD(CreateCommandPool);
        LOAD(DestroyCommandPool);
        LOAD(AllocateCommandBuffers);
        LOAD(FreeCommandBuffers);
        LOAD(ResetCommandBuffer);
        LOAD(BeginCommandBuffer);
        LOAD(EndCommandBuffer);
        LOAD(CreateFence);
        LOAD(DestroyFence);
        LOAD(ResetFences);
        LOAD(WaitForFences);
        LOAD(GetFenceStatus);
        LOAD(CreateSemaphore);
        LOAD(DestroySemaphore);
        LOAD(CreateBuffer);
        LOAD(DestroyBuffer);
        LOAD(GetBufferMemoryRequirements);
        LOAD(BindBufferMemory);
        LOAD(AllocateMemory);
        LOAD(FreeMemory);
        LOAD(MapMemory);
        LOAD(UnmapMemory);
        LOAD(FlushMappedMemoryRanges);
        LOAD(InvalidateMappedMemoryRanges);
        LOAD(GetMemoryHostPointerPropertiesEXT);
        LOAD(GetMemoryFdKHR);
        LOAD(CreateImage);
        LOAD(DestroyImage);
        LOAD(GetImageMemoryRequirements);
        LOAD(BindImageMemory);
        LOAD(GetImageSubresourceLayout);
        LOAD(CreateImageView);
        LOAD(DestroyImageView);
        LOAD(CreateSampler);
        LOAD(DestroySampler);
        LOAD(CreateShaderModule);
        LOAD(DestroyShaderModule);
        LOAD(CreateDescriptorSetLayout);
        LOAD(DestroyDescriptorSetLayout);
        LOAD(CreatePipelineLayout);
        LOAD(DestroyPipelineLayout);
        LOAD(CreateComputePipelines);
        LOAD(DestroyPipeline);
        LOAD(CreateDescriptorPool);
        LOAD(DestroyDescriptorPool);
        LOAD(AllocateDescriptorSets);
        LOAD(UpdateDescriptorSets);
        LOAD(CreateQueryPool);
        LOAD(DestroyQueryPool);
        LOAD(GetQueryPoolResults);
        LOAD(CmdResetQueryPool);
        LOAD(CmdWriteTimestamp);
        LOAD(CmdPipelineBarrier);
        LOAD(CmdCopyImage);
        LOAD(CmdBlitImage);
        LOAD(CmdCopyImageToBuffer);
        LOAD(CmdCopyBufferToImage);
        LOAD(CmdBindPipeline);
        LOAD(CmdBindDescriptorSets);
        LOAD(CmdDispatch);
#undef LOAD
        loaded = GetDeviceQueue != nullptr && QueueSubmit != nullptr;
    }
};
