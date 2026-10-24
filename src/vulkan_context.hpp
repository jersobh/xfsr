#pragma once

#include "common.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

struct VulkanContext {
    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    bool validation = false;
    uint32_t width = 1920;
    uint32_t height = 1080;

    void init(uint32_t w, uint32_t h, const char* title);
    void destroy();
    bool shouldClose() const;
    void poll();
};
