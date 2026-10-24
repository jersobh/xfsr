#include "gpu_image.hpp"
#include "dual_devices.hpp"

void GpuImage::destroy() {
    if (!device) return;
    DeviceDispatch* d = (ctx && ctx->vk.loaded) ? &ctx->vk : nullptr;
    if (d) {
        if (mapped && d->UnmapMemory) {
            d->UnmapMemory(device, memory);
            mapped = nullptr;
        }
        if (sampler && d->DestroySampler) d->DestroySampler(device, sampler, nullptr);
        if (view && d->DestroyImageView) d->DestroyImageView(device, view, nullptr);
        if (image && d->DestroyImage) d->DestroyImage(device, image, nullptr);
        if (memory && d->FreeMemory) d->FreeMemory(device, memory, nullptr);
    } else {
        volkLoadDevice(device);
        if (mapped) {
            vkUnmapMemory(device, memory);
            mapped = nullptr;
        }
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        if (view) vkDestroyImageView(device, view, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
    sampler = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    ctx = nullptr;
}

void GpuImage::flushMapped() {
    if (!mapped || hostCoherent || !device) return;
    if (ctx && ctx->vk.FlushMappedMemoryRanges) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkCheck(ctx->vk.FlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges");
        return;
    }
    volkLoadDevice(device);
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    vkCheck(vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges");
}

void GpuImage::invalidateMapped() {
    if (!mapped || hostCoherent || !device) return;
    if (ctx && ctx->vk.InvalidateMappedMemoryRanges) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkCheck(ctx->vk.InvalidateMappedMemoryRanges(device, 1, &range),
                "vkInvalidateMappedMemoryRanges");
        return;
    }
    volkLoadDevice(device);
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    vkCheck(vkInvalidateMappedMemoryRanges(device, 1, &range), "vkInvalidateMappedMemoryRanges");
}

GpuImage createImage(DeviceContext* ctx, uint32_t width, uint32_t height, VkFormat format,
                     VkImageUsageFlags usage, VkMemoryPropertyFlags memProps, VkImageTiling tiling,
                     bool createSamplerFlag) {
    if (!ctx || !ctx->device) throw std::runtime_error("createImage: null DeviceContext");
    ctx->loadDispatch();
    auto& d = ctx->vk;
    GpuImage out{};
    out.ctx = ctx;
    out.device = ctx->device;
    out.phys = ctx->physical;
    out.width = width;
    out.height = height;
    out.format = format;
    out.hostVisible = (memProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = tiling;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(d.CreateImage(ctx->device, &ici, nullptr, &out.image), "vkCreateImage");

    VkMemoryRequirements req{};
    d.GetImageMemoryRequirements(ctx->device, out.image, &req);
    out.size = req.size;

    uint32_t typeIndex = 0;
    bool got = false;
    try {
        typeIndex = findMemoryType(ctx->physical, req.memoryTypeBits, memProps);
        got = true;
        out.hostCoherent = (memProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    } catch (...) {
        if (out.hostVisible) {
            VkMemoryPropertyFlags withoutCoherent =
                (memProps & ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) |
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            typeIndex = findMemoryType(ctx->physical, req.memoryTypeBits, withoutCoherent);
            got = true;
            out.hostCoherent = false;
        }
    }
    if (!got) throw std::runtime_error("No suitable memory type");

    VkPhysicalDeviceMemoryProperties memPropsInfo{};
    vkGetPhysicalDeviceMemoryProperties(ctx->physical, &memPropsInfo);
    if (out.hostVisible) {
        out.hostCoherent =
            (memPropsInfo.memoryTypes[typeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) !=
            0;
    }

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = typeIndex;
    vkCheck(d.AllocateMemory(ctx->device, &mai, nullptr, &out.memory), "vkAllocateMemory(image)");
    vkCheck(d.BindImageMemory(ctx->device, out.image, out.memory, 0), "vkBindImageMemory");

    if (tiling == VK_IMAGE_TILING_LINEAR) {
        VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout{};
        d.GetImageSubresourceLayout(ctx->device, out.image, &sub, &layout);
        out.rowPitch = layout.rowPitch;
        out.size = layout.size;
        out.offset = layout.offset;
    } else {
        out.rowPitch = static_cast<VkDeviceSize>(width) * 4;
    }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    vkCheck(d.CreateImageView(ctx->device, &vci, nullptr, &out.view), "vkCreateImageView");

    if (createSamplerFlag) {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        vkCheck(d.CreateSampler(ctx->device, &sci, nullptr, &out.sampler), "vkCreateSampler");
    }

    if (out.hostVisible) {
        vkCheck(d.MapMemory(ctx->device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped), "vkMapMemory");
    }
    return out;
}

GpuImage createImage(VkPhysicalDevice phys, VkDevice device, uint32_t width, uint32_t height,
                     VkFormat format, VkImageUsageFlags usage, VkMemoryPropertyFlags memProps,
                     VkImageTiling tiling, bool createSamplerFlag) {
    // Demo path: no DeviceContext — use global volk; GpuImage::ctx stays null.
    volkLoadDevice(device);
    GpuImage out{};
    out.device = device;
    out.phys = phys;
    out.width = width;
    out.height = height;
    out.format = format;
    out.hostVisible = (memProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = tiling;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(device, &ici, nullptr, &out.image), "vkCreateImage");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, out.image, &req);
    out.size = req.size;
    uint32_t typeIndex = findMemoryType(phys, req.memoryTypeBits, memProps);
    out.hostCoherent = (memProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = typeIndex;
    vkCheck(vkAllocateMemory(device, &mai, nullptr, &out.memory), "vkAllocateMemory(image)");
    vkCheck(vkBindImageMemory(device, out.image, out.memory, 0), "vkBindImageMemory");

    if (tiling == VK_IMAGE_TILING_LINEAR) {
        VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(device, out.image, &sub, &layout);
        out.rowPitch = layout.rowPitch;
        out.size = layout.size;
        out.offset = layout.offset;
    } else {
        out.rowPitch = static_cast<VkDeviceSize>(width) * 4;
    }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    vkCheck(vkCreateImageView(device, &vci, nullptr, &out.view), "vkCreateImageView");

    if (createSamplerFlag) {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        vkCheck(vkCreateSampler(device, &sci, nullptr, &out.sampler), "vkCreateSampler");
    }
    if (out.hostVisible) {
        vkCheck(vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped), "vkMapMemory");
    }
    (void)tiling;
    return out;
}

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                     VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                     VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                     VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void transitionImage(DeviceContext* ctx, VkCommandBuffer cmd, VkImage image,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    if (ctx && ctx->vk.CmdPipelineBarrier) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        ctx->vk.CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        return;
    }
    transitionImage(cmd, image, oldLayout, newLayout, srcStage, dstStage, srcAccess, dstAccess);
}
