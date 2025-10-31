#include "openslide-decode-gsf.h"

GsfInfile *_openslide_gsf_open_archive(char const *filename) {
	GsfInfile *infile;
	GError *error = NULL;
	GsfInput *src;
	char *display_name;

	src = gsf_input_stdio_new(filename, &error);
	if (error) {
		display_name = g_filename_display_name(filename);
		// g_printerr(_("%s: Failed to open %s: %s\n"),
		// 	    g_get_prgname(),
		// 	    display_name,
		// 	    error->message);
		g_free(display_name);
		return NULL;
	}

	infile = gsf_infile_zip_new(src, NULL);
	if (infile) {
		g_object_unref(src);
		return infile;
	}

	infile = gsf_infile_msole_new(src, NULL);
	if (infile) {
		g_object_unref(src);
		return infile;
	}

	infile = gsf_infile_tar_new(src, NULL);
	if (infile) {
		g_object_unref(src);
		return infile;
	}

	display_name = g_filename_display_name(filename);
	// g_printerr(_("%s: Failed to recognize %s as an archive\n"),
	// 	    g_get_prgname(),
	// 	    display_name);
	g_free(display_name);

	g_object_unref(src);
	return NULL;
}