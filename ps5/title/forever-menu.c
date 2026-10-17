#include <stdio.h>
#include <string.h>
#include "forever-menu.h"

static const enum forever_menu_item selectable[] = {
    FOREVER_MENU_FRAME_GENERATION,
    FOREVER_MENU_HDR,
    FOREVER_MENU_HUD,
    FOREVER_MENU_REINSTALL,
    FOREVER_MENU_UNINSTALL,
    FOREVER_MENU_CLOSE,
};

static struct forever_menu_result none(void)
{
    return (struct forever_menu_result){FOREVER_MENU_COMMAND_NONE, 0};
}

static void copy_status(struct forever_menu_state *menu, const char *text)
{
    snprintf(menu->status, sizeof(menu->status), "%s", text ? text : "");
}

void forever_menu_init(struct forever_menu_state *menu)
{
    memset(menu, 0, sizeof(*menu));
}

void forever_menu_open(struct forever_menu_state *menu, const struct forever_menu_values *values)
{
    menu->page = FOREVER_MENU_MAIN;
    menu->selected = FOREVER_MENU_FRAME_GENERATION;
    menu->confirmation_selected = 0;
    if (values) menu->values = *values;
    menu->status[0] = 0;
}

int forever_menu_is_open(const struct forever_menu_state *menu)
{
    return menu->page != FOREVER_MENU_CLOSED;
}

void forever_menu_move(struct forever_menu_state *menu, int direction)
{
    if (!direction || menu->page == FOREVER_MENU_CLOSED || menu->page == FOREVER_MENU_PROGRESS ||
        menu->page == FOREVER_MENU_NOTICE) return;
    if (menu->page == FOREVER_MENU_CONFIRM_REINSTALL || menu->page == FOREVER_MENU_CONFIRM_UNINSTALL)
    {
        menu->confirmation_selected = !menu->confirmation_selected;
        return;
    }
    for (size_t i = 0; i < sizeof(selectable) / sizeof(selectable[0]); i++)
        if (selectable[i] == menu->selected)
        {
            size_t count = sizeof(selectable) / sizeof(selectable[0]);
            menu->selected = selectable[(i + (direction > 0 ? 1 : count - 1)) % count];
            return;
        }
    menu->selected = FOREVER_MENU_FRAME_GENERATION;
}

struct forever_menu_result forever_menu_select(struct forever_menu_state *menu)
{
    if (menu->page == FOREVER_MENU_CONFIRM_REINSTALL || menu->page == FOREVER_MENU_CONFIRM_UNINSTALL)
    {
        enum forever_menu_page page = menu->page;
        if (!menu->confirmation_selected)
        {
            menu->page = FOREVER_MENU_MAIN;
            menu->selected = page == FOREVER_MENU_CONFIRM_REINSTALL ?
                FOREVER_MENU_REINSTALL : FOREVER_MENU_UNINSTALL;
            return none();
        }
        return (struct forever_menu_result){
            page == FOREVER_MENU_CONFIRM_REINSTALL ? FOREVER_MENU_COMMAND_REINSTALL :
                                                     FOREVER_MENU_COMMAND_UNINSTALL,
            0};
    }
    if (menu->page != FOREVER_MENU_MAIN) return none();
    switch (menu->selected)
    {
    case FOREVER_MENU_FRAME_GENERATION:
        return (struct forever_menu_result){FOREVER_MENU_COMMAND_SET_FRAME_GENERATION,
            menu->values.frame_generation == FOREVER_FG_OFF ? FOREVER_FG_ON : FOREVER_FG_OFF};
    case FOREVER_MENU_HDR:
        return (struct forever_menu_result){FOREVER_MENU_COMMAND_SET_HDR, !menu->values.hdr};
    case FOREVER_MENU_HUD:
        return (struct forever_menu_result){FOREVER_MENU_COMMAND_SET_HUD, !menu->values.hud};
    case FOREVER_MENU_REINSTALL:
        menu->page = FOREVER_MENU_CONFIRM_REINSTALL;
        menu->confirmation_selected = 0;
        return none();
    case FOREVER_MENU_UNINSTALL:
        menu->page = FOREVER_MENU_CONFIRM_UNINSTALL;
        menu->confirmation_selected = 0;
        return none();
    case FOREVER_MENU_CLOSE:
        return (struct forever_menu_result){FOREVER_MENU_COMMAND_CLOSE, 0};
    case FOREVER_MENU_ADDONS:
        return none();
    }
    return none();
}

struct forever_menu_result forever_menu_back(struct forever_menu_state *menu)
{
    if (menu->page == FOREVER_MENU_CONFIRM_REINSTALL || menu->page == FOREVER_MENU_CONFIRM_UNINSTALL)
    {
        enum forever_menu_page page = menu->page;
        menu->page = FOREVER_MENU_MAIN;
        menu->selected = page == FOREVER_MENU_CONFIRM_REINSTALL ?
            FOREVER_MENU_REINSTALL : FOREVER_MENU_UNINSTALL;
        return none();
    }
    if (menu->page == FOREVER_MENU_MAIN || menu->page == FOREVER_MENU_NOTICE)
        return (struct forever_menu_result){FOREVER_MENU_COMMAND_CLOSE, 0};
    return none();
}

