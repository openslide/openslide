#!/usr/bin/python

import struct, sys, os

HEADER_SIZE = 296
STRUCT_SIZE = 9

def is_correct_size(filename):
    filesize = os.path.getsize(filename)
    return (filesize - HEADER_SIZE) % STRUCT_SIZE == 0


def try_reading(f):
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




for filename in sys.argv[1:]:
    if not is_correct_size(filename):
        print "%s: file not of expected size" % (filename)
    else:
        print filename
        f = open(filename, "rb")
        try:
            try_reading(f)
        finally:
            f.close()
