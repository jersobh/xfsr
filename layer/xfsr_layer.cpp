// VK_LAYER_XFSR — dual-GPU FSR1 offload (demo mode 2).
// Enable: ENABLE_XFSR=1
// Config: XFSR_FSR=1 XFSR_PRESET=quality|balanced|performance
//         XFSR_OFFLOAD=1 (default)
// Legacy CROSSVULKAN_* env names still accepted.
//
// Proxy swapchain: app gets low-res dGPU images; real present is iGPU + FSR1.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <volk.h>
#include <vulkan/vk_layer.h>

#include "dual_devices.hpp"
#include "frame_transfer.hpp"
#include "fsr_pass.hpp"
#include "gpu_image.hpp"

#ifndef VK_LAYER_EXPORT
#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define VK_LAYER_EXPORT
#endif
#endif

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL xfsr_GetInstanceProcAddr(VkInstance instance,
                                                                  const char* pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL xfsr_GetDeviceProcAddr(VkDevice device, const char* pName);
}

namespace {

// When creating the companion iGPU device via volk, skip our CreateDevice hook
// (pNext has no loader link info — would return INITIALIZATION_FAILED).
thread_local bool g_bypassCreateDevice = false;
// Prevent volk → our CreateSwapchain re-entry (CS2 stack overflow).
thread_local int g_inLayerSwapchain = 0;
// When true, surface-caps hook returns the real ICD extent (no shrink). Needed so
// CreateSwapchain can read the true window size — a re-entrant/shrunk query makes
// displayW==renderW and the low-res buffer sits in a corner of the window.
thread_local int g_bypassCapsShrink = 0;

struct WsiFns {
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
};

const char* xfsrEnv(const char* name, const char* legacy = nullptr) {
    if (const char* e = std::getenv(name); e && e[0]) return e;
    if (legacy) {
        if (const char* e = std::getenv(legacy); e && e[0]) return e;
    }
    return nullptr;
}

bool verboseLog() {
    const char* v = std::getenv("XFSR_VERBOSE");
    return v && v[0] != '\0' && v[0] != '0';
}

void logv(const char* fmt, ...) {
    if (!verboseLog()) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

float presetScaleFromEnv() {
    const char* p = xfsrEnv("XFSR_PRESET", "CROSSVULKAN_PRESET_MODE");
    if (!p) return 1.5f;
    if (std::strcmp(p, "balanced") == 0) return 1.7f;
    if (std::strcmp(p, "performance") == 0 || std::strcmp(p, "perf") == 0) return 2.0f;
    return 1.5f;
}

// Wayland often reports currentExtent=UINT_MAX until the first swapchain exists, so
// caps-shrink never runs and benches stay identity @ full window (flat FPS). Hint the
// real window via XFSR_DISPLAY_EXTENT=WxH (xfsr-run --output) or XFSR_OUTPUT_*.
void applyDisplayExtentHint(uint32_t* outW, uint32_t* outH) {
    if (!outW || !outH) return;
    const char* e = xfsrEnv("XFSR_DISPLAY_EXTENT", "CROSSVULKAN_DISPLAY_EXTENT");
    unsigned w = 0, h = 0;
    if (e && e[0] && std::sscanf(e, "%ux%u", &w, &h) == 2 && w > 0 && h > 0) {
        *outW = w;
        *outH = h;
        return;
    }
    const char* ew = std::getenv("XFSR_OUTPUT_W");
    const char* eh = std::getenv("XFSR_OUTPUT_H");
    if (ew && eh) {
        w = static_cast<unsigned>(std::atoi(ew));
        h = static_cast<unsigned>(std::atoi(eh));
        if (w > 0 && h > 0) {
            *outW = w;
            *outH = h;
        }
    }
}

bool fsrEnabled() {
    const char* f = xfsrEnv("XFSR_FSR", "CROSSVULKAN_FSR");
    if (f && f[0] == '0') return false;
    return true;
}

// Spatial FSR1 upscale (EASU/RCAS). Default on when XFSR_FSR≠0.
// Override: XFSR_LAYER_FSR=0|1
bool layerFsrPassEnabled() {
    const char* o = xfsrEnv("XFSR_LAYER_FSR", "CROSSVULKAN_LAYER_FSR");
    if (o && o[0] == '0') return false;
    if (o && o[0] == '1') return true;
    return fsrEnabled();
}

bool offloadEnabled() {
    // Default OFF: dual-GPU DMA crashes/slows many titles (CS2). Opt in with
    // XFSR_OFFLOAD=1 or xfsr-run --offload.
    const char* o = xfsrEnv("XFSR_OFFLOAD", "CROSSVULKAN_OFFLOAD");
    if (!o || !o[0]) return false;
    return o[0] != '0';
}

// When the app ignores caps shrink (DXVK/Proton often does), force render extent in
// CreateSwapchain. Default ON when FSR is enabled. XFSR_FORCE_SCALE=0 disables.
bool forceScaleEnabled() {
    const char* o = xfsrEnv("XFSR_FORCE_SCALE", "CROSSVULKAN_FORCE_SCALE");
    if (o && o[0] == '0') return false;
    if (o && o[0] == '1') return true;
    return fsrEnabled();
}

// OpenGL via Zink: Mesa sizes the EGL drawable from the Vulkan swapchain extent.
// Caps-shrink → Zink creates @ render res → drawable becomes a postage-stamp inside
// the still-large GL window. Detect Zink and skip shrink; force-scale on CreateSwapchain.
bool zinkActive() {
    const char* a = std::getenv("MESA_LOADER_DRIVER_OVERRIDE");
    const char* b = std::getenv("GALLIUM_DRIVER");
    auto isZink = [](const char* s) {
        return s && (std::strcmp(s, "zink") == 0 || std::strncmp(s, "zink,", 5) == 0);
    };
    return isZink(a) || isZink(b);
}

void computeRenderSize(uint32_t displayW, uint32_t displayH, float scale, uint32_t* outW,
                       uint32_t* outH) {
    uint32_t rw = std::max(64u, static_cast<uint32_t>(displayW / scale + 0.5f));
    uint32_t rh = std::max(64u, static_cast<uint32_t>(displayH / scale + 0.5f));
    if (displayW == 1920 && displayH == 1080) {
        if (scale >= 1.9f) {
            rw = 960;
            rh = 540;
        } else if (scale >= 1.6f) {
            rw = 1129;
            rh = 635;
        } else {
            rw = 1280;
            rh = 720;
        }
    }
    *outW = rw;
    *outH = rh;
}

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    // From CreateInstance pNext — required to create companion devices from a layer.
    PFN_vkLayerCreateDevice layerCreateDevice = nullptr;
    PFN_vkLayerDestroyDevice layerDestroyDevice = nullptr;
    float scale = 1.5f;
    uint32_t realW = 0;
    uint32_t realH = 0;
    bool capsShrunk = false; // true when we rewrote currentExtent to render res
    bool volkReady = false;
    // Dedupe caps-shrink logs (Zink queries every frame).
    uint32_t lastLogCw = 0, lastLogCh = 0, lastLogRw = 0, lastLogRh = 0;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    InstanceData* inst = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;

    bool isDiscrete = false;
    uint32_t graphicsFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;

    DeviceContext dgpu{}; // adopt()'d app device
    bool dgpuReady = false;

    DeviceContext igpu{}; // companion present/FSR device
    bool igpuReady = false;
    bool igpuFailed = false; // do not retry companion every recreate

    FrameTransfer xfer{};
    bool xferReady = false;
    // Zink+offload: FSR result iGPU → dGPU (present stays on dGPU WSI).
    FrameTransfer xferBack{};
    bool xferBackReady = false;

    // Stable storage if we inject device extensions into CreateDevice.
    std::vector<std::string> injectedExtNames;
    std::vector<const char*> injectedExtPtrs;

    // Dedupe "ready" / first-present logs across swapchain recreates.
    uint32_t loggedRw = 0, loggedRh = 0, loggedDw = 0, loggedDh = 0;
    bool loggedFirstPresent = false;
};

struct ProxySwapchain {
    VkSwapchainKHR handle = VK_NULL_HANDLE; // fake = pointer cast of this
    DeviceData* dev = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    uint32_t displayW = 0;
    uint32_t displayH = 0;
    uint32_t renderW = 0;
    uint32_t renderH = 0;
    // Must match the app's VkSwapchainCreateInfoKHR — wrong format ⇒ magenta/swizzled junk.
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    uint32_t minImageCount = 3;
    VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkSurfaceTransformFlagBitsKHR preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkBool32 clipped = VK_TRUE;

    // Present WSI swapchain (display res) on the present device (dGPU or iGPU).
    VkSwapchainKHR realSwapchain = VK_NULL_HANDLE;
    std::vector<VkImage> realImages;
    std::vector<VkImageView> realViews; // for FSR STORAGE → swapchain (when format allows)
    bool fsrDirectToSwapchain = false;
    bool fsrDirectSwizzleBgra = false;
    // App-facing handle MUST be on the *app* (dGPU) device. Source2 leaks some WSI
    // calls past the layer into the NVIDIA ICD — an iGPU handle there SIGSEGVs.
    // Same-GPU: appSwapchain == realSwapchain. Dual-GPU: opaqueAppHandle (pointer cast)
    // so we never attach a second WSI to the window surface (SURFACE_LOST / OUT_OF_DATE).
    VkSwapchainKHR appSwapchain = VK_NULL_HANDLE;
    bool opaqueAppHandle = false;

    // 2-slot present ring: blit can overlap the next app frame (no QueueWaitIdle).
    static constexpr uint32_t kFrames = 2;
    VkCommandBuffer cmds[kFrames]{};
    VkSemaphore imageAvailable[kFrames]{};
    VkSemaphore renderFinished[kFrames]{};
    VkFence inFlight[kFrames]{};
    uint32_t frameSlot = 0;

    FsrPass fsr{};
    bool pipelineReady = false;
    bool offloadIgpu = false; // true: FrameTransfer → iGPU (FSR and/or present)
    // Zink/NVIDIA GLX SIGSEGVs if the window WSI is on the iGPU (opaque app handle).
    // Keep present on dGPU; still FSR on iGPU via host round-trip.
    bool zinkOffloadRoundtrip = false;
    bool useFsr = false;      // FSR1 EASU/RCAS when XFSR_FSR=1 and scaling

