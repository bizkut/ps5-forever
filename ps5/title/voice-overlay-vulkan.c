/* One opaque Listening cue on the title's output. Staging pixels are immutable
 * after the first raster; each acquired image owns its completion semaphore. */
#ifdef __PROSPERO__
#include "voice-overlay-vulkan.h"
#include "voice-overlay-raster.h"
#include "../probes/localproc/psmkit-io.h"
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
extern PFN_vkVoidFunction vk_common_GetDeviceProcAddr(VkDevice, const char *);
extern int pw_title_voice_enabled(void);

#define OVERLAY_DEVICE_FUNCTIONS(X) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
    X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
    X(FlushMappedMemoryRanges) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CreateFence) X(DestroyFence) \
    X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) \
    X(QueueSubmit) X(DeviceWaitIdle) X(GetSwapchainImagesKHR)

struct overlay_frame
{
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    VkSemaphore ready;
    uint32_t family;
    int submitted, recorded;
};
struct overlay_swapchain
{
    struct overlay_swapchain *next;
    VkSwapchainKHR handle;
    VkFormat format;
    enum voice_overlay_format pixels;
    unsigned x, y, width, height, scale;
    uint32_t count, layers;
    int supported, submitted, wants_warmup, prepared;
    VkImage *images;
    struct overlay_frame *frames;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
};
struct overlay_queue
{
    struct overlay_queue *next;
    VkQueue handle;
    uint32_t family;
};
struct overlay_device
{
    struct overlay_device *next;
    VkDevice handle;
    VkPhysicalDeviceMemoryProperties memory;
    struct overlay_swapchain *swapchains;
    struct overlay_queue *queues;
    uint32_t family_count, *families;
    uint32_t present_family;
    int ready, have_present_family;
#define OVERLAY_FIELD(name) PFN_vk##name name;
    OVERLAY_DEVICE_FUNCTIONS(OVERLAY_FIELD)
#undef OVERLAY_FIELD
};
static pthread_mutex_t overlay_lock = PTHREAD_MUTEX_INITIALIZER;
static struct overlay_device *devices;
/* This bridge has one statically linked RADV ICD. These entry addresses are
 * driver code, not instance-owned dispatch storage (as in runtime-vulkan.c). */
static PFN_vkCreateDevice real_create_device;
static PFN_vkGetPhysicalDeviceMemoryProperties physical_memory;
static unsigned logged_session;
enum { WARMUP_SWAPCHAIN = 1u, WARMUP_CAPTURE = 2u };
static atomic_uint warmup_requested;
static void prepare_swapchain(struct overlay_device *, struct overlay_swapchain *, uint32_t family);

void voice_overlay_vk_request_warmup(void)
{
    atomic_fetch_or_explicit(&warmup_requested, WARMUP_CAPTURE, memory_order_relaxed);
}

static struct overlay_device *find_device(VkDevice handle)
{
    for (struct overlay_device *device = devices; device; device = device->next)
        if (device->handle == handle) return device;
    return NULL;
}

static int pixel_format(VkFormat format, VkColorSpaceKHR space, enum voice_overlay_format *out)
{
    int pq = space == VK_COLOR_SPACE_HDR10_ST2084_EXT;
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: *out = VOICE_OVERLAY_BGRA8_UNORM; return 1;
    case VK_FORMAT_B8G8R8A8_SRGB: *out = VOICE_OVERLAY_BGRA8_SRGB; return 1;
    case VK_FORMAT_R8G8B8A8_UNORM: *out = VOICE_OVERLAY_RGBA8_UNORM; return 1;
    case VK_FORMAT_R8G8B8A8_SRGB: *out = VOICE_OVERLAY_RGBA8_SRGB; return 1;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        *out = pq ? VOICE_OVERLAY_A2B10G10R10_PQ : VOICE_OVERLAY_A2B10G10R10_SDR; return 1;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        *out = pq ? VOICE_OVERLAY_A2R10G10B10_PQ : VOICE_OVERLAY_A2R10G10B10_SDR; return 1;
    default: return 0;
    }
}

