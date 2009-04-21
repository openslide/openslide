#!/usr/bin/python

import struct, sys, os

f = open(sys.argv[1])

HEADER_OFFSET = 37

f.seek(HEADER_OFFSET)

filesize = os.stat(sys.argv[1]).st_size
num_items = (filesize - HEADER_OFFSET) / 4

num_skipped = 0
i = 0

try:
    while True:
        n = struct.unpack("<i", f.read(4))[0]
        possible_lineno = (n - HEADER_OFFSET) / 4.0

        if possible_lineno < 0 or possible_lineno >= num_items \
                or int(possible_lineno) != possible_lineno:
            s = "%7d %11d" % (i, n)
        else:
            s = "%7d %11d %10d    -> %10s" % (i, n, possible_lineno, \
                                                  "%+d" % (possible_lineno-i))

        i = i+1

        if n == 0:
            num_skipped = num_skipped + 1
            continue

        if num_skipped > 0:
            print '%7s %11s %10s %30d' % ('.','.','.', num_skipped)
            num_skipped = 0

        print s
except:
    pass
