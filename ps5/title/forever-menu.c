#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "forever-menu.h"

#define ADDON_VISIBLE_ROWS 5

static const enum forever_menu_item selectable[] = {
    FOREVER_MENU_FRAME_GENERATION,
    FOREVER_MENU_HDR,
    FOREVER_MENU_HUD,
    FOREVER_MENU_VOICE,
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
    menu->voice_downloading = 0;
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
    if (menu->page == FOREVER_MENU_WAGO_KEY)
    {
        menu->wago_choice = (menu->wago_choice + (direction > 0 ? 1 : 2)) % 3;
        return;
    }
    if (menu->page == FOREVER_MENU_CONFIRM_REINSTALL ||
        menu->page == FOREVER_MENU_CONFIRM_UNINSTALL ||
        menu->page == FOREVER_MENU_ADDON_CONFIRM)
    {
        menu->confirmation_selected = !menu->confirmation_selected;
        return;
    }
    if (menu->page == FOREVER_MENU_ADDON_PAGE)
    {
        size_t count = menu->addon_count + 1; /* last selection is the fixed Wago row */
        menu->addon_selected = (menu->addon_selected + (direction > 0 ? 1 : count - 1)) % count;
        if (menu->addon_selected < menu->addon_count) addon_selection_visible(menu);
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

static int ascii_equal(unsigned char left, unsigned char right)
{
    if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + ('a' - 'A'));
    if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + ('a' - 'A'));
    return left == right;
}

static int ascii_contains(const char *text, const char *query)
{
    size_t text_length = strlen(text), query_length = strlen(query);
    if (!query_length) return 1;
    if (query_length > text_length) return 0;
    for (size_t start = 0; start + query_length <= text_length; start++)
    {
        size_t at = 0;
        while (at < query_length &&
               ascii_equal((unsigned char)text[start + at], (unsigned char)query[at])) at++;
        if (at == query_length) return 1;
    }
    return 0;
}

static void sort_rows(struct forever_addon_row *rows, size_t count, int downloads)
{
    for (size_t i = 1; i < count; i++)
    {
        struct forever_addon_row row = rows[i];
        size_t position = i;
        while (position)
        {
            const struct forever_addon_row *previous = &rows[position - 1];
            int before = downloads ?
                (row.downloads > previous->downloads ||
                 (row.downloads == previous->downloads &&
                  strcmp(row.name, previous->name) < 0)) :
                strcmp(row.name, previous->name) < 0;
            if (!before) break;
            rows[position] = *previous;
            position--;
        }
        rows[position] = row;
    }
}

static int find_special(const struct forever_addon_row *row, const char *key)
{
    return row && !strcmp(row->key, key);
}

static void rebuild_find(struct forever_menu_state *menu, int reset_selection)
{
    char selected_key[FOREVER_ADDON_KEY_BYTES] = {0};
    size_t count = 0;
    if (!reset_selection)
    {
        const struct forever_addon_row *selected = selected_addon(menu);
        if (selected) snprintf(selected_key, sizeof(selected_key), "%s", selected->key);
    }
    menu->addon_rows[count] = (struct forever_addon_row){0};
    snprintf(menu->addon_rows[count].key, sizeof(menu->addon_rows[count].key), "menu:search");
    snprintf(menu->addon_rows[count].name, sizeof(menu->addon_rows[count].name),
             "Search\xe2\x80\xa6");
    snprintf(menu->addon_rows[count].status, sizeof(menu->addon_rows[count].status),
             "%s", menu->addon_query[0] ? "Edit query" : "Type or press Cross");
    count++;
    menu->addon_match_count = 0;
    for (size_t i = 0; i < menu->addon_catalog_count; i++)
    {
        const struct forever_addon_row *row = &menu->addon_catalog_rows[i];
        const char *name = row->filter_name[0] ? row->filter_name : row->name;
        const char *description = row->filter_description[0] ?
            row->filter_description : row->description;
        if (!ascii_contains(name, menu->addon_query) &&
            !ascii_contains(description, menu->addon_query)) continue;
        if (count >= FOREVER_ADDON_VIEW_ROWS - 1) break;
        menu->addon_rows[count++] = *row;
        menu->addon_match_count++;
    }
    menu->addon_rows[count] = (struct forever_addon_row){0};
    snprintf(menu->addon_rows[count].key, sizeof(menu->addon_rows[count].key), "menu:github");
    snprintf(menu->addon_rows[count].name, sizeof(menu->addon_rows[count].name),
             "Add from GitHub link\xe2\x80\xa6");
    snprintf(menu->addon_rows[count].status, sizeof(menu->addon_rows[count].status),
             "owner/repo or github.com link");
    menu->addon_count = ++count;
    menu->addon_selected = 0;
    if (reset_selection && menu->addon_query[0] && menu->addon_match_count)
        menu->addon_selected = 1;
    else if (!reset_selection && selected_key[0])
        for (size_t i = 0; i < menu->addon_count; i++)
            if (!strcmp(menu->addon_rows[i].key, selected_key))
            {
                menu->addon_selected = i;
                break;
            }
    menu->addon_scroll = menu->addon_selected >= ADDON_VISIBLE_ROWS ?
        menu->addon_selected - ADDON_VISIBLE_ROWS + 1 : 0;
}