static void destroy_frame(struct overlay_device *device, struct overlay_frame *frame)
{
    if (frame->ready) device->DestroySemaphore(device->handle, frame->ready, NULL);
    if (frame->fence) device->DestroyFence(device->handle, frame->fence, NULL);
    if (frame->pool) device->DestroyCommandPool(device->handle, frame->pool, NULL);
    memset(frame, 0, sizeof(*frame));
}
static void destroy_staging(struct overlay_device *device, struct overlay_swapchain *swapchain)
{
    if (swapchain->mapped) device->UnmapMemory(device->handle, swapchain->memory);
    if (swapchain->buffer) device->DestroyBuffer(device->handle, swapchain->buffer, NULL);
    if (swapchain->memory) device->FreeMemory(device->handle, swapchain->memory, NULL);
    swapchain->mapped = NULL;
    swapchain->buffer = VK_NULL_HANDLE;
    swapchain->memory = VK_NULL_HANDLE;
}
static void destroy_swapchain(struct overlay_device *device, struct overlay_swapchain *swapchain)
{
    for (uint32_t i = 0; i < swapchain->count; i++) destroy_frame(device, &swapchain->frames[i]);
    destroy_staging(device, swapchain);
    free(swapchain);
}

static VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                              const VkAllocationCallbacks *allocator, VkDevice *out)
{
    PFN_vkCreateDevice create;
    PFN_vkGetPhysicalDeviceMemoryProperties memory;
    pthread_mutex_lock(&overlay_lock);
    create = real_create_device;
    memory = physical_memory;
    pthread_mutex_unlock(&overlay_lock);
    VkResult result = create(physical, info, allocator, out);
    if (result != VK_SUCCESS) return result;
    struct overlay_device *device = calloc(1, sizeof(*device) + info->queueCreateInfoCount * sizeof(uint32_t));
    if (!device) { io_log("voice overlay setup failed stage=device-memory"); return result; }
    device->handle = *out;
    device->ready = 1;
    device->families = (uint32_t *)(device + 1);
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++)
    {
        if (info->pQueueCreateInfos[i].flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT) continue;
        uint32_t family = info->pQueueCreateInfos[i].queueFamilyIndex, at = 0;
        while (at < device->family_count && device->families[at] != family) at++;
        if (at == device->family_count) device->families[device->family_count++] = family;
    }
    memory(physical, &device->memory);
#define OVERLAY_LOAD(name) \
    device->name = (PFN_vk##name)vk_common_GetDeviceProcAddr(*out, "vk" #name); \
    if (!device->name) { device->ready = 0; io_log("voice overlay missing proc=vk" #name); }
    OVERLAY_DEVICE_FUNCTIONS(OVERLAY_LOAD)
#undef OVERLAY_LOAD
    pthread_mutex_lock(&overlay_lock);
    device->next = devices;
    devices = device;
    pthread_mutex_unlock(&overlay_lock);
    return result;
}

static void destroy_device(VkDevice handle, const VkAllocationCallbacks *allocator)
{
    if (!handle) return;
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)vk_common_GetDeviceProcAddr(handle, "vkDestroyDevice");
    pthread_mutex_lock(&overlay_lock);
    struct overlay_device **at = &devices, *device;
    while (*at && (*at)->handle != handle) at = &(*at)->next;
    if ((device = *at))
    {
        *at = device->next;
        for (struct overlay_swapchain *swapchain = device->swapchains; swapchain; swapchain = swapchain->next)
            if (swapchain->submitted) { device->DeviceWaitIdle(handle); break; }
        while (device->swapchains)
        {
            struct overlay_swapchain *swapchain = device->swapchains;
            device->swapchains = swapchain->next;
            destroy_swapchain(device, swapchain);
        }
        while (device->queues)
        {
            struct overlay_queue *queue = device->queues;
            device->queues = queue->next;
            free(queue);
        }
        free(device);
    }
    pthread_mutex_unlock(&overlay_lock);
    destroy(handle, allocator);
}

static void track_queue(VkDevice handle, uint32_t family, VkQueue queue)
{
    pthread_mutex_lock(&overlay_lock);
    struct overlay_device *device = find_device(handle);
    if (device && queue)
    {
        struct overlay_queue *entry;
        for (entry = device->queues; entry && entry->handle != queue; entry = entry->next) {}
        if (!entry && (entry = malloc(sizeof(*entry))))
        {
            *entry = (struct overlay_queue){device->queues, queue, family};
            device->queues = entry;
        }
    }
    pthread_mutex_unlock(&overlay_lock);
}
static void get_device_queue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out)
{
    PFN_vkGetDeviceQueue get = (PFN_vkGetDeviceQueue)vk_common_GetDeviceProcAddr(device, "vkGetDeviceQueue");
    get(device, family, index, out);
    track_queue(device, family, *out);
}
static void get_device_queue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *out)
{
    PFN_vkGetDeviceQueue2 get = (PFN_vkGetDeviceQueue2)vk_common_GetDeviceProcAddr(device, "vkGetDeviceQueue2");
    get(device, info, out);
    /* CPU-visible staging cannot be used for protected content. */
    if (!(info->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT)) track_queue(device, info->queueFamilyIndex, *out);
}

