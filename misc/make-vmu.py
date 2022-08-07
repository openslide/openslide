#!/usr/bin/python3
#
#  OpenSlide, a library for reading whole slide image files
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

#
# Convert a slide file into a fake Hamamatsu VMU slide that's good enough
# to fool OpenSlide.
#

from configparser import RawConfigParser
from math import gcd
from openslide import OpenSlide
import struct
import sys

BUF_HEIGHT = 512

class VmuLevel:
    def __init__(self, osr, level):
        self._osr = osr
        self.level = level
        self.width, self.height = osr.level_dimensions[level]
        self.column_width = gcd(self.width, 400)
        self.downsample = osr.level_downsamples[level]

    def save(self, path):
        with open(path, 'wb') as fh:
            # Header
            fh.write(struct.pack('<2c2x3i8xi4x', b'G', b'N', self.width,
                    self.height, self.column_width, 32))

            # Body
            for col in range(self.width // self.column_width):
                # BUF_HEIGHT rows at a time
                for i in range((self.height + BUF_HEIGHT - 1) // BUF_HEIGHT):
                    rows = min(BUF_HEIGHT, self.height - i * BUF_HEIGHT)
                    img = self._osr.read_region(
                            (int(col * self.column_width * self.downsample),
                            int(i * BUF_HEIGHT * self.downsample)),
                            self.level,
                            (self.column_width, rows)).load()
                    for y in range(rows):
                        for x in range(self.column_width):
                            pix = [v << 4 for v in img[x, y]]
                            # FIXME: ignores alpha
                            fh.write(struct.pack('<3h', *pix[0:3]))


def make_vmu(in_path, out_base):
    path_conf = out_base + '.vmu'
    path_0 = out_base + '.l0'
    path_1 = out_base + '.l1'

    with OpenSlide(in_path) as osr:
        l0 = VmuLevel(osr, 0)
        l1 = VmuLevel(osr, osr.get_best_level_for_downsample(32))
        for i, l in enumerate([l0, l1]):
            print(f'Level {i}: {l.column_width} pixels/column')
        l0.save(path_0)
        l1.save(path_1)

    section = 'Uncompressed Virtual Microscope Specimen'
    conf = {
        'NoLayers': '1',
        'ImageFile': path_0,
        'MapFile': path_1,
        'BitsPerPixel': '36',
        'PixelOrder': 'RGB',
    }

    c = RawConfigParser()
    c.optionxform = str
    c.add_section(section)
    for k, v in conf.items():
        c.set(section, k, v)
    with open(path_conf, 'w') as fh:
        c.write(fh)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f'Usage: {sys.argv[0]} infile outbase')
        sys.exit(1)
    make_vmu(sys.argv[1], sys.argv[2])
