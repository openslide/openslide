/*
 * TeksqRay (sdpc, dyqx) support
 */

#include "openslide-private.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-hevc.h"

#include <math.h>

static const char SDPC_EXT[] = ".sdpc";
static const char DYQX_EXT[] = ".dyqx";

static const uint16_t PIC_HEAD_FLAG = 0x5153;
static const uint16_t PERSON_INFO_FLAG = 0x4950;
static const uint16_t EXTRA_INFO_FLAG = 0x4945;
static const uint16_t MACROGRAPH_INFO_FLAG = 0x494D;
static const uint16_t PIC_INFO_FLAG = 0x4649;

enum CompressMode {
  Jpeg = 0,
  Bmp = 1,
  Png = 2,
  Tiff = 3,
  Hevc = 4
};

struct PicHead {
  uint16_t flag;
  unsigned char *version;
  uint32_t headSize;
  uint64_t fileSize;
  uint32_t macrograph;
  uint32_t personInfor;
  uint32_t hierarchy;
  uint32_t srcWidth;
  uint32_t srcHeight;
  uint32_t sliceWidth;
  uint32_t sliceHeight;
  uint32_t thumbnailWidth;
  uint32_t thumbnailHeight;
  unsigned char *bpp;
  unsigned char *quality;
  J_COLOR_SPACE colrSpace;
  float scale;
  double ruler;
  uint32_t rate;
  uint64_t extraOffset;
  uint64_t tileOffset;
  enum CompressMode sliceFmt;
  unsigned char *headSpace;
};

static void destroy_picHead(struct PicHead *picHead) {
  g_free(picHead->version);
  g_free(picHead->bpp);
  g_free(picHead->quality);
  g_free(picHead->headSpace);
  g_free(picHead);
}

typedef struct PicHead PicHead;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PicHead, destroy_picHead)

struct PersonInfo {
  uint16_t flag;
  uint32_t inforSize;
  unsigned char *pathologyID;
  unsigned char *name;
  unsigned char *sex;
  unsigned char *age;
  unsigned char *departments;
  unsigned char *hospital;
  unsigned char *submittedSamples;
  unsigned char *clinicalDiagnosis;
  unsigned char *pathologicalDiagnosis;
  unsigned char *reportDate;
  unsigned char *attendingDoctor;
  unsigned char *remark;
  uint64_t nextOffset;
  uint32_t reversed_1;
  uint32_t reversed_2;
  unsigned char *reversed;
};

static void destroy_personInfo(struct PersonInfo *personInfo) {
  g_free(personInfo->pathologyID);
  g_free(personInfo->name);
  g_free(personInfo->sex);
  g_free(personInfo->age);
  g_free(personInfo->departments);
  g_free(personInfo->hospital);
  g_free(personInfo->submittedSamples);
  g_free(personInfo->clinicalDiagnosis);
  g_free(personInfo->pathologicalDiagnosis);
  g_free(personInfo->reportDate);
  g_free(personInfo->attendingDoctor);
  g_free(personInfo->remark);
  g_free(personInfo->reversed);
  g_free(personInfo);
}

typedef struct PersonInfo PersonInfo;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PersonInfo, destroy_personInfo)

struct ExtraInfo {
  uint16_t flag;
  uint32_t inforSize;
  uint64_t nextOffset;
  unsigned char *model;
  float ccmGamma;
  // float[] ccmRgbRate;
  // float[] ccmHsvRate;
  // float[] ccm;
  unsigned char *timeConsuming;
  uint32_t scanTime;
  // uint16_t[] stepTime;
  unsigned char *serial;
  unsigned char *fusionLayer;
  float step;
  uint16_t focusPoint;
  uint16_t validFocusPoint;
  unsigned char *barCode;
  float cameraGamma;
  float cameraExposure;
  float cameraGain;
  int32_t headSpace1;
  int32_t headSpace2;
  unsigned char *objectiveModel;
  unsigned char *reversed;
};

static void destroy_extraInfo(struct ExtraInfo *extraInfo) {
  g_free(extraInfo->model);
  g_free(extraInfo->timeConsuming);
  g_free(extraInfo->serial);
  g_free(extraInfo->fusionLayer);
  g_free(extraInfo->barCode);
  g_free(extraInfo->objectiveModel);
  g_free(extraInfo->reversed);
  g_free(extraInfo);
}

typedef struct ExtraInfo ExtraInfo;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ExtraInfo, destroy_extraInfo)

struct MacrographInfo {
  uint16_t flag;
  uint64_t rgb; // bak?? not used for now
  uint32_t width;
  uint32_t height;
  uint32_t chance; // channel?? not used for now
  uint32_t step;
  uint64_t rgbSize;
  uint64_t encodeSize;
  unsigned char *quality;
  uint64_t nextLayerOffset;
  uint32_t headSpace_1;
  uint32_t headSpace_2;
  unsigned char *headSpace;
};

