#include "fsr3_pass.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

void ffxCheck(FfxErrorCode code, const char* what) {
    if (code != FFX_OK) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s failed: FfxErrorCode=%d", what, static_cast<int>(code));
        throw std::runtime_error(buf);
    }
}

VkImageCreateInfo imageCreateInfoFor(const GpuImage& img, bool isDepth) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = img.format;
    ci.extent = {img.width, img.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = isDepth
                   ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
                   : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return ci;
}

} // namespace

void Fsr3Pass::init(DeviceContext* deviceCtx, VkInstance inst) {
    ctx = deviceCtx;
    instance = inst;
    ctx->makeCurrent();

    VkDeviceContext vkCtx{};
    vkCtx.vkDevice = ctx->device;
    vkCtx.vkPhysicalDevice = ctx->physical;
    vkCtx.vkDeviceProcAddr = vkGetDeviceProcAddr;
    vkCtx.instanceFunctions.getPhysicalDeviceFeatures2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    vkCtx.instanceFunctions.enumerateDeviceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            vkGetInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));
    vkCtx.instanceFunctions.getPhysicalDeviceMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties"));
    vkCtx.instanceFunctions.getPhysicalDeviceProperties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));

    if (!vkCtx.instanceFunctions.getPhysicalDeviceFeatures2) {
        vkCtx.instanceFunctions.getPhysicalDeviceFeatures2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if (!vkCtx.instanceFunctions.getPhysicalDeviceProperties2) {
        vkCtx.instanceFunctions.getPhysicalDeviceProperties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
    }

    ffxDevice = ffxGetDeviceVK(&vkCtx);

    const size_t maxContexts = FFX_FSR3UPSCALER_CONTEXT_COUNT;
    const size_t scratchBytes = ffxGetScratchMemorySizeVK(ctx->physical, maxContexts);
    scratch.assign(scratchBytes, 0);
    ffxCheck(ffxGetInterfaceVK(&backendInterface, ffxDevice, scratch.data(), scratch.size(), maxContexts),
             "ffxGetInterfaceVK");
}

void Fsr3Pass::destroyContext() {
    if (!contextReady) return;
    ctx->makeCurrent();
    ctx->waitIdle();
    if (sharedReady) {
        backendInterface.fpDestroyResource(&backendInterface, sharedDilatedDepth, 0);
        backendInterface.fpDestroyResource(&backendInterface, sharedDilatedMvs, 0);
        backendInterface.fpDestroyResource(&backendInterface, sharedReconstructed, 0);
        sharedReady = false;
    }
    ffxFsr3UpscalerContextDestroy(&context);
    contextReady = false;
    std::memset(&context, 0, sizeof(context));
}

void Fsr3Pass::createContext() {
    destroyContext();
    ctx->makeCurrent();

    FfxFsr3UpscalerContextDescription desc{};
    desc.flags = FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    desc.maxRenderSize = {renderW, renderH};
    desc.maxUpscaleSize = {displayW, displayH};
    desc.fpMessage = nullptr;
    desc.backendInterface = backendInterface;

    ffxCheck(ffxFsr3UpscalerContextCreate(&context, &desc), "ffxFsr3UpscalerContextCreate");
    contextReady = true;

    FfxFsr3UpscalerSharedResourceDescriptions shared{};
    ffxCheck(ffxFsr3UpscalerGetSharedResourceDescriptions(&context, &shared),
             "ffxFsr3UpscalerGetSharedResourceDescriptions");

    ffxCheck(backendInterface.fpCreateResource(&backendInterface, &shared.dilatedDepth, 0,
                                               &sharedDilatedDepth),
             "create dilatedDepth");
    ffxCheck(backendInterface.fpCreateResource(&backendInterface, &shared.dilatedMotionVectors, 0,
                                               &sharedDilatedMvs),
             "create dilatedMotionVectors");
    ffxCheck(backendInterface.fpCreateResource(&backendInterface, &shared.reconstructedPrevNearestDepth,
                                               0, &sharedReconstructed),
             "create reconstructedPrevNearestDepth");
    sharedReady = true;
    needReset = true;
    frameIndex = 0;
}

