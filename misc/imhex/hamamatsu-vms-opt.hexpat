#pragma description Hamamatsu VMS .opt files

#include <std/core.pat>
#include <std/io.pat>
#include <std/mem.pat>

#pragma endian little

struct Optimisation {
    u32 offset;
    padding[36];
} [[format_read("read_optimisation")]];

fn read_optimisation(Optimisation v) {
    return std::format("{} (0x{:08X})", v.offset, v.offset);
};

Optimisation optimisations[while(!std::mem::eof())] @ 0;