    std::vector<GpuImage> appImages;
    uint32_t acquireIndex = 0;
    uint32_t presentCount = 0;
};

std::mutex g_mu;
std::unordered_map<void*, InstanceData*> g_instances;
std::unordered_map<void*, DeviceData*> g_devices;
std::unordered_map<void*, ProxySwapchain*> g_proxies;
// Companion VkDevices are created via loader callback (not in g_devices). volkLoadDevice
// still goes through our GDPA — forward to the ICD/loader GDPA saved at create time.
std::unordered_map<void*, PFN_vkGetDeviceProcAddr> g_companionGdpa;

void* keyOf(VkInstance i) { return reinterpret_cast<void*>(i); }
void* keyOf(VkDevice d) { return reinterpret_cast<void*>(d); }
void* keyOf(VkSwapchainKHR s) { return reinterpret_cast<void*>(s); }

ProxySwapchain* proxyOf(VkSwapchainKHR s) {
    auto it = g_proxies.find(keyOf(s));
    return it == g_proxies.end() ? nullptr : it->second;
}

// Present WSI lives on iGPU only for classic dual-GPU (non-Zink). Zink round-trip keeps dGPU WSI.
bool presentsOnIgpu(const ProxySwapchain* ps) {
    return ps && ps->offloadIgpu && !ps->zinkOffloadRoundtrip;
}

template <typename T>
T getInstProc(InstanceData* id, const char* name) {
    return reinterpret_cast<T>(id->gipa(id->instance, name));
}

template <typename T>
T getDevProc(DeviceData* dd, const char* name) {
    return reinterpret_cast<T>(dd->gdpa(dd->device, name));
}

// Next-layer (dGPU) or companion ICD (iGPU) WSI entry points — never our hooks.
WsiFns wsiFnsFor(DeviceData* dd, bool offloadIgpu) {
    WsiFns f{};
    if (offloadIgpu && dd && dd->igpu.device && dd->igpu.deviceGdpa) {
        auto gdpa = dd->igpu.deviceGdpa;
        VkDevice dev = dd->igpu.device;
        f.CreateSwapchainKHR =
            reinterpret_cast<PFN_vkCreateSwapchainKHR>(gdpa(dev, "vkCreateSwapchainKHR"));
        f.DestroySwapchainKHR =
            reinterpret_cast<PFN_vkDestroySwapchainKHR>(gdpa(dev, "vkDestroySwapchainKHR"));
        f.GetSwapchainImagesKHR =
            reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(gdpa(dev, "vkGetSwapchainImagesKHR"));
        f.AcquireNextImageKHR =
            reinterpret_cast<PFN_vkAcquireNextImageKHR>(gdpa(dev, "vkAcquireNextImageKHR"));
        f.QueuePresentKHR =
            reinterpret_cast<PFN_vkQueuePresentKHR>(gdpa(dev, "vkQueuePresentKHR"));
        return f;
    }
    if (dd) {
        f.CreateSwapchainKHR = dd->CreateSwapchainKHR;
        f.DestroySwapchainKHR = dd->DestroySwapchainKHR;
        f.GetSwapchainImagesKHR = dd->GetSwapchainImagesKHR;
        f.AcquireNextImageKHR = dd->AcquireNextImageKHR;
        f.QueuePresentKHR = dd->QueuePresentKHR;
    }
    return f;
}

void destroyPresentRing(ProxySwapchain* ps, DeviceContext& ctx) {
    if (!ps || !ctx.device) return;
    ctx.loadDispatch();
    auto& d = ctx.vk;
    if (ctx.cmdPool && ps->cmds[0] && d.FreeCommandBuffers) {
        d.FreeCommandBuffers(ctx.device, ctx.cmdPool, ProxySwapchain::kFrames, ps->cmds);
    }
    for (uint32_t i = 0; i < ProxySwapchain::kFrames; ++i) {
        ps->cmds[i] = VK_NULL_HANDLE;
        if (ps->inFlight[i] && d.DestroyFence) d.DestroyFence(ctx.device, ps->inFlight[i], nullptr);
        if (ps->imageAvailable[i] && d.DestroySemaphore)
            d.DestroySemaphore(ctx.device, ps->imageAvailable[i], nullptr);
        if (ps->renderFinished[i] && d.DestroySemaphore)
            d.DestroySemaphore(ctx.device, ps->renderFinished[i], nullptr);
        ps->inFlight[i] = VK_NULL_HANDLE;
        ps->imageAvailable[i] = VK_NULL_HANDLE;
        ps->renderFinished[i] = VK_NULL_HANDLE;
    }
    for (VkImageView v : ps->realViews) {
        if (v && d.DestroyImageView) d.DestroyImageView(ctx.device, v, nullptr);
    }
    ps->realViews.clear();
    ps->fsrDirectToSwapchain = false;
}

void destroyProxy(ProxySwapchain* ps) {
    if (!ps) return;
    DeviceData* dd = ps->dev;
    if (ps->pipelineReady) {
        // Avoid FsrPass::destroy → DeviceWaitIdle on every swapchain recreate (CS2 was
        // getting SUBOPTIMAL each frame → recreate → waitIdle ≈ single-digit FPS).
        if (dd && dd->dgpuReady && dd->QueueWaitIdle && dd->graphicsQueue)
            dd->QueueWaitIdle(dd->graphicsQueue);
        ps->fsr.destroy();
        ps->pipelineReady = false;
    }
    for (auto& img : ps->appImages) img.destroy();
    ps->appImages.clear();
    if (dd) {
        DeviceContext& presentCtx = presentsOnIgpu(ps) ? dd->igpu : dd->dgpu;
        if (ps->offloadIgpu && dd->igpuReady) {
            if (dd->igpu.graphicsQueue) {
                dd->igpu.loadDispatch();
                if (dd->igpu.vk.QueueWaitIdle) dd->igpu.vk.QueueWaitIdle(dd->igpu.graphicsQueue);
            }
        }
        if (dd->QueueWaitIdle && dd->graphicsQueue) dd->QueueWaitIdle(dd->graphicsQueue);

        destroyPresentRing(ps, presentCtx);
        if (ps->realSwapchain) {
            WsiFns wsi = wsiFnsFor(dd, presentsOnIgpu(ps));
            if (wsi.DestroySwapchainKHR)
                wsi.DestroySwapchainKHR(presentCtx.device, ps->realSwapchain, nullptr);
        }
        // Dual-GPU may use an opaque app handle (not an ICD swapchain).
        if (ps->appSwapchain && ps->appSwapchain != ps->realSwapchain && !ps->opaqueAppHandle) {
            WsiFns dwsi = wsiFnsFor(dd, false);
            if (dwsi.DestroySwapchainKHR)
                dwsi.DestroySwapchainKHR(dd->dgpu.device, ps->appSwapchain, nullptr);
        }
    }
    ps->realSwapchain = VK_NULL_HANDLE;
    ps->appSwapchain = VK_NULL_HANDLE;
    ps->realImages.clear();
    ps->offloadIgpu = false;
    ps->zinkOffloadRoundtrip = false;
    ps->useFsr = false;
    delete ps;
}

bool tryInitCompanionIgpu(DeviceData* dd, VkSurfaceKHR surface) {
    if (!dd || !dd->inst) return false;
    if (dd->igpuReady) return true;
    if (dd->igpuFailed) return false;

    InstanceData* id = dd->inst;
    if (!id->layerCreateDevice || !id->layerDestroyDevice) {
        dd->igpuFailed = true;
        logv("[xfsr] iGPU companion unavailable — loader CreateDevice callback missing\n");
        return false;
    }

    // Enumerate candidates with diagnostics (hybrid WSI often blocks iGPU present).
    uint32_t count = 0;
    id->EnumeratePhysicalDevices(id->instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (count) id->EnumeratePhysicalDevices(id->instance, &count, devices.data());
    logv("[xfsr] companion probe: %u physical device(s)\n", count);

    DualDevices dual{};
    // Bypass our CreateDevice hook if the loader still routes through the layer GIPA.
    g_bypassCreateDevice = true;
    bool ok = false;
    try {
        for (auto phys : devices) {
            VkPhysicalDeviceProperties props{};
            id->GetPhysicalDeviceProperties(phys, &props);
            const bool isIgpu =
                props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                (props.vendorID == 0x1002 &&
                 props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
            if (!isIgpu) {
                logv("[xfsr] companion skip %s (type=%u)\n", props.deviceName,
                     static_cast<unsigned>(props.deviceType));
                continue;
            }
            dual.integrated.physical = phys;
            dual.integrated.props = props;
            vkGetPhysicalDeviceMemoryProperties(phys, &dual.integrated.memProps);
            dual.integrated.timestampPeriod = props.limits.timestampPeriod;
            try {
                // Pass next-layer GIPA so the companion device chain skips this layer.
                dual.integrated.createLogicalViaLoader(id->instance, surface, id->layerCreateDevice,
                                                       id->gipa, id->layerDestroyDevice);
            } catch (const std::exception& e) {
                logv("[xfsr] companion createLogical failed on %s: %s\n", props.deviceName,
                     e.what());
                dual.integrated = {};
                continue;
            }
            if (!dual.integrated.canPresent) {
                logv("[xfsr] companion %s: no present support on this surface — "
                     "same-GPU blit fallback\n",
                     props.deviceName);
                dual.integrated.destroy();
                dual.integrated = {};
                continue;
            }
            dual.presentDevice = &dual.integrated;
            ok = true;
            break;
        }
    } catch (const std::exception& e) {
        logv("[xfsr] companion exception: %s\n", e.what());
        ok = false;
    }
    g_bypassCreateDevice = false;
    if (!ok || !dual.integrated.device) {
        dd->igpuFailed = true;
        logv("[xfsr] iGPU companion unavailable — same-GPU blit fallback\n");
        return false;
    }
    dd->igpu = dual.integrated;
    dual.integrated = {};
    dual.presentDevice = nullptr;
    dd->igpuReady = true;
    if (dd->igpu.device && dd->igpu.deviceGdpa) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_companionGdpa[keyOf(dd->igpu.device)] = dd->igpu.deviceGdpa;
    }
    logv("[xfsr] iGPU companion ready: %s\n", dd->igpu.props.deviceName);
    return true;
}

bool createPresentRing(ProxySwapchain* ps, DeviceContext& ctx) {
    ctx.loadDispatch();
    if (!ctx.cmdPool || !ctx.vk.AllocateCommandBuffers) return false;
    auto& d = ctx.vk;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = ctx.cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = ProxySwapchain::kFrames;
    vkCheck(d.AllocateCommandBuffers(ctx.device, &cai, ps->cmds), "alloc present cmds");

    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < ProxySwapchain::kFrames; ++i) {
        vkCheck(d.CreateSemaphore(ctx.device, &semci, nullptr, &ps->imageAvailable[i]), "sem");
        vkCheck(d.CreateSemaphore(ctx.device, &semci, nullptr, &ps->renderFinished[i]), "sem2");
        vkCheck(d.CreateFence(ctx.device, &fci, nullptr, &ps->inFlight[i]), "fence");
    }
    ps->frameSlot = 0;
    return true;
}

// Forward decl — used by setupOffloadPipeline (defined with CreateSwapchain helpers).
bool queryTrueSurfaceExtent(DeviceData* dd, VkSurfaceKHR surface, uint32_t* outW, uint32_t* outH);

bool setupOffloadPipeline(ProxySwapchain* ps, VkSwapchainKHR oldPresentSwapchain,
                          VkSwapchainKHR oldAppSwapchain) {
    DeviceData* dd = ps->dev;
    if (!dd || !dd->dgpuReady || !dd->inst || !dd->inst->volkReady) return false;
    // Proxy + FSR path is driven by XFSR_FSR; OFFLOAD only selects dual-GPU.
    if (!fsrEnabled()) return false;
    if (ps->renderW == 0 || ps->renderH == 0 || ps->displayW == 0 || ps->displayH == 0) return false;
    if (ps->format == VK_FORMAT_UNDEFINED) return false;

    try {
        const bool scaling = ps->renderW != ps->displayW || ps->renderH != ps->displayH;
        // Dual-GPU DMA only pays off when we actually render lower-res. Full-res
        // transfer+present on Vega is pure overhead vs native dGPU present.
        if (scaling && offloadEnabled()) {
            ps->offloadIgpu = tryInitCompanionIgpu(dd, ps->surface);
        } else {
            ps->offloadIgpu = false;
            if (!scaling && verboseLog() &&
                (dd->loggedRw != ps->renderW || dd->loggedRh != ps->renderH ||
                 dd->loggedDw != ps->displayW || dd->loggedDh != ps->displayH)) {
                logv("[xfsr] no scale (render==display) — skip dual-GPU transfer\n");
            }
        }
        // Zink + NVIDIA GLX cannot survive an iGPU-owned window WSI. Keep present on
        // the dGPU; still run FSR on the iGPU via dGPU→iGPU→dGPU round-trip.
        ps->zinkOffloadRoundtrip = ps->offloadIgpu && zinkActive();
        if (ps->zinkOffloadRoundtrip) {
            logv("[xfsr] Zink+offload: dGPU WSI + iGPU FSR (round-trip)\n");
        }
        DeviceContext* presentCtx =
            presentsOnIgpu(ps) ? &dd->igpu : &dd->dgpu;

        if (ps->offloadIgpu) {
            if (!dd->xferReady) {
                dd->xfer.init(&dd->dgpu, &dd->igpu);
                dd->xferReady = true;
            }
            dd->xfer.resize(ps->renderW, ps->renderH, ps->format, 4);
            if (ps->zinkOffloadRoundtrip) {
                if (!dd->xferBackReady) {
                    dd->xferBack.init(&dd->igpu, &dd->dgpu);
                    dd->xferBackReady = true;
                }
                // FSR output is always RGBA8 UNORM at display size.
                dd->xferBack.resize(ps->displayW, ps->displayH, VK_FORMAT_R8G8B8A8_UNORM, 4);
            }
        }

        // Any caps query during WSI create must see the true window extent.
        struct CapsBypass {
            CapsBypass() { ++g_bypassCapsShrink; }
            ~CapsBypass() { --g_bypassCapsShrink; }
        } capsBypass;

        auto createWsi = [&](DeviceContext* ctx, bool igpuWsi, VkExtent2D extent,
                             VkImageUsageFlags usage, VkSwapchainKHR oldSc, VkSwapchainKHR* out)
            -> VkResult {
            ctx->loadDispatch();
            WsiFns wsi = wsiFnsFor(dd, igpuWsi);
            if (!wsi.CreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;
            VkSwapchainCreateInfoKHR sci{};
            sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            sci.surface = ps->surface;
            sci.minImageCount = std::max(2u, ps->minImageCount);
            sci.imageFormat = ps->format;
            sci.imageColorSpace = ps->colorSpace;
            sci.imageExtent = extent;
            sci.imageArrayLayers = 1;
            sci.imageUsage = usage;
            sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            sci.preTransform = ps->preTransform;
            sci.compositeAlpha = ps->compositeAlpha;
            sci.presentMode = ps->presentMode;
            sci.clipped = ps->clipped;
            sci.oldSwapchain = oldSc;
            VkResult cr = wsi.CreateSwapchainKHR(ctx->device, &sci, nullptr, out);
            if (cr != VK_SUCCESS && ps->presentMode != VK_PRESENT_MODE_FIFO_KHR) {
                sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
                cr = wsi.CreateSwapchainKHR(ctx->device, &sci, nullptr, out);
            }
            return cr;
        };

        // Present WSI must be the real window size — never the shrunk render extent.
        // Creating a WSI at render size first (old dual-GPU shadow order) resized the
        // native window to 400x300 while glmark2 still thought 800x600 → postage-stamp.
        uint32_t presentW = ps->displayW, presentH = ps->displayH;
        uint32_t trueW = 0, trueH = 0;
        if (queryTrueSurfaceExtent(dd, ps->surface, &trueW, &trueH)) {
            if (presentW < trueW || presentH < trueH) {
                std::fprintf(stderr,
                             "[xfsr] bump present extent %ux%u → %ux%u (avoid postage-stamp)\n",
                             presentW, presentH, trueW, trueH);
                presentW = trueW;
                presentH = trueH;
                ps->displayW = trueW;
                ps->displayH = trueH;
            }
        }
        // Prefer STORAGE so FSR can write the swapchain directly (skip fullscreen copy).
        const VkImageUsageFlags presentUsage =
            ps->imageUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        const VkImageUsageFlags shadowUsage =
            ps->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        auto createPresentWsi = [&](DeviceContext* ctx, bool igpuWsi, VkSwapchainKHR oldSc,
                                    VkSwapchainKHR* out) -> VkResult {
            VkResult cr = createWsi(ctx, igpuWsi, {presentW, presentH}, presentUsage, oldSc, out);
            if (cr != VK_SUCCESS) {
                cr = createWsi(ctx, igpuWsi, {presentW, presentH},
                               ps->imageUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, oldSc, out);
            }
            return cr;
        };

        // Display-sized WSI only. Classic dual-GPU: iGPU owns the window; app handle is
        // opaque (no second WSI). Zink round-trip: dGPU owns the window (real ICD handle).
        (void)shadowUsage;
        (void)oldAppSwapchain;
        VkResult cr = VK_ERROR_INITIALIZATION_FAILED;
        if (presentsOnIgpu(ps)) {
            cr = createPresentWsi(&dd->igpu, true, oldPresentSwapchain, &ps->realSwapchain);
            if (cr != VK_SUCCESS) {
                logv("[xfsr] iGPU present swapchain failed: %d (fmt=%u) — same-GPU fallback\n",
                     static_cast<int>(cr), static_cast<unsigned>(ps->format));
                ps->offloadIgpu = false;
                ps->zinkOffloadRoundtrip = false;
            } else {
                presentCtx = &dd->igpu;
                ps->opaqueAppHandle = true;
                ps->appSwapchain = VK_NULL_HANDLE; // set to opaque pointer after ProxySwapchain*
                cr = VK_SUCCESS;
            }
        }

        if (!presentsOnIgpu(ps)) {
            presentCtx = &dd->dgpu;
            ps->opaqueAppHandle = false;
            cr = createPresentWsi(&dd->dgpu, false,
                                 oldPresentSwapchain ? oldPresentSwapchain : oldAppSwapchain,
                                 &ps->realSwapchain);
            if (cr != VK_SUCCESS) return false;
            ps->appSwapchain = ps->realSwapchain;
        }

        WsiFns wsi = wsiFnsFor(dd, presentsOnIgpu(ps));
        if (!wsi.GetSwapchainImagesKHR) return false;
        uint32_t n = 0;
        wsi.GetSwapchainImagesKHR(presentCtx->device, ps->realSwapchain, &n, nullptr);
        ps->realImages.resize(n);
        wsi.GetSwapchainImagesKHR(presentCtx->device, ps->realSwapchain, &n, ps->realImages.data());

        if (!createPresentRing(ps, *presentCtx)) return false;

        dd->dgpu.loadDispatch();
        const VkImageUsageFlags appUsage =
            (ps->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        ps->appImages.resize(std::max(2u, n));
        for (size_t i = 0; i < ps->appImages.size(); ++i) {
            ps->appImages[i] =
                createImage(&dd->dgpu, ps->renderW, ps->renderH, ps->format, appUsage,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, true);
        }

        ps->useFsr = scaling && layerFsrPassEnabled();
        ps->fsrDirectToSwapchain = false;
        ps->fsrDirectSwizzleBgra = false;
        if (ps->useFsr) {
            DeviceContext* fsrCtx = ps->offloadIgpu ? &dd->igpu : &dd->dgpu;
            fsrCtx->loadDispatch();
            ps->fsr.init(fsrCtx);
            ps->fsr.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
            // Round-trip FSR writes an iGPU image; cannot STORAGE into the dGPU swapchain.
            if (!ps->zinkOffloadRoundtrip && ps->fsr.canDirectToSwapchain(ps->format)) {
                ps->realViews.resize(ps->realImages.size());
                for (size_t i = 0; i < ps->realImages.size(); ++i) {
                    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                    vci.image = ps->realImages[i];
                    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    vci.format = ps->format;
                    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    vci.subresourceRange.levelCount = 1;
                    vci.subresourceRange.layerCount = 1;
                    if (presentCtx->vk.CreateImageView(presentCtx->device, &vci, nullptr,
                                                       &ps->realViews[i]) == VK_SUCCESS) {
                        ps->fsrDirectToSwapchain = true;
                    } else {
                        ps->fsrDirectToSwapchain = false;
                        break;
                    }
                }
                if (!ps->fsrDirectToSwapchain) {
                    for (VkImageView v : ps->realViews) {
                        if (v && presentCtx->vk.DestroyImageView)
                            presentCtx->vk.DestroyImageView(presentCtx->device, v, nullptr);
                    }
                    ps->realViews.clear();
                } else {
                    ps->fsrDirectSwizzleBgra = FsrPass::formatNeedsBgraSwizzle(ps->format);
                    logv("[xfsr] FSR → swapchain direct (no fullscreen copy), fmt=%u swizzleBGRA=%d "
                         "rcas=%d\n",
                         static_cast<unsigned>(ps->format), ps->fsrDirectSwizzleBgra ? 1 : 0,
                         ps->fsr.enableRcas ? 1 : 0);
                }
            }
        }

        const bool cfgChanged = dd->loggedRw != ps->renderW || dd->loggedRh != ps->renderH ||
                                dd->loggedDw != ps->displayW || dd->loggedDh != ps->displayH;
        if (cfgChanged) {
            dd->loggedRw = ps->renderW;
            dd->loggedRh = ps->renderH;
            dd->loggedDw = ps->displayW;
            dd->loggedDh = ps->displayH;
            dd->loggedFirstPresent = false;
            if (ps->zinkOffloadRoundtrip) {
                logv("[xfsr] dual-GPU ready: render %ux%u on %s → FSR on %s → present on %s "
                     "%ux%u fmt=%u%s\n",
                     ps->renderW, ps->renderH, dd->dgpu.props.deviceName, dd->igpu.props.deviceName,
                     dd->dgpu.props.deviceName, ps->displayW, ps->displayH,
                     static_cast<unsigned>(ps->format), ps->useFsr ? " (FSR1 round-trip)" : "");
            } else if (ps->offloadIgpu) {
                logv("[xfsr] dual-GPU ready: render %ux%u on %s → %s present %ux%u fmt=%u%s\n",
                     ps->renderW, ps->renderH, dd->dgpu.props.deviceName, dd->igpu.props.deviceName,
                     ps->displayW, ps->displayH, static_cast<unsigned>(ps->format),
                     ps->useFsr ? " (FSR1)" : " (blit)");
            } else if (scaling) {
                logv("[xfsr] same-GPU blit ready: render %ux%u → display %ux%u fmt=%u on %s%s\n",
                     ps->renderW, ps->renderH, ps->displayW, ps->displayH,
                     static_cast<unsigned>(ps->format), dd->dgpu.props.deviceName,
                     ps->useFsr ? " (FSR1)" : "");
            } else {
                logv("[xfsr] identity present ready: %ux%u fmt=%u on %s\n", ps->displayW, ps->displayH,
                     static_cast<unsigned>(ps->format), dd->dgpu.props.deviceName);
            }
        }
        ps->pipelineReady = true;
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xfsr] offload setup failed: %s\n", e.what());
        DeviceContext* presentCtx = presentsOnIgpu(ps) ? &dd->igpu : &dd->dgpu;
        destroyPresentRing(ps, *presentCtx);
        if (ps->realSwapchain) {
            WsiFns wsi = wsiFnsFor(dd, presentsOnIgpu(ps));
            if (wsi.DestroySwapchainKHR)
                wsi.DestroySwapchainKHR(presentCtx->device, ps->realSwapchain, nullptr);
            ps->realSwapchain = VK_NULL_HANDLE;
        }
        if (ps->appSwapchain && ps->appSwapchain != ps->realSwapchain) {
            WsiFns dwsi = wsiFnsFor(dd, false);
            if (dwsi.DestroySwapchainKHR)
                dwsi.DestroySwapchainKHR(dd->dgpu.device, ps->appSwapchain, nullptr);
        }
        ps->appSwapchain = VK_NULL_HANDLE;
        for (auto& img : ps->appImages) img.destroy();
        ps->appImages.clear();
        ps->offloadIgpu = false;
        ps->zinkOffloadRoundtrip = false;
        return false;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkInstance* pInstance) {
    PFN_vkLayerCreateDevice layerCreateDevice = nullptr;
    PFN_vkLayerDestroyDevice layerDestroyDevice = nullptr;
    auto* layerCreateInfo = const_cast<VkLayerInstanceCreateInfo*>(
        reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext));
    VkLayerInstanceCreateInfo* linkInfo = nullptr;
    while (layerCreateInfo) {
        if (layerCreateInfo->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            if (layerCreateInfo->function == VK_LAYER_LINK_INFO) {
                linkInfo = layerCreateInfo;
            } else if (layerCreateInfo->function == VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK) {
                layerCreateDevice = layerCreateInfo->u.layerDevice.pfnLayerCreateDevice;
                layerDestroyDevice = layerCreateInfo->u.layerDevice.pfnLayerDestroyDevice;
            }
        }
        layerCreateInfo = const_cast<VkLayerInstanceCreateInfo*>(
            reinterpret_cast<const VkLayerInstanceCreateInfo*>(layerCreateInfo->pNext));
    }
    if (!linkInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    linkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

    auto create =
        reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult r = create(pCreateInfo, pAllocator, pInstance);
    if (r != VK_SUCCESS) return r;

    auto* id = new InstanceData();
    id->instance = *pInstance;
    id->gipa = gipa;
    id->layerCreateDevice = layerCreateDevice;
    id->layerDestroyDevice = layerDestroyDevice;
    id->scale = presetScaleFromEnv();
    applyDisplayExtentHint(&id->realW, &id->realH);
    id->DestroyInstance = getInstProc<PFN_vkDestroyInstance>(id, "vkDestroyInstance");
    id->EnumeratePhysicalDevices =
        getInstProc<PFN_vkEnumeratePhysicalDevices>(id, "vkEnumeratePhysicalDevices");
    id->GetPhysicalDeviceProperties =
        getInstProc<PFN_vkGetPhysicalDeviceProperties>(id, "vkGetPhysicalDeviceProperties");
    id->GetPhysicalDeviceQueueFamilyProperties =
        getInstProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            id, "vkGetPhysicalDeviceQueueFamilyProperties");
    id->GetPhysicalDeviceSurfaceCapabilitiesKHR =
        getInstProc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            id, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    id->CreateDevice = getInstProc<PFN_vkCreateDevice>(id, "vkCreateDevice");

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_instances[keyOf(*pInstance)] = id;
    }

    // Point volk at our GIPA (loader handles), not raw ICD — otherwise physdev handles mismatch.
    if (volkInitialize() == VK_SUCCESS) {
        volk_vkGetInstanceProcAddr = xfsr_GetInstanceProcAddr;
        volkLoadInstance(*pInstance);
        id->volkReady = true;
    }

    logv("[xfsr] instance scale=%.2f (layerCreateDevice=%s)%s%ux%u\n", id->scale,
         id->layerCreateDevice ? "yes" : "no",
         (id->realW && id->realH) ? " displayHint=" : " displayHint=none ",
         id->realW, id->realH);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL xfsr_DestroyInstance(VkInstance instance,
                                                const VkAllocationCallbacks* pAllocator) {
    InstanceData* id = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_instances.find(keyOf(instance));
        if (it != g_instances.end()) {
            id = it->second;
            g_instances.erase(it);
        }
    }
    if (id) {
        id->DestroyInstance(instance, pAllocator);
        delete id;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
    InstanceData* id = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_instances.empty()) id = g_instances.begin()->second;
    }
    if (!id || !id->GetPhysicalDeviceSurfaceCapabilitiesKHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r =
        id->GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities);
    if (r != VK_SUCCESS || !fsrEnabled() || g_bypassCapsShrink > 0) return r;

