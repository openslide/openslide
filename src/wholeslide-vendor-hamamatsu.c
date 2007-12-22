#include "config.h"

#include <glib.h>
#include <stdio.h>

#include <jpeglib.h>

#include "wholeslide-private.h"

static const char GROUP_VMS[] = "Virtual Microscope Specimen";
static const char KEY_OPT_FILE[] = "OptimisationFile";
static const char KEY_MAP_FILE[] = "MapFile";
static const char KEY_IMAGE_FILE[] = "ImageFile";
static const char KEY_OBJECTIVE[] = "SourceLens";


bool _ws_try_hamamatsu(wholeslide_t *wsd, const char *filename) {
  char *dirname = g_path_get_dirname(filename);
  char *opt_filename = NULL;
  char *map_filename = NULL;
  char *image_filename = NULL;
  FILE *f = NULL;

  // first, see if it's a VMS file
  GKeyFile *vms_file = g_key_file_new();
  if (!g_key_file_load_from_file(vms_file, filename, G_KEY_FILE_NONE, NULL)) {
    goto FAIL;
  }

  printf("vms file exists\n");

  if (!g_key_file_has_group(vms_file, GROUP_VMS)) {
    goto FAIL;
  }

  printf("vms file has group\n");


  // extract relevant info
  char *tmp;
  tmp = g_key_file_get_string(vms_file,
			      GROUP_VMS,
			      KEY_OPT_FILE,
			      NULL);
  if (tmp) {
    opt_filename = g_build_filename(dirname, tmp, NULL);
    g_free(tmp);
  }

  tmp = g_key_file_get_string(vms_file,
			      GROUP_VMS,
			      KEY_MAP_FILE,
			      NULL);
  if (tmp) {
    map_filename = g_build_filename(dirname, tmp, NULL);
    g_free(tmp);
  }

  tmp = g_key_file_get_string(vms_file,
			      GROUP_VMS,
			      KEY_IMAGE_FILE,
			      NULL);
  if (tmp) {
    image_filename = g_build_filename(dirname, tmp, NULL);
    g_free(tmp);
  }


  printf("opt: %s, map: %s, image: %s\n",
	 opt_filename, map_filename, image_filename);

  // check image filename (the others are sort of optional)
  if (!image_filename) {
    goto FAIL;
  }

  // check to make sure the image is a real jpeg
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);

  if ((f = fopen(image_filename, "rb")) == NULL) {
    goto FAIL;
  }

  printf("fopen success\n");

  int result;

  jpeg_stdio_src(&cinfo, f);
  result = jpeg_read_header(&cinfo, FALSE);  // read headers
  if (result != JPEG_HEADER_OK && result != JPEG_HEADER_TABLES_ONLY) {
    jpeg_destroy_decompress(&cinfo);
    goto FAIL;
  }

  jpeg_start_decompress(&cinfo);

  unsigned int restart_interval = cinfo.restart_interval;
  JDIMENSION MCUs_per_row = cinfo.MCUs_per_row;
  JDIMENSION MCU_rows_in_scan = cinfo.MCU_rows_in_scan;

  printf("w: %d, h: %d, restart_interval: %d\n"
	 "mcus_per_row: %d, mcu_rows_in_scan: %d\n"
	 "restart intervals per row: %d\n"
	 "leftover mcus: %d\n",
	 cinfo.output_width, cinfo.output_height,
	 restart_interval,
	 MCUs_per_row, MCU_rows_in_scan,
	 MCUs_per_row / restart_interval,
	 MCUs_per_row % restart_interval);


  // generate the optimization list
  jpeg_destroy_decompress(&cinfo);
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);

  uint64_t *mcu_row_starts = g_new(uint64_t, MCU_rows_in_scan);

  rewind(f);
  _ws_jpeg_fancy_src(&cinfo, f, -1, -1);
  jpeg_read_header(&cinfo, FALSE);

  printf("offset: %d\n", _ws_jpeg_fancy_src_get_filepos(&cinfo));

  for (int i = 0; i < MCU_rows_in_scan; i++) {
    
  }


 FAIL:
  if (f) {
    fclose(f);
  }
  g_free(dirname);
  g_free(opt_filename);
  g_free(image_filename);
  g_free(map_filename);
  g_key_file_free(vms_file);
  return false;
}
