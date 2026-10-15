/* Force-included into every Unix-side Wine source built for the PS5
 * (ps5/build-wine.sh). The PS5's libc lacks a few FreeBSD functions that the
 * SDK's platform layer (libps5platform.a) supplies under a ps5_ prefix; the
 * calls are redirected here with function-like macros, so the type names
 * (struct if_nameindex) are untouched. The macros are variadic so that the
 * prototypes in <net/if.h> become matching ps5_ declarations instead of
 * errors. */
#ifndef PS5_WINE_COMPAT_H
#define PS5_WINE_COMPAT_H
#ifdef __PROSPERO__

struct if_nameindex;
struct if_nameindex *ps5_if_nameindex(void);
void ps5_if_freenameindex(struct if_nameindex *ptr);
#define if_nameindex(...) ps5_if_nameindex(__VA_ARGS__)
#define if_freenameindex(...) ps5_if_freenameindex(__VA_ARGS__)

#endif
#endif
