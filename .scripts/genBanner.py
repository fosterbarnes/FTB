"""Builds the 3D HOME Menu banner model (meta/banner_3ds.cgfx) from a Blender scene.

Usage: python -P genBanner.py <banner.blend> <out.cgfx>

The .blend must hold a COMMON empty (exported scene root) -> world empty -> mesh children. HOME Menu
spins the whole model about its vertical axis once per 10 s, so world sits on that axis. Preview the
framing through the scene's Banner Camera, a copy of pycgfx's banner-camera.gltf (30 degree vertical
FOV from glTF (0, 1, 44.786)): only about y 40-212 of the 240 px top screen shows between HOME's bars.

Blender (newest install under Program Files) exports COMMON and its children to glTF; pycgfx
(https://github.com/skyfloogle/pycgfx, no license, so not vendored; pinned checkout in
%LOCALAPPDATA%\\pycgfx) converts it. Then the CGFX gets the fixes from Epic0522/ClouDS-Music-FA's
hardware-tested banner converter: every mesh names its bone, no skinning data may remain, and
materials are made matte (pycgfx's PBR specular washes models out under HOME's light). Textures are
RGB565 instead of pycgfx's RGBA4 (opaque model, smoother shading). Materials are converted
single-sided whatever Blender's Backface Culling says: pycgfx draws a double-sided material by
duplicating every vertex. The double-sided v0.0.51 banner froze HOME Menu on hardware; single-sided
output matches the older working banner's conversion path. This is a candidate fix until tested
on hardware, not proof that double-sided geometry caused the freeze.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

_PYCGFX = Path(os.environ['LOCALAPPDATA']) / 'pycgfx'
_PYCGFX_COMMIT = '1f78850086f3a77c41e07162e842f97a5bf3c18a'
_BLENDER_ROOT = Path(os.environ['ProgramFiles']) / 'Blender Foundation'
_MAX_CGFX = 0x80000

# Runs inside Blender: exports COMMON and its descendants (not the preview camera) with modifiers applied.
_EXPORT = '''
import bpy, sys
out = sys.argv[sys.argv.index("--") + 1]
root = bpy.data.objects.get("COMMON")
if root is None:
    raise SystemExit("no COMMON empty in the scene")
bpy.ops.object.select_all(action="DESELECT")
for ob in (root, *root.children_recursive):
    ob.select_set(True)
bpy.ops.export_scene.gltf(filepath=out, export_format="GLTF_SEPARATE", use_selection=True,
    export_apply=True, export_cameras=False, export_lights=False, export_animations=False,
    export_skins=False, export_morph=False, export_tangents=False, export_vertex_color="NONE",
    export_attributes=False, export_extras=False, export_yup=True)
'''


def findBlender():
    """Returns the blender.exe of the newest 'Blender X.Y' install."""
    installs = []
    for folder in _BLENDER_ROOT.glob('Blender *'):
        match = re.fullmatch(r'Blender (\d+)\.(\d+)', folder.name)
        if match and (folder / 'blender.exe').is_file():
            installs.append(((int(match[1]), int(match[2])), folder / 'blender.exe'))
    if not installs:
        sys.exit(f"No Blender install found under '{_BLENDER_ROOT}'.")
    return max(installs)[1]


def exportScene(blendPath, gltfPath):
    result = subprocess.run([findBlender(), '--background', '--factory-startup', blendPath,
                             '--python-exit-code', '1', '--python-expr', _EXPORT, '--', gltfPath],
                            capture_output=True, text=True)
    if result.returncode != 0 or not gltfPath.is_file():
        sys.exit(f'Blender glTF export failed ({result.returncode}):\n{result.stdout}\n{result.stderr}')


def makeSingleSided(gltf):
    """Blender exports doubleSided unless a material has Backface Culling on; see the docstring."""
    for material in gltf.model.materials or []:
        material.doubleSided = False


def bindMeshes(model):
    """Sets each SOBJ mesh's mesh_node_name to its glTF node's bone (pycgfx names a mesh
    '<node mesh>_<material>' and leaves the field empty; HOME's renderer looks it up)."""
    bones = [name for name in model.skeleton.bones]
    for mesh in model.meshes.data.contents:
        owners = [b for b in bones if mesh.name == b or mesh.name.startswith(b + '_')]
        if not owners:
            sys.exit(f"No bone matches mesh '{mesh.name}'; name each mesh object and its mesh data alike.")
        mesh.mesh_node_name = max(owners, key=len)


def assertRigid(model, usage):
    """HOME Menu freezes on soft-skinned banners even where Azahar draws them."""
    for shape in model.shapes.data.contents:
        for primitiveSet in shape.primitive_sets.data.contents:
            if primitiveSet.skinning_mode != 0:
                sys.exit(f"'{shape.name}' uses skinning mode {primitiveSet.skinning_mode}.")
        for attribute in shape.vertex_attributes.data.contents:
            # Interleaved attributes hold several streams; plain ones are their own stream.
            streams = attribute.vertex_streams.data.contents if hasattr(attribute, 'vertex_streams') \
                else (attribute,)
            for stream in streams:
                if stream.usage in (usage.BoneIndex, usage.BoneWeight):
                    sys.exit(f"'{shape.name}' still has bone indices or weights.")


def makeMatte(model, colorFloat, lightingFlags):
    """Keeps diffuse lighting but bypasses the specular combiner stage and its lookup table."""
    for name in model.materials:
        material = model.materials[name]
        material.material_color.constant[0] = colorFloat(0, 0, 0, 1)
        specular = material.fragment_shader.texture_combiners[2]
        specular.src_rgb = 0xFFF
        specular.combine_rgb = 0
        material.fragment_shader.fragment_lighting.flags = lightingFlags(0)
        material.fragment_shader.fragment_lighting_table.distribution_0_sampler = None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    blendPath, outPath = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    if not blendPath.is_file():
        sys.exit(f"'{blendPath}' not found.")
    commit = subprocess.run(['git', '-C', _PYCGFX, 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if commit.returncode != 0 or commit.stdout.strip() != _PYCGFX_COMMIT:
        sys.exit(f"Need pycgfx {_PYCGFX_COMMIT} at '{_PYCGFX}' (git clone, then checkout).")

    sys.path.insert(0, str(_PYCGFX))
    import gltflib
    import main as pycgfx
    from cgfx.mtob import ColorFloat, FragmentLightingFlags
    from cgfx.primitives import VertexAttributeUsage
    from cgfx.txob import TextureFormat

    toTxob = pycgfx.swizzler.to_txob
    pycgfx.swizzler.to_txob = lambda im: toTxob(im, TextureFormat.RGB565)

    with tempfile.TemporaryDirectory() as folder:
        gltfPath = Path(folder) / 'banner.gltf'
        exportScene(blendPath, gltfPath)
        gltf = gltflib.GLTF.load(str(gltfPath), load_file_resources=True)
        makeSingleSided(gltf)
        cgfx = pycgfx.convert_gltf(gltf)

    model = cgfx.data.models['COMMON']
    bindMeshes(model)
    assertRigid(model, VertexAttributeUsage)
    makeMatte(model, ColorFloat, FragmentLightingFlags)
    data = pycgfx.write(cgfx)
    if len(data) > _MAX_CGFX:
        sys.exit(f'CGFX is {len(data)} bytes; HOME Menu allows at most {_MAX_CGFX}. Lower the Decimate ratio.')

    outPath.write_bytes(data)
    print(f'{outPath.name}: {len(data)} bytes')


if __name__ == '__main__':
    main()
