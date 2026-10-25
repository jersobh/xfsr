#include "frame_transfer.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

void destroyImportedBuffer(DeviceContext* ctx, VkBuffer& buf, VkDeviceMemory& mem) {
    if (!ctx || !ctx->device) return;
    ctx->loadDispatch();
    auto& d = ctx->vk;
    if (buf && d.DestroyBuffer) {
        d.DestroyBuffer(ctx->device, buf, nullptr);
        buf = VK_NULL_HANDLE;
    }
    if (mem && d.FreeMemory) {
        d.FreeMemory(ctx->device, mem, nullptr);
        mem = VK_NULL_HANDLE;
    }
}

void createImportedHostBuffer(DeviceContext* ctx, void* hostPtr, VkDeviceSize size,
                              VkBuffer* outBuf, VkDeviceMemory* outMem) {
    ctx->loadDispatch();
    auto& d = ctx->vk;

    VkExternalMemoryBufferCreateInfo extBuf{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    extBuf.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &extBuf;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(d.CreateBuffer(ctx->device, &bci, nullptr, outBuf), "vkCreateBuffer(host import)");

    VkMemoryRequirements req{};
    d.GetBufferMemoryRequirements(ctx->device, *outBuf, &req);

    VkMemoryHostPointerPropertiesEXT hostProps{
        VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    vkCheck(d.GetMemoryHostPointerPropertiesEXT(
                ctx->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, hostPtr,
                &hostProps),
            "vkGetMemoryHostPointerPropertiesEXT");

    const uint32_t typeBits = req.memoryTypeBits & hostProps.memoryTypeBits;
    if (typeBits == 0) {
        throw std::runtime_error(std::string("No memory type for imported host pointer on ") +
                                 ctx->props.deviceName);
    }
    uint32_t typeIndex = 0;
    try {
        typeIndex = findMemoryType(ctx->physical, typeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } catch (...) {
        typeIndex = findMemoryType(ctx->physical, typeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }

    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = hostPtr;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &import;
    mai.allocationSize = std::max(req.size, size);
    mai.memoryTypeIndex = typeIndex;
    vkCheck(d.AllocateMemory(ctx->device, &mai, nullptr, outMem), "vkAllocateMemory(host import)");
    vkCheck(d.BindBufferMemory(ctx->device, *outBuf, *outMem, 0), "vkBindBufferMemory(host import)");
}

bool probeDmaBufCaps(DeviceContext* src, DeviceContext* dst) {
    if (!src || !dst || !src->extDmaBuf || !dst->extDmaBuf || !src->extExternalMemoryFd ||
        !dst->extExternalMemoryFd) {
        return false;
    }
    src->loadDispatch();
    if (!src->vk.GetMemoryFdKHR) return false;

    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkExternalMemoryHandleTypeFlagBits handle =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkPhysicalDeviceExternalBufferInfo srcInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    srcInfo.handleType = handle;
    srcInfo.usage = usage;
    VkExternalBufferProperties srcProps{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(src->physical, &srcInfo, &srcProps);
    const auto srcFeat = srcProps.externalMemoryProperties.externalMemoryFeatures;
    if (!(srcFeat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) return false;

    VkPhysicalDeviceExternalBufferInfo dstInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    dstInfo.handleType = handle;
    dstInfo.usage = usage;
    VkExternalBufferProperties dstProps{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(dst->physical, &dstInfo, &dstProps);
    const auto dstFeat = dstProps.externalMemoryProperties.externalMemoryFeatures;
    if (!(dstFeat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) return false;
    return true;
}

// Export a transfer buffer from src as dma-buf and import it on dst.
void createDmaBufPair(DeviceContext* src, DeviceContext* dst, VkDeviceSize size, VkBuffer* outSrcBuf,
                      VkDeviceMemory* outSrcMem, VkBuffer* outDstBuf, VkDeviceMemory* outDstMem) {
    src->loadDispatch();
    dst->loadDispatch();
    auto& sd = src->vk;
    auto& dd = dst->vk;
    if (!sd.GetMemoryFdKHR) throw std::runtime_error("GetMemoryFdKHR missing on src");

    const VkExternalMemoryHandleTypeFlagBits handle =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkExternalMemoryBufferCreateInfo extBuf{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    extBuf.handleTypes = handle;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &extBuf;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(sd.CreateBuffer(src->device, &bci, nullptr, outSrcBuf), "vkCreateBuffer(dma src)");

    VkMemoryRequirements req{};
    sd.GetBufferMemoryRequirements(src->device, *outSrcBuf, &req);

    VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exportInfo.handleTypes = handle;

    // Prefer device-local; fall back to host-visible (PRIME often stages through sysmem).
    uint32_t typeIndex = 0;
    try {
        typeIndex = findMemoryType(src->physical, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } catch (...) {
        typeIndex = findMemoryTypePrefer(src->physical, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exportInfo;
    mai.allocationSize = std::max(req.size, size);
    mai.memoryTypeIndex = typeIndex;
    vkCheck(sd.AllocateMemory(src->device, &mai, nullptr, outSrcMem), "vkAllocateMemory(dma src)");
    vkCheck(sd.BindBufferMemory(src->device, *outSrcBuf, *outSrcMem, 0),
            "vkBindBufferMemory(dma src)");

    VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    fdInfo.memory = *outSrcMem;
    fdInfo.handleType = handle;
    int fd = -1;
    vkCheck(sd.GetMemoryFdKHR(src->device, &fdInfo, &fd), "vkGetMemoryFdKHR");
    if (fd < 0) throw std::runtime_error("vkGetMemoryFdKHR returned invalid fd");

    // Import on dst (takes ownership of fd on success).
    VkExternalMemoryBufferCreateInfo dstExt{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    dstExt.handleTypes = handle;
    VkBufferCreateInfo dbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    dbci.pNext = &dstExt;
    dbci.size = size;
    dbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    dbci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult cr = dd.CreateBuffer(dst->device, &dbci, nullptr, outDstBuf);
    if (cr != VK_SUCCESS) {
        ::close(fd);
        destroyImportedBuffer(src, *outSrcBuf, *outSrcMem);
        throw std::runtime_error("vkCreateBuffer(dma dst) failed");
    }

    VkMemoryRequirements dreq{};
    dd.GetBufferMemoryRequirements(dst->device, *outDstBuf, &dreq);

    VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    import.handleType = handle;
    import.fd = fd;

    VkMemoryAllocateInfo dmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    dmai.pNext = &import;
    dmai.allocationSize = std::max(dreq.size, size);
    try {
        dmai.memoryTypeIndex =
            findMemoryType(dst->physical, dreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } catch (...) {
        dmai.memoryTypeIndex = findMemoryTypePrefer(dst->physical, dreq.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }

    cr = dd.AllocateMemory(dst->device, &dmai, nullptr, outDstMem);
    if (cr != VK_SUCCESS) {
        // Spec: failed import does not consume fd.
        ::close(fd);
        destroyImportedBuffer(dst, *outDstBuf, *outDstMem);
        destroyImportedBuffer(src, *outSrcBuf, *outSrcMem);
        throw std::runtime_error("vkAllocateMemory(dma import) failed");
    }
    vkCheck(dd.BindBufferMemory(dst->device, *outDstBuf, *outDstMem, 0),
            "vkBindBufferMemory(dma dst)");
}

void copyImageToBuffer(DeviceContext* ctx, VkCommandBuffer cmd, VkImage src, VkBuffer dst,
                       uint32_t w, uint32_t h) {
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    ctx->vk.CmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

void copyBufferToImage(DeviceContext* ctx, VkCommandBuffer cmd, VkBuffer src, VkImage dst,
                       uint32_t w, uint32_t h) {
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    ctx->vk.CmdCopyBufferToImage(cmd, src, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void copyImage(DeviceContext* ctx, VkCommandBuffer cmd, VkImage src, VkImage dst, uint32_t w,
               uint32_t h) {
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {w, h, 1};
    ctx->vk.CmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

} // namespace

const char* FrameTransfer::backendName(TransferBackend b) {
    switch (b) {
    case TransferBackend::DmaBuf:
        return "dma_buf";
    case TransferBackend::SharedHost:
        return "shared_host";
    default:
        return "memcpy";
    }
}

void FrameTransfer::init(DeviceContext* src, DeviceContext* dst) {
    srcCtx = src;
    dstCtx = dst;

    srcCtx->loadDispatch();
    dstCtx->loadDispatch();

    // NVIDIA↔Mesa dma-buf often probes OK then DEVICE_LOSTs on first submit
    // ("Not enough memory for command submission" on RADV). Prefer shared-host there
    // unless XFSR_DMABUF=1 forces a try.
    const char* forceDma = std::getenv("XFSR_DMABUF");
    const bool nvidiaInvolved =
        src->props.vendorID == 0x10deu || dst->props.vendorID == 0x10deu;
    const bool wantDma =
        probeDmaBufCaps(src, dst) &&
        ((forceDma && forceDma[0] == '1') || (!nvidiaInvolved && !(forceDma && forceDma[0] == '0')));
    if (wantDma) {
        backend = TransferBackend::DmaBuf;
    } else if (src->extExternalMemoryHost && dst->extExternalMemoryHost) {
        backend = TransferBackend::SharedHost;
    } else {
        backend = TransferBackend::Memcpy;
    }
    useSharedHost = backend == TransferBackend::SharedHost;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    pci.queueFamilyIndex = srcCtx->graphicsFamily;
    vkCheck(srcCtx->vk.CreateCommandPool(srcCtx->device, &pci, nullptr, &srcPool), "xfer src pool");

    pci.queueFamilyIndex = dstCtx->graphicsFamily;
    vkCheck(dstCtx->vk.CreateCommandPool(dstCtx->device, &pci, nullptr, &dstPool), "xfer dst pool");

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;

    for (int i = 0; i < kSlots; ++i) {
        cai.commandPool = srcPool;
        vkCheck(srcCtx->vk.AllocateCommandBuffers(srcCtx->device, &cai, &slots[i].srcCmd),
                "xfer src cmd");
        vkCheck(srcCtx->vk.CreateFence(srcCtx->device, &fci, nullptr, &slots[i].srcFence),
                "xfer src fence");

        cai.commandPool = dstPool;
        vkCheck(dstCtx->vk.AllocateCommandBuffers(dstCtx->device, &cai, &slots[i].dstCmd),
                "xfer dst cmd");
        vkCheck(dstCtx->vk.CreateFence(dstCtx->device, &fci, nullptr, &slots[i].dstFence),
                "xfer dst fence");
    }

    std::printf("FrameTransfer: backend=%s\n", backendName(backend));
}

void FrameTransfer::resize(uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp) {
    width = w;
    height = h;
    format = fmt;
    bytesPerPixel = bpp;
    resetPipeline();

    const VkDeviceSize align =
        std::max(srcCtx->minHostPointerAlignment, dstCtx->minHostPointerAlignment);
    const VkDeviceSize rawBytes =
        static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * bpp;
    const VkDeviceSize allocBytes = alignUp(rawBytes, align ? align : 4096);

    // Probe dma-buf once so all slots share the same backend.
    if (backend == TransferBackend::DmaBuf) {
        VkBuffer tSrcBuf = VK_NULL_HANDLE, tDstBuf = VK_NULL_HANDLE;
        VkDeviceMemory tSrcMem = VK_NULL_HANDLE, tDstMem = VK_NULL_HANDLE;
        try {
            createDmaBufPair(srcCtx, dstCtx, allocBytes, &tSrcBuf, &tSrcMem, &tDstBuf, &tDstMem);
            destroyImportedBuffer(srcCtx, tSrcBuf, tSrcMem);
            destroyImportedBuffer(dstCtx, tDstBuf, tDstMem);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "[xfsr] dma-buf staging failed (%s) — falling back to shared_host/memcpy\n",
                         e.what());
            backend = srcCtx->extExternalMemoryHost && dstCtx->extExternalMemoryHost
                          ? TransferBackend::SharedHost
                          : TransferBackend::Memcpy;
            useSharedHost = backend == TransferBackend::SharedHost;
        }
    }

    for (int i = 0; i < kSlots; ++i) {
        Slot& s = slots[i];
        destroyImportedBuffer(srcCtx, s.srcBuf, s.srcMem);
        destroyImportedBuffer(dstCtx, s.dstBuf, s.dstMem);
        if (s.hostPtr) {
            std::free(s.hostPtr);
            s.hostPtr = nullptr;
            s.hostBytes = 0;
        }
        s.srcLinear.destroy();
        s.dstLinear.destroy();
        s.dstOptimal.destroy();
        s.pendingUpload = false;

        if (backend == TransferBackend::DmaBuf) {
            createDmaBufPair(srcCtx, dstCtx, allocBytes, &s.srcBuf, &s.srcMem, &s.dstBuf,
                             &s.dstMem);
        } else if (backend == TransferBackend::SharedHost) {
            void* ptr = nullptr;
            if (posix_memalign(&ptr, static_cast<size_t>(align), static_cast<size_t>(allocBytes)) !=
                    0 ||
                !ptr) {
                throw std::runtime_error("posix_memalign failed for shared host buffer");
            }
            std::memset(ptr, 0, static_cast<size_t>(allocBytes));
            s.hostPtr = ptr;
            s.hostBytes = static_cast<size_t>(allocBytes);

            createImportedHostBuffer(srcCtx, s.hostPtr, allocBytes, &s.srcBuf, &s.srcMem);
            createImportedHostBuffer(dstCtx, s.hostPtr, allocBytes, &s.dstBuf, &s.dstMem);
        } else {
            const VkMemoryPropertyFlags hostCached =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            const VkMemoryPropertyFlags hostFallback =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

            try {
                s.srcLinear = createImage(
                    srcCtx, w, h, format,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, hostCached,
                    VK_IMAGE_TILING_LINEAR, false);
            } catch (...) {
                s.srcLinear = createImage(
                    srcCtx, w, h, format,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, hostFallback,
                    VK_IMAGE_TILING_LINEAR, false);
            }

            try {
                s.dstLinear = createImage(
                    dstCtx, w, h, format,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, hostCached,
                    VK_IMAGE_TILING_LINEAR, false);
            } catch (...) {
                s.dstLinear = createImage(
                    dstCtx, w, h, format,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, hostFallback,
                    VK_IMAGE_TILING_LINEAR, false);
            }
        }

        s.dstOptimal = createImage(
            dstCtx, w, h, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, true);
    }

    std::printf("Transfer staging %ux%u fmt=0x%x bpp=%u bytes=%llu backend=%s align=%llu\n", w, h,
                static_cast<unsigned>(format), bytesPerPixel,
                static_cast<unsigned long long>(allocBytes), backendName(backend),
                static_cast<unsigned long long>(align));
}

void FrameTransfer::resetPipeline() {
    if (srcCtx) srcCtx->waitIdle();
    if (dstCtx) dstCtx->waitIdle();
    produceIndex = 0;
    framesProduced = 0;
    roundTripReady = -1;
    for (int i = 0; i < kSlots; ++i) slots[i].pendingUpload = false;
}

void FrameTransfer::destroy() {
    resetPipeline();
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = slots[i];
        destroyImportedBuffer(srcCtx, s.srcBuf, s.srcMem);
        destroyImportedBuffer(dstCtx, s.dstBuf, s.dstMem);
        if (s.hostPtr) {
            std::free(s.hostPtr);
            s.hostPtr = nullptr;
        }
        s.srcLinear.destroy();
        s.dstLinear.destroy();
        s.dstOptimal.destroy();
        if (srcCtx && srcCtx->vk.DestroyFence && s.srcFence)
            srcCtx->vk.DestroyFence(srcCtx->device, s.srcFence, nullptr);
        if (dstCtx && dstCtx->vk.DestroyFence && s.dstFence)
            dstCtx->vk.DestroyFence(dstCtx->device, s.dstFence, nullptr);
        s.srcFence = s.dstFence = VK_NULL_HANDLE;
        s.srcCmd = s.dstCmd = VK_NULL_HANDLE;
    }
    if (srcCtx && srcPool && srcCtx->vk.DestroyCommandPool)
        srcCtx->vk.DestroyCommandPool(srcCtx->device, srcPool, nullptr);
    if (dstCtx && dstPool && dstCtx->vk.DestroyCommandPool)
        dstCtx->vk.DestroyCommandPool(dstCtx->device, dstPool, nullptr);
    srcPool = dstPool = VK_NULL_HANDLE;
    srcCtx = dstCtx = nullptr;
}

const GpuImage& FrameTransfer::transferSync(const GpuImage& srcOptimal, double& outMemcpyMs,
                                            double& outTotalMs) {
    auto tAll0 = std::chrono::steady_clock::now();
    outMemcpyMs = 0;
    Slot& s = slots[0];
    srcCtx->loadDispatch();
    dstCtx->loadDispatch();
    auto& sd = srcCtx->vk;
    auto& dd = dstCtx->vk;

    vkCheck(sd.WaitForFences(srcCtx->device, 1, &s.srcFence, VK_TRUE, UINT64_MAX), "xfer wait src");
    vkCheck(sd.ResetFences(srcCtx->device, 1, &s.srcFence), "xfer reset src");

    sd.ResetCommandBuffer(s.srcCmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    sd.BeginCommandBuffer(s.srcCmd, &bi);

    transitionImage(srcCtx, s.srcCmd, srcOptimal.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);

    if (usesGpuBuffer()) {
        copyImageToBuffer(srcCtx, s.srcCmd, srcOptimal.image, s.srcBuf, width, height);
    } else {
        transitionImage(srcCtx, s.srcCmd, s.srcLinear.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_READ_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
        copyImage(srcCtx, s.srcCmd, srcOptimal.image, s.srcLinear.image, width, height);
        transitionImage(srcCtx, s.srcCmd, s.srcLinear.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_HOST_READ_BIT);
    }

    transitionImage(srcCtx, s.srcCmd, srcOptimal.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);

    sd.EndCommandBuffer(s.srcCmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.srcCmd;
    vkCheck(sd.QueueSubmit(srcCtx->graphicsQueue, 1, &si, s.srcFence), "xfer submit download");
    vkCheck(sd.WaitForFences(srcCtx->device, 1, &s.srcFence, VK_TRUE, UINT64_MAX),
            "xfer fence download");

    if (!usesGpuBuffer()) {
        s.srcLinear.invalidateMapped();
        auto t0 = std::chrono::steady_clock::now();
        const auto* srcBytes = static_cast<const uint8_t*>(s.srcLinear.mapped) + s.srcLinear.offset;
        auto* dstBytes = static_cast<uint8_t*>(s.dstLinear.mapped) + s.dstLinear.offset;
        const size_t rowBytes = static_cast<size_t>(width) * bytesPerPixel;
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(dstBytes + y * s.dstLinear.rowPitch, srcBytes + y * s.srcLinear.rowPitch,
                        rowBytes);
        }
        auto t1 = std::chrono::steady_clock::now();
        outMemcpyMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        s.dstLinear.flushMapped();
    }

    vkCheck(dd.WaitForFences(dstCtx->device, 1, &s.dstFence, VK_TRUE, UINT64_MAX), "xfer wait dst");
    vkCheck(dd.ResetFences(dstCtx->device, 1, &s.dstFence), "xfer reset dst");

    dd.ResetCommandBuffer(s.dstCmd, 0);
    dd.BeginCommandBuffer(s.dstCmd, &bi);

    transitionImage(dstCtx, s.dstCmd, s.dstOptimal.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);

    if (usesGpuBuffer()) {
        copyBufferToImage(dstCtx, s.dstCmd, s.dstBuf, s.dstOptimal.image, width, height);
    } else {
        transitionImage(dstCtx, s.dstCmd, s.dstLinear.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
        copyImage(dstCtx, s.dstCmd, s.dstLinear.image, s.dstOptimal.image, width, height);
    }

    transitionImage(dstCtx, s.dstCmd, s.dstOptimal.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    dd.EndCommandBuffer(s.dstCmd);
    si.pCommandBuffers = &s.dstCmd;
    vkCheck(dd.QueueSubmit(dstCtx->graphicsQueue, 1, &si, s.dstFence), "xfer submit upload");
    vkCheck(dd.WaitForFences(dstCtx->device, 1, &s.dstFence, VK_TRUE, UINT64_MAX),
            "xfer fence upload");

    auto tAll1 = std::chrono::steady_clock::now();
    outTotalMs = std::chrono::duration<double, std::milli>(tAll1 - tAll0).count();
    return s.dstOptimal;
}

const GpuImage& FrameTransfer::transferPipeline(const GpuImage& srcOptimal, double& outMemcpyMs,
                                                double& outTotalMs, uint32_t waitSemCount,
                                                const VkSemaphore* waitSems,
                                                const VkPipelineStageFlags* waitStages,
                                                VkImageLayout srcLayout) {
    // Memcpy path cannot overlap cleanly across devices — stay fully sync.
    // transferSync always assumes SHADER_READ_ONLY; transition if needed.
    if (!pipelined || !usesGpuBuffer()) {
        if (srcLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            srcCtx->resetAndBegin();
            transitionImage(srcCtx, srcCtx->cmd, srcOptimal.image, srcLayout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            srcCtx->endSubmitWait();
        }
        return transferSync(srcOptimal, outMemcpyMs, outTotalMs);
    }

    auto tAll0 = std::chrono::steady_clock::now();
    outMemcpyMs = 0;
    srcCtx->loadDispatch();
    dstCtx->loadDispatch();
    auto& sd = srcCtx->vk;
    auto& dd = dstCtx->vk;

    const int fill = produceIndex % kSlots;
    Slot& s = slots[fill];

    // Reclaim slot (download + upload from N−3 finished).
    vkCheck(sd.WaitForFences(srcCtx->device, 1, &s.srcFence, VK_TRUE, UINT64_MAX),
            "xfer async wait src");
    vkCheck(dd.WaitForFences(dstCtx->device, 1, &s.dstFence, VK_TRUE, UINT64_MAX),
            "xfer async wait dst");
    vkCheck(sd.ResetFences(srcCtx->device, 1, &s.srcFence), "xfer async reset src");

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    // Kick download(N) — wait on app semaphores in-queue (no host stall).
    sd.ResetCommandBuffer(s.srcCmd, 0);
    sd.BeginCommandBuffer(s.srcCmd, &bi);
    recordDownload(s.srcCmd, srcOptimal, fill, srcLayout);
    sd.EndCommandBuffer(s.srcCmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (waitSemCount > 0 && waitSems) {
        si.waitSemaphoreCount = waitSemCount;
        si.pWaitSemaphores = waitSems;
        si.pWaitDstStageMask = waitStages;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.srcCmd;
    vkCheck(sd.QueueSubmit(srcCtx->graphicsQueue, 1, &si, s.srcFence), "xfer async download");
    s.pendingUpload = true;

    // Later submits must not wait on the app present-semaphores again.
    si.waitSemaphoreCount = 0;
    si.pWaitSemaphores = nullptr;
    si.pWaitDstStageMask = nullptr;

    // Kick upload(N−1) once its download has finished (overlap with next app frame).
    if (framesProduced >= 1) {
        const int prev = (fill + kSlots - 1) % kSlots;
        Slot& p = slots[prev];
        if (p.pendingUpload) {
            vkCheck(sd.WaitForFences(srcCtx->device, 1, &p.srcFence, VK_TRUE, UINT64_MAX),
                    "xfer async wait prev download");
            vkCheck(dd.ResetFences(dstCtx->device, 1, &p.dstFence), "xfer async reset prev dst");
            dd.ResetCommandBuffer(p.dstCmd, 0);
            dd.BeginCommandBuffer(p.dstCmd, &bi);
            recordUpload(p.dstCmd, prev);
            dd.EndCommandBuffer(p.dstCmd);
            si.pCommandBuffers = &p.dstCmd;
            vkCheck(dd.QueueSubmit(dstCtx->graphicsQueue, 1, &si, p.dstFence),
                    "xfer async upload");
            p.pendingUpload = false;
        }
    }

    const GpuImage* result = nullptr;
    if (framesProduced == 0) {
        // Prime: finish slot 0 so the first present has pixels.
        vkCheck(sd.WaitForFences(srcCtx->device, 1, &s.srcFence, VK_TRUE, UINT64_MAX),
                "xfer async prime download");
        vkCheck(dd.ResetFences(dstCtx->device, 1, &s.dstFence), "xfer async prime reset dst");
        dd.ResetCommandBuffer(s.dstCmd, 0);
        dd.BeginCommandBuffer(s.dstCmd, &bi);
        recordUpload(s.dstCmd, fill);
        dd.EndCommandBuffer(s.dstCmd);
        si.pCommandBuffers = &s.dstCmd;
        vkCheck(dd.QueueSubmit(dstCtx->graphicsQueue, 1, &si, s.dstFence), "xfer async prime upload");
        vkCheck(dd.WaitForFences(dstCtx->device, 1, &s.dstFence, VK_TRUE, UINT64_MAX),
                "xfer async prime wait");
        s.pendingUpload = false;
        result = &s.dstOptimal;
    } else if (framesProduced == 1) {
        const int prev = (fill + kSlots - 1) % kSlots;
        vkCheck(dd.WaitForFences(dstCtx->device, 1, &slots[prev].dstFence, VK_TRUE, UINT64_MAX),
                "xfer async warm wait");
        result = &slots[prev].dstOptimal;
    } else {
        // Consume frame N−2 while N downloads and N−1 uploads.
        const int ready = (fill + kSlots - 2) % kSlots;
        vkCheck(dd.WaitForFences(dstCtx->device, 1, &slots[ready].dstFence, VK_TRUE, UINT64_MAX),
                "xfer async ready wait");
        result = &slots[ready].dstOptimal;
    }

    produceIndex = (fill + 1) % kSlots;
    ++framesProduced;
    auto tAll1 = std::chrono::steady_clock::now();
    outTotalMs = std::chrono::duration<double, std::milli>(tAll1 - tAll0).count();
    return *result;
}

void FrameTransfer::recordDownload(VkCommandBuffer cmd, const GpuImage& srcOptimal, int slot,
                                   VkImageLayout srcLayout) {
    srcCtx->loadDispatch();
    Slot& s = slots[slot % kSlots];
    transitionImage(srcCtx, cmd, srcOptimal.image, srcLayout,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);
    if (usesGpuBuffer()) {
        copyImageToBuffer(srcCtx, cmd, srcOptimal.image, s.srcBuf, width, height);
    } else {
        transitionImage(srcCtx, cmd, s.srcLinear.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_READ_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
        copyImage(srcCtx, cmd, srcOptimal.image, s.srcLinear.image, width, height);
        transitionImage(srcCtx, cmd, s.srcLinear.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_HOST_READ_BIT);
    }
    transitionImage(srcCtx, cmd, srcOptimal.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    srcLayout, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0);
}

void FrameTransfer::recordUpload(VkCommandBuffer cmd, int slot) {
    dstCtx->loadDispatch();
    Slot& s = slots[slot % kSlots];
    // WAR vs the previous reader of this slot (present blit / FSR sample) — TOP_OF_PIPE
    // gave no execution dependency. Deliberately not ALL_COMMANDS: the dGPU upload
    // should still overlap unrelated app rendering on the shared queue.
    transitionImage(dstCtx, cmd, s.dstOptimal.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (usesGpuBuffer()) {
        copyBufferToImage(dstCtx, cmd, s.dstBuf, s.dstOptimal.image, width, height);
    } else {
        transitionImage(dstCtx, cmd, s.dstLinear.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
        copyImage(dstCtx, cmd, s.dstLinear.image, s.dstOptimal.image, width, height);
    }
    transitionImage(dstCtx, cmd, s.dstOptimal.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}
