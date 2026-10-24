#pragma once

#include "dual_devices.hpp"
#include "gpu_image.hpp"

#include <string>

// CPU-rasterized bitmap text uploaded each frame and composited in present.
struct HudOverlay {
    DeviceContext* ctx = nullptr;
    GpuImage image;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // RGBA8
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    void init(DeviceContext* deviceCtx, uint32_t w, uint32_t h);
    void destroy();
    void setText(const std::string& text);
    void upload(); // GPU upload of pixels → image (SHADER_READ_ONLY)
};
