# Generates the first-person view models with Blender: the rifle,
# models/rifle.glb, and the arms that hold it, models/arms.glb. They are
# separate models, so a weapon can be dropped or swapped: the arms'
# "weapon" bone carries the weapon they hold, and each weapon has its own
# set of arm animations (rifle_idle, rifle_fire, rifle_reload, rifle_draw)
# played with the weapon's own (idle, fire, reload, draw; the rifle's
# reload moves its magazine and charging handle). Animations of the same
# name have the same length.
#
# The rifle is extruded profiles, cut by booleans and bevelled. The arms
# are tubes, subdivided. The animations are keyframed as where the weapon
# and the hands go and how the fingers bend; the arms follow by two-bone
# IK, and the poses that hold something close each finger until it
# touches the rifle. Both models get ambient occlusion baked into their
# base color. Deterministic for a given Blender version (5.2). Run from
# the project root, then re-import:
#
#   blender -b --factory-startup --python tools/gen_view_models.py [-- <out_dir>]
#   godot --headless --recovery-mode --path . --import
#
# The glb files' .import settings are part of the output: animations at
# 60 fps, kept whole (remove_immutable_tracks off: a finger that holds
# still in one animation must still be posed by it), the textures
# embedded. Nothing loops: ViewModel restarts idle when it ends.
#
# Everything is computed in Godot's space (meters, +Y up, -Z forward) and
# converted to Blender's (+Z up, -Y forward) when building objects; the
# glTF export converts it back. The rifle's space is the Weapon node's:
# the barrel along -Z through the origin, the muzzle at z = MUZZLE_Z. The
# arms' space is the camera's: they hold the weapon at WEAPON_REST.

import math
import sys

import bmesh
import bpy
import numpy
from bpy_extras import anim_utils
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

# The muzzle (weapon.tscn's Muzzle), meters along -Z.
MUZZLE_Z = -0.495
# Where the arms hold the weapon at rest, from the camera.
WEAPON_REST = Vector((0.17, -0.2, -0.4))
# The bake rate of the animations, frames per second.
FPS = 60
# The ambient occlusion: texture size (pixels), how far it looks (meters),
# and how dark it gets (the darkest base color multiplier).
AO_SIZE = 1024
AO_DISTANCE = 0.04
AO_FLOOR = 0.35

# Shoulders, from the camera; elbows point along POLE_*.
SHOULDER_R = Vector((0.18, -0.26, 0.05))
SHOULDER_L = Vector((-0.10, -0.28, -0.15))
POLE_R = Vector((0.8, -1.0, 0.2))
POLE_L = Vector((-0.6, -1.0, -0.1))
UPPER_ARM = 0.31
FOREARM = 0.29

# The right hand, in its bone's space (the left one is its mirror image):
# +Y from the wrist to the knuckles, the back of the hand +Z, the thumb on
# -X. KNUCKLES is the wrist-to-knuckles distance.
KNUCKLES = 0.09
# Per finger: name, base, segment lengths, radius.
FINGERS = [
    ("index", Vector((-0.026, 0.087, 0.0)), (0.043, 0.026, 0.022), 0.0092),
    ("middle", Vector((-0.008, 0.091, 0.0)), (0.047, 0.029, 0.023), 0.0095),
    ("ring", Vector((0.010, 0.088, 0.0)), (0.044, 0.027, 0.022), 0.009),
    ("pinky", Vector((0.026, 0.080, 0.0)), (0.034, 0.021, 0.019), 0.008),
]
THUMB_BASE = Vector((-0.020, 0.014, -0.006))
THUMB_DIR = Vector((-0.45, 0.6, -0.66))
THUMB_NAIL = Vector((-0.55, 0.0, 0.85))
THUMB_LENGTHS = (0.040, 0.032, 0.027)
THUMB_RADII = (0.0145, 0.0115, 0.0105)
# The palm's cross-sections (rounded rectangles) along Y: y, half width,
# half thickness, z offset.
PALM = [
    (-0.050, 0.024, 0.019, 0.0),
    (-0.010, 0.027, 0.018, 0.0),
    (0.015, 0.035, 0.0165, 0.001),
    (0.045, 0.041, 0.016, 0.002),
    (0.075, 0.042, 0.0145, 0.002),
    (0.090, 0.040, 0.0125, 0.002),
    (0.099, 0.034, 0.0095, 0.002),
    (0.104, 0.022, 0.006, 0.002),
]
# Subdividing shrinks the tubes; their cages are this much fatter.
SUBDIVISION_INFLATE = 1.04

# Finger poses: per finger, the bend of each joint and the spread,
# degrees. Poses that hold something start from these and close round it
# (close_fingers).
FINGER_NAMES = ("thumb", "index", "middle", "ring", "pinky")
# How far each joint may bend while closing (thumb, then fingers).
THUMB_MAX = (55.0, 50.0, 60.0)
FINGER_MAX = (95.0, 105.0, 80.0)
STRAIGHT = {
    "thumb": (-35, 0, 0, 0), "index": (5, 5, 5, 0), "middle": (5, 5, 5, 0),
    "ring": (5, 5, 5, 0), "pinky": (5, 5, 5, 0),
}
OPEN = {
    "thumb": (0, 0, 5, -20), "index": (10, 10, 5, 5), "middle": (12, 12, 6, 0),
    "ring": (14, 12, 6, -4), "pinky": (16, 12, 6, -8),
}
CURLED = {
    "thumb": (0, 0, 0, -15), "index": (10, 10, 5, 0), "middle": (55, 55, 30, 0),
    "ring": (65, 60, 30, -2), "pinky": (70, 60, 30, -4),
}
# Pressing the trigger bends the trigger finger this much more, degrees.
SQUEEZE = (10, 18, 8)
# Relaxing the support hand opens it this much, degrees.
RELAX = 6.0

# Godot's space to Blender's (and its inverse).
TO_BLENDER = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))
TO_GODOT = TO_BLENDER.transposed()
# Mirrors x (the left hand's local geometry).
MIRROR = Matrix.Diagonal((-1.0, 1.0, 1.0, 1.0))

SMOOTH, LINEAR, OUT, IN = range(4)


def deg(d):
    return math.radians(d)


def rotation(axis, degrees):
    return Matrix.Rotation(deg(degrees), 4, Vector(axis))


def euler(x, y, z):
    # Godot's Basis.from_euler (YXZ order), in degrees.
    return rotation((0, 1, 0), y) @ rotation((1, 0, 0), x) @ rotation((0, 0, 1), z)


def xform(rot=None, origin=(0.0, 0.0, 0.0)):
    m = rot.copy() if rot is not None else Matrix.Identity(4)
    m.translation = Vector(origin)
    return m


def translated(m, offset):
    # Godot's Transform3D.translated: offset in the parent's space.
    return Matrix.Translation(Vector(offset)) @ m


def translated_local(m, offset):
    return m @ Matrix.Translation(Vector(offset))


