#!/usr/bin/python3

import struct
import sys

def read(fh, fmt):
    buf = fh.read(struct.calcsize(fmt))
    return struct.unpack(fmt, buf)

def check_magic(found, expected):
    assert found.decode().rstrip('\0') == expected

with open(sys.argv[1], 'rb') as fh:
    magic, offset = read(fh, '<16s76xQ')
    check_magic(magic, 'ZISRAWFILE')
    fh.seek(offset)

    magic, len = read(fh, '<16s16xL252x')
    check_magic(magic, 'ZISRAWMETADATA')
    xml = fh.read(len).decode()
    print(xml)
