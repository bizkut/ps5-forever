/* The home-screen icon from the installed Battle.net Launcher.exe: its
 * largest PNG icon resource, scaled to 448 pixels on a dark 512x512 tile.
 * The write broker copies the result over the home screen's copies, which a
 * title cannot reach (WB_ICON_MAGIC); they show after the console restarts.
 *
 * Adapted from ps5-forever examples/battlenet/icon.c (commit 6cf23f2d):
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: MIT
 * Changes: header-only; writes the icon and the record of its source program
 * to caller-given paths through forever-setup.h; the PNG is built in memory;
 * stb_image v2.30 (third_party/stb). */
#ifndef TITLE_ICON_H
#define TITLE_ICON_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "forever-setup.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

#define TITLE_ICON_SIZE 512
#define TITLE_ICON_LOGO 448
#define TITLE_ICON_MAX_PROGRAM (64u * 1024u * 1024u)
#define TITLE_ICON_RT_ICON 3

static const uint8_t title_icon_png_signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
static const uint8_t title_icon_tile[3] = {13, 11, 24};

static inline uint16_t title_icon_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t title_icon_rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The PE image's sections, to turn resource addresses into file offsets */
struct title_icon_pe
{
    const uint8_t *data;
    size_t size, sections, table, base;
};

struct title_icon_best
{
    const uint8_t *png;
    size_t size;
    int width, height;
};

static inline int title_icon_pe_offset(const struct title_icon_pe *pe, uint32_t rva, uint32_t length,
                                       size_t *offset)
{
    for (size_t i = 0; i < pe->sections; i++)
    {
        const uint8_t *s = pe->data + pe->table + 40 * i;
        uint32_t virtual_size = title_icon_rd32(s + 8), address = title_icon_rd32(s + 12);
        uint32_t raw_size = title_icon_rd32(s + 16), raw = title_icon_rd32(s + 20);
        uint32_t span = virtual_size > raw_size ? virtual_size : raw_size;
        if (rva < address || rva - address >= span) continue;
        *offset = (size_t)raw + (rva - address);
        return *offset <= pe->size && length <= pe->size - *offset;
    }
    return 0;
}

/* Every leaf below a resource directory, keeping the largest PNG */
static inline void title_icon_leaves(const struct title_icon_pe *pe, uint32_t entry, int depth,
                                     struct title_icon_best *best)
{
    size_t leaf, offset;
    uint32_t rva, length;
    int width, height, components;

    if (entry & 0x80000000u)
    {
        size_t dir = pe->base + (entry & 0x7fffffffu);
        unsigned count;
        if (depth > 3 || dir + 16 > pe->size) return;
        count = title_icon_rd16(pe->data + dir + 12) + title_icon_rd16(pe->data + dir + 14);
        for (unsigned i = 0; i < count && dir + 16 + 8 * (i + 1) <= pe->size; i++)
            title_icon_leaves(pe, title_icon_rd32(pe->data + dir + 16 + 8 * i + 4), depth + 1, best);
        return;
    }
    leaf = pe->base + entry;
    if (leaf + 8 > pe->size) return;
    rva = title_icon_rd32(pe->data + leaf);
    length = title_icon_rd32(pe->data + leaf + 4);
    if (length < sizeof(title_icon_png_signature) || length > INT32_MAX ||
        !title_icon_pe_offset(pe, rva, length, &offset))
        return;
    if (memcmp(pe->data + offset, title_icon_png_signature, sizeof(title_icon_png_signature)) ||
        !stbi_info_from_memory(pe->data + offset, (int)length, &width, &height, &components) ||
        (int64_t)width * height <= (int64_t)best->width * best->height)
        return;
    best->png = pe->data + offset;
    best->size = length;
    best->width = width;
    best->height = height;
}

