#!/usr/bin/python

import struct, sys, os

f = open(sys.argv[1], "rb")

HEADER_SIZE = 296
STRUCT_SIZE = 9

f.seek(0, 2)
filesize = f.tell()

if (filesize - HEADER_SIZE) % STRUCT_SIZE != 0:
    raise IOError("file not of expected size")

f.seek(HEADER_SIZE)

while True:
    data = f.read(STRUCT_SIZE)
    if len(data) == 0:
        break

    unpacked = struct.unpack("<iiB", data)

    x = int(unpacked[0]) / 256.0
    y = int(unpacked[1]) / 256.0
    zz = int(unpacked[2])

    print '%25.8f %25.8f %10d' % (x, y, zz)