def from_axes(x, y, z, origin=(0.0, 0.0, 0.0)):
    m = Matrix.Identity(4)
    for i in range(3):
        m[i][0], m[i][1], m[i][2] = x[i], y[i], z[i]
    m.translation = Vector(origin)
    return m


def smoothstep(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def lerp_xform(a, b, w):
    la, ra, _ = a.decompose()
    lb, rb, _ = b.decompose()
    if ra.dot(rb) < 0.0:
        rb.negate()
    return Matrix.LocRotScale(la.lerp(lb, w), ra.slerp(rb, w), None)


def to_blender(p):
    return Vector((p[0], -p[2], p[1]))


# --- Scene, objects and materials ---

def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = FPS
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 128
    scene.cycles.seed = 0
    scene.cycles.use_denoising = False
    world = bpy.data.worlds.new("World")
    world.light_settings.distance = AO_DISTANCE
    scene.world = world


def link(obj):
    bpy.context.scene.collection.objects.link(obj)
    return obj


def select_only(objects):
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]


def apply_modifiers(obj):
    select_only([obj])
    for mod in list(obj.modifiers):
        bpy.ops.object.modifier_apply(modifier=mod.name)


def join(objects, name):
    select_only(objects)
    bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    obj.name = name
    obj.data.name = name
    return obj


def linear(srgb):
    return tuple(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in srgb)


def make_material(name, color, metallic, roughness, ao):
    """The base color is the baked occlusion times color (sRGB)."""
    color = linear(color)
    mat = bpy.data.materials.new(name)
    tree = mat.node_tree
    bsdf = tree.nodes["Principled BSDF"]
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    tex = tree.nodes.new("ShaderNodeTexImage")
    tex.image = ao
    mix = tree.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MULTIPLY"
    mix.inputs[0].default_value = 1.0
    mix.inputs[7].default_value = (*color, 1.0)
    tree.links.new(tex.outputs["Color"], mix.inputs[6])
    tree.links.new(mix.outputs[2], bsdf.inputs["Base Color"])
    tree.nodes.active = tex
    return mat


def make_materials(prefix, specs):
    ao = bpy.data.images.new(prefix + "_ao", AO_SIZE, AO_SIZE, alpha=False)
    materials = [make_material(name, *spec, ao) for name, spec in specs]
    return materials, ao


def bake_ao(obj, ao, hide):
    """Unwraps obj and bakes its ambient occlusion into ao, with the
    objects in hide out of the way."""
    for other in hide:
        other.hide_render = True
    select_only([obj])
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=deg(60), island_margin=0.004)
    bpy.ops.object.mode_set(mode="OBJECT")
    # Into a scratch image, from a texture node outside the shading (the
    # materials read ao).
    scratch = bpy.data.images.new("bake", AO_SIZE, AO_SIZE, alpha=False)
    targets = []
    for mat in obj.data.materials:
        node = mat.node_tree.nodes.new("ShaderNodeTexImage")
        node.image = scratch
        mat.node_tree.nodes.active = node
        targets.append((mat, node))
    bake = bpy.context.scene.render.bake
    bake.margin = 8
    bake.use_clear = True
    bpy.ops.object.bake(type="AO")
    for mat, node in targets:
        mat.node_tree.nodes.remove(node)
    pixels = numpy.empty(AO_SIZE * AO_SIZE * 4, dtype=numpy.float32)
    scratch.pixels.foreach_get(pixels)
    bpy.data.images.remove(scratch)
    pixels = pixels.reshape(-1, 4)
    pixels[:, :3] = AO_FLOOR + (1.0 - AO_FLOOR) * pixels[:, :3]
    ao.pixels.foreach_set(pixels.ravel())
    ao.file_format = "PNG"
    ao.pack()
    for other in hide:
        other.hide_render = False


# --- Shapes: bmesh geometry, given in Godot's space ---

def polygon_area(p):
    return 0.5 * sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1]
                     for i in range(len(p)))


# Frames mapping a profile's (u, v) and the extrusion's w to Godot's space.
SIDE = Matrix(((0, 0, 1), (0, 1, 0), (1, 0, 0)))  # (z, y) profiles along x
ALONG = Matrix.Identity(3)  # (x, y) profiles along z


def extrude(bm, profile, w0, w1, frame=SIDE, material=0):
    """A prism: profile, in the (u, v) plane, from w0 to w1 along w; frame
    (3x3, or 4x4 to place it too) maps (u, v, w) to Godot's space."""
    p = list(profile)
    if (polygon_area(p) < 0.0) != (frame.determinant() < 0.0):
        p.reverse()
    bottom = [bm.verts.new(to_blender(frame @ Vector((u, v, w0)))) for u, v in p]
    top = [bm.verts.new(to_blender(frame @ Vector((u, v, w1)))) for u, v in p]
    faces = [bm.faces.new(list(reversed(bottom))), bm.faces.new(top)]
    for i in range(len(p)):
        j = (i + 1) % len(p)
        faces.append(bm.faces.new((bottom[i], bottom[j], top[j], top[i])))
    for f in faces:
        f.material_index = material
    return faces


def box(bm, lo, hi, material=0):
    profile = [(lo[2], lo[1]), (hi[2], lo[1]), (hi[2], hi[1]), (lo[2], hi[1])]
    return extrude(bm, profile, lo[0], hi[0], SIDE, material)


def circle(r, sides, center=(0.0, 0.0), phase=0.0):
    return [(center[0] + r * math.cos(phase + math.tau * i / sides),
             center[1] + r * math.sin(phase + math.tau * i / sides)) for i in range(sides)]


def cylinder(bm, x, y, z0, z1, r, sides=32, material=0, phase=0.0):
    return extrude(bm, circle(r, sides, (x, y), phase), z0, z1, ALONG, material)


def superellipse(a, b, segments, exponent=2.0):
    points = []
    for j in range(segments):
        t = math.tau * j / segments
        c, s = math.cos(t), math.sin(t)
        points.append((a * math.copysign(abs(c) ** (2.0 / exponent), c),
                       b * math.copysign(abs(s) ** (2.0 / exponent), s)))
    return points


def tube(bm, rings, weights, tips=(None, None), material=0):
    """Quads between rings (loops of points, all the same size), capped by
    fans to the tips (None: open). weights[i] is ring i's {group: weight}
    (and its tip's)."""
    deform = bm.verts.layers.deform.verify()

    def vert(p, w):
        v = bm.verts.new(to_blender(p))
        for group, weight in w.items():
            v[deform][group] = weight
        return v

    rows = [[vert(p, w) for p in ring] for ring, w in zip(rings, weights)]
    n = len(rings[0])
    faces = []
    for a, b in zip(rows, rows[1:]):
        for j in range(n):
            k = (j + 1) % n
            faces.append(bm.faces.new((a[j], a[k], b[k], b[j])))
    for row, tip, w in ((rows[0], tips[0], weights[0]), (rows[-1], tips[1], weights[-1])):
        if tip is not None:
            t = vert(tip, w)
            for j in range(n):
                faces.append(bm.faces.new((row[j], row[(j + 1) % n], t)))
    for f in faces:
        f.material_index = material
    return faces


