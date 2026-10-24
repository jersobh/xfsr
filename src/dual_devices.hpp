#pragma once

#include "common.hpp"
#include "device_dispatch.hpp"
#include <vulkan/vk_layer.h>

struct DeviceContext {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t presentFamily = UINT32_MAX;
    bool canPresent = false;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    float timestampPeriod = 1.0f;
    bool extExternalMemoryHost = false;
    bool supportsFp16 = false;
    bool ownsDevice = true; // false when adopt()'d (app owns VkDevice)
    // When set, destroy() uses the loader callback (companion devices from a layer).
    PFN_vkLayerDestroyDevice layerDestroyDevice = nullptr;
    // Companion devices created outside our layer chain: load dispatch via this GDPA.
    PFN_vkGetDeviceProcAddr deviceGdpa = nullptr;
    VkDeviceSize minHostPointerAlignment = 4096;

    // Per-device table — prefer this over makeCurrent()/global volk on hot paths.
    DeviceDispatch vk{};

    // Non-blocking submit helpers (3-slot pipeline)
    void endSubmit(VkFence signalFence);
    void waitFence(VkFence f) const;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool timestampPool = VK_NULL_HANDLE;

    void createCommandResources();
    void destroy();
    // Ensures vk dispatch is loaded. Still refreshes global volk for demo code that
    // has not been migrated — layer hot paths should use ctx->vk instead.
    void makeCurrent() const;
    void loadDispatch();
    void waitIdle() const;
    void resetAndBegin();
    void endSubmitWait();

    // Bind an already-created VkDevice (app device in the Vulkan layer).
    void adopt(VkPhysicalDevice phys, VkDevice dev, uint32_t graphicsFam, uint32_t presentFam);
    // Create a fresh logical device on this physical (used for companion iGPU).
    void createLogical(VkSurfaceKHR surface);
    // Layer path: create via VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK (avoids LLP_LAYER_22).
    void createLogicalViaLoader(VkInstance instance, VkSurfaceKHR surface,
                                PFN_vkLayerCreateDevice layerCreateDevice,
                                PFN_vkGetInstanceProcAddr layerGipa,
                                PFN_vkLayerDestroyDevice layerDestroy);
};

struct DualDevices {
    DeviceContext discrete;
    DeviceContext integrated;
    DeviceContext* presentDevice = nullptr; // prefers integrated when both can present

    void init(VkInstance instance, VkSurfaceKHR surface);
    // Layer helper: pick integrated GPU and createLogical on it (surface required).
    bool initIntegratedCompanion(VkInstance instance, VkSurfaceKHR surface);
    void destroy();
};
