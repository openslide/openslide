#!/usr/bin/python

import struct, sys

f = open(sys.argv[1])

HEADER_OFFSET = 37

f.seek(HEADER_OFFSET)

try:
    while True:
        n = struct.unpack("<i", f.read(4))[0]
        possible_lineno = (n - HEADER_OFFSET) / 4.0
        if possible_lineno < 0 or int(possible_lineno) != possible_lineno:
            print "%11d" % (n)
        else:
            print "%11d %10d" % (n, possible_lineno)
except:
    pass
