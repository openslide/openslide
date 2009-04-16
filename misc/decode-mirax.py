#!/usr/bin/python

import struct, sys, os

f = open(sys.argv[1])

HEADER_OFFSET = 37

f.seek(HEADER_OFFSET)

filesize = os.stat(sys.argv[1]).st_size
num_items = (filesize - HEADER_OFFSET) / 4

skipped = False
i = 0

try:
    while True:
        n = struct.unpack("<i", f.read(4))[0]
        possible_lineno = (n - HEADER_OFFSET) / 4.0

        if possible_lineno < 0 or possible_lineno > num_items \
                or int(possible_lineno) != possible_lineno:
            s = "%7d %11d" % (i, n)
        else:
            s = "%7d %11d %10d    -> %10s" % (i, n, possible_lineno, \
                                                  "%+d" % (possible_lineno-i))

        i = i+1

        if n == 0:
            skipped = True
            continue

        if skipped:
            skipped = False
            print '%7s %11s %10s' % ('.','.','.')

        print s
except:
    pass
