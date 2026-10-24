#pragma once

#include "dual_devices.hpp"
#include "gpu_image.hpp"

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <vector>

// FSR 3.1.4 temporal upscaler (FidelityFX SDK 1.1.4 Linux Vulkan).
// AMD's branded "3.1.5" is in FSR SDK 2.x (DX12 DLLs only today).
struct Fsr3Pass {
    DeviceContext* ctx = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    FfxDevice ffxDevice = nullptr;
    FfxInterface backendInterface{};
    std::vector<uint8_t> scratch;
    FfxFsr3UpscalerContext context{};
    bool contextReady = false;

    FfxResourceInternal sharedDilatedDepth{};
    FfxResourceInternal sharedDilatedMvs{};
    FfxResourceInternal sharedReconstructed{};
    bool sharedReady = false;

    GpuImage outputImg;
    uint32_t renderW = 0;
    uint32_t renderH = 0;
    uint32_t displayW = 0;
    uint32_t displayH = 0;

    float sharpness = 0.2f;
    bool enableSharpening = true;
    bool needReset = true;
    uint64_t frameIndex = 0;
    float lastFrameMs = 16.0f;

    float cameraNear = 0.1f;
    float cameraFar = 100.0f;
    float cameraFovY = 60.0f * 3.14159265f / 180.0f;

    void init(DeviceContext* deviceCtx, VkInstance inst);
    void resize(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh);
    void destroy();
    void resetHistory() { needReset = true; }

    void getJitter(float& outX, float& outY) const;

    void record(VkCommandBuffer cmd, const GpuImage& color, const GpuImage& depth,
                const GpuImage& motion);
    const GpuImage& output() const { return outputImg; }

private:
    void destroyContext();
    void createContext();
    FfxResource wrapImage(const GpuImage& img, FfxResourceStates state, const wchar_t* name,
                          bool isDepth) const;
};
