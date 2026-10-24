/* Loader entry points — kept out of the C++ TU that links volk (symbol clash). */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

PFN_vkVoidFunction VKAPI_CALL xfsr_GetInstanceProcAddr(VkInstance instance, const char* pName);
PFN_vkVoidFunction VKAPI_CALL xfsr_GetDeviceProcAddr(VkDevice device, const char* pName);

#if defined(__GNUC__) && __GNUC__ >= 4
#define XFSR_EXPORT __attribute__((visibility("default")))
#else
#define XFSR_EXPORT
#endif

XFSR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return xfsr_GetInstanceProcAddr(instance, pName);
}

XFSR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return xfsr_GetDeviceProcAddr(device, pName);
}

XFSR_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
