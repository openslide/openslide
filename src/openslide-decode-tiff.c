/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
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
#include "openslide-decode-tiff.h"
#include "openslide-decode-jpeg.h"

#include <glib.h>
#include <tiffio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cairo.h>

#include "openslide-hash.h"
#include "gdal/port/cpl_vsi.h"

#include "tiffiop.h"
/*
 * Dummy functions to fill the omitted client procedures.
 */
static int
_tiffDummyMapProc(thandle_t fd, void** pbase, toff_t* psize)
{
	(void) fd; (void) pbase; (void) psize;
	return (0);
}

static void
_tiffDummyUnmapProc(thandle_t fd, void* base, toff_t size)
{
	(void) fd; (void) base; (void) size;
}

static TIFF*
_TIFFClientOpen(
	const char* name, const char* mode,
	thandle_t clientdata,
	TIFFReadWriteProc readproc,
	TIFFReadWriteProc writeproc,
	TIFFSeekProc seekproc,
	TIFFCloseProc closeproc,
	TIFFSizeProc sizeproc,
	TIFFMapFileProc mapproc,
	TIFFUnmapFileProc unmapproc
)
{
	static const char module[] = "TIFFClientOpen";
	TIFF *tif;
	int m;
	const char* cp;

	/* The following are configuration checks. They should be redundant, but should not
	 * compile to any actual code in an optimised release build anyway. If any of them
	 * fail, (makefile-based or other) configuration is not correct */
	assert(sizeof(uint8_t) == 1);
	assert(sizeof(int8_t) == 1);
	assert(sizeof(uint16_t) == 2);
	assert(sizeof(int16_t) == 2);
	assert(sizeof(uint32_t) == 4);
	assert(sizeof(int32_t) == 4);
	assert(sizeof(uint64_t) == 8);
	assert(sizeof(int64_t) == 8);
	assert(sizeof(tmsize_t)==sizeof(void*));
	{
		union{
			uint8_t a8[2];
			uint16_t a16;
		} n;
		n.a8[0]=1;
		n.a8[1]=0;
                (void)n;
		#ifdef WORDS_BIGENDIAN
		assert(n.a16==256);
		#else
		assert(n.a16==1);
		#endif
	}

	m = _TIFFgetMode(mode, module);
	if (m == -1)
		goto bad2;
	tif = (TIFF *)_TIFFmalloc((tmsize_t)(sizeof (TIFF) + strlen(name) + 1));
	if (tif == NULL) {
		TIFFErrorExt(clientdata, module, "%s: Out of memory (TIFF structure)", name);
		goto bad2;
	}
	_TIFFmemset(tif, 0, sizeof (*tif));
	tif->tif_name = (char *)tif + sizeof (TIFF);
	strcpy(tif->tif_name, name);
	tif->tif_mode = m &~ (O_CREAT|O_TRUNC);
	tif->tif_curdir = (uint16_t) -1;		/* non-existent directory */
	tif->tif_curoff = 0;
	tif->tif_curstrip = (uint32_t) -1;	/* invalid strip */
	tif->tif_row = (uint32_t) -1;		/* read/write pre-increment */
	tif->tif_clientdata = clientdata;
	if (!readproc || !writeproc || !seekproc || !closeproc || !sizeproc) {
		TIFFErrorExt(clientdata, module,
		    "One of the client procedures is NULL pointer.");
		_TIFFfree(tif);
		goto bad2;
	}
	tif->tif_readproc = readproc;
	tif->tif_writeproc = writeproc;
	tif->tif_seekproc = seekproc;
	tif->tif_closeproc = closeproc;
	tif->tif_sizeproc = sizeproc;
	if (mapproc)
		tif->tif_mapproc = mapproc;
	else
		tif->tif_mapproc = _tiffDummyMapProc;
	if (unmapproc)
		tif->tif_unmapproc = unmapproc;
	else
		tif->tif_unmapproc = _tiffDummyUnmapProc;
	_TIFFSetDefaultCompressionState(tif);    /* setup default state */
	/*
	 * Default is to return data MSB2LSB and enable the
	 * use of memory-mapped files and strip chopping when
	 * a file is opened read-only.
	 */
	tif->tif_flags = FILLORDER_MSB2LSB;
	if (m == O_RDONLY )
		tif->tif_flags |= TIFF_MAPPED;

	#ifdef STRIPCHOP_DEFAULT
	if (m == O_RDONLY || m == O_RDWR)
		tif->tif_flags |= STRIPCHOP_DEFAULT;
	#endif

	/*
	 * Process library-specific flags in the open mode string.
	 * The following flags may be used to control intrinsic library
	 * behavior that may or may not be desirable (usually for
	 * compatibility with some application that claims to support
	 * TIFF but only supports some brain dead idea of what the
	 * vendor thinks TIFF is):
	 *
	 * 'l' use little-endian byte order for creating a file
	 * 'b' use big-endian byte order for creating a file
	 * 'L' read/write information using LSB2MSB bit order
	 * 'B' read/write information using MSB2LSB bit order
	 * 'H' read/write information using host bit order
	 * 'M' enable use of memory-mapped files when supported
	 * 'm' disable use of memory-mapped files
	 * 'C' enable strip chopping support when reading
	 * 'c' disable strip chopping support
	 * 'h' read TIFF header only, do not load the first IFD
	 * '4' ClassicTIFF for creating a file (default)
	 * '8' BigTIFF for creating a file
         * 'D' enable use of deferred strip/tile offset/bytecount array loading.
         * 'O' on-demand loading of values instead of whole array loading (implies D)
	 *
	 * The use of the 'l' and 'b' flags is strongly discouraged.
	 * These flags are provided solely because numerous vendors,
	 * typically on the PC, do not correctly support TIFF; they
	 * only support the Intel little-endian byte order.  This
	 * support is not configured by default because it supports
	 * the violation of the TIFF spec that says that readers *MUST*
	 * support both byte orders.  It is strongly recommended that
	 * you not use this feature except to deal with busted apps
	 * that write invalid TIFF.  And even in those cases you should
	 * bang on the vendors to fix their software.
	 *
	 * The 'L', 'B', and 'H' flags are intended for applications
	 * that can optimize operations on data by using a particular
	 * bit order.  By default the library returns data in MSB2LSB
	 * bit order for compatibility with older versions of this
	 * library.  Returning data in the bit order of the native CPU
	 * makes the most sense but also requires applications to check
	 * the value of the FillOrder tag; something they probably do
	 * not do right now.
	 *
	 * The 'M' and 'm' flags are provided because some virtual memory
	 * systems exhibit poor behavior when large images are mapped.
	 * These options permit clients to control the use of memory-mapped
	 * files on a per-file basis.
	 *
	 * The 'C' and 'c' flags are provided because the library support
	 * for chopping up large strips into multiple smaller strips is not
	 * application-transparent and as such can cause problems.  The 'c'
	 * option permits applications that only want to look at the tags,
	 * for example, to get the unadulterated TIFF tag information.
	 */
	for (cp = mode; *cp; cp++)
		switch (*cp) {
			case 'b':
				#ifndef WORDS_BIGENDIAN
				if (m&O_CREAT)
					tif->tif_flags |= TIFF_SWAB;
				#endif
				break;
			case 'l':
				#ifdef WORDS_BIGENDIAN
				if ((m&O_CREAT))
					tif->tif_flags |= TIFF_SWAB;
				#endif
				break;
			case 'B':
				tif->tif_flags = (tif->tif_flags &~ TIFF_FILLORDER) |
				    FILLORDER_MSB2LSB;
				break;
			case 'L':
				tif->tif_flags = (tif->tif_flags &~ TIFF_FILLORDER) |
				    FILLORDER_LSB2MSB;
				break;
			case 'H':
				tif->tif_flags = (tif->tif_flags &~ TIFF_FILLORDER) |
				    HOST_FILLORDER;
				break;
			case 'M':
				if (m == O_RDONLY)
					tif->tif_flags |= TIFF_MAPPED;
				break;
			case 'm':
				if (m == O_RDONLY)
					tif->tif_flags &= ~TIFF_MAPPED;
				break;
			case 'C':
				if (m == O_RDONLY)
					tif->tif_flags |= TIFF_STRIPCHOP;
				break;
			case 'c':
				if (m == O_RDONLY)
					tif->tif_flags &= ~TIFF_STRIPCHOP;
				break;
			case 'h':
				tif->tif_flags |= TIFF_HEADERONLY;
				break;
			case '8':
				if (m&O_CREAT)
					tif->tif_flags |= TIFF_BIGTIFF;
				break;
			case 'D':
			        tif->tif_flags |= TIFF_DEFERSTRILELOAD;
				break;
			case 'O':
				if( m == O_RDONLY )
					tif->tif_flags |= (TIFF_LAZYSTRILELOAD | TIFF_DEFERSTRILELOAD);
				break;
		}

#ifdef DEFER_STRILE_LOAD
        /* Compatibility with old DEFER_STRILE_LOAD compilation flag */
        /* Probably unneeded, since to the best of my knowledge (E. Rouault) */
        /* GDAL was the only user of this, and will now use the new 'D' flag */
        tif->tif_flags |= TIFF_DEFERSTRILELOAD;
#endif

	/*
	 * Read in TIFF header.
	 */
  TIFFReadWriteProc tif_readproc = tif->tif_readproc;
  thandle_t tif_clientdata = tif->tif_clientdata;
  void* tif_header = &tif->tif_header;
  tmsize_t headerSize = sizeof (TIFFHeaderClassic);
  char buffer [4];
  sprintf(buffer, "%ld", headerSize);
  printf("size = %s", buffer);
  tmsize_t size = VSIFReadL(tif_header, 1, headerSize, (VSILFILE *) clientdata);
	if ((m & O_TRUNC) ||
	    !size) {
		if (tif->tif_mode == O_RDONLY) {
			TIFFErrorExt(tif->tif_clientdata, name,
			    "Cannot read TIFF header");
			goto bad;
		}
		/*
		 * Setup header and write.
		 */
		#ifdef WORDS_BIGENDIAN
		tif->tif_header.common.tiff_magic = (tif->tif_flags & TIFF_SWAB)
		    ? TIFF_LITTLEENDIAN : TIFF_BIGENDIAN;
		#else
		tif->tif_header.common.tiff_magic = (tif->tif_flags & TIFF_SWAB)
		    ? TIFF_BIGENDIAN : TIFF_LITTLEENDIAN;
		#endif
		if (!(tif->tif_flags&TIFF_BIGTIFF))
		{
			tif->tif_header.common.tiff_version = TIFF_VERSION_CLASSIC;
			tif->tif_header.classic.tiff_diroff = 0;
			if (tif->tif_flags & TIFF_SWAB)
				TIFFSwabShort(&tif->tif_header.common.tiff_version);
			tif->tif_header_size = sizeof(TIFFHeaderClassic);
		}
		else
		{
			tif->tif_header.common.tiff_version = TIFF_VERSION_BIG;
			tif->tif_header.big.tiff_offsetsize = 8;
			tif->tif_header.big.tiff_unused = 0;
			tif->tif_header.big.tiff_diroff = 0;
			if (tif->tif_flags & TIFF_SWAB)
			{
				TIFFSwabShort(&tif->tif_header.common.tiff_version);
				TIFFSwabShort(&tif->tif_header.big.tiff_offsetsize);
			}
			tif->tif_header_size = sizeof (TIFFHeaderBig);
		}
		/*
		 * The doc for "fopen" for some STD_C_LIBs says that if you
		 * open a file for modify ("+"), then you must fseek (or
		 * fflush?) between any freads and fwrites.  This is not
		 * necessary on most systems, but has been shown to be needed
		 * on Solaris.
		 */
		TIFFSeekFile( tif, 0, SEEK_SET );
		if (!WriteOK(tif, &tif->tif_header, (tmsize_t)(tif->tif_header_size))) {
			TIFFErrorExt(tif->tif_clientdata, name,
			    "Error writing TIFF header");
			goto bad;
		}
		/*
		 * Setup the byte order handling.
		 */
		if (tif->tif_header.common.tiff_magic == TIFF_BIGENDIAN) {
			#ifndef WORDS_BIGENDIAN
			tif->tif_flags |= TIFF_SWAB;
			#endif
		} else {
			#ifdef WORDS_BIGENDIAN
			tif->tif_flags |= TIFF_SWAB;
			#endif
		}
		/*
		 * Setup default directory.
		 */
		if (!TIFFDefaultDirectory(tif))
			goto bad;
		tif->tif_diroff = 0;
		tif->tif_dirlist = NULL;
		tif->tif_dirlistsize = 0;
		tif->tif_dirnumber = 0;
		return (tif);
	}
	/*
	 * Setup the byte order handling.
	 */
	if (tif->tif_header.common.tiff_magic != TIFF_BIGENDIAN &&
	    tif->tif_header.common.tiff_magic != TIFF_LITTLEENDIAN
	    #if MDI_SUPPORT
	    &&
	    #if HOST_BIGENDIAN
	    tif->tif_header.common.tiff_magic != MDI_BIGENDIAN
	    #else
	    tif->tif_header.common.tiff_magic != MDI_LITTLEENDIAN
	    #endif
	    ) {
		TIFFErrorExt(tif->tif_clientdata, name,
		    "Not a TIFF or MDI file, bad magic number %"PRIu16" (0x%"PRIx16")",
	    #else
	    ) {
		TIFFErrorExt(tif->tif_clientdata, name,
		    "Not a TIFF file, bad magic number %"PRIu16" (0x%"PRIx16")",
	    #endif
		    tif->tif_header.common.tiff_magic,
		    tif->tif_header.common.tiff_magic);
		goto bad;
	}
	if (tif->tif_header.common.tiff_magic == TIFF_BIGENDIAN) {
		#ifndef WORDS_BIGENDIAN
		tif->tif_flags |= TIFF_SWAB;
		#endif
	} else {
		#ifdef WORDS_BIGENDIAN
		tif->tif_flags |= TIFF_SWAB;
		#endif
	}
	if (tif->tif_flags & TIFF_SWAB) 
		TIFFSwabShort(&tif->tif_header.common.tiff_version);
	if ((tif->tif_header.common.tiff_version != TIFF_VERSION_CLASSIC)&&
	    (tif->tif_header.common.tiff_version != TIFF_VERSION_BIG)) {
		TIFFErrorExt(tif->tif_clientdata, name,
		    "Not a TIFF file, bad version number %"PRIu16" (0x%"PRIx16")",
		    tif->tif_header.common.tiff_version,
		    tif->tif_header.common.tiff_version);
		goto bad;
	}
	if (tif->tif_header.common.tiff_version == TIFF_VERSION_CLASSIC)
	{
		if (tif->tif_flags & TIFF_SWAB)
			TIFFSwabLong(&tif->tif_header.classic.tiff_diroff);
		tif->tif_header_size = sizeof(TIFFHeaderClassic);
	}
	else
	{
		if (!ReadOK(tif, ((uint8_t*)(&tif->tif_header) + sizeof(TIFFHeaderClassic)), (sizeof(TIFFHeaderBig) - sizeof(TIFFHeaderClassic))))
		{
			TIFFErrorExt(tif->tif_clientdata, name,
			    "Cannot read TIFF header");
			goto bad;
		}
		if (tif->tif_flags & TIFF_SWAB)
		{
			TIFFSwabShort(&tif->tif_header.big.tiff_offsetsize);
			TIFFSwabLong8(&tif->tif_header.big.tiff_diroff);
		}
		if (tif->tif_header.big.tiff_offsetsize != 8)
		{
			TIFFErrorExt(tif->tif_clientdata, name,
			    "Not a TIFF file, bad BigTIFF offsetsize %"PRIu16" (0x%"PRIx16")",
			    tif->tif_header.big.tiff_offsetsize,
			    tif->tif_header.big.tiff_offsetsize);
			goto bad;
		}
		if (tif->tif_header.big.tiff_unused != 0)
		{
			TIFFErrorExt(tif->tif_clientdata, name,
			    "Not a TIFF file, bad BigTIFF unused %"PRIu16" (0x%"PRIx16")",
			    tif->tif_header.big.tiff_unused,
			    tif->tif_header.big.tiff_unused);
			goto bad;
		}
		tif->tif_header_size = sizeof(TIFFHeaderBig);
		tif->tif_flags |= TIFF_BIGTIFF;
	}
	tif->tif_flags |= TIFF_MYBUFFER;
	tif->tif_rawcp = tif->tif_rawdata = 0;
	tif->tif_rawdatasize = 0;
        tif->tif_rawdataoff = 0;
        tif->tif_rawdataloaded = 0;

	switch (mode[0]) {
		case 'r':
			if (!(tif->tif_flags&TIFF_BIGTIFF))
				tif->tif_nextdiroff = tif->tif_header.classic.tiff_diroff;
			else
				tif->tif_nextdiroff = tif->tif_header.big.tiff_diroff;
			/*
			 * Try to use a memory-mapped file if the client
			 * has not explicitly suppressed usage with the
			 * 'm' flag in the open mode (see above).
			 */
			if (tif->tif_flags & TIFF_MAPPED)
			{
				toff_t n;
				if (TIFFMapFileContents(tif,(void**)(&tif->tif_base),&n))
				{
					tif->tif_size=(tmsize_t)n;
					assert((toff_t)tif->tif_size==n);
				}
				else
					tif->tif_flags &= ~TIFF_MAPPED;
			}
			/*
			 * Sometimes we do not want to read the first directory (for example,
			 * it may be broken) and want to proceed to other directories. I this
			 * case we use the TIFF_HEADERONLY flag to open file and return
			 * immediately after reading TIFF header.
			 */
			if (tif->tif_flags & TIFF_HEADERONLY)
				return (tif);

			/*
			 * Setup initial directory.
			 */
			if (TIFFReadDirectory(tif)) {
				tif->tif_rawcc = (tmsize_t)-1;
				tif->tif_flags |= TIFF_BUFFERSETUP;
				return (tif);
			}
			break;
		case 'a':
			/*
			 * New directories are automatically append
			 * to the end of the directory chain when they
			 * are written out (see TIFFWriteDirectory).
			 */
			if (!TIFFDefaultDirectory(tif))
				goto bad;
			return (tif);
	}
bad:
	tif->tif_mode = O_RDONLY;	/* XXX avoid flush */
        TIFFCleanup(tif);
bad2:
	return ((TIFF*)0);
}

CPL_INLINE static void CPL_IGNORE_RET_VAL_INT(CPL_UNUSED int unused) {}

#define HANDLE_CACHE_MAX 32

struct _openslide_tiffcache {
  char *filename;
  GQueue *cache;
  GMutex lock;
  int outstanding;
};

// not thread-safe, like libtiff
struct tiff_file_handle {
  struct _openslide_tiffcache *tc;
  int64_t offset;
  int64_t size;
};

struct associated_image {
  struct _openslide_associated_image base;
  struct _openslide_tiffcache *tc;
  tdir_t directory;
};

#define SET_DIR_OR_FAIL(tiff, i)					\
  do {									\
    if (!_openslide_tiff_set_dir(tiff, i, err)) {			\
      return false;							\
    }									\
  } while (0)

#define GET_FIELD_OR_FAIL(tiff, tag, type, result)			\
  do {									\
    type tmp;								\
    if (!TIFFGetField(tiff, tag, &tmp)) {				\
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,		\
                  "Cannot get required TIFF tag: %d", tag);		\
      return false;							\
    }									\
    result = tmp;							\
  } while (0)

