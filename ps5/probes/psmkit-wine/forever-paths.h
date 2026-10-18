/* User data lives outside the app folder, which a store update replaces.
 * ShadowMountPlus 1.7beta3+ mounts /data into the title's sandbox.
 * Keep in step with runtime_profiles.USER_DATA_ROOT and the B1 profiles. */
#ifndef FOREVER_PATHS_H
#define FOREVER_PATHS_H

#define FOREVER_ROOT "/data/forever"
#define FOREVER_PREFIX FOREVER_ROOT "/prefix"
#define FOREVER_HOME FOREVER_ROOT "/home"
#define FOREVER_SHADER_CACHE FOREVER_ROOT "/radv-shader-cache"
#define FOREVER_STATE FOREVER_ROOT "/state"
#define FOREVER_DRIVE_C FOREVER_PREFIX "/drive_c"
/* the Battle.net launcher's size and time the home-screen icon was made from */
#define FOREVER_ICON_SOURCE FOREVER_ROOT "/.icon-source"
/* Shipped inside the app: first-start defaults and the list of prefix files
 * the app owns, with sources below FOREVER_APP_ROOT. */
#define FOREVER_APP_ROOT "/app0/wine-title"
#define FOREVER_APP_SHARE FOREVER_APP_ROOT "/share/forever"

#endif