/* The program's largest PNG icon resource: 1 found, 0 none */
static inline int title_icon_find(const uint8_t *data, size_t size, struct title_icon_best *best)
{
    struct title_icon_pe pe = {.data = data, .size = size};
    size_t header, optional, directories;
    unsigned count;
    uint32_t rsrc;

    memset(best, 0, sizeof(*best));
    if (size < 0x40 || data[0] != 'M' || data[1] != 'Z') return 0;
    header = title_icon_rd32(data + 0x3c);
    if (header > size - 24 || memcmp(data + header, "PE\0\0", 4)) return 0;
    pe.sections = title_icon_rd16(data + header + 6);
    optional = header + 24;
    if (optional + 2 > size) return 0;
    directories = optional + (title_icon_rd16(data + optional) == 0x10b ? 96 : 112);
    pe.table = optional + title_icon_rd16(data + header + 20);
    if (directories + 3 * 8 > size || pe.table + 40 * pe.sections > size) return 0;
    rsrc = title_icon_rd32(data + directories + 2 * 8);
    if (!rsrc || !title_icon_pe_offset(&pe, rsrc, 16, &pe.base)) return 0;
    count = title_icon_rd16(data + pe.base + 12) + title_icon_rd16(data + pe.base + 14);
    for (unsigned i = 0; i < count && pe.base + 16 + 8 * (i + 1) <= size; i++)
    {
        const uint8_t *e = data + pe.base + 16 + 8 * i;
        if (title_icon_rd32(e) == TITLE_ICON_RT_ICON) title_icon_leaves(&pe, title_icon_rd32(e + 4), 1, best);
    }
    return best->png != NULL;
}

/* The logo, scaled to TITLE_ICON_LOGO pixels with bilinear filtering on
 * premultiplied alpha, centred on the tile: RGB, TITLE_ICON_SIZE squared */
static inline void title_icon_compose(const uint8_t *rgba, int w, int h, uint8_t *out)
{
    const int size = TITLE_ICON_SIZE, logo = TITLE_ICON_LOGO, origin = (size - logo) / 2;
    for (int i = 0; i < size * size; i++) memcpy(out + 3 * i, title_icon_tile, 3);
    for (int y = 0; y < logo; y++)
        for (int x = 0; x < logo; x++)
        {
            float fx = (x + 0.5f) * w / logo - 0.5f, fy = (y + 0.5f) * h / logo - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, y0 = fy < 0 ? 0 : (int)fy;
            int x1 = x0 + 1 < w ? x0 + 1 : w - 1, y1 = y0 + 1 < h ? y0 + 1 : h - 1;
            float tx = fx - x0, ty = fy - y0, c[4] = {0, 0, 0, 0};
            if (tx < 0) tx = 0;
            if (ty < 0) ty = 0;
            const int xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
            const float ws[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
            for (int k = 0; k < 4; k++)
            {
                const uint8_t *p = rgba + 4 * ((size_t)ys[k] * w + xs[k]);
                float a = p[3] / 255.0f;
                for (int ch = 0; ch < 3; ch++) c[ch] += ws[k] * p[ch] * a;
                c[3] += ws[k] * a;
            }
            uint8_t *o = out + 3 * ((size_t)(origin + y) * size + origin + x);
            for (int ch = 0; ch < 3; ch++)
            {
                float v = c[ch] + title_icon_tile[ch] * (1 - c[3]);
                o[ch] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
            }
        }
}

static inline uint32_t title_icon_crc(uint32_t value, const uint8_t *data, size_t size)
{
    value = ~value;
    for (size_t i = 0; i < size; i++)
    {
        value ^= data[i];
        for (int k = 0; k < 8; k++) value = value & 1 ? 0xedb88320u ^ (value >> 1) : value >> 1;
    }
    return ~value;
}

static inline uint8_t *title_icon_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return p + 4;
}

static inline uint8_t *title_icon_chunk(uint8_t *p, const char *type, const uint8_t *data, size_t size)
{
    uint8_t *start;
    p = title_icon_put32(p, (uint32_t)size);
    start = p;
    memcpy(p, type, 4);
    if (size) memcpy(p + 4, data, size);
    p += 4 + size;
    return title_icon_put32(p, title_icon_crc(0, start, 4 + size));
}

/* A TITLE_ICON_SIZE-square RGB PNG, its image data in stored (uncompressed)
 * deflate blocks: no compressor needed, about 770 KiB. Malloc'd, or NULL. */