void voice_overlay_vk_swapchain(VkDevice handle, const VkSwapchainCreateInfoKHR *info, VkSwapchainKHR swapchain)
{
    pthread_mutex_lock(&overlay_lock);
    struct overlay_device *device = find_device(handle);
    if (!device) { pthread_mutex_unlock(&overlay_lock); return; }
    enum voice_overlay_format format;
    int supported = device->ready && pixel_format(info->imageFormat, info->imageColorSpace, &format);
    uint32_t count = 0;
    VkResult result = VK_SUCCESS;
    if (supported) result = device->GetSwapchainImagesKHR(handle, swapchain, &count, NULL);
    if (result != VK_SUCCESS || !count) supported = 0;
    if (!supported) count = 0;
    struct overlay_swapchain *entry = calloc(1, sizeof(*entry) + count * (sizeof(VkImage) + sizeof(struct overlay_frame)));
    if (!entry) { pthread_mutex_unlock(&overlay_lock); return; }
    entry->handle = swapchain;
    entry->format = info->imageFormat;
    entry->layers = info->imageArrayLayers;
    entry->supported = supported;
    if (supported)
    {
        entry->pixels = format;
        entry->images = (VkImage *)(entry + 1);
        entry->frames = (struct overlay_frame *)(entry->images + count);
        entry->count = count;
        result = device->GetSwapchainImagesKHR(handle, swapchain, &count, entry->images);
        if (result != VK_SUCCESS || count != entry->count) entry->supported = 0;
        voice_overlay_layout(info->imageExtent.width, info->imageExtent.height, VOICE_OVERLAY_LISTENING,
                             &entry->x, &entry->y, &entry->width, &entry->height, &entry->scale);
        if (!entry->width || !entry->height) entry->supported = 0;
    }
    entry->wants_warmup = entry->supported && pw_title_voice_enabled();
    if (entry->wants_warmup)
    {
        if (device->have_present_family) prepare_swapchain(device, entry, device->present_family);
        else atomic_fetch_or_explicit(&warmup_requested, WARMUP_SWAPCHAIN, memory_order_relaxed);
    }
    entry->next = device->swapchains;
    device->swapchains = entry;
    pthread_mutex_unlock(&overlay_lock);
}
static void destroy_swapchain_khr(VkDevice handle, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator)
{
    PFN_vkDestroySwapchainKHR destroy = (PFN_vkDestroySwapchainKHR)vk_common_GetDeviceProcAddr(handle, "vkDestroySwapchainKHR");
    pthread_mutex_lock(&overlay_lock);
    struct overlay_device *device = find_device(handle);
    if (device)
    {
        struct overlay_swapchain **at = &device->swapchains, *entry;
        while (*at && (*at)->handle != swapchain) at = &(*at)->next;
        if ((entry = *at))
        {
            *at = entry->next;
            /* Recreation is off the frame hot path; retire all copies and
             * presentation semaphore waits before freeing their storage. */
            if (entry->submitted) device->DeviceWaitIdle(handle);
            destroy_swapchain(device, entry);
        }
    }
    pthread_mutex_unlock(&overlay_lock);
    destroy(handle, swapchain, allocator);
}