#undef TIFFSetDirectory
bool _openslide_tiff_set_dir(TIFF *tiff,
                             tdir_t dir,
                             GError **err) {
  if (dir == TIFFCurrentDirectory(tiff)) {
    // avoid libtiff unnecessarily rereading directory contents
    return true;
  }
  if (!TIFFSetDirectory(tiff, dir)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot set TIFF directory %d", dir);
    return false;
  }
  return true;
}
#define TIFFSetDirectory _OPENSLIDE_POISON(_openslide_tiff_set_dir)

bool _openslide_tiff_level_init(TIFF *tiff,
                                tdir_t dir,
                                struct _openslide_level *level,
                                struct _openslide_tiff_level *tiffl,
                                GError **err) {
  // set the directory
  SET_DIR_OR_FAIL(tiff, dir);

  // figure out tile size
  int64_t tw, th;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_TILEWIDTH, uint32_t, tw);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_TILELENGTH, uint32_t, th);

  // get image size
  int64_t iw, ih;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, uint32_t, iw);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, uint32_t, ih);

  // decide whether we can bypass libtiff when reading tiles
  uint16_t compression, planar_config, photometric;
  uint16_t bits_per_sample, samples_per_pixel;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_COMPRESSION, uint16_t, compression);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_PLANARCONFIG, uint16_t, planar_config);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_PHOTOMETRIC, uint16_t, photometric);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_BITSPERSAMPLE, uint16_t, bits_per_sample);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_SAMPLESPERPIXEL, uint16_t, samples_per_pixel);
  bool read_direct =
    compression == COMPRESSION_JPEG &&
    planar_config == PLANARCONFIG_CONTIG &&
    (photometric == PHOTOMETRIC_RGB || photometric == PHOTOMETRIC_YCBCR) &&
    bits_per_sample == 8 &&
    samples_per_pixel == 3;
  //g_debug("directory %d, read_direct %d", dir, read_direct);

  // safe now, start writing
  if (level) {
    level->w = iw;
    level->h = ih;
    // tile size hints
    level->tile_w = tw;
    level->tile_h = th;
  }

  if (tiffl) {
    tiffl->dir = dir;
    tiffl->image_w = iw;
    tiffl->image_h = ih;
    tiffl->tile_w = tw;
    tiffl->tile_h = th;

    // num tiles in each dimension
    tiffl->tiles_across = (iw / tw) + !!(iw % tw);   // integer ceiling
    tiffl->tiles_down = (ih / th) + !!(ih % th);

    tiffl->tile_read_direct = read_direct;
    tiffl->photometric = photometric;
  }

  return true;
}

