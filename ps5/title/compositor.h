/* CPU composition of Wine's shared window surfaces into display_image(). */
#ifndef PS5_TITLE_COMPOSITOR_H
#define PS5_TITLE_COMPOSITOR_H

#include <stdint.h>
#include <wine/ps5_desktop.h>
struct display_rect;

void compositor_init(int cursor_x, int cursor_y);
/* Forget a disconnected hub's windows and release every mapping. */
void compositor_reset(void);
void compositor_window(const struct ps5_desktop_window *window);
void compositor_destroy(uint64_t id);
void compositor_damage(const struct ps5_desktop_damage *damage);
void compositor_cursor(int x, int y);
/* Called on the compositor thread, after full protocol validation. */
void compositor_cursor_shape(const void *message);
/* Font and message panel adapted from ps5-forever examples/battlenet/cpu_desktop.c
 * (commit 6cf23f2d, Copyright (C) 2026 Mihawk, MIT) */
void compositor_message(const char *text);
/* The interactive Forever menu uses the build-time Noto glyph table and its
 * own large-screen geometry above compositor_message. */
void compositor_menu(const char *text);
int compositor_menu_line_at(int x, int y);
int compositor_menu_line_bounds(int line, struct display_rect *rect);
int compositor_menu_value_bounds(int line, struct display_rect *rect);
int compositor_menu_text_supported(const char *text);
int compositor_menu_footer_fits(void);
/* Recompose dirty rectangles and report them to display_damage(). Presenting
 * and the vblank wait belong to the main loop. */
void compositor_compose(void);

#endif
