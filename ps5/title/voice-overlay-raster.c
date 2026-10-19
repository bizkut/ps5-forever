#include <stdint.h>
#include <string.h>
#include "voice-overlay-raster.h"
#include "bitmap-font.h"

#define PANEL_WIDTH 82u
#define PANEL_HEIGHT 23u
#define TEXT_LEFT 20u
#define TEXT_TOP 8u

const char *voice_overlay_label(enum voice_overlay_state state)
{
    return state == VOICE_OVERLAY_LISTENING ? "Listening" : "";
}

void voice_overlay_layout(unsigned surface_w, unsigned surface_h,
                          enum voice_overlay_state state, unsigned *x, unsigned *y,
                          unsigned *w, unsigned *h, unsigned *scale)
{
    *x = *y = *w = *h = *scale = 0;
    if (state != VOICE_OVERLAY_LISTENING || !surface_w || !surface_h) return;
    unsigned s = surface_h / 540;
    if (s < 2) s = 2;
    if (s > 4) s = 4;
    *scale = s;
    *w = surface_w < PANEL_WIDTH * s ? surface_w : PANEL_WIDTH * s;
    *h = surface_h < PANEL_HEIGHT * s ? surface_h : PANEL_HEIGHT * s;
    *x = (surface_w - *w) / 2;
    *y = (surface_h - *h) / 2;
}

size_t voice_overlay_bytes(enum voice_overlay_format format, unsigned w, unsigned h)
{
    if ((unsigned)format > VOICE_OVERLAY_A2R10G10B10_PQ || !w || !h ||
        (size_t)w > SIZE_MAX / 4 / h) return 0;
    return (size_t)w * h * 4;
}

/* Transfer copies bypass format conversion, so palettes contain encoded
 * destination pixels, not linear shader inputs. PQ values are round-to-nearest
 * ST 2084 codes, precomputed once rather than evaluating pow() per present.
 * Linear sRGB red -> BT.2020 = (0.627404, 0.069097, 0.016391).
 * At 150 nits its PQ codes are (514, 309, 205); neutral codes are 193/267/592
 * for the 2/6/200-nit panel/border/text respectively. */
static const struct overlay_palette
{
    uint32_t panel, border, text, dot;
} palettes[] = {
    [VOICE_OVERLAY_BGRA8_UNORM] = {0xff101010u, 0xff303030u, 0xffffffffu, 0xffff0000u},
    [VOICE_OVERLAY_BGRA8_SRGB] = {0xff101010u, 0xff303030u, 0xffffffffu, 0xffff0000u},
    [VOICE_OVERLAY_RGBA8_UNORM] = {0xff101010u, 0xff303030u, 0xffffffffu, 0xff0000ffu},
    [VOICE_OVERLAY_RGBA8_SRGB] = {0xff101010u, 0xff303030u, 0xffffffffu, 0xff0000ffu},
    [VOICE_OVERLAY_A2B10G10R10_SDR] = {0xc4010040u, 0xcc1304c1u, 0xffffffffu, 0xc00003ffu},
    [VOICE_OVERLAY_A2B10G10R10_PQ] = {0xcc1304c1u, 0xd0b42d0bu, 0xe5094250u, 0xccd4d602u},
    [VOICE_OVERLAY_A2R10G10B10_SDR] = {0xc4010040u, 0xcc1304c1u, 0xffffffffu, 0xfff00000u},
    [VOICE_OVERLAY_A2R10G10B10_PQ] = {0xcc1304c1u, 0xd0b42d0bu, 0xe5094250u, 0xe024d4cdu},
};

static void fill_rect(void *out, unsigned w, unsigned h, unsigned x, unsigned y,
                      unsigned rect_w, unsigned rect_h, uint32_t color)
{
    if (x >= w || y >= h) return;
    if (rect_w > w - x) rect_w = w - x;
    if (rect_h > h - y) rect_h = h - y;
    for (unsigned dy = 0; dy < rect_h; dy++)
    {
        unsigned char *row = (unsigned char *)out + ((size_t)(y + dy) * w + x) * 4;
        for (unsigned dx = 0; dx < rect_w; dx++)
            memcpy(row + (size_t)dx * 4, &color, sizeof(color));
    }
}

int voice_overlay_render(enum voice_overlay_format format, enum voice_overlay_state state,
                         unsigned w, unsigned h, unsigned scale, void *out, size_t out_bytes)
{
    size_t bytes = voice_overlay_bytes(format, w, h);
    if (state != VOICE_OVERLAY_LISTENING || !bytes || !scale || scale > 4 ||
        !out || out_bytes < bytes) return -1;
    const struct overlay_palette *colors = &palettes[format];
    fill_rect(out, w, h, 0, 0, w, h, colors->panel);
    /* A seven-logical-pixel red disk, aligned with the seven-row glyphs. */
    for (int y = -3; y <= 3; y++)
        for (int x = -3; x <= 3; x++)
            if (x * x + y * y <= 9)
                fill_rect(out, w, h, (unsigned)(11 + x) * scale, (unsigned)(11 + y) * scale,
                          scale, scale, colors->dot);
    const char *label = voice_overlay_label(state);
    for (unsigned i = 0; label[i]; i++)
        for (unsigned row = 0; row < 7; row++)
        {
            unsigned bits = title_bitmap_font[(unsigned char)label[i] - 32][row];
            for (unsigned column = 0; column < 5; column++)
                if (bits & (1u << (4 - column)))
                    fill_rect(out, w, h, (TEXT_LEFT + 6 * i + column) * scale,
                              (TEXT_TOP + row) * scale, scale, scale, colors->text);
        }
    /* Draw last so even a heavily clipped panel retains its opaque border. */
    fill_rect(out, w, h, 0, 0, w, 1, colors->border);
    fill_rect(out, w, h, 0, h - 1, w, 1, colors->border);
    fill_rect(out, w, h, 0, 0, 1, h, colors->border);
    fill_rect(out, w, h, w - 1, 0, 1, h, colors->border);
    return 0;
}