static void destroy_macrographInfo(struct MacrographInfo *macrographInfo) {
  g_free(macrographInfo->quality);
  g_free(macrographInfo->headSpace);
  g_free(macrographInfo);
}

typedef struct MacrographInfo MacrographInfo;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MacrographInfo, destroy_macrographInfo)

struct PicInfo {
  uint16_t flag;
  uint32_t infoSize;
  uint32_t layer;
  uint32_t sliceNum;
  uint32_t sliceNumX;
  uint32_t sliceNumY;
  uint64_t layerSize;
  uint64_t nextLayerOffset;
  float curScale;
  double ruler;
  uint32_t defaultX;
  uint32_t defaultY;
  unsigned char *bmpFlag;
  unsigned char *headSpace;
};

static void destroy_picInfo(struct PicInfo *picInfo) {
  g_free(picInfo->bmpFlag);
  g_free(picInfo->headSpace);
  g_free(picInfo);
}

typedef struct PicInfo PicInfo;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PicInfo, destroy_picInfo)

struct teksqray_ops_data {
  char *filename;
  enum CompressMode sliceFmt;
};

struct image {
  int64_t start_in_file;
  int32_t length;
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

  int32_t tiles_across;
  int32_t tiles_down;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_free(l);
}

typedef struct level level;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(level, destroy_level)

static void destroy(openslide_t *osr) {
  struct teksqray_ops_data *data = osr->data;

  // levels
  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *) osr->levels[i]);
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

static bool decode_item(struct _openslide_file *f,
                        uint64_t offset,
                        uint64_t length,
                        uint32_t *dest,
                        int w, int h,
                        OpenHevc_Handle openHevcHandle,
                        GError **err) {
  if (length == 0) {
    return false;
  }

  if (offset && !_openslide_fseek(f, offset, SEEK_SET, err)) {
    return false;
  }

  g_autofree void *buf = g_malloc(length);
  if (!_openslide_fread_exact(f, buf, length, err)) {
    return false;
  }

  if (openHevcHandle)
    return _openslide_hevc_decode_buffer(buf, length, (uint8_t *) dest, openHevcHandle, err);
  else
    return _openslide_jpeg_decode_buffer(buf, length, dest, w, h, err);
}

static uint32_t *decode_thumb(struct _openslide_file *f,
                              uint64_t offset,
                              uint64_t length,
                              int w, int h,
                              GError **err) {
  if (length == 0)
    return false;

  if (offset && !_openslide_fseek(f, offset, SEEK_SET, err))
    return false;

  if (!_openslide_fseek(f, 54, SEEK_CUR, err))
    return false;

  g_assert(length - 54 == w * h * 4);
  g_autofree uint32_t *dest = g_malloc(length - 54);
  if (!_openslide_fread_exact(f, dest, length - 54, err))
    return false;

  return g_steal_pointer(&dest);
}

