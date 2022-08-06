#!/usr/bin/python3

import sys
import zlib

for name in sys.argv[1:]:
    with open(name, 'rb') as fh:
        uncompressed = fh.read()
    compressed = zlib.compress(uncompressed, 9)
    print(f'  &(const struct synthetic_item){{')
    print(f'    .name = "{name}",')
    print(f'    .description = "fill this in",')
    print(f'    .is_valid = true,')
    print(f'    .is_image = true,')
    print(f'    .decode = decode_{name},')
    print(f'    .uncompressed_size = {len(uncompressed)},')
    print(f'    .compressed_size = {len(compressed)},')
    print(f'    .compressed_data = (const uint8_t[]){{')
    for i in range(0, len(compressed), 12):
        print('      ', end='')
        for b in compressed[i:i + 12]:
            print(f' 0x{b:02x},', end='')
        print()
    print(f'    }}')
    print(f'  }},')
