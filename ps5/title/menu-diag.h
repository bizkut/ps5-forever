#ifndef PS5_TITLE_MENU_DIAG_H
#define PS5_TITLE_MENU_DIAG_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"

enum forever_menu_diag_kind
{
    FOREVER_MENU_DIAG_PAD,
    FOREVER_MENU_DIAG_KEY,
    FOREVER_MENU_DIAG_CLICK,
    FOREVER_MENU_DIAG_WAIT,
    FOREVER_MENU_DIAG_DUMP,
    FOREVER_MENU_DIAG_ADDON,
};

struct forever_menu_diag_command
{
    enum forever_menu_diag_kind kind;
    uint32_t code;
    int x, y;
    unsigned milliseconds;
    char name[49];
};

static inline int forever_menu_diag_number(const char *text, long minimum, long maximum, long *value)
{
    char *end;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || end == text || *end || parsed < minimum || parsed > maximum) return -EINVAL;
    *value = parsed;
    return 0;
}

static inline int forever_menu_diag_parse(
    const char *input, size_t length, struct forever_menu_diag_command *command)
{
    static const struct { const char *name; uint32_t code; } pads[] = {
        {"OPTIONS", PAD_OPTIONS}, {"UP", PAD_UP}, {"DOWN", PAD_DOWN},
        {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT}, {"CROSS", PAD_CROSS},
        {"CIRCLE", PAD_CIRCLE}, {"L1", PAD_L1}, {"R1", PAD_R1},
        {"SQUARE", PAD_SQUARE}, {"TRIANGLE", PAD_TRIANGLE}, {"R3", PAD_R3},
        {"L2", PAD_L2},
    };
    static const struct { const char *name; uint32_t code; } keys[] = {
        {"F12", 0x45}, {"ESCAPE", 0x29}, {"ENTER", 0x28},
        {"UP", 0x52}, {"DOWN", 0x51},
    };
    char line[160], *verb, *first, *second;
    long number;

    if (!input || !command || !length || length >= sizeof(line)) return -EINVAL;
    memcpy(line, input, length);
    line[length] = 0;
    while (length && (line[length - 1] == '\r' || line[length - 1] == ' ' ||
                      line[length - 1] == '\t')) line[--length] = 0;
    verb = line;
    while (*verb == ' ' || *verb == '\t') verb++;
    first = strpbrk(verb, " \t");
    if (!first) return -EINVAL;
    *first++ = 0;
    while (*first == ' ' || *first == '\t') first++;
    if (!*first) return -EINVAL;
    *command = (struct forever_menu_diag_command){0};
    if (!strcmp(verb, "pad") || !strcmp(verb, "key"))
    {
        const int pad = !strcmp(verb, "pad");
        const size_t count = pad ? sizeof(pads) / sizeof(pads[0]) : sizeof(keys) / sizeof(keys[0]);
        for (size_t i = 0; i < count; i++)
        {
            const char *name = pad ? pads[i].name : keys[i].name;
            if (!strcmp(first, name))
            {
                command->kind = pad ? FOREVER_MENU_DIAG_PAD : FOREVER_MENU_DIAG_KEY;
                command->code = pad ? pads[i].code : keys[i].code;
                return 0;
            }
        }
        return -EINVAL;
    }
    if (!strcmp(verb, "wait"))
    {
        if (forever_menu_diag_number(first, 0, 60000, &number)) return -EINVAL;
        command->kind = FOREVER_MENU_DIAG_WAIT;
        command->milliseconds = (unsigned)number;
        return 0;
    }
    if (!strcmp(verb, "click"))
    {
        second = strpbrk(first, " \t");
        if (!second) return -EINVAL;
        *second++ = 0;
        while (*second == ' ' || *second == '\t') second++;
        if (forever_menu_diag_number(first, 0, 1919, &number)) return -EINVAL;
        command->x = (int)number;
        if (forever_menu_diag_number(second, 0, 1079, &number)) return -EINVAL;
        command->y = (int)number;
        command->kind = FOREVER_MENU_DIAG_CLICK;
        return 0;
    }
    if (!strcmp(verb, "addon"))
    {
        length = strlen(first);
        if (!length || length >= sizeof(command->name) || strstr(first, "..") ||
            first[0] == '/') return -EINVAL;
        for (size_t i = 0; i < length; i++)
            if (!((first[i] >= 'a' && first[i] <= 'z') ||
                  (first[i] >= 'A' && first[i] <= 'Z') ||
                  (first[i] >= '0' && first[i] <= '9') ||
                  strchr(":._-/", first[i])))
                return -EINVAL;
        memcpy(command->name, first, length + 1);
        command->kind = FOREVER_MENU_DIAG_ADDON;
        return 0;
    }
    if (!strcmp(verb, "dump"))
    {
        length = strlen(first);
        if (!length || length >= sizeof(command->name)) return -EINVAL;
        for (size_t i = 0; i < length; i++)
            if (!((first[i] >= 'a' && first[i] <= 'z') ||
                  (first[i] >= 'A' && first[i] <= 'Z') ||
                  (first[i] >= '0' && first[i] <= '9') || first[i] == '-' || first[i] == '_'))
                return -EINVAL;
        memcpy(command->name, first, length + 1);
        command->kind = FOREVER_MENU_DIAG_DUMP;
        return 0;
    }
    return -EINVAL;
}

#endif
