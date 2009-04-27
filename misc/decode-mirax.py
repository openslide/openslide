#!/usr/bin/python

import struct, sys, os

f = open(sys.argv[1])

HEADER_OFFSET = 37

f.seek(HEADER_OFFSET)

filesize = os.stat(sys.argv[1]).st_size
num_items = (filesize - HEADER_OFFSET) / 4

values = []


def get_pointer(num):
    ptr = (num - HEADER_OFFSET) / 4.0
    if int(ptr) == ptr and ptr > 0 and ptr < num_items:
        return int(ptr)
    else:
        return None


try:
    while True:
        values.append(struct.unpack("<i", f.read(4))[0])
except:
    pass

values = (values)
seen = [False] * len(values)


def decode4s(values, ptr):
    firstvals = []

    while True:
        count = values[ptr]
        ptr = ptr + 2
        for i in range(0, count):
            vals = (values[ptr],
                    values[ptr+1],
                    values[ptr+2],
                    values[ptr+3])
            ptr = ptr + 4
            if vals[3] != 4:
                return firstvals
            firstvals.append(vals[0])


# follow all pointers
ptr = 0
while ptr is not None:
    value = values[ptr]
    #print "values[%d] -> %d" % (ptr, value)
    if value == 0:
        #print "  *** %3d" % (value)
        ptr = ptr + 1
        continue
    elif value == 128:
        items = decode4s(values, ptr)
        for z in items:
            width = 216
            print "%d %d" % (z % width, z / width)
        break
    ptr = get_pointer(value)
