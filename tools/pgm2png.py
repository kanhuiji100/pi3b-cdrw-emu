#!/usr/bin/env python3
"""Convert the UI's 128x64 P5 PGM dump to a PNG (no external deps)."""
import struct, sys, zlib

def read_pgm(path):
    data = open(path, 'rb').read()
    if not data.startswith(b'P5'):
        raise SystemExit('not a P5 PGM')
    fields = []
    pos = 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos+1].isspace():
            pos += 1
        if data[pos:pos+1] == b'#':
            while pos < len(data) and data[pos:pos+1] != b'\n':
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos+1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, maxv = fields
    pixels = data[pos:pos + w*h]
    return w, h, pixels

def chunk(tag, payload):
    return (struct.pack('>I', len(payload)) + tag + payload +
            struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

def main():
    src, dst = sys.argv[1], sys.argv[2]
    w, h, pix = read_pgm(src)
    raw = b''.join(b'\x00' + pix[y*w:(y+1)*w] for y in range(h))
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    open(dst, 'wb').write(png)

main()
