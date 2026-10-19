/* Parent-only RADV bridge, ported from winetitlerun/title.c. */
#ifdef __PROSPERO__
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wine/ps5_title.h>
#include <wine/ps5_title_input.h>
#include <wine/ps5_title_gamepad.h>
#include <wine/ps5_suspend.h>
#include <vulkan/vulkan.h>
#include "../localproc/psmkit-io.h"
#include "../../title/voice-overlay-vulkan.h"

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
extern PFN_vkVoidFunction vk_common_GetDeviceProcAddr(VkDevice, const char *);

int pw_title_read_input(union ps5_title_input *, unsigned, int, int, int, int);
int pw_title_set_cursor(const void *, unsigned);
unsigned pw_title_get_gamepad(unsigned, struct wine_ps5_gamepad_state *, int);
unsigned pw_title_set_gamepad_vibration(unsigned, unsigned, unsigned);
void pw_title_enable_gamepad(int);
void *pw_title_profile_proc(const char *);
unsigned pw_title_voice_overlay(void);
static VkResult title_create_instance(const VkInstanceCreateInfo *info,
                                     const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    /* VK_KHR_display is native-only; the PE uses VK_KHR_win32_surface. */
    const char *extensions[info->enabledExtensionCount + 1];
    VkInstanceCreateInfo native = *info;
    memcpy(extensions, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof(*extensions));
    extensions[native.enabledExtensionCount++] = VK_KHR_DISPLAY_EXTENSION_NAME;
    native.ppEnabledExtensionNames = extensions;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
    return create(&native, allocator, instance);
}

/* What a refused swapchain asked for: RADV's VideoOut checks return
 * VK_ERROR_INITIALIZATION_FAILED without saying which one failed. */
static VkResult title_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                       const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain)
{
    PFN_vkCreateSwapchainKHR create = (PFN_vkCreateSwapchainKHR)vk_common_GetDeviceProcAddr(device, "vkCreateSwapchainKHR");
    VkSwapchainCreateInfoKHR native = *info;
    native.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info = &native;
    VkResult result = create(device, info, allocator, swapchain);
    if (result == VK_SUCCESS) voice_overlay_vk_swapchain(device, info, *swapchain);
    if (result < 0)
    {
        char chain[96] = "";
        size_t used = 0;
        for (const VkBaseInStructure *next = info->pNext; next && used + 12 < sizeof(chain); next = next->pNext)
            used += snprintf(chain + used, sizeof(chain) - used, " %d", (int)next->sType);
        io_log("title vulkan swapchain failed %d: images %u format %d colorspace %d extent %ux%u layers %u"
               " usage %#x sharing %d transform %#x alpha %#x present %d clipped %u flags %#x old %d next:%s",
               result, info->minImageCount, info->imageFormat, info->imageColorSpace, info->imageExtent.width,
               info->imageExtent.height, info->imageArrayLayers, info->imageUsage, info->imageSharingMode,
               info->preTransform, info->compositeAlpha, info->presentMode, info->clipped, info->flags,
               info->oldSwapchain != VK_NULL_HANDLE, chain);
    }
    return result;
}

/* Frame pacing as presented: every 10 s, the frames presented, their rate,
 * the mean, 99th-percentile (the "1% low") and worst interval between presents. */
typedef void (*title_hang_diagnostic_fn)(unsigned, unsigned);

static PFN_vkQueuePresentKHR title_real_present;
static pthread_mutex_t title_present_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t title_present_window_ns, title_present_last_ns;
static uint32_t title_present_ms_x10[4096];
static unsigned title_present_count;
static _Atomic int64_t title_present_completed_ns;
static _Atomic uintptr_t title_hang_diagnostic;
static _Atomic int title_hang_watchdog_started, title_hang_reported;

static int64_t title_now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void title_sleep_until(int64_t target_ns)
{
    for (;;)
    {
        int64_t remaining = target_ns - title_now_ns();
        struct timespec delay;

        if (remaining <= 0) return;
        delay.tv_sec = remaining / 1000000000;
        delay.tv_nsec = remaining % 1000000000;
        if (!nanosleep(&delay, NULL) || errno != EINTR) return;
    }
}

static void title_sleep_one_second(void)
{
    title_sleep_until(title_now_ns() + 1000000000);
}


