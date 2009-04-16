#!/usr/bin/python

import struct, sys, os

f = open(sys.argv[1])

HEADER_OFFSET = 37

f.seek(HEADER_OFFSET)

filesize = os.stat(sys.argv[1]).st_size
num_items = (filesize - HEADER_OFFSET) / 4

i = 0

values = []


def is_possible_pointer(num):
    num = (n - HEADER_OFFSET) / 4.0
    return int(num) == num


try:
    while True:
        n = struct.unpack("<i", f.read(4))[0]
        values.append(n)
except:
    pass

values = (values)
seen = [False] * len(values)
