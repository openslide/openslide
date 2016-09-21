/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-zip.h"
#include "openslide-decode-jpeg.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>

//#define g_debug(...) fprintf(stderr, __VA_ARGS__)

// remove _OPENSLIDE_POISON(xxx) warnings
#undef zip_open 
#undef zip_fopen

// searches index of a file inside a zip archive
// if name is a path, the match is attempted twice with backslash and forward slash
// the zip_name_locate searches very fast if it's case-sensitive (because of hash table)
// a FL_NOCASE flag defaults to a sequential search, and can be very slow

zip_int64_t _openslide_zip_name_locate(struct _openslide_ziphandle *zh, const char *filename, zip_flags_t flags)
{
	zip_int64_t idx;

	// i believe zip_name_locate is thread-safe, therefore we don't need to lock the mutex

	gchar *name=g_strdup(filename);
	gchar *s;

	for (s=name; *s; s++)
		{ if (*s=='/') *s = '\\';	}
	//g_debug("locating name %s...", name);
	idx = zip_name_locate(zh->handle, name, flags);
	if (idx<0)
	{
		bool has_slash=false;
		for (s=name; *s; s++)
			{ if (*s=='\\')
				{ *s = '/'; has_slash=true; }
			}
		//g_debug("locating name %s...", name);
		if (has_slash) // Repeat name search only if slashes exist
		{	idx = zip_name_locate(zh->handle, name, flags);	}
	}
	//g_debug("found index %i\n",(int)ix);
	g_free(name);
	return idx;
}

// open archive from zip source, fills _openslide_ziphandle structure
bool _openslide_zip_open_archive_from_source(zip_source_t *zs, struct _openslide_ziphandle *zh, GError **err)
{
	zip_error_t ze;
	zip_error_init(&ze);

	zh->handle = zip_open_from_source(zs,ZIP_RDONLY,&ze);

	if (zh->handle==NULL || ze.zip_err || ze.sys_err ) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
		    	"_openslide_zip_open_archive_from_source: returning libzip error (%s) while trying to open zip archive.",
				zip_error_strerror(&ze));
		return false;
	}

	g_mutex_init(&zh->lock);

	return true;
}

// open archive by file name, fills _openslide_ziphandle structure
bool _openslide_zip_open_archive(const char *filename, struct _openslide_ziphandle *zh, GError **err)
{
    
#if defined _WIN32
//includes WIN64 
    int zei = 0;

	zh->handle = zip_open(filename,ZIP_RDONLY,&zei);

	if (zh->handle==NULL || zei!=0 ) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
		    	"_openslide_zip_open_archive: returning libzip error %i while trying to open zip archive.", zei);
		return false;
	}

	g_mutex_init(&zh->lock);

	return true;

#else
// This is an alternative solution that calls _openslide_fopen, as by openslide convention, and then creates zip_source from FILE handle
// unfortunately it does not run on Windows, where libzip exits with "stream doesn't support seeking"
	
	zip_error_t ze;
	zip_error_init(&ze);

	FILE *f = _openslide_fopen(filename,"rb",err);
	if (!f) {	
		return false;
	}
	zip_source_t *zs = zip_source_filep_create(f, 0, 0, &ze);
	
	if (!zs )//|| ze.zip_err || ze.sys_err)
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
		    	"_openslide_zip_open_archive: returning libzip error \"%s\" while creating zip source from file handle for file \"%s\".",
				zip_error_strerror(&ze),filename);
		return false;
	}
	
	if (_openslide_zip_open_archive_from_source(zs, zh, err)) {	
		return true; }
	else {
		zip_source_free(zs); // must delete zip_source only when opening failed
		return false;
	}
#endif
}

bool _openslide_zip_close_archive(struct _openslide_ziphandle *zh)
{
	int ret = 0;
	if (zh->handle)
	{
		//TODO: should assert that mutex is unlocked
		//g_mutex_lock(&zh->lock);
		//g_debug("closing handle %p\n", (void*)zh->handle);
		ret = zip_close(zh->handle);
		//g_debug("zip_close returns %i\n", ret);
		//g_mutex_unlock(&zh->lock);
		g_mutex_clear(&zh->lock);
		zh->handle=NULL;
	}
	return ret==0;
}

// Read file from zip archive into memory
// The buffer is slice-allocated and needs to be deleted with g_slice_free after use
// In case of failure, nothing gets allocated.