static inline uint8_t *title_icon_png(const uint8_t *rgb, size_t *png_size)
{
    const size_t size = TITLE_ICON_SIZE, row = 1 + 3 * size, raw = row * size;
    const size_t blocks = (raw + 65534) / 65535, idat = 2 + raw + 5 * blocks + 4;
    const uint8_t ihdr[13] = {0, 0, size >> 8, size & 0xff, 0, 0, size >> 8, size & 0xff, 8, 2, 0, 0, 0};
    uint8_t *z = malloc(idat), *png = z ? malloc(8 + 25 + 12 + idat + 12) : NULL, *p;
    uint32_t a = 1, b = 0, adler;

    if (!png)
    {
        free(z);
        return NULL;
    }
    p = z;
    *p++ = 0x78;
    *p++ = 0x01;
    for (size_t done = 0, y = 0, x = 0; done < raw;)
    {
        size_t length = raw - done < 65535 ? raw - done : 65535;
        *p++ = done + length == raw;
        *p++ = (uint8_t)length;
        *p++ = (uint8_t)(length >> 8);
        *p++ = (uint8_t)~length;
        *p++ = (uint8_t)(~length >> 8);
        for (size_t i = 0; i < length; i++)
        {
            uint8_t v = x == 0 ? 0 : rgb[(y * size) * 3 + x - 1]; /* filter byte 0, then the row */
            *p++ = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
            if (++x == row) x = 0, y++;
        }
        done += length;
    }
    adler = b << 16 | a;
    p = title_icon_put32(p, adler);
    memcpy(png, title_icon_png_signature, 8);
    p = title_icon_chunk(png + 8, "IHDR", ihdr, sizeof(ihdr));
    p = title_icon_chunk(p, "IDAT", z, idat);
    p = title_icon_chunk(p, "IEND", NULL, 0);
    free(z);
    *png_size = (size_t)(p - png);
    return png;
}

/* The whole file, at most limit bytes: malloc'd, or NULL with errno */
static inline uint8_t *title_icon_read(const char *path, size_t limit, size_t *size)
{
    struct stat st;
    uint8_t *data = NULL;
    int fd = open(path, O_RDONLY);

    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > limit)
        errno = EFBIG;
    else if ((data = malloc((size_t)st.st_size)))
    {
        size_t done = 0;
        ssize_t got;
        while (done < (size_t)st.st_size && (got = read(fd, data + done, (size_t)st.st_size - done)) > 0)
            done += (size_t)got;
        if (done != (size_t)st.st_size)
        {
            free(data);
            data = NULL;
            errno = EIO;
        }
        *size = done;
    }
    close(fd);
    return data;
}

/* Make icon from program's icon unless source already records this program
 * (its size and time). 1 written, 0 current, -errno failure (-ENOENT: no
 * program, -EINVAL: it has no PNG icon that decodes). */
static inline int title_icon_update(const char *program, const char *icon, const char *source)
{
    struct title_icon_best best;
    struct stat st;
    char record[64], recorded[64];
    uint8_t *data, *rgba, *rgb, *png;
    size_t size = 0, png_size = 0;
    int width, height, components, error;

    if (stat(program, &st)) return -errno;
    snprintf(record, sizeof(record), "%lld %lld\n", (long long)st.st_size, (long long)st.st_mtime);
    if (forever_exists(icon) && forever_read_file(source, recorded, sizeof(recorded)) >= 0 &&
        !strcmp(record, recorded))
        return 0;
    if (!(data = title_icon_read(program, TITLE_ICON_MAX_PROGRAM, &size))) return -errno;
    if (!title_icon_find(data, size, &best))
    {
        free(data);
        return -EINVAL;
    }
    rgba = stbi_load_from_memory(best.png, (int)best.size, &width, &height, &components, 4);
    free(data);
    if (!rgba) return -EINVAL;
    if (!(rgb = malloc((size_t)TITLE_ICON_SIZE * TITLE_ICON_SIZE * 3)))
    {
        stbi_image_free(rgba);
        return -ENOMEM;
    }
    title_icon_compose(rgba, width, height, rgb);
    stbi_image_free(rgba);
    png = title_icon_png(rgb, &png_size);
    free(rgb);
    if (!png) return -ENOMEM;
    error = forever_write_file(icon, png, png_size);
    free(png);
    if (error) return error;
    return (error = forever_write_file(source, record, strlen(record))) ? error : 1;
}

#endif