static void addon_working(struct forever_menu_state *menu, const char *status)
{
    menu->addon_loading = 1;
    copy_status(menu, status);
}

struct forever_menu_result forever_menu_select(struct forever_menu_state *menu)
{
    if (menu->page == FOREVER_MENU_WAGO_KEY)
    {
        static const enum forever_menu_command commands[] = {
            FOREVER_MENU_COMMAND_WAGO_IME, FOREVER_MENU_COMMAND_WAGO_FILE,
            FOREVER_MENU_COMMAND_WAGO_REMOVE};
        menu->page = FOREVER_MENU_ADDON_PAGE;
        return addon_result(menu, commands[menu->wago_choice], NULL);
    }
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
        if (menu->addon_selected == menu->addon_count && !menu->addon_loading)
        {
            menu->page = FOREVER_MENU_WAGO_KEY;
            menu->wago_choice = 0;
            menu->status[0] = 0;
            return none();
        }
        const struct forever_addon_row *row = selected_addon(menu);
        if (!row || menu->addon_loading) return none();
        if (menu->addon_tab == FOREVER_ADDON_FIND)
        {
            if (find_special(row, "menu:search"))
                return addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_SEARCH_IME, NULL);
            if (find_special(row, "menu:github"))
                return addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_GITHUB_IME, NULL);
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
    if (menu->voice_downloading && menu->selected != FOREVER_MENU_CLOSE) return none();
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
    case FOREVER_MENU_VOICE:
        if (menu->voice_downloading) return none();
        return (struct forever_menu_result){
            .command = FOREVER_MENU_COMMAND_SET_VOICE, .enabled = !menu->values.voice};
    case FOREVER_MENU_ADDONS:
        menu->page = FOREVER_MENU_ADDON_PAGE;
        menu->addon_channel = 0;
        menu->addon_channel_count = 0;
        menu->addon_tab = FOREVER_ADDON_INSTALLED;
        menu->addon_sort_downloads = 0;
        menu->addon_selected = menu->addon_scroll = menu->addon_count = 0;
        menu->addon_catalog_count = menu->addon_match_count = 0;
        menu->addon_query[0] = 0;
        menu->addon_pending_github = 0;
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
        size_t count = menu->addon_channel_count;
        if (count < 2) return none();
        menu->addon_channel = (menu->addon_channel +
            (button == FOREVER_ADDON_R1 ? 1 : count - 1)) % count;
        menu->addon_selected = menu->addon_scroll = menu->addon_count = 0;
        addon_working(menu, "Switching WoW channel...");
        return addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_REFRESH, NULL);
    }
    case FOREVER_ADDON_SQUARE:
        if (menu->addon_tab == FOREVER_ADDON_FIND)
        {
            menu->addon_sort_downloads = !menu->addon_sort_downloads;
            sort_rows(menu->addon_catalog_rows, menu->addon_catalog_count,
                      menu->addon_sort_downloads);
            rebuild_find(menu, 0);
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
    int successful_action;
    if (!snapshot || (menu->page != FOREVER_MENU_ADDON_PAGE &&
                      menu->page != FOREVER_MENU_ADDON_CONFIRM) ||
        snapshot->tab != menu->addon_tab) return;
    successful_action = snapshot->wago_saved || menu->addon_pending_github;
    selected = selected_addon(menu);
    menu->wago_status = snapshot->wago_status;
    if (snapshot->channel_count)
    {
        menu->addon_channel_count = snapshot->channel_count < FOREVER_ADDON_CHANNELS ?
            snapshot->channel_count : FOREVER_ADDON_CHANNELS;
        memcpy(menu->addon_channels, snapshot->channels,
               menu->addon_channel_count * sizeof(menu->addon_channels[0]));
    }
    if (selected) snprintf(selected_key, sizeof(selected_key), "%s", selected->key);
    if (menu->addon_tab == FOREVER_ADDON_FIND)
    {
        menu->addon_catalog_count = snapshot->count < FOREVER_ADDON_ROWS ?
            snapshot->count : FOREVER_ADDON_ROWS;
        memcpy(menu->addon_catalog_rows, snapshot->rows,
               menu->addon_catalog_count * sizeof(menu->addon_catalog_rows[0]));
        sort_rows(menu->addon_catalog_rows, menu->addon_catalog_count,
                  menu->addon_sort_downloads);
        rebuild_find(menu, 0);
    }
    else
    {
        menu->addon_count = snapshot->count < FOREVER_ADDON_VIEW_ROWS ?
            snapshot->count : FOREVER_ADDON_VIEW_ROWS;
        memcpy(menu->addon_rows, snapshot->rows,
               menu->addon_count * sizeof(menu->addon_rows[0]));
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
    }
    menu->addon_loading = 0;
    if (snapshot->wago_saved)
        copy_status(menu, snapshot->wago_saved == 2 ? "Wago key removed." : "Wago key saved.");
    else if (menu->addon_pending_github)
    {
        menu->addon_pending_github = 0;
        copy_status(menu, "Installed from GitHub.");
    }
    else if (menu->addon_tab != FOREVER_ADDON_FIND)
        copy_status(menu, menu->addon_count ? "" : "No add-ons found for this view.");
    else
        menu->status[0] = 0;
    if (snapshot->notice[0] && !successful_action) copy_status(menu, snapshot->notice);
    if (snapshot->message[0])
    {
        copy_status(menu, snapshot->message);
        if (!strcmp(snapshot->message, "Wago rejected this key."))
            menu->wago_status = FOREVER_WAGO_REJECTED;
    }
}