static VkResult prepare_frame(struct overlay_device *device, struct overlay_swapchain *swapchain,
                               struct overlay_frame *frame, uint32_t family)
{
    VkResult result;
    int creating_staging = 0;
    if (frame->submitted)
    {
        result = device->WaitForFences(device->handle, 1, &frame->fence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) return result;
        frame->submitted = 0;
    }
    if (frame->pool && frame->family != family)
    {
        device->DestroyCommandPool(device->handle, frame->pool, NULL);
        frame->pool = VK_NULL_HANDLE;
        frame->command = VK_NULL_HANDLE;
        frame->recorded = 0;
    }
    if (!frame->pool)
    {
        VkCommandPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = family};
        if ((result = device->CreateCommandPool(device->handle, &info, NULL, &frame->pool))) goto failed;
        frame->family = family;
        VkCommandBufferAllocateInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = frame->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if ((result = device->AllocateCommandBuffers(device->handle, &command, &frame->command))) goto failed;
    }
    if (!frame->fence)
    {
        VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if ((result = device->CreateFence(device->handle, &info, NULL, &frame->fence))) goto failed;
    }
    if (!frame->ready)
    {
        VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if ((result = device->CreateSemaphore(device->handle, &info, NULL, &frame->ready))) goto failed;
    }
    if (!swapchain->buffer)
    {
        creating_staging = 1;
        size_t bytes = voice_overlay_bytes(swapchain->pixels, swapchain->width, swapchain->height);
        VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = device->family_count > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = device->family_count, .pQueueFamilyIndices = device->families};
        if ((result = device->CreateBuffer(device->handle, &info, NULL, &swapchain->buffer))) goto failed;
        VkMemoryRequirements requirements;
        device->GetBufferMemoryRequirements(device->handle, swapchain->buffer, &requirements);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < device->memory.memoryTypeCount; i++)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (device->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            {
                type = i;
                if (device->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
            }
        if (type == UINT32_MAX) { result = VK_ERROR_FEATURE_NOT_PRESENT; goto failed; }
        int coherent = !!(device->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = type};
        if ((result = device->AllocateMemory(device->handle, &allocation, NULL, &swapchain->memory)) ||
            (result = device->BindBufferMemory(device->handle, swapchain->buffer, swapchain->memory, 0)) ||
            (result = device->MapMemory(device->handle, swapchain->memory, 0, VK_WHOLE_SIZE, 0, &swapchain->mapped))) goto failed;
        /* One immutable raster is shared by all acquired images. */
        if (voice_overlay_render(swapchain->pixels, VOICE_OVERLAY_LISTENING, swapchain->width,
                                 swapchain->height, swapchain->scale, swapchain->mapped, bytes))
        { result = VK_ERROR_INITIALIZATION_FAILED; goto failed; }
        if (!coherent)
        {
            VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = swapchain->memory, .offset = 0, .size = VK_WHOLE_SIZE};
            if ((result = device->FlushMappedMemoryRanges(device->handle, 1, &range))) goto failed;
        }
    }
    return VK_SUCCESS;
failed:
    if (creating_staging) destroy_staging(device, swapchain);
    destroy_frame(device, frame);
    return result;
}

/* The registry lock is held for both preparation and presentation. */
static struct overlay_swapchain *find_present_swapchain(VkQueue handle, const VkPresentInfoKHR *info,
                                                         struct overlay_device **out_device, uint32_t *family)
{
    *out_device = NULL;
    for (struct overlay_device *device = devices; device; device = device->next)
        for (struct overlay_queue *queue = device->queues; queue; queue = queue->next)
            if (queue->handle == handle)
            {
                *out_device = device;
                *family = queue->family;
                device->present_family = queue->family;
                device->have_present_family = 1;
                if (info->swapchainCount)
                    for (struct overlay_swapchain *swapchain = device->swapchains; swapchain; swapchain = swapchain->next)
                        if (swapchain->handle == info->pSwapchains[0]) return swapchain;
                return NULL;
            }
    return NULL;
}

