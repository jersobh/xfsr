#pragma once

#include "common.hpp"

struct DeviceContext;

struct GpuImage {
    DeviceContext* ctx = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkDeviceSize size = 0;
    VkDeviceSize rowPitch = 0;
    VkDeviceSize offset = 0; // subresource offset for mapped range
    void* mapped = nullptr;
    bool hostVisible = false;
    bool hostCoherent = false;

    void destroy();
    void flushMapped();      // after host writes (no-op if coherent)
    void invalidateMapped(); // before host reads (no-op if coherent)
};

GpuImage createImage(DeviceContext* ctx, uint32_t width, uint32_t height, VkFormat format,
                     VkImageUsageFlags usage, VkMemoryPropertyFlags memProps,
                     VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL, bool createSampler = false);

// Legacy overload for call sites that only have phys/device (loads via global volk).
GpuImage createImage(VkPhysicalDevice phys, VkDevice device, uint32_t width, uint32_t height,
                     VkFormat format, VkImageUsageFlags usage, VkMemoryPropertyFlags memProps,
                     VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL, bool createSampler = false);

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                     VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                     VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                     VkAccessFlags dstAccess);

void transitionImage(DeviceContext* ctx, VkCommandBuffer cmd, VkImage image,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess);