void forever_menu_addon_error(struct forever_menu_state *menu, const char *text)
{
    if (menu->page != FOREVER_MENU_ADDON_PAGE &&
        menu->page != FOREVER_MENU_ADDON_CONFIRM) return;
    menu->addon_loading = 0;
    if (text && !strcmp(text, "Wago rejected this key."))
        menu->wago_status = FOREVER_WAGO_REJECTED;
    copy_status(menu, text && *text ? text : "The add-on operation failed.");
    menu->addon_pending_github = 0;
}

int forever_menu_addon_pause(struct forever_menu_state *menu, const char *text)
{
    if (menu->page != FOREVER_MENU_ADDON_PAGE &&
        menu->page != FOREVER_MENU_ADDON_CONFIRM &&
        menu->page != FOREVER_MENU_WAGO_KEY) return 0;
    menu->addon_loading = menu->addon_pending_github = 0;
    forever_menu_notice(menu, text);
    return 1;
}

static size_t utf8_size(const unsigned char *text, size_t available)
{
    if (!available) return 0;
    if (text[0] < 0x80) return 1;
    if ((text[0] & 0xe0) == 0xc0 && available >= 2 &&
        (text[1] & 0xc0) == 0x80) return 2;
    if ((text[0] & 0xf0) == 0xe0 && available >= 3 &&
        (text[1] & 0xc0) == 0x80 && (text[2] & 0xc0) == 0x80) return 3;
    if ((text[0] & 0xf8) == 0xf0 && available >= 4 &&
        (text[1] & 0xc0) == 0x80 && (text[2] & 0xc0) == 0x80 &&
        (text[3] & 0xc0) == 0x80) return 4;
    return 0;
}

