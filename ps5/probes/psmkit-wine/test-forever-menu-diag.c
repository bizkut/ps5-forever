#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../../title/menu-diag.h"

static struct forever_menu_diag_command parse(const char *line)
{
    struct forever_menu_diag_command command;
    assert(!forever_menu_diag_parse(line, strlen(line), &command));
    return command;
}

int main(void)
{
    struct forever_menu_diag_command command;
    static const struct { const char *line; unsigned code; } pads[] = {
        {"pad OPTIONS", PAD_OPTIONS}, {"pad UP", PAD_UP}, {"pad DOWN", PAD_DOWN},
        {"pad LEFT", PAD_LEFT}, {"pad RIGHT", PAD_RIGHT}, {"pad CROSS", PAD_CROSS},
        {"pad CIRCLE", PAD_CIRCLE}, {"pad L2", PAD_L2}, {"pad TRIANGLE", PAD_TRIANGLE},
    };
    for (size_t i = 0; i < sizeof(pads) / sizeof(pads[0]); i++)
    {
        command = parse(pads[i].line);
        assert(command.kind == FOREVER_MENU_DIAG_PAD && command.code == pads[i].code);
    }
    command = parse("key F12"); assert(command.kind == FOREVER_MENU_DIAG_KEY && command.code == 0x45);
    command = parse("key ESCAPE"); assert(command.code == 0x29);
    command = parse("key ENTER"); assert(command.code == 0x28);
    command = parse("key UP"); assert(command.code == 0x52);
    command = parse("key DOWN\r"); assert(command.code == 0x51);
    command = parse("click 1919 1079");
    assert(command.kind == FOREVER_MENU_DIAG_CLICK && command.x == 1919 && command.y == 1079);
    command = parse("wait 500");
    assert(command.kind == FOREVER_MENU_DIAG_WAIT && command.milliseconds == 500);
    command = parse("dump main-menu_1");
    assert(command.kind == FOREVER_MENU_DIAG_DUMP && !strcmp(command.name, "main-menu_1"));
    static const char *const invalid[] = {
        "pad R2", "pad options", "key A", "wait -1", "wait 60001",
        "click 1920 0", "click 0 1080", "click 1", "dump ../escape",
        "dump has.dot", "dump ", "unknown value", "",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(forever_menu_diag_parse(invalid[i], strlen(invalid[i]), &command) == -EINVAL);
    puts("forever menu diagnostic command parser: PASS");
    return 0;
}
