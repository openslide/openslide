/*
 *  ImHex pattern for Zeiss CZI
 *
 *  Copyright (c) 2024 Benjamin Gilbert
 *  All rights reserved.
 *
 *  This code is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  This code is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this code. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#pragma description Zeiss CZI
#pragma endian little

import std.core;
import std.io;
import std.mem;
import std.string;

enum PixelType : u32 {
    PT_GRAY8 = 0,
    PT_GRAY16,
    PT_GRAY32FLOAT,
    PT_BGR24,
    PT_BGR48,
    PT_BGR96FLOAT = 8,
    PT_BGRA32,
    PT_GRAY64COMPLEX,
    PT_BGR192COMPLEX,
    PT_GRAY32,
    PT_GRAY64,
};


enum Compression : u32 {
    COMP_NONE = 0,
    COMP_JPEG,
    COMP_LZW,
    COMP_JXR = 4,
    COMP_ZSTD0,
    COMP_ZSTD1,
    COMP_OTHER,
};


enum PyramidType : u8 {
    PYR_NONE = 0,
    PYR_SINGLE,
    PYR_MULTIPLE,
};


// a fixed-length string that displays without its trailing NUL bytes
struct String<auto Size> {
    char s[Size];
} [[format_read("format_string"), sealed]];

fn format_string(ref String<Size> s) {
    return std::format("{}", s.s);
};


// an alternative to type::Magic that displays without its trailing NUL
// bytes
struct Magic<auto Value, auto Size> {
    String<Size> s;
    if (std::string::substr(s.s, 0, sizeof(Value)) != Value) {
        std::warning(std::format("Invalid magic: {} != {}", s.s, Value));
    }
} [[format_read("format_magic"), sealed]];

fn format_magic(ref Magic<Value, Size> m) {
    return std::format("{}", m.s);
};


struct SegmentHeader<auto Magic> {
    Magic<Magic, 16> segment_id;
    u64 allocated_size;
    u64 used_size;
} [[format_read("format_segment_header")]];

fn format_segment_header(ref SegmentHeader<Magic> hdr) {
    return std::format("{}", hdr.segment_id);
};


struct DataFileHeader {
    SegmentHeader<"ZISRAWFILE"> header;
    u32 major;
    u32 minor;
    padding[8];
    u8 primary_file_guid[16];
    u8 file_guid[16];
    u32 file_part;
    u64 subblk_dir_pos;
    u64 meta_pos;
    u32 update_pending;
    u64 att_dir_pos;
};


struct MetadataSegment {
    SegmentHeader<"ZISRAWMETADATA"> header;
    u32 xml_size;
    u32 attach_size;
    padding[248];
    char xml[xml_size];
};


struct ZstdPayloadHeader {
    u8 size;
    if (size > 1) {
        u8 chunk_type;
        u8 is_hi_low_pack;
    }
    padding[size - 3];
} [[format_read("format_zstd_payload_header")]];

fn format_zstd_payload_header(ZstdPayloadHeader hdr) {
    if (hdr.size > 1) {
        return std::format("hilo={}", hdr.is_hi_low_pack);
    } else {
        return "hilo=0";
    }
};


struct DimensionEntry {
    String<4> dimension;
    s32 start;
    u32 size;
    float start_coordinate;
    u32 stored_size;
} [[format_read("format_dimension_entry")]];

fn format_dimension_entry(ref DimensionEntry de) {
    return std::format("{}: {} [{}, {}]", de.dimension, de.start, de.stored_size, de.size);
};


struct DirectoryEntry {
    Magic<"DV", 2> schema;
    PixelType pixel_type;
    u64 file_pos;
    u32 file_part;
    Compression compression;
    PyramidType pyramid_type;
    padding[5];
    u32 ndimensions;
    DimensionEntry dimensions[ndimensions] [[format_read("format_dimensions")]];
} [[format_read("format_directory_entry")]];

fn format_directory_entry(ref DirectoryEntry de) {
    return std::format("{}", de.dimensions);
};

fn format_dimensions(ref auto de) {
    str ret = "";
    u32 downsample = 0;
    for (u32 i = 0, i < std::core::member_count(de), i = i + 1) {
        if (ret != "") {
            ret += ", ";
        }
        ret += std::format("{}: {}", de[i].dimension, de[i].start);
        if (de[i].dimension == "X") {
            downsample = de[i].size / de[i].stored_size;
        }
    }
    if (downsample) {
        ret += std::format(", DS: {}", downsample);
    }
    return ret;
};


struct SubBlockSegment {
    SegmentHeader<"ZISRAWSUBBLOCK"> header;
    u32 meta_size;
    u32 attach_size;
    u64 data_size;
    DirectoryEntry entry;
    padding[288 - ($ - addressof(this))];
    std::mem::Bytes<meta_size> meta;
    if (entry.compression == Compression::COMP_ZSTD1) {
        ZstdPayloadHeader zstd;
        std::mem::Bytes<data_size - zstd.size> data;
    } else {
        std::mem::Bytes<data_size> data;
    }
};


struct DirectoryEntryAndSegment {
    DirectoryEntry entry [[inline]];
    SubBlockSegment segment @ entry.file_pos + addressof(parent.parent);
} [[format_read("format_directory_entry_and_segment")]];

fn format_directory_entry_and_segment(ref DirectoryEntryAndSegment deas) {
    return std::format("{}", deas.entry);
};


struct SubBlockDirectorySegment {
    SegmentHeader<"ZISRAWDIRECTORY"> header;
    u32 entry_count;
    padding[124];
    DirectoryEntryAndSegment entries[entry_count] [[inline]];
};


struct AttachmentEntry {
    Magic<"A1", 2> schema;
    padding[10];
    u64 file_pos;
    u32 file_part;
    u8 guid[16];
    String<8> file_type;
    String<80> name;
} [[format_read("format_attachment_entry")]];

fn format_attachment_entry(ref AttachmentEntry ae) {
    return std::format("{} ({})", ae.name, ae.file_type);
};


using CziFile;
struct AttachmentSegment {
    SegmentHeader<"ZISRAWATTACH"> header;
    u32 data_size;
    padding[12];
    AttachmentEntry entry;
    padding[112];
    if (entry.file_type == "CZI") {
        CziFile data;
    } else {
        std::mem::Bytes<data_size> data;
    }
};


struct AttachmentEntryAndSegment {
    AttachmentEntry entry [[inline]];
    AttachmentSegment segment @ entry.file_pos + addressof(parent.parent);
} [[format_read("format_attachment_entry_and_segment")]];

fn format_attachment_entry_and_segment(ref AttachmentEntryAndSegment aeas) {
    return std::format("{}", aeas.entry);
};


struct AttachmentDirectorySegment {
    SegmentHeader<"ZISRAWATTDIR"> header;
    u32 entry_count;
    padding[252];
    AttachmentEntryAndSegment entries[entry_count] [[inline]];
};


struct CziFile {
    DataFileHeader file;
    MetadataSegment metadata @ file.meta_pos + addressof(this);
    SubBlockDirectorySegment subblocks @ file.subblk_dir_pos + addressof(this);
    AttachmentDirectorySegment attachments @ file.att_dir_pos + addressof(this);
};


CziFile czi @ 0 [[inline]];
