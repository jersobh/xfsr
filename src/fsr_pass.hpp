#pragma once

#include "dual_devices.hpp"
#include "gpu_image.hpp"

struct FsrPass {
    DeviceContext* ctx = nullptr;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline easuPipeline = VK_NULL_HANDLE;
    VkPipeline rcasPipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    // One set/UBO per ring slot — never rewrite descriptors while a prior frame may still use them.
    static constexpr int kOutRing = 3;
    VkDescriptorSet easuSets[kOutRing]{};
    VkDescriptorSet rcasSets[kOutRing]{};
    VkBuffer easuUbos[kOutRing]{};
    VkDeviceMemory easuUboMem[kOutRing]{};
    void* easuUboMapped[kOutRing]{};
    VkBuffer rcasUbos[kOutRing]{};
    VkDeviceMemory rcasUboMem[kOutRing]{};
    void* rcasUboMapped[kOutRing]{};

    GpuImage easuOut[kOutRing];
    GpuImage rcasOut[kOutRing];
    int writeOut = 0;
    int readyOut = 0;
    uint32_t renderW = 0;
    uint32_t renderH = 0;
    uint32_t displayW = 0;
    uint32_t displayH = 0;
    float sharpness = 0.2f; // 0..1 mapped for RCAS
    bool enableRcas = true; // XFSR_RCAS=0 → EASU only
    bool useFp16 = false;
    VkFormat outputFormat = VK_FORMAT_R8G8B8A8_UNORM;
    // When true, final store swizzles RGB→BGR (B8G8R8A8 swapchain). Set per record().
    bool swizzleBgra = false;

    void init(DeviceContext* deviceCtx);
    void resize(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh);
    void destroy();

    // True when FSR can STORAGE-write the swapchain (RGBA8 or BGRA8 UNORM/SRGB if supported).
    bool canDirectToSwapchain(VkFormat swapFormat) const;
    static bool formatNeedsBgraSwizzle(VkFormat swapFormat);

    // src must be sampled at render resolution. Optional directDst* writes the final pass
    // into a swapchain image/view — no blit/copy after. swizzleBgra for B8G8R8A8_*.
    void record(VkCommandBuffer cmd, const GpuImage& src,
                VkImageView directDstView = VK_NULL_HANDLE,
                VkImage directDstImage = VK_NULL_HANDLE, bool directSwizzleBgra = false);

    const GpuImage& output() const {
        return enableRcas ? rcasOut[readyOut] : easuOut[readyOut];
    }
    double readGpuMs() const;
};
