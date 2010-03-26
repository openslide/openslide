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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#ifdef HAVE_IO_H
# include <io.h>
#endif

#include <tiffio.h>

#ifndef O_BINARY
# define O_BINARY	0
#endif

int	swabflag;
int	bigendian;
int	typeshift[14];		/* data type shift counts */
long	typemask[14];		/* data type masks */
uint32	maxitems = 24;		/* maximum indirect data items to print */

char*	bytefmt = "%s%#02x";		/* BYTE */
char*	sbytefmt = "%s%d";		/* SBYTE */
char*	shortfmt = "%s%u";		/* SHORT */
char*	sshortfmt = "%s%d";		/* SSHORT */
char*	longfmt = "%s%lu";		/* LONG */
char*	slongfmt = "%s%ld";		/* SLONG */
char*	rationalfmt = "%s%g";		/* RATIONAL */
char*	srationalfmt = "%s%g";		/* SRATIONAL */
char*	floatfmt = "%s%g";		/* FLOAT */
char*	doublefmt = "%s%g";		/* DOUBLE */
char*	ifdfmt = "%s%#04x";		/* IFD offset */

static	void dump(int, off_t);

static	TIFFHeader hdr;

#define	ord(e)	((int)e)

/*
 * Initialize shift & mask tables and byte
 * swapping state according to the file
 * byte order.
 */
static void
InitByteOrder(int magic)
{
	typemask[0] = 0;
	typemask[ord(TIFF_BYTE)] = 0xff;
	typemask[ord(TIFF_SBYTE)] = 0xff;
	typemask[ord(TIFF_UNDEFINED)] = 0xff;
	typemask[ord(TIFF_SHORT)] = 0xffff;
	typemask[ord(TIFF_SSHORT)] = 0xffff;
	typemask[ord(TIFF_LONG)] = 0xffffffff;
	typemask[ord(TIFF_SLONG)] = 0xffffffff;
	typemask[ord(TIFF_IFD)] = 0xffffffff;
	typemask[ord(TIFF_RATIONAL)] = 0xffffffff;
	typemask[ord(TIFF_SRATIONAL)] = 0xffffffff;
	typemask[ord(TIFF_FLOAT)] = 0xffffffff;
	typemask[ord(TIFF_DOUBLE)] = 0xffffffff;
	typeshift[0] = 0;
	typeshift[ord(TIFF_LONG)] = 0;
	typeshift[ord(TIFF_SLONG)] = 0;
	typeshift[ord(TIFF_IFD)] = 0;
	typeshift[ord(TIFF_RATIONAL)] = 0;
	typeshift[ord(TIFF_SRATIONAL)] = 0;
	typeshift[ord(TIFF_FLOAT)] = 0;
	typeshift[ord(TIFF_DOUBLE)] = 0;
	if (magic == TIFF_BIGENDIAN || magic == MDI_BIGENDIAN) {
		typeshift[ord(TIFF_BYTE)] = 24;
		typeshift[ord(TIFF_SBYTE)] = 24;
		typeshift[ord(TIFF_SHORT)] = 16;
		typeshift[ord(TIFF_SSHORT)] = 16;
		swabflag = !bigendian;
	} else {
		typeshift[ord(TIFF_BYTE)] = 0;
		typeshift[ord(TIFF_SBYTE)] = 0;
		typeshift[ord(TIFF_SHORT)] = 0;
		typeshift[ord(TIFF_SSHORT)] = 0;
		swabflag = bigendian;
	}
}

static	off_t ReadDirectory(int, unsigned, off_t);
static	void ReadError(char*);
static	void Error(const char*, ...);
static	void Fatal(const char*, ...);

static void
dump(int fd, off_t diroff)
{
	unsigned i;

	lseek(fd, (off_t) 0, 0);
	if (read(fd, (char*) &hdr, sizeof (hdr)) != sizeof (hdr))
		ReadError("TIFF header");
	/*
	 * Setup the byte order handling.
	 */
	if (hdr.tiff_magic != TIFF_BIGENDIAN && hdr.tiff_magic != TIFF_LITTLEENDIAN &&
#if HOST_BIGENDIAN
	    // MDI is sensitive to the host byte order, unlike TIFF
	    MDI_BIGENDIAN != hdr.tiff_magic )
#else
	    MDI_LITTLEENDIAN != hdr.tiff_magic )
#endif
		Fatal("Not a TIFF or MDI file, bad magic number %u (%#x)",
		    hdr.tiff_magic, hdr.tiff_magic);
	InitByteOrder(hdr.tiff_magic);
	/*
	 * Swap header if required.
	 */
	if (swabflag) {
		TIFFSwabShort(&hdr.tiff_version);
		TIFFSwabLong(&hdr.tiff_diroff);
	}
	/*
	 * Now check version (if needed, it's been byte-swapped).
	 * Note that this isn't actually a version number, it's a
	 * magic number that doesn't change (stupid).
	 */
	if (hdr.tiff_version != TIFF_VERSION)
		Fatal("Not a TIFF file, bad version number %u (%#x)",
		    hdr.tiff_version, hdr.tiff_version); 
	printf("Magic: %#x <%s-endian> Version: %#x\n",
	    hdr.tiff_magic,
	    hdr.tiff_magic == TIFF_BIGENDIAN ? "big" : "little",
	    hdr.tiff_version);
	if (diroff == 0)
	    diroff = hdr.tiff_diroff;
	for (i = 0; diroff != 0; i++) {
		if (i > 0)
			putchar('\n');
		diroff = ReadDirectory(fd, i, diroff);
	}
}

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
#define	NWIDTHS	(sizeof (datawidth) / sizeof (datawidth[0]))
static	int TIFFFetchData(int, TIFFDirEntry*, void*);
static	void PrintTag(FILE*, uint16);
static	void PrintType(FILE*, uint16);
static	void PrintData(FILE*, uint16, uint32, unsigned char*);
static	void PrintByte(FILE*, const char*, TIFFDirEntry*);
static	void PrintShort(FILE*, const char*, TIFFDirEntry*);
static	void PrintLong(FILE*, const char*, TIFFDirEntry*);

/*
 * Read the next TIFF directory from a file
 * and convert it to the internal format.
 * We read directories sequentially.
 */
static off_t
ReadDirectory(int fd, unsigned ix, off_t off)
{
	register TIFFDirEntry *dp;
	register unsigned int n;
	TIFFDirEntry *dir = 0;
	uint16 dircount;
	int space;
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
