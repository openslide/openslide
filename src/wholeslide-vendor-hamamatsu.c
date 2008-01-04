#include "config.h"

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>

#include <jpeglib.h>

#include "wholeslide-private.h"

static const char GROUP_VMS[] = "Virtual Microscope Specimen";
static const char KEY_OPT_FILE[] = "OptimisationFile";
static const char KEY_MAP_FILE[] = "MapFile";
static const char KEY_IMAGE_FILE[] = "ImageFile";
static const char KEY_OBJECTIVE[] = "SourceLens";

#define INPUT_BUF_SIZE  4096


static bool compute_optimization(FILE *f,
				 uint64_t *mcu_starts_count,
				 int64_t **mcu_starts) {
  // check to make sure the image is a real jpeg
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);

  int header_result;

  jpeg_stdio_src(&cinfo, f);
  header_result = jpeg_read_header(&cinfo, FALSE);  // read headers
  if ((header_result != JPEG_HEADER_OK
       && header_result != JPEG_HEADER_TABLES_ONLY)
      || cinfo.num_components != 3 || cinfo.restart_interval == 0) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  jpeg_start_decompress(&cinfo);

  unsigned int restart_interval = cinfo.restart_interval;
  JDIMENSION MCUs_per_row = cinfo.MCUs_per_row;
  JDIMENSION MCU_rows_in_scan = cinfo.MCU_rows_in_scan;

  uint64_t MCUs = MCUs_per_row * MCU_rows_in_scan;

  *mcu_starts_count = MCUs / restart_interval;

  unsigned int leftover_mcus = MCUs_per_row % restart_interval;

  printf("w: %d, h: %d, restart_interval: %d\n"
	 "mcus_per_row: %d, mcu_rows_in_scan: %d\n"
	 "leftover mcus: %d\n",
	 cinfo.output_width, cinfo.output_height,
	 restart_interval,
	 MCUs_per_row, MCU_rows_in_scan,
	 leftover_mcus);

  if (leftover_mcus != 0) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }


  // generate the optimization list, by finding restart markers
  jpeg_destroy_decompress(&cinfo);

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  rewind(f);
  _ws_jpeg_fancy_src(&cinfo, f, NULL, 0, 0, 0, 0);

  *mcu_starts = g_new0(int64_t, *mcu_starts_count);

  jpeg_read_header(&cinfo, FALSE);

  // the first entry
  (*mcu_starts)[0] = _ws_jpeg_fancy_src_get_filepos(&cinfo);
  printf("offset: %#llx\n", (*mcu_starts)[0]);

  // now find the rest of the MCUs
  bool last_was_ff = false;
  uint64_t marker = 0;
  while (marker < *mcu_starts_count) {
    if (cinfo.src->bytes_in_buffer == 0) {
      (cinfo.src->fill_input_buffer)(&cinfo);
    }
    uint8_t b = *(cinfo.src->next_input_byte++);
    cinfo.src->bytes_in_buffer--;

    if (last_was_ff) {
      // EOI?
      if (b == JPEG_EOI) {
	// we're done
	break;
      } else if (b >= 0xD0 && b < 0xD8) {
	// marker
	(*mcu_starts)[1 + marker++] = _ws_jpeg_fancy_src_get_filepos(&cinfo);
      }
    }
    last_was_ff = b == 0xFF;
  }

  /*
  for (uint64_t i = 0; i < *mcu_starts_count; i++) {
    printf(" %lld\n", (*mcu_starts)[i]);
  }
  */

  // success, now clean up
  jpeg_destroy_decompress(&cinfo);
  return true;
}



bool _ws_try_hamamatsu(wholeslide_t *wsd, const char *filename) {
  char *dirname = g_path_get_dirname(filename);
  char *opt_filename = NULL;
  char *map_filename = NULL;
  char *image_filename = NULL;
  bool success = false;

  // this format has 2 jpeg files
  FILE *jpegs[2] = { NULL, NULL };
  uint64_t mcu_starts_count_array[2] = { 0, 0 };
  int64_t *mcu_starts_array[2] = { NULL, NULL };


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

  wsd->objective_power =
    g_key_file_get_double(vms_file, GROUP_VMS, KEY_OBJECTIVE, NULL);

  printf("opt: %s, map: %s, image: %s\n",
	 opt_filename, map_filename, image_filename);

  // check image filename (the others are sort of optional)
  if (!image_filename || !map_filename) {
    goto FAIL;
  }

  // compute the optimization lists
  // image 0
  if ((jpegs[0] = fopen(image_filename, "rb")) == NULL) {
    goto FAIL;
  }
  if (!compute_optimization(jpegs[0],
			    &mcu_starts_count_array[0], &mcu_starts_array[0])) {
    goto FAIL;
  }

  // image 1
  if ((jpegs[1] = fopen(map_filename, "rb")) == NULL) {
    goto FAIL;
  }
  if (!compute_optimization(jpegs[1],
			    &mcu_starts_count_array[1], &mcu_starts_array[1])) {
    goto FAIL;
  }

  _ws_add_jpeg_ops(wsd, 2, jpegs, mcu_starts_count_array, mcu_starts_array);
  success = true;
  goto DONE;

 FAIL:
  if (jpegs[0]) {
    fclose(jpegs[0]);
  }
  if (jpegs[1]) {
    fclose(jpegs[1]);
  }
  success = false;

 DONE:
  g_free(dirname);
  g_free(opt_filename);
  g_free(image_filename);
  g_free(map_filename);
  g_key_file_free(vms_file);

  return success;
}
