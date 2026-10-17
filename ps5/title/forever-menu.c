#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "forever-menu.h"

#define ADDON_VISIBLE_ROWS 5

static const enum forever_menu_item selectable[] = {
    FOREVER_MENU_FRAME_GENERATION,
    FOREVER_MENU_HDR,
    FOREVER_MENU_HUD,
    FOREVER_MENU_ADDONS,
    FOREVER_MENU_REINSTALL,
    FOREVER_MENU_UNINSTALL,
    FOREVER_MENU_CLOSE,
};

static struct forever_menu_result none(void)
{
    return (struct forever_menu_result){.command = FOREVER_MENU_COMMAND_NONE};
}

static struct forever_menu_result addon_result(
    const struct forever_menu_state *menu, enum forever_menu_command command, const char *key)
{
    struct forever_menu_result result = {
        .command = command, .channel = menu->addon_channel, .tab = menu->addon_tab};
    if (key) snprintf(result.key, sizeof(result.key), "%s", key);
    return result;
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
    menu->addon_confirmation = FOREVER_MENU_COMMAND_NONE;
    if (values) menu->values = *values;
    menu->status[0] = 0;
}

int forever_menu_is_open(const struct forever_menu_state *menu)
{
    return menu->page != FOREVER_MENU_CLOSED;
}

static void addon_selection_visible(struct forever_menu_state *menu)
{
    if (menu->addon_selected < menu->addon_scroll) menu->addon_scroll = menu->addon_selected;
    if (menu->addon_selected >= menu->addon_scroll + ADDON_VISIBLE_ROWS)
        menu->addon_scroll = menu->addon_selected - ADDON_VISIBLE_ROWS + 1;
}

