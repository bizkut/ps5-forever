/* The title's screen (display.h). VideoOut setup as the SDK's platform header
 * documents it (ps5platform/videoout.h); the tile layout is the one
 * ps5-native-app-boilerplate's demo renderer writes from the CPU. */
#include <string.h>
#include <ps5platform/kernel.h>
#include <ps5platform/videoout.h>
#include "display.h"

/* A 1920x1080 buffer in 128x128 tiles: 15 tiles across, 9 down, 64 KiB each,
 * rounded up to the 16 MiB the demo renderer allocates. */
#define FRAME_BYTES 0x1000000u
#define ALIGN 0x200000u
#define MEMORY_TYPE_WC_GARLIC 3
#define TILES_ACROSS ((DISPLAY_WIDTH + 127) >> 7)

static int video = -1;
static uint8_t *frames[2];
static int shown;   /* the buffer on screen */
static uint32_t image[DISPLAY_WIDTH * DISPLAY_HEIGHT];
/* What each buffer lacks of the image: the union of the damage since it was
 * last written. */
static struct display_rect stale[2];

static void add_rect(struct display_rect *to, const struct display_rect *rect)
{
    if (to->left >= to->right || to->top >= to->bottom)
    {
        *to = *rect;
        return;
    }
    if (rect->left < to->left) to->left = rect->left;
    if (rect->top < to->top) to->top = rect->top;
    if (rect->right > to->right) to->right = rect->right;
    if (rect->bottom > to->bottom) to->bottom = rect->bottom;
}

/* Byte offset of pixel (x, y) in a tiled buffer. */
static inline size_t tiled_offset(unsigned x, unsigned y)
{
    uint32_t offset = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^
                      ((y << 8) & 0x4000u) ^ ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^
                      ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
    return ((size_t)((y >> 7) * TILES_ACROSS + (x >> 7)) << 16) + offset;
}

static void flush_line(const void *address)
{
    __asm__ volatile("clflush (%0)" :: "r"(address) : "memory");
}

/* Copies a rectangle of the image into a tiled buffer. Four pixels with x a
 * multiple of four are 16 contiguous bytes ((x << 2) & 0xc); a 64-byte line
 * holds such groups from four rows ((y << 4) & 0x70), so the lines are
 * flushed once the whole rectangle is written: VideoOut scans out what
 * memory holds. */
static void copy_rect(uint8_t *frame, const struct display_rect *rect)
{
    int left = rect->left & ~3, right = (rect->right + 3) & ~3;
    if (right > DISPLAY_WIDTH) right = DISPLAY_WIDTH;
    for (int y = rect->top; y < rect->bottom; y++)
    {
        const uint32_t *row = image + (size_t)y * DISPLAY_WIDTH;
        for (int x = left; x < right; x += 4) memcpy(frame + tiled_offset(x, y), row + x, 16);
    }
    for (int y = rect->top; y < rect->bottom; y++)
        for (int x = left; x < right; x += 4)
            if (!(tiled_offset(x, y) & 0x30)) flush_line(frame + tiled_offset(x, y));
    /* A rectangle starting at a row that is not a multiple of four leaves the
     * lines of its first rows unflushed by the test above. */
    for (int y = rect->top; y < rect->bottom && (y & 3); y++)
        for (int x = left; x < right; x += 4) flush_line(frame + tiled_offset(x, y));
    __asm__ volatile("mfence" ::: "memory");
}

int display_open(const char **what)
{
    uint8_t attribute[PS5_VIDEO_OUT_ATTRIBUTE_BYTES] = {0};
    struct ps5_video_out_buffer buffers[2];
    int64_t physical = 0;
    void *memory = NULL;
    int r;

    *what = "sceVideoOutOpen";
    if ((video = sceVideoOutOpen(PS5_VIDEO_OUT_USER_SYSTEM, 0, 0, NULL)) < 0) return video;
    *what = "sceKernelAllocateDirectMemory";
    if ((r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), 2 * FRAME_BYTES, ALIGN,
                                           MEMORY_TYPE_WC_GARLIC, &physical))) return r;
    *what = "sceKernelMapDirectMemory";
    if ((r = sceKernelMapDirectMemory(&memory, 2 * FRAME_BYTES,
                                      PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | 0x30, 0,
                                      physical, ALIGN))) return r;
    for (int i = 0; i < 2; i++)
    {
        frames[i] = (uint8_t *)memory + i * FRAME_BYTES;
        buffers[i] = (struct ps5_video_out_buffer){frames[i], NULL, {NULL, NULL}};
        stale[i] = (struct display_rect){0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT};
    }
    memset(image, 0, sizeof(image));
    copy_rect(frames[0], &stale[0]);
    stale[0] = (struct display_rect){0};
    sceVideoOutSetFlipRate(video, 0);
    sceVideoOutSetBufferAttribute2(attribute, PS5_VIDEO_OUT_PIXEL_FORMAT_B8G8R8A8_SDR,
                                   PS5_VIDEO_OUT_TILING_64KB_R_X, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0, 0);
    *what = "sceVideoOutRegisterBuffers2";
    if ((r = sceVideoOutRegisterBuffers2(video, 0, 0, buffers, 2, attribute, 0, NULL))) return r;
    *what = "sceVideoOutSubmitFlip";
    if ((r = sceVideoOutSubmitFlip(video, 0, PS5_VIDEO_OUT_FLIP_VSYNC, 0)) < 0) return r;
    shown = 0;
    return 0;
}

uint32_t *display_image(void)
{
    return image;
}

void display_damage(const struct display_rect *rect)
{
    struct display_rect r = *rect;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > DISPLAY_WIDTH) r.right = DISPLAY_WIDTH;
    if (r.bottom > DISPLAY_HEIGHT) r.bottom = DISPLAY_HEIGHT;
    if (r.left >= r.right || r.top >= r.bottom) return;
    add_rect(&stale[0], &r);
    add_rect(&stale[1], &r);
}

void display_present(void)
{
    int next = !shown;

    if (stale[next].left < stale[next].right && stale[next].top < stale[next].bottom)
    {
        /* The buffer on screen may be scanned out until the flip shows. */
        while (sceVideoOutIsFlipPending(video) > 0) sceVideoOutWaitVblank(video);
        copy_rect(frames[next], &stale[next]);
        stale[next] = (struct display_rect){0};
        sceVideoOutSubmitFlip(video, next, PS5_VIDEO_OUT_FLIP_VSYNC, 0);
        shown = next;
    }
    sceVideoOutWaitVblank(video);
}