static void normalize_query(const char *input, char output[FOREVER_ADDON_QUERY_BYTES])
{
    const unsigned char *text = (const unsigned char *)(input ? input : "");
    size_t input_length = strlen((const char *)text), source = 0, used = 0;
    while (source < input_length && used < FOREVER_ADDON_QUERY_BYTES - 1)
    {
        size_t bytes = utf8_size(text + source, input_length - source);
        if (!bytes) { source++; continue; }
        if (text[source] < 0x20 || text[source] == 0x7f) { source += bytes; continue; }
        if (used + bytes > FOREVER_ADDON_QUERY_BYTES - 1) break;
        memcpy(output + used, text + source, bytes);
        used += bytes;
        source += bytes;
    }
    output[used] = 0;
}

int forever_menu_find_set_query(struct forever_menu_state *menu, const char *query)
{
    char normalized[FOREVER_ADDON_QUERY_BYTES];
    if (!menu || menu->page != FOREVER_MENU_ADDON_PAGE ||
        menu->addon_tab != FOREVER_ADDON_FIND) return 0;
    normalize_query(query, normalized);
    if (!strcmp(menu->addon_query, normalized)) return 0;
    memcpy(menu->addon_query, normalized, sizeof(menu->addon_query));
    rebuild_find(menu, 1);
    menu->status[0] = 0;
    return 1;
}

int forever_menu_find_append(struct forever_menu_state *menu, const char *text)
{
    char combined[FOREVER_ADDON_QUERY_BYTES * 2];
    if (!menu || !text) return 0;
    snprintf(combined, sizeof(combined), "%s%s", menu->addon_query, text);
    return forever_menu_find_set_query(menu, combined);
}

int forever_menu_find_backspace(struct forever_menu_state *menu)
{
    char query[FOREVER_ADDON_QUERY_BYTES];
    size_t length;
    if (!menu || !menu->addon_query[0]) return 0;
    memcpy(query, menu->addon_query, sizeof(query));
    length = strlen(query);
    do length--; while (length && ((unsigned char)query[length] & 0xc0) == 0x80);
    query[length] = 0;
    return forever_menu_find_set_query(menu, query);
}

/* The keyboard already filtered live as it typed (main.c); a cancel keeps that
 * query, and a confirm applies the final text. */
void forever_menu_find_ime(struct forever_menu_state *menu, const char *text, int accepted)
{
    if (accepted) forever_menu_find_set_query(menu, text);
}

static int ascii_prefix(const char *text, const char *prefix)
{
    while (*prefix)
    {
        if (!*text || !ascii_equal((unsigned char)*text, (unsigned char)*prefix)) return 0;
        text++;
        prefix++;
    }
    return 1;
}

static int repo_character(unsigned char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') ||
           character == '.' || character == '_' || character == '-';
}

static int github_input_error(char *error, size_t error_size)
{
    if (error && error_size)
        snprintf(error, error_size,
                 "Enter owner/repo or an https://github.com/owner/repo link.");
    return -1;
}

