#!/usr/bin/python

import struct, sys, os

f = open(sys.argv[1])

i = 0

def printX(bb):
    print ("%5.02x" * 32) % bb

def printB(bb):
    print ("%5d" * 32) % bb

def printH(hh):
    print ("%10d" * 16) % hh

def printI(ii):
    print ("%20d" * 8) % ii

def printF(ff):
    print ("%20g" * 8) % ff

def printD(dd):
    print ("%40g" * 4) % dd

def printQ(qq):
    print ("%40d" * 4) % qq


while True:
    data = f.read(32)
    if not data:
        break

    print i
#    printX(struct.unpack("32B", data))
#    printB(struct.unpack("32b", data))
#    printB(struct.unpack("32B", data))
#    printH(struct.unpack("<16h", data))
#    printH(struct.unpack("<16H", data))
#    printH(struct.unpack(">16h", data))
#    printH(struct.unpack(">16H", data))
    printI(struct.unpack("<8i", data))
#    printI(struct.unpack("<8I", data))
#    printI(struct.unpack(">8i", data))
#    printI(struct.unpack(">8I", data))
#    printQ(struct.unpack("<4q", data))
#    printQ(struct.unpack("<4Q", data))
#    printQ(struct.unpack(">4q", data))
#    printQ(struct.unpack(">4Q", data))
#    printF(struct.unpack(">8f", data))
#    printF(struct.unpack(">8f", data))
#    printD(struct.unpack(">4d", data))
#    printD(struct.unpack(">4d", data))

    print

    i = i + 1