def finish(bm, name, materials, sharp_angle=None):
    """The object of bm: normals outward, shaded smooth (but for edges
    sharper than sharp_angle, degrees)."""
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    for f in bm.faces:
        f.smooth = True
    if sharp_angle is not None:
        for e in bm.edges:
            if e.is_manifold and e.calc_face_angle(0.0) > deg(sharp_angle):
                e.smooth = False
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    for m in materials:
        mesh.materials.append(m)
    return link(bpy.data.objects.new(name, mesh))


def solid(name, materials, build, bevel, cutters=None, segments=2):
    """A hard surface part: build(bm) makes its pieces, cutters(bm) what
    is cut out of them (their faces keep their materials); then bevelled
    (edges sharper than 30 degrees, bevel meters wide), the bevels'
    normals hardened so the faces next to them stay flat."""
    bm = bmesh.new()
    build(bm)
    obj = finish(bm, name, materials, sharp_angle=30.0)
    if cutters is not None:
        cbm = bmesh.new()
        cutters(cbm)
        cutter = finish(cbm, name + "_cutter", materials)
        mod = obj.modifiers.new("cut", "BOOLEAN")
        mod.operation = "DIFFERENCE"
        mod.solver = "EXACT"
        mod.material_mode = "TRANSFER"
        mod.use_self = True
        mod.object = cutter
    if bevel > 0.0:
        mod = obj.modifiers.new("bevel", "BEVEL")
        mod.width = bevel
        mod.segments = segments
        mod.limit_method = "ANGLE"
        mod.angle_limit = deg(30)
        mod.harden_normals = True
    apply_modifiers(obj)
    if cutters is not None:
        bpy.data.objects.remove(cutter)
    return obj


def make_armature(name, bones):
    """bones: (name, parent, Godot space transform), parents first. The
    bones point along their Y axes."""
    data = bpy.data.armatures.new(name)
    obj = link(bpy.data.objects.new(name, data))
    select_only([obj])
    bpy.ops.object.mode_set(mode="EDIT")
    for bone_name, parent, m in bones:
        eb = data.edit_bones.new(bone_name)
        eb.head = (0.0, 0.0, 0.0)
        eb.tail = (0.0, 0.03, 0.0)
        eb.matrix = TO_BLENDER @ m
        if parent:
            eb.parent = data.edit_bones[parent]
    bpy.ops.object.mode_set(mode="OBJECT")
    for pb in obj.pose.bones:
        pb.rotation_mode = "QUATERNION"
    return obj


def skin(obj, armature, groups):
    """Deform obj by armature; groups are its vertex groups, in the order
    their indices were used."""
    for group in groups:
        obj.vertex_groups.new(name=group)
    obj.parent = armature
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = armature


def rest(armature, bone):
    # A bone's rest transform, in Godot's space.
    return TO_GODOT @ armature.data.bones[bone].matrix_local


# --- The rifle ---

RIFLE_BONES = ("gun", "magazine", "charging_handle")


