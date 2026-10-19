#ifndef FOREVER_VOICE_OVERLAY_RASTER_H
#define FOREVER_VOICE_OVERLAY_RASTER_H

#include <stddef.h>

enum voice_overlay_state
{
    VOICE_OVERLAY_HIDDEN,
    VOICE_OVERLAY_LISTENING,
};

enum voice_overlay_format
{
    VOICE_OVERLAY_BGRA8_UNORM,
    VOICE_OVERLAY_BGRA8_SRGB,
    VOICE_OVERLAY_RGBA8_UNORM,
    VOICE_OVERLAY_RGBA8_SRGB,
    VOICE_OVERLAY_A2B10G10R10_SDR,
    VOICE_OVERLAY_A2B10G10R10_PQ,
    VOICE_OVERLAY_A2R10G10B10_SDR,
    VOICE_OVERLAY_A2R10G10B10_PQ,
};

/* Empty for hidden or invalid states; otherwise "Listening". */
const char *voice_overlay_label(enum voice_overlay_state state);

/* Horizontally and vertically centred. Scale is surface_h / 540 clamped to
 * 2..4 (1080p: 2; 2160p: 4). The 82x23 logical
 * panel is clipped to the surface; hidden/invalid/empty surfaces give zeros.
 * All five output pointers must be non-NULL. */
void voice_overlay_layout(unsigned surface_w, unsigned surface_h,
                          enum voice_overlay_state state, unsigned *x, unsigned *y,
                          unsigned *w, unsigned *h, unsigned *scale);

/* Tightly packed, four bytes per pixel. Zero for invalid formats, zero-sized
 * panels, or overflow. No allocation is performed by any of these functions. */
size_t voice_overlay_bytes(enum voice_overlay_format format, unsigned w, unsigned h);

/* Opaque panel with a one-physical-pixel border, red dot and white bitmap text.
 * Geometry is in panel-local pixels, scale in 1..4; smaller panels clip content.
 * SDR UNORM and SRGB both contain sRGB-encoded values: a transfer copy to an
 * SRGB image does not itself perform an sRGB encode. Packed SDR 10-bit uses the
 * same encoding. PQ stores BT.2020/ST 2084: 200-nit white, 2-nit background,
 * 6-nit border, and 150-nit linear sRGB red transformed into BT.2020 primaries.
 * Pixels are native Vulkan 32-bit packed values (8-bit byte order on PS5/x86).
 * Returns 0 on success, -1 without writing for hidden/invalid states, format,
 * scale, zero/overflowing geometry, NULL output, or an undersized buffer. */
int voice_overlay_render(enum voice_overlay_format format, enum voice_overlay_state state,
                         unsigned w, unsigned h, unsigned scale, void *out, size_t out_bytes);

#endif
