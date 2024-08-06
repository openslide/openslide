/*
 * InteMedic (tron) support
 */

// References used...
//   https://github.com/dotnet/runtime/tree/main/src/libraries/System.IO.Compression/src/System/IO/Compression
//   https://www.nuget.org/packages/IC.SlideServices.FileFormat.Tronmedi.NET40/
//   https://github.com/matt-wu/AES/
//   https://github.com/lacchain/openssl-pqe-engine/tree/61d0fe530720f6b7e646db786c79f3db716133f3/ibrand_service

#include "openslide-private.h"
#include "openslide-decode-aes.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-pbkdf2.h"

#include <math.h>

#include <openssl/sha.h>
#include "json.h"

static const bool SupportLegacy = false; // TODO support Legacy(version <= 3)

static const char TRON_EXT[] = ".tron";

static const char MetadataFileName[] = ".tron";

static const char CypherKey[] = "7D4D665B98FB4C6BA7F820A77BF53DA677E28AAA3C8147A4863EAC0042A9713A2D7FF16AEE2F4602A1908948196CB78659B1FCB3A6E14CDA839E2617AC44694B";

static const char SlideMetadata[] = "SlideMetadata";
static const char KEY_MINIMUM_LOD_LEVEL[] = "MinimumLODLevel";
static const char KEY_MAXIMUM_LOD_LEVEL[] = "MaximumLODLevel";
static const char KEY_MAXIMUM_ZOOM_LEVEL[] = "MaximumZoomLevel";
static const char KEY_BACKGROUND_COLOR[] = "BackgroundColor";
static const char KEY_HORIZONTAL_TILE_COUNT[] = "HorizontalTileCount";
static const char KEY_VERTICAL_TILE_COUNT[] = "VerticalTileCount";
static const char KEY_TILE_SIZE[] = "TileSize";
static const char KEY_HORIZONTAL_RESOLUTION[] = "HorizontalResolution";
static const char KEY_VERTICAL_RESOLUTION[] = "VerticalResolution";
static const char KEY_ADDITIONAL_DATA[] = "AdditionalData";
static const char KEY_SCAN_DATE_UTC[] = "ScanDateUtc";
static const char KEY_SCAN_TIME[] = "ScanTime";
static const char KEY_RESAMPLE_FACTOR[] = "ResampleFactor";
static const char KEY_SCANNER_MODEL[] = "ScannerModel";

static const char LabelFileName[] = "label";
static const char MacroFileName[] = "macro";
static const char SampleFileName[] = "sample";
static const char BlankFileName[] = "blank";

// This is an abstract concept and NOT the ZLib compression level.
// There may or may not be any correspondence with the a possible implementation-specific level-parameter of the deflater.
enum CompressionLevel {
  /// <summary>
  /// The compression operation should balance compression speed and output size.
  /// </summary>
  Optimal = 0,

  /// <summary>
  /// The compression operation should complete as quickly as possible, even if the resulting file is not optimally compressed.
  /// </summary>
  Fastest = 1,

  /// <summary>
  /// No compression should be performed on the file.
  /// </summary>
  NoCompression = 2,

  /// <summary>
  /// The compression operation should create output as small as possible, even if the operation takes a longer time to complete.
  /// </summary>
  SmallestSize = 3,
};

enum BitFlagValues {
  IsEncrypted = 0x1,
  DataDescriptor = 0x8,
  UnicodeFileNameAndComment = 0x800
};

enum CompressionMethodValues {
  Stored = 0x0,
  C_Deflate = 0x8,
  C_Deflate64 = 0x9,
  BZip2 = 0xC,
  LZMA = 0xE
};

enum ZipVersionNeededValues {
  Default = 10,
  ExplicitDirectory = 20,
  Z_Deflate = 20,
  Z_Deflate64 = 21,
  Zip64 = 45
};

enum ZipVersionMadeByPlatform {
  Windows = 0,
  Unix = 3,
};

struct ZipArchiveEntry {
  bool _originallyInArchive;
  uint32_t _diskNumberStart;
  enum ZipVersionMadeByPlatform _versionMadeByPlatform;
  enum ZipVersionNeededValues _versionMadeBySpecification;
  enum ZipVersionNeededValues _versionToExtract;
  enum BitFlagValues _generalPurposeBitFlag;
  bool _isEncrypted;
  enum CompressionMethodValues _storedCompressionMethod;
  uint64_t _compressedSize;
  uint64_t _uncompressedSize;
  uint64_t _offsetOfLocalHeader;
  uint64_t _storedOffsetOfCompressedData;
  uint32_t _crc32;
  bool _currentlyOpenForWrite;
  bool _everOpenedForWrite;
  uint32_t _externalFileAttr;
  char *_storedEntryName;
  enum CompressionLevel _compressionLevel;
};

static void destroy_entry(struct ZipArchiveEntry *entry) {
  g_free(entry->_storedEntryName);
  g_free(entry);
}

typedef struct ZipArchiveEntry ZipArchiveEntry;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ZipArchiveEntry, destroy_entry)

struct ZipCentralDirectoryFileHeader {
  // char VersionMadeByCompatibility; skip for now
  // char VersionMadeBySpecification; skip for now
  uint16_t VersionNeededToExtract;
  uint16_t GeneralPurposeBitFlag;
  uint16_t CompressionMethod;
  uint32_t LastModified; // convert this on the fly
  uint32_t Crc32;
  uint64_t CompressedSize;
  uint64_t UncompressedSize;
  uint16_t FilenameLength;
  uint16_t ExtraFieldLength;
  uint16_t FileCommentLength;
  uint32_t DiskNumberStart;
  uint16_t InternalFileAttributes;
  uint32_t ExternalFileAttributes;
  uint64_t RelativeOffsetOfLocalHeader;

  char *Filename;
  // char *FileComment; no comments for now
};

static void destroy_header(struct ZipCentralDirectoryFileHeader *header) {
  // g_free(header->VersionMadeByCompatibility);
  // g_free(header->VersionMadeBySpecification);
  g_free(header->Filename);
  g_free(header);
}

typedef struct ZipCentralDirectoryFileHeader ZipCentralDirectoryFileHeader;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ZipCentralDirectoryFileHeader, destroy_header)

struct Zip64EndOfCentralDirectoryRecord {
  uint64_t SizeOfThisRecord;
  uint16_t VersionMadeBy;
  uint16_t VersionNeededToExtract;
  uint32_t NumberOfThisDisk;
  uint32_t NumberOfDiskWithStartOfCD;
  uint64_t NumberOfEntriesOnThisDisk;
  uint64_t NumberOfEntriesTotal;
  uint64_t SizeOfCentralDirectory;
  uint64_t OffsetOfCentralDirectory;
};