static void *title_hang_watchdog(void *unused)
{
    (void)unused;

    for (;;)
    {
        title_hang_diagnostic_fn diagnostic;
        int64_t last, now;

        title_sleep_one_second();
        diagnostic = (title_hang_diagnostic_fn)
            atomic_load_explicit(&title_hang_diagnostic, memory_order_acquire);
        last = atomic_load_explicit(&title_present_completed_ns, memory_order_relaxed);
        if (!diagnostic || !last) continue;
        now = title_now_ns();
        if (wine_ps5_present_is_hung(now, last))
        {
            if (!atomic_exchange_explicit(&title_hang_reported, 1, memory_order_relaxed))
            {
                static const int64_t late_ns[] = { INT64_C(10000000000), INT64_C(30000000000) };
                unsigned snapshot, captured = 0, active = 1;
                int64_t first_sample_ns = title_now_ns();

                io_log("title hang watchdog stalled_ms=%lld diagnostic=begin "
                       "snapshots=%u interval_ms=1000 late_samples=2 late_ms=10000,30000",
                       (long long)((now - last) / 1000000), WINE_PS5_HANG_SNAPSHOT_COUNT);
                for (snapshot = 1; snapshot <= WINE_PS5_HANG_SNAPSHOT_COUNT; snapshot++)
                {
                    diagnostic = (title_hang_diagnostic_fn)
                        atomic_load_explicit(&title_hang_diagnostic, memory_order_acquire);
                    if (!diagnostic) { active = 0; break; }
                    diagnostic(snapshot, WINE_PS5_HANG_SNAPSHOT_COUNT);
                    captured++;
                    if (snapshot != WINE_PS5_HANG_SNAPSHOT_COUNT)
                        title_sleep_until(first_sample_ns + snapshot * INT64_C(1000000000));
                }
                for (snapshot = 0; active && snapshot < sizeof(late_ns) / sizeof(late_ns[0]); snapshot++)
                {
                    title_sleep_until(first_sample_ns + late_ns[snapshot]);
                    diagnostic = (title_hang_diagnostic_fn)
                        atomic_load_explicit(&title_hang_diagnostic, memory_order_acquire);
                    if (!diagnostic) break;
                    diagnostic(WINE_PS5_HANG_SNAPSHOT_COUNT + snapshot + 1,
                               WINE_PS5_HANG_TOTAL_SNAPSHOT_COUNT);
                    captured++;
                }
                io_log("title hang watchdog diagnostic=end snapshots=%u", captured);
            }
        }
        else
            atomic_store_explicit(&title_hang_reported, 0, memory_order_relaxed);
    }
    return NULL;
}

static void title_set_hang_diagnostic(title_hang_diagnostic_fn diagnostic)
{
    atomic_store_explicit(&title_hang_diagnostic, (uintptr_t)diagnostic, memory_order_release);
    if (diagnostic && !atomic_exchange_explicit(&title_hang_watchdog_started, 1, memory_order_relaxed))
    {
        pthread_t thread;
        int error = pthread_create(&thread, NULL, title_hang_watchdog, NULL);

        if (error)
        {
            atomic_store_explicit(&title_hang_watchdog_started, 0, memory_order_relaxed);
            io_log("title hang watchdog start failed error=%d", error);
        }
    }
}

static int title_compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static VkResult title_queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    unsigned session = pw_title_voice_overlay();
    VkResult result = voice_overlay_vk_present(queue, info, title_real_present, session);
    int64_t now = title_now_ns();
    atomic_store_explicit(&title_present_completed_ns, now, memory_order_relaxed);
    atomic_store_explicit(&title_hang_reported, 0, memory_order_relaxed);
    pthread_mutex_lock(&title_present_lock);
    if (title_present_last_ns && title_present_count < sizeof(title_present_ms_x10) / sizeof(title_present_ms_x10[0]))
        title_present_ms_x10[title_present_count++] = (uint32_t)((now - title_present_last_ns) / 100000);
    title_present_last_ns = now;
    if (!title_present_window_ns) title_present_window_ns = now;
    if (now - title_present_window_ns >= 10000000000LL && title_present_count)
    {
        uint64_t sum = 0;
        unsigned n = title_present_count;
        for (unsigned i = 0; i < n; i++) sum += title_present_ms_x10[i];
        qsort(title_present_ms_x10, n, sizeof(title_present_ms_x10[0]), title_compare_u32);
        double seconds = (now - title_present_window_ns) / 1e9;
        io_log("title vulkan frames window_s=%.1f frames=%u fps=%.1f mean_ms=%.2f p99_ms=%.1f max_ms=%.1f",
               seconds, n, n / seconds, sum / 10.0 / n, title_present_ms_x10[(n * 99) / 100] / 10.0,
               title_present_ms_x10[n - 1] / 10.0);
        title_present_count = 0;
        title_present_window_ns = now;
    }
    pthread_mutex_unlock(&title_present_lock);
    return result;
}