def build_rifle():
    materials, ao = make_materials("rifle", [
        ("metal", ((0.10, 0.10, 0.11), 0.7, 0.42)),
        ("furniture", ((0.50, 0.42, 0.30), 0.0, 0.72)),
        ("polymer", ((0.06, 0.06, 0.065), 0.0, 0.62)),
        ("recess", ((0.02, 0.02, 0.02), 0.3, 0.8)),
        ("lens", ((0.04, 0.07, 0.10), 0.3, 0.05)),
    ])
    METAL, FURNITURE, POLYMER, RECESS, LENS = range(5)
    parts = {bone: [] for bone in RIFLE_BONES}

    def receiver(bm):
        # Upper receiver, lower receiver with the magazine well, and the
        # controls.
        extrude(bm, [(-0.10, -0.012), (0.19, -0.012), (0.19, 0.022), (0.18, 0.031),
                     (-0.10, 0.031)], -0.024, 0.024, material=METAL)
        extrude(bm, [(-0.078, -0.012), (0.19, -0.012), (0.17, -0.036), (0.0, -0.036),
                     (-0.008, -0.060), (-0.082, -0.060), (-0.084, -0.040)],
                -0.022, 0.022, material=METAL)

    def receiver_cuts(bm):
        # Ejection port (right), charging handle slot (left), magazine well.
        box(bm, (0.019, 0.0, 0.03), (0.03, 0.019, 0.095), RECESS)
        box(bm, (-0.03, 0.012, -0.080), (-0.020, 0.022, 0.0), RECESS)
        extrude(bm, [(-0.074, -0.08), (-0.016, -0.08), (-0.016, -0.02), (-0.074, -0.02)],
                -0.013, 0.013, material=RECESS)

    parts["gun"].append(solid("receiver", materials, receiver, 0.0025, receiver_cuts, 3))

    def details(bm):
        box(bm, (0.022, -0.030, -0.004), (0.026, -0.022, 0.004), METAL)
        box(bm, (-0.027, -0.026, 0.080), (-0.022, -0.018, 0.105), METAL)
        box(bm, (-0.026, -0.008, 0.005), (-0.022, 0.004, 0.022), METAL)
        box(bm, (0.0235, -0.002, 0.028), (0.026, 0.0, 0.097), METAL)
        # Trigger guard and trigger.
        box(bm, (-0.007, -0.068, 0.012), (0.007, -0.062, 0.080), METAL)
        box(bm, (-0.007, -0.068, 0.012), (0.007, -0.036, 0.019), METAL)
        extrude(bm, [(0.050, -0.035), (0.056, -0.035), (0.058, -0.049), (0.055, -0.058),
                     (0.050, -0.061), (0.052, -0.050)], -0.003, 0.003, material=METAL)
        # Rear sight, front sight post.
        box(bm, (-0.011, 0.043, 0.15), (0.011, 0.056, 0.175), METAL)
        extrude(bm, [(-0.305, 0.043), (-0.280, 0.043), (-0.286, 0.062), (-0.296, 0.068),
                     (-0.301, 0.062)], -0.008, 0.008, material=METAL)
        # Gas block, buffer tube.
        box(bm, (-0.013, -0.013, -0.365), (0.013, 0.016, -0.34), METAL)
        cylinder(bm, 0.0, 0.006, 0.18, 0.30, 0.0155, material=METAL)

    parts["gun"].append(solid("details", materials, details, 0.001))

    def grip(bm):
        extrude(bm, [(0.075, -0.034), (0.083, -0.070), (0.092, -0.105), (0.104, -0.140),
                     (0.115, -0.152), (0.150, -0.151), (0.162, -0.140), (0.150, -0.095),
                     (0.137, -0.055), (0.128, -0.034)], -0.016, 0.016, material=FURNITURE)

    parts["gun"].append(solid("grip", materials, grip, 0.005, segments=3))

    def stock(bm):
        extrude(bm, [(0.24, 0.026), (0.40, 0.032), (0.405, -0.096), (0.365, -0.094),
                     (0.28, -0.040), (0.24, -0.022)], -0.019, 0.019, material=FURNITURE)
        extrude(bm, [(0.40, 0.034), (0.42, 0.034), (0.42, -0.100), (0.40, -0.100)],
                -0.021, 0.021, material=POLYMER)

    def stock_cuts(bm):
        # A lightening slot through the stock.
        extrude(bm, [(0.29, -0.012), (0.37, 0.004), (0.37, -0.060), (0.33, -0.058)],
                -0.03, 0.03, material=FURNITURE)

    parts["gun"].append(solid("stock", materials, stock, 0.004, stock_cuts, 3))

    def handguard(bm):
        extrude(bm, circle(0.031, 8, phase=math.pi / 8.0), -0.32, -0.10, ALONG, FURNITURE)

    def vents(bm):
        for k in range(3):
            z = -0.29 + 0.06 * k
            for side in (-1.0, 1.0):
                x = side * 0.031
                box(bm, (min(x * 0.85, x * 1.2), -0.006, z), (max(x * 0.85, x * 1.2), 0.006, z + 0.035),
                    RECESS)
            box(bm, (-0.006, -0.04, z), (0.006, -0.025, z + 0.035), RECESS)

    parts["gun"].append(solid("handguard", materials, handguard, 0.003, vents, 3))

    def rail(bm):
        box(bm, (-0.011, 0.028, -0.32), (0.011, 0.038, 0.18), METAL)
        for k in range(49):
            z = -0.315 + 0.01 * k
            box(bm, (-0.0105, 0.037, z), (0.0105, 0.043, z + 0.0055), METAL)

    parts["gun"].append(solid("rail", materials, rail, 0.0007))

    def barrel(bm):
        cylinder(bm, 0.0, 0.0, -0.45, -0.32, 0.0095, material=METAL)
        cylinder(bm, 0.0, 0.0, MUZZLE_Z, -0.445, 0.0135, material=METAL)

    def ports(bm):
        cylinder(bm, 0.0, 0.0, MUZZLE_Z - 0.01, MUZZLE_Z + 0.03, 0.0065, 16, RECESS)
        for k in range(3):
            z = -0.485 + 0.012 * k
            box(bm, (-0.02, -0.004, z), (0.02, 0.004, z + 0.006), RECESS)

    parts["gun"].append(solid("barrel", materials, barrel, 0.0012, ports))

    def sight(bm):
        box(bm, (-0.012, 0.043, 0.00), (0.012, 0.056, 0.06), METAL)
        extrude(bm, circle(0.0185, 32, (0.0, 0.074)), -0.012, 0.072, ALONG, METAL)
        box(bm, (0.016, 0.065, 0.02), (0.026, 0.080, 0.04), METAL)

    def sight_bore(bm):
        extrude(bm, circle(0.0145, 32, (0.0, 0.074)), -0.02, 0.08, ALONG, RECESS)

    parts["gun"].append(solid("sight", materials, sight, 0.002, sight_bore, 3))

    def lens(bm):
        extrude(bm, circle(0.0146, 32, (0.0, 0.074)), 0.02, 0.022, ALONG, LENS)

    parts["gun"].append(solid("lens", materials, lens, 0.0))

    def magazine(bm):
        front, back = [], []
        for k in range(9):
            t = k / 8.0
            y = -0.015 - 0.185 * t
            front.append((-0.074 - 0.040 * t * t, y))
            back.insert(0, (-0.016 - 0.036 * t * t - 0.004 * t, y))
        extrude(bm, front + back, -0.0125, 0.0125, material=POLYMER)
        extrude(bm, [(-0.118, -0.197), (-0.051, -0.197), (-0.050, -0.212), (-0.119, -0.212)],
                -0.0145, 0.0145, material=POLYMER)

    def mag_ribs(bm):
        for side in (-1.0, 1.0):
            for k in range(4):
                t0 = 0.15 + 0.18 * k
                t1 = t0 + 0.1
                profile = [(-0.060 - 0.038 * t * t, -0.015 - 0.185 * t) for t in (t0, t1)]
                profile += [(-0.048 - 0.038 * t * t, -0.015 - 0.185 * t) for t in (t1, t0)]
                x = 0.0125 * side
                extrude(bm, profile, min(x, x - 0.0025 * side), max(x, x - 0.0025 * side),
                        material=POLYMER)

    parts["magazine"].append(solid("magazine", materials, magazine, 0.002, mag_ribs, 3))

    def handle(bm):
        box(bm, (-0.028, 0.013, -0.075), (-0.020, 0.021, -0.062), METAL)
        extrude(bm, circle(0.0075, 24, (-0.0685, 0.017)), -0.045, -0.026, SIDE, METAL)

    parts["charging_handle"].append(solid("charging_handle", materials, handle, 0.0015))

    armature = make_armature("Rifle", [
        ("gun", None, Matrix.Identity(4)),
        ("magazine", "gun", Matrix.Identity(4)),
        ("charging_handle", "gun", Matrix.Identity(4)),
    ])
    # Rigid: each part's vertices follow its bone.
    objects = []
    for bone, objs in parts.items():
        obj = join(objs, bone) if len(objs) > 1 else objs[0]
        group = obj.vertex_groups.new(name=bone)
        group.add(range(len(obj.data.vertices)), 1.0, "REPLACE")
        objects.append(obj)
    gun = join(objects, "Gun")
    gun.parent = armature
    gun.modifiers.new("Armature", "ARMATURE").object = armature
    return armature, gun, ao


def bone_trees(obj, bones):
    """BVH trees of obj's faces (in Godot's space, at rest), per bone."""
    mesh = obj.data
    groups = {g.index: g.name for g in obj.vertex_groups}
    verts = [TO_GODOT @ v.co for v in mesh.vertices]
    faces = {bone: [] for bone in bones}
    for poly in mesh.polygons:
        v = mesh.vertices[poly.vertices[0]]
        bone = groups[max(v.groups, key=lambda g: g.weight).group]
        faces[bone].append(tuple(poly.vertices))
    return {bone: BVHTree.FromPolygons(verts, faces[bone]) for bone in bones}


# --- The arms ---