    // Native Vulkan: shrink only on discrete. Zink: shrink on whatever GPU Mesa picked —
    // libxfsr_gl_shim.so remaps glViewport/glScissor from window → render size.
    if (id->GetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties props{};
        id->GetPhysicalDeviceProperties(physicalDevice, &props);
        const bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (!discrete && !zinkActive()) return r;
    }

    const uint32_t cw = pSurfaceCapabilities->currentExtent.width;
    const uint32_t ch = pSurfaceCapabilities->currentExtent.height;
    // Remember true window size before we rewrite currentExtent.
    if (cw != 0xFFFFFFFFu && ch != 0xFFFFFFFFu && cw > 0 && ch > 0) {
        id->realW = cw;
        id->realH = ch;
    }

    // Concrete currentExtent (X11 / some Wayland ICDs): shrink and remember window size.
    // Wayland UINT_MAX: once we know the window (from a prior CreateSwapchain), replace
    // UINT_MAX with the shrunk size so apps that prefer currentExtent pick render res.
    // Never use maxImageExtent as the window — it is a driver ceiling.
    uint32_t winW = 0, winH = 0;
    if (cw != 0xFFFFFFFFu && ch != 0xFFFFFFFFu && cw > 0 && ch > 0) {
        winW = cw;
        winH = ch;
    } else if (id->realW > 0 && id->realH > 0) {
        winW = id->realW;
        winH = id->realH;
    }
    if (winW > 0 && winH > 0) {
        uint32_t rw = 0, rh = 0;
        computeRenderSize(winW, winH, id->scale, &rw, &rh);
        const uint32_t drvMaxW = pSurfaceCapabilities->maxImageExtent.width;
        const uint32_t drvMaxH = pSurfaceCapabilities->maxImageExtent.height;
        pSurfaceCapabilities->currentExtent = {rw, rh};
        pSurfaceCapabilities->minImageExtent = {
            std::min(rw, pSurfaceCapabilities->minImageExtent.width),
            std::min(rh, pSurfaceCapabilities->minImageExtent.height)};
        // Report window as max (clamped to driver) — never render size (postage-stamp).
        pSurfaceCapabilities->maxImageExtent = {
            drvMaxW ? std::min(winW, drvMaxW) : winW,
            drvMaxH ? std::min(winH, drvMaxH) : winH};
        id->capsShrunk = true;
        if (winW != id->lastLogCw || winH != id->lastLogCh || rw != id->lastLogRw ||
            rh != id->lastLogRh) {
            id->lastLogCw = winW;
            id->lastLogCh = winH;
            id->lastLogRw = rw;
            id->lastLogRh = rh;
            logv("[xfsr] caps shrink current %ux%u → %ux%u (max stays %ux%u)\n", winW, winH, rw, rh,
                 winW, winH);
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_CreateDevice(VkPhysicalDevice physicalDevice,
                                                 const VkDeviceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkDevice* pDevice) {
    InstanceData* idEarly = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_instances.empty()) idEarly = g_instances.begin()->second;
    }
    if (g_bypassCreateDevice) {
        if (!idEarly || !idEarly->CreateDevice) return VK_ERROR_INITIALIZATION_FAILED;
        return idEarly->CreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }

    auto* layerCreateInfo = const_cast<VkLayerDeviceCreateInfo*>(
        reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));
    while (layerCreateInfo &&
           (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
            layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
        layerCreateInfo = const_cast<VkLayerDeviceCreateInfo*>(
            reinterpret_cast<const VkLayerDeviceCreateInfo*>(layerCreateInfo->pNext));
    }
    if (!layerCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    InstanceData* id = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_instances.empty()) id = g_instances.begin()->second;
    }

    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (id && id->CreateDevice) create = id->CreateDevice;