static PFN_vkVoidFunction title_get_device_proc(VkDevice device, const char *name)
{
    PFN_vkVoidFunction overlay = voice_overlay_vk_device_proc(name);
    if (overlay && vk_common_GetDeviceProcAddr(device, name)) return overlay;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)title_create_swapchain;
    if (!strcmp(name, "vkQueuePresentKHR"))
    {
        PFN_vkQueuePresentKHR real = (PFN_vkQueuePresentKHR)vk_common_GetDeviceProcAddr(device, name);
        if (!real) return NULL;
        if (!title_real_present) title_real_present = real;
        return title_real_present == real ? (PFN_vkVoidFunction)title_queue_present : (PFN_vkVoidFunction)real;
    }
    return vk_common_GetDeviceProcAddr(device, name);
}

static PFN_vkVoidFunction title_get_instance_proc(VkInstance instance, const char *name)
{
    PFN_vkVoidFunction overlay = voice_overlay_vk_instance_proc(instance, name);
    if (overlay) return overlay;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)title_create_instance;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)title_get_device_proc;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)title_create_swapchain;
    return vk_icdGetInstanceProcAddr(instance, name);
}

/* The console has one output and no window system: a window's surface is the
 * output itself, a VK_KHR_display plane in the output's first mode (the mode
 * reported to Windows), whatever the window's size; the swapchain is sized by
 * the program. As WoWPS5's win32u does (runtime/wine-ps5/wine-ps5.patch). */
