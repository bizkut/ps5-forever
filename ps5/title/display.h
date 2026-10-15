/* The title's screen: two 1920x1080 B8G8R8A8 scan-out buffers in direct
 * memory, flipped at vblank. The title composes into an ordinary linear
 * image (display_image) and copies the changed rectangles to the buffer it
 * shows next, in the 64 KiB R_X tiles VideoOut's tiling 0 uses. */
#ifndef PS5_TITLE_DISPLAY_H
#define PS5_TITLE_DISPLAY_H

#include <stdint.h>

#define DISPLAY_WIDTH 1920
#define DISPLAY_HEIGHT 1080

struct display_rect { int left, top, right, bottom; };

/* 0, or the failing call's result; the step goes to *what. */
int display_open(const char **what);
/* The composed image: DISPLAY_WIDTH * DISPLAY_HEIGHT pixels, B,G,R,A bytes
 * (0xAARRGGBB as a little-endian word). */
uint32_t *display_image(void);
/* Marks a rectangle of the image changed. */
void display_damage(const struct display_rect *rect);
/* Copies what changed since each buffer was last shown into the next buffer,
 * flips to it at the next vblank, and waits for that vblank. */
void display_present(void);

#endif