    VkPhysicalDeviceProperties propsEarly{};
    if (id && id->GetPhysicalDeviceProperties)
        id->GetPhysicalDeviceProperties(physicalDevice, &propsEarly);
    const bool wantHostExt =
        propsEarly.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && fsrEnabled() &&
        offloadEnabled(); // only needed for dual-GPU FrameTransfer

    VkDeviceCreateInfo ci = *pCreateInfo;
    std::vector<const char*> extPtrs;
    bool injectedHost = false;
    if (wantHostExt && pCreateInfo) {
        bool already = false;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
            extPtrs.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
            if (std::strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                            VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0)
                already = true;
        }
        if (!already) {
            // Probe support via next GIPA / volk instance dispatch.
            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> avail(extCount);
            if (extCount)
                vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount,
                                                     avail.data());
            for (const auto& e : avail) {
                if (std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) ==
                    0) {
                    extPtrs.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
                    ci.enabledExtensionCount = static_cast<uint32_t>(extPtrs.size());
                    ci.ppEnabledExtensionNames = extPtrs.data();
                    injectedHost = true;
                    break;
                }
            }
        }
    }

    VkResult r = create(physicalDevice, &ci, pAllocator, pDevice);
    if (r != VK_SUCCESS && injectedHost) {
        logv("[xfsr] CreateDevice with external_memory_host failed (%d); retrying without\n",
             static_cast<int>(r));
        r = create(physicalDevice, pCreateInfo, pAllocator, pDevice);
        injectedHost = false;
    }
    if (r != VK_SUCCESS) return r;

    auto* dd = new DeviceData();
    dd->device = *pDevice;
    dd->physical = physicalDevice;
    dd->inst = id;
    dd->gdpa = gdpa;
    if (injectedHost) {
        dd->injectedExtNames.emplace_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        logv("[xfsr] injected %s on discrete device\n", VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    }
    dd->DestroyDevice = getDevProc<PFN_vkDestroyDevice>(dd, "vkDestroyDevice");
    dd->CreateSwapchainKHR = getDevProc<PFN_vkCreateSwapchainKHR>(dd, "vkCreateSwapchainKHR");
    dd->DestroySwapchainKHR = getDevProc<PFN_vkDestroySwapchainKHR>(dd, "vkDestroySwapchainKHR");
    dd->GetSwapchainImagesKHR =
        getDevProc<PFN_vkGetSwapchainImagesKHR>(dd, "vkGetSwapchainImagesKHR");
    dd->AcquireNextImageKHR = getDevProc<PFN_vkAcquireNextImageKHR>(dd, "vkAcquireNextImageKHR");
    dd->QueuePresentKHR = getDevProc<PFN_vkQueuePresentKHR>(dd, "vkQueuePresentKHR");
    dd->GetDeviceQueue = getDevProc<PFN_vkGetDeviceQueue>(dd, "vkGetDeviceQueue");
    dd->DeviceWaitIdle = getDevProc<PFN_vkDeviceWaitIdle>(dd, "vkDeviceWaitIdle");
    dd->QueueWaitIdle = getDevProc<PFN_vkQueueWaitIdle>(dd, "vkQueueWaitIdle");
    dd->QueueSubmit = getDevProc<PFN_vkQueueSubmit>(dd, "vkQueueSubmit");
    dd->CreateSemaphore = getDevProc<PFN_vkCreateSemaphore>(dd, "vkCreateSemaphore");
    dd->DestroySemaphore = getDevProc<PFN_vkDestroySemaphore>(dd, "vkDestroySemaphore");
    dd->CreateFence = getDevProc<PFN_vkCreateFence>(dd, "vkCreateFence");
    dd->DestroyFence = getDevProc<PFN_vkDestroyFence>(dd, "vkDestroyFence");
    dd->WaitForFences = getDevProc<PFN_vkWaitForFences>(dd, "vkWaitForFences");
    dd->ResetFences = getDevProc<PFN_vkResetFences>(dd, "vkResetFences");

    VkPhysicalDeviceProperties props = propsEarly;
    if (id && props.deviceType == 0)
        id->GetPhysicalDeviceProperties(physicalDevice, &props);
    dd->isDiscrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

    uint32_t qCount = 0;
    if (id) id->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qCount);
    if (id) id->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, qf.data());
    for (uint32_t i = 0; i < qCount; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            dd->graphicsFamily = i;
            break;
        }
    }
    // Prefer queue family from create info
    if (pCreateInfo->queueCreateInfoCount > 0) {
        dd->graphicsFamily = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
    }
    dd->GetDeviceQueue(dd->device, dd->graphicsFamily, 0, &dd->graphicsQueue);

    // Defer DeviceContext::adopt until CreateSwapchain — calling volkLoadDevice here
    // races the app's first post-create instance queries on some loaders.

    std::lock_guard<std::mutex> lock(g_mu);
    g_devices[keyOf(*pDevice)] = dd;
    logv("[xfsr] device %s (%s)\n", props.deviceName, dd->isDiscrete ? "discrete" : "other");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL xfsr_DestroyDevice(VkDevice device,
                                              const VkAllocationCallbacks* pAllocator) {
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        // Destroy proxies on this device
        for (auto it = g_proxies.begin(); it != g_proxies.end();) {
            if (it->second->dev && it->second->dev->device == device) {
                destroyProxy(it->second);
                it = g_proxies.erase(it);
            } else {
                ++it;
            }
        }
        auto dit = g_devices.find(keyOf(device));
        if (dit != g_devices.end()) {
            dd = dit->second;
            g_devices.erase(dit);
        }
    }
    if (dd) {
        if (dd->xferReady) {
            dd->xfer.destroy();
            dd->xferReady = false;
        }
        if (dd->xferBackReady) {
            dd->xferBack.destroy();
            dd->xferBackReady = false;
        }
        if (dd->igpuReady) {
            {
                std::lock_guard<std::mutex> lock(g_mu);
                g_companionGdpa.erase(keyOf(dd->igpu.device));
            }
            dd->igpu.destroy(); // owns companion VkDevice
            dd->igpuReady = false;
        }
        if (dd->dgpuReady) {
            dd->dgpu.destroy(); // does not destroy app VkDevice
            dd->dgpuReady = false;
        }
        dd->DestroyDevice(device, pAllocator);
        delete dd;
    }
}