// clip right/bottom edges of tile in last row/column
bool _openslide_tiff_clip_tile(struct _openslide_tiff_level *tiffl,
                               uint32_t *tiledata,
                               int64_t tile_col, int64_t tile_row,
                               GError **err) {
  return _openslide_clip_tile(tiledata,
                              tiffl->tile_w, tiffl->tile_h,
                              tiffl->image_w - tile_col * tiffl->tile_w,
                              tiffl->image_h - tile_row * tiffl->tile_h,
                              err);
}

static bool tiff_read_region(TIFF *tiff,
                             uint32_t *dest,
                             int64_t x, int64_t y,
                             int32_t w, int32_t h,
                             GError **err) {
  TIFFRGBAImage img;
  char emsg[1024] = "unknown error";
  bool success = false;

  // init
  if (!TIFFRGBAImageOK(tiff, emsg)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Failure in TIFFRGBAImageOK: %s", emsg);
    return false;
  }
  if (!TIFFRGBAImageBegin(&img, tiff, 1, emsg)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Failure in TIFFRGBAImageBegin: %s", emsg);
    return false;
  }
  img.req_orientation = ORIENTATION_TOPLEFT;
  img.col_offset = x;
  img.row_offset = y;

  // draw it
  if (TIFFRGBAImageGet(&img, dest, w, h)) {
    // convert ABGR -> ARGB
    for (uint32_t *p = dest; p < dest + w * h; p++) {
      uint32_t val = GUINT32_SWAP_LE_BE(*p);
      *p = (val << 24) | (val >> 8);
    }
    success = true;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "TIFFRGBAImageGet failed");
    memset(dest, 0, w * h * 4);
  }

  // done
  TIFFRGBAImageEnd(&img);
  return success;
}

