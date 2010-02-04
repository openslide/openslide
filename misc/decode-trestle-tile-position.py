#!/usr/bin/python

import struct, sys, os

f = open(sys.argv[1])

i = 0

while True:
    data = f.read(32)
    if not data:
        break

    unpacked = struct.unpack("<2i8B2i8B", data)

    a0 = unpacked[0]
    a1 = unpacked[1]
    b0 = unpacked[10]
    b1 = unpacked[11]

    d0 = unpacked[2:10]
    d1 = unpacked[12:]

    int_format = "%12.d."
    hex_format = "   " + "%3.2x" * 8
    print "%12d:" % (i) + (int_format * 2) % (a0, a1) + hex_format % (unpacked[2:10]) + (int_format * 2) % (b0, b1) + hex_format % (unpacked[12:])

    i = i + 1
