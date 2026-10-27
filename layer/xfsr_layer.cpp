// VK_LAYER_XFSR — dual-GPU FSR1 offload (demo mode 2).
// Enable: ENABLE_XFSR=1
// Config: XFSR_FSR=1 XFSR_PRESET=quality|balanced|performance|ultra
//         XFSR_OFFLOAD=1  XFSR_IGPU_PRESENT=0|1 (one-way try; 0=round-trip only)
//         XFSR_LOD_BIAS=auto|off|<float>  XFSR_LOD_BIAS_OFFSET=<float>
// Legacy CROSSVULKAN_* env names still accepted.
//
// Proxy swapchain: app gets low-res dGPU images; prefer iGPU present (one-way),
// else dGPU WSI + iGPU FSR round-trip.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <volk.h>
#include <vulkan/vk_layer.h>

#include "dual_devices.hpp"
#include "frame_transfer.hpp"
#include "fsr_pass.hpp"
#include "gpu_image.hpp"
#include "nis_pass.hpp"
#include "edge_pass.hpp"
#include "offload_roundtrip.hpp"
#include "xfsr_controls.hpp"

#ifndef VK_LAYER_EXPORT
#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define VK_LAYER_EXPORT
#endif
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // xfsr-run exports XFSR_SCALE — prefer it when set (covers ultra 2.5× etc.).
    if (const char* s = std::getenv("XFSR_SCALE")) {
        // Manual parse — avoid strtof pulling unexpected libc ABIs into the layer.
        float v = 0.f, frac = 0.f, place = 0.1f;
        const char* p = s;
        while (*p == ' ' || *p == '\t') ++p;
        while (*p >= '0' && *p <= '9') {
            v = v * 10.f + static_cast<float>(*p - '0');
            ++p;
        }
        if (*p == '.') {
            ++p;
            while (*p >= '0' && *p <= '9') {
                frac += static_cast<float>(*p - '0') * place;
                place *= 0.1f;
                ++p;
            }
        }
        v += frac;
        if (p != s && v >= 1.01f && v <= 8.0f) return v;
    }
    const char* p = xfsrEnv("XFSR_PRESET", "CROSSVULKAN_PRESET_MODE");
    if (!p) return 1.5f;
    if (std::strcmp(p, "ultra") == 0 || std::strcmp(p, "U") == 0 ||
        std::strcmp(p, "extreme") == 0)
        return 2.5f;
    if (std::strcmp(p, "balanced") == 0 || std::strcmp(p, "W") == 0) return 1.7f;
    if (std::strcmp(p, "performance") == 0 || std::strcmp(p, "perf") == 0 ||
        std::strcmp(p, "E") == 0)
        return 2.0f;
    return 1.5f;
}

// Parse unsigned decimal without strto*/sscanf (those bind __isoc23_*@GLIBC_2.38
// on modern toolchains and break Proton/steamrt's glibc 2.31 dlopen).
bool parseU32(const char* s, uint32_t* out, const char** endOut = nullptr) {
    if (!s || !out) return false;
    const char* p = s;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') return false;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
        const uint32_t d = static_cast<uint32_t>(*p - '0');
        if (v > (UINT32_MAX - d) / 10u) return false;
        v = v * 10u + d;
        ++p;
    }
    *out = v;
    if (endOut) *endOut = p;
    return true;
}

// Wayland often reports currentExtent=UINT_MAX until the first swapchain exists, so
// caps-shrink never runs and benches stay identity @ full window (flat FPS). Hint the
// real window via XFSR_DISPLAY_EXTENT=WxH (xfsr-run --output) or XFSR_OUTPUT_*.
void applyDisplayExtentHint(uint32_t* outW, uint32_t* outH) {
    if (!outW || !outH) return;
    const char* e = xfsrEnv("XFSR_DISPLAY_EXTENT", "CROSSVULKAN_DISPLAY_EXTENT");
    if (e && e[0]) {
        uint32_t pw = 0, ph = 0;
        const char* mid = nullptr;
        if (parseU32(e, &pw, &mid) && mid && (*mid == 'x' || *mid == 'X') &&
            parseU32(mid + 1, &ph) && pw > 0 && ph > 0) {
            *outW = pw;
            *outH = ph;
            return;
        }
    }
    const char* ew = std::getenv("XFSR_OUTPUT_W");
    const char* eh = std::getenv("XFSR_OUTPUT_H");
    if (ew && eh) {
        uint32_t w = 0, h = 0;
        if (parseU32(ew, &w) && parseU32(eh, &h) && w > 0 && h > 0) {
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

xfsr_ctrl::Upscaler resolveUpscaler() {
    if (xfsr_ctrl::enabled()) return xfsr_ctrl::peek().upscaler;
    if (const char* u = xfsrEnv("XFSR_UPSCALER", "CROSSVULKAN_UPSCALER")) {
        if (u[0]) {
            if (std::strcmp(u, "edge") == 0 || u[0] == '3') return xfsr_ctrl::Upscaler::Edge;
            if (std::strcmp(u, "nis") == 0 || u[0] == '2') return xfsr_ctrl::Upscaler::Nis;
            if (std::strcmp(u, "blit") == 0 || u[0] == '0') return xfsr_ctrl::Upscaler::Blit;
            return xfsr_ctrl::Upscaler::Fsr1;
        }
    }
    return layerFsrPassEnabled() ? xfsr_ctrl::Upscaler::Fsr1 : xfsr_ctrl::Upscaler::Blit;
}

[[maybe_unused]] const char* upscalerTag(xfsr_ctrl::Upscaler u) {
    switch (u) {
    case xfsr_ctrl::Upscaler::Blit:
        return "blit";
    case xfsr_ctrl::Upscaler::Nis:
        return "nis";
    case xfsr_ctrl::Upscaler::Edge:
        return "edge";
    default:
        return "FSR1";
    }
}

// --hybrid / XFSR_HYBRID=1: prefer iGPU one-way + edge; never settle on round-trip.
bool hybridWanted() {
    if (const char* h = xfsrEnv("XFSR_HYBRID", "CROSSVULKAN_HYBRID")) {
        if (h[0] == '1') return true;
        if (h[0] == '0') return false;
    }
    return false;
}

bool offloadEnabled() {
    // Prefer explicit env — HUD peek can lag or be tainted by ambient IGPU_PRESENT.
    const char* o = xfsrEnv("XFSR_OFFLOAD", "CROSSVULKAN_OFFLOAD");
    if (o && o[0] == '0') return false;
    if (o && o[0] && o[0] != '0') return true;
    if (hybridWanted()) return true;
    if (xfsr_ctrl::enabled()) return xfsr_ctrl::peek().offload;
    return false;
}

struct DeviceData; // forward — igpuPresentWanted reads dGPU vendor

// One-way iGPU present: XFSR_IGPU_PRESENT=0 → round-trip only; =1 → one-way.
// Default with offload: one-way on any vendor. MangoHud still forces round-trip
// (opaque swapchain SIGSEGV) unless XFSR_IGPU_PRESENT=force.
bool igpuPresentWanted(const DeviceData* dd);

// When the app ignores caps shrink (DXVK/Proton often does), force render extent in
// CreateSwapchain. Default ON when FSR is enabled. XFSR_FORCE_SCALE=0 disables.
// WARNING: force-scale must NEVER create proxy images smaller than the app's
// imageExtent — framebuffers are sized to that extent (DEVICE_LOST otherwise).
bool forceScaleEnabled() {
    const char* o = xfsrEnv("XFSR_FORCE_SCALE", "CROSSVULKAN_FORCE_SCALE");
    if (o && o[0] == '0') return false;
    if (o && o[0] == '1') return true;
    return fsrEnabled();
}

// Advertise shrunk currentExtent so well-behaved apps (xfsr_load_vk, Zink) create
// matching framebuffers. Default ON. Set XFSR_CAPS_SHRINK=0 for Source2/CS2 which
// treat currentExtent changes as a resize storm.
bool capsShrinkEnabled() {
    const char* o = xfsrEnv("XFSR_CAPS_SHRINK", "CROSSVULKAN_CAPS_SHRINK");
    if (o && o[0] == '0') return false;
    if (o && o[0] == '1') return true;
    return fsrEnabled();
}

// Manual float parse — avoid strtof (glibc 2.38 __isoc23_* breaks Proton steamrt).
bool parseFloat(const char* s, float* out, const char** endOut = nullptr) {
    if (!s || !out) return false;
    const char* p = s;
    while (*p == ' ' || *p == '\t') ++p;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        ++p;
    } else if (*p == '+') {
        ++p;
    }
    if ((*p < '0' || *p > '9') && *p != '.') return false;
    float v = 0.f, frac = 0.f, place = 0.1f;
    const char* start = p;
    while (*p >= '0' && *p <= '9') {
        v = v * 10.f + static_cast<float>(*p - '0');
        ++p;
    }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            frac += static_cast<float>(*p - '0') * place;
            place *= 0.1f;
            ++p;
        }
    }
    if (p == start) return false;
    v += frac;
    if (neg) v = -v;
    *out = v;
    if (endOut) *endOut = p;
    return true;
}

