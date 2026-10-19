#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../title/voice-overlay-raster.h"

static void labels_and_layout(void)
{
    unsigned x, y, w, h, scale;
    assert(!strcmp(voice_overlay_label(VOICE_OVERLAY_LISTENING), "Listening"));
    assert(!strcmp(voice_overlay_label(VOICE_OVERLAY_HIDDEN), ""));
    voice_overlay_layout(1920, 1080, VOICE_OVERLAY_LISTENING, &x, &y, &w, &h, &scale);
    assert(x == 878 && y == 517 && w == 164 && h == 46 && scale == 2);
    voice_overlay_layout(3840, 2160, VOICE_OVERLAY_LISTENING, &x, &y, &w, &h, &scale);
    assert(x == 1756 && y == 1034 && w == 328 && h == 92 && scale == 4);
    voice_overlay_layout(200, 100, VOICE_OVERLAY_LISTENING, &x, &y, &w, &h, &scale);
    assert(x == (200 - w) / 2 && y == (100 - h) / 2 &&
           x + w <= 200 && y + h <= 100 && scale == 2);
    voice_overlay_layout(1, 1, VOICE_OVERLAY_LISTENING, &x, &y, &w, &h, &scale);
    assert(!x && !y && w == 1 && h == 1 && scale == 2);
    voice_overlay_layout(1920, 1080, VOICE_OVERLAY_HIDDEN, &x, &y, &w, &h, &scale);
    assert(!x && !y && !w && !h && !scale);
    voice_overlay_layout(0, 1080, VOICE_OVERLAY_LISTENING, &x, &y, &w, &h, &scale);
    assert(!x && !y && !w && !h && !scale);
}

static unsigned pq_code(double nits)
{
    /* Independent ST 2084 oracle, not the renderer's precomputed palette. */
    double p = pow(nits / 10000.0, 2610.0 / 16384.0);
    double value = pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) /
                      (1.0 + 2392.0 / 128.0 * p), 2523.0 / 32.0);
    return (unsigned)floor(value * 1023.0 + 0.5);
}

static uint32_t packed_pixel(const unsigned char *image, unsigned w, unsigned x, unsigned y)
{
    uint32_t pixel;
    memcpy(&pixel, image + ((size_t)y * w + x) * 4, sizeof(pixel));
    return pixel;
}