void Fsr3Pass::resize(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) {
    renderW = rw;
    renderH = rh;
    displayW = dw;
    displayH = dh;
    ctx->makeCurrent();
    outputImg.destroy();
    outputImg = createImage(ctx->physical, ctx->device, displayW, displayH, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, true);
    createContext();
}

void Fsr3Pass::destroy() {
    if (!ctx) return;
    destroyContext();
    outputImg.destroy();
    scratch.clear();
    std::memset(&backendInterface, 0, sizeof(backendInterface));
    ffxDevice = nullptr;
    ctx = nullptr;
    instance = VK_NULL_HANDLE;
}

void Fsr3Pass::getJitter(float& outX, float& outY) const {
    if (renderW == 0 || displayW == 0) {
        outX = outY = 0.0f;
        return;
    }
    const int32_t phase = ffxFsr3UpscalerGetJitterPhaseCount(static_cast<int32_t>(renderW),
                                                             static_cast<int32_t>(displayW));
    const int32_t index = static_cast<int32_t>(frameIndex % static_cast<uint64_t>(std::max(1, phase)));
    ffxFsr3UpscalerGetJitterOffset(&outX, &outY, index, phase);
}

FfxResource Fsr3Pass::wrapImage(const GpuImage& img, FfxResourceStates state, const wchar_t* name,
                                bool isDepth) const {
    // R32F depth export is a color image with depth semantics for FFX (Vega offload path).
    const bool depthStencilFmt = isDepth && img.format != VK_FORMAT_R32_SFLOAT;
    VkImageCreateInfo ci = imageCreateInfoFor(img, depthStencilFmt);
    FfxResourceUsage extra = FFX_RESOURCE_USAGE_READ_ONLY;
    if (isDepth) extra = static_cast<FfxResourceUsage>(extra | FFX_RESOURCE_USAGE_DEPTHTARGET);
    FfxResourceDescription desc = ffxGetImageResourceDescriptionVK(img.image, ci, extra);
    return ffxGetResourceVK(reinterpret_cast<void*>(img.image), desc, name, state);
}

void Fsr3Pass::record(VkCommandBuffer cmd, const GpuImage& color, const GpuImage& depth,
                      const GpuImage& motion) {
    if (!contextReady) return;
    ctx->makeCurrent();

    // Ensure output is UAV-ready
    transitionImage(cmd, outputImg.image,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, VK_ACCESS_SHADER_WRITE_BIT);

    float jx = 0, jy = 0;
    getJitter(jx, jy);

    FfxFsr3UpscalerDispatchDescription d{};
    d.commandList = ffxGetCommandListVK(cmd);
    d.color = wrapImage(color, FFX_RESOURCE_STATE_COMPUTE_READ, L"color", false);
    d.depth = wrapImage(depth, FFX_RESOURCE_STATE_COMPUTE_READ, L"depth", true);
    d.motionVectors = wrapImage(motion, FFX_RESOURCE_STATE_COMPUTE_READ, L"motion", false);
    d.exposure = {};
    d.reactive = {};
    d.transparencyAndComposition = {};
    d.dilatedDepth = backendInterface.fpGetResource(&backendInterface, sharedDilatedDepth);
    d.dilatedMotionVectors = backendInterface.fpGetResource(&backendInterface, sharedDilatedMvs);
    d.reconstructedPrevNearestDepth =
        backendInterface.fpGetResource(&backendInterface, sharedReconstructed);
    d.output = wrapImage(outputImg, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"fsr3out", false);
    d.jitterOffset = {jx, jy};
    d.motionVectorScale = {static_cast<float>(renderW), static_cast<float>(renderH)};
    d.renderSize = {renderW, renderH};
    d.upscaleSize = {displayW, displayH};
    d.enableSharpening = enableSharpening;
    d.sharpness = sharpness;
    d.frameTimeDelta = lastFrameMs;
    d.preExposure = 1.0f;
    d.reset = needReset;
    d.cameraNear = cameraNear;
    d.cameraFar = cameraFar;
    d.cameraFovAngleVertical = cameraFovY;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = 0;

    ffxCheck(ffxFsr3UpscalerContextDispatch(&context, &d), "ffxFsr3UpscalerContextDispatch");

    transitionImage(cmd, outputImg.image,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

    needReset = false;
    ++frameIndex;
}