static VkResult record_frame(struct overlay_device *device, struct overlay_swapchain *swapchain,
                              struct overlay_frame *frame, uint32_t image)
{
    if (frame->recorded) return VK_SUCCESS;
    VkResult result = device->ResetCommandPool(device->handle, frame->pool, 0);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!result) result = device->BeginCommandBuffer(frame->command, &begin);
    if (result) return result;
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchain->images[image],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, swapchain->layers}};
    device->CmdPipelineBarrier(frame->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    for (uint32_t layer = 0; layer < swapchain->layers; layer++)
    {
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
            .imageOffset = {(int32_t)swapchain->x, (int32_t)swapchain->y, 0},
            .imageExtent = {swapchain->width, swapchain->height, 1}};
        device->CmdCopyBufferToImage(frame->command, swapchain->buffer, barrier.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    device->CmdPipelineBarrier(frame->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    result = device->EndCommandBuffer(frame->command);
    if (!result) frame->recorded = 1;
    return result;
}

static void prepare_swapchain(struct overlay_device *device, struct overlay_swapchain *swapchain, uint32_t family)
{
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < swapchain->count; i++)
    {
        result = prepare_frame(device, swapchain, &swapchain->frames[i], family);
        if (!result) result = record_frame(device, swapchain, &swapchain->frames[i], i);
        if (result) break;
    }
    swapchain->prepared = result == VK_SUCCESS;
    if (result) swapchain->supported = 0;
    io_log("voice overlay warmup format=%d images=%u status=%d",
           (int)swapchain->format, swapchain->count, result);
}

int voice_overlay_vk_prepare_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    /* First enabled presentation (or a live enable's 400-ms deferral) pays
     * the allocation and RADV meta-copy compilation, never first visibility.
     * This does not submit commands, consume semaphores, or change PresentInfo. */
    if (!atomic_load_explicit(&warmup_requested, memory_order_relaxed)) return 0;
    unsigned requested = atomic_exchange_explicit(&warmup_requested, 0, memory_order_relaxed);
    if (!requested) return 0;
    int prepared = 0;
    pthread_mutex_lock(&overlay_lock);
    struct overlay_device *device;
    uint32_t family = 0;
    struct overlay_swapchain *swapchain = find_present_swapchain(queue, info, &device, &family);
    if (swapchain && swapchain->supported &&
        (!swapchain->prepared || swapchain->frames[0].family != family) &&
        (swapchain->wants_warmup || (requested & WARMUP_CAPTURE)))
    {
        prepare_swapchain(device, swapchain, family);
        prepared = 1;
    }
    /* A second/replacement swapchain may not have presented yet. Keep its
     * startup request until its own first hidden present, without losing a
     * concurrent live-enable request from the input thread. */
    for (struct overlay_device *at = devices; at; at = at->next)
        for (struct overlay_swapchain *pending = at->swapchains; pending; pending = pending->next)
            if (pending->supported && pending->wants_warmup && !pending->prepared)
                atomic_fetch_or_explicit(&warmup_requested, WARMUP_SWAPCHAIN, memory_order_relaxed);
    pthread_mutex_unlock(&overlay_lock);
    return prepared;
}

