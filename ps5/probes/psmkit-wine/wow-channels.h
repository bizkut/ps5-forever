/* Structural WoW discovery shared by launch, settings and the Add-ons menu.
 * No product allow-list participates in launch admission. */
#ifndef FOREVER_WOW_CHANNELS_H
#define FOREVER_WOW_CHANNELS_H
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#define WOW_PATH_BYTES 1100
#define WOW_FOLDER_BYTES 256
#define WOW_PRODUCT_BYTES 128
struct wow_channel
{
    char folder[WOW_FOLDER_BYTES], product[WOW_PRODUCT_BYTES];
    char windows_cwd[WOW_PATH_BYTES], unix_cwd[WOW_PATH_BYTES];
    char executable[WOW_FOLDER_BYTES], unix_loader[WOW_PATH_BYTES];
};
typedef int (*wow_channel_fn)(const struct wow_channel *, void *);
static inline int wow_regular(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISREG(st.st_mode);
}
static inline int wow_join(char *out, size_t size, const char *directory, const char *name)
{
    int n = snprintf(out, size, "%s/%s", directory, name);
    return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}
static inline int wow_executable_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 7 || strncasecmp(name, "Wow", 3) || strcasecmp(name + n - 4, ".exe")) return 0;
    for (size_t i = 3; i < n - 4; ++i)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z'))) return 0;
    return 1;
}
static inline int wow_folder_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 3 || n >= WOW_FOLDER_BYTES || name[0] != '_' || name[n - 1] != '_') return 0;
    for (size_t i = 1; i + 1 < n; ++i)
        if ((unsigned char)name[i] < 32 || strchr("<>:\"/\\|?*", name[i])) return 0;
    return 1;
}
static inline int wow_channel_parse(const char *image, struct wow_channel *channel)
{
    char path[WOW_PATH_BYTES], *exe, *folder, *world;
    size_t n = strlen(image);
    if (n >= sizeof(path) || n < 3 || (image[0] != 'C' && image[0] != 'c' && image[0] != 'W' && image[0] != 'w') ||
        image[1] != ':' || (image[2] != '/' && image[2] != '\\')) return -EINVAL;
    memcpy(path, image, n + 1);
    for (size_t i = 2; i < n; ++i) if (path[i] == '\\') path[i] = '/';
    if (strstr(path, "/../") || strstr(path, "/./") || strstr(path, "//")) return -EINVAL;
    exe = strrchr(path, '/');
    if (!exe || !wow_executable_name(exe + 1)) return -EINVAL;
    *exe++ = 0; folder = strrchr(path, '/');
    if (!folder || !wow_folder_name(folder + 1)) return -EINVAL;
    *folder++ = 0; world = strrchr(path, '/');
    if (!world || strcasecmp(world + 1, "World of Warcraft")) return -EINVAL;
    memset(channel, 0, sizeof(*channel));
    snprintf(channel->folder, sizeof(channel->folder), "%s", folder);
    snprintf(channel->executable, sizeof(channel->executable), "%s", exe);
    n = (size_t)(exe - path - 1);
    memcpy(channel->windows_cwd, image, n); channel->windows_cwd[n] = 0;
    return 0;
}
static inline int wow_product_valid(const char *text)
{
    size_t n = strlen(text);
    if (!n || n >= WOW_PRODUCT_BYTES) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') ||
              (text[i] >= '0' && text[i] <= '9') || text[i] == '_' || text[i] == '-')) return 0;
    return 1;
}
/* .build.info is transient. Only an identified row may fill missing metadata. */
static inline void wow_build_product(const char *root, const char *folder, char *product)
{
    char path[WOW_PATH_BYTES], line[8192], branch[WOW_FOLDER_BYTES], expected[WOW_PRODUCT_BYTES];
    int branch_column = -1, product_column = -1;
    FILE *file;
    if (wow_join(path, sizeof(path), root, ".build.info") || !(file = fopen(path, "r"))) return;
    snprintf(branch, sizeof(branch), "%.*s", (int)strlen(folder) - 2, folder + 1);
    snprintf(expected, sizeof(expected), "wow_%.*s", (int)sizeof(expected) - 5, branch);
    if (!strcasecmp(folder, "_retail_")) strcpy(expected, "wow");
    else if (!strcasecmp(folder, "_ptr_")) strcpy(expected, "wowt");
    else if (!strcasecmp(folder, "_xptr_")) strcpy(expected, "wowxptr");
    else if (!strcasecmp(folder, "_forever_")) strcpy(expected, "wowf");
    if (fgets(line, sizeof(line), file))
    {
        char *at = line, *next;
        for (int i = 0; at; ++i, at = next)
        {
            next = strchr(at, '|'); if (next) *next++ = 0;
            if (!strncmp(at, "Branch!", 7)) branch_column = i;
            if (!strncmp(at, "Product!", 8)) product_column = i;
        }
    }
    while (fgets(line, sizeof(line), file))
    {
        char *at = line, *next, *row_branch = NULL, *row_product = NULL;
        line[strcspn(line, "\r\n")] = 0;
        for (int i = 0; at; ++i, at = next)
        {
            next = strchr(at, '|'); if (next) *next++ = 0;
            if (i == branch_column) row_branch = at;
            if (i == product_column) row_product = at;
        }
        if (row_product && wow_product_valid(row_product) &&
            (!strcasecmp(row_product, expected) || (row_branch && !strcasecmp(row_branch, branch))))
        { strcpy(product, row_product); break; }
    }
    fclose(file);
}
static inline int wow_ascii_equal(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
    {
        unsigned char left = *a, right = *b;
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) return 0;
    }
    return !*a && !*b;
}
/* Resolve both leaves in one directory pass. Windows spelling is insensitive
 * to ASCII case, but two native case variants are ambiguous, even when one
 * happens to match the requested spelling exactly. Outputs use on-disk case. */
