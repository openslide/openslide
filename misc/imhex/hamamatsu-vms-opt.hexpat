#pragma description Hamamatsu VMS .opt files

import std.core;
import std.io;
import std.mem;

#pragma endian little
#pragma pattern_limit 1000000

struct Optimisation {
    u32 offset;
    padding[36];

    u32 _next_offset [[hidden, no_unique_address]];
    if (_next_offset < offset) {
        break;
    }
} [[format_read("read_optimisation")]];

fn read_optimisation(Optimisation v) {
    return std::format("{} (0x{:08X})", v.offset, v.offset);
};

struct File {
    Optimisation optimisations[while(true)] [[inline]];
} [[format_read("read_file")]];

fn read_file(File v) {
    return std::format("{} optimisations", std::core::member_count(v.optimisations));
};

File files[while(!std::mem::eof())] @ 0;
