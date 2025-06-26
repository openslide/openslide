#!/usr/bin/python3

import hashlib
import struct
import sys

def read(fh, fmt):
    buf = fh.read(struct.calcsize(fmt))
    return struct.unpack(fmt, buf)

def check_magic(found, expected):
    assert found.decode().rstrip('\0') == expected

h = hashlib.sha256()
with open(sys.argv[1], 'rb') as fh:
    magic, guids, offset = read(fh, '<16s32x32s12xQ')
    check_magic(magic, 'ZISRAWFILE')
    h.update(guids)
    fh.seek(offset)

    magic, len = read(fh, '<16s16xL252x')
    check_magic(magic, 'ZISRAWMETADATA')
    h.update(fh.read(len))
    h.update(b'\0')

print(h.hexdigest())
