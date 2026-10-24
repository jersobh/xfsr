#include "dual_devices.hpp"

#include <iostream>

static bool hasExtension(VkPhysicalDevice phys, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, exts.data());
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

struct LoaderDeviceCreate {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkLayerCreateDevice createFn = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
};

static void createLogicalDevice(DeviceContext& ctx, VkSurfaceKHR surface,
                                const LoaderDeviceCreate* loader = nullptr) {
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &qCount, families.data());

    for (uint32_t i = 0; i < qCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            if (ctx.graphicsFamily == UINT32_MAX) ctx.graphicsFamily = i;
        }
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physical, i, surface, &present);
        if (present) {
            ctx.canPresent = true;
            if (ctx.presentFamily == UINT32_MAX) ctx.presentFamily = i;
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ctx.graphicsFamily == i) {
                ctx.presentFamily = i;
            }
        }
    }
    if (ctx.graphicsFamily == UINT32_MAX) {
        throw std::runtime_error(std::string("No graphics queue on ") + ctx.props.deviceName);
    }
    if (!ctx.canPresent) {
        ctx.presentFamily = ctx.graphicsFamily;
    }

    float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    std::vector<uint32_t> uniqueFamilies = {ctx.graphicsFamily};
    if (ctx.presentFamily != ctx.graphicsFamily) uniqueFamilies.push_back(ctx.presentFamily);
    for (uint32_t fam : uniqueFamilies) {
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = fam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        qcis.push_back(qci);
    }

    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (!hasExtension(ctx.physical, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        throw std::runtime_error(std::string("Missing swapchain on ") + ctx.props.deviceName);
    }
    // Cross-GPU staging: prefer dma-buf, then shared host import, else memcpy.
    if (hasExtension(ctx.physical, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        ctx.extExternalMemoryHost = true;
    } else {
        std::printf("WARN: %s missing VK_EXT_external_memory_host — memcpy fallback if no dma-buf\n",
                    ctx.props.deviceName);
    }
    if (hasExtension(ctx.physical, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        ctx.extExternalMemoryFd = true;
    }
    if (ctx.extExternalMemoryFd &&
        hasExtension(ctx.physical, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        ctx.extDmaBuf = true;
    }

    VkPhysicalDeviceFeatures features{};
    // FP16 FSR needs shaderFloat16 (1.2) + 16-bit storage (1.1).
    VkPhysicalDevice16BitStorageFeatures storage16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &storage16;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(ctx.physical, &f2);
    ctx.supportsFp16 = f12.shaderFloat16 && storage16.storageBuffer16BitAccess;

    VkPhysicalDevice16BitStorageFeatures enable16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    if (ctx.supportsFp16) {
        enable16.storageBuffer16BitAccess = VK_TRUE;
        enable12.shaderFloat16 = VK_TRUE;
        enable12.pNext = &enable16;
    }

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    dci.ppEnabledExtensionNames = exts.data();
    dci.pEnabledFeatures = &features;
    if (ctx.supportsFp16) dci.pNext = &enable12;

    if (loader && loader->createFn) {
        PFN_vkGetDeviceProcAddr nextGdpa = nullptr;
        vkCheck(loader->createFn(loader->instance, ctx.physical, &dci, nullptr, &ctx.device,
                                 loader->gipa, &nextGdpa),
                "vkLayerCreateDevice");
        ctx.deviceGdpa = nextGdpa;
    } else {
        vkCheck(vkCreateDevice(ctx.physical, &dci, nullptr, &ctx.device), "vkCreateDevice");
    }

    ctx.makeCurrent();
    vkGetDeviceQueue(ctx.device, ctx.graphicsFamily, 0, &ctx.graphicsQueue);
    vkGetDeviceQueue(ctx.device, ctx.presentFamily, 0, &ctx.presentQueue);

    if (ctx.extExternalMemoryHost) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &hostProps;
        vkGetPhysicalDeviceProperties2(ctx.physical, &p2);
        ctx.minHostPointerAlignment = hostProps.minImportedHostPointerAlignment;
        if (ctx.minHostPointerAlignment == 0) ctx.minHostPointerAlignment = 4096;
    }
}

void DeviceContext::loadDispatch() {
    if (!device || vk.loaded) return;
    PFN_vkGetDeviceProcAddr gdpa = deviceGdpa;
    if (!gdpa) gdpa = vkGetDeviceProcAddr;
    vk.load(device, gdpa);
}

void DeviceContext::makeCurrent() const {
    if (!device) return;
    // Prefer per-device DeviceDispatch (ctx->vk). Global volkLoadDevice mutates process-wide
    // pointers and is unsafe when NVIDIA↔Vega switch from multiple threads — layer hot
    // paths must use ctx->vk, not this. Demo / leftover volk callers still need it.
    auto* self = const_cast<DeviceContext*>(this);
    self->loadDispatch();
    if (deviceGdpa) {
        const PFN_vkGetDeviceProcAddr saved = vkGetDeviceProcAddr;
        vkGetDeviceProcAddr = deviceGdpa;
        volkLoadDevice(device);
        vkGetDeviceProcAddr = saved;
    } else {
        volkLoadDevice(device);
    }
}

void DeviceContext::createCommandResources() {
    loadDispatch();
    makeCurrent(); // demo / leftover volk callers
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = graphicsFamily;
    vkCheck(vk.CreateCommandPool(device, &pci, nullptr, &cmdPool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkCheck(vk.AllocateCommandBuffers(device, &cai, &cmd), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCheck(vk.CreateFence(device, &fci, nullptr, &fence), "vkCreateFence");

    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = 8;
    vkCheck(vk.CreateQueryPool(device, &qci, nullptr, &timestampPool), "vkCreateQueryPool");
}

void DeviceContext::adopt(VkPhysicalDevice phys, VkDevice dev, uint32_t graphicsFam,
                          uint32_t presentFam) {
    physical = phys;
    device = dev;
    ownsDevice = false;
    graphicsFamily = graphicsFam;
    presentFamily = presentFam;
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    timestampPeriod = props.limits.timestampPeriod;
    loadDispatch();
    makeCurrent();
    vk.GetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
    vk.GetDeviceQueue(device, presentFamily, 0, &presentQueue);
    canPresent = true;
    extExternalMemoryHost = hasExtension(phys, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    extExternalMemoryFd = hasExtension(phys, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    extDmaBuf = extExternalMemoryFd &&
                hasExtension(phys, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    if (extExternalMemoryHost) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &hostProps;
        vkGetPhysicalDeviceProperties2(phys, &p2);
        minHostPointerAlignment = hostProps.minImportedHostPointerAlignment;
        if (minHostPointerAlignment == 0) minHostPointerAlignment = 4096;
    }
    VkPhysicalDevice16BitStorageFeatures storage16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &storage16;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    supportsFp16 = f12.shaderFloat16 && storage16.storageBuffer16BitAccess;
    createCommandResources();
}

void DeviceContext::createLogical(VkSurfaceKHR surface) {
    ownsDevice = true;
    layerDestroyDevice = nullptr;
    createLogicalDevice(*this, surface, nullptr);
    createCommandResources();
}

void DeviceContext::createLogicalViaLoader(VkInstance instance, VkSurfaceKHR surface,
                                           PFN_vkLayerCreateDevice layerCreateDevice,
                                           PFN_vkGetInstanceProcAddr layerGipa,
                                           PFN_vkLayerDestroyDevice layerDestroy) {
    if (!layerCreateDevice || !layerGipa) {
        throw std::runtime_error("createLogicalViaLoader: missing loader callbacks");
    }
    ownsDevice = true;
    layerDestroyDevice = layerDestroy;
    LoaderDeviceCreate loader{instance, layerCreateDevice, layerGipa};
    createLogicalDevice(*this, surface, &loader);
    createCommandResources();
}

void DeviceContext::destroy() {
    if (!device) return;
    loadDispatch();
    if (ownsDevice && vk.DeviceWaitIdle) vk.DeviceWaitIdle(device);
    if (timestampPool && vk.DestroyQueryPool) vk.DestroyQueryPool(device, timestampPool, nullptr);
    if (fence && vk.DestroyFence) vk.DestroyFence(device, fence, nullptr);
    if (cmdPool && vk.DestroyCommandPool) vk.DestroyCommandPool(device, cmdPool, nullptr);
    if (ownsDevice) {
        if (layerDestroyDevice) {
            layerDestroyDevice(device, nullptr, vk.DestroyDevice ? vk.DestroyDevice : vkDestroyDevice);
        } else if (vk.DestroyDevice) {
            vk.DestroyDevice(device, nullptr);
        }
    }
    timestampPool = VK_NULL_HANDLE;
    fence = VK_NULL_HANDLE;
    cmdPool = VK_NULL_HANDLE;
    cmd = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    physical = VK_NULL_HANDLE;
    layerDestroyDevice = nullptr;
    deviceGdpa = nullptr;
    vk = {};
}

void DeviceContext::waitIdle() const {
    if (!device) return;
    const_cast<DeviceContext*>(this)->loadDispatch();
    if (vk.DeviceWaitIdle) vk.DeviceWaitIdle(device);
}

void DeviceContext::resetAndBegin() {
    // Sync global volk to this device for demo code (scene/present/hud) that still
    // uses volk entry points. Layer hot paths use ctx->vk and do not call this.
    makeCurrent();
    vkCheck(vk.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vkCheck(vk.ResetFences(device, 1, &fence), "vkResetFences");
    vkCheck(vk.ResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vk.BeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
    vk.CmdResetQueryPool(cmd, timestampPool, 0, 8);
}

void DeviceContext::endSubmit(VkFence signalFence) {
    loadDispatch();
    vkCheck(vk.EndCommandBuffer(cmd), "vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkCheck(vk.QueueSubmit(graphicsQueue, 1, &si, signalFence), "vkQueueSubmit");
}

void DeviceContext::waitFence(VkFence f) const {
    if (!device || !f) return;
    const_cast<DeviceContext*>(this)->loadDispatch();
    vkCheck(vk.WaitForFences(device, 1, &f, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

void DeviceContext::endSubmitWait() {
    endSubmit(fence);
    waitFence(fence);
}

void DualDevices::init(VkInstance instance, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) throw std::runtime_error("No Vulkan physical devices");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    for (auto phys : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys, &props);
        std::printf("Found GPU: %s (type=%u vendor=0x%x)\n",
                    props.deviceName, props.deviceType, props.vendorID);

        DeviceContext* slot = nullptr;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && !discrete.physical) {
            slot = &discrete;
        } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && !integrated.physical) {
            slot = &integrated;
        }
        if (!slot) continue;

        slot->physical = phys;
        slot->props = props;
        vkGetPhysicalDeviceMemoryProperties(phys, &slot->memProps);
        slot->timestampPeriod = props.limits.timestampPeriod;
        slot->createLogical(surface);
    }

    if (!discrete.physical || !integrated.physical) {
        throw std::runtime_error(
            "Need both a discrete and an integrated GPU. "
            "Found discrete=" + std::to_string(discrete.physical != nullptr) +
            " integrated=" + std::to_string(integrated.physical != nullptr));
    }

    if (integrated.canPresent) {
        presentDevice = &integrated;
    } else if (discrete.canPresent) {
        presentDevice = &discrete;
    } else {
        throw std::runtime_error("Neither GPU can present to the window surface");
    }

    std::printf("Render GPU (discrete): %s\n", discrete.props.deviceName);
    std::printf("Upscale GPU (integrated): %s\n", integrated.props.deviceName);
    std::printf("Present GPU: %s (canPresent dGPU=%d iGPU=%d)\n",
                presentDevice->props.deviceName,
                discrete.canPresent ? 1 : 0,
                integrated.canPresent ? 1 : 0);
}

bool DualDevices::initIntegratedCompanion(VkInstance instance, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        std::fprintf(stderr, "[xfsr] companion: no physical devices\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    for (auto phys : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys, &props);
        if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
            !(props.vendorID == 0x1002 && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) {
            continue;
        }
        integrated.physical = phys;
        integrated.props = props;
        vkGetPhysicalDeviceMemoryProperties(phys, &integrated.memProps);
        integrated.timestampPeriod = props.limits.timestampPeriod;
        try {
            integrated.createLogical(surface);
        } catch (...) {
            integrated = {};
            continue;
        }
        if (!integrated.canPresent) {
            integrated.destroy();
            integrated = {};
            continue;
        }
        presentDevice = &integrated;
        return true;
    }
    return false;
}

void DualDevices::destroy() {
    discrete.destroy();
    integrated.destroy();
    presentDevice = nullptr;
}