static bool decode_jpeg(const void *buf, uint32_t buflen,
                        const void *tables, uint32_t tables_len,  // optional
                        J_COLOR_SPACE space,
                        uint32_t *dest,
                        int32_t w, int32_t h,
                        GError **err) {
  volatile bool result = false;
  jmp_buf env;

  struct jpeg_decompress_struct *cinfo;
  struct _openslide_jpeg_decompress *dc =
    _openslide_jpeg_decompress_create(&cinfo);

  if (setjmp(env) == 0) {
    _openslide_jpeg_decompress_init(dc, &env);

    // load JPEG tables
    if (tables) {
      _openslide_jpeg_mem_src(cinfo, tables, tables_len);
      if (jpeg_read_header(cinfo, false) != JPEG_HEADER_TABLES_ONLY) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Couldn't load JPEG tables");
        goto DONE;
      }
    }

    // set up I/O
    _openslide_jpeg_mem_src(cinfo, buf, buflen);

    // read header
    if (jpeg_read_header(cinfo, true) != JPEG_HEADER_OK) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read JPEG header");
      goto DONE;
    }

    // set color space from TIFF photometric tag (for Aperio)
    cinfo->jpeg_color_space = space;

    // decompress
    if (!_openslide_jpeg_decompress_run(dc, dest, false, w, h, err)) {
      goto DONE;
    }
    result = true;
  } else {
    // setjmp has returned again
    _openslide_jpeg_propagate_error(err, dc);
  }