static void destroy_record(struct Zip64EndOfCentralDirectoryRecord *record) {
  g_free(record);
}

typedef struct Zip64EndOfCentralDirectoryRecord Zip64EndOfCentralDirectoryRecord;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Zip64EndOfCentralDirectoryRecord, destroy_record)

struct Zip64EndOfCentralDirectoryLocator {
  uint32_t NumberOfDiskWithZip64EOCD;
  uint64_t OffsetOfZip64EOCD;
  uint32_t TotalNumberOfDisks;
};

static void destroy_locator(struct Zip64EndOfCentralDirectoryLocator *locator) {
  g_free(locator);
}

typedef struct Zip64EndOfCentralDirectoryLocator Zip64EndOfCentralDirectoryLocator;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Zip64EndOfCentralDirectoryLocator, destroy_locator)

struct ZipEndOfCentralDirectoryBlock {
  uint32_t Signature;
  uint16_t NumberOfThisDisk;
  uint16_t NumberOfTheDiskWithTheStartOfTheCentralDirectory;
  uint16_t NumberOfEntriesInTheCentralDirectoryOnThisDisk;
  uint16_t NumberOfEntriesInTheCentralDirectory;
  uint32_t SizeOfCentralDirectory;
  uint32_t OffsetOfStartOfCentralDirectoryWithRespectToTheStartingDiskNumber;
  // char *ArchiveComment; no comments for now
};

static void destroy_eocd(struct ZipEndOfCentralDirectoryBlock *eocd) {
  g_free(eocd);
}

typedef struct ZipEndOfCentralDirectoryBlock ZipEndOfCentralDirectoryBlock;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ZipEndOfCentralDirectoryBlock, destroy_eocd)

struct intemedic_ops_data {
  char *filename;
};

struct image {
  uint64_t start_in_file;
  uint64_t compressed_size;
  uint64_t uncompressed_size;
  int32_t imageno; // used only for cache lookup
  int32_t width;
  int32_t height;
  int refcount;
};

struct tile {
  struct image *image;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_free(l);
}

typedef struct level level;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(level, destroy_level)

static void destroy(openslide_t *osr) {
  struct intemedic_ops_data *data = osr->data;

  // levels
  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *)osr->levels[i]);
  }
  g_free(osr->levels);

  // the ops data
  g_free(data->filename);
  g_free(data);
}

static void image_unref(struct image *image) {
  if (!--image->refcount) {
    g_free(image);
  }
}

typedef struct image image;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(image, image_unref)

static void tile_free(gpointer data) {
  struct tile *tile = data;
  image_unref(tile->image);
  g_free(tile);
}

static void *read_compressed_data(struct _openslide_file *f,
                                  int64_t size, int64_t offset,
                                  GError **err) {
  if (!_openslide_fseek(f, offset, SEEK_SET, err)) {
    g_prefix_error(err, "Cannot seek compressed data: ");
    return NULL;
  }

  g_autofree void *buffer = g_malloc(size);
  if (!_openslide_fread_exact(f, buffer, size, err)) {
    g_prefix_error(err, "Error while reading compressed data: ");
    return NULL;
  }

  return g_steal_pointer(&buffer);
}

static void *decode_item(struct _openslide_file *f,
                         uint64_t compressed_size, uint64_t uncompressed_size, uint64_t offset,
                         GError **err) {
  g_autofree void *compressed_data = read_compressed_data(f,
                                                          compressed_size, offset,
                                                          err);
  if (!compressed_data) {
    g_prefix_error(err, "Cannot read compressed data: ");
    return NULL;
  }

  void *uncompressed = _openslide_inflate_buffer_2(compressed_data,
                                                   compressed_size,
                                                   uncompressed_size,
                                                   err);
  g_free(g_steal_pointer(&compressed_data));
  if (!uncompressed) {
    g_prefix_error(err, "Error decompressing compressed data: ");
    return NULL;
  }

  return uncompressed;
}

static uint32_t *read_image(openslide_t *osr,
                            struct image *image,
                            int w, int h,
                            GError **err) {
  struct intemedic_ops_data *data = osr->data;
  bool result = false;

  g_autoptr(_openslide_file) f = _openslide_fopen(data->filename, err);
  if (f == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, "File is NULL");
    return NULL;
  }
  g_autofree void *uncompressed = decode_item(f,
                                              image->compressed_size,
                                              image->uncompressed_size,
                                              image->start_in_file,
                                              err);
  if (!uncompressed) {
    g_prefix_error(err, "Error decompressing tile buffer: ");
    return NULL;
  }

  g_autofree uint32_t *dest = g_malloc(w * h * 4);
  result = _openslide_jpeg_decode_buffer(uncompressed,
                                         image->uncompressed_size,
                                         dest, w, h,
                                         err);
  g_free(g_steal_pointer(&uncompressed));
  if (!result) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't decode jpeg buffer");
    return NULL;
  }
  return g_steal_pointer(&dest);
}

