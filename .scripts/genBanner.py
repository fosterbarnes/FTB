"""Builds the full-width HOME Menu banner model (meta/banner_3ds.cgfx) from a 400x240 image.

Usage: genBanner.py <pycgfx checkout> <400x240 banner.png> <out.cgfx>

HOME Menu views banners through a fixed camera (pycgfx's banner-camera.gltf): 30 degree vertical FOV
from (0, 1, 44.786), so at depth 0 the 240 px screen height spans 24 units and 1 unit is 10 px. Two
flat quads covering x -20..20, y -11..13 at z 0 therefore show the image 1:1 on the top screen, at
screen depth (no 3D offset).

pycgfx (https://github.com/skyfloogle/pycgfx, no license, so not vendored) converts the scene. It caps
textures at 256 px, so each half (200x240) gets its own 256x256 texture. Its materials are lit, which
would shade the flat image, so they are rewritten to output the texture unchanged. Nearest filtering
keeps the 1:1 texels sharp and the seam between the halves exact.
"""
import json
import struct
import sys
import tempfile
from pathlib import Path

from PIL import Image

_WIDTH = 400
_HEIGHT = 240
_TEXTURE = 256
_PX_PER_UNIT = 10
_CENTER_Y = 1
_NEAREST = 9728
_CLAMP = 33071
_FLOAT = 5126
_USHORT = 5123
_MAX_CGFX = 0x80000


def writeScene(image, folder):
    """Writes a two-quad glTF (left and right halves) plus its buffer and textures into folder."""
    half = _WIDTH // 2
    top = _CENTER_Y + _HEIGHT / 2 / _PX_PER_UNIT
    bottom = top - _HEIGHT / _PX_PER_UNIT
    uMax = half / _TEXTURE
    vMax = _HEIGHT / _TEXTURE

    blob = bytearray()
    accessors, views, meshes, nodes, materials, textures, images = [], [], [], [], [], [], []

    def addAccessor(data, componentType, count, kind, extra=None):
        views.append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': len(data)})
        blob.extend(data)
        blob.extend(b'\0' * (-len(blob) % 4))
        accessors.append({'bufferView': len(views) - 1, 'componentType': componentType, 'count': count,
                          'type': kind, **(extra or {})})
        return len(accessors) - 1

    for index, name in enumerate(('left', 'right')):
        x0 = (index * half - _WIDTH / 2) / _PX_PER_UNIT
        x1 = x0 + half / _PX_PER_UNIT
        # Corners: top-left, bottom-left, bottom-right, top-right; glTF texcoords have v=0 at the top.
        positions = [(x0, top, 0), (x0, bottom, 0), (x1, bottom, 0), (x1, top, 0)]
        uvs = [(0, 0), (0, vMax), (uMax, vMax), (uMax, 0)]

        position = addAccessor(b''.join(struct.pack('<3f', *p) for p in positions), _FLOAT, 4, 'VEC3',
                               {'min': [x0, bottom, 0], 'max': [x1, top, 0]})
        normal = addAccessor(struct.pack('<3f', 0, 0, 1) * 4, _FLOAT, 4, 'VEC3')
        texcoord = addAccessor(b''.join(struct.pack('<2f', *uv) for uv in uvs), _FLOAT, 4, 'VEC2')
        indices = addAccessor(struct.pack('<6H', 0, 1, 2, 0, 2, 3), _USHORT, 6, 'SCALAR')

        texture = Image.new('RGB', (_TEXTURE, _TEXTURE))
        texture.paste(image.crop((index * half, 0, index * half + half, _HEIGHT)), (0, 0))
        texture.save(folder / f'{name}.png')

        images.append({'name': name, 'uri': f'{name}.png'})
        textures.append({'source': index, 'sampler': 0})
        materials.append({'name': name, 'pbrMetallicRoughness': {'baseColorTexture': {'index': index}}})
        meshes.append({'name': name, 'primitives': [{'attributes': {'POSITION': position, 'NORMAL': normal,
                       'TEXCOORD_0': texcoord}, 'indices': indices, 'material': index}]})
        nodes.append({'name': name, 'mesh': index})

    (folder / 'banner.bin').write_bytes(blob)
    scene = {
        'asset': {'version': '2.0'},
        'scene': 0,
        'scenes': [{'nodes': list(range(len(nodes)))}],
        'nodes': nodes, 'meshes': meshes, 'materials': materials, 'textures': textures, 'images': images,
        'samplers': [{'magFilter': _NEAREST, 'minFilter': _NEAREST, 'wrapS': _CLAMP, 'wrapT': _CLAMP}],
        'buffers': [{'uri': 'banner.bin', 'byteLength': len(blob)}],
        'bufferViews': views, 'accessors': accessors,
    }
    (folder / 'banner.gltf').write_text(json.dumps(scene))
    return folder / 'banner.gltf'


def makeUnlit(cgfx):
    """Makes every material output its texture as-is: no lighting, no specular."""
    for name in cgfx.data.models['COMMON'].materials:
        material = cgfx.data.models['COMMON'].materials[name]
        material.flags = 0
        combiners = material.fragment_shader.texture_combiners
        # Stage 0 replaces with texture 0; later stages pass the previous stage through.
        combiners[0].src_rgb = combiners[0].src_alpha = 0xFF3
        combiners[0].combine_rgb = combiners[0].combine_alpha = 0
        for combiner in combiners[1:]:
            combiner.src_rgb = combiner.src_alpha = 0xFFF
            combiner.combine_rgb = combiner.combine_alpha = 0


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)

    pycgfxDir, imagePath, outPath = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    if not (pycgfxDir / 'main.py').is_file():
        sys.exit(f"pycgfx not found at '{pycgfxDir}'.")

    sys.path.insert(0, str(pycgfxDir))
    import gltflib
    import main as pycgfx
    from cgfx.txob import TextureFormat

    image = Image.open(imagePath).convert('RGB')
    if image.size != (_WIDTH, _HEIGHT):
        sys.exit(f'{imagePath} is {image.size[0]}x{image.size[1]}, expected {_WIDTH}x{_HEIGHT}.')

    # RGB565 instead of pycgfx's RGBA4: the banner is opaque, and 565 keeps the theme colors closer.
    toTxob = pycgfx.swizzler.to_txob
    pycgfx.swizzler.to_txob = lambda im: toTxob(im, TextureFormat.RGB565)

    with tempfile.TemporaryDirectory() as folder:
        gltf = gltflib.GLTF.load(str(writeScene(image, Path(folder))), load_file_resources=True)
        cgfx = pycgfx.convert_gltf(gltf)

    makeUnlit(cgfx)
    data = pycgfx.write(cgfx)
    if len(data) > _MAX_CGFX:
        sys.exit(f'CGFX is {len(data)} bytes; HOME Menu allows at most {_MAX_CGFX}.')

    outPath.write_bytes(data)


if __name__ == '__main__':
    main()
