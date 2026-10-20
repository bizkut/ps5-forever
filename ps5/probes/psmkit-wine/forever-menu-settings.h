/* Bounded, host-testable file operations used by the Forever popup menu. */
#ifndef FOREVER_MENU_SETTINGS_H
#define FOREVER_MENU_SETTINGS_H

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "forever-setup.h"
#include "../../title/forever-menu.h"

#include "writebroker-menu.h"
#define FOREVER_MENU_FILE_BYTES 16384
#define FOREVER_HDR_OUTPUT_DEFAULT "hdr=on\n"

static inline int forever_menu_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Find one whitespace-delimited key=value token. Duplicate keys are refused. */
static inline int forever_menu_switch_span(const char *text, size_t size, const char *key,
                                            size_t *begin, size_t *end)
{
    size_t key_size = strlen(key);
    int found = 0;
    for (size_t i = 0; i < size;)
    {
        size_t token;
        while (i < size && forever_menu_space(text[i])) i++;
        token = i;
        while (i < size && !forever_menu_space(text[i])) i++;
        if (i - token > key_size && text[token + key_size] == '=' &&
            !memcmp(text + token, key, key_size))
        {
            if (found) return -EINVAL;
            found = 1;
            *begin = token + key_size + 1;
            *end = i;
        }
    }
    return found;
}

static inline int forever_menu_read_switch(const char *path, const char *key, int fallback,
                                            int *enabled)
{
    char text[FOREVER_MENU_FILE_BYTES];
    size_t begin = 0, end = 0;
    ssize_t size = forever_read_file(path, text, sizeof(text));
    int found;
    *enabled = !!fallback;

    if (size == -ENOENT)
    {
        *enabled = !!fallback;
        return 0;
    }
    if (size < 0) return (int)size;
    found = forever_menu_switch_span(text, (size_t)size, key, &begin, &end);
    if (found < 0) return found;
    if (!found)
    {
        *enabled = !!fallback;
        return 0;
    }
    if (end - begin == 2 && !memcmp(text + begin, "on", 2)) *enabled = 1;
    else if (end - begin == 3 && !memcmp(text + begin, "off", 3)) *enabled = 0;
    else return -EINVAL;
    return 0;
}

static inline int forever_menu_read_frame_generation(
    const char *path, enum forever_frame_generation fallback,
    enum forever_frame_generation *value)
{
    char text[FOREVER_MENU_FILE_BYTES];
    size_t begin = 0, end = 0;
    ssize_t size;
    int found;

    *value = fallback;
    size = forever_read_file(path, text, sizeof(text));
    if (size == -ENOENT) return 0;
    if (size < 0) return (int)size;
    found = forever_menu_switch_span(text, (size_t)size, "fg", &begin, &end);
    if (found < 0) return found;
    if (!found) return 0;
    if (end - begin == 2 && !memcmp(text + begin, "on", 2)) *value = FOREVER_FG_ON;
    else if (end - begin == 3 && !memcmp(text + begin, "off", 3)) *value = FOREVER_FG_OFF;
    else if (end - begin == 4 && !memcmp(text + begin, "auto", 4)) *value = FOREVER_FG_AUTO;
    else return -EINVAL;
    return 0;
}

static inline int forever_menu_read_settings(
    const char *frame_generation, const char *hdr, const char *hud, const char *mouse,
    const char *voice, enum forever_frame_generation *frame_generation_value,
    int *hdr_value, int *hud_value, int *invert_scroll_value, int *voice_value)
{
    int first = 0, error;
    *frame_generation_value = FOREVER_FG_ON;
    *hdr_value = 1;
    *hud_value = 0;
    *invert_scroll_value = 1;
    *voice_value = 0;
    error = forever_menu_read_frame_generation(
        frame_generation, FOREVER_FG_ON, frame_generation_value);
    if (error && !first) first = error;
    error = forever_menu_read_switch(hdr, "hdr", 1, hdr_value);
    if (error && !first) first = error;
    error = forever_menu_read_switch(hud, "hud", 0, hud_value);
    if (error && !first) first = error;
    error = forever_menu_read_switch(mouse, "invert_scroll", 1, invert_scroll_value);
    if (error && !first) first = error;
    error = forever_menu_read_switch(voice, "voice", 0, voice_value);
    if (error && !first) first = error;
    return first;
}

