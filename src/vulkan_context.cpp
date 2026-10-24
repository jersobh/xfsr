#include "vulkan_context.hpp"

#include <iostream>

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << "[Vulkan] " << data->pMessage << "\n";
    }
    return VK_FALSE;
}

void VulkanContext::init(uint32_t w, uint32_t h, const char* title) {
    width = w;
    height = h;

    if (!glfwInit()) throw std::runtime_error("glfwInit failed");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title, nullptr, nullptr);
    if (!window) throw std::runtime_error("glfwCreateWindow failed");

    vkCheck(volkInitialize(), "volkInitialize");

    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);

    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            validation = true;
            break;
        }
    }
    if (validation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xfsr";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "xfsr";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_2;

    const char* enabledLayers[] = {"VK_LAYER_KHRONOS_validation"};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    if (validation) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = enabledLayers;
    }

    vkCheck(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
    volkLoadInstance(instance);

    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debugCallback;
        vkCheck(vkCreateDebugUtilsMessengerEXT(instance, &dci, nullptr, &debugMessenger),
                "vkCreateDebugUtilsMessengerEXT");
    }

    vkCheck(glfwCreateWindowSurface(instance, window, nullptr, &surface), "glfwCreateWindowSurface");
    std::printf("Vulkan instance ready (validation=%s)\n", validation ? "on" : "off");
}

void VulkanContext::destroy() {
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (debugMessenger) vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    surface = VK_NULL_HANDLE;
    debugMessenger = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    window = nullptr;
}

bool VulkanContext::shouldClose() const {
    return glfwWindowShouldClose(window);
}

void VulkanContext::poll() {
    glfwPollEvents();
}