int forever_menu_github_repo(const char *input, char *repo, size_t repo_size,
                             char *error, size_t error_size)
{
    static const char github[] = "https://github.com/";
    static const char github_www[] = "https://www.github.com/";
    const char *path, *owner, *repository, *after;
    size_t owner_length, repository_length;
    int url = 0;
    if (!input || !repo || !repo_size || !*input)
        return github_input_error(error, error_size);
    if (ascii_prefix(input, github_www))
    {
        path = input + sizeof(github_www) - 1;
        url = 1;
    }
    else if (ascii_prefix(input, github))
    {
        path = input + sizeof(github) - 1;
        url = 1;
    }
    else
    {
        if (strstr(input, "://")) return github_input_error(error, error_size);
        path = input;
    }
    owner = path;
    after = strchr(owner, '/');
    if (!after) return github_input_error(error, error_size);
    owner_length = (size_t)(after - owner);
    repository = after + 1;
    after = strchr(repository, '/');
    repository_length = after ? (size_t)(after - repository) : strlen(repository);
    if (repository_length >= 4 &&
        repository[repository_length - 4] == '.' &&
        ascii_equal((unsigned char)repository[repository_length - 3], 'g') &&
        ascii_equal((unsigned char)repository[repository_length - 2], 'i') &&
        ascii_equal((unsigned char)repository[repository_length - 1], 't'))
        repository_length -= 4;
    if (!owner_length || owner_length > 100 ||
        !repository_length || repository_length > 100)
        return github_input_error(error, error_size);
    for (size_t i = 0; i < owner_length; i++)
        if (!repo_character((unsigned char)owner[i]))
            return github_input_error(error, error_size);
    for (size_t i = 0; i < repository_length; i++)
        if (!repo_character((unsigned char)repository[i]))
            return github_input_error(error, error_size);
    if (after)
    {
        if (!url && after[1]) return github_input_error(error, error_size);
        for (const char *extra = after + 1; url && *extra; extra++)
            if (!repo_character((unsigned char)*extra) && *extra != '/')
                return github_input_error(error, error_size);
    }
    if (owner_length + 1 + repository_length >= repo_size)
        return github_input_error(error, error_size);
    memcpy(repo, owner, owner_length);
    repo[owner_length] = '/';
    memcpy(repo + owner_length + 1, repository, repository_length);
    repo[owner_length + 1 + repository_length] = 0;
    if (error && error_size) error[0] = 0;
    return 0;
}

struct forever_menu_result forever_menu_github_ime(
    struct forever_menu_state *menu, const char *text, int accepted)
{
    char repo[FOREVER_ADDON_KEY_BYTES], error[FOREVER_MENU_STATUS_BYTES];
    struct forever_menu_result result;
    if (!accepted || !menu || menu->page != FOREVER_MENU_ADDON_PAGE ||
        menu->addon_tab != FOREVER_ADDON_FIND) return none();
    if (forever_menu_github_repo(text, repo, sizeof(repo), error, sizeof(error)))
    {
        copy_status(menu, error);
        return none();
    }
    menu->addon_tab = FOREVER_ADDON_INSTALLED;
    menu->addon_selected = menu->addon_scroll = menu->addon_count = 0;
    menu->addon_pending_github = 1;
    addon_working(menu, "Installing from GitHub...");
    result = addon_result(menu, FOREVER_MENU_COMMAND_ADDONS_INSTALL_GITHUB, repo);
    return result;
}