static inline int wow_channel_resolve_files(const char *cwd, const char *requested,
                                            char *executable, char *loader, void *context)
{
    char wanted_loader[WOW_FOLDER_BYTES], path[WOW_PATH_BYTES];
    const char *wanted[] = {requested, wanted_loader};
    char *found[] = {executable, loader};
    unsigned matches[2] = {0, 0};
    struct dirent *entry;
    DIR *directory;
    int error = 0, length;
    (void)context;
    if (!wow_executable_name(requested)) return -EINVAL;
    length = snprintf(wanted_loader, sizeof(wanted_loader), "%.*s_loader.dll",
                      (int)strlen(requested) - 4, requested);
    if (length < 0 || (size_t)length >= sizeof(wanted_loader)) return -ENAMETOOLONG;
    if (!(directory = opendir(cwd))) return -errno;
    errno = 0;
    while ((entry = readdir(directory)))
        for (unsigned i = 0; i < 2; ++i)
            if (wow_ascii_equal(entry->d_name, wanted[i]))
            {
                if (++matches[i] > 1) error = -EEXIST;
                else strcpy(found[i], entry->d_name);
            }
    if (errno && !error) error = -errno;
    closedir(directory);
    if (error) return error;
    for (unsigned i = 0; i < 2; ++i)
    {
        if (matches[i] != 1) return -ENOENT;
        if (wow_join(path, sizeof(path), cwd, found[i])) return -ENAMETOOLONG;
        if (!wow_regular(path)) return -ENOENT;
    }
    return 0;
}

