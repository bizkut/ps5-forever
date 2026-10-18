#ifndef PS5_CURSOR_ARROW_H
#define PS5_CURSOR_ARROW_H

#define PS5_CURSOR_WIDTH 12
#define PS5_CURSOR_HEIGHT 19
/* Both the software desktop cursor and the GPU cursor plane hide when idle,
 * so a static arrow does not sit on an OLED panel. */
#define CURSOR_IDLE_US (3u * 1000000u)

/* # is the opaque black outline, . the opaque white fill, space transparent. */
static const char ps5_cursor_arrow[PS5_CURSOR_HEIGHT][PS5_CURSOR_WIDTH + 1] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.........# ",
    "#..........#",
    "#.....######",
    "#..#..#     ",
    "#.# #..#    ",
    "##  #..#    ",
    "#    #..#   ",
    "     #..#   ",
    "      ##    ",
};

#endif