static inline int forever_menu_rewrite_switch(const char *path, const char *key, int enabled)
{
    char text[FOREVER_MENU_FILE_BYTES], output[FOREVER_MENU_FILE_BYTES + 16], parent[1100];
    const char *value = enabled ? "on" : "off";
    size_t value_size = enabled ? 2 : 3, begin = 0, end = 0, output_size, key_size = strlen(key);
    ssize_t size = forever_read_file(path, text, sizeof(text));
    int found;

    if (size == -ENOENT) size = 0;
    else if (size < 0) return (int)size;
    found = forever_menu_switch_span(text, (size_t)size, key, &begin, &end);
    if (found < 0) return found;
    if (found)
    {
        if (begin + value_size + (size_t)size - end > sizeof(output)) return -EFBIG;
        memcpy(output, text, begin);
        memcpy(output + begin, value, value_size);
        memcpy(output + begin + value_size, text + end, (size_t)size - end);
        output_size = begin + value_size + (size_t)size - end;
    }
    else
    {
        output_size = (size_t)size;
        if (output_size && !forever_menu_space(text[output_size - 1])) output[output_size++] = '\n';
        if (output_size + key_size + value_size + 2 > sizeof(output)) return -EFBIG;
        if (size) memcpy(output, text, (size_t)size);
        memcpy(output + output_size, key, key_size);
        output_size += key_size;
        output[output_size++] = '=';
        memcpy(output + output_size, value, value_size);
        output_size += value_size;
        output[output_size++] = '\n';
    }
    if (strlen(path) >= sizeof(parent)) return -ENAMETOOLONG;
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash) return -EINVAL;
    *slash = 0;
    int error = forever_mkdirs(parent);
    return error ? error : forever_write_file(path, output, output_size);
}

/* Preserve every byte except the single HDROutput digit; append the setting
 * when an existing user Config.wtf has no such line. */
static inline int forever_menu_rewrite_wow_hdr(const char *path, int enabled)
{
    char text[FOREVER_MENU_FILE_BYTES], output[FOREVER_MENU_FILE_BYTES + 32];
    static const char prefix[] = "SET HDROutput \"";
    size_t output_size, value_at = 0;
    ssize_t size = forever_read_file(path, text, sizeof(text));
    int found = 0;

    if (size < 0) return (int)size;
    memcpy(output, text, (size_t)size);
    output_size = (size_t)size;
    for (size_t start = 0; start < (size_t)size;)
    {
        size_t end = start;
        while (end < (size_t)size && text[end] != '\n') end++;
        size_t at = start;
        while (at < end && (text[at] == ' ' || text[at] == '\t')) at++;
        if (end - at >= sizeof(prefix) + 1 && !memcmp(text + at, prefix, sizeof(prefix) - 1) &&
            (text[at + sizeof(prefix) - 1] == '0' || text[at + sizeof(prefix) - 1] == '1') &&
            text[at + sizeof(prefix)] == '"')
        {
            if (found) return -EINVAL;
            found = 1;
            value_at = at + sizeof(prefix) - 1;
        }
        start = end < (size_t)size ? end + 1 : end;
    }
    if (found)
        output[value_at] = enabled ? '1' : '0';
    else
    {
        static const char setting[] = "SET HDROutput \"1\"\n";
        if (output_size && output[output_size - 1] != '\n') output[output_size++] = '\n';
        if (output_size + sizeof(setting) - 1 > sizeof(output)) return -EFBIG;
        memcpy(output + output_size, setting, sizeof(setting) - 1);
        output[output_size + 15] = enabled ? '1' : '0';
        output_size += sizeof(setting) - 1;
    }
    return forever_write_file(path, output, output_size);
}

struct forever_wow_hdr_context { int enabled, changed; };
static inline int forever_wow_hdr_visit(const struct wow_channel *channel, void *opaque)
{
    struct forever_wow_hdr_context *context = opaque;
    char config[WOW_PATH_BYTES];
    int error;
    if (!channel->executable[0]) return 0;
    if ((error = wow_join(config, sizeof(config), channel->unix_cwd, "WTF/Config.wtf")) ||
        (error = forever_menu_rewrite_wow_hdr(config, context->enabled))) return error;
    ++context->changed;
    return 0;
}
/* Install absent presets, then change only HDROutput in each detected channel. */
static inline int forever_menu_set_wow_hdr(const char *share, const char *prefix, int enabled)
{
    struct forever_wow_hdr_context context = {enabled, 0};
    int error = forever_install_wow_config(share, prefix);
    if (error < 0) return error;
    error = wow_channels_scan(prefix, forever_wow_hdr_visit, &context);
    return error ? error : context.changed;
}

/* Missing or malformed flags retain the release's established HDR-on path. */
static inline int forever_menu_hdr_runtime_enabled(const char *path)
{
    int enabled = 1;
    return forever_menu_read_switch(path, "hdr", 1, &enabled) ? 1 : enabled;
}

static inline int forever_menu_path(char *path, size_t size, const char *prefix, unsigned index,
                                    int reinstall)
{
    const char *relative = wb_menu_remove_relative(index, reinstall);
    int length;
    if (!relative) return -EINVAL;
    length = snprintf(path, size, "%s%s", prefix, relative);
    return length < 0 || (size_t)length >= size ? -ENAMETOOLONG : 0;
}

static inline int forever_menu_delete_path_allowed(const char *prefix, const char *path, int reinstall)
{
    char allowed[1100];
    for (unsigned i = 0; wb_menu_remove_relative(i, reinstall); i++)
        if (!forever_menu_path(allowed, sizeof(allowed), prefix, i, reinstall) && !strcmp(allowed, path))
            return 1;
    return 0;
}


#endif
