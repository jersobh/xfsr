#pragma once

#include "dual_devices.hpp"
#include "gpu_image.hpp"

// Cross-device frame handoff.
// Prefer: dma-buf fd export/import (VK_EXT_external_memory_dma_buf).
// Then: one 4 KiB-aligned host allocation imported into BOTH devices via
// VK_EXT_external_memory_host. Fallback: HOST_CACHED linear images + memcpy.
//
// Async 3-slot pipeline (dma-buf / shared-host):
//   frame N   — download in flight
//   frame N−1 — upload in flight
//   frame N−2 — ready for FSR/present
enum class TransferBackend : int {
    Memcpy = 0,
    SharedHost = 1,
    DmaBuf = 2,
};

struct FrameTransfer {
    static constexpr int kSlots = 3;

    DeviceContext* srcCtx = nullptr;
    DeviceContext* dstCtx = nullptr;
    TransferBackend backend = TransferBackend::Memcpy;
    bool useSharedHost = false; // legacy alias: backend == SharedHost
    bool pipelined = true;

    struct Slot {
        void* hostPtr = nullptr;
        size_t hostBytes = 0;
        VkBuffer srcBuf = VK_NULL_HANDLE;
        VkDeviceMemory srcMem = VK_NULL_HANDLE;
        VkBuffer dstBuf = VK_NULL_HANDLE;
        VkDeviceMemory dstMem = VK_NULL_HANDLE;

        GpuImage srcLinear;
        GpuImage dstLinear;

        GpuImage dstOptimal;
        VkCommandBuffer srcCmd = VK_NULL_HANDLE;
        VkCommandBuffer dstCmd = VK_NULL_HANDLE;
        VkFence srcFence = VK_NULL_HANDLE;
        VkFence dstFence = VK_NULL_HANDLE;
        bool pendingUpload = false; // download submitted; upload not yet kicked
    };
    Slot slots[kSlots]{};

    VkCommandPool srcPool = VK_NULL_HANDLE;
    VkCommandPool dstPool = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t bytesPerPixel = 4;
    int produceIndex = 0;
    int framesProduced = 0;

    void init(DeviceContext* src, DeviceContext* dst);
    void resize(uint32_t w, uint32_t h, VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM,
                uint32_t bpp = 4);
    void destroy();
    void resetPipeline();

    bool usesGpuBuffer() const {
        return backend == TransferBackend::DmaBuf || backend == TransferBackend::SharedHost;
    }
    static const char* backendName(TransferBackend b);

    const GpuImage& transferSync(const GpuImage& srcOptimal, double& outMemcpyMs,
                                 double& outTotalMs);
    // True async 3-slot: overlaps download(N), upload(N−1), consume(N−2).
    // Optional waitSems are waited on the download submit (no host stall) — use for
    // app QueuePresent wait-semaphores on the dGPU.
    // srcLayout: app swapchain images are PRESENT_SRC; FSR/compute outputs are often
    // SHADER_READ_ONLY (Zink offload round-trip back-haul).
    const GpuImage& transferPipeline(const GpuImage& srcOptimal, double& outMemcpyMs,
                                     double& outTotalMs, uint32_t waitSemCount = 0,
                                     const VkSemaphore* waitSems = nullptr,
                                     const VkPipelineStageFlags* waitStages = nullptr,
                                     VkImageLayout srcLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    void recordDownload(VkCommandBuffer cmd, const GpuImage& srcOptimal, int slot,
                        VkImageLayout srcLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    void recordUpload(VkCommandBuffer cmd, int slot);
    GpuImage& slotDstOptimal(int slot) { return slots[slot].dstOptimal; }
    VkFence slotSrcFence(int slot) { return slots[slot].srcFence; }
    VkFence slotDstFence(int slot) { return slots[slot].dstFence; }
    VkBuffer slotDstBuf(int slot) { return slots[slot].dstBuf; }

    GpuImage& dstLinear() { return slots[0].dstLinear; }
    GpuImage& dstOptimal() { return slots[0].dstOptimal; }
    GpuImage& srcLinear() { return slots[0].srcLinear; }
    void* hostPtr() { return slots[0].hostPtr; }
    size_t hostBytes() const { return slots[0].hostBytes; }
};
