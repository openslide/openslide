import struct
import sys

jpeg = 0
last = float('inf')

with open(sys.argv[1]) as fh:
    while True:
        buf = fh.read(40)
        if not buf:
            break
        val = struct.unpack('<q32x', buf)[0]

        if val < last:
            print '=====', jpeg
            jpeg += 1
        last = val

        print val
