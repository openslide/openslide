#pragma description Hamamatsu VMU NGR files

#pragma endian little
#pragma pattern_limit 1000000

import std.io;
import type.magic;

struct Header {
    type::Magic<"GN"> magic;
    padding[2];
    u32 width;
    u32 height;
    u32 column_width;
    padding[8];
    u32 start;
};

struct Pixel {
    u16 r;
    u16 g;
    u16 b;
} [[format_read("format_pixel"), static]];

fn format_pixel(Pixel p) {
    return std::format("({}, {}, {})", p.r, p.g, p.b);
};

struct Row<auto width> {
    Pixel pixels[width] [[inline]];
};

struct Column<auto width, auto height> {
    Row<width> rows[height] [[inline]];
};

Header header @ 0;
Column<header.column_width, header.height> columns[header.width / header.column_width] @ header.start;