static inline int wow_channel_read(const char *root, const char *windows_root, const char *folder,
                                   struct wow_channel *channel)
{
    char path[WOW_PATH_BYTES], line[256];
    FILE *file;
    DIR *directory;
    struct dirent *entry;
    int n, has_flavor = 0;
    if (!wow_folder_name(folder)) return -EINVAL;
    memset(channel, 0, sizeof(*channel));
    strcpy(channel->folder, folder);
    if (wow_join(channel->unix_cwd, sizeof(channel->unix_cwd), root, folder)) return -ENAMETOOLONG;
    n = snprintf(channel->windows_cwd, sizeof(channel->windows_cwd), "%s\\%s", windows_root, folder);
    if (n < 0 || (size_t)n >= sizeof(channel->windows_cwd)) return -ENAMETOOLONG;
    if (!(directory = opendir(channel->unix_cwd))) return -errno;
    if (!wow_join(path, sizeof(path), channel->unix_cwd, ".flavor.info") && (file = fopen(path, "r")))
    {
        has_flavor = 1;
        if (fgets(line, sizeof(line), file) && fgets(line, sizeof(line), file))
        {
            line[strcspn(line, "\r\n")] = 0;
            if (wow_product_valid(line)) strcpy(channel->product, line);
        }
        fclose(file);
    }
    if (!channel->product[0]) wow_build_product(root, folder, channel->product);
    while ((entry = readdir(directory)))
        if (wow_executable_name(entry->d_name) &&
            !wow_join(path, sizeof(path), channel->unix_cwd, entry->d_name) && wow_regular(path) &&
            (!channel->executable[0] || strcmp(entry->d_name, channel->executable) < 0))
            snprintf(channel->executable, sizeof(channel->executable), "%s", entry->d_name);
    closedir(directory);
    if (!has_flavor && !channel->executable[0]) return -ENOENT;
    if (channel->executable[0])
    {
        char loader[WOW_FOLDER_BYTES];
        int error = wow_channel_resolve_files(channel->unix_cwd, channel->executable,
                                              channel->executable, loader, NULL);
        /* Settings/Add-ons still discover incomplete or ambiguous installs;
         * the empty loader keeps them out of direct-start admission. */
        if (error && error != -ENOENT && error != -EEXIST) return error;
        if (!error && wow_join(channel->unix_loader, sizeof(channel->unix_loader),
                               channel->unix_cwd, loader)) return -ENAMETOOLONG;
    }
    return 0;
}
static inline int wow_channels_scan(const char *prefix, wow_channel_fn visit, void *context)
{
    static const char windows_root[] = "C:\\Program Files (x86)\\World of Warcraft";
    char root[WOW_PATH_BYTES];
    struct wow_channel channel;
    DIR *directory;
    struct dirent *entry;
    int error;
    if ((error = wow_join(root, sizeof(root), prefix, "drive_c/Program Files (x86)/World of Warcraft"))) return error;
    if (!(directory = opendir(root))) return errno == ENOENT ? 0 : -errno;
    while ((entry = readdir(directory)))
    {
        if (!wow_folder_name(entry->d_name)) continue;
        error = wow_channel_read(root, windows_root, entry->d_name, &channel);
        if (error == -ENOENT || error == -ENOTDIR) continue;
        if (error || (error = visit(&channel, context))) { closedir(directory); return error; }
    }
    closedir(directory);
    return 0;
}
struct wow_selection { const char *name; struct wow_channel *channel; int rank; };
static inline int wow_select_visit(const struct wow_channel *channel, void *context)
{
    struct wow_selection *selection = context;
    int rank = !strcasecmp(channel->folder, "_classic_beta_") ? 3 :
               !strcasecmp(channel->folder, "_forever_") ? 2 : 1;
    if (!channel->executable[0] || !channel->unix_loader[0]) return 0;
    if (selection->name[0])
    {
        if (strcasecmp(channel->folder, selection->name)) return 0;
        rank = 4;
    }
    if (rank > selection->rank || (rank == selection->rank && strcmp(channel->folder, selection->channel->folder) < 0))
    { *selection->channel = *channel; selection->rank = rank; }
    return 0;
}
static inline int wow_channel_select(const char *prefix, const char *name, struct wow_channel *channel)
{
    struct wow_selection selection = {name, channel, 0};
    int error;
    if (*name && !wow_folder_name(name)) return -EINVAL;
    error = wow_channels_scan(prefix, wow_select_visit, &selection);
    return error ? error : selection.rank ? 0 : -ENOENT;
}
/* TACT and Agent use different identifiers for the two retail PTR products.
 * Play's supplied uid is never rewritten; only direct start needs this alias. */
static inline const char *wow_channel_launch_uid(const struct wow_channel *channel)
{
    if (!strcasecmp(channel->product, "wowt")) return "wow_ptr";
    if (!strcasecmp(channel->product, "wowxptr")) return "wow_ptr_x";
    return channel->product;
}
static inline const char *wow_channel_label(const struct wow_channel *channel)
{
    static const struct { const char *product, *label; } labels[] = {
        {"wow", "WoW Retail"}, {"wowt", "WoW PTR"}, {"wowxptr", "WoW xPTR"},
        {"wow_beta", "WoW Beta"}, {"wowf", "WoW Forever"},
        {"wow_classic", "WoW Classic"}, {"wow_classic_beta", "WoW Classic Beta"},
        {"wow_classic_ptr", "WoW Classic PTR"}, {"wow_classic_era", "WoW Classic Era"},
        {"wow_classic_era_ptr", "WoW Classic Era PTR"}, {"wow_classic_era_beta", "WoW Classic Era Beta"},
        {"wow_anniversary", "WoW Anniversary"}
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i)
        if (!strcasecmp(channel->product, labels[i].product)) return labels[i].label;
    return channel->folder;
}
#endif
