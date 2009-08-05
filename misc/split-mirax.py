#!/usr/bin/python

import struct, sys, os


def rr(f):
    return struct.unpack("<i", f.read(4))[0]


filename = sys.argv[1]

f = open(filename)
dir = os.path.dirname(filename)

HEADER_OFFSET = 37

f.seek(HEADER_OFFSET)

filesize = os.stat(sys.argv[1]).st_size
num_items = (filesize - HEADER_OFFSET) / 4

# read first pointer
top = rr(f)

# seek
f.seek(top)

# read table pointers
table = []

while True:
    ptr = rr(f)
    if ptr == 0:
        break

    table.append(ptr)


print "table: " + str(table)


# read each item
for ptr in table:
    print ptr
    f.seek(ptr)
    rr(f)  # 0
    newptr = rr(f)

    # seek
    f.seek(newptr)

    while True:
        # read page stuff
        pages = rr(f)
        nextptr = rr(f)

        while pages != 0:
            tileno = rr(f)
            fileoffset = rr(f)
            filelen = rr(f)
            fileno = rr(f)

            pages = pages - 1

            # open file and get data
            filename = os.path.join(dir, "Data%0.4d.dat" % (fileno))
            ff = open(filename)
            ff.seek(fileoffset)
            data = ff.read(filelen)
            ff.close()

            # write it
            outfilename = "Data%0.4d_%0.10d.jpg" % (fileno, tileno)
            of = open(outfilename, "w")
            of.write(data)
            of.close()

        if nextptr == 0:
            break
