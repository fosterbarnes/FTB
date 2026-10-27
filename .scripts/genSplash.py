"""Swaps the ETC1 textures inside the launch splash (meta/logo_3ds.bcma.lz) in place.

Usage: genSplash.py <logo.bcma.lz> <name.bclim>=<tex3ds raw etc1 .bin> ...

The splash is an LZ11-compressed DARC of layouts, animations, and BCLIM textures. Replacements must
keep each texture's size and format, so only texture bytes change and no DARC offsets move.
"""
import struct
import sys

_LOGO_REGION = 0x2000
_ETC1 = 10


def lz11Decompress(src):
    if src[0] != 0x11:
        raise ValueError('not LZ11 data')
    size = src[1] | src[2] << 8 | src[3] << 16
    pos = 4
    out = bytearray()
    while len(out) < size:
        flags = src[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(src[pos])
                pos += 1
                continue
            b = src[pos]
            kind = b >> 4
            if kind == 0:
                length = (((b & 0xF) << 4) | (src[pos + 1] >> 4)) + 0x11
                disp = ((src[pos + 1] & 0xF) << 8) | src[pos + 2]
                pos += 3
            elif kind == 1:
                length = (((b & 0xF) << 12) | (src[pos + 1] << 4) | (src[pos + 2] >> 4)) + 0x111
                disp = ((src[pos + 2] & 0xF) << 8) | src[pos + 3]
                pos += 4
            else:
                length = kind + 1
                disp = ((b & 0xF) << 8) | src[pos + 1]
                pos += 2
            for _ in range(length):
                out.append(out[-disp - 1])
    return bytes(out)


def lz11Compress(data):
    # Greedy matcher over a 4 KiB window, indexed by 3-byte prefixes.
    out = bytearray([0x11, len(data) & 0xFF, (len(data) >> 8) & 0xFF, (len(data) >> 16) & 0xFF])
    chains = {}
    pos = 0
    while pos < len(data):
        flagsAt = len(out)
        out.append(0)
        for bit in range(8):
            if pos >= len(data):
                break
            bestLen, bestDist = 0, 0
            key = data[pos:pos + 3]
            if len(key) == 3:
                for start in reversed(chains.get(key, [])[-64:]):
                    dist = pos - start
                    if dist > 0x1000:
                        break
                    length = 0
                    limit = min(0x10110, len(data) - pos)
                    while length < limit and data[start + length] == data[pos + length]:
                        length += 1
                    if length > bestLen:
                        bestLen, bestDist = length, dist
            step = bestLen if bestLen >= 3 else 1
            if bestLen >= 3:
                out[flagsAt] |= 0x80 >> bit
                disp = bestDist - 1
                if bestLen <= 0x10:
                    out += bytes([((bestLen - 1) << 4) | (disp >> 8), disp & 0xFF])
                elif bestLen <= 0x110:
                    n = bestLen - 0x11
                    out += bytes([n >> 4, ((n & 0xF) << 4) | (disp >> 8), disp & 0xFF])
                else:
                    n = bestLen - 0x111
                    out += bytes([0x10 | (n >> 12), (n >> 4) & 0xFF, ((n & 0xF) << 4) | (disp >> 8), disp & 0xFF])
            else:
                out.append(data[pos])
            for p in range(pos, pos + step):
                chains.setdefault(data[p:p + 3], []).append(p)
            pos += step
    return bytes(out)


def darcFiles(darc):
    """Maps leaf file name -> (offset, size)."""
    magic, _, _, _, _, tableOffset, _, _ = struct.unpack_from('<4sHHIIIII', darc, 0)
    if magic != b'darc':
        raise ValueError('not a DARC archive')
    count = struct.unpack_from('<III', darc, tableOffset)[2]
    namesAt = tableOffset + count * 12
    files = {}
    for i in range(count):
        nameOffset, offset, size = struct.unpack_from('<III', darc, tableOffset + i * 12)
        if nameOffset >> 24:
            continue
        start = namesAt + (nameOffset & 0xFFFFFF)
        end = start
        while darc[end:end + 2] != b'\0\0':
            end += 2
        files[darc[start:end].decode('utf-16-le')] = (offset, size)
    return files


def readTex3dsRaw(path):
    raw = open(path, 'rb').read()
    # tex3ds -z none still writes a 4-byte header: type 0x00, then the 24-bit data size.
    if len(raw) < 4 or raw[0] != 0 or (raw[1] | raw[2] << 8 | raw[3] << 16) != len(raw) - 4:
        raise ValueError(f'{path}: unexpected tex3ds header')
    return raw[4:]


def main(args):
    if len(args) < 2:
        raise SystemExit(__doc__)
    logoPath = args[0]
    darc = bytearray(lz11Decompress(open(logoPath, 'rb').read()))
    files = darcFiles(darc)
    for spec in args[1:]:
        name, binPath = spec.split('=', 1)
        if name not in files:
            raise ValueError(f'{name} not found in {logoPath}')
        offset, size = files[name]
        clim = bytes(darc[offset:offset + size])
        dataSize = clim.rfind(b'CLIM')
        imag = clim.rfind(b'imag')
        width, height, fmt = struct.unpack_from('<HHI', clim, imag + 8)
        if fmt != _ETC1:
            raise ValueError(f'{name}: format {fmt}, expected ETC1')
        texture = readTex3dsRaw(binPath)
        if len(texture) != dataSize:
            raise ValueError(f'{name}: {len(texture)} bytes, expected {dataSize} ({width}x{height})')
        darc[offset:offset + dataSize] = texture
    packed = lz11Compress(bytes(darc))
    if lz11Decompress(packed) != darc:
        raise ValueError('LZ11 round trip mismatch')
    if len(packed) > _LOGO_REGION:
        raise ValueError(f'splash is {len(packed)} bytes, limit {_LOGO_REGION}')
    open(logoPath, 'wb').write(packed + bytes(_LOGO_REGION - len(packed)))
    print(f'{logoPath}: {len(packed)} of {_LOGO_REGION} bytes')


if __name__ == '__main__':
    main(sys.argv[1:])