class Arms:
    def __init__(self, trees):
        # The rifle's surfaces, for the fingers to close round.
        self.trees = trees
        self.unreachable = 0.0
        # Hand targets in the rifle's space.
        rake = deg(20.0)
        # Right hand round the pistol grip (raked back): the back of the
        # hand out to the right, fingers wrapping round its front.
        self.grip_r = hand_at((0.036, -0.102, 0.098), (0.0, -math.sin(rake), -math.cos(rake)),
                              (1.0, 0.0, 0.0))
        # Left hand under the handguard, palm up, fingers round its side.
        self.foregrip_l = hand_at((0.018, -0.047, -0.22), (0.82, 0.12, -0.55), (0.0, -1.0, 0.0))
        # Left hand holding the magazine, palm against its left side.
        self.mag_grab_l = hand_at((-0.033, -0.115, -0.090), (0.0, -0.25, -1.0), (-1.0, 0.0, 0.0))
        self.mag_in_hand = self.mag_grab_l.inverted()
        # Left hand pinching the charging handle's knob from below.
        self.handle_grab_l = hand_at((-0.050, 0.000, -0.070), (0.25, 0.55, -0.80),
                                     (-1.0, 0.2, 0.3))
        self.finger_rest = {}  # bone -> rest relative to its parent

    def shoulder(self, side):
        return SHOULDER_R if side == "R" else SHOULDER_L

    def solve(self, side, target):
        """Two-bone IK: the upper arm, forearm and hand reaching for target
        (camera space), the elbow towards the side's pole."""
        shoulder = self.shoulder(side)
        pole = POLE_R if side == "R" else POLE_L
        to = target.translation - shoulder
        self.unreachable = max(self.unreachable, to.length - UPPER_ARM - FOREARM)
        d = min(max(to.length, abs(UPPER_ARM - FOREARM) + 0.01), (UPPER_ARM + FOREARM) * 0.9999)
        direction = to.normalized()
        a = math.acos(min(max((UPPER_ARM ** 2 + d * d - FOREARM ** 2) / (2.0 * UPPER_ARM * d),
                              -1.0), 1.0))
        p = (pole - direction * pole.dot(direction)).normalized()
        elbow = shoulder + (direction * math.cos(a) + p * math.sin(a)) * UPPER_ARM
        wrist = shoulder + direction * d
        # The elbow bends round x.
        hinge = p.cross(direction).normalized()
        y = (elbow - shoulder).normalized()
        upper = from_axes(hinge, y, hinge.cross(y), shoulder)
        # The forearm turns with the hand (its twist shows at the elbow,
        # out of view, rather than at the wrist).
        y = (wrist - elbow).normalized()
        hand_x = target.col[0].xyz
        x = hand_x - y * hand_x.dot(y)
        x = hinge if x.length < 0.1 else x.normalized()
        fore = from_axes(x, y, x.cross(y), elbow)
        hand = target.copy()
        hand.translation = wrist
        return upper, fore, hand

    def bones(self):
        """The skeleton: the weapon, then each arm, straight towards its idle
        hand, bending round the idle pose's hinge."""
        weapon = xform(origin=WEAPON_REST)
        bones = [("weapon", None, weapon)]
        idle = {"R": weapon @ self.grip_r, "L": weapon @ self.foregrip_l}
        for side in ("R", "L"):
            mirror = MIRROR if side == "L" else Matrix.Identity(4)
            upper, _, _ = self.solve(side, idle[side])
            shoulder = self.shoulder(side)
            y = (idle[side].translation - shoulder).normalized()
            x = upper.col[0].xyz
            x = (x - y * x.dot(y)).normalized()
            straight = from_axes(x, y, x.cross(y))
            bones.append(("upper_arm." + side, None, translated(straight, shoulder)))
            bones.append(("forearm." + side, "upper_arm." + side,
                          translated(straight, shoulder + y * UPPER_ARM)))
            hand = idle[side].copy()
            hand.translation = shoulder + y * (UPPER_ARM + FOREARM)
            bones.append(("hand." + side, "forearm." + side, hand))
            # Fingers, mirrored for the left hand (M B M keeps a rotation a
            # rotation).
            thumb_y = THUMB_DIR.normalized()
            thumb_z = (THUMB_NAIL - thumb_y * THUMB_NAIL.dot(thumb_y)).normalized()
            chains = [("thumb", THUMB_BASE, THUMB_LENGTHS, from_axes(thumb_y.cross(thumb_z),
                                                                    thumb_y, thumb_z))]
            chains += [(name, base, lengths, Matrix.Identity(4))
                       for name, base, lengths, _ in FINGERS]
            for finger, base, lengths, orient in chains:
                parent = "hand." + side
                local = translated(mirror @ orient @ mirror, mirror @ base)
                global_ = hand
                for k in range(3):
                    global_ = global_ @ local
                    name = f"{finger}_{k + 1}.{side}"
                    self.finger_rest[name] = local
                    bones.append((name, parent, global_))
                    parent = name
                    local = xform(origin=(0.0, lengths[k], 0.0))
        return bones

    # Fingers.

    def finger_chain(self, hand, side, finger, curl):
        """The finger's segments' transforms for the hand at hand, bent by
        curl (joint bends and spread, degrees)."""
        chain = []
        parent = hand
        spread = 1.0 if side == "R" else -1.0
        for k in range(3):
            local = self.finger_rest[f"{finger}_{k + 1}.{side}"]
            bend = rotation((1, 0, 0), -curl[k])
            if k == 0:
                bend = rotation((0, 0, 1), curl[3] * spread) @ bend
            parent = parent @ xform(local.to_3x3().to_4x4() @ bend, local.translation)
            chain.append(parent)
        return chain

    def touches(self, chain, finger, first, trees):
        for k in range(first, 3):
            length, radius = finger_size(finger, k)
            samples = 5 if k == 2 else 4
            for i in range(1 if k == 0 else 0, samples):
                y = length * (i + 1) / 4.0
                if k == 2 and i == samples - 1:
                    y = length + radius * 0.6
                p = chain[k] @ Vector((0.0, y, 0.0))
                for tree in trees:
                    if signed_distance(tree, p) < radius * 0.85:
                        return True
        return False

    def close_finger(self, hand, side, finger, start, trees):
        curl = list(start)
        limits = THUMB_MAX if finger == "thumb" else FINGER_MAX
        for k in range(3):
            while curl[k] < limits[k]:
                step = list(curl)
                for j in range(k, 3):
                    step[j] = min(step[j] + 2.0, limits[j])
                if self.touches(self.finger_chain(hand, side, finger, step), finger, k, trees):
                    break
                curl = step
        return curl

    def close_fingers(self, hand, side, base, fingers, bones):
        """base with the fingers closed round the given bones' parts, for
        the hand at hand (rifle space): each joint in turn, from the
        knuckle, bends (and those past it with it) until the finger
        touches. The thumb swings to where it closes with the least bend."""
        trees = [self.trees[b] for b in bones]
        pose = {f: list(c) for f, c in base.items()}
        for finger in fingers:
            if finger != "thumb":
                pose[finger] = self.close_finger(hand, side, finger, pose[finger], trees)
                continue
            best, best_bend = None, math.inf
            for spread in range(-40, 45, 5):
                start = list(pose[finger])
                start[3] = spread
                if self.touches(self.finger_chain(hand, side, finger, start), finger, 0, trees):
                    continue
                curl = self.close_finger(hand, side, finger, start, trees)
                closed = any(curl[k] < THUMB_MAX[k] for k in range(3))
                bend = curl[0] + curl[1] + curl[2] + abs(spread) * 0.5
                if closed and bend < best_bend:
                    best, best_bend = curl, bend
            if best is not None:
                pose[finger] = best
        return pose

    def make_poses(self):
        held = ["thumb", "middle", "ring", "pinky"]
        # The trigger finger rests on the trigger, or lies along the
        # receiver.
        resting = self.close_fingers(self.grip_r, "R", STRAIGHT, held, ["gun"])
        self.grip = self.close_fingers(self.grip_r, "R", resting, ["index"], ["gun"])
        self.trigger = bent(self.grip, ["index"], SQUEEZE)
        self.finger_out = resting
        self.foregrip = self.close_fingers(self.foregrip_l, "L", STRAIGHT, FINGER_NAMES, ["gun"])
        self.foregrip_loose = bent(self.foregrip, FINGER_NAMES, (-RELAX,) * 3)
        self.mag_hold = self.close_fingers(self.mag_grab_l, "L", STRAIGHT, FINGER_NAMES,
                                           ["magazine"])
        self.pinch_open = CURLED
        self.pinch = self.close_fingers(self.handle_grab_l, "L", CURLED, ["index", "thumb"],
                                        ["charging_handle"])


