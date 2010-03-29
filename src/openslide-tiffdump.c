/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
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
 *
 * This file is derived from tiffdump.c:
 *
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 *
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 *
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-tiffdump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>


#include <tiffio.h>


static int datawidth[] = {
    0,	/* nothing */
    1,	/* TIFF_BYTE */
    1,	/* TIFF_ASCII */
    2,	/* TIFF_SHORT */
    4,	/* TIFF_LONG */
    8,	/* TIFF_RATIONAL */
    1,	/* TIFF_SBYTE */
    1,	/* TIFF_UNDEFINED */
    2,	/* TIFF_SSHORT */
    4,	/* TIFF_SLONG */
    8,	/* TIFF_SRATIONAL */
    4,	/* TIFF_FLOAT */
    8,	/* TIFF_DOUBLE */
    4	/* TIFF_IFD */
};



static	off_t ReadDirectory(int, unsigned, off_t);
static	void ReadError(char*);
static	void Error(const char*, ...);
static	void Fatal(const char*, ...);

/* returns list of hashtables of (ttag_t -> struct _openslide_tiffdump) */
GSList *_openslide_tiffdump(FILE *f) {
  GSList *result = NULL;
  TIFFHeader hdr;

  fseeko(f, 0, SEEK_SET);
  if (fread(&hdr, sizeof (hdr), 1, f) != 1) {
    ReadError("TIFF header");
  }

  /*
   * Check magic
   */
  if (hdr.tiff_magic != TIFF_BIGENDIAN && hdr.tiff_magic != TIFF_LITTLEENDIAN) {
    Fatal("Not a TIFF file, bad magic number %u (%#x)",
	  hdr.tiff_magic, hdr.tiff_magic);
  }

  /*
   * Setup the byte order handling.
   */
  bool swap = ((hdr.tiff_magic == TIFF_BIGENDIAN) && !HOST_BIGENDIAN) ||
    ((hdr.tiff_magic == TIFF_LITTLEENDIAN) && HOST_BIGENDIAN);

  /*
   * Swap header if required.
   */
  if (swap) {
    TIFFSwabShort(&hdr.tiff_version);
    TIFFSwabLong(&hdr.tiff_diroff);
  }

  /*
   * Now check version (if needed, it's been byte-swapped).
   * Note that this isn't actually a version number, it's a
   * magic number that doesn't change (stupid).
   */
  if (hdr.tiff_version != TIFF_VERSION) {
    Fatal("Not a TIFF file, bad version number %u (%#x)",
	  hdr.tiff_version, hdr.tiff_version);
  }

  printf("Magic: %#x <%s-endian> Version: %#x\n",
	 hdr.tiff_magic,
	 hdr.tiff_magic == TIFF_BIGENDIAN ? "big" : "little",
	 hdr.tiff_version);


  // initialize loop detector
  GHashTable *loop_detector = g_hash_table_new_full(_openslide_int64_hash,
						    _openslide_int64_equal,
						    _openslide_int64_free,
						    NULL);
  // read all the directories
  int64_t diroff = hdr.tiff_diroff;
  while (diroff != 0) {
    GHashTable *ht = ReadDirectory(fd, i, &diroff, loop_detector);
    result = g_slist_prepend(result, ht);
  }
  g_hash_table_unref(loop_detector);

  return g_slist_reverse(result);
}

static	int TIFFFetchData(int, TIFFDirEntry*, void*);

/*
 * Read the next TIFF directory from a file
 * and convert it to the internal format.
 * We read directories sequentially.
 */
