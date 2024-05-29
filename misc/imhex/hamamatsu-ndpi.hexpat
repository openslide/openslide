// Based on ImHex TIFF pattern
// License: GPLv2

#pragma description Hamamatsu NDPI
#pragma endian little
#pragma eval_depth 100

import std.io;
import std.mem;
import std.string;
import std.core;
import type.magic;

struct TIFFHeader {
    type::Magic<"II"> Magic;
    type::Magic<"\x2a\0"> Version;
    u64 Offset;
};

enum TIFFFieldType : u16 {
    BYTE = 1,
    ASCII = 2,
    SHORT = 3,
    LONG = 4,
    RATIONAL = 5,
    SBYTE = 6,
    UNDEFINED = 7,
    SSHORT = 8,
    SLONG = 9,
    SRATIONAL = 10,
    FLOAT = 11,
    DOUBLE = 12,
};

struct TIFFRational<T> {
    T Numerator;
    T Denominator;
} [[format_read("format_read_rational")]];

fn format_read_rational(auto r) {
    return std::format("{}/{}", r.Numerator, r.Denominator);
};

enum TIFFTag : u16 {
    ImageWidth = 0x0100,
    ImageLength = 0x0101,
    BitsPerSample = 0x0102,
    Compression = 0x0103,
    PhotometricInterpretation = 0x0106,
    ImageDescription = 0x010E,
    Make = 0x010F,
    Model = 0x0110,
    StripOffsets = 0x0111,
    SamplesPerPixel = 0x0115,
    RowsPerStrip = 0x0116,
    StripByteCounts = 0x0117,
    XResolution = 0x011A,
    YResolution = 0x011B,
    ResolutionUnit = 0x0128,
    Software = 0x0131,
    DateTime = 0x0132,
    YCbCrSubSampling = 0x0212,
    ReferenceBlackWhite = 0x0214,
    SourceLens = 0xff8d,
    XOffsetFromSlideCentre = 0xff8e,
    YOffsetFromSlideCentre = 0xff8f,
    ZOffsetFromSlideCentre = 0xff90,
    OptimisationLow = 0xff92,
    OptimisationHigh = 0xff98,
    SerialNumber = 0xffa2,
    NDPIMetadata = 0xffa9,
};

struct ValueArray<T, auto Count> {
    if (Count > 1) {
        T Values[Count];
    } else {
        T Values[Count] [[hidden, no_unique_address]];
        T Value;
    }
} [[inline]];

struct ValueOffset<T, auto Count> {
    u32 ValueOffsetHigh @ addressof(parent.parent) + 12 * parent.parent.NumberDirectoryEntries + 4 * std::core::array_index() + 10;
    u64 Size = sizeof(T) * Count;
    // 0xffb3 (ASCII) is an offset even when the value fits in 4 bytes
    if (Size <= 4 && parent.Tag != 0xffb3) {
        // no simple way to append two separate byte arrays into a
        // ValueArray, so support the case we care about (StripOffsets and
        // StripByteCounts) and don't support value extensions on inline
        // values otherwise
        if (parent.Type == TIFFFieldType::LONG && Count == 1) {
            u32 ValueOffsetLow;
            u64 Value = (ValueOffsetHigh << 32) | ValueOffsetLow [[export]];
        } else if (ValueOffsetHigh) {
            std::error(std::format("Unsupported value offset extension in tag {}", parent.Tag));
        } else {
            ValueArray<T, Count> ValueArray;
            padding[4 - Size];
        }
    } else {
        u32 ValueOffsetLow;
        u64 Offset = (ValueOffsetHigh << 32) | ValueOffsetLow [[export]];
        ValueArray<T, Count> ValueArray @ Offset;
    }
} [[inline]];

struct IFDEntry {
    TIFFTag Tag;
    TIFFFieldType Type;
    u32 Count;

    match (Type) {
        (TIFFFieldType::BYTE): ValueOffset<u8, Count> ValueOffset;
        (TIFFFieldType::ASCII): ValueOffset<char, Count> ValueOffset;
        (TIFFFieldType::SHORT): ValueOffset<u16, Count> ValueOffset;
        (TIFFFieldType::LONG): ValueOffset<u32, Count> ValueOffset;
        (TIFFFieldType::RATIONAL): ValueOffset<TIFFRational<u32>, Count> ValueOffset;
        (TIFFFieldType::SBYTE): ValueOffset<s8, Count> ValueOffset;
        (TIFFFieldType::UNDEFINED): ValueOffset<u8, Count> ValueOffset;
        (TIFFFieldType::SSHORT): ValueOffset<s16, Count> ValueOffset;
        (TIFFFieldType::SLONG): ValueOffset<s32, Count> ValueOffset;
        (TIFFFieldType::SRATIONAL): ValueOffset<TIFFRational<s32>, Count> ValueOffset;
        (TIFFFieldType::FLOAT): ValueOffset<float, Count> ValueOffset;
        (TIFFFieldType::DOUBLE): ValueOffset<double, Count> ValueOffset;
        (_): {
            padding[4];
            std::print(std::format("TIFFFieldType {} not supported", u16(Type)));
        }
    }
} [[name(std::string::replace(std::core::formatted_value(Tag), "TIFFTag::", ""))]];

fn get_field(ref auto entries, TIFFTag tag) {
    for (u64 i = 0, i < std::core::member_count(entries), i = i + 1) {
        if (entries[i].Tag == tag) {
            return i;
        }
    }
    std::error(std::format("Tag {} not found in directory", tag));
};

u64 currentIFD = 0;
struct IFD {
    u64 Number = currentIFD;
    u16 NumberDirectoryEntries;
    IFDEntry DirectoryEntry[NumberDirectoryEntries];
    u64 NextIFD;

    try {
        u64 OffsetField = get_field(DirectoryEntry, TIFFTag::StripOffsets);
        u64 ByteCountField = get_field(DirectoryEntry, TIFFTag::StripByteCounts);
        if (DirectoryEntry[OffsetField].Count != 1) {
            std::warning(std::format("IFD {} strip count {} != 1", currentIFD, DirectoryEntry[OffsetField].Count));
        }
        u64 Offset = DirectoryEntry[OffsetField].ValueOffset.Value;
        u64 ByteCount = DirectoryEntry[ByteCountField].ValueOffset.Value;
        std::mem::Bytes<ByteCount> ImageData @ Offset;
    } catch {}
} [[name(std::format("IFD {}", Number))]];

struct IFDS {
    IFD IFD;
    if (IFD.NextIFD > 0) {
        currentIFD += 1;
        IFDS IFD_tmp @ IFD.NextIFD;
    }
} [[inline]];

struct TIFFFile {
    TIFFHeader Header;
    IFDS @ Header.Offset;
};

TIFFFile File @ 0x00;