// Map app-facing oldSwapchain (possibly a proxy handle) to a real ICD swapchain.
VkSwapchainKHR realOldSwapchain(VkSwapchainKHR oldSwapchain) {
    if (oldSwapchain == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(g_mu);
    ProxySwapchain* oldPs = proxyOf(oldSwapchain);
    return oldPs ? oldPs->realSwapchain : oldSwapchain;
}

// True window size from the ICD (never the shrunk currentExtent). Updates inst->realW/H.
// Wayland often reports currentExtent=0xFFFFFFFF — maxImageExtent is a driver ceiling, not
// the window, so we must not treat it as the display size (that caused postage-stamps or
// runaway 2× growth when callers invented display=app*scale).
bool queryTrueSurfaceExtent(DeviceData* dd, VkSurfaceKHR surface, uint32_t* outW, uint32_t* outH) {
    if (!dd || !dd->inst || !outW || !outH) return false;
    InstanceData* id = dd->inst;
    uint32_t w = 0, h = 0;
    if (id->GetPhysicalDeviceSurfaceCapabilitiesKHR) {
        VkSurfaceCapabilitiesKHR caps{};
        ++g_bypassCapsShrink;
        const VkResult cr =
            id->GetPhysicalDeviceSurfaceCapabilitiesKHR(dd->physical, surface, &caps);
        --g_bypassCapsShrink;
        if (cr == VK_SUCCESS) {
            const uint32_t cw = caps.currentExtent.width;
            const uint32_t ch = caps.currentExtent.height;
            if (cw != 0xFFFFFFFFu && ch != 0xFFFFFFFFu && cw > 0 && ch > 0) {
                w = cw;
                h = ch;
            }
        }
    }
    if (w == 0 || h == 0) {
        // Cached from a prior concrete caps query / successful proxy.
        w = id->realW;
        h = id->realH;
    }
    if (w == 0 || h == 0) return false;
    id->realW = w;
    id->realH = h;
    *outW = w;
    *outH = h;
    return true;
}

VkResult createSwapchainPassthrough(DeviceData* dd, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                    const VkAllocationCallbacks* pAllocator,
                                    VkSwapchainKHR* pSwapchain) {
    VkSwapchainCreateInfoKHR ci = *pCreateInfo;
    ci.oldSwapchain = realOldSwapchain(pCreateInfo->oldSwapchain);
    // Always restore full window size when not proxying — postage-stamp otherwise.
    uint32_t realW = 0, realH = 0;
    if (dd && pCreateInfo && pCreateInfo->surface &&
        queryTrueSurfaceExtent(dd, pCreateInfo->surface, &realW, &realH)) {
        if (ci.imageExtent.width < realW || ci.imageExtent.height < realH) {
            std::fprintf(stderr,
                         "[xfsr] passthrough unshrink %ux%u → %ux%u (proxy unavailable)\n",
                         ci.imageExtent.width, ci.imageExtent.height, realW, realH);
            ci.imageExtent.width = realW;
            ci.imageExtent.height = realH;
        }
    }
    return dd->CreateSwapchainKHR(dd->device, &ci, pAllocator, pSwapchain);
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_CreateSwapchainKHR(VkDevice device,
                                                       const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkSwapchainKHR* pSwapchain) {
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_devices.find(keyOf(device));
        if (it != g_devices.end()) dd = it->second;
    }
    if (!dd) return VK_ERROR_INITIALIZATION_FAILED;

    // Re-entry (volk hooked CreateSwapchain on the app device) → pass through only.
    if (g_inLayerSwapchain > 0) {
        return createSwapchainPassthrough(dd, pCreateInfo, pAllocator, pSwapchain);
    }

    // Proxy + FSR. Zink needs libxfsr_gl_shim (viewport remap) via xfsr-run LD_PRELOAD.
    // Dual-GPU (--offload) always uses an opaque app handle; same-GPU Zink keeps the real
    // WSI handle + separate app images (shim fixes GL viewport).
    const bool wantProxy =
        dd->inst && dd->inst->volkReady && fsrEnabled() && pCreateInfo && pCreateInfo->surface &&
        (dd->isDiscrete || zinkActive());

    if (!wantProxy) {
        return createSwapchainPassthrough(dd, pCreateInfo, pAllocator, pSwapchain);
    }

    struct InSwapchainGuard {
        InSwapchainGuard() { ++g_inLayerSwapchain; }
        ~InSwapchainGuard() { --g_inLayerSwapchain; }
    } guard;

    if (!dd->dgpuReady) {
        try {
            dd->dgpu.adopt(dd->physical, device, dd->graphicsFamily, dd->graphicsFamily);
            dd->dgpuReady = true;
        } catch (const std::exception& e) {
            logv("[xfsr] adopt dGPU failed: %s\n", e.what());
            return createSwapchainPassthrough(dd, pCreateInfo, pAllocator, pSwapchain);
        }
    }

    auto* ps = new ProxySwapchain();
    ps->dev = dd;
    ps->surface = pCreateInfo->surface;
    ps->format = pCreateInfo->imageFormat;
    ps->colorSpace = pCreateInfo->imageColorSpace;
    ps->minImageCount = pCreateInfo->minImageCount;
    ps->imageUsage = pCreateInfo->imageUsage;
    ps->preTransform = pCreateInfo->preTransform;
    ps->compositeAlpha = pCreateInfo->compositeAlpha;
    ps->presentMode = pCreateInfo->presentMode;
    ps->clipped = pCreateInfo->clipped;

    // App sizes framebuffers to imageExtent — proxy images must match that (or DEVICE_LOST).
    // Real WSI must match the *window* size. Never invent display=app*scale: on Wayland
    // that grows the wl_surface and doubles every recreate (500→1000→2000→…).
    const uint32_t appW = pCreateInfo->imageExtent.width;
    const uint32_t appH = pCreateInfo->imageExtent.height;
    ps->renderW = appW;
    ps->renderH = appH;

    uint32_t surfW = 0, surfH = 0;
    const bool haveSurf = queryTrueSurfaceExtent(dd, pCreateInfo->surface, &surfW, &surfH);
    const float scale = dd->inst ? dd->inst->scale : 1.5f;

            if (haveSurf && (appW < surfW || appH < surfH)) {
        // App honored caps shrink (native VK or Zink+shim) — present at true window.
        ps->displayW = surfW;
        ps->displayH = surfH;
        if (zinkActive()) {
            logv("[xfsr] zink proxy: render %ux%u → display %ux%u (needs libxfsr_gl_shim)\n", appW,
                 appH, surfW, surfH);
        }
    } else if (forceScaleEnabled() && !zinkActive()) {
        // Full-window create (DXVK / apps that ignore caps): present at window size,
        // render at preset. Never invent display=app*scale.
        const uint32_t winW = haveSurf ? std::max(surfW, appW) : appW;
        const uint32_t winH = haveSurf ? std::max(surfH, appH) : appH;
        uint32_t rw = 0, rh = 0;
        computeRenderSize(winW, winH, scale, &rw, &rh);
        if (rw < winW || rh < winH) {
            ps->renderW = rw;
            ps->renderH = rh;
            ps->displayW = winW;
            ps->displayH = winH;
            if (dd->inst) {
                dd->inst->realW = winW;
                dd->inst->realH = winH;
            }
            logv("[xfsr] force-scale: app %ux%u → render %ux%u display %ux%u\n", appW, appH, rw, rh,
                 winW, winH);
        } else {
            ps->displayW = winW;
            ps->displayH = winH;
        }
    } else {
        // Unknown surface: trust app extent. Do not invent app*scale.
        ps->displayW = appW;
        ps->displayH = appH;
        if (dd->inst && appW > 0 && appH > 0) {
            dd->inst->realW = appW;
            dd->inst->realH = appH;
        }
    }

    // Identity: render == display — completely bypass the proxy (zero xfsr overhead).
    if (ps->renderW == ps->displayW && ps->renderH == ps->displayH) {
        logv("[xfsr] identity present (render==display %ux%u) — bypass proxy\n", ps->renderW,
             ps->renderH);
        delete ps;
        return createSwapchainPassthrough(dd, pCreateInfo, pAllocator, pSwapchain);
    }

    const VkSwapchainKHR oldApp = pCreateInfo->oldSwapchain;
    VkSwapchainKHR oldPresent = VK_NULL_HANDLE;
    VkSwapchainKHR oldShadow = VK_NULL_HANDLE;
    // Retire previous proxy; leave swapchain handles null so destroyProxy does not
    // double-destroy — ICDs recycle them via oldSwapchain on the creates below.
    if (oldApp != VK_NULL_HANDLE) {
        ProxySwapchain* oldPs = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_proxies.find(keyOf(oldApp));
            if (it != g_proxies.end()) {
                oldPs = it->second;
                g_proxies.erase(it);
            }
        }
        if (oldPs) {
            oldPresent = oldPs->realSwapchain;
            if (!oldPs->opaqueAppHandle && oldPs->appSwapchain &&
                oldPs->appSwapchain != oldPs->realSwapchain)
                oldShadow = oldPs->appSwapchain;
            oldPs->realSwapchain = VK_NULL_HANDLE;
            oldPs->appSwapchain = VK_NULL_HANDLE;
            oldPs->opaqueAppHandle = false;
            destroyProxy(oldPs);
        } else {
            oldPresent = oldApp; // passthrough handle
        }
    }
    (void)oldShadow;

    if (!setupOffloadPipeline(ps, oldPresent, oldShadow)) {
        std::fprintf(stderr,
                     "[xfsr] proxy setup failed (render %ux%u display %ux%u) — passthrough\n",
                     ps->renderW, ps->renderH, ps->displayW, ps->displayH);
        delete ps;
        return createSwapchainPassthrough(dd, pCreateInfo, pAllocator, pSwapchain);
    }

    // Same-GPU: real ICD swapchain handle. Dual-GPU: opaque pointer (window WSI is iGPU-only).
    if (ps->opaqueAppHandle) {
        ps->handle = reinterpret_cast<VkSwapchainKHR>(reinterpret_cast<uintptr_t>(ps));
        ps->appSwapchain = ps->handle;
    } else {
        ps->handle = ps->appSwapchain ? ps->appSwapchain : ps->realSwapchain;
    }
    *pSwapchain = ps->handle;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_proxies[keyOf(ps->handle)] = ps;
    }
    std::fprintf(stderr,
                 "[xfsr] proxy ready: handle=%p render=%ux%u display=%ux%u imgs=%zu %s%s%s\n",
                 reinterpret_cast<void*>(ps->handle), ps->renderW, ps->renderH, ps->displayW,
                 ps->displayH, ps->appImages.size(), ps->offloadIgpu ? "dual-GPU" : "same-GPU",
                 ps->useFsr ? "+FSR1" : "", ps->opaqueAppHandle ? " (opaque app handle)" : "");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL xfsr_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks* pAllocator) {
    ProxySwapchain* ps = nullptr;
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_proxies.find(keyOf(swapchain));
        if (it != g_proxies.end()) {
            ps = it->second;
            g_proxies.erase(it);
        }
        auto dit = g_devices.find(keyOf(device));
        if (dit != g_devices.end()) dd = dit->second;
    }
    if (ps) {
        destroyProxy(ps);
        return;
    }
    if (dd) dd->DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                          uint32_t* pSwapchainImageCount,
                                                          VkImage* pSwapchainImages) {
    ProxySwapchain* ps = nullptr;
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ps = proxyOf(swapchain);
        auto dit = g_devices.find(keyOf(device));
        if (dit != g_devices.end()) dd = dit->second;
    }
    if (ps && ps->pipelineReady) {
        const uint32_t n = static_cast<uint32_t>(ps->appImages.size());
        if (!pSwapchainImages) {
            *pSwapchainImageCount = n;
            return VK_SUCCESS;
        }
        const uint32_t count = std::min(*pSwapchainImageCount, n);
        for (uint32_t i = 0; i < count; ++i) pSwapchainImages[i] = ps->appImages[i].image;
        *pSwapchainImageCount = count;
        return (count < n) ? VK_INCOMPLETE : VK_SUCCESS;
    }
    if (!dd) return VK_ERROR_INITIALIZATION_FAILED;
    return dd->GetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                        uint64_t /*timeout*/, VkSemaphore semaphore,
                                                        VkFence fence, uint32_t* pImageIndex) {
    ProxySwapchain* ps = nullptr;
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ps = proxyOf(swapchain);
        auto dit = g_devices.find(keyOf(device));
        if (dit != g_devices.end()) dd = dit->second;
    }
    if (ps && ps->pipelineReady) {
        // Fake acquire: app images are always available. Must signal sync objects —
        // vkcube waits on the acquire semaphore before drawing.
        // Do NOT touch dd->dgpu.cmd here — present owns that buffer.
        ps->acquireIndex = (ps->acquireIndex + 1) % static_cast<uint32_t>(ps->appImages.size());
        *pImageIndex = ps->acquireIndex;
        if (dd && dd->QueueSubmit &&
            (semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE)) {
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            if (semaphore != VK_NULL_HANDLE) {
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = &semaphore;
            }
            VkResult sr = dd->QueueSubmit(dd->graphicsQueue, 1, &si, fence);
            if (sr != VK_SUCCESS) return sr;
        }
        return VK_SUCCESS;
    }
    if (!dd) return VK_ERROR_INITIALIZATION_FAILED;
    return dd->AcquireNextImageKHR(device, swapchain, /*timeout*/ UINT64_MAX, semaphore, fence,
                                   pImageIndex);
}