void forever_menu_move(struct forever_menu_state *menu, int direction)
{
    if (!direction || menu->page == FOREVER_MENU_CLOSED || menu->page == FOREVER_MENU_PROGRESS ||
        menu->page == FOREVER_MENU_NOTICE) return;
    if (menu->page == FOREVER_MENU_CONFIRM_REINSTALL ||
        menu->page == FOREVER_MENU_CONFIRM_UNINSTALL ||
        menu->page == FOREVER_MENU_ADDON_CONFIRM)
    {
        menu->confirmation_selected = !menu->confirmation_selected;
        return;
    }
    if (menu->page == FOREVER_MENU_ADDON_PAGE)
    {
        if (!menu->addon_count) return;
        if (direction > 0)
            menu->addon_selected = (menu->addon_selected + 1) % menu->addon_count;
        else
            menu->addon_selected =
                (menu->addon_selected + menu->addon_count - 1) % menu->addon_count;
        addon_selection_visible(menu);
        menu->status[0] = 0;
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

static const struct forever_addon_row *selected_addon(const struct forever_menu_state *menu)
{
    return menu->addon_selected < menu->addon_count ?
        &menu->addon_rows[menu->addon_selected] : NULL;
}

static void sort_addons(struct forever_menu_state *menu)
{
    for (size_t i = 1; i < menu->addon_count; i++)
    {
        struct forever_addon_row row = menu->addon_rows[i];
        size_t position = i;
        while (position)
        {
            const struct forever_addon_row *previous = &menu->addon_rows[position - 1];
            int before = menu->addon_sort_downloads ?
                (row.downloads > previous->downloads ||
                 (row.downloads == previous->downloads &&
                  strcmp(row.name, previous->name) < 0)) :
                strcmp(row.name, previous->name) < 0;
            if (!before) break;
            menu->addon_rows[position] = *previous;
            position--;
        }
        menu->addon_rows[position] = row;
    }
}

static void addon_working(struct forever_menu_state *menu, const char *status)
{
    menu->addon_loading = 1;
    copy_status(menu, status);
}

struct forever_menu_result forever_menu_select(struct forever_menu_state *menu)
{
    if (menu->page == FOREVER_MENU_ADDON_CONFIRM)
    {
        enum forever_menu_command command = menu->addon_confirmation;
        const struct forever_addon_row *row = selected_addon(menu);
        menu->page = FOREVER_MENU_ADDON_PAGE;
        menu->addon_confirmation = FOREVER_MENU_COMMAND_NONE;
        if (!menu->confirmation_selected || !row) return none();
        addon_working(menu, command == FOREVER_MENU_COMMAND_ADDONS_REMOVE ?
                     "Removing add-on..." : "Installing starter pack...");
        return addon_result(menu, command, row->key);
    }
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
            .command = page == FOREVER_MENU_CONFIRM_REINSTALL ? FOREVER_MENU_COMMAND_REINSTALL :
                                                                    FOREVER_MENU_COMMAND_UNINSTALL};
    }
    if (menu->page == FOREVER_MENU_ADDON_PAGE)
    {
        const struct forever_addon_row *row = selected_addon(menu);
        if (!row || menu->addon_loading) return none();
        if (menu->addon_tab == FOREVER_ADDON_FIND)
        {
            if (row->curse_only)
            {
                copy_status(menu, "Available on CurseForge only. There is no install action.");
                return none();
            }
            if (row->installable)
            {
                addon_working(menu, "Installing add-on...");
                return addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_INSTALL, row->key);
            }
            copy_status(menu, row->status[0] ? row->status :
                        "This catalogue entry cannot be installed here.");
            return none();
        }
        else if (menu->addon_tab == FOREVER_ADDON_STARTER_PACKS && row->installable)
        {
            menu->page = FOREVER_MENU_ADDON_CONFIRM;
            menu->confirmation_selected = 0;
            menu->addon_confirmation = FOREVER_MENU_COMMAND_ADDONS_INSTALL_PACK;
        }
        else
            copy_status(menu, "Details for the selected add-on are shown below.");
        return none();
    }
    if (menu->page != FOREVER_MENU_MAIN) return none();
    switch (menu->selected)
    {
    case FOREVER_MENU_FRAME_GENERATION:
        return (struct forever_menu_result){
            .command = FOREVER_MENU_COMMAND_SET_FRAME_GENERATION,
            .enabled = menu->values.frame_generation == FOREVER_FG_OFF ? FOREVER_FG_ON :
                                                                          FOREVER_FG_OFF};
    case FOREVER_MENU_HDR:
        return (struct forever_menu_result){
            .command = FOREVER_MENU_COMMAND_SET_HDR, .enabled = !menu->values.hdr};
    case FOREVER_MENU_HUD:
        return (struct forever_menu_result){
            .command = FOREVER_MENU_COMMAND_SET_HUD, .enabled = !menu->values.hud};
    case FOREVER_MENU_ADDONS:
        menu->page = FOREVER_MENU_ADDON_PAGE;
        menu->addon_channel = FOREVER_ADDON_CLASSIC_BETA;
        menu->addon_tab = FOREVER_ADDON_INSTALLED;
        menu->addon_sort_downloads = 0;
        menu->addon_selected = menu->addon_scroll = menu->addon_count = 0;
        addon_working(menu, "Starting the add-on service...");
        return addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_OPEN, NULL);
    case FOREVER_MENU_REINSTALL:
        menu->page = FOREVER_MENU_CONFIRM_REINSTALL;
        menu->confirmation_selected = 0;
        return none();
    case FOREVER_MENU_UNINSTALL:
        menu->page = FOREVER_MENU_CONFIRM_UNINSTALL;
        menu->confirmation_selected = 0;
        return none();
    case FOREVER_MENU_CLOSE:
        return (struct forever_menu_result){.command = FOREVER_MENU_COMMAND_CLOSE};
    }
    return none();
}