static void encodings(void)
{
    const struct
    {
        enum voice_overlay_format format;
        uint32_t panel, border, text, dot;
    } cases[] = {
        {VOICE_OVERLAY_BGRA8_UNORM, 0xff101010u, 0xff303030u, 0xffffffffu, 0xffff0000u},
        {VOICE_OVERLAY_BGRA8_SRGB, 0xff101010u, 0xff303030u, 0xffffffffu, 0xffff0000u},
        {VOICE_OVERLAY_RGBA8_UNORM, 0xff101010u, 0xff303030u, 0xffffffffu, 0xff0000ffu},
        {VOICE_OVERLAY_RGBA8_SRGB, 0xff101010u, 0xff303030u, 0xffffffffu, 0xff0000ffu},
        {VOICE_OVERLAY_A2B10G10R10_SDR, 0xc4010040u, 0xcc1304c1u, 0xffffffffu, 0xc00003ffu},
        {VOICE_OVERLAY_A2B10G10R10_PQ, 0xcc1304c1u, 0xd0b42d0bu, 0xe5094250u, 0xccd4d602u},
        {VOICE_OVERLAY_A2R10G10B10_SDR, 0xc4010040u, 0xcc1304c1u, 0xffffffffu, 0xfff00000u},
        {VOICE_OVERLAY_A2R10G10B10_PQ, 0xcc1304c1u, 0xd0b42d0bu, 0xe5094250u, 0xe024d4cdu},
    };
    /* Deliberately unaligned output, with guard bytes on both sides. */
    unsigned char storage[164 * 46 * 4 + 2];
    unsigned char *image = storage + 1;
    size_t bytes = sizeof(storage) - 2;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        assert(voice_overlay_bytes(cases[i].format, 164, 46) == bytes);
        assert(!voice_overlay_bytes(cases[i].format, 0, 46));
        assert(!voice_overlay_bytes(cases[i].format, UINT_MAX, UINT_MAX));
        memset(storage, 0xa5, sizeof(storage));
        assert(!voice_overlay_render(cases[i].format, VOICE_OVERLAY_LISTENING,
                                     164, 46, 2, image, bytes));
        assert(storage[0] == 0xa5 && storage[sizeof(storage) - 1] == 0xa5);
        assert(packed_pixel(image, 164, 1, 1) == cases[i].panel);
        assert(packed_pixel(image, 164, 0, 0) == cases[i].border);
        assert(packed_pixel(image, 164, 163, 45) == cases[i].border);
        /* First stroke of L, its scale-2 block, and the empty pixel after it. */
        assert(packed_pixel(image, 164, 40, 16) == cases[i].text);
        assert(packed_pixel(image, 164, 41, 17) == cases[i].text);
        assert(packed_pixel(image, 164, 42, 16) == cases[i].panel);
        assert(packed_pixel(image, 164, 22, 22) == cases[i].dot);
        for (size_t p = 0; p < bytes; p += 4)
        {
            uint32_t pixel;
            memcpy(&pixel, image + p, sizeof(pixel));
            assert(pixel == cases[i].panel || pixel == cases[i].border ||
                   pixel == cases[i].text || pixel == cases[i].dot);
        }
        if (cases[i].format == VOICE_OVERLAY_A2B10G10R10_PQ ||
            cases[i].format == VOICE_OVERLAY_A2R10G10B10_PQ)
        {
            unsigned white = packed_pixel(image, 164, 40, 16) & 1023;
            unsigned expected = pq_code(200.0);
            assert(white + 1 >= expected && white <= expected + 1);
            assert((cases[i].panel & 1023) == pq_code(2.0));
            assert((cases[i].border & 1023) == pq_code(6.0));
            /* A 150-nit linear sRGB red converted to BT.2020 before PQ. */
            unsigned red = cases[i].format == VOICE_OVERLAY_A2B10G10R10_PQ ?
                cases[i].dot & 1023 : (cases[i].dot >> 20) & 1023;
            unsigned blue = cases[i].format == VOICE_OVERLAY_A2B10G10R10_PQ ?
                (cases[i].dot >> 20) & 1023 : cases[i].dot & 1023;
            assert(red == pq_code(150.0 * 0.627404));
            assert(((cases[i].dot >> 10) & 1023) == pq_code(150.0 * 0.069097));
            assert(blue == pq_code(150.0 * 0.016391));
        }
    }
}

static void rejected_and_clipped(void)
{
    unsigned char image[164 * 46 * 4], original[sizeof(image)];
    memset(image, 0xa5, sizeof(image));
    memcpy(original, image, sizeof(image));
    assert(voice_overlay_render(VOICE_OVERLAY_BGRA8_UNORM, VOICE_OVERLAY_HIDDEN,
                                164, 46, 2, image, sizeof(image)) == -1);
    assert(!memcmp(image, original, sizeof(image)));
    assert(voice_overlay_render(VOICE_OVERLAY_BGRA8_UNORM, VOICE_OVERLAY_LISTENING,
                                164, 46, 2, image, sizeof(image) - 1) == -1);
    assert(!memcmp(image, original, sizeof(image)));
    assert(voice_overlay_render(VOICE_OVERLAY_BGRA8_UNORM, VOICE_OVERLAY_LISTENING,
                                164, 46, 0, image, sizeof(image)) == -1);
    assert(voice_overlay_render(VOICE_OVERLAY_BGRA8_UNORM, VOICE_OVERLAY_LISTENING,
                                164, 46, 2, NULL, sizeof(image)) == -1);
    assert(!voice_overlay_bytes((enum voice_overlay_format)99, 164, 46));
    assert(voice_overlay_render((enum voice_overlay_format)99, VOICE_OVERLAY_LISTENING,
                                164, 46, 2, image, sizeof(image)) == -1);
    assert(!memcmp(image, original, sizeof(image)));
    assert(!voice_overlay_render(VOICE_OVERLAY_BGRA8_UNORM, VOICE_OVERLAY_LISTENING,
                                 1, 1, 2, image, 4));
    assert(packed_pixel(image, 1, 0, 0) == 0xff303030u);
    assert(!memcmp(image + 4, original + 4, sizeof(image) - 4));
}

int main(void)
{
    labels_and_layout();
    encodings();
    rejected_and_clipped();
    puts("title voice overlay: ok");
    return 0;
}