static VkResult title_create_display_surface(VkInstance instance, uint32_t width,
                                             uint32_t height, VkSurfaceKHR *surface)
{
    VkPhysicalDevice physical;
    VkDisplayPropertiesKHR displays[4];
    VkDisplayModePropertiesKHR modes[16];
    VkDisplayPlanePropertiesKHR planes[16];
    VkDisplayPlaneCapabilitiesKHR capabilities;
    uint32_t count = 1, display_count = 4, mode_count = 16, plane_count = 16, plane = UINT32_MAX;
    PFN_vkEnumeratePhysicalDevices enumerate = (void *)vk_icdGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR get_displays = (void *)vk_icdGetInstanceProcAddr(instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR get_planes = (void *)vk_icdGetInstanceProcAddr(instance, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");
    PFN_vkGetDisplayPlaneSupportedDisplaysKHR get_supported = (void *)vk_icdGetInstanceProcAddr(instance, "vkGetDisplayPlaneSupportedDisplaysKHR");
    PFN_vkGetDisplayPlaneCapabilitiesKHR get_capabilities = (void *)vk_icdGetInstanceProcAddr(instance, "vkGetDisplayPlaneCapabilitiesKHR");
    PFN_vkGetDisplayModePropertiesKHR get_modes = (void *)vk_icdGetInstanceProcAddr(instance, "vkGetDisplayModePropertiesKHR");
    PFN_vkCreateDisplayPlaneSurfaceKHR create = (void *)vk_icdGetInstanceProcAddr(instance, "vkCreateDisplayPlaneSurfaceKHR");
    VkResult result;
    if (!enumerate || !get_displays || !get_planes || !get_supported || !get_capabilities || !get_modes || !create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if ((result = enumerate(instance, &count, &physical)) < 0 || !count) goto failed;
    if ((result = get_displays(physical, &display_count, displays)) < 0 || !display_count) goto failed;
    if ((result = get_modes(physical, displays[0].display, &mode_count, modes)) < 0 || !mode_count) goto failed;
    if ((result = get_planes(physical, &plane_count, planes)) < 0 || !plane_count) goto failed;
    for (uint32_t i = 0; i < plane_count && plane == UINT32_MAX; i++)
    {
        VkDisplayKHR supported[4];
        uint32_t supported_count = 4;
        if (get_supported(physical, i, &supported_count, supported) < 0) continue;
        for (uint32_t j = 0; j < supported_count; j++)
            if (supported[j] == displays[0].display) plane = i;
    }
    if (plane == UINT32_MAX) { result = VK_ERROR_INITIALIZATION_FAILED; goto failed; }
    if ((result = get_capabilities(physical, modes[0].displayMode, plane, &capabilities)) < 0) goto failed;
    VkDisplaySurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = modes[0].displayMode, .planeIndex = plane,
        .planeStackIndex = planes[plane].currentStackIndex,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .globalAlpha = 1.0f,
        .alphaMode = (capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR) ?
                     VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR :
                     (capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR) ?
                     VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR : VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR,
        .imageExtent = modes[0].parameters.visibleRegion,
    };
    if ((result = create(instance, &info, NULL, surface)) < 0) goto failed;
    io_log("title vulkan display surface: window %ux%u, output %ux%u at %u mHz, plane %u",
           width, height, info.imageExtent.width, info.imageExtent.height,
           modes[0].parameters.refreshRate, plane);
    return VK_SUCCESS;
failed:
    io_log("title vulkan display surface: window %ux%u failed result=%d", width, height, result);
    return result < 0 ? result : VK_ERROR_INITIALIZATION_FAILED;
}

static unsigned title_display_modes(struct wine_ps5_display_mode *out, unsigned capacity)
{
    VkInstance instance;
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions};
    PFN_vkCreateInstance create = (void *)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
    VkResult status = create(&info, NULL, &instance);
    if (status)
    {
        io_log("title vulkan display modes: vkCreateInstance=%d", status);
        return 0;
    }
    PFN_vkEnumeratePhysicalDevices enumerate = (void *)vk_icdGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR get_displays = (void *)vk_icdGetInstanceProcAddr(instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    PFN_vkGetDisplayModePropertiesKHR get_modes = (void *)vk_icdGetInstanceProcAddr(instance, "vkGetDisplayModePropertiesKHR");
    PFN_vkDestroyInstance destroy = (void *)vk_icdGetInstanceProcAddr(instance, "vkDestroyInstance");
    VkPhysicalDevice physical;
    VkDisplayPropertiesKHR display;
    VkDisplayModePropertiesKHR modes[8];
    uint32_t count = 1, display_count = 1, mode_count = 8;
    unsigned result = 0;
    if ((status = enumerate(instance, &count, &physical)) || !count)
        io_log("title vulkan display modes: vkEnumeratePhysicalDevices=%d count=%u", status, count);
    else if ((status = get_displays(physical, &display_count, &display)) < 0 || !display_count)
        io_log("title vulkan display modes: vkGetPhysicalDeviceDisplayPropertiesKHR=%d count=%u",
               status, display_count);
    else if ((status = get_modes(physical, display.display, &mode_count, modes)) < 0)
        io_log("title vulkan display modes: vkGetDisplayModePropertiesKHR=%d", status);
    else
        for (; result < mode_count && result < capacity; result++)
            out[result] = (struct wine_ps5_display_mode){
                modes[result].parameters.visibleRegion.width, modes[result].parameters.visibleRegion.height,
                modes[result].parameters.refreshRate};
    destroy(instance, NULL);
    io_log("title vulkan display modes: %u", result);
    return result;
}

void *pw_title_vulkan_proc(const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (void *)title_get_instance_proc;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (void *)title_get_device_proc;
    if (!strcmp(name, "wineCreateDisplaySurface")) return (void *)title_create_display_surface;
    if (!strcmp(name, "wineGetDisplayModes")) return (void *)title_display_modes;
    if (!strcmp(name, "wineReadInput")) return (void *)pw_title_read_input;
    if (!strcmp(name, "wineSetCursor")) return (void *)pw_title_set_cursor;
    if (!strcmp(name, "wineGetGamepad")) return (void *)pw_title_get_gamepad;
    if (!strcmp(name, "wineSetGamepadVibration")) return (void *)pw_title_set_gamepad_vibration;
    if (!strcmp(name, "wineEnableGamepad")) return (void *)pw_title_enable_gamepad;
    if (!strcmp(name, "wineSetHangDiagnostic")) return (void *)title_set_hang_diagnostic;
    return pw_title_profile_proc(name);
}
#endif