struct forever_menu_result forever_menu_addon_button(
    struct forever_menu_state *menu, enum forever_addon_button button)
{
    const struct forever_addon_row *row;
    if (menu->page != FOREVER_MENU_ADDON_PAGE || menu->addon_loading) return none();
    row = selected_addon(menu);
    switch (button)
    {
    case FOREVER_ADDON_LEFT:
    case FOREVER_ADDON_RIGHT:
        menu->addon_tab = (enum forever_addon_tab)(
            (menu->addon_tab + (button == FOREVER_ADDON_RIGHT ? 1 : FOREVER_ADDON_TAB_COUNT - 1)) %
            FOREVER_ADDON_TAB_COUNT);
        menu->addon_selected = menu->addon_scroll = menu->addon_count = 0;
        addon_working(menu, "Loading add-ons...");
        return addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_REFRESH, NULL);
    case FOREVER_ADDON_L1:
    case FOREVER_ADDON_R1:
    {
        enum forever_addon_channel channel =
            button == FOREVER_ADDON_R1 ? FOREVER_ADDON_FINAL : FOREVER_ADDON_CLASSIC_BETA;
        if (menu->addon_channel == channel) return none();
        menu->addon_channel = channel;
        menu->addon_selected = menu->addon_scroll = menu->addon_count = 0;
        addon_working(menu, "Switching WoW channel...");
        return addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_REFRESH, NULL);
    }
    case FOREVER_ADDON_SQUARE:
        if (menu->addon_tab == FOREVER_ADDON_FIND)
        {
            menu->addon_sort_downloads = !menu->addon_sort_downloads;
            sort_addons(menu);
            menu->addon_selected = menu->addon_scroll = 0;
            copy_status(menu, menu->addon_sort_downloads ?
                        "Catalogue sorted by downloads." : "Catalogue sorted by name.");
            return none();
        }
        if (row && (menu->addon_tab == FOREVER_ADDON_INSTALLED ||
                    menu->addon_tab == FOREVER_ADDON_UPDATES) &&
            row->update_available)
        {
            addon_working(menu, "Updating add-on...");
            return addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_UPDATE, row->key);
        }
        copy_status(menu, "No update is available for the selected add-on.");
        return none();
    case FOREVER_ADDON_TRIANGLE:
        if (menu->addon_tab == FOREVER_ADDON_INSTALLED ||
            menu->addon_tab == FOREVER_ADDON_UPDATES)
        {
            addon_working(menu, "Updating all add-ons...");
            return addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_UPDATE_ALL, NULL);
        }
        return none();
    case FOREVER_ADDON_R3:
        if (row && (menu->addon_tab == FOREVER_ADDON_INSTALLED ||
                    menu->addon_tab == FOREVER_ADDON_UPDATES))
        {
            menu->page = FOREVER_MENU_ADDON_CONFIRM;
            menu->confirmation_selected = 0;
            menu->addon_confirmation = FOREVER_MENU_COMMAND_ADDONS_REMOVE;
        }
        return none();
    }
    return none();
}

void forever_menu_addon_snapshot(
    struct forever_menu_state *menu, const struct forever_addon_snapshot *snapshot)
{
    char selected_key[FOREVER_ADDON_KEY_BYTES] = {0};
    const struct forever_addon_row *selected;
    if (!snapshot || (menu->page != FOREVER_MENU_ADDON_PAGE &&
                      menu->page != FOREVER_MENU_ADDON_CONFIRM) ||
        snapshot->tab != menu->addon_tab) return;
    selected = selected_addon(menu);
    if (selected) snprintf(selected_key, sizeof(selected_key), "%s", selected->key);
    menu->addon_count = snapshot->count < FOREVER_ADDON_ROWS ?
        snapshot->count : FOREVER_ADDON_ROWS;
    memcpy(menu->addon_rows, snapshot->rows,
           menu->addon_count * sizeof(menu->addon_rows[0]));
    if (menu->addon_tab == FOREVER_ADDON_FIND && menu->addon_sort_downloads)
        sort_addons(menu);
    menu->addon_selected = menu->addon_scroll = 0;
    if (selected_key[0])
        for (size_t i = 0; i < menu->addon_count; i++)
            if (!strcmp(menu->addon_rows[i].key, selected_key))
            {
                menu->addon_selected = i;
                menu->addon_scroll = i >= ADDON_VISIBLE_ROWS ?
                    i - ADDON_VISIBLE_ROWS + 1 : 0;
                break;
            }
    menu->addon_loading = 0;
    copy_status(menu, menu->addon_count ? "" : "No add-ons found for this view.");
}

