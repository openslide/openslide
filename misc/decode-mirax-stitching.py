#!/usr/bin/python

import struct, sys, os

f = open(sys.argv[1])

HEADER_OFFSET = 296

f.seek(HEADER_OFFSET)

while True:
    x = struct.unpack("<i", f.read(4))[0]
    y = struct.unpack("<i", f.read(4))[0]
    zz = f.read(1)

    print '%10s %10s' % (x, y)