static bool read_missing_tile(openslide_t *osr,
                              cairo_t *cr,
                              struct _openslide_level *level,
                              void *arg G_GNUC_UNUSED,
                              GError **err G_GNUC_UNUSED) {
  bool success = true;

  struct level *l = (struct level *)level;
  int64_t tile_w = l->base.tile_w;
  int64_t tile_h = l->base.tile_h;

  uint8_t bg_r = 0xFF;
  uint8_t bg_g = 0xFF;
  uint8_t bg_b = 0xFF;
  const char *bgcolor = openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BACKGROUND_COLOR);
  if (bgcolor) {
    uint64_t bg = g_ascii_strtoull(bgcolor, NULL, 16);
    bg_r = (bg >> 16) & 0xFF;
    bg_g = (bg >> 8) & 0xFF;
    bg_b = bg & 0xFF;
  }

  // draw background
  double r = bg_r / 255.0;
  double g = bg_g / 255.0;
  double b = bg_b / 255.0;
  cairo_set_source_rgb(cr, r, g, b);
  cairo_rectangle(cr, 0, 0, tile_w, tile_h);
  cairo_fill(cr);

  return success;
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col G_GNUC_UNUSED,
                      int64_t tile_row G_GNUC_UNUSED,
                      void *data,
                      void *arg G_GNUC_UNUSED,
                      GError **err) {
  struct tile *tile = data;
  bool success = true;

  int iw = tile->image->width;
  int ih = tile->image->height;

  // cache
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level,
                                            tile->image->imageno,
                                            0,
                                            &cache_entry);

  if (!tiledata) {
    tiledata = read_image(osr, tile->image, iw, ih, err);
    if (tiledata == NULL) {
      return false;
    }
    _openslide_cache_put(osr->cache,
                         level, tile->image->imageno, 0,
                         tiledata,
                         iw * ih * 4,
                         &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
      cairo_image_surface_create_for_data((unsigned char *)tiledata,
                                          CAIRO_FORMAT_RGB24,
                                          iw, ih, iw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return success;
}

static bool paint_region(openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct level *l = (struct level *)level;

  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / level->downsample,
                                      y / level->downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops intemedic_ops = {
    .paint_region = paint_region,
    .destroy = destroy,
};

static bool intemedic_tron_detect(const char *filename G_GNUC_UNUSED,
                                  struct _openslide_tifflike *tl,
                                  GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // verify filename
  if (!g_str_has_suffix(filename, TRON_EXT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not have %s extension", TRON_EXT);
    return false;
  }

  // verify existence
  GError *tmp_err = NULL;
  if (!_openslide_fexists(filename, &tmp_err)) {
    if (tmp_err != NULL) {
      g_propagate_prefixed_error(err, tmp_err, "Testing whether file exists: ");
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "File does not exist");
    }
    return false;
  }

  return true;
}

static char *read_string_from_file(struct _openslide_file *f, int len) {
  g_autofree char *str = g_malloc(len + 1);
  str[len] = '\0';

  if (!_openslide_fread_exact(f, str, len, NULL)) {
    return NULL;
  }
  return g_steal_pointer(&str);
}

static bool read_le_uint16_from_file_with_result(struct _openslide_file *f,
                                                 uint16_t *OUT) {
  if (!_openslide_fread_exact(f, OUT, 2, NULL)) {
    return false;
  }

  *OUT = GUINT16_FROM_LE(*OUT);
  // g_debug("%d", i);

  return true;
}

static uint16_t read_le_uint16_from_file(struct _openslide_file *f) {
  uint16_t i;

  if (!read_le_uint16_from_file_with_result(f, &i)) {
    // -1 means error
    i = -1;
  }

  return i;
}

static bool read_le_uint32_from_file_with_result(struct _openslide_file *f,
                                                 uint32_t *OUT) {
  if (!_openslide_fread_exact(f, OUT, 4, NULL)) {
    return false;
  }

  *OUT = GUINT32_FROM_LE(*OUT);
  // g_debug("%d", i);

  return true;
}

static uint32_t read_le_uint32_from_file(struct _openslide_file *f) {
  uint32_t i;

  if (!read_le_uint32_from_file_with_result(f, &i)) {
    // -1 means error
    i = -1;
  }

  return i;
}

static bool read_le_uint64_from_file_with_result(struct _openslide_file *f,
                                                 uint64_t *OUT) {
  if (!_openslide_fread_exact(f, OUT, 8, NULL)) {
    return false;
  }

  *OUT = GUINT64_FROM_LE(*OUT);
  // g_debug("%d", i);

  return true;
}

static uint64_t read_le_uint64_from_file(struct _openslide_file *f) {
  uint64_t i;

  if (!read_le_uint64_from_file_with_result(f, &i)) {
    // -1 means error
    i = -1;
  }

  return i;
}

static void insert_tile(struct level *l,
                        struct image *image,
                        double pos_x, double pos_y,
                        int tile_x, int tile_y,
                        int tile_w, int tile_h,
                        int zoom_level) {
  // increment image refcount
  image->refcount++;

  // generate tile
  struct tile *tile = g_new0(struct tile, 1);
  tile->image = image;

  // compute offset
  double offset_x = pos_x - (tile_x * l->base.tile_w);
  double offset_y = pos_y - (tile_y * l->base.tile_h);

  // insert
  _openslide_grid_tilemap_add_tile(l->grid,
                                   tile_x, tile_y,
                                   offset_x, offset_y,
                                   tile_w, tile_h,
                                   tile);

  if (!true) {
    g_debug("zoom %d, tile %d %d, pos %.10g %.10g, offset %.10g %.10g",
            zoom_level, tile_x, tile_y, pos_x, pos_y, offset_x, offset_y);
  }
}

// by calling this, we are using local header _storedEntryNameBytes.Length and extraFieldLength
// to find start of data, but still using central directory size information
static bool try_skip_block(struct _openslide_file *f,
                           struct ZipArchiveEntry *entry,
                           uint16_t *filenameLength,
                           GError **err) {
  uint32_t SignatureConstant = 0x04034B50;
  if (!_openslide_fseek(f, entry->_offsetOfLocalHeader, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to local file header");
    return false;
  }
  const int OffsetToFilenameLength = 22; // from the point after the signature

  if (read_le_uint32_from_file(f) != SignatureConstant)
    return false;

  if (_openslide_fsize(f, err) < _openslide_ftell(f, err) + OffsetToFilenameLength)
    return false;

  if (!_openslide_fseek(f, OffsetToFilenameLength, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek to file name length");
    return false;
  }

  *filenameLength = read_le_uint16_from_file(f);
  uint16_t extraFieldLength = read_le_uint16_from_file(f);

  if (_openslide_fsize(f, err) < _openslide_ftell(f, err) + *filenameLength + extraFieldLength)
    return false;

  if (!_openslide_fseek(f, *filenameLength + extraFieldLength, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek to compressed data");
    return false;
  }
  return true;
}

static bool process_local_files(struct _openslide_file *f,
                                uint64_t numberOfEntries,
                                struct ZipArchiveEntry **entries,
                                int zoom_levels,
                                struct level **levels,
                                GError **err) {
  int32_t image_number = 0;
  for (uint64_t i = 0; i < numberOfEntries; i++) {
    struct ZipArchiveEntry *entry = entries[i];
    if (strcmp(entry->_storedEntryName, MetadataFileName) == 0 ||
        strcmp(entry->_storedEntryName, LabelFileName) == 0 ||
        strcmp(entry->_storedEntryName, MacroFileName) == 0 ||
        strcmp(entry->_storedEntryName, SampleFileName) == 0 ||
        strcmp(entry->_storedEntryName, BlankFileName) == 0)
      continue;

    char *tiledatafilename = entry->_storedEntryName;

    uint16_t filenameLength;
    if (!try_skip_block(f, entry, &filenameLength, err)) {
      g_prefix_error(err, "A local file header is corrupt: ");
      return false;
    }

    entry->_storedOffsetOfCompressedData = _openslide_ftell(f, err);

    uint64_t compressed_size = entry->_compressedSize;
    uint64_t uncompressed_size = entry->_uncompressedSize;
    uint64_t offset = entry->_storedOffsetOfCompressedData;
    if (compressed_size == 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Length is zero");
      break;
    }

    if (offset && !_openslide_fseek(f, offset, SEEK_SET, err)) {
      g_prefix_error(err, "Cannot seek to offset: ");
      break;
    }

    int32_t tile_col = 0;
    int32_t tile_row = 0;
    int32_t zoom_level = 0;

    // spilt filename
    char filename[filenameLength + 1];
    strcpy(filename, tiledatafilename);
    char *temp = strtok(filename, "\\");
    int j = 0;

    while (temp) {
      if (j == 0) {
        sscanf(temp, "%d", &zoom_level);
      } else if (j == 2) {
        sscanf(temp, "%d", &tile_row);
      } else if (j == 3) {
        temp = strtok(temp, ".");
        sscanf(temp, "%d", &tile_col);
      }
      temp = strtok(NULL, "\\");
      j++;
    }

    if (zoom_level < 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "zoom level < 0");
      return false;
    } else if (zoom_level >= zoom_levels) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "zoom level >= zoom levels");
      return false;
    }

    struct level *l = levels[zoom_level];
    int64_t tile_w = l->base.tile_w;
    int64_t tile_h = l->base.tile_h;

    // position in this level
    int32_t pos_x = tile_w * tile_col;
    int32_t pos_y = tile_h * tile_row;

    // populate the image structure
    g_autoptr(image) image = g_new0(struct image, 1);
    image->start_in_file = offset;
    image->compressed_size = compressed_size;
    image->uncompressed_size = uncompressed_size;
    image->imageno = image_number++;
    image->refcount = 1;
    image->width = tile_w;
    image->height = tile_h;

    // start processing 1 image into 1 tile
    // increments image refcount
    insert_tile(l, image,
                pos_x, pos_y,
                pos_x / l->base.tile_w,
                pos_y / l->base.tile_h,
                tile_w, tile_h,
                zoom_level);
  }

  return true;
}

// Returns true if we are out of bytes
static bool seek_backwards_and_read(struct _openslide_file *f, void *buf, int buflen, int *bufferPointer, GError **err) {
  if (_openslide_ftell(f, err) >= buflen) {
    if (!_openslide_fseek(f, -buflen, SEEK_CUR, err))
      return false;
    if (!_openslide_fread_exact(f, buf, buflen, err))
      return false;
    if (!_openslide_fseek(f, -buflen, SEEK_CUR, err))
      return false;
    *bufferPointer = buflen - 1;
    return false;
  } else {
    int bytesToRead = _openslide_ftell(f, err);
    if (!_openslide_fseek(f, 0, SEEK_SET, err))
      return false;
    if (!_openslide_fread_exact(f, buf, bytesToRead, err))
      return false;
    if (!_openslide_fseek(f, 0, SEEK_SET, err))
      return false;
    *bufferPointer = bytesToRead - 1;
    return true;
  }
}

// Assumes all bytes of signatureToFind are non zero, looks backwards from current position in stream,
// assumes maxBytesToRead is positive, ensures to not read beyond the provided max number of bytes,
// if the signature is found then returns true and positions stream at first byte of signature
// if the signature is not found, returns false
static bool seek_backwards_2_signature(struct _openslide_file *f, uint32_t signatureToFind, int maxBytesToRead, GError **err) {
  g_assert(signatureToFind != 0);
  g_assert(maxBytesToRead > 0);

  int bufferPointer = 0;
  uint32_t currentSignature = 0;
  int BackwardsSeekingBufferSize = 32;
  char buffer[BackwardsSeekingBufferSize];

  bool outOfBytes = false;
  bool signatureFound = false;

  int bytesRead = 0;
  while (!signatureFound && !outOfBytes && bytesRead <= maxBytesToRead) {
    outOfBytes = seek_backwards_and_read(f, buffer, BackwardsSeekingBufferSize, &bufferPointer, err);

    g_assert(bufferPointer < BackwardsSeekingBufferSize);

    while (bufferPointer >= 0 && !signatureFound) {
      currentSignature = (currentSignature << 8) | ((uint32_t)buffer[bufferPointer]);
      if (currentSignature == signatureToFind)
        signatureFound = true;
      else
        bufferPointer--;
    }

    bytesRead += BackwardsSeekingBufferSize;
  }

  if (!signatureFound)
    return false;
  else {
    if (!_openslide_fseek(f, bufferPointer, SEEK_CUR, err)) {
      return false;
    }
    return true;
  }
}

static enum CompressionLevel map_compression_level(enum BitFlagValues generalPurposeBitFlag, enum CompressionMethodValues compressionMethod) {
  // Information about the Deflate compression option is stored in bits 1 and 2 of the general purpose bit flags.
  // If the compression method is not Deflate, the Deflate compression option is invalid - default to NoCompression.
  if (compressionMethod == C_Deflate || compressionMethod == C_Deflate64) {
    switch ((int)generalPurposeBitFlag & 0x6) {
    case 0:
      return Optimal;
    case 2:
      return SmallestSize;
    case 4:
      return Fastest;
    case 6:
      return Fastest;
    default:
      return Optimal;
    }
  } else {
    return NoCompression;
  }
}

static bool read_central_directory(struct _openslide_file *f,
                                   GPtrArray *entry_array,
                                   uint64_t _centralDirectoryStart,
                                   uint64_t _expectedNumberOfEntries,
                                   uint64_t *numberOfEntries,
                                   GError **err) {
  if (!_openslide_fseek(f, _centralDirectoryStart, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to CD: ");
    return false;
  }

  // uint32_t Mask32Bit = 0xFFFFFFFF;
  // uint16_t Mask16Bit = 0xFFFF;
  *numberOfEntries = 0;
  uint32_t SignatureConstant = 0x02014B50;
  while (read_le_uint32_from_file(f) == SignatureConstant) {
    g_autoptr(ZipCentralDirectoryFileHeader) header = g_new0(struct ZipCentralDirectoryFileHeader, 1);
    // char VersionMadeBySpecification[1];
    // _openslide_fread_exact(f, VersionMadeBySpecification, sizeof(VersionMadeBySpecification), err);
    // header->VersionMadeBySpecification = VersionMadeBySpecification;
    // char VersionMadeByCompatibility[1];
    // _openslide_fread_exact(f, VersionMadeByCompatibility, sizeof(VersionMadeByCompatibility), err);
    // header->VersionMadeByCompatibility = VersionMadeByCompatibility;
    // skip VersionMadeBySpecification and VersionMadeBySpecification, for now
    if (!_openslide_fseek(f, 2, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within CD: ");
      return false;
    }
    header->VersionNeededToExtract = read_le_uint16_from_file(f);
    header->GeneralPurposeBitFlag = read_le_uint16_from_file(f);
    header->CompressionMethod = read_le_uint16_from_file(f);
    header->LastModified = read_le_uint32_from_file(f);
    header->Crc32 = read_le_uint32_from_file(f);
    uint32_t compressedSizeSmall = read_le_uint32_from_file(f);
    uint32_t uncompressedSizeSmall = read_le_uint32_from_file(f);
    header->FilenameLength = read_le_uint16_from_file(f);
    header->ExtraFieldLength = read_le_uint16_from_file(f);
    header->FileCommentLength = read_le_uint16_from_file(f);
    uint16_t diskNumberStartSmall = read_le_uint16_from_file(f);
    header->InternalFileAttributes = read_le_uint16_from_file(f);
    header->ExternalFileAttributes = read_le_uint32_from_file(f);
    uint32_t relativeOffsetOfLocalHeaderSmall = read_le_uint32_from_file(f);

    header->Filename = read_string_from_file(f, header->FilenameLength);

    // bool uncompressedSizeInZip64 = uncompressedSizeSmall == Mask32Bit;
    // bool compressedSizeInZip64 = compressedSizeSmall == Mask32Bit;
    // bool relativeOffsetInZip64 = relativeOffsetOfLocalHeaderSmall == Mask32Bit;
    // bool diskNumberStartInZip64 = diskNumberStartSmall == Mask16Bit;

    uint64_t endExtraFields = _openslide_ftell(f, err) + header->ExtraFieldLength;

    // There are zip files that have malformed ExtraField blocks in which GetJustZip64Block() silently bails out without reading all the way to the end
    // of the ExtraField block. Thus we must force the stream's position to the proper place.
    if (!_openslide_fseek(f, endExtraFields, SEEK_SET, err)) {
      g_prefix_error(err, "Couldn't seek to end of ExtraField block");
      return false;
    }

    g_assert(header->FileCommentLength == 0);

    header->UncompressedSize = uncompressedSizeSmall;
    header->CompressedSize = compressedSizeSmall;
    header->RelativeOffsetOfLocalHeader = relativeOffsetOfLocalHeaderSmall;
    header->DiskNumberStart = diskNumberStartSmall;

    struct ZipArchiveEntry *entry = g_new0(struct ZipArchiveEntry, 1);
    g_ptr_array_add(entry_array, entry);

    entry->_originallyInArchive = true;
    entry->_diskNumberStart = header->DiskNumberStart;
    // entry->_versionMadeByPlatform = (enum ZipVersionMadeByPlatform)header->VersionMadeByCompatibility;
    // entry->_versionMadeBySpecification = (enum ZipVersionNeededValues)header->VersionMadeBySpecification;
    entry->_versionToExtract = (enum ZipVersionNeededValues)header->VersionNeededToExtract;
    entry->_generalPurposeBitFlag = (enum BitFlagValues)header->GeneralPurposeBitFlag;
    entry->_isEncrypted = (entry->_generalPurposeBitFlag & IsEncrypted) != 0;
    if ((enum CompressionMethodValues)header->CompressionMethod == C_Deflate) {
      if (entry->_versionToExtract < Z_Deflate) {
        entry->_versionToExtract = Z_Deflate;
      }
      if (entry->_versionMadeBySpecification < Z_Deflate) {
        entry->_versionMadeBySpecification = Z_Deflate;
      }
    } else if ((enum CompressionMethodValues)header->CompressionMethod == C_Deflate64) {
      if (entry->_versionToExtract < Z_Deflate64) {
        entry->_versionToExtract = Z_Deflate64;
      }
      if (entry->_versionMadeBySpecification < Z_Deflate64) {
        entry->_versionMadeBySpecification = Z_Deflate64;
      }
    }
    entry->_storedCompressionMethod = (enum CompressionMethodValues)header->CompressionMethod;
    entry->_compressedSize = header->CompressedSize;
    entry->_uncompressedSize = header->UncompressedSize;
    entry->_externalFileAttr = header->ExternalFileAttributes;
    entry->_offsetOfLocalHeader = header->RelativeOffsetOfLocalHeader;
    // we don't know this yet: should be _offsetOfLocalHeader + 30 + _storedEntryNameBytes.Length + extrafieldlength
    // but entryname/extra length could be different in LH
    entry->_crc32 = header->Crc32;

    entry->_currentlyOpenForWrite = false;
    entry->_everOpenedForWrite = false;
    entry->_storedEntryName = g_malloc(header->FilenameLength + 1);
    strcpy(entry->_storedEntryName, header->Filename);
    entry->_compressionLevel = map_compression_level(entry->_generalPurposeBitFlag, entry->_storedCompressionMethod);

    (*numberOfEntries)++;
  }

  if (*numberOfEntries != _expectedNumberOfEntries) {
    g_prefix_error(err, "Number of entries expected in End Of Central Directory does not correspond to number of entries in Central Directory.");
    return false;
  }

  return true;
}

// This function reads all the EOCD stuff it needs to find the offset to the start of the central directory
// This offset gets put in _centralDirectoryStart and the number of this disk gets put in _numberOfThisDisk
// Also does some verification that this isn't a split/spanned archive
// Also checks that offset to CD isn't out of bounds
static bool read_end_of_central_directory(struct _openslide_file *f,
                                          uint64_t *_centralDirectoryStart,
                                          uint64_t *_expectedNumberOfEntries,
                                          GError **err) {
  // This seeks backwards almost to the beginning of the EOCD, one byte after where the signature would be
  // located if the EOCD had the minimum possible size (no file zip comment)
  int SizeOfBlockWithoutSignature = 18; // This is the minimum possible size, assuming the zip file comments variable section is empty
  if (!_openslide_fseek(f, -SizeOfBlockWithoutSignature, SEEK_END, err))
    g_prefix_error(err, "Couldn't seek to EOCD: ");

  uint32_t SignatureConstant = 0x06054B50;
  int SignatureSize = sizeof(uint32_t);
  int ZipFileCommentMaxLength = 65535;

  // If the EOCD has the minimum possible size (no zip file comment), then exactly the previous 4 bytes will contain the signature
  // But if the EOCD has max possible size, the signature should be found somewhere in the previous 64K + 4 bytes
  if (!seek_backwards_2_signature(f,
                                  SignatureConstant,
                                  ZipFileCommentMaxLength + SignatureSize, err)) {
    g_prefix_error(err, "Couldn't seek to EOCD: ");
    return false;
  }

  uint64_t eocdStart = _openslide_ftell(f, err);

  g_autoptr(ZipEndOfCentralDirectoryBlock) eocd = g_new0(struct ZipEndOfCentralDirectoryBlock, 1);
  if (read_le_uint32_from_file(f) != SignatureConstant) {
    g_prefix_error(err, "Couldn't find any SignatureConstant");
    return false;
  }
  // read the EOCD
  eocd->Signature = SignatureConstant;
  eocd->NumberOfThisDisk = read_le_uint16_from_file(f);
  eocd->NumberOfTheDiskWithTheStartOfTheCentralDirectory = read_le_uint16_from_file(f);
  eocd->NumberOfEntriesInTheCentralDirectoryOnThisDisk = read_le_uint16_from_file(f);
  eocd->NumberOfEntriesInTheCentralDirectory = read_le_uint16_from_file(f);
  eocd->SizeOfCentralDirectory = read_le_uint32_from_file(f);
  eocd->OffsetOfStartOfCentralDirectoryWithRespectToTheStartingDiskNumber = read_le_uint32_from_file(f);

  uint16_t commentLength = read_le_uint16_from_file(f);
  g_assert(commentLength == 0);

  if (eocd->NumberOfThisDisk != eocd->NumberOfTheDiskWithTheStartOfTheCentralDirectory) {
    g_prefix_error(err, "Split or spanned archives are not supported.");
    return false;
  }

  // uint32_t _numberOfThisDisk = eocd->NumberOfThisDisk;
  *_centralDirectoryStart = eocd->OffsetOfStartOfCentralDirectoryWithRespectToTheStartingDiskNumber;

  if (eocd->NumberOfEntriesInTheCentralDirectory != eocd->NumberOfEntriesInTheCentralDirectoryOnThisDisk) {
    g_prefix_error(err, "Split or spanned archives are not supported.");
    return false;
  }

  *_expectedNumberOfEntries = eocd->NumberOfEntriesInTheCentralDirectory;

  // Tries to find the Zip64 End of Central Directory Locator, then the Zip64 End of Central Directory, assuming the
  // End of Central Directory block has already been found, as well as the location in the stream where the EOCD starts.

  uint32_t Mask32Bit = 0xFFFFFFFF;
  uint16_t Mask16Bit = 0xFFFF;
  SizeOfBlockWithoutSignature = 16;
  SignatureConstant = 0x07064B50;
  SignatureSize = sizeof(uint32_t);
  // Only bother looking for the Zip64-EOCD stuff if we suspect it is needed because some value is FFFFFFFFF
  // because these are the only two values we need, we only worry about these
  // if we don't find the Zip64-EOCD, we just give up and try to use the original values
  if (eocd->NumberOfThisDisk == Mask16Bit ||
      eocd->OffsetOfStartOfCentralDirectoryWithRespectToTheStartingDiskNumber == Mask32Bit ||
      eocd->NumberOfEntriesInTheCentralDirectory == Mask16Bit) {
    // Read Zip64 End of Central Directory Locator

    // This seeks forwards almost to the beginning of the Zip64-EOCDL, one byte after where the signature would be located
    if (!_openslide_fseek(f, eocdStart - SizeOfBlockWithoutSignature, SEEK_SET, err))
      g_prefix_error(err, "Couldn't seek to EOCD: ");

    // Exactly the previous 4 bytes should contain the Zip64-EOCDL signature
    // if we don't find it, assume it doesn't exist and use data from normal EOCD
    if (seek_backwards_2_signature(f,
                                   SignatureConstant,
                                   SignatureSize, err)) {
      // use locator to get to Zip64-EOCD
      g_autoptr(Zip64EndOfCentralDirectoryLocator) locator = g_new0(struct Zip64EndOfCentralDirectoryLocator, 1);
      if (read_le_uint32_from_file(f) != SignatureConstant) {
        g_prefix_error(err, "Couldn't find any SignatureConstant");
        return false;
      }

      locator->NumberOfDiskWithZip64EOCD = read_le_uint32_from_file(f);
      locator->OffsetOfZip64EOCD = read_le_uint64_from_file(f);
      locator->TotalNumberOfDisks = read_le_uint32_from_file(f);

      if (locator->OffsetOfZip64EOCD > LONG_MAX) {
        g_prefix_error(err, "Offset to Zip64 End Of Central Directory record cannot be held in an Int64.");
        return false;
      }

      uint64_t zip64EOCDOffset = locator->OffsetOfZip64EOCD;

      if (!_openslide_fseek(f, zip64EOCDOffset, SEEK_CUR, err))
        g_prefix_error(err, "Couldn't seek to EOCD: ");

      // Read Zip64 End of Central Directory Record

      g_autoptr(Zip64EndOfCentralDirectoryRecord) record = g_new0(struct Zip64EndOfCentralDirectoryRecord, 1);
      SignatureConstant = 0x06064B50;
      if (read_le_uint32_from_file(f) != SignatureConstant) {
        g_prefix_error(err, "Couldn't find any SignatureConstant");
        return false;
      }

      record->SizeOfThisRecord = read_le_uint64_from_file(f);
      record->VersionMadeBy = read_le_uint16_from_file(f);
      record->VersionNeededToExtract = read_le_uint16_from_file(f);
      record->NumberOfThisDisk = read_le_uint32_from_file(f);
      record->NumberOfDiskWithStartOfCD = read_le_uint32_from_file(f);
      record->NumberOfEntriesOnThisDisk = read_le_uint64_from_file(f);
      record->NumberOfEntriesTotal = read_le_uint64_from_file(f);
      record->SizeOfCentralDirectory = read_le_uint64_from_file(f);
      record->OffsetOfCentralDirectory = read_le_uint64_from_file(f);

      // _numberOfThisDisk = record->NumberOfThisDisk;

      if (record->NumberOfEntriesTotal > LONG_MAX) {
        g_prefix_error(err, "Number of Entries cannot be held in an Int64.");
        return false;
      }

      if (record->OffsetOfCentralDirectory > LONG_MAX) {
        g_prefix_error(err, "Offset to Central Directory cannot be held in an Int64.");
        return false;
      }

      if (record->NumberOfEntriesTotal != record->NumberOfEntriesOnThisDisk) {
        g_prefix_error(err, "Split or spanned archives are not supported.");
        return false;
      }

      *_expectedNumberOfEntries = record->NumberOfEntriesTotal;
      *_centralDirectoryStart = record->OffsetOfCentralDirectory;
    }
  }

  if (*_centralDirectoryStart > (uint64_t)_openslide_fsize(f, err)) {
    g_prefix_error(err, "Offset to Central Directory cannot be held in an Int64.");
    return false;
  }

  return true;
}

static bool intemedic_tron_open(openslide_t *osr, const char *filename,
                                struct _openslide_tifflike *tl G_GNUC_UNUSED,
                                struct _openslide_hash *quickhash1 G_GNUC_UNUSED, GError **err) {
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }

  // read zip archive
  uint64_t _expectedNumberOfEntries;
  uint64_t _centralDirectoryStart;
  if (!read_end_of_central_directory(f, &_centralDirectoryStart, &_expectedNumberOfEntries, err)) {
    g_prefix_error(err, "Central Directory corrupt: ");
    return false;
  }
  uint64_t numberOfEntries;
  g_autoptr(GPtrArray) entry_array =
      g_ptr_array_new_with_free_func((GDestroyNotify)destroy_entry);
  if (!read_central_directory(f, entry_array, _centralDirectoryStart, _expectedNumberOfEntries, &numberOfEntries, err)) {
    g_prefix_error(err, "Central Directory corrupt: ");
    return false;
  }

  // add properties
  for (uint64_t i = 0; i < numberOfEntries; i++) {
    struct ZipArchiveEntry *entry = entry_array->pdata[i];
    if (strcmp(entry->_storedEntryName, MetadataFileName) == 0) {
      uint16_t filenameLength;
      if (!try_skip_block(f, entry, &filenameLength, err)) {
        g_prefix_error(err, "A local file header is corrupt: ");
        return false;
      }

      entry->_storedOffsetOfCompressedData = _openslide_ftell(f, err);

      g_autofree void *uncompressed = decode_item(f,
                                                  entry->_compressedSize,
                                                  entry->_uncompressedSize,
                                                  entry->_storedOffsetOfCompressedData,
                                                  err);
      if (!uncompressed) {
        g_prefix_error(err, "Error decompressing slideMetadata buffer: ");
        return false;
      }

      // read header
      uint8_t header[4];
      memcpy(header, uncompressed, 4);
      // Check TRON header
      if (header[0] != 'T' || header[1] != 'R' || header[2] != 'O' || header[3] != 'N') {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unsupported file: %c%c%c%c", header[0], header[1], header[2], header[3]);
        return false;
      }

      uint8_t *slideMetadata = (uint8_t *)uncompressed;
      uint32_t version = (uint32_t)(slideMetadata[4] | (slideMetadata[5] << 8) | (slideMetadata[6] << 16) | (slideMetadata[7] << 24));
      // support version 4 only, for now
      if (!SupportLegacy && version <= 3) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unsupported file version: %d", version);
        return false;
      }

      // deserialize body
      uint8_t first[32];
      for (int i = 8; i < 40; i++)
        first[i - 8] = slideMetadata[i];

      int blockSize = 128;
      int num = blockSize / 8;

      int dataLength = entry->_uncompressedSize - 40;
      uint8_t data[dataLength];
      for (uint64_t i = 40; i < entry->_uncompressedSize; i++)
        data[i - 40] = slideMetadata[i];

      uint8_t salt[num];
      for (int i = 0; i < num; i++)
        salt[i] = data[i];

      uint8_t array[num];
      for (int i = num; i < num * 2; i++)
        array[i - num] = data[i];

      uint8_t array2[dataLength - num * 2];
      for (int i = num * 2; i < dataLength; i++)
        array2[i - num * 2] = data[i];

      tRfc2898DeriveBytes *rfc2898DeriveBytes = _openslide_Rfc2898DeriveBytes_Init((const unsigned char *)CypherKey, (uint32_t)strlen(CypherKey), salt, num);
      uint8_t *bytes = _openslide_Rfc2898DeriveBytes_GetBytes(rfc2898DeriveBytes, 32);
      free(rfc2898DeriveBytes);
      int cipherLen = sizeof(array2);
      _openslide_aes_decode_cbc(AES_CYPHER_256, array2, cipherLen, bytes, array);
      free(bytes);

      // PKCS#7 padding;
      // this is a byte padding and common with CBC mode,
      // let x be the remaining byte, then fill the rest with x as byte encoded.
      // For example, if two bytes needed to fill then pad two times 0x02.
      // This also has a special case, that what if the last block is full, the create a new block and fill with 0x1F 16-times
      uint8_t x = array2[cipherLen - 1];
      g_assert(x >= 0x01 && x <= 0x1F);

      // hash check
      uint8_t second[SHA256_DIGEST_LENGTH];
      EVP_MD_CTX *mdctx = EVP_MD_CTX_create();
      const EVP_MD *md = EVP_sha256();
      int clearLen = cipherLen - x;
      g_assert(cipherLen == (clearLen + 16 - (clearLen % 16)));
      EVP_DigestInit_ex(mdctx, md, NULL); // ex or ex2
      EVP_DigestUpdate(mdctx, array2, clearLen);
      EVP_DigestFinal_ex(mdctx, second, 0);
      EVP_MD_CTX_destroy(mdctx);

      if (strncmp((const char *)first, (const char *)second, SHA256_DIGEST_LENGTH) != 0) {
        g_prefix_error(err, "hash mismatch: ");
        break;
      }

      json_object *slideMetadataJson = json_tokener_parse((const char *)array2);
      json_object *slideMetadataObj = json_object_object_get(slideMetadataJson, SlideMetadata);

      json_object_object_foreach(slideMetadataObj, key, val) {
        const char *value = json_object_to_json_string(val);
        if (strcmp(key, KEY_MINIMUM_LOD_LEVEL) == 0 || strcmp(key, KEY_MAXIMUM_LOD_LEVEL) == 0 || strcmp(key, KEY_MAXIMUM_ZOOM_LEVEL) == 0 ||
            strcmp(key, KEY_HORIZONTAL_TILE_COUNT) == 0 || strcmp(key, KEY_VERTICAL_TILE_COUNT) == 0 ||
            strcmp(key, KEY_HORIZONTAL_RESOLUTION) == 0 || strcmp(key, KEY_VERTICAL_RESOLUTION) == 0) {
          g_hash_table_insert(osr->properties,
                              g_strdup_printf("intemedic.%s", key),
                              g_strdup(value));
        } else if (strcmp(key, KEY_BACKGROUND_COLOR) == 0) {
          char bg_value[strlen(value) + 1];
          strcpy(bg_value, value);
          char *temp = strtok(bg_value, ",");
          uint8_t r;
          uint8_t g;
          uint8_t b;
          int j = 0;
          while (temp) {
            if (j == 0) {
              sscanf(temp + 1, "%hhu", &r);
            } else if (j == 1) {
              sscanf(temp, "%hhu", &g);
              temp = strtok(temp, ",");
              sscanf(temp, "%hhu", &b);
            }
            temp = strtok(NULL, "\\");
            j++;
          }
          int64_t bg = (r << 16) | (g << 8) | b;
          g_hash_table_insert(osr->properties,
                              g_strdup_printf("intemedic.%s", key),
                              g_strdup_printf("%" PRId64, bg));
        } else if (strcmp(key, KEY_TILE_SIZE) == 0) {
          char ts_value[strlen(value) + 1];
          strcpy(ts_value, value);
          char *token = strtok(ts_value, ",");
          int32_t tile_size = 0;
          if (token != NULL) {
            sscanf(token + 1, "%d", &tile_size);
            g_hash_table_insert(osr->properties,
                                g_strdup_printf("intemedic.%s", key),
                                g_strdup(token + 1));
          }
        } else if (strcmp(key, KEY_ADDITIONAL_DATA) == 0) {
          json_object_object_foreach(val, key1, val1) {
            const char *value1 = json_object_to_json_string(val1);
            if (strcmp(key1, KEY_SCAN_DATE_UTC) == 0 ||
                strcmp(key1, KEY_SCAN_TIME) == 0 ||
                strcmp(key, KEY_RESAMPLE_FACTOR) == 0 ||
                strcmp(key1, KEY_SCANNER_MODEL) == 0) {
              g_hash_table_insert(osr->properties,
                                  g_strdup_printf("intemedic.%s", key1),
                                  g_strdup(value1));
            }
          }
        }
      }

      json_object_put(slideMetadataJson);
      g_free(g_steal_pointer(&uncompressed));
    } else if (strcmp(entry->_storedEntryName, LabelFileName) == 0 ||
               strcmp(entry->_storedEntryName, MacroFileName) == 0) {
      const char *associated_image_name = strcmp(entry->_storedEntryName, LabelFileName) == 0 ? "label" : "macro";

      uint16_t filenameLength;
      if (!try_skip_block(f, entry, &filenameLength, err)) {
        g_prefix_error(err, "A local file header is corrupt: ");
        return false;
      }

      entry->_storedOffsetOfCompressedData = _openslide_ftell(f, err);

      void *uncompressed = decode_item(f,
                                       entry->_compressedSize,
                                       entry->_uncompressedSize,
                                       entry->_storedOffsetOfCompressedData,
                                       err);
      if (!uncompressed) {
        g_prefix_error(err, "Error decompressing associated image buffer: %s", associated_image_name);
        return false;
      }

      if (!_openslide_jpeg_add_associated_image_2(osr, associated_image_name, filename, uncompressed, entry->_uncompressedSize, err)) {
        g_prefix_error(err, "Couldn't read associated image: %s", associated_image_name);
        return false;
      }
    } else {
      // TODO
    }
  }

  char *tiles_across_str = g_hash_table_lookup(osr->properties, "intemedic.HorizontalTileCount");
  char *tiles_down_str = g_hash_table_lookup(osr->properties, "intemedic.VerticalTileCount");
  int64_t tiles_across;
  int64_t tiles_down;
  if (!_openslide_parse_int64(tiles_across_str, &tiles_across)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid HorizontalTileCount");
    return false;
  }
  if (!_openslide_parse_int64(tiles_down_str, &tiles_down)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid VerticalTileCount");
    return false;
  }

  char *tile_size_str = g_hash_table_lookup(osr->properties, "intemedic.TileSize");
  int64_t tile_size;
  if (!_openslide_parse_int64(tile_size_str, &tile_size)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid TileSize");
    return false;
  }

  // calculate base dimensions
  int64_t base_h = tiles_down * tile_size;
  int64_t base_w = tiles_across * tile_size;

  char *MinimumLODLevel_str = g_hash_table_lookup(osr->properties, "intemedic.MinimumLODLevel");
  char *MaximumLODLevel_str = g_hash_table_lookup(osr->properties, "intemedic.MaximumLODLevel");
  int64_t MinimumLODLevel = 0;
  int64_t MaximumLODLevel = 0;
  if (!_openslide_parse_int64(MinimumLODLevel_str, &MinimumLODLevel)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid MinimumLODLevel");
    return false;
  }
  if (!_openslide_parse_int64(MaximumLODLevel_str, &MaximumLODLevel)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid MaximumLODLevel");
    return false;
  }
  // calculate level count
  int32_t zoom_levels = (MaximumLODLevel - MinimumLODLevel) + 1;

  // add properties
  char *bg_str = g_hash_table_lookup(osr->properties, "intemedic.BackgroundColor");
  int64_t bg;
  uint8_t r;
  uint8_t g;
  uint8_t b;
  if (_openslide_parse_int64(bg_str, &bg)) {
    r = (bg >> 16) & 0xFF;
    g = (bg >> 8) & 0xFF;
    b = bg & 0xFF;
    _openslide_set_background_color_prop(osr,
                                         r,
                                         g,
                                         b);
  }

  // set MPP and objective power
  _openslide_duplicate_double_prop(osr, "intemedic.MaximumZoomLevel",
                                   OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  _openslide_duplicate_double_prop(osr, "intemedic.HorizontalResolution",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(osr, "intemedic.VerticalResolution",
                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);

  // set up level dimensions and such
  g_autoptr(GPtrArray) level_array =
      g_ptr_array_new_with_free_func((GDestroyNotify)destroy_level);
  int64_t downsample = 1;
  for (int i = 0; i < zoom_levels; i++) {
    // ensure downsample is > 0 and a power of 2
    if (downsample <= 0 || (downsample & (downsample - 1))) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Invalid downsample %" PRId64, downsample);
      return false;
    }

    if (i == 0)
      downsample = 1;
    else
      downsample *= 2;

    struct level *l = g_new0(struct level, 1);
    g_ptr_array_add(level_array, l);

    l->base.downsample = downsample;
    l->base.tile_w = (double)tile_size;
    l->base.tile_h = (double)tile_size;

    l->base.w = base_w / l->base.downsample;
    if (l->base.w == 0)
      l->base.w = 1;
    l->base.h = base_h / l->base.downsample;
    if (l->base.h == 0)
      l->base.h = 1;

    l->grid = _openslide_grid_create_tilemap_2(osr,
                                               tile_size,
                                               tile_size,
                                               read_tile, read_missing_tile, tile_free);
  }

  // build up the tiles
  if (!process_local_files(f,
                           numberOfEntries,
                           (struct ZipArchiveEntry **)entry_array->pdata,
                           zoom_levels,
                           (struct level **)level_array->pdata,
                           err)) {
    return false;
  }

  // build ops data
  struct intemedic_ops_data *data = g_new0(struct intemedic_ops_data, 1);
  data->filename = g_strdup(filename);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->level_count = zoom_levels;
  osr->levels = (struct _openslide_level **)
      g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &intemedic_ops;

  return true;
}

const struct _openslide_format _openslide_format_intemedic = {
    .name = "intemedic-tron",
    .vendor = "intemedic",
    .detect = intemedic_tron_detect,
    .open = intemedic_tron_open,
};
