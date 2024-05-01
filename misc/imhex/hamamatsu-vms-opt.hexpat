#pragma description Hamamatsu VMS .opt files

import std.core;
import std.io;
import std.mem;

#pragma endian little

struct Optimisation {
    u32 offset;
    padding[36];
} [[format_read("read_optimisation")]];

fn read_optimisation(Optimisation v) {
    return std::format("{} (0x{:08X})", v.offset, v.offset);
};

Optimisation optimisations[while(!std::mem::eof())] @ 0;
