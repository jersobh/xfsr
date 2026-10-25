#pragma once

// In-game controls for VK_LAYER_XFSR: hotkeys + control file + status overlay.
// Preset / FSR / offload / one-way toggles request a swapchain recreate.

#include "dual_devices.hpp"
#include "gpu_image.hpp"

#include <cstdint>

namespace xfsr_ctrl {

struct Snapshot {
    float scale = 1.5f;
    const char* preset = "quality";
    bool rcas = true;
    bool fsr = true;          // FSR1 EASU (vs blit upscale)
    bool offload = false;     // dual-GPU
    bool oneWay = true;       // prefer iGPU present when offload
    bool oneWayForce = false; // XFSR_IGPU_PRESENT=force (NVIDIA)
    bool overlayPinned = false;
    bool showOverlay = false; // pinned or toast
    bool recreateSwapchain = false; // preset size change (deferred / risky with MH)
    // Offload/one-way: rebuild present path in-place (keep app images — no OUT_OF_DATE).
    bool reconfigureOffload = false;
    // One-shot: clear forceRoundTripOffload / igpuFailed so one-way can retry.
    bool retryOffload = false;
    char status[192]{};
};

// Call once from CreateInstance (or lazily). Safe to call repeatedly.
void initFromEnv();

// XFSR_HUD=0 disables. Default on.
bool enabled();

// Poll X11 hotkeys + control file; update runtime state.
Snapshot poll();

// Current values without polling (for CreateSwapchain).
Snapshot peek();

// Clear the one-shot recreate flag after the layer returns OUT_OF_DATE.
void clearRecreateRequest();

// Clear reconfigureOffload after the layer applied it.
void clearReconfigureOffload();

// Clear retryOffload after the layer applied it.
void clearRetryOffload();

// Force a toast (e.g. after apply).
void showToast(const char* msg);

// GPU overlay: rasterize status into a corner blit (swapchain must be TRANSFER_DST).
struct OverlayGpu {
    DeviceContext* ctx = nullptr;
    GpuImage image;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    char lastText[192]{};
};

void destroyOverlay(OverlayGpu* ov);
// Returns false if overlay skipped (disabled / no text / GPU fail).
bool recordOverlay(OverlayGpu* ov, DeviceContext* ctx, VkCommandBuffer cmd, VkImage swapImage,
                   uint32_t swapW, uint32_t swapH, const char* text);

} // namespace xfsr_ctrl