static VkResult present_recording(VkQueue queue, const VkPresentInfoKHR *info,
                                   PFN_vkQueuePresentKHR present, unsigned session)
{
    VkResult result = VK_SUCCESS;
    pthread_mutex_lock(&overlay_lock);
    struct overlay_device *device;
    uint32_t family = 0;
    struct overlay_swapchain *swapchain = find_present_swapchain(queue, info, &device, &family);
    int shown = 0, submitted = 0;
    struct overlay_frame *frame = NULL;
    if (swapchain && swapchain->supported && swapchain->prepared && info->pImageIndices[0] < swapchain->count)
    {
        frame = &swapchain->frames[info->pImageIndices[0]];
        /* The visible path never creates/rasterizes/records resources. */
        if (frame->family == family && frame->recorded)
        {
            if (frame->submitted)
            {
                result = device->WaitForFences(device->handle, 1, &frame->fence, VK_TRUE, UINT64_MAX);
                if (!result) frame->submitted = 0;
            }
            if (!result)
            {
                VkPipelineStageFlags stages[info->waitSemaphoreCount ? info->waitSemaphoreCount : 1];
                for (uint32_t i = 0; i < info->waitSemaphoreCount; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .waitSemaphoreCount = info->waitSemaphoreCount, .pWaitSemaphores = info->pWaitSemaphores,
                    .pWaitDstStageMask = stages, .commandBufferCount = 1, .pCommandBuffers = &frame->command,
                    .signalSemaphoreCount = 1, .pSignalSemaphores = &frame->ready};
                result = device->ResetFences(device->handle, 1, &frame->fence);
                if (!result)
                {
                    submitted = 1;
                    /* vkd3d swapchain.c:4170-4183 submits its output blit
                     * before the FG deadline wait, then presents afterward.
                     * This cue can queue behind intervening game rendering.
                     * That visible-only delay is accepted for brief recording
                     * holds; hidden frames never enter this submission path. */
                    result = device->QueueSubmit(queue, 1, &submit, frame->fence);
                }
                if (!result) swapchain->submitted = frame->submitted = shown = 1;
            }
        }
    }
    if (shown)
    {
        /* The console has one output: copy the cue into the first presented
         * swapchain, retaining every entry/result/extension in a batched present. */
        VkPresentInfoKHR next = *info;
        next.waitSemaphoreCount = 1;
        next.pWaitSemaphores = &frame->ready;
        result = present(queue, &next);
    }
    else if (!submitted && result != VK_ERROR_DEVICE_LOST) result = present(queue, info);
    if (logged_session != session)
    {
        io_log("voice overlay format=%d shown=%d", swapchain ? (int)swapchain->format : VK_FORMAT_UNDEFINED,
               shown && result >= VK_SUCCESS);
        logged_session = session;
    }
    pthread_mutex_unlock(&overlay_lock);
    return result;
}

VkResult voice_overlay_vk_present(VkQueue queue, const VkPresentInfoKHR *info,
                                  PFN_vkQueuePresentKHR present, unsigned session)
{
    /* START may precede the first present (startup or a stalled render). Pay
     * any pending preparation on a not-yet-visible, unchanged present, then
     * allow the active session to draw its already-cached cue next frame. */
    if (voice_overlay_vk_prepare_present(queue, info)) return present(queue, info);
    return session ? present_recording(queue, info, present, session) : present(queue, info);
}

PFN_vkVoidFunction voice_overlay_vk_instance_proc(VkInstance instance, const char *name)
{
    if (strcmp(name, "vkCreateDevice"))
    {
        PFN_vkVoidFunction wrapper = voice_overlay_vk_device_proc(name);
        return wrapper && vk_icdGetInstanceProcAddr(instance, name) ? wrapper : NULL;
    }
    if (!instance) return NULL;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)vk_icdGetInstanceProcAddr(instance, name);
    PFN_vkGetPhysicalDeviceMemoryProperties memory = (PFN_vkGetPhysicalDeviceMemoryProperties)
        vk_icdGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!create || !memory) return NULL;
    pthread_mutex_lock(&overlay_lock);
    real_create_device = create;
    physical_memory = memory;
    pthread_mutex_unlock(&overlay_lock);
    return (PFN_vkVoidFunction)create_device;
}
PFN_vkVoidFunction voice_overlay_vk_device_proc(const char *name)
{
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)destroy_device;
    if (!strcmp(name, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)get_device_queue;
    if (!strcmp(name, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction)get_device_queue2;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)destroy_swapchain_khr;
    return NULL;
}
#endif