static uint32_t *read_image(openslide_t *osr,
                            struct image *image,
                            int w, int h,
                            GError **err) {
  struct teksqray_ops_data *data = osr->data;
  bool result = false;

  g_autoptr(_openslide_file) f = _openslide_fopen(data->filename, err);
  if (f == NULL) {
    return false;
  }

  g_autofree uint32_t *dest = g_malloc(w * h * 4);

  OpenHevc_Handle openHevcHandle = NULL;
  if (data->sliceFmt == Hevc) {
    // initialize hevc decoder
    openHevcHandle = _openslide_hevc_decompress_init(err);
    if (openHevcHandle == NULL) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't initialize hevc decoder");
      return NULL;
    }
  }

  result = decode_item(f, image->start_in_file, image->length, dest, w, h, openHevcHandle, err);

  if (openHevcHandle) {
    _openslide_hevc_decompress_destroy(openHevcHandle);
  }

  if (!result) {
    return NULL;
  }
  return g_steal_pointer(&dest);
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col G_GNUC_UNUSED,
                      int64_t tile_row G_GNUC_UNUSED,
                      void *data,
                      void *arg G_GNUC_UNUSED,
                      GError **err) {
  struct level *l = (struct level *) level;
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
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
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
  struct level *l = (struct level *) level;

  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / level->downsample,
                                      y / level->downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops teksqray_ops = {
    .paint_region = paint_region,
    .destroy = destroy,
};

static bool teksqray_sdpc_dyqx_detect(const char *filename G_GNUC_UNUSED,
                                      struct _openslide_tifflike *tl,
                                      GError **err) {
                                      // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // verify filename
  if (!g_str_has_suffix(filename, SDPC_EXT) && !g_str_has_suffix(filename, DYQX_EXT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not have %s/%s extension", SDPC_EXT, DYQX_EXT);
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

static unsigned char *read_byte_from_file(struct _openslide_file *f) {
  int len = 1;
  g_autofree unsigned char *str = g_malloc(len + 1);
  str[len] = '\0';

  if (!_openslide_fread_exact(f, str, len, NULL)) {
    return NULL;
  }
  return g_steal_pointer(&str);
}

static unsigned char *read_string_from_file(struct _openslide_file *f, int len) {
  g_autofree unsigned char *str = g_malloc(len + 1);
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

static bool read_le_int32_from_file_with_result(struct _openslide_file *f,
                                                int32_t *OUT) {
  if (!_openslide_fread_exact(f, OUT, 4, NULL)) {
    return false;
  }

  *OUT = GINT32_FROM_LE(*OUT);
  // g_debug("%d", i);

  return true;
}

static int32_t read_le_int32_from_file(struct _openslide_file *f) {
  int32_t i;

  if (!read_le_int32_from_file_with_result(f, &i)) {
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

static bool process_tiles_info_from_header(struct _openslide_file *f,
                                           int64_t seek_location,
                                           int zoom_level,
                                           int tile_count,
                                           int32_t *image_number,
                                           struct level *l,
                                           GError **err) {
  int32_t offset = seek_location + tile_count * 4;
  // read all the data into the list
  for (int i = 0; i < tile_count; i++) {
    int32_t length = read_le_int32_from_file(f);
    if (offset < 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "offset < 0");
      return false;
    }
    if (length < 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "length < 0");
      return false;
    }

    int64_t tile_w = l->base.tile_w;
    int64_t tile_h = l->base.tile_h;

    // position in this level
    int32_t tile_col = i % l->tiles_across;
    int32_t tile_row = i / l->tiles_across;
    int32_t pos_x = tile_w * tile_col;
    int32_t pos_y = tile_h * tile_row;

    // populate the image structure
    g_autoptr(image) image = g_new0(struct image, 1);
    image->start_in_file = offset;
    image->length = length;
    image->imageno = (*image_number)++;
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

    offset += length;
  }

  return true;
}

static bool read_pic_head(struct _openslide_file *f,
                          struct PicHead *picHead,
                          GError **err) {
                          // check flag
  picHead->flag = read_le_uint16_from_file(f);
  if (picHead->flag != PIC_HEAD_FLAG) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported SqPicHead flag: %x", picHead->flag);
    return false;
  }
  picHead->version = read_string_from_file(f, 16);
  picHead->headSize = read_le_uint32_from_file(f);
  picHead->fileSize = read_le_uint64_from_file(f);
  g_assert(picHead->fileSize == _openslide_fsize(f, err));
  picHead->macrograph = read_le_uint32_from_file(f);
  g_assert(picHead->macrograph == 2);
  picHead->personInfor = read_le_uint32_from_file(f);
  g_assert(picHead->personInfor == 1);
  picHead->hierarchy = read_le_uint32_from_file(f);
  picHead->srcWidth = read_le_uint32_from_file(f);
  picHead->srcHeight = read_le_uint32_from_file(f);
  picHead->sliceWidth = read_le_uint32_from_file(f);
  picHead->sliceHeight = read_le_uint32_from_file(f);
  picHead->thumbnailWidth = read_le_uint32_from_file(f);
  picHead->thumbnailHeight = read_le_uint32_from_file(f);
  picHead->bpp = read_byte_from_file(f);
  picHead->quality = read_byte_from_file(f);
  g_autofree unsigned char *colrSpace = read_byte_from_file(f);
  picHead->colrSpace = (J_COLOR_SPACE) (*colrSpace);
  if (!_openslide_fseek(f, 3, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek within SqPicHead: ");
    return false;
  }
  unsigned char scaleBuf[4];
  if (!_openslide_fread_exact(f, scaleBuf, sizeof(scaleBuf), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read scale within SqPicHead");
    return false;
  }
  float *scale = (float *) scaleBuf;
  picHead->scale = *scale;
  unsigned char rulerBuf[8];
  if (!_openslide_fread_exact(f, rulerBuf, sizeof(rulerBuf), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read ruler within SqPicHead");
    return false;
  }
  double *ruler = (double *) rulerBuf;
  picHead->ruler = *ruler;
  picHead->rate = read_le_uint32_from_file(f);
  picHead->extraOffset = read_le_uint64_from_file(f);
  picHead->tileOffset = read_le_uint64_from_file(f);
  g_autofree unsigned char *sliceFmt = read_byte_from_file(f);
  picHead->sliceFmt = (enum CompressMode) (*sliceFmt);
  g_assert(picHead->sliceFmt == Jpeg || picHead->sliceFmt == Hevc);

  // printf("flag : %x \n"
  //        "version : %s \n"
  //        "headSize : %d \n"
  //        "fileSize : %ld \n"
  //        "macrograph : %d \n"
  //        "personInfor : %d \n"
  //        "hierarchy : %d \n"
  //        "srcWidth : %d \n"
  //        "srcHeight : %d \n"
  //        "sliceWidth : %d \n"
  //        "sliceHeight : %d \n"
  //        "thumbnailWidth : %d \n"
  //        "thumbnailHeight : %d \n"
  //        "bpp : %u \n"
  //        "quality : %u \n"
  //        "colorSpace : %d \n"
  //        "scale : %f \n"
  //        "ruler : %f \n"
  //        "rate : %d \n"
  //        "extraOffset : %ld \n"
  //        "tileOffset : %ld \n"
  //        "sliceFormat : %d \n",
  //        picHead->flag,
  //        picHead->version,
  //        picHead->headSize,
  //        picHead->fileSize,
  //        picHead->macrograph,
  //        picHead->personInfor,
  //        picHead->hierarchy,
  //        picHead->srcWidth,
  //        picHead->srcHeight,
  //        picHead->sliceWidth,
  //        picHead->sliceHeight,
  //        picHead->thumbnailWidth,
  //        picHead->thumbnailHeight,
  //        *picHead->bpp,
  //        *picHead->quality,
  //        picHead->colrSpace,
  //        picHead->scale,
  //        picHead->ruler,
  //        picHead->rate,
  //        picHead->extraOffset,
  //        picHead->tileOffset,
  //        picHead->sliceFmt);

  return true;
}

static bool read_person_info(struct _openslide_file *f,
                             struct PersonInfo *personInfo,
                             GError **err) {
                             // check flag
  personInfo->flag = read_le_uint16_from_file(f);
  if (personInfo->flag != PERSON_INFO_FLAG) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported SqPersonInfo flag: %x", personInfo->flag);
    return false;
  }
  personInfo->inforSize = read_le_uint32_from_file(f);
  personInfo->pathologyID = read_string_from_file(f, 64);
  personInfo->name = read_string_from_file(f, 64);
  personInfo->sex = read_byte_from_file(f);
  personInfo->age = read_byte_from_file(f);
  personInfo->departments = read_string_from_file(f, 64);
  personInfo->hospital = read_string_from_file(f, 64);
  personInfo->submittedSamples = read_string_from_file(f, 1024);
  personInfo->clinicalDiagnosis = read_string_from_file(f, 2048);
  personInfo->pathologicalDiagnosis = read_string_from_file(f, 2048);
  personInfo->reportDate = read_string_from_file(f, 64);
  personInfo->attendingDoctor = read_string_from_file(f, 64);
  personInfo->remark = read_string_from_file(f, 1024);
  personInfo->nextOffset = read_le_uint64_from_file(f);
  personInfo->reversed_1 = read_le_uint32_from_file(f);
  g_assert(personInfo->reversed_1 == 0);
  personInfo->reversed_2 = read_le_uint32_from_file(f);
  g_assert(personInfo->reversed_2 == 0);
  personInfo->reversed = read_string_from_file(f, 256);

  // printf("flag : %x \n"
  //        "inforSize : %d \n"
  //        "pathologyID : %s \n"
  //        "name : %s \n"
  //        "sex : %u \n"
  //        "age : %u \n"
  //        "departments : %s \n"
  //        "hospital : %s \n"
  //        "submittedSamples : %s \n"
  //        "clinicalDiagnosis : %s \n"
  //        "pathologicalDiagnosis : %s \n"
  //        "reportDate : %s \n"
  //        "attendingDoctor : %s \n"
  //        "remark : %s \n"
  //        "nextOffset : %ld \n"
  //        "reversed_1 : %d \n"
  //        "reversed_2 : %d \n"
  //        "reversed : %s \n",
  //        personInfo->flag,
  //        personInfo->inforSize,
  //        personInfo->pathologyID,
  //        personInfo->name,
  //        *personInfo->sex,
  //        *personInfo->age,
  //        personInfo->departments,
  //        personInfo->hospital,
  //        personInfo->submittedSamples,
  //        personInfo->clinicalDiagnosis,
  //        personInfo->pathologicalDiagnosis,
  //        personInfo->reportDate,
  //        personInfo->attendingDoctor,
  //        personInfo->remark,
  //        personInfo->nextOffset,
  //        personInfo->reversed_1,
  //        personInfo->reversed_2,
  //        personInfo->reversed);

  return true;
}

static bool read_extra_info(struct _openslide_file *f,
                            struct ExtraInfo *extraInfo,
                            GError **err) {
                            // check flag
  extraInfo->flag = read_le_uint16_from_file(f);
  if (extraInfo->flag != EXTRA_INFO_FLAG) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported SqExtraInfo flag: %x", extraInfo->flag);
    return false;
  }
  extraInfo->inforSize = read_le_uint32_from_file(f);
  extraInfo->nextOffset = read_le_uint64_from_file(f);
  extraInfo->model = read_string_from_file(f, 20);
  unsigned char ccmGammaBuf[4];
  if (!_openslide_fread_exact(f, ccmGammaBuf, sizeof(ccmGammaBuf), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read ccmGamma within SqPicHead");
    return false;
  }
  float *ccmGamma = (float *) ccmGammaBuf;
  extraInfo->ccmGamma = *ccmGamma;
  // skip ccmRgbRate[3], ccmHsvRate[3] and ccm[9], for now
  if (!_openslide_fseek(f, 60, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek within SqExtraInfo: ");
    return false;
  }
  extraInfo->timeConsuming = read_string_from_file(f, 32);
  extraInfo->scanTime = read_le_uint32_from_file(f);
  // skip stepTime[10], for now
  if (!_openslide_fseek(f, 20, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek within SqExtraInfo: ");
    return false;
  }
  extraInfo->serial = read_string_from_file(f, 32);
  extraInfo->fusionLayer = read_byte_from_file(f);
  unsigned char stepBuf[4];
  if (!_openslide_fread_exact(f, stepBuf, sizeof(stepBuf), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read step within SqPicHead");
    return false;
  }
  float *step = (float *) stepBuf;
  extraInfo->step = *step;
  extraInfo->focusPoint = read_le_uint16_from_file(f);
  extraInfo->validFocusPoint = read_le_uint16_from_file(f);
  extraInfo->barCode = read_string_from_file(f, 128);
  unsigned char cameraGammaBuf[4];
  if (!_openslide_fread_exact(f, cameraGammaBuf, sizeof(cameraGammaBuf), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read cameraGamma within SqPicHead");
    return false;
  }
  float *cameraGamma = (float *) cameraGammaBuf;
  extraInfo->cameraGamma = *cameraGamma;
  unsigned char cameraExposureBuf[4];
  if (!_openslide_fread_exact(f, cameraExposureBuf, sizeof(cameraExposureBuf), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read cameraExposure within SqPicHead");
    return false;
  }
  float *cameraExposure = (float *) cameraExposureBuf;
  extraInfo->cameraExposure = *cameraExposure;
  unsigned char cameraGainBuf[4];
  if (!_openslide_fread_exact(f, cameraGainBuf, sizeof(cameraGainBuf), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read cameraGain within SqPicHead");
    return false;
  }
  float *cameraGain = (float *) cameraGainBuf;
  extraInfo->cameraGain = *cameraGain;
  extraInfo->headSpace1 = read_le_uint32_from_file(f);
  extraInfo->headSpace2 = read_le_uint32_from_file(f);
  extraInfo->objectiveModel = read_string_from_file(f, 128);
  extraInfo->reversed = read_string_from_file(f, 297);

  // printf("flag : %x \n"
  //        "inforSize : %d \n"
  //        "nextOffset : %ld \n"
  //        "model : %s \n"
  //        "ccmGamma : %f \n"
  //        "timeConsuming : %s \n"
  //        "scanTime : %d \n"
  //        "serial : %s \n"
  //        "fusionLayer : %u \n"
  //        "step : %f \n"
  //        "focusPoint : %d \n"
  //        "validFocusPoint : %d \n"
  //        "barCode : %s \n"
  //        "cameraGamma : %f \n"
  //        "cameraExposure : %f \n"
  //        "cameraGain : %f \n"
  //        "headSpace1 : %d \n"
  //        "headSpace2 : %d \n"
  //        "objectiveModel : %s \n"
  //        "reversed : %s \n",
  //        extraInfo->flag,
  //        extraInfo->inforSize,
  //        extraInfo->nextOffset,
  //        extraInfo->model,
  //        extraInfo->ccmGamma,
  //        extraInfo->timeConsuming,
  //        extraInfo->scanTime,
  //        extraInfo->serial,
  //        *extraInfo->fusionLayer,
  //        extraInfo->step,
  //        extraInfo->focusPoint,
  //        extraInfo->validFocusPoint,
  //        extraInfo->barCode,
  //        extraInfo->cameraGamma,
  //        extraInfo->cameraExposure,
  //        extraInfo->cameraGain,
  //        extraInfo->headSpace1,
  //        extraInfo->headSpace2,
  //        extraInfo->objectiveModel,
  //        extraInfo->reversed);

  return true;
}

static bool read_macrograph_info(struct _openslide_file *f,
                                 struct MacrographInfo *macrographInfo,
                                 GError **err) {
                                 // check flag
  macrographInfo->flag = read_le_uint16_from_file(f);
  if (macrographInfo->flag != MACROGRAPH_INFO_FLAG) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported SqMacrographInfo flag: %x", macrographInfo->flag);
    return false;
  }
  macrographInfo->rgb = read_le_uint64_from_file(f);
  macrographInfo->width = read_le_uint32_from_file(f);
  macrographInfo->height = read_le_uint32_from_file(f);
  macrographInfo->chance = read_le_uint32_from_file(f);
  macrographInfo->step = read_le_uint32_from_file(f);
  macrographInfo->rgbSize = read_le_uint64_from_file(f);
  macrographInfo->encodeSize = read_le_uint64_from_file(f);
  macrographInfo->quality = read_byte_from_file(f);
  macrographInfo->nextLayerOffset = read_le_uint64_from_file(f);
  macrographInfo->headSpace_1 = read_le_uint32_from_file(f);
  macrographInfo->headSpace_2 = read_le_uint32_from_file(f);
  macrographInfo->headSpace = read_string_from_file(f, 64);

  // printf("flag : %x \n"
  //        "rgb : %ld \n"
  //        "width : %d \n"
  //        "height : %d \n"
  //        "chance : %d \n"
  //        "step : %d \n"
  //        "rgbSize : %ld \n"
  //        "encodeSize : %ld \n"
  //        "quality : %u \n"
  //        "nextLayerOffset : %ld \n"
  //        "headSpace_1 : %d \n"
  //        "headSpace_2 : %d \n"
  //        "headSpace : %s \n",
  //        macrographInfo->flag,
  //        macrographInfo->rgb,
  //        macrographInfo->width,
  //        macrographInfo->height,
  //        macrographInfo->chance,
  //        macrographInfo->step,
  //        macrographInfo->rgbSize,
  //        macrographInfo->encodeSize,
  //        *macrographInfo->quality,
  //        macrographInfo->nextLayerOffset,
  //        macrographInfo->headSpace_1,
  //        macrographInfo->headSpace_2,
  //        macrographInfo->headSpace);

  return true;
}

static bool read_pic_info(struct _openslide_file *f,
                          struct PicInfo *picInfo,
                          bool thumb,
                          GError **err) {
                          // check flag
  picInfo->flag = read_le_uint16_from_file(f);
  if (picInfo->flag != PIC_INFO_FLAG) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported SqPicInfo flag: %x", picInfo->flag);
    return false;
  }
  picInfo->infoSize = read_le_uint32_from_file(f);
  picInfo->layer = read_le_uint32_from_file(f);
  picInfo->sliceNum = read_le_uint32_from_file(f);
  if (thumb)
    g_assert(picInfo->sliceNum == 1);
  picInfo->sliceNumX = read_le_uint32_from_file(f);
  if (thumb)
    g_assert(picInfo->sliceNumX == 1);
  picInfo->sliceNumY = read_le_uint32_from_file(f);
  if (thumb)
    g_assert(picInfo->sliceNumY == 1);
  picInfo->layerSize = read_le_uint64_from_file(f);
  picInfo->nextLayerOffset = read_le_uint64_from_file(f);
  unsigned char curScaleBuf[4];
  if (!_openslide_fread_exact(f, curScaleBuf, sizeof(curScaleBuf), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read curScale within SqPicInfo");
    return false;
  }
  float *curScale = (float *) curScaleBuf;
  picInfo->curScale = *curScale;
  unsigned char rulerBuf[8];
  if (!_openslide_fread_exact(f, rulerBuf, sizeof(rulerBuf), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read ruler within SqPicInfo");
    return false;
  }
  double *ruler = (double *) rulerBuf;
  picInfo->ruler = *ruler;
  picInfo->defaultX = read_le_uint32_from_file(f);
  picInfo->defaultY = read_le_uint32_from_file(f);
  picInfo->bmpFlag = read_byte_from_file(f);
  picInfo->headSpace = read_string_from_file(f, 63);

  // printf("flag : %x \n"
  //        "infoSize : %d \n"
  //        "layer : %d \n"
  //        "sliceNum : %d \n"
  //        "sliceNumX : %d \n"
  //        "sliceNumY : %d \n"
  //        "layerSize : %ld \n"
  //        "nextLayerOffset : %ld \n"
  //        "curScale : %f \n"
  //        "ruler : %f \n"
  //        "defaultX : %d \n"
  //        "defaultY : %d \n"
  //        "bmpFlag : %u \n"
  //        "headSpace : %s \n",
  //        picInfo->flag,
  //        picInfo->infoSize,
  //        picInfo->layer,
  //        picInfo->sliceNum,
  //        picInfo->sliceNumX,
  //        picInfo->sliceNumY,
  //        picInfo->layerSize,
  //        picInfo->nextLayerOffset,
  //        picInfo->curScale,
  //        picInfo->ruler,
  //        picInfo->defaultX,
  //        picInfo->defaultY,
  //        *picInfo->bmpFlag,
  //        picInfo->headSpace);

  return true;
}

static bool teksqray_sdpc_dyqx_open(openslide_t *osr, const char *filename,
                                    struct _openslide_tifflike *tl G_GNUC_UNUSED,
                                    struct _openslide_hash *quickhash1 G_GNUC_UNUSED, GError **err) {
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }

  g_autoptr(PicHead) picHead = g_new0(struct PicHead, 1);
  if (!read_pic_head(f, picHead, err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "failed to read pic head");
    return false;
  }
  g_hash_table_insert(osr->properties,
                      g_strdup("teksqray.sliceWidth"),
                      _openslide_format_double((double) picHead->sliceWidth));
  g_hash_table_insert(osr->properties,
                      g_strdup("teksqray.sliceHeight"),
                      _openslide_format_double((double) picHead->sliceHeight));
  g_hash_table_insert(osr->properties,
                      g_strdup("teksqray.scale"),
                      _openslide_format_double(picHead->scale));
  g_hash_table_insert(osr->properties,
                      g_strdup("teksqray.ruler"),
                      _openslide_format_double(picHead->ruler));
  g_hash_table_insert(osr->properties,
                      g_strdup("teksqray.rate"),
                      _openslide_format_double((double) picHead->rate));

  if (!_openslide_fseek(f, picHead->headSize, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to SqPersonInfo: ");
    return false;
  }
  g_autoptr(PersonInfo) personInfo = g_new0(struct PersonInfo, 1);
  if (!read_person_info(f, personInfo, err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "failed to read person info");
    return false;
  }

  if (picHead->extraOffset > 0) {
    g_autoptr(ExtraInfo) extraInfo = g_new0(struct ExtraInfo, 1);
    // SqExtraInfo
    if (!_openslide_fseek(f, picHead->extraOffset, SEEK_SET, err)) {
      g_prefix_error(err, "Couldn't seek to SqExtraInfo: ");
      return false;
    }
    if (!read_extra_info(f, extraInfo, err)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "failed to read extra info");
      return false;
    }

    // add properties
    g_hash_table_insert(osr->properties,
                        g_strdup("teksqray.scanTime"),
                        _openslide_format_double((double) extraInfo->scanTime));
                      // g_hash_table_insert(osr->properties, // TODO!! fix leaking memory here
                      //                     g_strdup("teksqray.timeConsuming"),
                      //                     extraInfo->timeConsuming);
  }

  int MacroInfoSize = 123;
  g_autoptr(GPtrArray) macrographInfo_array =
    g_ptr_array_new_with_free_func((GDestroyNotify) destroy_macrographInfo);
  uint64_t offset = personInfo->nextOffset;
  // SqMacrographInfo
  for (int i = 0; i < (int) picHead->macrograph; i++) {
    struct MacrographInfo *macrographInfo = g_new0(struct MacrographInfo, 1);
    g_ptr_array_add(macrographInfo_array, macrographInfo);
    if (!_openslide_fseek(f, offset, SEEK_SET, err)) {
      g_prefix_error(err, "Couldn't seek within SqMacrographInfo: ");
      return false;
    }
    if (!read_macrograph_info(f, macrographInfo, err)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "failed to read macrograph info");
      return false;
    }

    // add associated images (label and macro)
    const char *associated_image_name = i == 0 ? "label" : "macro";
    if (!_openslide_jpeg_add_associated_image(osr, associated_image_name, filename, offset + MacroInfoSize, err)) {
      g_prefix_error(err, "Couldn't read associated image: %s", associated_image_name);
      return false;
    }

    offset = macrographInfo->nextLayerOffset;
  }

  struct MacrographInfo *macrographInfo = macrographInfo_array->pdata[picHead->macrograph - 1];
  uint64_t thumbnail_info_in_file = macrographInfo->nextLayerOffset;
  if (!_openslide_fseek(f, thumbnail_info_in_file, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  g_autoptr(PicInfo) thumbInfo = g_new0(struct PicInfo, 1);
  if (!read_pic_info(f, thumbInfo, true, err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "failed to read thumb info");
    return false;
  }

  // add associated images (thumbnail)
  int ThumbInfoSize = 122;
  g_assert(thumbInfo->infoSize == ThumbInfoSize);
  uint64_t thumbnail_data_in_file = thumbnail_info_in_file + ThumbInfoSize;
  uint32_t *thumbdata = decode_thumb(f, thumbnail_data_in_file, thumbInfo->layerSize, picHead->thumbnailWidth, picHead->thumbnailHeight, err);
  if (thumbdata == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "failed to read thumb data");
    return false;
  }
  if (!_openslide_jpeg_add_associated_image_3(osr, "thumbnail", filename, thumbdata, picHead->thumbnailWidth, picHead->thumbnailHeight, err)) {
    g_prefix_error(err, "Couldn't read associated image: %s", "thumbnail");
    return false;
  }

  // read base dimensions
  int64_t base_h = picHead->srcHeight;
  int64_t base_w = picHead->srcWidth;
  // calculate level count
  int32_t zoom_levels = picHead->hierarchy;
  int32_t tile_width = picHead->sliceWidth;
  int32_t tile_height = picHead->sliceHeight;

  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_with_free_func((GDestroyNotify) destroy_level);
  int64_t downsample;

  long nextLayerOffset = thumbInfo->nextLayerOffset;
  g_autoptr(GPtrArray) picInfo_array =
    g_ptr_array_new_with_free_func((GDestroyNotify) destroy_picInfo);
  // SqPicInfo
  int32_t image_number = 0;
  int PicInfoSize = 122;
  for (int i = 0; i < zoom_levels; i++) {
    struct PicInfo *layerPicInfo = g_new0(struct PicInfo, 1);
    g_ptr_array_add(picInfo_array, layerPicInfo);
    if (!_openslide_fseek(f, nextLayerOffset, SEEK_SET, err)) {
      g_prefix_error(err, "Couldn't seek within nextLayerOffset: ");
      return false;
    }
    if (!read_pic_info(f, layerPicInfo, false, err)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "failed to read layer pic info");
      return false;
    }

    // set up level dimensions and such
    downsample = 1 / (layerPicInfo->curScale);

    // ensure downsample is > 0 and a power of 2
    if (downsample <= 0 || (downsample & (downsample - 1))) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Invalid downsample %" PRId64, downsample);
      return false;
    }

    struct level *l = g_new0(struct level, 1);
    g_ptr_array_add(level_array, l);

    l->base.downsample = downsample;
    l->base.tile_w = (double) tile_width;
    l->base.tile_h = (double) tile_height;

    l->base.w = base_w / l->base.downsample;
    l->base.h = base_h / l->base.downsample;

    int64_t tile_rows = layerPicInfo->sliceNumY;
    int64_t tile_cols = layerPicInfo->sliceNumX;
    g_assert(tile_rows >= 1);
    g_assert(tile_cols >= 1);
    l->tiles_across = tile_cols;
    l->tiles_down = tile_rows;

    l->grid = _openslide_grid_create_tilemap(osr,
                                             tile_width,
                                             tile_height,
                                             read_tile, tile_free);

    int64_t seek_location = nextLayerOffset + PicInfoSize;
    int32_t tile_count = layerPicInfo->sliceNum; // tile count in current level
    g_assert(tile_count == l->tiles_across * l->tiles_down);
    // build up the tiles
    if (!process_tiles_info_from_header(f,
                                        seek_location,
                                        i,
                                        tile_count,
                                        &image_number,
                                        l,
                                        err)) {
      return false;
    }

    nextLayerOffset = layerPicInfo->nextLayerOffset;
  }

  // set MPP and objective power
  _openslide_duplicate_double_prop(osr, "teksqray.rate",
                                   OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  _openslide_duplicate_double_prop(osr, "teksqray.ruler",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(osr, "teksqray.ruler",
                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);

  for (int i = 0; i < zoom_levels; i++) {
    struct level *l = level_array->pdata[i];
    if (!l->base.tile_w || !l->base.tile_h) {
      // invalidate
      for (i = 0; i < zoom_levels; i++) {
        struct level *l = level_array->pdata[i];
        l->base.tile_w = 0;
        l->base.tile_h = 0;
      }
      break;
    }
  }

  // build ops data
  struct teksqray_ops_data *data = g_new0(struct teksqray_ops_data, 1);
  data->filename = g_strdup(filename);
  data->sliceFmt = picHead->sliceFmt;

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->level_count = zoom_levels;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &teksqray_ops;

  return true;
}

const struct _openslide_format _openslide_format_teksqray_sdpc_dyqx = {
    .name = "teksqray-sdpc-dyqx",
    .vendor = "teksqray",
    .detect = teksqray_sdpc_dyqx_detect,
    .open = teksqray_sdpc_dyqx_open,
};
