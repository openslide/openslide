import struct
import sys

class EOFError(Exception):
    pass


def decode(fh, fmt):
    count = struct.calcsize(fmt)
    buf = fh.read(count)
    if not buf:
        raise EOFError
    elif len(buf) < count:
        raise IOError('Short read')
    return struct.unpack(fmt, buf)


with open(sys.argv[1]) as fh:
    try:
        # Walk header
        marker_byte = 0
        while marker_byte != 0xda:  # SOS
            flag, marker_byte = decode(fh, '2B')
            if flag != 0xff:
                raise ValueError('Expected marker, found something else')
            if marker_byte == 0xd8:
                # SOI; no marker segment
                continue
            count = decode(fh, '>H')[0]
            fh.seek(count - 2, 1)

        # Walk entropy-coded data
        base = fh.tell()
        print base
        while True:
            buf = fh.read(4 << 10)
            if not buf:
                break
            off = 0
            while True:
                off = buf.find('\xff', off)
                if off == -1:
                    break
                elif off == len(buf) - 1:
                    ch = fh.read(1)
                    if len(ch) != 1:
                        raise IOError('Short read')
                    buf += ch
                marker_byte = struct.unpack('B', buf[off + 1])[0]
                if marker_byte >= 0xd0 and marker_byte <= 0xd7:
                    print base + off + 2
                off += 2
            base += len(buf)
    except EOFError:
        pass
    except Exception:
        print 'At {}:'.format(fh.tell())
        raise
