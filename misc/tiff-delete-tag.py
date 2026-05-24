#!/usr/bin/env python3
#
#  Delete one tag from a TIFF file.
#
#  Copyright (c) 2012 Carnegie Mellon University
#  Copyright (c) 2026 Benjamin Gilbert
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

from argparse import ArgumentParser
from contextlib import closing
import struct

class TiffFile:
    def __init__(self, path):
        self._fh = open(path, 'r+b')
        # Check header, decide endianness
        endian = self.read(2)
        if endian == b'II':
            self._fmt_prefix = '<'
        elif endian == b'MM':
            self._fmt_prefix = '>'
        else:
            raise Exception('Not a TIFF file')
        # Check TIFF version
        self._bigtiff = False
        version = self.read_fmt('H')
        if version == 42:
            pass
        elif version == 43:
            self._bigtiff = True
            magic2, reserved = self.read_fmt('HH')
            if magic2 != 8 or reserved != 0:
                raise Exception('Bad BigTIFF header')
        else:
            raise Exception('Not a TIFF file')
        # Leave file offset at pointer to first directory

    def _convert_format(self, fmt):
        # Format strings can have special characters:
        # y: 16-bit   signed on little TIFF, 64-bit   signed on BigTIFF
        # Y: 16-bit unsigned on little TIFF, 64-bit unsigned on BigTIFF
        # z: 32-bit   signed on little TIFF, 64-bit   signed on BigTIFF
        # Z: 32-bit unsigned on little TIFF, 64-bit unsigned on BigTIFF
        if self._bigtiff:
            fmt = fmt.translate(str.maketrans('yYzZ', 'qQqQ'))
        else:
            fmt = fmt.translate(str.maketrans('yYzZ', 'hHiI'))
        return self._fmt_prefix + fmt

    def fmt_size(self, fmt):
        return struct.calcsize(self._convert_format(fmt))

    def read(self, *args):
        return self._fh.read(*args)

    def write(self, *args):
        return self._fh.write(*args)

    def seek(self, *args):
        self._fh.seek(*args)

    def tell(self):
        return self._fh.tell()

    def read_fmt(self, fmt):
        fmt = self._convert_format(fmt)
        vals = struct.unpack(fmt, self.read(struct.calcsize(fmt)))
        if len(vals) == 1:
            return vals[0]
        else:
            return vals

    def write_fmt(self, fmt, *args):
        fmt = self._convert_format(fmt)
        self.write(struct.pack(fmt, *args))

    def close(self):
        self._fh.close()


parser = ArgumentParser()
parser.add_argument('-d', '--directory', type=int, default=0,
        help='directory number')
parser.add_argument('filename', help='TIFF to modify')
parser.add_argument('tag', type=int, help='tag number to remove')
args = parser.parse_args()

directory: int = args.directory

with closing(TiffFile(args.filename)) as fh:
    entry_size = fh.fmt_size('HHZZ')

    # Seek to correct directory
    dir_base = fh.read_fmt('Z')
    fh.seek(dir_base)
    while directory > 0:
        count = fh.read_fmt('Y')
        fh.seek(entry_size * count, 1)
        dir_base = fh.read_fmt('Z')
        if dir_base == 0:
            raise Exception('No such TIFF directory')
        fh.seek(dir_base)
        directory -= 1

    # Find the desired tag
    tag_count = fh.read_fmt('Y')
    tags_remaining = tag_count
    while tags_remaining > 0:
        pos = fh.tell()
        cur_tag, _type, _count, _value = fh.read_fmt('HHZZ')
        tags_remaining -= 1
        if cur_tag == args.tag:
            # Delete it.  Always copy the next-IFD offset as a 64-bit value
            # to support NDPI.
            buf = fh.read(entry_size * tags_remaining + fh.fmt_size('Q'))
            fh.seek(pos)
            fh.write(buf)
            fh.seek(dir_base)
            fh.write_fmt('Y', tag_count - 1)
            break
    else:
        raise Exception('No such tag')
