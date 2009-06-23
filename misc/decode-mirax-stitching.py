#!/usr/bin/python

import struct, sys, os

f = open(sys.argv[1])

HEADER_OFFSET = 296

f.seek(HEADER_OFFSET)

try:
    while True:
        x = int(struct.unpack("<i", f.read(4))[0]) / 256.0
        y = int(struct.unpack("<i", f.read(4))[0]) / 256.0
        zz = f.read(1)

        print '%10g %10g' % (x, y)
except:
    pass
