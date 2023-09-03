#pragma description MIRAX Index.dat

#include <std/io.pat>
#include <std/mem.pat>

#pragma endian little
#pragma eval_depth 1024

// configured by the user in the Settings tab.
// only the Slidedat knows how many records we have.
u32 hier_record_count in;
u32 nonhier_record_count in;

struct HierEntry {
    u32 image;
    u32 offset;
    u32 length;
    u32 file;
} [[format_read("format_hier_entry"), static]];

fn format_hier_entry(HierEntry e) {
    return std::format("[{}. {}, {}, {}]", e.image, e.offset, e.length, e.file);
};

struct NonHierEntry {
    u32 padding1;
    u32 padding2;
    u32 offset;
    u32 length;
    u32 file;
} [[format_read("format_nonhier_entry"), static]];

fn format_nonhier_entry(NonHierEntry e) {
    return std::format("[{}, {}, {}]", e.offset, e.length, e.file);
};

u32 cur_page = 0;
struct Page<Entry> {
    u32 page_num = cur_page;
    u32 count;
    u32 next;
    Entry entries[count];
} [[format_read("format_page"), name(std::format("[{}]", page_num))]];

fn format_page(auto p) {
    return std::format("[{} entries]", p.count);
};

struct Pages<Entry> {
    Page<Entry> page;
    if (page.next != 0) {
        cur_page = cur_page + 1;
        Pages<Entry> next_page @ page.next [[inline]];
    }
} [[inline]];

struct Record<Entry> {
    if (std::mem::read_unsigned($, 4) != 0) {
        cur_page = 0;
        Pages<Entry> *pages : u32;
    } else {
        padding[4];
    }
};

struct File {
    char version[5];
    char slide_id[32];
    Record<HierEntry> *hier_records[hier_record_count] : u32;
    Record<NonHierEntry> *nonhier_records[nonhier_record_count] : u32;
} [[inline]];

File file @ 0;
