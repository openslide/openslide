#!/usr/bin/python

import struct, sys, os


def rr(f):
    return struct.unpack("<i", f.read(4))[0]


filename = sys.argv[1]

f = open(filename)
dir = os.path.dirname(filename)

HEADER_OFFSET = 37

f.seek(HEADER_OFFSET + 4)

filesize = os.stat(sys.argv[1]).st_size
num_items = (filesize - HEADER_OFFSET) / 4

# read first pointer
top = rr(f)

# seek
f.seek(top)

# read table pointers, skip 1st 0 ptr
table = []
first_zero = False
while True:
    ptr = rr(f)
    if ptr == 0:
        if not first_zero:
            first_zero = True
            continue
        else:
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
            rr(f) # 0
            rr(f) # 0
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
            outfilename = "Data%0.4d_noheader.dat" % (fileno)
            of = open(outfilename, "w")
            of.write(data)
            of.close()

        if nextptr == 0:
            break
