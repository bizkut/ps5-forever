/* Real native overlay code; Vulkan device/command boundaries are host fakes.
 * Verify cold allocation and meta-copy recording happen before visibility. */
#define __PROSPERO__ 1
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "../../title/voice-overlay-vulkan.c"

static unsigned allocations, maps, recordings, copies, barriers, submissions, waits, presentations;
static uintptr_t next_handle = 10;
static uint32_t pixels[328 * 92];
static int enabled, expect_overlay;
static VkSemaphore original_ready = (VkSemaphore)(uintptr_t)500;
#define HANDLE(type) ((type)(uintptr_t)++next_handle)

int pw_title_voice_enabled(void) { return enabled; }
void io_log(const char *format, ...) { (void)format; }
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{ (void)instance; (void)name; return NULL; }
PFN_vkVoidFunction vk_common_GetDeviceProcAddr(VkDevice device, const char *name)
{ (void)device; (void)name; return NULL; }

static VkResult make_pool(VkDevice d, const VkCommandPoolCreateInfo *i, const VkAllocationCallbacks *a, VkCommandPool *out)
{ (void)d; (void)a; assert(i->queueFamilyIndex <= 1); allocations++; *out = HANDLE(VkCommandPool); return VK_SUCCESS; }
static void free_pool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a)
{ (void)d; (void)p; (void)a; }
static VkResult make_commands(VkDevice d, const VkCommandBufferAllocateInfo *i, VkCommandBuffer *out)
{ (void)d; assert(i->commandBufferCount == 1); allocations++; *out = HANDLE(VkCommandBuffer); return VK_SUCCESS; }
static VkResult make_fence(VkDevice d, const VkFenceCreateInfo *i, const VkAllocationCallbacks *a, VkFence *out)
{ (void)d; (void)i; (void)a; allocations++; *out = HANDLE(VkFence); return VK_SUCCESS; }
static void free_fence(VkDevice d, VkFence f, const VkAllocationCallbacks *a)
{ (void)d; (void)f; (void)a; }
static VkResult make_semaphore(VkDevice d, const VkSemaphoreCreateInfo *i, const VkAllocationCallbacks *a, VkSemaphore *out)
{ (void)d; (void)i; (void)a; allocations++; *out = HANDLE(VkSemaphore); return VK_SUCCESS; }
static void free_semaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a)
{ (void)d; (void)s; (void)a; }
static VkResult make_buffer(VkDevice d, const VkBufferCreateInfo *i, const VkAllocationCallbacks *a, VkBuffer *out)
{ (void)d; (void)a; assert(i->usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT && i->size <= sizeof(pixels)); allocations++; *out = HANDLE(VkBuffer); return VK_SUCCESS; }
static void free_buffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a)
{ (void)d; (void)b; (void)a; }
static void requirements(VkDevice d, VkBuffer b, VkMemoryRequirements *out)
{ (void)d; (void)b; *out = (VkMemoryRequirements){.size = sizeof(pixels), .alignment = 4096, .memoryTypeBits = 1}; }
static VkResult make_memory(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *out)
{ (void)d; (void)a; assert(i->memoryTypeIndex == 0); allocations++; *out = HANDLE(VkDeviceMemory); return VK_SUCCESS; }
static void free_memory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a)
{ (void)d; (void)m; (void)a; }
static VkResult bind_buffer(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset)
{ (void)d; (void)b; (void)m; assert(!offset); return VK_SUCCESS; }
static VkResult map_buffer(VkDevice d, VkDeviceMemory m, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **out)
{ (void)d; (void)m; (void)flags; assert(!offset && size == VK_WHOLE_SIZE); maps++; *out = pixels; return VK_SUCCESS; }
static void unmap_buffer(VkDevice d, VkDeviceMemory m) { (void)d; (void)m; }
static VkResult swapchain_images(VkDevice d, VkSwapchainKHR s, uint32_t *count, VkImage *out)
{
    (void)d; (void)s;
    if (out) { assert(*count >= 3); for (unsigned i = 0; i < 3; i++) out[i] = HANDLE(VkImage); }
    *count = 3;
    return VK_SUCCESS;
}
static VkResult reset_pool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags flags)
{ (void)d; (void)p; assert(!flags); return VK_SUCCESS; }
static VkResult begin_commands(VkCommandBuffer c, const VkCommandBufferBeginInfo *i)
{ (void)c; assert(!i->flags); recordings++; return VK_SUCCESS; }
static VkResult end_commands(VkCommandBuffer c) { (void)c; return VK_SUCCESS; }
static void image_barrier(VkCommandBuffer c, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
    uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count, const VkBufferMemoryBarrier *buffer,
    uint32_t image_count, const VkImageMemoryBarrier *image)
{
    (void)c; (void)memory; (void)buffer; assert(!flags && !memory_count && !buffer_count && image_count == 1);
    if (image->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        assert(src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst == VK_PIPELINE_STAGE_TRANSFER_BIT && image->oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    else assert(src == VK_PIPELINE_STAGE_TRANSFER_BIT && image->newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    barriers++;
}
static void copy_pixels(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout layout, uint32_t count, const VkBufferImageCopy *copy)
{
    (void)c; (void)b; (void)i; assert(layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count == 1);
    assert(copy->imageExtent.width == 164 && copy->imageExtent.height == 46); copies++;
}
static VkResult wait_fences(VkDevice d, uint32_t count, const VkFence *f, VkBool32 all, uint64_t timeout)
{ (void)d; (void)f; assert(count == 1 && all && timeout == UINT64_MAX); waits++; return VK_SUCCESS; }
static VkResult reset_fences(VkDevice d, uint32_t count, const VkFence *f)
{ (void)d; (void)f; assert(count == 1); return VK_SUCCESS; }
static VkResult submit(VkQueue q, uint32_t count, const VkSubmitInfo *i, VkFence f)
{
    (void)q; (void)f; assert(count == 1 && i->waitSemaphoreCount == 1 && i->pWaitSemaphores[0] == original_ready);
    assert(i->pWaitDstStageMask[0] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    assert(i->commandBufferCount == 1 && i->signalSemaphoreCount == 1); submissions++; return VK_SUCCESS;
}
static VkResult present(VkQueue q, const VkPresentInfoKHR *info)
{
    (void)q; assert(info->waitSemaphoreCount == 1);
    assert((info->pWaitSemaphores[0] != original_ready) == expect_overlay);
    presentations++; return VK_SUCCESS;
}

int main(void)
{
    uint32_t families[] = {0, 1}, image_index = 0;
    VkImage images[] = {(VkImage)(uintptr_t)1, (VkImage)(uintptr_t)2, (VkImage)(uintptr_t)3};
    struct overlay_frame frames[3] = {0};
    struct overlay_swapchain swapchain = {.handle = (VkSwapchainKHR)(uintptr_t)2,
        .format = VK_FORMAT_A2B10G10R10_UNORM_PACK32, .pixels = VOICE_OVERLAY_A2B10G10R10_PQ,
        .x = 878, .y = 517, .width = 164, .height = 46, .scale = 2, .layers = 1,
        .count = 3, .supported = 1, .images = images, .frames = frames};
    struct overlay_queue queue = {.handle = (VkQueue)(uintptr_t)3, .family = 0};
    struct overlay_device device = {.handle = (VkDevice)(uintptr_t)4, .swapchains = &swapchain, .queues = &queue,
        .family_count = 2, .families = families, .ready = 1,
        .CreateCommandPool = make_pool, .DestroyCommandPool = free_pool, .AllocateCommandBuffers = make_commands,
        .CreateFence = make_fence, .DestroyFence = free_fence, .CreateSemaphore = make_semaphore, .DestroySemaphore = free_semaphore,
        .CreateBuffer = make_buffer, .DestroyBuffer = free_buffer, .GetBufferMemoryRequirements = requirements,
        .AllocateMemory = make_memory, .FreeMemory = free_memory, .BindBufferMemory = bind_buffer,
        .MapMemory = map_buffer, .UnmapMemory = unmap_buffer,
        .ResetCommandPool = reset_pool, .BeginCommandBuffer = begin_commands, .EndCommandBuffer = end_commands,
        .CmdPipelineBarrier = image_barrier, .CmdCopyBufferToImage = copy_pixels,
        .WaitForFences = wait_fences, .ResetFences = reset_fences, .QueueSubmit = submit,
        .GetSwapchainImagesKHR = swapchain_images};
    device.memory.memoryTypeCount = 1;
    device.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    devices = &device;
    VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &original_ready, .swapchainCount = 1, .pSwapchains = &swapchain.handle, .pImageIndices = &image_index};

    /* Disabled/hidden path does no Vulkan allocation, recording, or submit. */
    atomic_store(&warmup_requested, WARMUP_SWAPCHAIN); /* Another chain's startup request. */
    voice_overlay_vk_prepare_present(queue.handle, &info);
    assert(!allocations && !recordings && !submissions && !swapchain.prepared);
    assert(present(queue.handle, &info) == VK_SUCCESS);
    /* Even a cold visible call never falls back to allocating inside a hold. */
    assert(voice_overlay_vk_present(queue.handle, &info, present, 1) == VK_SUCCESS);
    assert(!allocations && !recordings && !submissions);

    /* Regression: CAPTURE then START, with no hidden present in between.
     * The real dispatcher warms on an unchanged frame and draws next frame. */
    enabled = 1;
    voice_overlay_vk_request_warmup();
    enabled = 0; /* A press's latched enable survives a later setting change. */
    assert(voice_overlay_vk_present(queue.handle, &info, present, 2) == VK_SUCCESS);
    assert(swapchain.prepared && maps == 1 && recordings == 3 && copies == 3 && barriers == 6 && !submissions);
    enabled = 1;
    unsigned warm_allocations = allocations;
    assert(warm_allocations == 14); /* Four per image + one shared buffer/memory. */
    for (unsigned i = 0; i < 3; i++) assert(frames[i].recorded && frames[i].fence && frames[i].ready);

    expect_overlay = 1;
    assert(voice_overlay_vk_present(queue.handle, &info, present, 2) == VK_SUCCESS);
    image_index = 1;
    assert(voice_overlay_vk_present(queue.handle, &info, present, 2) == VK_SUCCESS);
    assert(allocations == warm_allocations && maps == 1 && recordings == 3 && copies == 3 && submissions == 2);
    assert(!waits); /* Neither acquired image had an earlier cue copy. */
    assert(voice_overlay_vk_present(queue.handle, &info, present, 2) == VK_SUCCESS);
    assert(waits == 1 && submissions == 3 && allocations == warm_allocations && recordings == 3);
    expect_overlay = 0;
    voice_overlay_vk_request_warmup(); /* Already-warm holds do not repeat setup. */
    assert(voice_overlay_vk_present(queue.handle, &info, present, 0) == VK_SUCCESS);
    assert(submissions == 3 && allocations == warm_allocations);

    /* Once the actual present family is known, recreation warms at creation.
     * The disabled case still performs no Vulkan resource allocation. */
    VkSwapchainCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .imageFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32, .imageColorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT,
        .imageExtent = {1920, 1080}, .imageArrayLayers = 1};
    enabled = 0;
    voice_overlay_vk_swapchain(device.handle, &create, (VkSwapchainKHR)(uintptr_t)90);
    struct overlay_swapchain *replacement = device.swapchains;
    assert(!replacement->prepared && !replacement->buffer && allocations == warm_allocations);
    device.swapchains = replacement->next;
    destroy_swapchain(&device, replacement);
    enabled = 1;
    voice_overlay_vk_swapchain(device.handle, &create, (VkSwapchainKHR)(uintptr_t)91);
    replacement = device.swapchains;
    assert(replacement->prepared && replacement->frames[2].recorded);
    assert(allocations == 2 * warm_allocations && maps == 2 && recordings == 6 && copies == 6 && submissions == 3);
    device.swapchains = replacement->next;
    destroy_swapchain(&device, replacement);

    for (unsigned i = 0; i < 3; i++) destroy_frame(&device, &frames[i]);
    destroy_staging(&device, &swapchain);
    devices = NULL;
    destroy_device(VK_NULL_HANDLE, NULL); /* Legal no-op even without native dispatch. */
    puts("title voice overlay prewarm: ok");
    return 0;
}