def hand_at(knuckles, fingers, back):
    """A hand whose knuckles are at knuckles, its fingers along fingers and
    the back of the hand facing back."""
    y = Vector(fingers).normalized()
    back = Vector(back)
    z = (back - y * back.dot(y)).normalized()
    return from_axes(y.cross(z), y, z, Vector(knuckles) - y * KNUCKLES)


def finger_size(finger, k):
    if finger == "thumb":
        return THUMB_LENGTHS[k], THUMB_RADII[k]
    for name, _, lengths, radius in FINGERS:
        if name == finger:
            return lengths[k], radius * (1.0 - 0.07 * k)


def signed_distance(tree, p):
    location, normal, _, distance = tree.find_nearest(p)
    if location is None:
        return math.inf
    return distance if (p - location).dot(normal) >= 0.0 else -distance


def bent(pose, fingers, delta):
    out = {f: list(c) for f, c in pose.items()}
    for finger in fingers:
        for k in range(3):
            out[finger][k] += delta[k]
    return out


def ring(y, a, b, segments, center=(0.0, 0.0), exponent=2.0, wobble=None):
    points = []
    for j, (x, z) in enumerate(superellipse(a, b, segments, exponent)):
        k = wobble(math.tau * j / segments) if wobble else 1.0
        points.append(Vector((x * k + center[0], y, z * k + center[1])))
    return points


def build_arms(arms):
    materials, ao = make_materials("arms", [
        ("sleeve", ((0.27, 0.29, 0.19), 0.0, 0.95)),
        ("glove", ((0.16, 0.15, 0.14), 0.0, 0.8)),
        ("glove_pad", ((0.24, 0.23, 0.21), 0.0, 0.55)),
    ])
    SLEEVE, GLOVE, PAD = range(3)
    bones = arms.bones()
    armature = make_armature("Arms", bones)
    names = [b[0] for b in bones]
    group = {name: i for i, name in enumerate(names)}
    rests = {name: rest(armature, name) for name in names}

    def at(bone, points):
        return [rests[bone] @ p for p in points]

    bm = bmesh.new()
    pads = bmesh.new()
    for side in ("R", "L"):
        mirror = MIRROR if side == "L" else Matrix.Identity(4)
        upper, fore, hand = (group[f"{b}.{side}"] for b in ("upper_arm", "forearm", "hand"))
        # The sleeve, along the straight arm, bending at the elbow and
        # ending in a cuff over the wrist.
        elbow, wrist = UPPER_ARM, UPPER_ARM + FOREARM
        radius = [(-0.04, 0.045), (0.0, 0.062), (0.08, 0.060), (0.20, 0.054), (elbow, 0.050),
                  (elbow + 0.07, 0.052), (elbow + 0.18, 0.044), (wrist - 0.035, 0.038),
                  (wrist - 0.028, 0.044), (wrist + 0.005, 0.045)]
        rings, weights = [], []
        s = -0.04
        while s <= wrist + 0.005 + 1e-4:
            r = next(r0 + (r1 - r0) * (s - s0) / (s1 - s0)
                     for (s0, r0), (s1, r1) in zip(radius, radius[1:]) if s <= s1 + 1e-6)
            # Folds round the elbow and above the cuff.
            fold = math.exp(-((s - elbow) / 0.06) ** 2) + 0.6 * math.exp(-((s - wrist + 0.06) / 0.04) ** 2)
            phase = s * 40.0

            def wobble(t, fold=fold, phase=phase):
                return 1.0 + 0.05 * fold * math.sin(3.0 * t + phase) + 0.015 * math.sin(5.0 * t - phase * 0.7)

            r *= SUBDIVISION_INFLATE
            rings.append(at(f"upper_arm.{side}", ring(s, r * 1.06, r * 0.94, 16, wobble=wobble)))
            w = smoothstep(elbow - 0.06, elbow + 0.06, s)
            weights.append({upper: 1.0 - w, fore: w} if 0.0 < w < 1.0 else {fore if w >= 1.0 else upper: 1.0})
            s += 0.015 if abs(s - elbow) < 0.08 else 0.03
        # The cuff's lip, and its inside.
        end = wrist + 0.005
        for y in (end, end - 0.04):
            rings.append(at(f"upper_arm.{side}", ring(y, 0.034, 0.032, 16)))
            weights.append({fore: 1.0})
        tube(bm, rings, weights, (rests[f"upper_arm.{side}"] @ Vector((0.0, -0.06, 0.0)),
                                  rests[f"upper_arm.{side}"] @ Vector((0.0, end - 0.045, 0.0))),
             SLEEVE)

        # The palm.
        hand_name = f"hand.{side}"
        rings = [at(hand_name, [mirror @ p for p in ring(y, a * SUBDIVISION_INFLATE,
                                                         b * SUBDIVISION_INFLATE, 16,
                                                         (0.002, z), 2.6)])
                 for y, a, b, z in PALM]
        tube(bm, rings, [{hand: 1.0}] * len(rings),
             (rests[hand_name] @ (mirror @ Vector((0.002, PALM[0][0] - 0.01, 0.0))),
              rests[hand_name] @ (mirror @ Vector((0.002, PALM[-1][0] + 0.006, 0.002)))), GLOVE)
        # The knuckle pad on its back.
        pad = [(0.034 * math.copysign(abs(math.cos(t)) ** 0.5, math.cos(t)) + 0.002,
                0.077 + 0.011 * math.copysign(abs(math.sin(t)) ** 0.5, math.sin(t)))
               for t in (math.tau * i / 24 for i in range(24))]
        faces = extrude(pads, pad, 0.0115, 0.0185, rests[hand_name] @ mirror, PAD)
        deform = pads.verts.layers.deform.verify()
        for v in {v for f in faces for v in f.verts}:
            v[deform][hand] = 1.0

        # The fingers: one tube each, bending at the joints.
        for finger in FINGER_NAMES:
            segments = [group[f"{finger}_{k + 1}.{side}"] for k in range(3)]
            lengths = [finger_size(finger, k)[0] for k in range(3)]
            radii = [finger_size(finger, k)[1] * SUBDIVISION_INFLATE for k in range(3)]
            first = rests[f"{finger}_1.{side}"]
            rings, weights = [], []
            # (y along the straight finger, radius, weights)
            stations = [(-0.012, radii[0] * 1.05, {hand: 1.0}),
                        (0.0, radii[0], {hand: 0.5, segments[0]: 0.5})]
            y = 0.0
            for k in range(3):
                stations.append((y + lengths[k] * 0.5, radii[k], {segments[k]: 1.0}))
                y += lengths[k]
                if k < 2:
                    stations.append((y, (radii[k] + radii[k + 1]) * 0.5 * 0.97,
                                     {segments[k]: 0.5, segments[k + 1]: 0.5}))
            stations.append((y + radii[2] * 0.55, radii[2] * 0.8, {segments[2]: 1.0}))
            for sy, r, w in stations:
                rings.append([first @ (mirror @ p) for p in ring(sy, r, r * 0.85, 8)])
                weights.append(w)
            tube(bm, rings, weights, (first @ Vector((0.0, -0.02, 0.0)),
                                      first @ Vector((0.0, y + radii[2] * 0.95, 0.0))), GLOVE)

    # The vertex groups go in index order, before the modifiers carry the
    # weights over.
    obj = finish(bm, "arms", materials)
    pad_obj = finish(pads, "pads", materials, sharp_angle=30.0)
    for o in (obj, pad_obj):
        for name in names:
            o.vertex_groups.new(name=name)
    sub = obj.modifiers.new("subdivide", "SUBSURF")
    sub.levels = 2
    sub.render_levels = 2
    mod = pad_obj.modifiers.new("bevel", "BEVEL")
    mod.width = 0.002
    mod.segments = 2
    mod.limit_method = "ANGLE"
    apply_modifiers(obj)
    apply_modifiers(pad_obj)
    obj = join([obj, pad_obj], "ArmsMesh")
    obj.parent = armature
    obj.modifiers.new("Armature", "ARMATURE").object = armature
    return armature, obj, ao


