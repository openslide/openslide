#!/usr/bin/python

import struct, sys, os

f = open(sys.argv[1])

HEADER_OFFSET = 296

f.seek(HEADER_OFFSET)

while True:
    x1 = struct.unpack("<h", f.read(2))[0]
    x2 = struct.unpack("<h", f.read(2))[0]
    y1 = struct.unpack("<h", f.read(2))[0]
    y2 = struct.unpack("<h", f.read(2))[0]
    zz = f.read(1)

    print '%10s %10s %10s %10s %s' % (x1, x2, y1, y2, zz)
