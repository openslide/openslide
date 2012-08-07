#!/usr/bin/env python
#
#  Delete one tag from a TIFF file.
#
#  Copyright (c) 2012 Carnegie Mellon University
#  All rights reserved.
#
#  OpenSlide is free software: you can redistribute it and/or modify
#  it under the terms of the GNU Lesser General Public License as
#  published by the Free Software Foundation, version 2.1.
#
#  OpenSlide is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
#  GNU Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public
#  License along with OpenSlide. If not, see
#  <http://www.gnu.org/licenses/>.
#

from optparse import OptionParser
import struct
import sys

class TiffFile(file):
    def __init__(self, path):
        file.__init__(self, path, 'r+b')
        # Check header, decide endianness
        endian = self.read(2)
        if endian == 'II':
            self._fmt_prefix = '<'
        elif endian == 'MM':
            self._fmt_prefix = '>'
        else:
            raise IOError('Not a TIFF file')
        if self.read_fmt('H') != 42:
            raise IOError('Not a TIFF file')

    def read_fmt(self, fmt):
        fmt = self._fmt_prefix + fmt
        vals = struct.unpack(fmt, self.read(struct.calcsize(fmt)))
        if len(vals) == 1:
            return vals[0]
        else:
            return vals

    def write_fmt(self, fmt, *args):
        fmt = self._fmt_prefix + fmt
        self.write(struct.pack(fmt, *args))


# Parse command line
parser = OptionParser(usage='%prog [options] filename tag_number')
parser.add_option('-d', '--directory', dest='directory', type=int, default=0,
        help='Directory number (default 0)')
opts, args = parser.parse_args()
if len(args) != 2:
    parser.print_usage()
    sys.exit(2)

# Get arguments
filename, tag = args
directory = opts.directory
tag = int(tag, 0)

with TiffFile(filename) as fh:
    # Seek to correct directory
    fh.seek(4)
    dir_base = fh.read_fmt('I')
    fh.seek(dir_base)
    while directory > 0:
        count = fh.read_fmt('H')
        fh.seek(12 * count, 1)
        dir_base = fh.read_fmt('I')
        if dir_base == 0:
            raise IOError('No such TIFF directory')
        fh.seek(dir_base)
        directory -= 1

    # Find the desired tag
    tag_count = fh.read_fmt('H')
    tags_remaining = tag_count
    while tags_remaining > 0:
        pos = fh.tell()
        cur_tag, _type, _count, _value = fh.read_fmt('HHII')
        tags_remaining -= 1
        if cur_tag == tag:
            # Delete it
            buf = fh.read(12 * tags_remaining + 4)
            fh.seek(pos)
            fh.write(buf)
            fh.seek(dir_base)
            fh.write_fmt('H', tag_count - 1)
            break
    else:
        raise IOError('No such tag')
