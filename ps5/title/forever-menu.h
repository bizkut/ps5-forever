#ifndef PS5_TITLE_FOREVER_MENU_H
#define PS5_TITLE_FOREVER_MENU_H

#include <stddef.h>

#define FOREVER_MENU_TEXT_BYTES 4096
#define FOREVER_MENU_STATUS_BYTES 256
#define FOREVER_ADDON_ROWS 4096
#define FOREVER_ADDON_VIEW_ROWS (FOREVER_ADDON_ROWS + 2)
#define FOREVER_ADDON_KEY_BYTES 224
#define FOREVER_ADDON_NAME_BYTES 96
#define FOREVER_ADDON_DETAIL_BYTES 240
#define FOREVER_ADDON_SOURCE_BYTES 80
#define FOREVER_ADDON_DATE_BYTES 32
#define FOREVER_ADDON_QUERY_BYTES 65

/* Text rows used by the compositor and mouse hit testing. */
#define FOREVER_MENU_LINE_FRAME_GENERATION 2
#define FOREVER_MENU_LINE_HDR 3
#define FOREVER_MENU_LINE_HUD 4
#define FOREVER_MENU_LINE_VOICE 5
#define FOREVER_MENU_LINE_PROXIMITY_VOICE 6
#define FOREVER_MENU_LINE_ADDONS 8
#define FOREVER_MENU_LINE_REINSTALL 10
#define FOREVER_MENU_LINE_UNINSTALL 11
#define FOREVER_MENU_LINE_CLOSE 13
#define FOREVER_MENU_LINE_CANCEL 7
#define FOREVER_MENU_LINE_CONFIRM 8
#define FOREVER_MENU_LINE_ADDON_TABS 2
#define FOREVER_MENU_LINE_ADDON_FIRST 4
#define FOREVER_MENU_LINE_ADDON_LAST 8

enum forever_menu_page
{
    FOREVER_MENU_CLOSED,
    FOREVER_MENU_MAIN,
    FOREVER_MENU_CONFIRM_REINSTALL,
    FOREVER_MENU_CONFIRM_UNINSTALL,
    FOREVER_MENU_PROGRESS,
    FOREVER_MENU_NOTICE,
    FOREVER_MENU_ADDON_PAGE,
    FOREVER_MENU_ADDON_CONFIRM,
    FOREVER_MENU_WAGO_KEY,
};

enum forever_menu_item
{
    FOREVER_MENU_FRAME_GENERATION,
    FOREVER_MENU_HDR,
    FOREVER_MENU_HUD,
    FOREVER_MENU_VOICE,
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
    FOREVER_MENU_COMMAND_SET_VOICE,
    FOREVER_MENU_COMMAND_REINSTALL,
    FOREVER_MENU_COMMAND_UNINSTALL,
    FOREVER_MENU_COMMAND_CLOSE,
    FOREVER_MENU_COMMAND_ADDONS_OPEN,
    FOREVER_MENU_COMMAND_ADDONS_CLOSE,
    FOREVER_MENU_COMMAND_ADDONS_REFRESH,
    FOREVER_MENU_COMMAND_ADDONS_INSTALL,
    FOREVER_MENU_COMMAND_ADDONS_UPDATE,
    FOREVER_MENU_COMMAND_ADDONS_UPDATE_ALL,
    FOREVER_MENU_COMMAND_ADDONS_REMOVE,
    FOREVER_MENU_COMMAND_ADDONS_INSTALL_PACK,
    FOREVER_MENU_COMMAND_ADDONS_INSTALL_GITHUB,
    FOREVER_MENU_COMMAND_ADDONS_SEARCH_IME,
    FOREVER_MENU_COMMAND_ADDONS_GITHUB_IME,
    FOREVER_MENU_COMMAND_WAGO_IME,
    FOREVER_MENU_COMMAND_WAGO_SET,
    FOREVER_MENU_COMMAND_WAGO_FILE,
    FOREVER_MENU_COMMAND_WAGO_REMOVE,
};

enum forever_frame_generation
{
    FOREVER_FG_OFF,
    FOREVER_FG_ON,
    FOREVER_FG_AUTO,
};

#define FOREVER_ADDON_CHANNELS 64
struct forever_addon_install
{
    char path[1100];
    char label[96];
};

enum forever_addon_tab
{
    FOREVER_ADDON_INSTALLED,
    FOREVER_ADDON_UPDATES,
    FOREVER_ADDON_FIND,
    FOREVER_ADDON_STARTER_PACKS,
    FOREVER_ADDON_TAB_COUNT,
};

enum forever_addon_button
{
    FOREVER_ADDON_LEFT,
    FOREVER_ADDON_RIGHT,
    FOREVER_ADDON_L1,
    FOREVER_ADDON_R1,
    FOREVER_ADDON_SQUARE,
    FOREVER_ADDON_TRIANGLE,
    FOREVER_ADDON_R3,
};

