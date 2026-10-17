#ifndef PS5_TITLE_FOREVER_MENU_H
#define PS5_TITLE_FOREVER_MENU_H

#include <stddef.h>

#define FOREVER_MENU_TEXT_BYTES 1024
#define FOREVER_MENU_STATUS_BYTES 192

/* Text rows used by the compositor and mouse hit testing. */
#define FOREVER_MENU_LINE_FRAME_GENERATION 2
#define FOREVER_MENU_LINE_HDR 3
#define FOREVER_MENU_LINE_HUD 4
#define FOREVER_MENU_LINE_ADDONS 6
#define FOREVER_MENU_LINE_REINSTALL 8
#define FOREVER_MENU_LINE_UNINSTALL 9
#define FOREVER_MENU_LINE_CLOSE 11
#define FOREVER_MENU_LINE_CANCEL 7
#define FOREVER_MENU_LINE_CONFIRM 8

enum forever_menu_page
{
    FOREVER_MENU_CLOSED,
    FOREVER_MENU_MAIN,
    FOREVER_MENU_CONFIRM_REINSTALL,
    FOREVER_MENU_CONFIRM_UNINSTALL,
    FOREVER_MENU_PROGRESS,
    FOREVER_MENU_NOTICE,
};

enum forever_menu_item
{
    FOREVER_MENU_FRAME_GENERATION,
    FOREVER_MENU_HDR,
    FOREVER_MENU_HUD,
    FOREVER_MENU_ADDONS,
    FOREVER_MENU_REINSTALL,
    FOREVER_MENU_UNINSTALL,
    FOREVER_MENU_CLOSE,
};

enum forever_menu_command
{
    FOREVER_MENU_COMMAND_NONE,
    FOREVER_MENU_COMMAND_SET_FRAME_GENERATION,
    FOREVER_MENU_COMMAND_SET_HDR,
    FOREVER_MENU_COMMAND_SET_HUD,
    FOREVER_MENU_COMMAND_REINSTALL,
    FOREVER_MENU_COMMAND_UNINSTALL,
    FOREVER_MENU_COMMAND_CLOSE,
};

enum forever_frame_generation
{
    FOREVER_FG_OFF,
    FOREVER_FG_ON,
    FOREVER_FG_AUTO,
};

struct forever_menu_values
{
    enum forever_frame_generation frame_generation;
    int hdr;
    int hud;
};

struct forever_menu_result
{
    enum forever_menu_command command;
    int enabled;
};

struct forever_menu_state
{
    enum forever_menu_page page;
    enum forever_menu_item selected;
    int confirmation_selected; /* zero is Cancel, deliberately the default */
    struct forever_menu_values values;
    char status[FOREVER_MENU_STATUS_BYTES];
};

void forever_menu_init(struct forever_menu_state *menu);
void forever_menu_open(struct forever_menu_state *menu, const struct forever_menu_values *values);
int forever_menu_is_open(const struct forever_menu_state *menu);
void forever_menu_move(struct forever_menu_state *menu, int direction);
struct forever_menu_result forever_menu_select(struct forever_menu_state *menu);
struct forever_menu_result forever_menu_back(struct forever_menu_state *menu);
struct forever_menu_result forever_menu_click_line(struct forever_menu_state *menu, int line);
void forever_menu_commit(struct forever_menu_state *menu, enum forever_menu_command command, int enabled);
void forever_menu_status(struct forever_menu_state *menu, const char *text);
void forever_menu_progress(struct forever_menu_state *menu, const char *text);
void forever_menu_notice(struct forever_menu_state *menu, const char *text);
void forever_menu_close(struct forever_menu_state *menu);
void forever_menu_render(const struct forever_menu_state *menu, char *text, size_t size);

#endif
