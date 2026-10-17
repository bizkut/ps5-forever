#ifndef FOREVER_WRITEBROKER_MENU_H
#define FOREVER_WRITEBROKER_MENU_H

/* Version-5-compatible capability extension. An older broker closes the
 * connection on either request, which makes startup replace it with the
 * bundled broker before the menu can remove anything. */
#define WB_MENU_CAPS_MAGIC   0x53554e4du /* "MNUS" */
#define WB_MENU_REMOVE_MAGIC 0x564f4d52u /* "RMOV" */
#define WB_MENU_CAPS_REMOVE  0x00000001


/* The only roots the broker may remove. The cached installer is index 3 and
 * is admitted only for reinstall. Shared with the title's pure path guard. */
static inline const char *wb_menu_remove_relative(unsigned index, int reinstall)
{
    static const char *const relative[] = {
        "/drive_c/Program Files (x86)/Battle.net",
        "/drive_c/ProgramData/Battle.net",
        "/drive_c/users/steamuser/AppData/Local/Battle.net",
        "/drive_c/Battle.net-Setup.exe",
    };
    return index < 3u + !!reinstall ? relative[index] : (const char *)0;
}
#endif