struct forever_addon_row
{
    char key[FOREVER_ADDON_KEY_BYTES];
    char name[FOREVER_ADDON_NAME_BYTES];
    char status[48];
    char description[FOREVER_ADDON_DETAIL_BYTES];
    char filter_name[FOREVER_ADDON_NAME_BYTES];
    char filter_description[FOREVER_ADDON_DETAIL_BYTES];
    char source[FOREVER_ADDON_SOURCE_BYTES];
    char updated[FOREVER_ADDON_DATE_BYTES];
    unsigned long long downloads;
    int installable;
    int update_available;
    int check_failed;
    int curse_only;
};

enum forever_wago_key_status
{
    FOREVER_WAGO_NOT_SET,
    FOREVER_WAGO_SET,
    FOREVER_WAGO_NOT_CHECKED,
    FOREVER_WAGO_REJECTED,
};

struct forever_addon_snapshot
{
    enum forever_addon_tab tab;
    struct forever_addon_row rows[FOREVER_ADDON_ROWS];
    size_t count;
    enum forever_wago_key_status wago_status;
    int wago_saved;
    struct forever_addon_install channels[FOREVER_ADDON_CHANNELS];
    size_t channel_count;
    char message[FOREVER_MENU_STATUS_BYTES];
    char notice[FOREVER_MENU_STATUS_BYTES]; /* successful empty view, not an operation failure */
};

struct forever_menu_values
{
    enum forever_frame_generation frame_generation;
    int hdr;
    int hud;
    int voice;
};

struct forever_menu_result
{
    enum forever_menu_command command;
    int enabled;
    unsigned channel;
    enum forever_addon_tab tab;
    char key[FOREVER_ADDON_KEY_BYTES];
};

struct forever_menu_state
{
    enum forever_menu_page page;
    enum forever_menu_item selected;
    int confirmation_selected; /* zero is Cancel, deliberately the default */
    struct forever_menu_values values;
    int voice_downloading;
    unsigned addon_channel;
    struct forever_addon_install addon_channels[FOREVER_ADDON_CHANNELS];
    size_t addon_channel_count;
    enum forever_addon_tab addon_tab;
    int addon_sort_downloads;
    size_t addon_selected;
    size_t addon_scroll;
    size_t addon_count;
    int addon_loading;
    enum forever_menu_command addon_confirmation;
    struct forever_addon_row addon_rows[FOREVER_ADDON_VIEW_ROWS];
    struct forever_addon_row addon_catalog_rows[FOREVER_ADDON_ROWS];
    size_t addon_catalog_count;
    size_t addon_match_count;
    int addon_pending_github;
    char addon_query[FOREVER_ADDON_QUERY_BYTES];
    enum forever_wago_key_status wago_status;
    int wago_choice;
    char status[FOREVER_MENU_STATUS_BYTES];
};

void forever_menu_init(struct forever_menu_state *menu);
void forever_menu_open(struct forever_menu_state *menu, const struct forever_menu_values *values);
int forever_menu_is_open(const struct forever_menu_state *menu);
void forever_menu_move(struct forever_menu_state *menu, int direction);
struct forever_menu_result forever_menu_select(struct forever_menu_state *menu);
struct forever_menu_result forever_menu_addon_button(
    struct forever_menu_state *menu, enum forever_addon_button button);
void forever_menu_addon_snapshot(
    struct forever_menu_state *menu, const struct forever_addon_snapshot *snapshot);
void forever_menu_addon_error(struct forever_menu_state *menu, const char *text);
int forever_menu_addon_pause(struct forever_menu_state *menu, const char *text);
struct forever_menu_result forever_menu_back(struct forever_menu_state *menu);
struct forever_menu_result forever_menu_click_line(struct forever_menu_state *menu, int line);
void forever_menu_commit(struct forever_menu_state *menu, enum forever_menu_command command, int enabled);
void forever_menu_status(struct forever_menu_state *menu, const char *text);
int forever_menu_find_set_query(struct forever_menu_state *menu, const char *query);
int forever_menu_find_append(struct forever_menu_state *menu, const char *text);
int forever_menu_find_backspace(struct forever_menu_state *menu);
void forever_menu_find_ime(struct forever_menu_state *menu, const char *text, int accepted);
int forever_menu_github_repo(const char *input, char *repo, size_t repo_size,
                             char *error, size_t error_size);
struct forever_menu_result forever_menu_github_ime(
    struct forever_menu_state *menu, const char *text, int accepted);
void forever_menu_progress(struct forever_menu_state *menu, const char *text);
void forever_menu_notice(struct forever_menu_state *menu, const char *text);
void forever_menu_close(struct forever_menu_state *menu);
void forever_menu_render(const struct forever_menu_state *menu, char *text, size_t size);

#endif
