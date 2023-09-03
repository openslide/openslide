#pragma description MIRAX VIMSLIDE_POSITION_BUFFER non-hierarchical section

// TODO: support StitchingIntensityLevel, automatically inflating with
// https://github.com/Jusb3/ImHex-Zlib-Plugin

#include <std/io.pat>
#include <std/mem.pat>

#pragma endian little

struct Position {
    u8 flags;
    s32 x;
    s32 y;
} [[format_read("format_position"), static]];

fn format_position(Position p) {
    if (p.x == 0 && p.y == 0 && p.flags == 0) {
        return "-0-";
    }
    return std::format("({}, {}) [{:x}]", p.x, p.y, p.flags);
};

struct File {
    char version[5];
    char slide_id[32];
    char file[3];
    padding[256];
    Position positions[while(!std::mem::eof())];
} [[inline]];

File file @ 0;