DONE:
  _openslide_jpeg_decompress_destroy(dc);

  return result;
}

bool _openslide_tiff_read_tile(struct _openslide_tiff_level *tiffl,
                               TIFF *tiff,
                               uint32_t *dest,
                               int64_t tile_col, int64_t tile_row,
                               GError **err) {
  // set directory
  SET_DIR_OR_FAIL(tiff, tiffl->dir);

  if (tiffl->tile_read_direct) {
    // Fast path: read raw data, decode through libjpeg
    // Reading through tiff_read_region() reformats pixel data in three
    // passes: libjpeg converts from planar to R G B, libtiff converts
    // to BGRA, we convert to ARGB.  If we can bypass libtiff when
    // decoding JPEG tiles, we can reduce this to one optimized pass in
    // libjpeg-turbo.

    // read tables
    void *tables;
    uint32_t tables_len;
    if (!TIFFGetField(tiff, TIFFTAG_JPEGTABLES, &tables_len, &tables)) {
      // no separate tables
      tables = NULL;
      tables_len = 0;
    }

    // read data
    void *buf;
    int32_t buflen;
    if (!_openslide_tiff_read_tile_data(tiffl, tiff,
                                        &buf, &buflen,
                                        tile_col, tile_row,
                                        err)) {
      return false;
    }

    // decompress
    bool ret = decode_jpeg(buf, buflen, tables, tables_len,
                           tiffl->photometric == PHOTOMETRIC_YCBCR ? JCS_YCbCr : JCS_RGB,
                           dest,
                           tiffl->tile_w, tiffl->tile_h,
                           err);
    g_free(buf);
    return ret;
  } else {
    // Fallback: read tile through libtiff
    _openslide_performance_warn_once(&tiffl->warned_read_indirect,
                                     "Using slow libtiff read path for "
                                     "directory %d", tiffl->dir);
    return tiff_read_region(tiff, dest,
                            tile_col * tiffl->tile_w, tile_row * tiffl->tile_h,
                            tiffl->tile_w, tiffl->tile_h, err);
  }
}