bool _openslide_zip_read_file_data(struct _openslide_ziphandle *zh, zip_uint64_t index, gpointer *file_buf, size_t *file_len, GError **err)
{
	// zip_open is NOT thread-safe, i.e. you cannot open multiple files on the same zip archive from multiple threads.
    // hence we lock the mutex while accessing a file in an archive
    
    // any other file access function should use this one to access zip file data.

    zip_stat_t zstat;
    zip_int64_t bytes_read;
    zip_t *z = zh->handle;

    *file_buf = NULL;
    *file_len = 0;

    //g_debug("requesting file at index=%i from archive %p\n", (int)index, (void*)z);

	g_mutex_lock(&zh->lock); // This mutex must be released before exit of this function.

    // retrieving information about file at index
	memset(&zstat,0xff,sizeof(zstat));
	zip_error_clear(z);
    zip_stat_init(&zstat);

    if (zip_stat_index(z, index, 0, &zstat) == -1)
    {
    	zip_error_t *ze = zip_get_error(z);

    	g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
    	"_openslide_zip_read_file_data: zip_stat_index cannot retrieve stats on index %li - libzip message=\"%s\"",
			(long int)index, zip_error_strerror(ze));
    	goto FAIL_AND_UNLOCK;
    }

    const char *fname = (zstat.flags & ZIP_STAT_NAME)!=0 ? zstat.name : "<invalid>";

	//g_debug("stats: flags=%x, name=%s, mtime=%lf compsize=%li, size=%li\n",
	//		(int)zstat.flags, zstat.name, (double)zstat.mtime, (unsigned long)zstat.comp_size, (unsigned long)zstat.size);

    if ( (zstat.flags & ZIP_STAT_SIZE)==0 )
    {
    	g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
    	                "_openslide_zip_read_file_data: Cannot retrieve size information about file id %i, filename=\"%s\" from zip container",
						(int)zstat.index, fname);
    	goto FAIL_AND_UNLOCK;
    }

    if (zstat.size >= (uint64_t)1 << 31 )
    {
    	g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
    	                "_openslide_zip_read_file_data: This module can only handle file sizes smaller than 2GB, index=%i filename=\"%s\", size=%li",
						(int)zstat.index, fname, (long int)zstat.size);
    	goto FAIL_AND_UNLOCK;
    }

    // allocate buffer with known size
    *file_buf = g_slice_alloc((size_t)zstat.size);
    if (*file_buf==NULL)
    {
    	g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
    	                "_openslide_zip_read_file_data: g_slice_alloc couldn't allocate buffer with size=%i", (int)zstat.size);
    	goto FAIL_AND_UNLOCK;
    }
    *file_len = zstat.size;

    // read file into buffer


    zip_file_t *file = zip_fopen_index(z, index, 0);
    if (!file)
    {
    	zip_error_t *ze = zip_get_error(z);

    	g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "_openslide_zip_read_file_data: cannot open file %s at index %i - zip error = %s",
                    fname, (int)index, zip_error_strerror(ze));
    	goto FAIL_AND_UNLOCK;
    }

	//g_debug("opened file @index=%i file_p=%p\n", (int)index, (void*)file);
    bytes_read = zip_fread(file, *file_buf, zstat.size);
    // don't check zip_fread result before closing!
	//g_debug("file=%s bytes read=%i\n", fname, (int)bytes_read);

    int close_err = zip_fclose(file);

    if (close_err) { 
    	zip_error_t *ze = zip_get_error(z);

    	g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
    			"_openslide_zip_read_file_data: zip_fclose returned error code=%i. filename=%s ziperrortext=%s", 
                 close_err, fname, zip_error_strerror(ze));
    	goto FAIL_AND_UNLOCK;
    }

    if ((size_t)bytes_read != (size_t)zstat.size)
    {
    	zip_error_t *ze = zip_get_error(z);

    	g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
    			"_openslide_zip_read_file_data: While accessing file \"%s\" in zip archive, the number of bytes retrieved (%li) didn't match the file size in zip header (%li). Zip error=%s",
				fname, (long int)bytes_read, (long int)zstat.size, zip_error_strerror(ze));
    	goto FAIL_AND_UNLOCK;
    }

    g_mutex_unlock(&zh->lock);
    return true;

FAIL_AND_UNLOCK:
    if (*file_buf)  {   // in case of failure, free buffer if allocated
        g_slice_free1(zstat.size, *file_buf);
        *file_buf = NULL;
        *file_len = 0;
    }
    g_mutex_unlock(&zh->lock);    
    return false;
}