struct forever_menu_result forever_menu_back(struct forever_menu_state *menu)
{
    if (menu->page == FOREVER_MENU_WAGO_KEY)
    {
        menu->page = FOREVER_MENU_ADDON_PAGE;
        return none();
    }
    if (menu->page == FOREVER_MENU_ADDON_CONFIRM)
    {
        menu->page = FOREVER_MENU_ADDON_PAGE;
        menu->addon_confirmation = FOREVER_MENU_COMMAND_NONE;
        menu->confirmation_selected = 0;
        return none();
    }
    if (menu->page == FOREVER_MENU_ADDON_PAGE)
    {
        if (menu->addon_tab == FOREVER_ADDON_FIND && menu->addon_query[0] &&
            forever_menu_find_set_query(menu, ""))
            return none();
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

static int find_query_separate(const struct forever_menu_state *menu)
{
    return menu->addon_tab == FOREVER_ADDON_FIND && menu->addon_query[0];
}

struct forever_menu_result forever_menu_click_line(struct forever_menu_state *menu, int line)
{
    if (menu->page == FOREVER_MENU_WAGO_KEY)
    {
        if (line < 4 || line > 6) return none();
        menu->wago_choice = line - 4;
        return forever_menu_select(menu);
    }
    if (menu->page == FOREVER_MENU_MAIN)
    {
        switch (line)
        {
        case FOREVER_MENU_LINE_FRAME_GENERATION: menu->selected = FOREVER_MENU_FRAME_GENERATION; break;
        case FOREVER_MENU_LINE_HDR: menu->selected = FOREVER_MENU_HDR; break;
        case FOREVER_MENU_LINE_HUD: menu->selected = FOREVER_MENU_HUD; break;
        case FOREVER_MENU_LINE_VOICE: menu->selected = FOREVER_MENU_VOICE; break;
        case FOREVER_MENU_LINE_PROXIMITY_VOICE:
            copy_status(menu, "Proximity Voice Chat is coming later.");
            return none();
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
        int offset = find_query_separate(menu) ? 1 : 0;
        if (line == 9 + offset)
        {
            menu->addon_selected = menu->addon_count;
            return forever_menu_select(menu);
        }
        if (line == FOREVER_MENU_LINE_ADDON_TABS + offset)
            return forever_menu_addon_button(menu, FOREVER_ADDON_RIGHT);
        if (line >= FOREVER_MENU_LINE_ADDON_FIRST + offset &&
            line <= FOREVER_MENU_LINE_ADDON_LAST + offset)
        {
            size_t selected = menu->addon_scroll +
                (size_t)(line - FOREVER_MENU_LINE_ADDON_FIRST - offset);
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
    case FOREVER_MENU_COMMAND_SET_VOICE: menu->values.voice = !!enabled; break;
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
    const char *channel = menu->addon_channel < menu->addon_channel_count ?
        menu->addon_channels[menu->addon_channel].label : "Detecting installed WoW channels";
    append(&writer, " Add-ons \xe2\x80\x94 %s\n", channel);
    if (find_query_separate(menu))
        append(&writer, " Find: \"%s\"\n", menu->addon_query);
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
    {
        static const char *const states[] = {"not set", "set", "set (not checked)", "rejected"};
        unsigned state = (unsigned)menu->wago_status;
        append(&writer, "%s Wago key\t%s\n", mark(menu->addon_selected == menu->addon_count),
               state < sizeof(states) / sizeof(states[0]) ? states[state] : states[0]);
    }
    if (selected && !find_special(selected, "menu:search") &&
        !find_special(selected, "menu:github"))
    {
        append(&writer, " ---------------------------------------------\n");
        append(&writer, " %s\n", selected->description[0] ?
               selected->description : "No description is available.");
        append(&writer, " Source\t%s\n", selected->source[0] ? selected->source : "Unknown");
        append(&writer, " Last update\t%s\n",
               selected->updated[0] ? selected->updated : "Unknown");
    }
    if (menu->status[0])
        append(&writer, " \n %s", menu->status);
    else if (menu->addon_tab == FOREVER_ADDON_FIND &&
             menu->addon_query[0] && !menu->addon_match_count)
        append(&writer, " \n No add-ons match \"%s\"", menu->addon_query);
    else
        append(&writer, " \n ");
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
                 "%s Voice-to-Text\t%s\n"
                 "  Proximity Voice Chat\tComing later\n"
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
                 mark(menu->selected == FOREVER_MENU_VOICE), menu->values.voice ? "On" : "Off",
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
    if (menu->page == FOREVER_MENU_WAGO_KEY)
    {
        snprintf(text, size,
                 " Wago key\n"
                 " ---------------------------------------------\n"
                 " Wago Patreon supporters get a key: addons.wago.io/patreon\n"
                 "\n"
                 "%s Type key\n"
                 "%s Read from file\n"
                 "%s Remove key\n"
                 "\n"
                 " File: C:\\Forever\\Addons\\wago-key.txt\n",
                 mark(menu->wago_choice == 0), mark(menu->wago_choice == 1),
                 mark(menu->wago_choice == 2));
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
