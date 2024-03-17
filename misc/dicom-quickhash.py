#!/usr/bin/python3

import hashlib
from pydicom import dcmread
import sys

h = hashlib.sha256()
h.update(dcmread(sys.argv[1])[(0x20,0xe)].value.encode() + b'\0')

print(h.hexdigest())
