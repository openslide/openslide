#!/usr/bin/python

import struct, sys, os


def bin(s):
    return "".join(map(lambda b: b if b=='1' else ".", list("%.8d" % (int(_bin(s))))))
#    return "%.8d" % (int(_bin(s)))

def _bin(s):
    return str(s) if s<=1 else _bin(s>>1) + str(s&1)


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

    int_format = "%11.d."
    hex_format = "   " + "%3.2x" * 8
#    print "%12d:" % (i) + (int_format * 2) % (a0, a1) + hex_format % (d0) + (int_format * 2) % (b0, b1) + hex_format % (d1)
    print "%12d:" % (i) + (int_format * 2) % (a0, a1) + "   " + "".join(map(bin, d0)) + (int_format * 2) % (b0, b1) + "   " + "".join(map(bin, d1))

    i = i + 1