static off_t
ReadDirectory(int fd, unsigned ix, int64_t *diroff)
{
	register TIFFDirEntry *dp;
	register unsigned int n;
	TIFFDirEntry *dir = 0;
	uint16 dircount;
	int space;
	off_t off = *diroff;
	uint32 nextdiroff = 0;

	if (off == 0)			/* no more directories */
		goto done;
	if (lseek(fd, (off_t) off, 0) != off) {
		Fatal("Seek error accessing TIFF directory");
		goto done;
	}
	if (read(fd, (char*) &dircount, sizeof (uint16)) != sizeof (uint16)) {
		ReadError("directory count");
		goto done;
	}
	if (swabflag)
		TIFFSwabShort(&dircount);
	dir = (TIFFDirEntry *)_TIFFmalloc(dircount * sizeof (TIFFDirEntry));
	if (dir == NULL) {
		Fatal("No space for TIFF directory");
		goto done;
	}
	n = read(fd, (char*) dir, dircount*sizeof (*dp));
	if (n != dircount*sizeof (*dp)) {
		n /= sizeof (*dp);
		Error(
	    "Could only read %u of %u entries in directory at offset %#lx",
		    n, dircount, (unsigned long) off);
		dircount = n;
	}
	if (read(fd, (char*) &nextdiroff, sizeof (uint32)) != sizeof (uint32))
		nextdiroff = 0;
	if (swabflag)
		TIFFSwabLong(&nextdiroff);
	printf("Directory %u: offset %lu (%#lx) next %lu (%#lx)\n", ix,
	    (unsigned long)off, (unsigned long)off,
	    (unsigned long)nextdiroff, (unsigned long)nextdiroff);
	for (dp = dir, n = dircount; n > 0; n--, dp++) {
		if (swabflag) {
			TIFFSwabArrayOfShort(&dp->tdir_tag, 2);
			TIFFSwabArrayOfLong(&dp->tdir_count, 2);
		}
		PrintTag(stdout, dp->tdir_tag);
		putchar(' ');
		PrintType(stdout, dp->tdir_type);
		putchar(' ');
		printf("%lu<", (unsigned long) dp->tdir_count);
		if (dp->tdir_type >= NWIDTHS) {
			printf(">\n");
			continue;
		}
		space = dp->tdir_count * datawidth[dp->tdir_type];
		if (space <= 0) {
			printf(">\n");
			Error("Invalid count for tag %u", dp->tdir_tag);
			continue;
                }
		if (space <= 4) {
			switch (dp->tdir_type) {
			case TIFF_FLOAT:
			case TIFF_UNDEFINED:
			case TIFF_ASCII: {
				unsigned char data[4];
				_TIFFmemcpy(data, &dp->tdir_offset, 4);
				if (swabflag)
					TIFFSwabLong((uint32*) data);
				PrintData(stdout,
				    dp->tdir_type, dp->tdir_count, data);
				break;
			}
			case TIFF_BYTE:
				PrintByte(stdout, bytefmt, dp);
				break;
			case TIFF_SBYTE:
				PrintByte(stdout, sbytefmt, dp);
				break;
			case TIFF_SHORT:
				PrintShort(stdout, shortfmt, dp);
				break;
			case TIFF_SSHORT:
				PrintShort(stdout, sshortfmt, dp);
				break;
			case TIFF_LONG:
				PrintLong(stdout, longfmt, dp);
				break;
			case TIFF_SLONG:
				PrintLong(stdout, slongfmt, dp);
				break;
			case TIFF_IFD:
				PrintLong(stdout, ifdfmt, dp);
				break;
			}
		} else {
			unsigned char *data = (unsigned char *)_TIFFmalloc(space);
			if (data) {
				if (TIFFFetchData(fd, dp, data)) {
					if (dp->tdir_count > maxitems) {
						PrintData(stdout, dp->tdir_type,
						    maxitems, data);
						printf(" ...");
					} else
						PrintData(stdout, dp->tdir_type,
						    dp->tdir_count, data);
                                }
				_TIFFfree(data);
			} else
				Error("No space for data for tag %u",
				    dp->tdir_tag);
		}
		printf(">\n");
	}
done:
	if (dir)
		_TIFFfree((char *)dir);
	return (nextdiroff);
}

/*
 * Fetch a contiguous directory item.
 */
static int
TIFFFetchData(int fd, TIFFDirEntry* dir, void* cp)
{
	int cc, w;

	w = (dir->tdir_type < NWIDTHS ? datawidth[dir->tdir_type] : 0);
	cc = dir->tdir_count * w;
	if (lseek(fd, (off_t)dir->tdir_offset, 0) != (off_t)-1
	    && read(fd, cp, cc) != -1) {
		if (swabflag) {
			switch (dir->tdir_type) {
			case TIFF_SHORT:
			case TIFF_SSHORT:
				TIFFSwabArrayOfShort((uint16*) cp,
				    dir->tdir_count);
				break;
			case TIFF_LONG:
			case TIFF_SLONG:
			case TIFF_FLOAT:
			case TIFF_IFD:
				TIFFSwabArrayOfLong((uint32*) cp,
				    dir->tdir_count);
				break;
			case TIFF_RATIONAL:
				TIFFSwabArrayOfLong((uint32*) cp,
				    2*dir->tdir_count);
				break;
			case TIFF_DOUBLE:
				TIFFSwabArrayOfDouble((double*) cp,
				    dir->tdir_count);
				break;
			}
		}
		return (cc);
	}
	Error("Error while reading data for tag %u", dir->tdir_tag);
	return (0);
}