// Negative mip LOD bias so low internal res still samples sharper mips (PDF).
// XFSR_LOD_BIAS=off|0 → disable; =auto (default when FSR/scaling) → -log2(scale);
// numeric → absolute bias. XFSR_LOD_BIAS_OFFSET adds a user tweak.
float samplerLodBiasInject() {
    const char* e = xfsrEnv("XFSR_LOD_BIAS", "CROSSVULKAN_LOD_BIAS");
    if (e) {
        if (e[0] == '0' || std::strcmp(e, "off") == 0 || std::strcmp(e, "none") == 0)
            return 0.f;
        if (std::strcmp(e, "auto") != 0) {
            float v = 0.f;
            if (parseFloat(e, &v)) {
                if (const char* off =
                        xfsrEnv("XFSR_LOD_BIAS_OFFSET", "CROSSVULKAN_LOD_BIAS_OFFSET")) {
                    float o = 0.f;
                    if (parseFloat(off, &o)) v += o;
                }
                return std::clamp(v, -8.f, 8.f);
            }
        }
    }
    // auto / unset: only when FSR/scaling is on
    if (!fsrEnabled()) return 0.f;
    const float scale = presetScaleFromEnv();
    float bias = (scale > 1.01f) ? -std::log2(scale) : 0.f;
    if (const char* off = xfsrEnv("XFSR_LOD_BIAS_OFFSET", "CROSSVULKAN_LOD_BIAS_OFFSET")) {
        float v = 0.f;
        if (parseFloat(off, &v)) bias += v;
    }
    return std::clamp(bias, -8.f, 8.f);
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

// Same formula for Vulkan, DXVK, and Zink, windowed or fullscreen.
// 1080p ultra is 768×432; a 1280×720 window at the same scale is 512×288.
void computeRenderSize(uint32_t displayW, uint32_t displayH, float scale, uint32_t* outW,
                       uint32_t* outH) {
    const float s = scale < 1.01f ? 1.f : scale;
    uint32_t rw = std::max(64u, static_cast<uint32_t>(displayW / s + 0.5f));
    uint32_t rh = std::max(64u, static_cast<uint32_t>(displayH / s + 0.5f));
    if (rw > displayW) rw = displayW;
    if (rh > displayH) rh = displayH;
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
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR GetPhysicalDeviceSurfaceCapabilities2KHR =
        nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    // From CreateInstance pNext — required to create companion devices from a layer.
    PFN_vkLayerCreateDevice layerCreateDevice = nullptr;
    PFN_vkLayerDestroyDevice layerDestroyDevice = nullptr;
    float scale = 1.5f;
    uint32_t realW = 0;
    uint32_t realH = 0;
    bool realFromSurface = false; // realW/H came from compositor (not DISPLAY_EXTENT alone)
    uint32_t hintW = 0;           // XFSR_DISPLAY_EXTENT / OUTPUT_* seed for Wayland games
    uint32_t hintH = 0;
    bool capsShrunk = false; // true when we rewrote currentExtent to render res
    bool volkReady = false;
    // Dedupe caps-shrink logs (Zink queries every frame).
    uint32_t lastLogCw = 0, lastLogCh = 0, lastLogRw = 0, lastLogRh = 0;
    // Wayland currentExtent is UINT_MAX — the first CreateSwapchain imageExtent *is*
    // the window. Seed realFromSurface from it and force one OUT_OF_DATE so the next
    // create honors caps-shrink (native VK and Zink).
    bool waylandShrinkKick = false;
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
    PFN_vkCreateSampler CreateSampler = nullptr;

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
    // Round-trip offload: FSR result iGPU → dGPU (present stays on dGPU WSI).
    FrameTransfer xferBack{};
    bool xferBackReady = false;
    // After one-way WSI/present failure, prefer round-trip on next setup.
    bool forceRoundTripOffload = false;

    // Stable storage if we inject device extensions into CreateDevice.
    std::vector<std::string> injectedExtNames;
    std::vector<const char*> injectedExtPtrs;

    // Dedupe "ready" / first-present logs across swapchain recreates.
    uint32_t loggedRw = 0, loggedRh = 0, loggedDw = 0, loggedDh = 0;
    bool loggedFirstPresent = false;
    bool loggedFixedCaps = false;

    // Status overlay lives on the device so swapchain recreate (glmark2 scenes,
    // preset hotkey) cannot free GPU memory still referenced by in-flight presents.
    xfsr_ctrl::OverlayGpu overlay{};
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
    bool edgeDirectToSwapchain = false;
    bool fsrDirectSwizzleBgra = false; // shared BGRA swizzle for FSR/edge STORAGE→WSI
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
    // Per app-image release fences — fake acquire must not recycle an image while
    // present/FSR still reads it (NVIDIA DEVICE_LOST / xfsr_load_vk wait -4).
    std::vector<VkFence> appImgFence;
    VkFence lastCmdFence[kFrames]{}; // borrowed: which fence guards cmds[slot]

    FsrPass fsr{};
    NisPass nis{};
    EdgePass edge{};
    bool pipelineReady = false;
    bool offloadIgpu = false; // true: FrameTransfer → iGPU (FSR and/or present)
    // Round-trip: dGPU owns window WSI; iGPU runs FSR; frame hauled back for present.
    // One-way: iGPU owns WSI (opaque app handle). Prefer one-way; fall back on failure
    // (NVIDIA hybrids historically SIGSEGV when apps leak WSI to the wrong ICD).
    bool zinkOffloadRoundtrip = false;
    bool useFsr = false;  // FSR1 EASU/RCAS
    bool useNis = false;  // NIS-style spatial upscaler
    bool useEdge = false; // edge upscaler (hybrid default)

    std::vector<GpuImage> appImages;
    uint32_t acquireIndex = 0;
    uint32_t presentCount = 0;
};

std::mutex g_mu;
// Present (often Zink's queue thread) vs Create/DestroySwapchain (main thread):
// without this, destroyProxy/FrameTransfer::resize frees state mid-present →
// SIGSEGV in ICD/MangoHud (seen on gl zink+offload).
std::mutex g_proxyLife;
std::unordered_map<void*, InstanceData*> g_instances;
std::unordered_map<void*, DeviceData*> g_devices;
std::unordered_map<void*, ProxySwapchain*> g_proxies;
// Passthrough swapchains that should return OUT_OF_DATE once (zink Wayland kick).
std::unordered_set<void*> g_forceOutOfDateOnce;
// Identity present (no FSR proxy): cache WSI images so F10 overlay can blit.
struct PassthroughSc {
    DeviceData* dd = nullptr;
    std::vector<VkImage> images;
    uint32_t w = 0, h = 0;
    bool transferDst = false;
};
std::unordered_map<void*, PassthroughSc> g_passthrough;
VkSemaphore g_overlayDoneSem = VK_NULL_HANDLE;
VkFence g_overlayFence = VK_NULL_HANDLE;
VkCommandBuffer g_overlayCmd = VK_NULL_HANDLE;
DeviceData* g_overlaySyncDd = nullptr;
bool g_overlaySyncReady = false;
bool g_loggedOverlaySkip = false;
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

// Present WSI stays on the app/dGPU device whenever round-trip offload is used.
bool presentsOnIgpu(const ProxySwapchain* ps) {
    return ps && ps->offloadIgpu && !ps->zinkOffloadRoundtrip;
}

// Same-GPU (and round-trip): app's swapchain handle IS the real WSI. Swallowing
// OUT_OF_DATE there exhausts images / leaves fences unsignaled → freeze.
// Opaque one-way: we own real WSI under a different handle; safe to swallow.
bool appOwnsRealWsi(const ProxySwapchain* ps) {
    return ps && !ps->opaqueAppHandle && ps->realSwapchain != VK_NULL_HANDLE &&
           ps->appSwapchain == ps->realSwapchain;
}

bool mangoHudActive() {
    const char* m = std::getenv("MANGOHUD");
    if (m && m[0] && m[0] != '0') return true;
    // xfsr-run --mangohud may set this before the Vulkan MH layer loads.
    const char* x = std::getenv("XFSR_MANGOHUD");
    if (x && x[0] && x[0] != '0') return true;
    return false;
}

bool igpuPresentWanted(const DeviceData* dd) {
    bool want = true;
    bool force = false;
    if (xfsr_ctrl::enabled()) {
        const xfsr_ctrl::Snapshot s = xfsr_ctrl::peek();
        if (!s.offload) return false;
        want = s.oneWay;
        force = s.oneWayForce;
    } else {
        const char* o = xfsrEnv("XFSR_IGPU_PRESENT", "CROSSVULKAN_IGPU_PRESENT");
        if (o && o[0] == '0') return false;
        if (o && (std::strcmp(o, "force") == 0 || o[0] == '2')) force = true;
        else if (o && o[0] == '1')
            want = true;
        // unset → Mesa prefers one-way (want stays true)
    }
    if (!want) return false;

    // Opaque app swapchain handles SIGSEGV MangoHud (it caches by VkSwapchainKHR).
    // xfsr-run drops MangoHud for one-way; if it is still loaded, stay on round-trip.
    if (mangoHudActive() && !force) {
        static bool loggedMh = false;
        if (!loggedMh) {
            loggedMh = true;
            std::fprintf(stderr,
                         "[xfsr] one-way disabled with MangoHud (opaque WSI SIGSEGV); "
                         "using round-trip. XFSR_IGPU_PRESENT=force to override.\n");
        }
        return false;
    }
    // Zink keeps a raw NVIDIA QueuePresent and calls it with the app swapchain
    // after the layer returns. An opaque handle SIGSEGVs in libnvidia-glcore.
    // Round-trip returns the real dGPU swapchain. Native Vulkan stays one-way.
    if (zinkActive() && !force) {
        static bool loggedZink = false;
        if (!loggedZink) {
            loggedZink = true;
            std::fprintf(stderr,
                         "[xfsr] one-way disabled under Zink (NVIDIA presents the app "
                         "swapchain itself); using round-trip. XFSR_IGPU_PRESENT=force "
                         "to override.\n");
        }
        (void)dd;
        return false;
    }
    (void)dd;
    return true;
}

// Monotonic opaque swapchain ids — never reuse heap addresses as VkSwapchainKHR
// (MangoHud / apps cache by handle; pointer recycling → use-after-free SIGSEGV).
std::atomic<uint64_t> g_opaqueSwapchainSerial{1};
VkSwapchainKHR makeOpaqueSwapchainHandle() {
    const uint64_t id = g_opaqueSwapchainSerial.fetch_add(1, std::memory_order_relaxed);
    // Non-zero, low-bit clear; distinct from real ICD pointers on 64-bit Linux.
    return reinterpret_cast<VkSwapchainKHR>((id << 4) | 0x1ull);
}

void waitOffloadIdle(DeviceData* dd) {
    if (!dd) return;
    auto waitCtx = [](DeviceContext& ctx) {
        if (!ctx.device) return;
        ctx.loadDispatch();
        auto& d = ctx.vk;
        if (!d.QueueWaitIdle) return;
        if (ctx.graphicsQueue) d.QueueWaitIdle(ctx.graphicsQueue);
        if (ctx.presentQueue && ctx.presentQueue != ctx.graphicsQueue)
            d.QueueWaitIdle(ctx.presentQueue);
    };
    if (dd->dgpuReady) waitCtx(dd->dgpu);
    if (dd->igpuReady) waitCtx(dd->igpu);
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

void destroyAppImgFences(ProxySwapchain* ps, DeviceData* dd) {
    if (!ps || !dd || !dd->dgpu.device) return;
    dd->dgpu.loadDispatch();
    auto& d = dd->dgpu.vk;
    for (VkFence f : ps->appImgFence) {
        if (f && d.DestroyFence) d.DestroyFence(dd->dgpu.device, f, nullptr);
    }
    ps->appImgFence.clear();
    for (uint32_t i = 0; i < ProxySwapchain::kFrames; ++i) ps->lastCmdFence[i] = VK_NULL_HANDLE;
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
    ps->edgeDirectToSwapchain = false;
}

void waitPresentRing(ProxySwapchain* ps, DeviceContext& presentCtx) {
    if (!ps || !presentCtx.device) return;
    presentCtx.loadDispatch();
    auto& d = presentCtx.vk;
    if (!d.WaitForFences) return;
    for (uint32_t i = 0; i < ProxySwapchain::kFrames; ++i) {
        if (ps->inFlight[i])
            d.WaitForFences(presentCtx.device, 1, &ps->inFlight[i], VK_TRUE, UINT64_MAX);
    }
}

// Same-process GL shim (LD_PRELOAD): publish proxy render size so glViewport
// remaps when EGL still reports the window size (X11 Zink + opaque/proxy images).
void publishGlRemap(uint32_t rw, uint32_t rh) {
    if (rw < 64 || rh < 64) {
        unsetenv("XFSR_GL_REMAP");
        return;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%ux%u", rw, rh);
    setenv("XFSR_GL_REMAP", buf, 1);
}

void destroyProxy(ProxySwapchain* ps) {
    if (!ps) return;
    DeviceData* dd = ps->dev;
    if (dd) {
        DeviceContext& presentCtx = presentsOnIgpu(ps) ? dd->igpu : dd->dgpu;
        // Drain the present ring before freeing FSR/app images — overlay is on
        // DeviceData and must not be destroyed here (recreate races → SIGSEGV).
        waitPresentRing(ps, presentCtx);
        if (dd->dgpuReady) {
            dd->dgpu.loadDispatch();
            auto& dvk = dd->dgpu.vk;
            if (dvk.WaitForFences) {
                for (VkFence f : ps->appImgFence) {
                    if (f) dvk.WaitForFences(dd->dgpu.device, 1, &f, VK_TRUE, UINT64_MAX);
                }
            }
        }
    }
    if (ps->pipelineReady) {
        ps->fsr.destroy();
        ps->nis.destroy();
        ps->edge.destroy();
        ps->pipelineReady = false;
    }
    if (dd) destroyAppImgFences(ps, dd);
    for (auto& img : ps->appImages) img.destroy();
    ps->appImages.clear();
    if (dd) {
        DeviceContext& presentCtx = presentsOnIgpu(ps) ? dd->igpu : dd->dgpu;
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
    ps->useNis = false;
    ps->useEdge = false;
    delete ps;
    // No g_mu here (DestroyDevice may already hold it). Next proxy create re-publishes.
    unsetenv("XFSR_GL_REMAP");
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

    // Offload is dGPU-render → iGPU-FSR. If the app device is already integrated
    // (--igpu / Zink-on-Vega), dual-GPU transfer is pointless and often same-GPU
    // dma_buf + opaque one-way → SIGSEGV with MangoHud.
    if (dd->dgpuReady) {
        const auto& ap = dd->dgpu.props;
        const bool appIntegrated =
            ap.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
            (ap.vendorID == 0x1002 && ap.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (appIntegrated) {
            dd->igpuFailed = true;
            std::fprintf(stderr,
                         "[xfsr] --offload skipped: app already on integrated GPU (%s) — "
                         "using same-GPU FSR (drop --igpu to render on the discrete GPU)\n",
                         ap.deviceName);
            return false;
        }
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
            if (phys == dd->physical) {
                logv("[xfsr] companion skip app device (same physical)\n");
                continue;
            }
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
                          VkSwapchainKHR oldAppSwapchain, bool preserveAppImages = false) {
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
        // Prefer one-way iGPU present; round-trip when forced or XFSR_IGPU_PRESENT=0.
        // Hybrid mode refuses round-trip: fall back to same-GPU instead.
        if (ps->offloadIgpu) {
            const bool wantOneWay = igpuPresentWanted(dd) && !dd->forceRoundTripOffload;
            if (!wantOneWay && hybridWanted()) {
                std::fprintf(stderr,
                             "[xfsr] hybrid: one-way unavailable — same-GPU edge (no round-trip)\n");
                ps->offloadIgpu = false;
                ps->zinkOffloadRoundtrip = false;
            } else {
                ps->zinkOffloadRoundtrip = !wantOneWay;
                if (ps->zinkOffloadRoundtrip) {
                    logv("[xfsr] offload: dGPU WSI + iGPU FSR (round-trip)%s%s\n",
                         zinkActive() ? " [zink]" : "",
                         dd->forceRoundTripOffload ? " [forced]" : "");
                } else {
                    logv("[xfsr] offload: try one-way iGPU present%s%s\n",
                         zinkActive() ? " [zink]" : "", hybridWanted() ? " [hybrid]" : "");
                }
            }
        } else {
            ps->zinkOffloadRoundtrip = false;
        }
        DeviceContext* presentCtx =
            presentsOnIgpu(ps) ? &dd->igpu : &dd->dgpu;

        if (ps->offloadIgpu) {
            if (!dd->xferReady) {
                dd->xfer.init(&dd->dgpu, &dd->igpu);
                dd->xferReady = true;
            }
            dd->xfer.resize(ps->renderW, ps->renderH, ps->format, 4);
            logv("[xfsr] transfer backend=%s (dGPU→iGPU)\n",
                 FrameTransfer::backendName(dd->xfer.backend));
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
        // Must track whether STORAGE was actually granted — NVIDIA often rejects
        // STORAGE on WSI and the fallback lacks it; writing STORAGE then DEVICE_LOSTs.
        const VkImageUsageFlags presentUsageStorage =
            ps->imageUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        const VkImageUsageFlags presentUsageBlit =
            ps->imageUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const VkImageUsageFlags shadowUsage =
            ps->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        bool presentHasStorage = false;

        auto createPresentWsi = [&](DeviceContext* ctx, bool igpuWsi, VkSwapchainKHR oldSc,
                                    VkSwapchainKHR* out) -> VkResult {
            presentHasStorage = false;
            VkResult cr =
                createWsi(ctx, igpuWsi, {presentW, presentH}, presentUsageStorage, oldSc, out);
            if (cr == VK_SUCCESS) {
                presentHasStorage = true;
                return cr;
            }
            return createWsi(ctx, igpuWsi, {presentW, presentH}, presentUsageBlit, oldSc, out);
        };

        // Display-sized WSI only. One-way: iGPU owns the window (opaque app handle).
        // Round-trip: dGPU owns the window (real ICD handle for app/MangoHud).
        (void)shadowUsage;
        (void)oldAppSwapchain;
        VkResult cr = VK_ERROR_INITIALIZATION_FAILED;
        if (presentsOnIgpu(ps)) {
            cr = createPresentWsi(&dd->igpu, true, oldPresentSwapchain, &ps->realSwapchain);
            if (cr != VK_SUCCESS) {
                if (hybridWanted()) {
                    std::fprintf(stderr,
                                 "[xfsr] hybrid: iGPU present failed (%d) — same-GPU edge "
                                 "(refusing round-trip)\n",
                                 static_cast<int>(cr));
                    ps->offloadIgpu = false;
                    ps->zinkOffloadRoundtrip = false;
                    ps->opaqueAppHandle = false;
                    dd->forceRoundTripOffload = false;
                } else {
                    std::fprintf(stderr,
                                 "[xfsr] iGPU present swapchain failed: %d — fall back to "
                                 "round-trip\n",
                                 static_cast<int>(cr));
                    dd->forceRoundTripOffload = true;
                    ps->zinkOffloadRoundtrip = true;
                    ps->opaqueAppHandle = false;
                }
            } else {
                presentCtx = &dd->igpu;
                ps->opaqueAppHandle = true;
                ps->appSwapchain = VK_NULL_HANDLE; // set to opaque pointer after ProxySwapchain*
                cr = VK_SUCCESS;
            }
        }

        if (!presentsOnIgpu(ps)) {
            // Round-trip (or same-GPU): ensure back-haul transfer is ready when offloading.
            if (ps->offloadIgpu && ps->zinkOffloadRoundtrip) {
                if (!dd->xferBackReady) {
                    dd->xferBack.init(&dd->igpu, &dd->dgpu);
                    dd->xferBackReady = true;
                }
                dd->xferBack.resize(ps->displayW, ps->displayH, VK_FORMAT_R8G8B8A8_UNORM, 4);
                logv("[xfsr] transfer backend=%s (iGPU→dGPU back-haul)\n",
                     FrameTransfer::backendName(dd->xferBack.backend));
            }
            presentCtx = &dd->dgpu;
            // Round-trip / same-GPU: expose the real ICD swapchain. Opaque handles
            // here SIGSEGV MangoHud (it caches by handle; 0x11/0x21 serials blow up).
            // One-way iGPU present keeps opaqueAppHandle=true (set above).
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

        if (!preserveAppImages) {
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
            {
                destroyAppImgFences(ps, dd);
                VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                ps->appImgFence.resize(ps->appImages.size());
                for (size_t i = 0; i < ps->appImgFence.size(); ++i) {
                    vkCheck(dd->dgpu.vk.CreateFence(dd->dgpu.device, &fci, nullptr,
                                                    &ps->appImgFence[i]),
                            "appImgFence");
                }
            }
        } else if (ps->appImages.empty()) {
            return false;
        }

        {
            const xfsr_ctrl::Upscaler ups = resolveUpscaler();
            ps->useFsr = scaling && ups == xfsr_ctrl::Upscaler::Fsr1;
            ps->useNis = scaling && ups == xfsr_ctrl::Upscaler::Nis;
            ps->useEdge = scaling && ups == xfsr_ctrl::Upscaler::Edge;
            if (hybridWanted() && scaling && !ps->useEdge && !ps->useNis && !ps->useFsr) {
                // Hybrid defaults to edge when upscaler unset/blit.
                ps->useEdge = true;
            }
        }
        ps->fsrDirectToSwapchain = false;
        ps->edgeDirectToSwapchain = false;
        ps->fsrDirectSwizzleBgra = false;
        // Always init+resize when scaling so F3 can flip blit↔FSR↔NIS↔edge without recreate.
        if (scaling) {
            DeviceContext* fsrCtx = ps->offloadIgpu ? &dd->igpu : &dd->dgpu;
            fsrCtx->loadDispatch();
            ps->fsr.init(fsrCtx);
            if (xfsr_ctrl::enabled()) {
                const auto peek = xfsr_ctrl::peek();
                ps->fsr.enableRcas = peek.rcas;
            }
            ps->fsr.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
            try {
                ps->nis.init(fsrCtx);
                ps->nis.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[xfsr] NIS init failed: %s — FSR1/blit/edge only\n", e.what());
                ps->nis.destroy();
                if (ps->useNis) {
                    ps->useNis = false;
                    ps->useEdge = true;
                }
            }
            try {
                ps->edge.init(fsrCtx);
                if (xfsr_ctrl::enabled()) {
                    const auto peek = xfsr_ctrl::peek();
                    ps->edge.setSharpness(peek.edgeSharpness);
                    ps->edge.setPerfMode(peek.edgePerf);
                }
                ps->edge.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[xfsr] Edge init failed: %s — FSR1/NIS/blit only\n", e.what());
                ps->edge.destroy();
                if (ps->useEdge) {
                    ps->useEdge = false;
                    ps->useNis = ps->nis.ctx != nullptr;
                    if (!ps->useNis) ps->useFsr = true;
                }
            }
            // Round-trip cannot STORAGE into the other GPU's swapchain.
            // Direct STORAGE→WSI DEVICE_LOSTs on NVIDIA after first present — keep FSR
            // direct off. Edge hybrid presents on AMD iGPU where STORAGE→WSI is safe.
            const bool presentNotNvidia = presentCtx->props.vendorID != 0x10de;
            const bool wantEdgeDirect = presentHasStorage && !ps->zinkOffloadRoundtrip &&
                                        ps->useEdge && presentNotNvidia &&
                                        ps->edge.canDirectToSwapchain(ps->format);
            const bool wantFsrDirect = false && presentHasStorage && !ps->zinkOffloadRoundtrip &&
                                       ps->useFsr && ps->fsr.canDirectToSwapchain(ps->format);
            if (wantEdgeDirect || wantFsrDirect) {
                ps->realViews.resize(ps->realImages.size());
                bool viewsOk = true;
                for (size_t i = 0; i < ps->realImages.size(); ++i) {
                    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                    vci.image = ps->realImages[i];
                    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    vci.format = ps->format;
                    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    vci.subresourceRange.levelCount = 1;
                    vci.subresourceRange.layerCount = 1;
                    if (presentCtx->vk.CreateImageView(presentCtx->device, &vci, nullptr,
                                                       &ps->realViews[i]) != VK_SUCCESS) {
                        viewsOk = false;
                        break;
                    }
                }
                if (!viewsOk) {
                    for (VkImageView v : ps->realViews) {
                        if (v && presentCtx->vk.DestroyImageView)
                            presentCtx->vk.DestroyImageView(presentCtx->device, v, nullptr);
                    }
                    ps->realViews.clear();
                } else {
                    ps->edgeDirectToSwapchain = wantEdgeDirect;
                    ps->fsrDirectToSwapchain = wantFsrDirect;
                    ps->fsrDirectSwizzleBgra = EdgePass::formatNeedsBgraSwizzle(ps->format);
                    if (wantEdgeDirect) {
                        logv("[xfsr] edge → swapchain direct (no fullscreen copy), fmt=%u "
                             "swizzleBGRA=%d mode=%s\n",
                             static_cast<unsigned>(ps->format), ps->fsrDirectSwizzleBgra ? 1 : 0,
                             ps->edge.perfMode ? "perf" : "quality");
                    }
                    if (wantFsrDirect) {
                        logv("[xfsr] FSR → swapchain direct (no fullscreen copy), fmt=%u "
                             "swizzleBGRA=%d rcas=%d\n",
                             static_cast<unsigned>(ps->format), ps->fsrDirectSwizzleBgra ? 1 : 0,
                             ps->fsr.enableRcas ? 1 : 0);
                    }
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
        if (!preserveAppImages) {
            if (ps->appSwapchain && ps->appSwapchain != ps->realSwapchain && !ps->opaqueAppHandle) {
                WsiFns dwsi = wsiFnsFor(dd, false);
                if (dwsi.DestroySwapchainKHR)
                    dwsi.DestroySwapchainKHR(dd->dgpu.device, ps->appSwapchain, nullptr);
            }
            if (!ps->opaqueAppHandle) ps->appSwapchain = VK_NULL_HANDLE;
            for (auto& img : ps->appImages) img.destroy();
            ps->appImages.clear();
        }
        ps->offloadIgpu = false;
        ps->zinkOffloadRoundtrip = false;
        ps->pipelineReady = false;
        return false;
    }
}

// Hotkey F4: same-GPU FSR ↔ round-trip offload without touching the dGPU WSI
// (destroying a shared app/real handle mid-run SIGSEGVs MangoHud/Zink).
// One-way (iGPU WSI) is not switched live — restart with --one-way.
bool reconfigureOffloadPresentPath(ProxySwapchain* ps) {
    if (!ps || !ps->dev || !ps->pipelineReady) return false;
    DeviceData* dd = ps->dev;
    if (presentsOnIgpu(ps) || ps->opaqueAppHandle) {
        std::fprintf(stderr,
                     "[xfsr] live offload switch skipped (one-way WSI) — restart to change mode\n");
        return false;
    }
    if (ps->renderW == ps->displayW && ps->renderH == ps->displayH) {
        std::fprintf(stderr, "[xfsr] live offload skipped (no scale — nothing to transfer)\n");
        return false;
    }

    DeviceContext& presentCtx = dd->dgpu;
    waitPresentRing(ps, presentCtx);
    waitOffloadIdle(dd);

    const bool wantOffload = xfsr_ctrl::enabled() ? xfsr_ctrl::peek().offload : offloadEnabled();
    const xfsr_ctrl::Upscaler wantUps = resolveUpscaler();
    const bool wantFsr = wantUps == xfsr_ctrl::Upscaler::Fsr1;
    const bool wantNis = wantUps == xfsr_ctrl::Upscaler::Nis;
    const bool wantEdge = wantUps == xfsr_ctrl::Upscaler::Edge;
    const bool wantRcas = xfsr_ctrl::enabled() ? xfsr_ctrl::peek().rcas : true;

    ps->fsr.destroy();
    ps->nis.destroy();
    ps->edge.destroy();
    if (dd->xferReady) {
        dd->xfer.destroy();
        dd->xferReady = false;
    }
    if (dd->xferBackReady) {
        dd->xferBack.destroy();
        dd->xferBackReady = false;
    }

    try {
        DeviceContext* upCtx = &dd->dgpu;
        if (wantOffload) {
            if (!tryInitCompanionIgpu(dd, ps->surface)) {
                std::fprintf(stderr, "[xfsr] live offload: iGPU unavailable — staying same-GPU\n");
                ps->offloadIgpu = false;
                ps->zinkOffloadRoundtrip = false;
                ps->fsr.init(&dd->dgpu);
                upCtx = &dd->dgpu;
            } else {
                // Always round-trip for live toggle (dGPU keeps window WSI).
                ps->offloadIgpu = true;
                ps->zinkOffloadRoundtrip = true;
                dd->forceRoundTripOffload = true;
                dd->dgpu.loadDispatch();
                dd->igpu.loadDispatch();
                dd->xfer.init(&dd->dgpu, &dd->igpu);
                dd->xferReady = true;
                dd->xfer.resize(ps->renderW, ps->renderH, ps->format, 4);
                dd->xferBack.init(&dd->igpu, &dd->dgpu);
                dd->xferBackReady = true;
                dd->xferBack.resize(ps->displayW, ps->displayH, VK_FORMAT_R8G8B8A8_UNORM, 4);
                ps->fsr.init(&dd->igpu);
                upCtx = &dd->igpu;
            }
        } else {
            ps->offloadIgpu = false;
            ps->zinkOffloadRoundtrip = false;
            dd->dgpu.loadDispatch();
            ps->fsr.init(&dd->dgpu);
            upCtx = &dd->dgpu;
        }
        ps->useFsr = wantFsr;
        ps->useNis = wantNis;
        ps->useEdge = wantEdge;
        ps->fsr.enableRcas = wantRcas;
        // Always allocate FSR/NIS/edge outputs — F3 can flip without reconfigure.
        ps->fsr.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
        try {
            ps->nis.init(upCtx);
            ps->nis.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[xfsr] NIS init failed: %s\n", e.what());
            ps->nis.destroy();
            if (ps->useNis) {
                ps->useNis = false;
                ps->useEdge = true;
            }
        }
        try {
            ps->edge.init(upCtx);
            if (xfsr_ctrl::enabled()) {
                const auto peek = xfsr_ctrl::peek();
                ps->edge.setSharpness(peek.edgeSharpness);
                ps->edge.setPerfMode(peek.edgePerf);
            }
            ps->edge.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[xfsr] Edge init failed: %s\n", e.what());
            ps->edge.destroy();
            if (ps->useEdge) {
                ps->useEdge = false;
                ps->useFsr = true;
            }
        }
        // Companion create can clobber global volk — refresh present-device dispatch.
        dd->dgpu.loadDispatch();
        if (ps->offloadIgpu) dd->igpu.loadDispatch();
        // Present ring + real WSI stay on dGPU for both modes.
        std::fprintf(stderr,
                     "[xfsr] reconfigured live: offload=%s (round-trip) upscaler=%s xfer=%s\n",
                     ps->offloadIgpu ? "on" : "off",
                     ps->useEdge ? "edge" : (ps->useNis ? "nis" : (ps->useFsr ? "fsr1" : "blit")),
                     ps->offloadIgpu ? FrameTransfer::backendName(dd->xfer.backend) : "n/a");
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xfsr] live offload reconfigure failed: %s\n", e.what());
        // Best-effort same-GPU recovery.
        ps->offloadIgpu = false;
        ps->zinkOffloadRoundtrip = false;
        try {
            ps->fsr.destroy();
            ps->nis.destroy();
            ps->edge.destroy();
            ps->fsr.init(&dd->dgpu);
            ps->useFsr = wantFsr;
            ps->useNis = wantNis;
            ps->useEdge = wantEdge;
            ps->fsr.enableRcas = wantRcas;
            ps->fsr.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
            if (wantNis) {
                ps->nis.init(&dd->dgpu);
                ps->nis.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
            }
            if (wantEdge) {
                ps->edge.init(&dd->dgpu);
                if (xfsr_ctrl::enabled()) {
                    const auto peek = xfsr_ctrl::peek();
                    ps->edge.setSharpness(peek.edgeSharpness);
                    ps->edge.setPerfMode(peek.edgePerf);
                }
                ps->edge.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
            }
        } catch (...) {
        }
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
    xfsr_ctrl::initFromEnv();
    id->scale = xfsr_ctrl::enabled() ? xfsr_ctrl::peek().scale : presetScaleFromEnv();
    applyDisplayExtentHint(&id->hintW, &id->hintH);
    // Keep hint in realW only for caps-shrink seeding; queryTrueSurfaceExtent ignores
    // it until a concrete compositor extent is seen (avoids 1080p WSI on tiny windows).
    id->realW = id->hintW;
    id->realH = id->hintH;
    id->realFromSurface = false;
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
    id->GetPhysicalDeviceSurfaceCapabilities2KHR =
        getInstProc<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
            id, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
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

    // Always print once so Steam/Proton consoles show the layer actually loaded
    // (logv is gated on XFSR_VERBOSE and was easy to miss when injection failed).
    std::fprintf(stderr,
                 "[xfsr] layer active: scale=%.2f displayHint=%s%ux%u offload=%s\n",
                 id->scale,
                 (id->realW && id->realH) ? "" : "none ",
                 id->realW, id->realH,
                 (std::getenv("XFSR_OFFLOAD") && std::getenv("XFSR_OFFLOAD")[0] == '1') ? "1"
                                                                                         : "0");
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

// Mutate surface caps so DXVK/Zink create at FSR render size; proxy presents at win size.
void applyCapsShrink(InstanceData* id, VkPhysicalDevice physicalDevice,
                     VkSurfaceCapabilitiesKHR* caps) {
    if (!id || !caps || !fsrEnabled() || g_bypassCapsShrink > 0 || !capsShrinkEnabled()) return;

    const uint32_t cw = caps->currentExtent.width;
    const uint32_t ch = caps->currentExtent.height;
    if (cw != 0xFFFFFFFFu && ch != 0xFFFFFFFFu && cw > 0 && ch > 0) {
        id->realW = cw;
        id->realH = ch;
        id->realFromSurface = true;
    }

    // Caps shrink on any GPU — CoD4/Proton often selects the iGPU (discrete=0).

    uint32_t winW = 0, winH = 0;
    if (cw != 0xFFFFFFFFu && ch != 0xFFFFFFFFu && cw > 0 && ch > 0) {
        winW = cw;
        winH = ch;
    } else if (id->realFromSurface && id->realW > 0 && id->realH > 0) {
        winW = id->realW;
        winH = id->realH;
    } else if (id->hintW > 0 && id->hintH > 0) {
        // DXVK often queries caps2 before a concrete X11 currentExtent; seed from
        // XFSR_DISPLAY_EXTENT so max clamp still forces a render-sized swapchain.
        winW = id->hintW;
        winH = id->hintH;
    }
    if (winW == 0 || winH == 0) return;

    uint32_t rw = 0, rh = 0;
    computeRenderSize(winW, winH, id->scale, &rw, &rh);
    // DXVK pickImageExtent: if currentExtent is defined it returns it as-is; if
    // UINT_MAX it clamps desired to min/max. D3D9 setSurfaceExtent(preferred=native)
    // still clamps when current is UINT_MAX — so advertise undefined current + max=render.
    // Zink/Mesa: UINT_MAX current is treated as a literal extent → OOM / failed create;
    // it needs a concrete currentExtent=render so CreateSwapchain is 768 and the proxy
    // can upscale (otherwise identity 1920 + GL shim remap → postage stamp).
    if (zinkActive()) {
        caps->currentExtent = {rw, rh};
    } else {
        caps->currentExtent = {0xFFFFFFFFu, 0xFFFFFFFFu};
    }
    caps->minImageExtent = {rw, rh};
    caps->maxImageExtent = {rw, rh};
    id->capsShrunk = true;
    if (winW != id->lastLogCw || winH != id->lastLogCh || rw != id->lastLogRw ||
        rh != id->lastLogRh) {
        id->lastLogCw = winW;
        id->lastLogCh = winH;
        id->lastLogRw = rw;
        id->lastLogRh = rh;
        std::fprintf(stderr,
                     "[xfsr] caps shrink %ux%u → render %ux%u (max clamped; present %ux%u)%s\n",
                     winW, winH, rw, rh, winW, winH, zinkActive() ? " [zink]" : "");
    }
}

InstanceData* anyInstance() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_instances.empty() ? nullptr : g_instances.begin()->second;
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
    InstanceData* id = anyInstance();
    if (!id || !id->GetPhysicalDeviceSurfaceCapabilitiesKHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r =
        id->GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities);
    if (r == VK_SUCCESS && pSurfaceCapabilities)
        applyCapsShrink(id, physicalDevice, pSurfaceCapabilities);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities) {
    InstanceData* id = anyInstance();
    if (!id) return VK_ERROR_INITIALIZATION_FAILED;
    if (!id->GetPhysicalDeviceSurfaceCapabilities2KHR) {
        // Fallback: some ICDs only expose the v1 entry.
        if (!pSurfaceInfo || !pSurfaceCapabilities || !id->GetPhysicalDeviceSurfaceCapabilitiesKHR)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkResult r = id->GetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice, pSurfaceInfo->surface, &pSurfaceCapabilities->surfaceCapabilities);
        if (r == VK_SUCCESS) applyCapsShrink(id, physicalDevice, &pSurfaceCapabilities->surfaceCapabilities);
        return r;
    }
    VkResult r = id->GetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo,
                                                              pSurfaceCapabilities);
    if (r == VK_SUCCESS && pSurfaceCapabilities)
        applyCapsShrink(id, physicalDevice, &pSurfaceCapabilities->surfaceCapabilities);
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
    // Always inject transfer exts on discrete when FSR is on — needed for live F4
    // offload (CreateDevice already happened with offload=0 otherwise).
    const bool wantXferExts =
        propsEarly.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && fsrEnabled();

    VkDeviceCreateInfo ci = *pCreateInfo;
    std::vector<const char*> extPtrs;
    std::vector<std::string> newlyInjected;
    if (wantXferExts && pCreateInfo) {
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            extPtrs.push_back(pCreateInfo->ppEnabledExtensionNames[i]);

        auto alreadyEnabled = [&](const char* name) {
            for (const char* e : extPtrs) {
                if (std::strcmp(e, name) == 0) return true;
            }
            return false;
        };
        auto availHas = [&](const char* name, const std::vector<VkExtensionProperties>& avail) {
            for (const auto& e : avail) {
                if (std::strcmp(e.extensionName, name) == 0) return true;
            }
            return false;
        };

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> avail(extCount);
        if (extCount)
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, avail.data());

        const char* candidates[] = {
            VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        };
        for (const char* name : candidates) {
            if (alreadyEnabled(name) || !availHas(name, avail)) continue;
            // dma_buf requires external_memory_fd
            if (std::strcmp(name, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0 &&
                !alreadyEnabled(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                !availHas(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, avail)) {
                continue;
            }
            extPtrs.push_back(name);
            newlyInjected.emplace_back(name);
        }
        if (!newlyInjected.empty()) {
            ci.enabledExtensionCount = static_cast<uint32_t>(extPtrs.size());
            ci.ppEnabledExtensionNames = extPtrs.data();
        }
    }

    VkResult r = create(physicalDevice, &ci, pAllocator, pDevice);
    if (r != VK_SUCCESS && !newlyInjected.empty()) {
        logv("[xfsr] CreateDevice with transfer exts failed (%d); retrying without\n",
             static_cast<int>(r));
        r = create(physicalDevice, pCreateInfo, pAllocator, pDevice);
        newlyInjected.clear();
    }
    if (r != VK_SUCCESS) return r;

    auto* dd = new DeviceData();
    dd->device = *pDevice;
    dd->physical = physicalDevice;
    dd->inst = id;
    dd->gdpa = gdpa;
    for (const auto& name : newlyInjected) {
        dd->injectedExtNames.push_back(name);
        logv("[xfsr] injected %s on discrete device\n", name.c_str());
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
    dd->CreateSampler = getDevProc<PFN_vkCreateSampler>(dd, "vkCreateSampler");

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
    std::fprintf(stderr, "[xfsr] device %s (%s)\n", props.deviceName,
                 dd->isDiscrete ? "discrete" : "integrated/other");
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL xfsr_CreateSampler(VkDevice device,
                                                  const VkSamplerCreateInfo* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator,
                                                  VkSampler* pSampler) {
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_devices.find(keyOf(device));
        if (it != g_devices.end()) dd = it->second;
    }
    if (!dd || !dd->CreateSampler) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pCreateInfo) return dd->CreateSampler(device, pCreateInfo, pAllocator, pSampler);

    const float inject = samplerLodBiasInject();
    // Skip: disabled, shadow/compare samplers, single-mip (no LOD selection).
    if (inject == 0.f || pCreateInfo->compareEnable ||
        pCreateInfo->maxLod <= pCreateInfo->minLod + 0.001f) {
        return dd->CreateSampler(device, pCreateInfo, pAllocator, pSampler);
    }

    VkSamplerCreateInfo ci = *pCreateInfo;
    ci.mipLodBias = std::clamp(ci.mipLodBias + inject, -16.f, 16.f);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) == 0) {
        std::fprintf(stderr, "[xfsr] CreateSampler mipLodBias inject %+.3f (scale-aware)\n",
                     inject);
    }
    return dd->CreateSampler(device, &ci, pAllocator, pSampler);
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
        waitOffloadIdle(dd);
        xfsr_ctrl::destroyOverlay(&dd->overlay);
        // Overlay sync: semaphore/fence on app device; cmd buffer in dgpu.cmdPool.
        if (g_overlaySyncDd == dd) {
            if (dd->DestroySemaphore && g_overlayDoneSem)
                dd->DestroySemaphore(dd->device, g_overlayDoneSem, nullptr);
            if (dd->DestroyFence && g_overlayFence)
                dd->DestroyFence(dd->device, g_overlayFence, nullptr);
            g_overlayDoneSem = VK_NULL_HANDLE;
            g_overlayFence = VK_NULL_HANDLE;
            g_overlayCmd = VK_NULL_HANDLE;
            g_overlaySyncDd = nullptr;
            g_overlaySyncReady = false;
        }
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (auto it = g_passthrough.begin(); it != g_passthrough.end();) {
                if (it->second.dd == dd)
                    it = g_passthrough.erase(it);
                else
                    ++it;
            }
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

// True window size from the ICD (never the shrunk currentExtent). Updates inst->realW/H
// only when the compositor reports a concrete currentExtent.
// Wayland often reports currentExtent=0xFFFFFFFF — do NOT fall back to XFSR_DISPLAY_EXTENT
// here (that forced a 1080p WSI onto vkcube's 500² window → NVIDIA DEVICE_LOST).
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
        // Only reuse a size that came from a prior *concrete* compositor query —
        // never the XFSR_DISPLAY_EXTENT seed alone (see applyDisplayExtentHint).
        if (id->realFromSurface && id->realW > 0 && id->realH > 0) {
            w = id->realW;
            h = id->realH;
        }
    }
    if (w == 0 || h == 0) return false;
    id->realW = w;
    id->realH = h;
    id->realFromSurface = true;
    *outW = w;
    *outH = h;
    return true;
}

// XFSR_DISPLAY_EXTENT / OUTPUT_* — used when the app creates a near-fullscreen swapchain
// on Wayland (UINT_MAX currentExtent) so force-scale still knows the target present size.
// Re-read env: the GL shim may set XFSR_DISPLAY_* from wl_egl_window before shrink.
bool displayExtentHint(InstanceData* id, uint32_t* outW, uint32_t* outH) {
    if (!id || !outW || !outH) return false;
    uint32_t ew = 0, eh = 0;
    applyDisplayExtentHint(&ew, &eh);
    if (ew > 0 && eh > 0) {
        id->hintW = ew;
        id->hintH = eh;
    }
    if (id->hintW > 0 && id->hintH > 0) {
        *outW = id->hintW;
        *outH = id->hintH;
        return true;
    }
    return false;
}

// Adopt app device into dd->dgpu so identity overlay can submit (no FSR proxy).
bool ensureDgpuAdopted(DeviceData* dd) {
    if (!dd) return false;
    if (dd->dgpuReady) return true;
    try {
        dd->dgpu.adopt(dd->physical, dd->device, dd->graphicsFamily, dd->graphicsFamily);
        dd->dgpuReady = true;
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xfsr] overlay adopt failed: %s\n", e.what());
        return false;
    }
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
    // F10 overlay blits onto the WSI — needs TRANSFER_DST (bench only requests color).
    // Adopt early so exclusive-fullscreen identity (CoD/DXVK) can still draw the strip.
    const bool wantHud = xfsr_ctrl::enabled();
    if (wantHud) (void)ensureDgpuAdopted(dd);
    const VkImageUsageFlags usageWithDst = ci.imageUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool gotDst = false;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (wantHud && usageWithDst != ci.imageUsage) {
        VkSwapchainCreateInfoKHR ciDst = ci;
        ciDst.imageUsage = usageWithDst;
        r = dd->CreateSwapchainKHR(dd->device, &ciDst, pAllocator, pSwapchain);
        gotDst = (r == VK_SUCCESS);
        if (!gotDst && !g_loggedOverlaySkip) {
            std::fprintf(stderr,
                         "[xfsr] TRANSFER_DST swapchain create failed (%d) — overlay may be "
                         "unavailable\n",
                         static_cast<int>(r));
        }
    }
    if (!gotDst) r = dd->CreateSwapchainKHR(dd->device, &ci, pAllocator, pSwapchain);
    if (r == VK_SUCCESS && pSwapchain && *pSwapchain && dd->GetSwapchainImagesKHR) {
        PassthroughSc pt{};
        pt.dd = dd;
        pt.w = ci.imageExtent.width;
        pt.h = ci.imageExtent.height;
        pt.transferDst = gotDst || ((ci.imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0);
        uint32_t n = 0;
        dd->GetSwapchainImagesKHR(dd->device, *pSwapchain, &n, nullptr);
        pt.images.resize(n);
        if (n) dd->GetSwapchainImagesKHR(dd->device, *pSwapchain, &n, pt.images.data());
        std::lock_guard<std::mutex> lock(g_mu);
        // Retire old handle if the app recycled via oldSwapchain.
        if (ci.oldSwapchain != VK_NULL_HANDLE) g_passthrough.erase(keyOf(ci.oldSwapchain));
        g_passthrough[keyOf(*pSwapchain)] = std::move(pt);
        if (wantHud && pt.transferDst)
            logv("[xfsr] identity overlay armed %ux%u (TRANSFER_DST)\n", pt.w, pt.h);
    }
    return r;
}

bool ensureOverlaySync(DeviceData* dd) {
    if (g_overlaySyncReady && g_overlaySyncDd == dd) return true;
    if (!dd || !ensureDgpuAdopted(dd) || !dd->CreateSemaphore || !dd->CreateFence) return false;
    // Device changed (DXVK recreate) — previous sync objects are dead.
    g_overlayDoneSem = VK_NULL_HANDLE;
    g_overlayFence = VK_NULL_HANDLE;
    g_overlayCmd = VK_NULL_HANDLE;
    g_overlaySyncReady = false;
    g_overlaySyncDd = nullptr;
    dd->dgpu.loadDispatch();
    auto& d = dd->dgpu.vk;
    if (!dd->dgpu.cmdPool || !d.AllocateCommandBuffers) return false;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (dd->CreateSemaphore(dd->device, &sci, nullptr, &g_overlayDoneSem) != VK_SUCCESS)
        return false;
    if (dd->CreateFence(dd->device, &fci, nullptr, &g_overlayFence) != VK_SUCCESS) return false;
    // Own cmd buffer — never resetAndBegin() (that steals dgpu.fence → hang on toast).
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = dd->dgpu.cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (d.AllocateCommandBuffers(dd->dgpu.device, &cai, &g_overlayCmd) != VK_SUCCESS) return false;
    g_overlaySyncDd = dd;
    g_overlaySyncReady = true;
    return true;
}

// Identity present has no FSR proxy — still blit the F10/toast strip onto the WSI.
// Returns true if this path presented (outPr set); false → caller should present normally.
bool queuePresentWithOverlay(DeviceData* dd, VkQueue queue, const VkPresentInfoKHR* pPresentInfo,
                             const char* status, VkResult* outPr) {
    if (!pPresentInfo || !status || !status[0] || !outPr) return false;
    if (pPresentInfo->swapchainCount != 1 || !pPresentInfo->pSwapchains ||
        !pPresentInfo->pImageIndices)
        return false;

    PassthroughSc pt{};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_passthrough.find(keyOf(pPresentInfo->pSwapchains[0]));
        if (it == g_passthrough.end()) {
            if (!g_loggedOverlaySkip) {
                g_loggedOverlaySkip = true;
                std::fprintf(stderr,
                             "[xfsr] overlay skipped — swapchain not in identity cache "
                             "(proxy path or foreign WSI)\n");
            }
            return false;
        }
        pt = it->second;
    }
    if (pt.dd) dd = pt.dd;
    if (!dd || !dd->QueueSubmit || !dd->QueuePresentKHR) return false;
    if (!ensureDgpuAdopted(dd)) return false;

    const uint32_t idx = pPresentInfo->pImageIndices[0];
    if (idx >= pt.images.size() || pt.w < 64 || pt.h < 32) return false;
    if (!pt.transferDst) {
        if (!g_loggedOverlaySkip) {
            g_loggedOverlaySkip = true;
            std::fprintf(stderr,
                         "[xfsr] overlay skipped — swapchain has no TRANSFER_DST "
                         "(identity present)\n");
        }
        return false;
    }
    if (!ensureOverlaySync(dd)) {
        if (!g_loggedOverlaySkip) {
            g_loggedOverlaySkip = true;
            std::fprintf(stderr, "[xfsr] overlay skipped — sync objects failed\n");
        }
        return false;
    }

    dd->dgpu.loadDispatch();
    auto& d = dd->dgpu.vk;
    if (!d.ResetCommandBuffer || !d.BeginCommandBuffer || !d.EndCommandBuffer || !d.QueueSubmit)
        return false;

    if (dd->WaitForFences(dd->device, 1, &g_overlayFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return false;
    if (dd->ResetFences(dd->device, 1, &g_overlayFence) != VK_SUCCESS) return false;
    if (d.ResetCommandBuffer(g_overlayCmd, 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (d.BeginCommandBuffer(g_overlayCmd, &bi) != VK_SUCCESS) return false;

    // Preserve the frame: DXVK left PRESENT_SRC. UNDEFINED would discard the menu/scene
    // and only leave the status blit (CoD4 "overlay but no menu").
    transitionImage(&dd->dgpu, g_overlayCmd, pt.images[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!xfsr_ctrl::recordOverlay(&dd->overlay, &dd->dgpu, g_overlayCmd, pt.images[idx], pt.w, pt.h,
                                  status)) {
        d.EndCommandBuffer(g_overlayCmd);
        // Leave fence signaled so the next attempt can proceed.
        VkSubmitInfo mark{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        d.QueueSubmit(queue ? queue : dd->dgpu.graphicsQueue, 1, &mark, g_overlayFence);
        return false;
    }
    transitionImage(&dd->dgpu, g_overlayCmd, pt.images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    if (d.EndCommandBuffer(g_overlayCmd) != VK_SUCCESS) return false;

    // Submit on the present queue (DXVK may not use graphicsQueue for WSI).
    VkQueue submitQ = queue ? queue : dd->dgpu.graphicsQueue;
    std::vector<VkPipelineStageFlags> stages;
    if (pPresentInfo->waitSemaphoreCount > 0)
        stages.assign(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
    si.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
    si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_overlayCmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_overlayDoneSem;
    if (d.QueueSubmit(submitQ, 1, &si, g_overlayFence) != VK_SUCCESS) return false;

    VkPresentInfoKHR pi = *pPresentInfo;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_overlayDoneSem;
    *outPr = dd->QueuePresentKHR(queue, &pi);
    if (*outPr == VK_SUCCESS || *outPr == VK_SUBOPTIMAL_KHR) xfsr_ctrl::notePresented();
    return true;
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

    // Proxy + FSR on any GPU (iGPU included — CoD4/Proton often picks Vega, not the 1650).
    // Zink needs libxfsr_gl_shim (viewport remap) via xfsr-run LD_PRELOAD.
    // Dual-GPU (--offload) always uses an opaque app handle; same-GPU Zink keeps the real
    // WSI handle + separate app images (shim fixes GL viewport).
    const bool wantProxy =
        dd->inst && dd->inst->volkReady && fsrEnabled() && pCreateInfo && pCreateInfo->surface;

    if (!wantProxy) {
        std::fprintf(stderr,
                     "[xfsr] CreateSwapchain %ux%u passthrough (proxy off: volk=%d fsr=%d "
                     "discrete=%d zink=%d)\n",
                     pCreateInfo ? pCreateInfo->imageExtent.width : 0,
                     pCreateInfo ? pCreateInfo->imageExtent.height : 0,
                     dd->inst && dd->inst->volkReady ? 1 : 0, fsrEnabled() ? 1 : 0,
                     dd->isDiscrete ? 1 : 0, zinkActive() ? 1 : 0);
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

    // Apply hotkey preset scale here (not during present — live caps-shrink → Zink recreate crash).
    if (dd->inst && xfsr_ctrl::enabled()) {
        dd->inst->scale = xfsr_ctrl::peek().scale;
    }

    // App sizes framebuffers to imageExtent — proxy images must match that (or DEVICE_LOST).
    // Real WSI must match the *window* size. Never invent display=app*scale: on Wayland
    // that grows the wl_surface and doubles every recreate (500→1000→2000→…).
    const uint32_t appW = pCreateInfo->imageExtent.width;
    const uint32_t appH = pCreateInfo->imageExtent.height;
    // Zink occasionally passes UINT_MAX after caps advertised undefined current — reject.
    if (appW == 0 || appH == 0 || appW >= 0xFFFFu || appH >= 0xFFFFu) {
        delete ps;
        std::fprintf(stderr,
                     "[xfsr] CreateSwapchain refused bogus extent %ux%u\n", appW, appH);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    ps->renderW = appW;
    ps->renderH = appH;

    uint32_t surfW = 0, surfH = 0;
    bool haveSurf = queryTrueSurfaceExtent(dd, pCreateInfo->surface, &surfW, &surfH);
    const float scale = dd->inst ? dd->inst->scale : 1.5f;
    uint32_t hintW = 0, hintH = 0;
    const bool haveHint = displayExtentHint(dd->inst, &hintW, &hintH);

    // Prefer compositor-measured size; never the DISPLAY_EXTENT hint alone
    // (that made glmark2 800x600 look like a shrunk 1080p → postage stamp).
    if (!haveSurf && dd->inst && dd->inst->realFromSurface && dd->inst->realW > 0 &&
        dd->inst->realH > 0) {
        surfW = dd->inst->realW;
        surfH = dd->inst->realH;
        haveSurf = true;
    }
    // Caps-honored only when app extent ≈ expected render size for this scale.
    // glmark2's default 800x600 window is NOT ultra's 768x432 render of 1080p.
    auto looksLikeRenderSize = [&](uint32_t aw, uint32_t ah, uint32_t dw, uint32_t dh) {
        uint32_t rw = 0, rh = 0;
        computeRenderSize(dw, dh, scale, &rw, &rh);
        const int tol = 16;
        return std::abs(static_cast<int>(aw) - static_cast<int>(rw)) <= tol &&
               std::abs(static_cast<int>(ah) - static_cast<int>(rh)) <= tol;
    };
    if (haveSurf && (appW < surfW || appH < surfH) &&
        looksLikeRenderSize(appW, appH, surfW, surfH)) {
        // App honored caps shrink (native VK or Zink+shim) — present at true window.
        ps->displayW = surfW;
        ps->displayH = surfH;
        ps->renderW = appW;
        ps->renderH = appH;
        if (zinkActive()) {
            logv("[xfsr] zink proxy: render %ux%u → display %ux%u (needs libxfsr_gl_shim)\n", appW,
                 appH, surfW, surfH);
        } else {
            logv("[xfsr] caps-honored proxy: render %ux%u → display %ux%u\n", appW, appH, surfW,
                 surfH);
        }
    } else if (!haveSurf && haveHint && (appW < hintW || appH < hintH) &&
               looksLikeRenderSize(appW, appH, hintW, hintH)) {
        // Wayland Zink: wl_egl_window shim (or caps max clamp) created at render size
        // before any concrete compositor extent — DISPLAY_EXTENT is the present size.
        ps->displayW = hintW;
        ps->displayH = hintH;
        ps->renderW = appW;
        ps->renderH = appH;
        if (dd->inst) {
            dd->inst->realW = hintW;
            dd->inst->realH = hintH;
            dd->inst->realFromSurface = true;
        }
        logv("[xfsr] zink/wayland proxy: render %ux%u → display hint %ux%u\n", appW, appH, hintW,
             hintH);
    } else if (haveSurf && (appW < surfW || appH < surfH) &&
               !looksLikeRenderSize(appW, appH, surfW, surfH)) {
        // Smaller than surface but not our render size → real windowed app (e.g. glmark2
        // 800x600 on a 1080p output). Present at the app/window size, not the output.
        ps->displayW = appW;
        ps->displayH = appH;
        ps->renderW = appW;
        ps->renderH = appH;
        uint32_t rw = 0, rh = 0;
        computeRenderSize(appW, appH, scale, &rw, &rh);
        if (zinkActive() && (rw < appW || rh < appH)) {
            // Caps-shrink should make the next recreate match; for this create keep
            // images == app FB. Identity until Zink re-queries shrunk caps.
            logv("[xfsr] zink windowed %ux%u (surf %ux%u was not render-sized) — wait caps-shrink\n",
                 appW, appH, surfW, surfH);
        } else {
            logv("[xfsr] windowed app %ux%u (ignore surf/hint %ux%u)\n", appW, appH, surfW, surfH);
        }
        if (dd->inst) {
            dd->inst->realW = appW;
            dd->inst->realH = appH;
            dd->inst->realFromSurface = true;
        }
    } else if (forceScaleEnabled() && !zinkActive()) {
        // Full-window create (DXVK / apps that ignore caps): present at window size,
        // render at preset. On Wayland without a concrete extent, only trust the
        // DISPLAY_EXTENT hint when the app is creating a near-fullscreen swapchain
        // (otherwise vkcube 500² + hint 1080p → DEVICE_LOST).
        uint32_t winW = appW;
        uint32_t winH = appH;
        if (haveSurf) {
            winW = std::max(surfW, appW);
            winH = std::max(surfH, appH);
        } else if (haveHint && hintW > 0 && hintH > 0) {
            const bool nearFullscreen =
                appW >= (hintW * 4u) / 5u && appH >= (hintH * 4u) / 5u;
            if (nearFullscreen) {
                winW = hintW;
                winH = hintH;
            }
        }
        uint32_t rw = 0, rh = 0;
        computeRenderSize(winW, winH, scale, &rw, &rh);
        ps->displayW = winW;
        ps->displayH = winH;
        if (dd->inst) {
            dd->inst->realW = winW;
            dd->inst->realH = winH;
            // Wayland: ICD currentExtent stays UINT_MAX forever — the app's
            // imageExtent *is* the window size (Vulkan WSI). Seed so the next
            // GetCaps can shrink. X11 with a concrete surf still prefers haveSurf.
            if (haveSurf || (!haveSurf && winW == appW && winH == appH))
                dd->inst->realFromSurface = true;
        }
        // App images MUST match CreateSwapchain imageExtent — DXVK sizes framebuffers
        // to that extent. Returning smaller GetSwapchainImages (force OVERRIDE) →
        // DEVICE_LOST right after first present (CoD4 D3D9).
        if (appW < winW || appH < winH) {
            // Below window (caps-honored or lower in-game res) — safe to FSR.
            ps->renderW = std::max(appW, 1u);
            ps->renderH = std::max(appH, 1u);
            logv("[xfsr] force-scale: app %ux%u → display %ux%u\n", appW, appH, winW, winH);
        } else if (rw < winW || rh < winH) {
            // Full-window create despite maxImageExtent clamp — stay identity.
            std::fprintf(stderr,
                         "[xfsr] D3D9 swapchain %ux%u (wanted render %ux%u) — identity to "
                         "avoid DEVICE_LOST. Use borderless + in-game res ~%ux%u for FSR.\n",
                         appW, appH, rw, rh, rw, rh);
            ps->renderW = appW;
            ps->renderH = appH;
            // Caps already shrunk; recreate would submit the same native extent.
            if (dd->inst) dd->inst->waylandShrinkKick = true;
        }
    } else {
        // Unknown surface: trust app extent. Do not invent app*scale.
        ps->displayW = appW;
        ps->displayH = appH;
        if (dd->inst && appW > 0 && appH > 0) {
            dd->inst->realW = appW;
            dd->inst->realH = appH;
            // Treat app swapchain extent as the window when compositor extent was
            // unknown (typical Wayland UINT_MAX before first configure).
            dd->inst->realFromSurface = true;
        }
    }

    // Identity: render == display — completely bypass the proxy (zero xfsr overhead).
    if (ps->renderW == ps->displayW && ps->renderH == ps->displayH) {
        // First create at native size (DXVK exclusive FS / Wayland / Zink): one
        // OUT_OF_DATE so the next GetCaps (max clamped to render) rebuilds a proxy.
        // Skip when D3D9 already ignored the clamp (waylandShrinkKick set above).
        const bool kickShrink = dd->inst && dd->inst->scale >= 1.01f && capsShrinkEnabled() &&
                                !dd->inst->waylandShrinkKick && appW > 0 && appH > 0 &&
                                (zinkActive() || !haveSurf ||
                                 (haveSurf && appW >= surfW && appH >= surfH));
        if (kickShrink) {
            dd->inst->realW = haveSurf ? surfW : appW;
            dd->inst->realH = haveSurf ? surfH : appH;
            dd->inst->realFromSurface = true;
            dd->inst->waylandShrinkKick = true;
            uint32_t kickRw = 0, kickRh = 0;
            computeRenderSize(dd->inst->realW, dd->inst->realH, dd->inst->scale, &kickRw, &kickRh);
            std::fprintf(stderr,
                         "[xfsr] shrink kick: OUT_OF_DATE → next swapchain render %ux%u "
                         "(was identity %ux%u)\n",
                         kickRw, kickRh, appW, appH);
        }
        logv("[xfsr] identity present (render==display %ux%u) — bypass proxy%s\n", ps->renderW,
             ps->renderH, kickShrink ? " (shrink kick)" : "");
        xfsr_ctrl::publishPresentStatus(kickShrink ? "identity-kick" : "identity", ps->renderW,
                                        ps->renderH, ps->displayW, ps->displayH, 0);
        delete ps;
        VkResult pr = createSwapchainPassthrough(dd, pCreateInfo, pAllocator, pSwapchain);
        if (pr == VK_SUCCESS && kickShrink && pSwapchain && *pSwapchain) {
            std::lock_guard<std::mutex> lock(g_mu);
            g_forceOutOfDateOnce.insert(keyOf(*pSwapchain));
        }
        return pr;
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
            // Serialize with QueuePresent — Zink presents on a worker while the
            // main thread recreates; freeing the proxy under a live present SIGSEGVs.
            std::lock_guard<std::mutex> life(g_proxyLife);
            if (oldPs->offloadIgpu || (dd && (dd->xferReady || dd->xferBackReady)))
                waitOffloadIdle(dd);
            else if (dd && dd->dgpuReady)
                waitOffloadIdle(dd);
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

    // Hold life lock across setup so a concurrent present cannot observe a
    // half-resized FrameTransfer / FSR pass (dual-GPU round-trip).
    {
        std::lock_guard<std::mutex> life(g_proxyLife);
        if (!setupOffloadPipeline(ps, oldPresent, oldShadow)) {
            std::fprintf(stderr,
                         "[xfsr] proxy setup failed (render %ux%u display %ux%u) — passthrough\n",
                         ps->renderW, ps->renderH, ps->displayW, ps->displayH);
            delete ps;
            return createSwapchainPassthrough(dd, pCreateInfo, pAllocator, pSwapchain);
        }
    }

    // Same-GPU / round-trip offload: real ICD swapchain handle.
    // Legacy opaque path (if ever re-enabled): unique serial, never a recycled pointer.
    if (ps->opaqueAppHandle) {
        ps->handle = makeOpaqueSwapchainHandle();
        ps->appSwapchain = ps->handle;
    } else {
        ps->handle = ps->appSwapchain ? ps->appSwapchain : ps->realSwapchain;
    }
    *pSwapchain = ps->handle;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_proxies[keyOf(ps->handle)] = ps;
    }
    publishGlRemap(ps->renderW, ps->renderH);
    std::fprintf(stderr,
                 "[xfsr] proxy ready: handle=%p render=%ux%u display=%ux%u imgs=%zu %s%s%s\n",
                 reinterpret_cast<void*>(ps->handle), ps->renderW, ps->renderH, ps->displayW,
                 ps->displayH, ps->appImages.size(), ps->offloadIgpu ? "dual-GPU" : "same-GPU",
                 ps->useEdge ? "+edge"
                             : (ps->useNis ? "+NIS" : (ps->useFsr ? "+FSR1" : "")),
                 ps->opaqueAppHandle ? " (opaque app handle)" : "");
    {
        const char* mode = "proxy-blit";
        if (ps->useEdge)
            mode = (ps->offloadIgpu && !ps->zinkOffloadRoundtrip) ? "hybrid-edge" : "proxy-edge";
        else if (ps->useNis)
            mode = "proxy-nis";
        else if (ps->useFsr)
            mode = "proxy";
        xfsr_ctrl::publishPresentStatus(mode, ps->renderW, ps->renderH, ps->displayW, ps->displayH,
                                        0);
    }
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
        std::lock_guard<std::mutex> life(g_proxyLife);
        if (ps->offloadIgpu || (dd && (dd->xferReady || dd->xferBackReady)))
            waitOffloadIdle(dd);
        else if (dd && dd->dgpuReady)
            waitOffloadIdle(dd);
        destroyProxy(ps);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_passthrough.erase(keyOf(swapchain));
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
        if (xfsr_ctrl::enabled()) {
            xfsr_ctrl::Snapshot ctrl = xfsr_ctrl::poll();
            if (ctrl.retryOffload && dd) {
                dd->forceRoundTripOffload = false;
                dd->igpuFailed = false;
                xfsr_ctrl::clearRetryOffload();
            }
            // F4 applied in QueuePresent (holds g_proxyLife); don't clear here.
            // F1 preset: push scale then OUT_OF_DATE so the next create honors it.
            if (ctrl.recreateSwapchain) {
                if (dd && dd->inst) dd->inst->scale = ctrl.scale;
                xfsr_ctrl::clearRecreateRequest();
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
        }
        // Fake acquire: pick next app image, but WAIT until the last present/FSR that
        // read it has finished — otherwise the app redraws into an image still sampled
        // by our present cmd (NVIDIA DEVICE_LOST; xfsr_load_vk "wait failed (-4)").
        // Finite timeout: ResetFences + early OUT_OF_DATE left fences unsignaled and
        // UINT64_MAX froze Proton titles after first present (Village on NVIDIA).
        const uint32_t nImg = static_cast<uint32_t>(ps->appImages.size());
        if (nImg == 0) return VK_ERROR_OUT_OF_DATE_KHR;
        ps->acquireIndex = (ps->acquireIndex + 1) % nImg;
        *pImageIndex = ps->acquireIndex;
        if (dd && *pImageIndex < ps->appImgFence.size() && ps->appImgFence[*pImageIndex] &&
            dd->WaitForFences) {
            constexpr uint64_t kAcqFenceTimeoutNs = 1000000000ull; // 1s
            VkFence imgFence = ps->appImgFence[*pImageIndex];
            VkResult wr =
                dd->WaitForFences(dd->device, 1, &imgFence, VK_TRUE, kAcqFenceTimeoutNs);
            if (wr == VK_TIMEOUT) {
                static bool loggedFenceTo = false;
                if (!loggedFenceTo) {
                    loggedFenceTo = true;
                    std::fprintf(stderr,
                                 "[xfsr] appImgFence wait timed out — signaling + OUT_OF_DATE "
                                 "(avoids hard freeze after first present)\n");
                }
                // Unstick: empty submit so the next wait can proceed after recreate.
                if (dd->ResetFences && dd->QueueSubmit) {
                    dd->ResetFences(dd->device, 1, &imgFence);
                    VkSubmitInfo mark{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    dd->QueueSubmit(dd->graphicsQueue, 1, &mark, imgFence);
                }
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
            if (wr != VK_SUCCESS) return wr;
        }
        // Signal sync objects — vkcube waits on the acquire semaphore before drawing.
        // Do NOT touch dd->dgpu.cmd here — present owns that buffer.
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
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_forceOutOfDateOnce.find(keyOf(swapchain));
        if (it != g_forceOutOfDateOnce.end()) {
            g_forceOutOfDateOnce.erase(it);
            logv("[xfsr] wayland shrink kick: OUT_OF_DATE on acquire (recreate at render size)\n");
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
    }
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

    // Hold for the whole proxy present so Create/DestroySwapchain cannot free
    // FrameTransfer / proxy images under us (Zink dual-GPU + MangoHud SIGSEGV).
    std::unique_lock<std::mutex> life(g_proxyLife);

    ProxySwapchain* ps = nullptr;
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ps = proxyOf(pPresentInfo->pSwapchains[0]);
        if (ps) dd = ps->dev;
    }

    // Hotkeys / control file even on identity (no proxy) so preset can take effect
    // on the next CreateSwapchain.
    xfsr_ctrl::Snapshot ctrl{};
    if (xfsr_ctrl::enabled()) {
        ctrl = xfsr_ctrl::poll();
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (!dd) {
                auto pit = g_passthrough.find(keyOf(pPresentInfo->pSwapchains[0]));
                if (pit != g_passthrough.end() && pit->second.dd) dd = pit->second.dd;
            }
            if (!dd && !g_devices.empty()) dd = g_devices.begin()->second;
        }
        if (ctrl.retryOffload && dd) {
            dd->forceRoundTripOffload = false;
            dd->igpuFailed = false;
            xfsr_ctrl::clearRetryOffload();
        }
        // F4: same-GPU ↔ round-trip offload (keeps dGPU WSI). Drop this frame —
        // continuing present after tearing down FSR/xfer calls null ICD entry points.
        if (ctrl.reconfigureOffload && ps && ps->pipelineReady) {
            xfsr_ctrl::clearReconfigureOffload();
            reconfigureOffloadPresentPath(ps);
            life.unlock();
            if (pPresentInfo->pResults) {
                for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
                    pPresentInfo->pResults[i] = VK_SUCCESS;
            }
            return VK_SUCCESS;
        }
        // F1: OUT_OF_DATE only when a proxy is live (scale can apply). Identity /
        // fullscreen-locked benches exit on OUT_OF_DATE — keep running, store scale.
        if (ctrl.recreateSwapchain) {
            if (dd && dd->inst) dd->inst->scale = ctrl.scale;
            xfsr_ctrl::clearRecreateRequest();
            if (ps && ps->pipelineReady) {
                life.unlock();
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
        }
    }

    if (!ps || !ps->pipelineReady || !dd) {
        life.unlock();
        // One-shot OUT_OF_DATE after Wayland identity create so Zink recreates
        // with caps-shrunk extent (see CreateSwapchain zink shrink kick).
        {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_forceOutOfDateOnce.find(keyOf(pPresentInfo->pSwapchains[0]));
            if (it != g_forceOutOfDateOnce.end()) {
                g_forceOutOfDateOnce.erase(it);
                logv("[xfsr] wayland shrink kick: OUT_OF_DATE to recreate at render size\n");
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
        }
        // Identity / passthrough: still draw F10 overlay (proxy path does this above).
        if (dd && xfsr_ctrl::enabled() && ctrl.showOverlay && ctrl.status[0]) {
            VkResult opr = VK_SUCCESS;
            if (queuePresentWithOverlay(dd, queue, pPresentInfo, ctrl.status, &opr)) return opr;
        }
        if (dd && dd->QueuePresentKHR) {
            const VkResult pr = dd->QueuePresentKHR(queue, pPresentInfo);
            if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) xfsr_ctrl::notePresented();
            return pr;
        }
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

    // Live-safe: RCAS + edge sharp/mode + FSR↔blit↔NIS↔edge when passes exist.
    // Preset/adaptive scale (F1) uses recreateSwapchain above; F5 one-way needs restart.
    if (xfsr_ctrl::enabled() && (ps->fsr.ctx || ps->nis.ctx || ps->edge.ctx)) {
        ps->fsr.enableRcas = ctrl.rcas;
        if (ps->edge.ctx) {
            ps->edge.setSharpness(ctrl.edgeSharpness);
            ps->edge.setPerfMode(ctrl.edgePerf);
        }
        const bool scaling = ps->renderW != ps->displayW || ps->renderH != ps->displayH;
        if (scaling) {
            if (ctrl.upscaler == xfsr_ctrl::Upscaler::Fsr1 &&
                (ps->fsr.displayW == 0 || !ps->fsr.easuOut[0].image)) {
                try {
                    waitPresentRing(ps, presentCtx);
                    waitOffloadIdle(dd);
                    ps->fsr.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[xfsr] live FSR enable failed: %s\n", e.what());
                }
            }
            if (ctrl.upscaler == xfsr_ctrl::Upscaler::Nis &&
                (ps->nis.displayW == 0 || !ps->nis.out[0].image)) {
                try {
                    waitPresentRing(ps, presentCtx);
                    waitOffloadIdle(dd);
                    if (!ps->nis.ctx) {
                        DeviceContext* c = ps->offloadIgpu ? &dd->igpu : &dd->dgpu;
                        ps->nis.init(c);
                    }
                    ps->nis.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[xfsr] live NIS enable failed: %s\n", e.what());
                }
            }
            if (ctrl.upscaler == xfsr_ctrl::Upscaler::Edge &&
                (ps->edge.displayW == 0 || !ps->edge.out[0].image)) {
                try {
                    waitPresentRing(ps, presentCtx);
                    waitOffloadIdle(dd);
                    if (!ps->edge.ctx) {
                        DeviceContext* c = ps->offloadIgpu ? &dd->igpu : &dd->dgpu;
                        ps->edge.init(c);
                    }
                    ps->edge.setSharpness(ctrl.edgeSharpness);
                    ps->edge.setPerfMode(ctrl.edgePerf);
                    ps->edge.resize(ps->renderW, ps->renderH, ps->displayW, ps->displayH);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[xfsr] live Edge enable failed: %s\n", e.what());
                }
            }
            ps->useFsr = ctrl.upscaler == xfsr_ctrl::Upscaler::Fsr1 && ps->fsr.displayW != 0 &&
                         ps->fsr.easuOut[0].image;
            ps->useNis = ctrl.upscaler == xfsr_ctrl::Upscaler::Nis && ps->nis.displayW != 0 &&
                         ps->nis.out[0].image;
            ps->useEdge = ctrl.upscaler == xfsr_ctrl::Upscaler::Edge && ps->edge.displayW != 0 &&
                          ps->edge.out[0].image;
        }
    }

    try {
        const GpuImage* frameImg = &ps->appImages[idx];
        bool roundtripUpscaled = false;
        if (ps->zinkOffloadRoundtrip && dd->xferReady && dd->xferBackReady && ps->useFsr) {
            // Async round-trip: pipelined dGPU→iGPU, FSR on ready frame, pipelined iGPU→dGPU.
            // recordDownload accepts PRESENT_SRC (Zink) / SHADER_READ_ONLY (FSR out).
            std::vector<VkPipelineStageFlags> stages;
            if (pPresentInfo->waitSemaphoreCount > 0) {
                stages.assign(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
            const VkSemaphore* waitSems =
                pPresentInfo->waitSemaphoreCount > 0 ? pPresentInfo->pWaitSemaphores : nullptr;
            const bool fused = !OffloadRoundTrip::legacyRequested() &&
                               dd->xfer.usesGpuBuffer() && dd->xferBack.usesGpuBuffer();
            if (fused) {
                // One submit per device per stage; host only waits on frame-old fences.
                frameImg = &OffloadRoundTrip::submit(
                    dd->xfer, dd->xferBack, ps->fsr, ps->appImages[idx],
                    pPresentInfo->waitSemaphoreCount, waitSems,
                    stages.empty() ? nullptr : stages.data(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            } else {
                // Legacy chain (XFSR_RT_LEGACY=1 / memcpy backend): blocking iGPU FSR
                // submit between two independent transfer pipelines (~4 ms/frame floor).
                double memcpyMs = 0, totalMs = 0;
                const GpuImage& onIgpu = dd->xfer.transferPipeline(
                    ps->appImages[idx], memcpyMs, totalMs, pPresentInfo->waitSemaphoreCount,
                    waitSems, stages.empty() ? nullptr : stages.data(),
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

                dd->igpu.resetAndBegin();
                ps->fsr.record(dd->igpu.cmd, onIgpu);
                dd->igpu.endSubmitWait();

                frameImg = &dd->xferBack.transferPipeline(
                    ps->fsr.output(), memcpyMs, totalMs, 0, nullptr, nullptr,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                (void)memcpyMs;
                (void)totalMs;
            }
            roundtripUpscaled = true;
            (void)queue;
        } else if (ps->zinkOffloadRoundtrip && dd->xferReady && dd->xferBackReady && ps->useNis &&
                   ps->nis.ctx) {
            // NIS has no fused OffloadRoundTrip yet — blocking iGPU chain (same as FSR
            // legacy). Never record iGPU NIS into the dGPU present cmd (SIGSEGV).
            std::vector<VkPipelineStageFlags> stages;
            if (pPresentInfo->waitSemaphoreCount > 0) {
                stages.assign(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
            const VkSemaphore* waitSems =
                pPresentInfo->waitSemaphoreCount > 0 ? pPresentInfo->pWaitSemaphores : nullptr;
            double memcpyMs = 0, totalMs = 0;
            const GpuImage& onIgpu = dd->xfer.transferPipeline(
                ps->appImages[idx], memcpyMs, totalMs, pPresentInfo->waitSemaphoreCount, waitSems,
                stages.empty() ? nullptr : stages.data(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

            dd->igpu.resetAndBegin();
            ps->nis.record(dd->igpu.cmd, onIgpu);
            dd->igpu.endSubmitWait();

            frameImg = &dd->xferBack.transferPipeline(
                ps->nis.output(), memcpyMs, totalMs, 0, nullptr, nullptr,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            (void)memcpyMs;
            (void)totalMs;
            roundtripUpscaled = true;
            (void)queue;
        } else if (ps->zinkOffloadRoundtrip && dd->xferReady && dd->xferBackReady && ps->useEdge &&
                   ps->edge.ctx) {
            // Edge + round-trip (non-hybrid only): blocking chain. Hybrid refuses this path.
            std::vector<VkPipelineStageFlags> stages;
            if (pPresentInfo->waitSemaphoreCount > 0) {
                stages.assign(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
            const VkSemaphore* waitSems =
                pPresentInfo->waitSemaphoreCount > 0 ? pPresentInfo->pWaitSemaphores : nullptr;
            double memcpyMs = 0, totalMs = 0;
            const GpuImage& onIgpu = dd->xfer.transferPipeline(
                ps->appImages[idx], memcpyMs, totalMs, pPresentInfo->waitSemaphoreCount, waitSems,
                stages.empty() ? nullptr : stages.data(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

            dd->igpu.resetAndBegin();
            ps->edge.record(dd->igpu.cmd, onIgpu);
            dd->igpu.endSubmitWait();

            frameImg = &dd->xferBack.transferPipeline(
                ps->edge.output(), memcpyMs, totalMs, 0, nullptr, nullptr,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            (void)memcpyMs;
            (void)totalMs;
            roundtripUpscaled = true;
            (void)queue;
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

        // Recycle present cmd slot (may be a prior appImgFence on the same device).
        // Wait only — reset the submit fence immediately before QueueSubmit. Resetting
        // appImgFence then bailing on OUT_OF_DATE left it unsignaled → fake acquire
        // WaitForFences(UINT64_MAX) freezes the app (seen on glmark2 after SUBOPTIMAL).
        dd->dgpu.loadDispatch();
        auto& dgpuVk = dd->dgpu.vk;
        VkFence slotWait = ps->lastCmdFence[slot] ? ps->lastCmdFence[slot] : fence;
        if (slotWait) {
            if (ps->lastCmdFence[slot]) {
                vkCheck(dgpuVk.WaitForFences(dd->dgpu.device, 1, &slotWait, VK_TRUE, UINT64_MAX),
                        "wait cmd slot");
            } else {
                vkCheck(pd.WaitForFences(presentCtx.device, 1, &slotWait, VK_TRUE, UINT64_MAX),
                        "wait");
            }
        }
        VkFence imgRelease = VK_NULL_HANDLE;
        if (idx < ps->appImgFence.size()) imgRelease = ps->appImgFence[idx];
        if (imgRelease && imgRelease != slotWait) {
            vkCheck(dgpuVk.WaitForFences(dd->dgpu.device, 1, &imgRelease, VK_TRUE, UINT64_MAX),
                    "wait app img");
        }

        uint32_t realIdx = 0;
        // Finite timeout when the app owns WSI — UINT64_MAX hangs forever if images
        // were leaked by previously swallowed OUT_OF_DATE presents.
        const uint64_t acqTimeoutNs = appOwnsRealWsi(ps) ? 1000000000ull : UINT64_MAX;
        VkResult acq = wsi.AcquireNextImageKHR(presentCtx.device, ps->realSwapchain, acqTimeoutNs,
                                               acqSem, VK_NULL_HANDLE, &realIdx);
        if (acq == VK_TIMEOUT) {
            static bool loggedTo = false;
            if (!loggedTo) {
                loggedTo = true;
                std::fprintf(stderr,
                             "[xfsr] proxy acquire timed out — returning OUT_OF_DATE "
                             "(recreate WSI)\n");
            }
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            if (appOwnsRealWsi(ps)) {
                // App must recreate — we share the real swapchain handle.
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
            // Opaque dual-GPU: we own real WSI; don't force app recreate.
            static bool loggedAcq = false;
            if (!loggedAcq) {
                loggedAcq = true;
                std::fprintf(stderr, "[xfsr] ignoring VK_ERROR_OUT_OF_DATE_KHR on proxy acquire\n");
            }
            return VK_SUCCESS;
        }
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
            if (presentsOnIgpu(ps) && !dd->forceRoundTripOffload) {
                if (hybridWanted()) {
                    std::fprintf(stderr,
                                 "[xfsr] hybrid: acquire failed (%d) — next recreate drops "
                                 "offload (same-GPU edge)\n",
                                 static_cast<int>(acq));
                    // Disable offload via env so setupOffloadPipeline stays same-GPU.
                    setenv("XFSR_OFFLOAD", "0", 1);
                    unsetenv("XFSR_HYBRID");
                } else {
                    dd->forceRoundTripOffload = true;
                    std::fprintf(stderr,
                                 "[xfsr] offload: acquire failed (%d) on one-way — fall back to "
                                 "round-trip\n",
                                 static_cast<int>(acq));
                }
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
            return acq;
        }

        vkCheck(pd.ResetCommandBuffer(cmd, 0), "reset cmd");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(pd.BeginCommandBuffer(cmd, &bi), "begin cmd");

        const bool directViewsOk = realIdx < ps->realViews.size() &&
                                   ps->realViews[realIdx] != VK_NULL_HANDLE;
        const bool directFsr = ps->useFsr && ps->fsrDirectToSwapchain && directViewsOk;
        const bool directEdge = ps->useEdge && ps->edgeDirectToSwapchain && directViewsOk &&
                                ps->edge.ctx == &presentCtx;
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
            } else if (ps->useNis && ps->nis.ctx == &presentCtx) {
                ps->nis.record(cmd, *frameImg);
                const GpuImage& nisOut = ps->nis.output();
                transitionImage(&presentCtx, cmd, nisOut.image,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                srcImage = nisOut.image;
                srcW = nisOut.width;
                srcH = nisOut.height;
                srcFmt = nisOut.format ? nisOut.format : VK_FORMAT_R8G8B8A8_UNORM;
            } else if (ps->useEdge && ps->edge.ctx == &presentCtx) {
                // Hybrid one-way: edge on iGPU after low-res transfer (no back-haul).
                if (directEdge) {
                    ps->edge.record(cmd, *frameImg, ps->realViews[realIdx],
                                    ps->realImages[realIdx], swizzle);
                    wroteSwapchain = true;
                } else {
                    ps->edge.record(cmd, *frameImg);
                    const GpuImage& edgeOut = ps->edge.output();
                    transitionImage(&presentCtx, cmd, edgeOut.image,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT);
                    srcImage = edgeOut.image;
                    srcW = edgeOut.width;
                    srcH = edgeOut.height;
                    srcFmt = edgeOut.format ? edgeOut.format : VK_FORMAT_R8G8B8A8_UNORM;
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
            } else if (ps->useNis && ps->nis.ctx == &presentCtx) {
                // Only when NIS lives on the present device (same-GPU). Offload NIS
                // must take the round-trip branch above — cross-device record SIGSEGVs.
                transitionImage(&presentCtx, cmd, frameImg->image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT);
                ps->nis.record(cmd, *frameImg);
                const GpuImage& nisOut = ps->nis.output();
                transitionImage(&presentCtx, cmd, nisOut.image,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                srcImage = nisOut.image;
                srcW = nisOut.width;
                srcH = nisOut.height;
                srcFmt = nisOut.format ? nisOut.format : VK_FORMAT_R8G8B8A8_UNORM;
            } else if (ps->useEdge && ps->edge.ctx == &presentCtx) {
                transitionImage(&presentCtx, cmd, frameImg->image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT);
                if (directEdge) {
                    ps->edge.record(cmd, *frameImg, ps->realViews[realIdx],
                                    ps->realImages[realIdx], swizzle);
                    wroteSwapchain = true;
                } else {
                    ps->edge.record(cmd, *frameImg);
                    const GpuImage& edgeOut = ps->edge.output();
                    transitionImage(&presentCtx, cmd, edgeOut.image,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT);
                    srcImage = edgeOut.image;
                    srcW = edgeOut.width;
                    srcH = edgeOut.height;
                    srcFmt = edgeOut.format ? edgeOut.format : VK_FORMAT_R8G8B8A8_UNORM;
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

            if (xfsr_ctrl::enabled() && ctrl.showOverlay && ctrl.status[0]) {
                xfsr_ctrl::recordOverlay(&dd->overlay, &presentCtx, cmd, ps->realImages[realIdx],
                                         ps->displayW, ps->displayH, ctrl.status);
            }

            transitionImage(&presentCtx, cmd, ps->realImages[realIdx],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        } else if (xfsr_ctrl::enabled() && ctrl.showOverlay && ctrl.status[0]) {
            // Direct-to-swapchain FSR left PRESENT_SRC — briefly steal TRANSFER_DST.
            transitionImage(&presentCtx, cmd, ps->realImages[realIdx],
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            xfsr_ctrl::recordOverlay(&dd->overlay, &presentCtx, cmd, ps->realImages[realIdx],
                                     ps->displayW, ps->displayH, ctrl.status);
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
        // Classic iGPU-present / Zink round-trip already waited in the transfer submit.
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
        // Prefer appImgFence when present runs on the app/dGPU device so acquire can
        // wait for FSR/blit to finish reading that image. Fall back to present-ring fence.
        VkFence submitFence = fence;
        if (imgRelease && presentCtx.device == dd->dgpu.device) submitFence = imgRelease;
        if (submitFence == fence) {
            vkCheck(pd.ResetFences(presentCtx.device, 1, &fence), "reset present-ring fence");
        } else {
            vkCheck(dgpuVk.ResetFences(dd->dgpu.device, 1, &submitFence), "reset app img fence");
        }
        vkCheck(pd.QueueSubmit(presentCtx.presentQueue ? presentCtx.presentQueue
                                                       : presentCtx.graphicsQueue,
                               1, &si, submitFence),
                "submit");
        ps->lastCmdFence[slot] = submitFence;
        if (imgRelease && submitFence != imgRelease) {
            // Present on another device: mark image free after a dGPU no-op wait for
            // prior transfer work (round-trip / classic already finished the download).
            vkCheck(dgpuVk.ResetFences(dd->dgpu.device, 1, &imgRelease), "reset app img mark");
            VkSubmitInfo mark{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            vkCheck(dgpuVk.QueueSubmit(dd->dgpu.graphicsQueue, 1, &mark, imgRelease),
                    "signal app img release");
            ps->lastCmdFence[slot] = imgRelease;
        }

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
            if (presentsOnIgpu(ps) && !dd->forceRoundTripOffload) {
                if (hybridWanted()) {
                    std::fprintf(stderr,
                                 "[xfsr] hybrid: present failed — next recreate drops offload "
                                 "(same-GPU edge)\n");
                    setenv("XFSR_OFFLOAD", "0", 1);
                    unsetenv("XFSR_HYBRID");
                } else {
                    dd->forceRoundTripOffload = true;
                    std::fprintf(stderr,
                                 "[xfsr] offload: fall back to round-trip on next swapchain "
                                 "recreate (XFSR_IGPU_PRESENT=0 to skip one-way)\n");
                }
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
            return pr;
        }
        ps->frameSlot = (slot + 1) % ProxySwapchain::kFrames;

        ++ps->presentCount;
        xfsr_ctrl::notePresented();
        if (!dd->loggedFirstPresent) {
            dd->loggedFirstPresent = true;
            const char* mode = "";
            if (ps->zinkOffloadRoundtrip)
                mode = ", dual-GPU FSR round-trip";
            else if (ps->offloadIgpu)
                mode = ps->useEdge ? ", hybrid one-way edge" : ", dual-GPU one-way";
            std::fprintf(stderr, "[xfsr] first present ok (app img %u → display %ux%u%s)\n", idx,
                         ps->displayW, ps->displayH, mode);
        }
        if (ps->presentCount == 1 || ps->presentCount == 30 ||
            (ps->presentCount > 0 && ps->presentCount % 120 == 0)) {
            const char* st = "proxy-blit";
            if (ps->useEdge)
                st = (ps->offloadIgpu && !ps->zinkOffloadRoundtrip) ? "hybrid-edge" : "proxy-edge";
            else if (ps->useNis)
                st = "proxy-nis";
            else if (ps->useFsr)
                st = "proxy";
            xfsr_ctrl::publishPresentStatus(st, ps->renderW, ps->renderH, ps->displayW,
                                            ps->displayH, ps->presentCount);
        }
        if (verboseLog() &&
            (ps->presentCount == 60 || (ps->presentCount > 0 && ps->presentCount % 300 == 0))) {
            std::fprintf(stderr, "[xfsr] presented %u frames (%ux%u → %ux%u %s)\n", ps->presentCount,
                         ps->renderW, ps->renderH, ps->displayW, ps->displayH,
                         ps->useEdge ? "edge" : (ps->useNis ? "NIS" : (ps->useFsr ? "FSR1" : "blit")));
        }
        if (pPresentInfo->pResults) {
            for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
                pPresentInfo->pResults[i] = VK_SUCCESS;
        }
        // Same-GPU: app owns the real WSI — must propagate OUT_OF_DATE or images leak
        // and the next acquire hangs. Opaque dual-GPU: we own WSI; swallow + keep going.
        // SUBOPTIMAL is safe to treat as SUCCESS for both (keep presenting).
        if (pr == VK_ERROR_OUT_OF_DATE_KHR) {
            if (appOwnsRealWsi(ps)) return VK_ERROR_OUT_OF_DATE_KHR;
            static bool loggedOd = false;
            if (!loggedOd) {
                loggedOd = true;
                std::fprintf(stderr, "[xfsr] ignoring VK_ERROR_OUT_OF_DATE_KHR on proxy present\n");
            }
            return VK_SUCCESS;
        }
        if (pr == VK_SUBOPTIMAL_KHR) {
            static bool loggedSub = false;
            if (!loggedSub) {
                loggedSub = true;
                std::fprintf(stderr, "[xfsr] ignoring VK_SUBOPTIMAL_KHR on proxy present\n");
            }
            return VK_SUCCESS;
        }
        return pr;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xfsr] present failed: %s\n", e.what());
        if (presentsOnIgpu(ps) && dd && !dd->forceRoundTripOffload) {
            if (dd->xferReady) {
                waitOffloadIdle(dd);
                dd->xfer.destroy();
                dd->xferReady = false;
            }
            if (hybridWanted()) {
                setenv("XFSR_OFFLOAD", "0", 1);
                unsetenv("XFSR_HYBRID");
                std::fprintf(stderr,
                             "[xfsr] hybrid: present exception — next recreate same-GPU edge\n");
            } else {
                dd->forceRoundTripOffload = true;
                std::fprintf(stderr,
                             "[xfsr] offload: fall back to round-trip on next swapchain recreate "
                             "(XFSR_IGPU_PRESENT=0 to skip one-way)\n");
            }
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
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
    if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_GetPhysicalDeviceSurfaceCapabilities2KHR);
    return nullptr;
}

PFN_vkVoidFunction resolveDevice(const char* pName) {
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(::xfsr_GetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_DestroyDevice);
    if (std::strcmp(pName, "vkCreateSampler") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(xfsr_CreateSampler);
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