// Helper function: Loads an XML file from ZIP archive and starts parsing.
// optionally, the raw data can be included in the quickhash.
// load&parse xml from zip container, if wanted can also calculate quickhash over raw data
xmlDoc *_openslide_zip_parse_xml_file(struct _openslide_ziphandle *zh, const char *filename, zip_flags_t flags, GError **err, struct _openslide_hash *hash)
{
	zip_int64_t xml_file_id;
	bool success;
	xmlDoc *xmldoc = NULL;
	gpointer xmlbuf = NULL;
    size_t xmlsize = 0;

    // fetch & parse XML
    xml_file_id = _openslide_zip_name_locate(zh, filename, flags);

    if (xml_file_id==-1) { // file not found
        zip_error_t *ze = zip_get_error(zh->handle);

		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			                "Cannot locate XML description file \"%s\" in container. libzip message:\"%s\"", filename, ze->str);
		goto FINISH;
    }

    success = _openslide_zip_read_file_data(zh,xml_file_id,&xmlbuf,&xmlsize,err);

    //g_debug("ready to parse xml_file_id=%i, xmlbuf=%p, xmlsize=%i\n", (int)xml_file_id, (void*)xmlbuf, (int)xmlsize);

    if (!success) {
		g_prefix_error(err,"Cannot access VMIC XML description file at zip index %li - reason:", (long int)xml_file_id);
		goto FINISH;
    }

    if (hash)
    	_openslide_hash_data(hash,xmlbuf,xmlsize);

    xmldoc = xmlReadMemory(xmlbuf, xmlsize, NULL, NULL,
	                              XML_PARSE_NOERROR |
	                              XML_PARSE_NOWARNING |
	                              XML_PARSE_NONET);
    if (!xmldoc) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
		                "Cannot parse XML description file \"%s\"", filename);
		goto FINISH_AND_FREE;
    }

FINISH_AND_FREE:
	g_slice_free1(xmlsize, xmlbuf);

FINISH:
	return xmldoc;

}

// Public helper function: reads an file from zip archive at index position and decodes it into a buffer
// height and width of image are returned by reference and
// a buffer with RGBA image data is allocated with g_slice_alloc, which must be freed after use with g_slice_free

bool _openslide_zip_read_image(struct _openslide_ziphandle *zh, zip_uint64_t file_id, enum image_format dzif, uint32_t **pdestbuf, int32_t *pw, int32_t *ph, GError **err)
{
    gpointer cbuf;  // coded image data
    size_t cbufsize;

    bool success;
    int32_t dw,dh;
    uint32_t *destbuf = NULL; // decoded image data

    *pdestbuf = NULL;
    *pw = -1;
    *ph = -1;

    success = _openslide_zip_read_file_data(zh, file_id, &cbuf, &cbufsize, err);
    if (!success) return false;

    if (dzif==IMAGE_FORMAT_JPEG)
    {
    	success = _openslide_jpeg_decode_buffer_dimensions(cbuf, cbufsize, &dw, &dh, err);
        
    	if (success) {
            //g_debug("jpeg size %i,%i\n", (int)dw, (int)dh);
    		destbuf = g_slice_alloc((size_t)dw * dh * 4);
            
    		if (destbuf) {
        		success = _openslide_jpeg_decode_buffer(cbuf, cbufsize, destbuf,
    	                                          dw, dh, err);
        		if (success) {	
                    *pdestbuf = destbuf;
        			*pw = dw;
        			*ph = dh;
        		}
    		}
    		else {
    			g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
    				                "_openslide_zip_read_tile: can't allocate buffer for decoded image");
    		}
    	}
    }
    else if (dzif==IMAGE_FORMAT_PNG)
    {
    	// to add PNG support, we would require _openslide_png_decode_buffer(buf, buflen, dest, dw, dh, err)
        // until now, PNG is not used by VMIC
    	success = false;
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			                "_openslide_zip_read_tile: no PNG support");
    }
    else
    {
        // BMP is not used. So far only JPG based VMICs exist
        // there's some chance we may need to add JP2K support for future VMICs
    	success = false;
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			                "_openslide_zip_read_tile: unknown image format %i", (int)dzif);
    }
    g_slice_free1(cbufsize, cbuf); // delete buffer with file data
    return success;
}