bool _openslide_tiff_read_tile_data(struct _openslide_tiff_level *tiffl,
                                    TIFF *tiff,
                                    void **_buf, int32_t *_len,
                                    int64_t tile_col, int64_t tile_row,
                                    GError **err) {
  // set directory
  SET_DIR_OR_FAIL(tiff, tiffl->dir);

  // get tile number
  ttile_t tile_no = TIFFComputeTile(tiff,
                                    tile_col * tiffl->tile_w,
                                    tile_row * tiffl->tile_h,
                                    0, 0);

  //g_debug("_openslide_tiff_read_tile_data reading tile %d", tile_no);

  // get tile size
  toff_t *sizes;
  if (TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes) == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot get tile size");
    return false;  // ok, haven't allocated anything yet
  }
  tsize_t tile_size = sizes[tile_no];

  // get raw tile
  tdata_t buf = g_malloc(tile_size);
  tsize_t size = TIFFReadRawTile(tiff, tile_no, buf, tile_size);
  if (size == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read raw tile");
    g_free(buf);
    return false;
  }

  // set outputs
  *_buf = buf;
  *_len = size;
  return true;
}

// sets out-argument to indicate whether the tile data is zero bytes long
// returns false on error
bool _openslide_tiff_check_missing_tile(struct _openslide_tiff_level *tiffl,
                                        TIFF *tiff,
                                        int64_t tile_col, int64_t tile_row,
                                        bool *is_missing,
                                        GError **err) {
  // set directory
  if (!_openslide_tiff_set_dir(tiff, tiffl->dir, err)) {
    return false;
  }

  // get tile number
  ttile_t tile_no = TIFFComputeTile(tiff,
                                    tile_col * tiffl->tile_w,
                                    tile_row * tiffl->tile_h,
                                    0, 0);

  //g_debug("_openslide_tiff_check_missing_tile: tile %d", tile_no);

  // get tile size
  toff_t *sizes;
  if (!TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot get tile size");
    return false;
  }
  tsize_t tile_size = sizes[tile_no];

  // return result
  *is_missing = tile_size == 0;
  return true;
}