# --- Animation ---

def key(t, value, ease=SMOOTH, space="gun"):
    return (t, value, ease, space)


def sample(keys, t):
    if t <= keys[0][0]:
        return keys[0], keys[0], 0.0
    for k0, k1 in zip(keys, keys[1:]):
        if t <= k1[0]:
            w = (t - k0[0]) / max(k1[0] - k0[0], 1e-6)
            if k1[2] == SMOOTH:
                w = smoothstep(0.0, 1.0, w)
            elif k1[2] == OUT:
                w = 1.0 - (1.0 - w) * (1.0 - w)
            elif k1[2] == IN:
                w = w * w
            return k0, k1, w
    return keys[-1], keys[-1], 0.0


def resolve(k, weapon, hand_l):
    if k[3] == "gun":
        return weapon @ k[1]
    if k[3] == "hand.L":
        return hand_l @ k[1]
    return k[1]


def sample_xform(keys, t, weapon=None, hand_l=None):
    weapon = weapon if weapon is not None else Matrix.Identity(4)
    k0, k1, w = sample(keys, t)
    return lerp_xform(resolve(k0, weapon, hand_l), resolve(k1, weapon, hand_l), w)


def sample_float(keys, t):
    k0, k1, w = sample(keys, t)
    return k0[1] + (k1[1] - k0[1]) * w


def sample_fingers(keys, t):
    k0, k1, w = sample(keys, t)
    return {f: [a + (b - a) * w for a, b in zip(k0[1][f], k1[1][f])] for f in k0[1]}


def pose(arms, channels, t):
    """The arms' bones (camera space) and the rifle's (rifle space) at t."""
    arm_pose = {}
    weapon = xform(origin=WEAPON_REST) @ sample_xform(channels["gun"], t)
    arm_pose["weapon"] = weapon
    for side in ("R", "L"):
        target = sample_xform(channels["hand." + side], t, weapon)
        upper, fore, hand = arms.solve(side, target)
        arm_pose["upper_arm." + side] = upper
        arm_pose["forearm." + side] = fore
        arm_pose["hand." + side] = hand
        for finger, curl in sample_fingers(channels["fingers." + side], t).items():
            for k, xf in enumerate(arms.finger_chain(hand, side, finger, curl)):
                arm_pose[f"{finger}_{k + 1}.{side}"] = xf
    magazine = sample_xform(channels["magazine"], t, Matrix.Identity(4),
                            weapon.inverted() @ arm_pose["hand.L"])
    handle = xform(origin=(0.0, 0.0, sample_float(channels["handle"], t)))
    rifle_pose = {"gun": Matrix.Identity(4), "magazine": magazine, "charging_handle": handle}
    return arm_pose, rifle_pose


def add_animation(armature, name, length, poses):
    """An action of armature named name, keyed every frame from poses
    (frame -> {bone: Godot space transform}), in an NLA track (one
    animation each in the glTF)."""
    action = bpy.data.actions.new(name)
    slot = action.slots.new(id_type="OBJECT", name=armature.name)
    data = armature.animation_data or armature.animation_data_create()
    data.action = action
    data.action_slot = slot
    channels = anim_utils.action_ensure_channelbag_for_slot(action, slot)
    frames = len(poses)
    for bone in armature.pose.bones:
        rest_xf = bone.bone.matrix_local
        parent_rest = bone.bone.parent.matrix_local if bone.parent else Matrix.Identity(4)
        values = {"location": [], "rotation_quaternion": []}
        previous = None
        for frame in range(frames):
            g = TO_BLENDER @ poses[frame][bone.name]
            parent = TO_BLENDER @ poses[frame][bone.parent.name] if bone.parent else Matrix.Identity(4)
            basis = (parent_rest.inverted() @ rest_xf).inverted() @ (parent.inverted() @ g)
            loc, rot, _ = basis.decompose()
            if previous is not None and rot.dot(previous) < 0.0:
                rot.negate()
            previous = rot
            values["location"].append(loc)
            values["rotation_quaternion"].append(rot)
        for prop, keys in values.items():
            for index in range(len(keys[0])):
                fcurve = channels.fcurves.new(f'pose.bones["{bone.name}"].{prop}', index=index,
                                              group_name=bone.name)
                fcurve.keyframe_points.add(frames)
                co = []
                for frame in range(frames):
                    co += [frame, keys[frame][index]]
                fcurve.keyframe_points.foreach_set("co", co)
                fcurve.keyframe_points.foreach_set("interpolation", [1] * frames)  # LINEAR
                fcurve.update()
    data.action = None
    track = data.nla_tracks.new()
    track.name = name
    strip = track.strips.new(name, 0, action)
    strip.action_slot = slot
    track.mute = True


def animate(arms, rifle_armature, arms_armature, prefix, name, channels, length):
    frames = int(round(length * FPS))
    arm_poses, rifle_poses = [], []
    for frame in range(frames + 1):
        arm_pose, rifle_pose = pose(arms, channels, length * frame / frames)
        arm_poses.append(arm_pose)
        rifle_poses.append(rifle_pose)
    add_animation(arms_armature, f"{prefix}_{name}", length, arm_poses)
    add_animation(rifle_armature, name, length, rifle_poses)


