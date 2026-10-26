#pragma once

#include "dual_devices.hpp"
#include "gpu_image.hpp"

// Edge-aware spatial upscaler (quality / perf). Hybrid one-way or same-GPU.
struct EdgePass {
    DeviceContext* ctx = nullptr;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    static constexpr int kOutRing = 3;
    VkDescriptorSet sets[kOutRing]{};
    VkBuffer ubos[kOutRing]{};
    VkDeviceMemory uboMem[kOutRing]{};
    void* uboMapped[kOutRing]{};
    GpuImage out[kOutRing];
    int writeOut = 0;
    int readyOut = 0;

    uint32_t renderW = 0;
    uint32_t renderH = 0;
    uint32_t displayW = 0;
    uint32_t displayH = 0;
    float sharpness = 0.45f; // 0..1 — overridable live / XFSR_EDGE_SHARPNESS
    bool perfMode = false;   // XFSR_EDGE_MODE=perf|lite → cheaper path
    bool swizzleBgra = false;
    VkFormat outputFormat = VK_FORMAT_R8G8B8A8_UNORM;

    void init(DeviceContext* deviceCtx);
    void resize(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh);
    void destroy();

    void setSharpness(float s);
    void setPerfMode(bool perf);

    bool canDirectToSwapchain(VkFormat swapFormat) const;
    static bool formatNeedsBgraSwizzle(VkFormat swapFormat);

    // Optional directDst* writes the final image into a swapchain view (no blit).
    void record(VkCommandBuffer cmd, const GpuImage& src,
                VkImageView directDstView = VK_NULL_HANDLE,
                VkImage directDstImage = VK_NULL_HANDLE, bool directSwizzleBgra = false);

    const GpuImage& output() const { return out[readyOut]; }
};