static bool _get_associated_image_data(TIFF *tiff,
                                       struct associated_image *img,
                                       uint32_t *dest,
                                       GError **err) {
  int64_t width, height;

  // g_debug("read TIFF associated image: %d", img->directory);

  SET_DIR_OR_FAIL(tiff, img->directory);

  // ensure dimensions have not changed
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, uint32_t, width);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, uint32_t, height);
  if (img->base.w != width || img->base.h != height) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected associated image size: "
                "expected %"PRId64"x%"PRId64", got %"PRId64"x%"PRId64,
                img->base.w, img->base.h, width, height);
    return false;
  }

  // load the image
  return tiff_read_region(tiff, dest, 0, 0, width, height, err);
}

static bool get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *dest,
                                      GError **err) {
  struct associated_image *img = (struct associated_image *) _img;
  TIFF *tiff = _openslide_tiffcache_get(img->tc, err);
  bool success = false;
  if (tiff) {
    success = _get_associated_image_data(tiff, img, dest, err);
  }
  _openslide_tiffcache_put(img->tc, tiff);
  return success;
}

static void destroy_associated_image(struct _openslide_associated_image *_img) {
  struct associated_image *img = (struct associated_image *) _img;

  g_slice_free(struct associated_image, img);
}

static const struct _openslide_associated_image_ops tiff_associated_ops = {
  .get_argb_data = get_associated_image_data,
  .destroy = destroy_associated_image,
};

static bool _add_associated_image(openslide_t *osr,
                                  const char *name,
                                  struct _openslide_tiffcache *tc,
                                  tdir_t dir,
                                  TIFF *tiff,
                                  GError **err) {
  // set directory
  SET_DIR_OR_FAIL(tiff, dir);

  // get the dimensions
  int64_t w, h;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, uint32_t, w);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, uint32_t, h);

  // check compression
  uint16_t compression;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_COMPRESSION, uint16_t, compression);
  if (!TIFFIsCODECConfigured(compression)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported TIFF compression: %u", compression);
    return false;
  }

  // load into struct
  struct associated_image *img = g_slice_new0(struct associated_image);
  img->base.ops = &tiff_associated_ops;
  img->base.w = w;
  img->base.h = h;
  img->tc = tc;
  img->directory = dir;

  // save
  g_hash_table_insert(osr->associated_images, g_strdup(name), img);

  return true;
}

bool _openslide_tiff_add_associated_image(openslide_t *osr,
                                          const char *name,
                                          struct _openslide_tiffcache *tc,
                                          tdir_t dir,
                                          GError **err) {
  TIFF *tiff = _openslide_tiffcache_get(tc, err);
  bool ret = false;
  if (tiff) {
    ret = _add_associated_image(osr, name, tc, dir, tiff, err);
  }
  _openslide_tiffcache_put(tc, tiff);

  // safe even if successful
  g_prefix_error(err, "Can't read %s associated image: ", name);
  return ret;
}

static tsize_t tiff_do_read(thandle_t th, tdata_t buf, tsize_t size)
{
  return VSIFReadL( buf, 1, size, (VSILFILE *) th );
}

static tsize_t tiff_do_write(thandle_t th, tdata_t buf, tsize_t size)
{
  return VSIFWriteL( buf, 1, size, (VSILFILE *) th );
}

static toff_t tiff_do_seek(thandle_t th, toff_t offset, int whence)
{
  if( VSIFSeekL( (VSILFILE *) th, offset, whence ) == 0 )
    return (toff_t) VSIFTellL( (VSILFILE *) th );
  else
    return (toff_t) -1;
}

static int tiff_do_close(thandle_t th)
{
  return VSIFCloseL( (VSILFILE *) th );
}

