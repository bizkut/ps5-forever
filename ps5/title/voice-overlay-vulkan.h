/* Native game-parent Vulkan cue. No Wine/vkd3d resources or shaders are used. */
#ifndef FOREVER_VOICE_OVERLAY_VULKAN_H
#define FOREVER_VOICE_OVERLAY_VULKAN_H
#include <vulkan/vulkan.h>
PFN_vkVoidFunction voice_overlay_vk_instance_proc(VkInstance, const char *);
PFN_vkVoidFunction voice_overlay_vk_device_proc(const char *);
void voice_overlay_vk_swapchain(VkDevice, const VkSwapchainCreateInfoKHR *, VkSwapchainKHR);
/* A voice-enabled press latches preparation during its 400-ms deferral. */
void voice_overlay_vk_request_warmup(void);
int voice_overlay_vk_prepare_present(VkQueue, const VkPresentInfoKHR *);
VkResult voice_overlay_vk_present(VkQueue, const VkPresentInfoKHR *, PFN_vkQueuePresentKHR, unsigned session);
#endif
