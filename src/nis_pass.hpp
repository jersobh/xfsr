#pragma once

#include "dual_devices.hpp"
#include "gpu_image.hpp"

// Spatial NIS-style upscaler (edge-directional + optional sharpen). Same ring /
// descriptor pattern as FsrPass so the layer can flip FSR1↔NIS↔blit live.
struct NisPass {
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
    float sharpness = 0.35f; // 0..1
    bool swizzleBgra = false;
    VkFormat outputFormat = VK_FORMAT_R8G8B8A8_UNORM;

    void init(DeviceContext* deviceCtx);
    void resize(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh);
    void destroy();

    void record(VkCommandBuffer cmd, const GpuImage& src);

    const GpuImage& output() const { return out[readyOut]; }
};