static toff_t tiff_do_size(thandle_t th)
{
  vsi_l_offset  old_off;
  toff_t        file_size;

  old_off = VSIFTellL( (VSILFILE *) th );
  CPL_IGNORE_RET_VAL_INT(VSIFSeekL( (VSILFILE *) th, 0, SEEK_END ));

  file_size = (toff_t) VSIFTellL( (VSILFILE *) th );
  CPL_IGNORE_RET_VAL_INT(VSIFSeekL( (VSILFILE *) th, old_off, SEEK_SET ));

  return file_size;
}

#undef TIFFClientOpen
static TIFF *tiff_open(struct _openslide_tiffcache *tc, GError **err) {
  // open
  VSILFILE *fp = VSIFOpenL( tc->filename, "rb");
  if (fp == NULL) {
    return NULL;
	}

  // read magic
  uint8_t buf[4];
  if (VSIFReadL(buf, 4, 1, fp) != 1) {
    // can't read
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read TIFF magic number for %s", tc->filename);
    VSIFCloseL(fp);
    return NULL;
  }

  // get size
  if (VSIFSeekL(fp, 0, SEEK_END) == -1) {
    _openslide_io_error(err, "Couldn't seek to end of %s", tc->filename);
    VSIFCloseL(fp);
    return NULL;
  }
  int64_t size = VSIFTellL(fp);
  if (size == -1) {
    _openslide_io_error(err, "Couldn't ftello() for %s", tc->filename);
    VSIFCloseL(fp);
    return NULL;
  }
  //VSIFCloseL(fp);

  // check magic
  // TODO: remove if libtiff gets private error/warning callbacks
  if (buf[0] != buf[1]) {
    goto NOT_TIFF;
  }
  uint16_t version;
  switch (buf[0]) {
  case 'M':
    // big endian
    version = (buf[2] << 8) | buf[3];
    break;
  case 'I':
    // little endian
    version = (buf[3] << 8) | buf[2];
    break;
  default:
    goto NOT_TIFF;
  }
  if (version != 42 && version != 43) {
    goto NOT_TIFF;
  }

  // TIFFOpen
  // mode: m disables mmap to avoid sigbus and other mmap fragility
  TIFF *tiff = _TIFFClientOpen(tc->filename, "rm", (thandle_t) fp,
                              tiff_do_read, tiff_do_write, tiff_do_seek,
                              tiff_do_close, tiff_do_size, NULL, NULL);
  if( tiff != NULL ) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid TIFF: %s", tc->filename);
  } else {
      CPL_IGNORE_RET_VAL_INT(VSIFCloseL( fp ));
  }
      
	return tiff;

NOT_TIFF:
  g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "Not a TIFF file: %s", tc->filename);
  return NULL;
}
#define TIFFClientOpen _OPENSLIDE_POISON(_openslide_tiffcache_get)

struct _openslide_tiffcache *_openslide_tiffcache_create(const char *filename) {
  struct _openslide_tiffcache *tc = g_slice_new0(struct _openslide_tiffcache);
  tc->filename = g_strdup(filename);
  tc->cache = g_queue_new();
  g_mutex_init(&tc->lock);
  return tc;
}

TIFF *_openslide_tiffcache_get(struct _openslide_tiffcache *tc, GError **err) {
  //g_debug("get TIFF");
  g_mutex_lock(&tc->lock);
  tc->outstanding++;
  TIFF *tiff = g_queue_pop_head(tc->cache);
  g_mutex_unlock(&tc->lock);

  if (tiff == NULL) {
    //g_debug("create TIFF");
    // Does not check that we have the same file.  Then again, neither does
    // tiff_do_read.
    tiff = tiff_open(tc, err);
  }
  if (tiff == NULL) {
    g_mutex_lock(&tc->lock);
    tc->outstanding--;
    g_mutex_unlock(&tc->lock);
  }
  return tiff;
}

void _openslide_tiffcache_put(struct _openslide_tiffcache *tc, TIFF *tiff) {
  if (tiff == NULL) {
    return;
  }

  //g_debug("put TIFF");
  g_mutex_lock(&tc->lock);
  g_assert(tc->outstanding);
  tc->outstanding--;
  if (g_queue_get_length(tc->cache) < HANDLE_CACHE_MAX) {
    g_queue_push_head(tc->cache, tiff);
    tiff = NULL;
  }
  g_mutex_unlock(&tc->lock);

  if (tiff) {
    //g_debug("too many TIFFs");
    TIFFClose(tiff);
  }
}

void _openslide_tiffcache_destroy(struct _openslide_tiffcache *tc) {
  if (tc == NULL) {
    return;
  }
  g_mutex_lock(&tc->lock);
  TIFF *tiff;
  while ((tiff = g_queue_pop_head(tc->cache)) != NULL) {
    TIFFClose(tiff);
  }
  g_assert(tc->outstanding == 0);
  g_mutex_unlock(&tc->lock);
  g_queue_free(tc->cache);
  g_mutex_clear(&tc->lock);
  g_free(tc->filename);
  g_slice_free(struct _openslide_tiffcache, tc);
}