void forever_menu_addon_error(struct forever_menu_state *menu, const char *text)
{
    if (menu->page != FOREVER_MENU_ADDON_PAGE &&
        menu->page != FOREVER_MENU_ADDON_CONFIRM) return;
    menu->addon_loading = 0;
    copy_status(menu, text && *text ? text : "The add-on operation failed.");
}

struct forever_menu_result forever_menu_back(struct forever_menu_state *menu)
{
    if (menu->page == FOREVER_MENU_ADDON_CONFIRM)
    {
        menu->page = FOREVER_MENU_ADDON_PAGE;
        menu->addon_confirmation = FOREVER_MENU_COMMAND_NONE;
        menu->confirmation_selected = 0;
        return none();
    }
    if (menu->page == FOREVER_MENU_ADDON_PAGE)
    {
        menu->page = FOREVER_MENU_MAIN;
        menu->selected = FOREVER_MENU_ADDONS;
        menu->addon_loading = 0;
        copy_status(menu, "Finishing add-on changes...");
        return addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_CLOSE, NULL);
    }
    if (menu->page == FOREVER_MENU_CONFIRM_REINSTALL || menu->page == FOREVER_MENU_CONFIRM_UNINSTALL)
    {
        enum forever_menu_page page = menu->page;
        menu->page = FOREVER_MENU_MAIN;
        menu->selected = page == FOREVER_MENU_CONFIRM_REINSTALL ?
            FOREVER_MENU_REINSTALL : FOREVER_MENU_UNINSTALL;
        return none();
    }
    if (menu->page == FOREVER_MENU_MAIN || menu->page == FOREVER_MENU_NOTICE)
        return (struct forever_menu_result){.command = FOREVER_MENU_COMMAND_CLOSE};
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
        case FOREVER_MENU_LINE_ADDONS: menu->selected = FOREVER_MENU_ADDONS; break;
        case FOREVER_MENU_LINE_REINSTALL: menu->selected = FOREVER_MENU_REINSTALL; break;
        case FOREVER_MENU_LINE_UNINSTALL: menu->selected = FOREVER_MENU_UNINSTALL; break;
        case FOREVER_MENU_LINE_CLOSE: menu->selected = FOREVER_MENU_CLOSE; break;
        default: return none();
        }
        return forever_menu_select(menu);
    }
    if (menu->page == FOREVER_MENU_ADDON_PAGE)
    {
        if (line == FOREVER_MENU_LINE_ADDON_TABS)
            return forever_menu_addon_button(menu, FOREVER_ADDON_RIGHT);
        if (line >= FOREVER_MENU_LINE_ADDON_FIRST && line <= FOREVER_MENU_LINE_ADDON_LAST)
        {
            size_t selected = menu->addon_scroll +
                (size_t)(line - FOREVER_MENU_LINE_ADDON_FIRST);
            if (selected >= menu->addon_count) return none();
            menu->addon_selected = selected;
            menu->status[0] = 0;
            return forever_menu_select(menu);
        }
        return none();
    }
    if (menu->page == FOREVER_MENU_CONFIRM_REINSTALL ||
        menu->page == FOREVER_MENU_CONFIRM_UNINSTALL ||
        menu->page == FOREVER_MENU_ADDON_CONFIRM)
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

struct text_writer
{
    char *text;
    size_t size, used;
};

static void append(struct text_writer *writer, const char *format, ...)
{
    int length;
    va_list arguments;
    if (writer->used >= writer->size) return;
    va_start(arguments, format);
    length = vsnprintf(writer->text + writer->used, writer->size - writer->used,
                       format, arguments);
    va_end(arguments);
    if (length < 0) return;
    if ((size_t)length >= writer->size - writer->used)
        writer->used = writer->size - 1;
    else
        writer->used += (size_t)length;
}