struct forever_menu_result forever_menu_click_line(struct forever_menu_state *menu, int line)
{
    if (menu->page == FOREVER_MENU_MAIN)
    {
        switch (line)
        {
        case FOREVER_MENU_LINE_FRAME_GENERATION: menu->selected = FOREVER_MENU_FRAME_GENERATION; break;
        case FOREVER_MENU_LINE_HDR: menu->selected = FOREVER_MENU_HDR; break;
        case FOREVER_MENU_LINE_HUD: menu->selected = FOREVER_MENU_HUD; break;
        case FOREVER_MENU_LINE_REINSTALL: menu->selected = FOREVER_MENU_REINSTALL; break;
        case FOREVER_MENU_LINE_UNINSTALL: menu->selected = FOREVER_MENU_UNINSTALL; break;
        case FOREVER_MENU_LINE_CLOSE: menu->selected = FOREVER_MENU_CLOSE; break;
        case FOREVER_MENU_LINE_ADDONS: return none();
        default: return none();
        }
        return forever_menu_select(menu);
    }
    if (menu->page == FOREVER_MENU_CONFIRM_REINSTALL || menu->page == FOREVER_MENU_CONFIRM_UNINSTALL)
    {
        if (line == FOREVER_MENU_LINE_CANCEL) menu->confirmation_selected = 0;
        else if (line == FOREVER_MENU_LINE_CONFIRM) menu->confirmation_selected = 1;
        else return none();
        return forever_menu_select(menu);
    }
    return none();
}

void forever_menu_commit(struct forever_menu_state *menu, enum forever_menu_command command, int enabled)
{
    switch (command)
    {
    case FOREVER_MENU_COMMAND_SET_FRAME_GENERATION:
        menu->values.frame_generation = enabled ? FOREVER_FG_ON : FOREVER_FG_OFF;
        break;
    case FOREVER_MENU_COMMAND_SET_HDR: menu->values.hdr = !!enabled; break;
    case FOREVER_MENU_COMMAND_SET_HUD: menu->values.hud = !!enabled; break;
    default: return;
    }
    menu->status[0] = 0;
}

void forever_menu_status(struct forever_menu_state *menu, const char *text)
{
    copy_status(menu, text);
}

void forever_menu_progress(struct forever_menu_state *menu, const char *text)
{
    menu->page = FOREVER_MENU_PROGRESS;
    copy_status(menu, text);
}

void forever_menu_notice(struct forever_menu_state *menu, const char *text)
{
    menu->page = FOREVER_MENU_NOTICE;
    copy_status(menu, text);
}

void forever_menu_close(struct forever_menu_state *menu)
{
    menu->page = FOREVER_MENU_CLOSED;
    menu->status[0] = 0;
}

static const char *mark(int selected)
{
    return selected ? ">" : " ";
}

static const char *frame_generation_name(enum forever_frame_generation value)
{
    if (value == FOREVER_FG_AUTO) return "Auto";
    return value == FOREVER_FG_ON ? "On" : "Off";
}

void forever_menu_render(const struct forever_menu_state *menu, char *text, size_t size)
{
    if (!size) return;
    text[0] = 0;
    if (menu->page == FOREVER_MENU_CLOSED) return;
    if (menu->page == FOREVER_MENU_MAIN)
    {
        snprintf(text, size,
                 " Forever Launcher\n"
                 " ---------------------------------------------\n"
                 "%s Frame generation\t%s\n"
                 "%s HDR\t%s\n"
                 "%s Performance overlay\t%s\n"
                 " ---------------------------------------------\n"
                 "  Add-ons…\tComing soon\n"
                 " ---------------------------------------------\n"
                 "%s Reinstall Battle.net...\n"
                 "%s Uninstall Battle.net...\n"
                 " ---------------------------------------------\n"
                 "%s Close\n"
                 "\n%s",
                 mark(menu->selected == FOREVER_MENU_FRAME_GENERATION),
                 frame_generation_name(menu->values.frame_generation),
                 mark(menu->selected == FOREVER_MENU_HDR), menu->values.hdr ? "On" : "Off",
                 mark(menu->selected == FOREVER_MENU_HUD), menu->values.hud ? "On" : "Off",
                 mark(menu->selected == FOREVER_MENU_REINSTALL),
                 mark(menu->selected == FOREVER_MENU_UNINSTALL),
                 mark(menu->selected == FOREVER_MENU_CLOSE), menu->status);
        return;
    }
    if (menu->page == FOREVER_MENU_CONFIRM_REINSTALL)
    {
        snprintf(text, size,
                 " Reinstall Battle.net?\n"
                 " ---------------------------------------------\n"
                 " Stops Battle.net and its update agent, removes\n"
                 " their files, and downloads a fresh installer.\n"
                 " Installed games and their settings stay.\n"
                 "\n"
                 "\n%s Cancel\n%s Reinstall Battle.net\n",
                 mark(!menu->confirmation_selected), mark(menu->confirmation_selected));
        return;
    }
    if (menu->page == FOREVER_MENU_CONFIRM_UNINSTALL)
    {
        snprintf(text, size,
                 " Uninstall Battle.net?\n"
                 " ---------------------------------------------\n"
                 " Removes Battle.net and its update agent.\n"
                 " Installed games and their settings stay.\n"
                 "\n"
                 "\n"
                 "\n%s Cancel\n%s Uninstall Battle.net\n",
                 mark(!menu->confirmation_selected), mark(menu->confirmation_selected));
        return;
    }
    snprintf(text, size, " Forever Launcher\n ---------------------------------------------\n\n %s",
             menu->status);
}