def idle_keys(a):
    # Breathing.
    breath = xform(euler(-0.6, 0.3, 0.0), (0.0, -0.003, 0.002))
    return {
        "gun": [key(0.0, Matrix.Identity(4)), key(1.5, breath), key(3.0, Matrix.Identity(4))],
        "handle": [key(0.0, 0.0)],
        "magazine": [key(0.0, Matrix.Identity(4))],
        "hand.R": [key(0.0, a.grip_r)],
        "hand.L": [key(0.0, a.foregrip_l),
                   key(1.5, translated_local(a.foregrip_l, (0.0, 0.0, -0.002))),
                   key(3.0, a.foregrip_l)],
        "fingers.R": [key(0.0, a.grip)],
        "fingers.L": [key(0.0, a.foregrip), key(1.5, a.foregrip_loose), key(3.0, a.foregrip)],
    }


def fire_keys(a):
    return {
        "gun": [key(0.0, Matrix.Identity(4))],
        "handle": [key(0.0, 0.0)],
        "magazine": [key(0.0, Matrix.Identity(4))],
        "hand.R": [key(0.0, a.grip_r)],
        "hand.L": [key(0.0, a.foregrip_l)],
        "fingers.R": [key(0.0, a.grip), key(0.025, a.trigger, OUT), key(0.1, a.grip)],
        "fingers.L": [key(0.0, a.foregrip)],
    }


def reload_keys(a):
    identity = Matrix.Identity(4)
    # Rolled and raised into view to bring the magazine to the left hand,
    # then less for the charging handle.
    tilt = xform(euler(16, 10, -30), (-0.07, 0.07, 0.03))
    seated = tilt @ xform(euler(2, 0, -2), (0.0, 0.008, 0.0))
    tilt_handle = xform(euler(4, 6, -12), (-0.02, 0.0, 0.03))
    racked = tilt_handle @ xform(euler(-1.5, 0, 0), (0.0, 0.0, 0.008))
    # Out along the magazine's axis.
    out_axis = Vector((0.0, -1.0, -0.15)).normalized()
    pulled = translated(a.mag_grab_l, out_axis * 0.14)
    lined_up = translated(a.mag_grab_l, out_axis * 0.05)
    # Out of view below, fetching the next magazine (camera space).
    pouch = xform(euler(-40, 20, 60), (-0.22, -0.52, -0.18))
    handle_back = translated(a.handle_grab_l, (0.0, 0.0, 0.065))
    let_go = translated(handle_back, (-0.01, -0.01, 0.01))
    return {
        "gun": [
            key(0.0, identity), key(0.22, tilt), key(0.30, tilt),
            key(0.36, tilt @ xform(origin=(0.0, 0.006, 0.0))),
            key(0.98, tilt), key(1.04, seated, OUT), key(1.10, tilt),
            key(1.20, tilt_handle), key(1.29, tilt_handle), key(1.33, racked, OUT),
            key(1.5, identity),
        ],
        "handle": [
            key(0.0, 0.0), key(1.19, 0.0), key(1.28, 0.065), key(1.30, 0.065),
            key(1.33, 0.0, IN), key(1.5, 0.0),
        ],
        "magazine": [
            key(0.0, identity), key(0.30, identity),
            key(0.31, a.mag_in_hand, SMOOTH, "hand.L"),
            key(1.04, a.mag_in_hand, SMOOTH, "hand.L"), key(1.05, identity),
            key(1.5, identity),
        ],
        "hand.R": [key(0.0, a.grip_r)],
        "hand.L": [
            key(0.0, a.foregrip_l), key(0.22, a.mag_grab_l), key(0.30, a.mag_grab_l),
            key(0.44, pulled), key(0.66, pouch, SMOOTH, "camera"),
            key(0.76, pouch, SMOOTH, "camera"), key(0.94, lined_up),
            key(1.04, a.mag_grab_l, OUT), key(1.08, a.mag_grab_l),
            key(1.19, a.handle_grab_l), key(1.28, handle_back), key(1.32, let_go),
            key(1.5, a.foregrip_l),
        ],
        "fingers.R": [
            key(0.0, a.grip), key(0.15, a.finger_out), key(1.32, a.finger_out),
            key(1.48, a.grip),
        ],
        "fingers.L": [
            key(0.0, a.foregrip), key(0.12, OPEN), key(0.22, OPEN), key(0.28, a.mag_hold),
            key(1.06, a.mag_hold), key(1.14, a.pinch_open), key(1.19, a.pinch),
            key(1.29, a.pinch), key(1.32, OPEN, OUT), key(1.46, a.foregrip),
        ],
    }


def draw_keys(a):
    lowered = xform(euler(-55, 20, 35), (0.06, -0.30, 0.10))
    overshoot = xform(euler(2, -1, -1.5), (0.0, 0.004, 0.0))
    # Camera space.
    reaching = xform(euler(-30, 30, 50), (-0.18, -0.42, -0.30))
    return {
        "gun": [key(0.0, lowered), key(0.42, overshoot, OUT), key(0.6, Matrix.Identity(4))],
        "handle": [key(0.0, 0.0)],
        "magazine": [key(0.0, Matrix.Identity(4))],
        "hand.R": [key(0.0, a.grip_r)],
        "hand.L": [
            key(0.0, reaching, SMOOTH, "camera"), key(0.15, reaching, SMOOTH, "camera"),
            key(0.42, a.foregrip_l, OUT), key(0.6, a.foregrip_l),
        ],
        "fingers.R": [key(0.0, a.grip)],
        "fingers.L": [key(0.0, OPEN), key(0.30, OPEN), key(0.45, a.foregrip)],
    }


def export(objects, path):
    select_only(objects)
    bpy.ops.export_scene.gltf(
        filepath=path, export_format="GLB", use_selection=True,
        export_animation_mode="NLA_TRACKS", export_force_sampling=True,
        export_optimize_animation_size=False, export_yup=True, export_apply=False,
        export_def_bones=False, export_materials="EXPORT", export_image_format="AUTO")
    print("Wrote", path)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out_dir = argv[0] if argv else "models"
    reset()
    rifle_armature, gun, rifle_ao = build_rifle()
    arms = Arms(bone_trees(gun, RIFLE_BONES))
    arms_armature, arms_mesh, arms_ao = build_arms(arms)
    bake_ao(gun, rifle_ao, [arms_mesh])
    bake_ao(arms_mesh, arms_ao, [gun])
    arms.make_poses()
    for name, keys, length in (("idle", idle_keys, 3.0), ("fire", fire_keys, 0.1),
                               ("reload", reload_keys, 1.5), ("draw", draw_keys, 0.6)):
        animate(arms, rifle_armature, arms_armature, "rifle", name, keys(arms), length)
    if arms.unreachable > 0.0:
        print("WARNING: the hands fall %.3f m short of a target" % arms.unreachable)
    export([rifle_armature, gun], f"{out_dir}/rifle.glb")
    export([arms_armature, arms_mesh], f"{out_dir}/arms.glb")


main()