// CS2 / Source2 resolve this via GDPA; passing a proxy swapchain to the NVIDIA ICD SIGSEGVs.
VKAPI_ATTR VkResult VKAPI_CALL xfsr_AcquireNextImage2KHR(VkDevice device,
                                                         const VkAcquireNextImageInfoKHR* pInfo,
                                                         uint32_t* pImageIndex) {
    if (!pInfo || !pImageIndex) return VK_ERROR_INITIALIZATION_FAILED;
    return xfsr_AcquireNextImageKHR(device, pInfo->swapchain, pInfo->timeout, pInfo->semaphore,
                                    pInfo->fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_GetSwapchainStatusKHR(VkDevice device,
                                                          VkSwapchainKHR swapchain) {
    ProxySwapchain* ps = nullptr;
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ps = proxyOf(swapchain);
        auto dit = g_devices.find(keyOf(device));
        if (dit != g_devices.end()) dd = dit->second;
    }
    if (ps && ps->pipelineReady) return VK_SUCCESS;
    if (!dd || !dd->gdpa) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn = reinterpret_cast<PFN_vkGetSwapchainStatusKHR>(
        dd->gdpa(device, "vkGetSwapchainStatusKHR"));
    if (!fn) return VK_SUCCESS;
    return fn(device, swapchain);
}

// Source2 (CS2) resolves these; ICD + proxy swapchain handle → NVIDIA SIGSEGV.
VKAPI_ATTR VkResult VKAPI_CALL xfsr_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                      uint64_t presentId, uint64_t timeout) {
    DeviceData* dd = nullptr;
    bool isProxy = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        isProxy = proxyOf(swapchain) != nullptr;
        auto dit = g_devices.find(keyOf(device));
        if (dit != g_devices.end()) dd = dit->second;
    }
    if (isProxy) return VK_SUCCESS;
    if (!dd || !dd->gdpa) return VK_SUCCESS;
    auto fn = reinterpret_cast<PFN_vkWaitForPresentKHR>(dd->gdpa(device, "vkWaitForPresentKHR"));
    return fn ? fn(device, swapchain, presentId, timeout) : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_WaitForPresent2KHR(VkDevice /*device*/,
                                                       const void* /*pWaitInfo*/) {
    // Headers in this tree lack VkPresentWait2InfoKHR; never forward a proxy handle.
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_ReleaseSwapchainImagesEXT(
    VkDevice device, const VkReleaseSwapchainImagesInfoEXT* pInfo) {
    if (!pInfo) return VK_ERROR_INITIALIZATION_FAILED;
    ProxySwapchain* ps = nullptr;
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ps = proxyOf(pInfo->swapchain);
        auto dit = g_devices.find(keyOf(device));
        if (dit != g_devices.end()) dd = dit->second;
    }
    if (ps) return VK_SUCCESS;
    if (!dd || !dd->gdpa) return VK_SUCCESS;
    auto fn = reinterpret_cast<PFN_vkReleaseSwapchainImagesEXT>(
        dd->gdpa(device, "vkReleaseSwapchainImagesEXT"));
    if (!fn)
        fn = reinterpret_cast<PFN_vkReleaseSwapchainImagesEXT>(
            dd->gdpa(device, "vkReleaseSwapchainImagesKHR"));
    return fn ? fn(device, pInfo) : VK_SUCCESS;
}