static const char *addon_tab_name(enum forever_addon_tab tab)
{
    static const char *const names[] = {"Installed", "Updates", "Find", "Starter packs"};
    return (unsigned)tab < FOREVER_ADDON_TAB_COUNT ? names[tab] : "Installed";
}

static void render_addons(const struct forever_menu_state *menu, char *text, size_t size)
{
    struct text_writer writer = {text, size, 0};
    const struct forever_addon_row *selected = selected_addon(menu);
    const char *channel = menu->addon_channel == FOREVER_ADDON_FINAL ?
        "WoW: Forever (final)" : "WoW: Forever (beta)";
    append(&writer, " Add-ons \xe2\x80\x94 %s\n", channel);
    append(&writer, " ---------------------------------------------\n ");
    for (int tab = 0; tab < FOREVER_ADDON_TAB_COUNT; tab++)
        append(&writer, "%s%s%s%s", tab ? "   " : "",
               (unsigned)menu->addon_tab == (unsigned)tab ? "[" : "",
               addon_tab_name((enum forever_addon_tab)tab),
               (unsigned)menu->addon_tab == (unsigned)tab ? "]" : "");
    append(&writer, "\n ---------------------------------------------\n");
    for (size_t visible = 0; visible < ADDON_VISIBLE_ROWS; visible++)
    {
        size_t index = menu->addon_scroll + visible;
        if (index < menu->addon_count)
        {
            const struct forever_addon_row *row = &menu->addon_rows[index];
            char downloads[48];
            const char *status = row->curse_only ? "CurseForge only" : row->status;
            if (menu->addon_tab == FOREVER_ADDON_FIND && row->installable &&
                !row->curse_only && row->downloads)
            {
                snprintf(downloads, sizeof(downloads), "%llu downloads", row->downloads);
                status = downloads;
            }
            append(&writer, "%s %s\t%s\n", mark(index == menu->addon_selected),
                   row->name, status);
        }
        else
            append(&writer, " \n");
    }
    if (selected)
    {
        append(&writer, " ---------------------------------------------\n");
        append(&writer, " %s\n", selected->description[0] ?
               selected->description : "No description is available.");
        append(&writer, " Source\t%s\n", selected->source[0] ? selected->source : "Unknown");
        append(&writer, " Last update\t%s\n",
               selected->updated[0] ? selected->updated : "Unknown");
    }
    append(&writer, " \n %s", menu->status);
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
                 "%s Add-ons...\tManage\n"
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
                 mark(menu->selected == FOREVER_MENU_ADDONS),
                 mark(menu->selected == FOREVER_MENU_REINSTALL),
                 mark(menu->selected == FOREVER_MENU_UNINSTALL),
                 mark(menu->selected == FOREVER_MENU_CLOSE), menu->status);
        return;
    }
    if (menu->page == FOREVER_MENU_ADDON_PAGE)
    {
        render_addons(menu, text, size);
        return;
    }
    if (menu->page == FOREVER_MENU_ADDON_CONFIRM)
    {
        const struct forever_addon_row *row = selected_addon(menu);
        int removing = menu->addon_confirmation == FOREVER_MENU_COMMAND_ADDONS_REMOVE;
        snprintf(text, size,
                 " %s?\n"
                 " ---------------------------------------------\n"
                 " %s\n"
                 " Changes load the next time WoW starts, or type\n"
                 " /reload in WoW.\n"
                 "\n"
                 "\n"
                 "%s Cancel\n"
                 "%s %s\n",
                 removing ? "Remove add-on" : "Install starter pack",
                 row ? row->name : "Selected add-on",
                 mark(!menu->confirmation_selected),
                 mark(menu->confirmation_selected),
                 removing ? "Remove" : "Install");
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
