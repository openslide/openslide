#pragma description MIRAX VIMSLIDE_POSITION_BUFFER non-hierarchical section

import hex.dec;
import std.io;
import std.mem;

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

    u8 cmf = std::mem::read_unsigned($, 1, std::mem::Endian::Little);
    if ((cmf & 0xf) == 8) {
        u8 cmp[while(!std::mem::eof())] [[hidden]];
        auto decmp = std::mem::create_section("Decompressed");
        if (!hex::dec::zlib_decompress(cmp, decmp, 0)) {
            std::error("Decompression failed");
        }
        u32 count = std::mem::get_section_size(decmp) / sizeof(Position);
        Position positions[count] @ 0 in decmp;
        str _note = "See Sections tab for positions" [[export]];
    } else {
        Position positions[while(!std::mem::eof())];
    }
} [[inline]];

File file @ 0;