// Zink/Mesa resolve these; forwarding an opaque proxy handle into the ICD SIGSEGVs.
static bool swapchainIsProxy(VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(g_mu);
    return proxyOf(swapchain) != nullptr;
}

static DeviceData* deviceDataOf(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto dit = g_devices.find(keyOf(device));
    return dit != g_devices.end() ? dit->second : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_GetSwapchainCounterEXT(
    VkDevice device, VkSwapchainKHR swapchain, VkSurfaceCounterFlagBitsEXT counter,
    uint64_t* pCounterValue) {
    if (swapchainIsProxy(swapchain)) {
        if (pCounterValue) *pCounterValue = 0;
        return VK_SUCCESS;
    }
    DeviceData* dd = deviceDataOf(device);
    if (!dd || !dd->gdpa) return VK_SUCCESS;
    auto fn = reinterpret_cast<PFN_vkGetSwapchainCounterEXT>(
        dd->gdpa(device, "vkGetSwapchainCounterEXT"));
    return fn ? fn(device, swapchain, counter, pCounterValue) : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_GetPastPresentationTimingGOOGLE(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t* pPresentationTimingCount,
    VkPastPresentationTimingGOOGLE* pPresentationTimings) {
    if (swapchainIsProxy(swapchain)) {
        if (pPresentationTimingCount) *pPresentationTimingCount = 0;
        return VK_SUCCESS;
    }
    DeviceData* dd = deviceDataOf(device);
    if (!dd || !dd->gdpa) return VK_SUCCESS;
    auto fn = reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
        dd->gdpa(device, "vkGetPastPresentationTimingGOOGLE"));
    return fn ? fn(device, swapchain, pPresentationTimingCount, pPresentationTimings)
              : VK_SUCCESS;
}

// Newer present-timing entry points (not in all headers) — never forward proxy handles.
VKAPI_ATTR VkResult VKAPI_CALL xfsr_PresentTimingStubUnsupported(VkDevice /*device*/, ...) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL xfsr_SetHdrMetadataEXT(VkDevice device, uint32_t swapchainCount,
                                                  const VkSwapchainKHR* pSwapchains,
                                                  const VkHdrMetadataEXT* pMetadata) {
    DeviceData* dd = nullptr;
    std::vector<VkSwapchainKHR> real;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto dit = g_devices.find(keyOf(device));
        if (dit != g_devices.end()) dd = dit->second;
        if (!dd || !pSwapchains) return;
        real.resize(swapchainCount);
        for (uint32_t i = 0; i < swapchainCount; ++i) {
            // App-facing handle is the real ICD swapchain (or a passthrough handle).
            ProxySwapchain* ps = proxyOf(pSwapchains[i]);
            real[i] = (ps && ps->realSwapchain) ? ps->realSwapchain : pSwapchains[i];
        }
    }
    if (!dd || !dd->gdpa) return;
    auto fn =
        reinterpret_cast<PFN_vkSetHdrMetadataEXT>(dd->gdpa(device, "vkSetHdrMetadataEXT"));
    if (!fn) return;
    for (uint32_t i = 0; i < swapchainCount; ++i) {
        if (real[i] != VK_NULL_HANDLE)
            fn(device, 1, &real[i], pMetadata ? &pMetadata[i] : pMetadata);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_QueuePresentKHR(VkQueue queue,
                                                    const VkPresentInfoKHR* pPresentInfo) {
    if (!pPresentInfo || pPresentInfo->swapchainCount == 0) {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_devices.empty())
            return g_devices.begin()->second->QueuePresentKHR(queue, pPresentInfo);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    ProxySwapchain* ps = nullptr;
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ps = proxyOf(pPresentInfo->pSwapchains[0]);
        if (ps) dd = ps->dev;
    }

    if (!ps || !ps->pipelineReady || !dd) {
        if (dd && dd->QueuePresentKHR) return dd->QueuePresentKHR(queue, pPresentInfo);
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_devices.empty())
            return g_devices.begin()->second->QueuePresentKHR(queue, pPresentInfo);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const uint32_t idx = pPresentInfo->pImageIndices[0];
    if (idx >= ps->appImages.size()) return VK_ERROR_OUT_OF_DATE_KHR;

    const uint32_t slot = ps->frameSlot;
    VkCommandBuffer cmd = ps->cmds[slot];
    VkFence fence = ps->inFlight[slot];
    VkSemaphore acqSem = ps->imageAvailable[slot];
    VkSemaphore doneSem = ps->renderFinished[slot];
    DeviceContext& presentCtx = presentsOnIgpu(ps) ? dd->igpu : dd->dgpu;

    try {
        const GpuImage* frameImg = &ps->appImages[idx];
        bool roundtripUpscaled = false;
        if (ps->zinkOffloadRoundtrip && dd->xferReady && dd->xferBackReady && ps->useFsr) {
            // Sync round-trip: Zink leaves PRESENT_SRC; transfer expects SHADER_READ_ONLY.
            if (dd->QueueWaitIdle && dd->graphicsQueue) dd->QueueWaitIdle(dd->graphicsQueue);
            (void)queue;
            dd->dgpu.resetAndBegin();
            transitionImage(&dd->dgpu, dd->dgpu.cmd, ps->appImages[idx].image,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            dd->dgpu.endSubmitWait();

            double memcpyMs = 0, totalMs = 0;
            const GpuImage& onIgpu =
                dd->xfer.transferSync(ps->appImages[idx], memcpyMs, totalMs);

            dd->igpu.resetAndBegin();
            ps->fsr.record(dd->igpu.cmd, onIgpu);
            dd->igpu.endSubmitWait();

            frameImg = &dd->xferBack.transferSync(ps->fsr.output(), memcpyMs, totalMs);
            roundtripUpscaled = true;
            (void)memcpyMs;
            (void)totalMs;
        } else if (presentsOnIgpu(ps) && dd->xferReady) {
            // Chain app present-wait semaphores into the download submit (no host stall).
            std::vector<VkPipelineStageFlags> stages;
            if (pPresentInfo->waitSemaphoreCount > 0) {
                stages.assign(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
            double memcpyMs = 0, totalMs = 0;
            frameImg = &dd->xfer.transferPipeline(
                ps->appImages[idx], memcpyMs, totalMs, pPresentInfo->waitSemaphoreCount,
                pPresentInfo->waitSemaphoreCount > 0 ? pPresentInfo->pWaitSemaphores : nullptr,
                stages.empty() ? nullptr : stages.data());
            (void)memcpyMs;
            (void)totalMs;
            (void)queue;
        }

        presentCtx.loadDispatch();
        auto& pd = presentCtx.vk;
        WsiFns wsi = wsiFnsFor(dd, presentsOnIgpu(ps));
        if (!wsi.AcquireNextImageKHR || !wsi.QueuePresentKHR) return VK_ERROR_DEVICE_LOST;

        vkCheck(pd.WaitForFences(presentCtx.device, 1, &fence, VK_TRUE, UINT64_MAX), "wait");
        vkCheck(pd.ResetFences(presentCtx.device, 1, &fence), "reset fence");

        uint32_t realIdx = 0;
        VkResult acq = wsi.AcquireNextImageKHR(presentCtx.device, ps->realSwapchain, UINT64_MAX,
                                               acqSem, VK_NULL_HANDLE, &realIdx);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            // Window surface churn (dual-GPU / compositor). Don't force app recreate.
            static bool loggedAcq = false;
            if (!loggedAcq) {
                loggedAcq = true;
                std::fprintf(stderr, "[xfsr] ignoring VK_ERROR_OUT_OF_DATE_KHR on proxy acquire\n");
            }
            return VK_SUCCESS;
        }
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) return acq;

        vkCheck(pd.ResetCommandBuffer(cmd, 0), "reset cmd");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(pd.BeginCommandBuffer(cmd, &bi), "begin cmd");

        const bool directFsr = ps->useFsr && ps->fsrDirectToSwapchain &&
                               realIdx < ps->realViews.size() &&
                               ps->realViews[realIdx] != VK_NULL_HANDLE;
        const bool swizzle = ps->fsrDirectSwizzleBgra;

        VkImage srcImage = frameImg->image;
        uint32_t srcW = frameImg->width ? frameImg->width : ps->renderW;
        uint32_t srcH = frameImg->height ? frameImg->height : ps->renderH;
        // FSR intermediates are always RGBA8 UNORM; CS2/Zink swapchains are often BGRA_SRGB.
        VkFormat srcFmt = ps->format;
        bool wroteSwapchain = false;

        if (roundtripUpscaled) {
            // Display-sized RGBA8 already on dGPU (SHADER_READ_ONLY) — blit into WSI.
            transitionImage(&presentCtx, cmd, frameImg->image,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT);
            srcImage = frameImg->image;
            srcW = frameImg->width ? frameImg->width : ps->displayW;
            srcH = frameImg->height ? frameImg->height : ps->displayH;
            srcFmt = frameImg->format ? frameImg->format : VK_FORMAT_R8G8B8A8_UNORM;
        } else if (presentsOnIgpu(ps)) {
            // After transfer: SHADER_READ_ONLY on iGPU.
            if (ps->useFsr) {
                if (directFsr) {
                    ps->fsr.record(cmd, *frameImg, ps->realViews[realIdx],
                                   ps->realImages[realIdx], swizzle);
                    wroteSwapchain = true;
                } else {
                    ps->fsr.record(cmd, *frameImg);
                    const GpuImage& fsrOut = ps->fsr.output();
                    transitionImage(&presentCtx, cmd, fsrOut.image,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT);
                    srcImage = fsrOut.image;
                    srcW = fsrOut.width;
                    srcH = fsrOut.height;
                    srcFmt = fsrOut.format ? fsrOut.format : VK_FORMAT_R8G8B8A8_UNORM;
                }
            } else {
                transitionImage(&presentCtx, cmd, frameImg->image,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            }
        } else {
            // Same-GPU: app left PRESENT_SRC.
            if (ps->useFsr) {
                transitionImage(&presentCtx, cmd, frameImg->image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT);
                if (directFsr) {
                    ps->fsr.record(cmd, *frameImg, ps->realViews[realIdx],
                                   ps->realImages[realIdx], swizzle);
                    wroteSwapchain = true;
                } else {
                    ps->fsr.record(cmd, *frameImg);
                    const GpuImage& fsrOut = ps->fsr.output();
                    transitionImage(&presentCtx, cmd, fsrOut.image,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT);
                    srcImage = fsrOut.image;
                    srcW = fsrOut.width;
                    srcH = fsrOut.height;
                    srcFmt = fsrOut.format ? fsrOut.format : VK_FORMAT_R8G8B8A8_UNORM;
                }
            } else {
                transitionImage(&presentCtx, cmd, frameImg->image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            }
        }

        if (!wroteSwapchain) {
            transitionImage(&presentCtx, cmd, ps->realImages[realIdx], VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);

            // vkCmdCopyImage requires identical formats — FSR RGBA8 → BGRA_SRGB must blit.
            const bool canCopy =
                srcW == ps->displayW && srcH == ps->displayH && srcFmt == ps->format;
            if (canCopy) {
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {srcW, srcH, 1};
                pd.CmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                ps->realImages[realIdx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                &region);
            } else {
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[0] = {0, 0, 0};
                blit.srcOffsets[1] = {static_cast<int32_t>(srcW), static_cast<int32_t>(srcH), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstOffsets[0] = {0, 0, 0};
                blit.dstOffsets[1] = {static_cast<int32_t>(ps->displayW),
                                      static_cast<int32_t>(ps->displayH), 1};
                pd.CmdBlitImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                ps->realImages[realIdx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                &blit, VK_FILTER_LINEAR);
            }

            transitionImage(&presentCtx, cmd, ps->realImages[realIdx],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        }

        vkCheck(pd.EndCommandBuffer(cmd), "end cmd");

        // Same-GPU: chain app present-wait semaphores into this submit (no host stall).
        std::vector<VkSemaphore> waitSems;
        std::vector<VkPipelineStageFlags> waitStages;
        waitSems.reserve(pPresentInfo->waitSemaphoreCount + 1u);
        waitStages.reserve(pPresentInfo->waitSemaphoreCount + 1u);
        // Classic iGPU-present offload already waited in the transfer submit.
        // Zink round-trip drained the dGPU via QueueWaitIdle above.
        if (!ps->offloadIgpu) {
            for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; ++i) {
                waitSems.push_back(pPresentInfo->pWaitSemaphores[i]);
                waitStages.push_back(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                     VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
        }
        waitSems.push_back(acqSem);
        waitStages.push_back(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = static_cast<uint32_t>(waitSems.size());
        si.pWaitSemaphores = waitSems.data();
        si.pWaitDstStageMask = waitStages.data();
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &doneSem;
        vkCheck(pd.QueueSubmit(presentCtx.presentQueue ? presentCtx.presentQueue
                                                       : presentCtx.graphicsQueue,
                               1, &si, fence),
                "submit");

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &doneSem;
        pi.swapchainCount = 1;
        pi.pSwapchains = &ps->realSwapchain;
        pi.pImageIndices = &realIdx;
        VkQueue presentQ =
            presentCtx.presentQueue ? presentCtx.presentQueue : presentCtx.graphicsQueue;
        VkResult pr = wsi.QueuePresentKHR(presentQ, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR && pr != VK_ERROR_OUT_OF_DATE_KHR) {
            std::fprintf(stderr, "[xfsr] QueuePresent failed: %d\n", static_cast<int>(pr));
            return pr;
        }

        ps->frameSlot = (slot + 1) % ProxySwapchain::kFrames;

        ++ps->presentCount;
        if (!dd->loggedFirstPresent) {
            dd->loggedFirstPresent = true;
            std::fprintf(stderr, "[xfsr] first present ok (app img %u → display %ux%u%s)\n", idx,
                         ps->displayW, ps->displayH,
                         ps->zinkOffloadRoundtrip
                             ? ", dual-GPU FSR round-trip"
                             : (ps->offloadIgpu ? ", dual-GPU" : ""));
        }
        if (ps->presentCount == 60 || (ps->presentCount > 0 && ps->presentCount % 300 == 0)) {
            std::fprintf(stderr, "[xfsr] presented %u frames (%ux%u → %ux%u %s)\n", ps->presentCount,
                         ps->renderW, ps->renderH, ps->displayW, ps->displayH,
                         ps->useFsr ? "FSR1" : "blit");
        }
        if (pPresentInfo->pResults) {
            for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
                pPresentInfo->pResults[i] = VK_SUCCESS;
        }
        // Swallow SUBOPTIMAL / OUT_OF_DATE. Returning them makes the app recreate every
        // frame (especially dual-GPU). We own the real WSI; keep presenting.
        if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR) {
            static bool loggedSub = false;
            if (!loggedSub) {
                loggedSub = true;
                std::fprintf(stderr, "[xfsr] ignoring %s on proxy present\n",
                             pr == VK_SUBOPTIMAL_KHR ? "VK_SUBOPTIMAL_KHR"
                                                     : "VK_ERROR_OUT_OF_DATE_KHR");
            }
            return VK_SUCCESS;
        }
        return pr;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xfsr] present failed: %s\n", e.what());
        return VK_ERROR_DEVICE_LOST;
    }
}

PFN_vkVoidFunction resolveInstance(const char* pName) {
    if (std::strcmp(pName, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_CreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_DestroyInstance);
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(::xfsr_GetInstanceProcAddr);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(::xfsr_GetDeviceProcAddr);
    if (std::strcmp(pName, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_CreateDevice);
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_GetPhysicalDeviceSurfaceCapabilitiesKHR);
    return nullptr;
}

PFN_vkVoidFunction resolveDevice(const char* pName) {
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(::xfsr_GetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_DestroyDevice);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_CreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_DestroySwapchainKHR);
    if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_GetSwapchainImagesKHR);
    if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_AcquireNextImageKHR);
    if (std::strcmp(pName, "vkAcquireNextImage2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_AcquireNextImage2KHR);
    if (std::strcmp(pName, "vkGetSwapchainStatusKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_GetSwapchainStatusKHR);
    if (std::strcmp(pName, "vkWaitForPresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_WaitForPresentKHR);
    if (std::strcmp(pName, "vkWaitForPresent2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_WaitForPresent2KHR);
    if (std::strcmp(pName, "vkReleaseSwapchainImagesKHR") == 0 ||
        std::strcmp(pName, "vkReleaseSwapchainImagesEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_ReleaseSwapchainImagesEXT);
    if (std::strcmp(pName, "vkSetHdrMetadataEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_SetHdrMetadataEXT);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_QueuePresentKHR);
    if (std::strcmp(pName, "vkGetSwapchainCounterEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_GetSwapchainCounterEXT);
    if (std::strcmp(pName, "vkGetPastPresentationTimingGOOGLE") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_GetPastPresentationTimingGOOGLE);
    // Timing helpers Zink probes — stub (never forward opaque proxy handles).
    if (pName && (std::strcmp(pName, "vkGetPastPresentationTimingEXT") == 0 ||
                  std::strcmp(pName, "vkSetSwapchainPresentTimingQueueSizeEXT") == 0 ||
                  std::strcmp(pName, "vkGetSwapchainTimingPropertiesEXT") == 0 ||
                  std::strcmp(pName, "vkGetSwapchainTimeDomainPropertiesEXT") == 0)) {
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_PresentTimingStubUnsupported);
    }
    // Log unknown WSI-ish lookups — helpful when CS2 grows new swapchain entry points.
    if (verboseLog() && pName &&
        (std::strstr(pName, "Swapchain") || std::strstr(pName, "Present") ||
         std::strstr(pName, "FullScreen") || std::strstr(pName, "HdrMetadata"))) {
        logv("[xfsr] GDPA passthrough: %s\n", pName);
    }
    return nullptr;
}

} // namespace

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL xfsr_GetInstanceProcAddr(VkInstance instance,
                                                                  const char* pName) {
    if (auto f = resolveInstance(pName)) return f;
    if (auto f = resolveDevice(pName)) return f;
    InstanceData* id = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_instances.find(keyOf(instance));
        if (it != g_instances.end()) id = it->second;
    }
    if (id && id->gipa) return id->gipa(instance, pName);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL xfsr_GetDeviceProcAddr(VkDevice device, const char* pName) {
    DeviceData* dd = nullptr;
    InstanceData* id = nullptr;
    PFN_vkGetDeviceProcAddr companionGdpa = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_devices.find(keyOf(device));
        if (it != g_devices.end()) dd = it->second;
        auto cit = g_companionGdpa.find(keyOf(device));
        if (cit != g_companionGdpa.end()) companionGdpa = cit->second;
        if (!g_instances.empty()) id = g_instances.begin()->second;
    }
    // Only intercept swapchain/present on the app's tracked dGPU device.
    // Companion iGPU must get real ICD entry points (otherwise Presenter recurses into us).
    if (dd) {
        if (auto f = resolveDevice(pName)) return f;
        if (dd->gdpa) return dd->gdpa(device, pName);
    }
    if (companionGdpa) return companionGdpa(device, pName);
    if (id && id->gipa) {
        auto gdpa =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(id->gipa(id->instance, "vkGetDeviceProcAddr"));
        if (gdpa) return gdpa(device, pName);
    }
    return nullptr;
}

} // extern "C"
