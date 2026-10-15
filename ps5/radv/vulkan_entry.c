/* The two entry points win32u takes from libvulkan.so with dlsym
 * (wine/dlls/win32u/vulkan.c, vulkan_init_once): RADV's archive exports its
 * ICD and common entry points under other names. */
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
PFN_vkVoidFunction vk_common_GetDeviceProcAddr(VkDevice device, const char *name);

__attribute__((visibility("default"))) PFN_vkVoidFunction
vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    return vk_icdGetInstanceProcAddr(instance, name);
}

__attribute__((visibility("default"))) PFN_vkVoidFunction
vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    return vk_common_GetDeviceProcAddr(device, name);
}

/* The SDK's crtbeginS.o is empty: this library's own handle, which ACO's C++
 * static objects register their destructors with. */
__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;
