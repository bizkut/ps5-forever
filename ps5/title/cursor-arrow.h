#ifndef PS5_CURSOR_ARROW_H
#define PS5_CURSOR_ARROW_H

#define PS5_CURSOR_WIDTH 12
#define PS5_CURSOR_HEIGHT 19

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
