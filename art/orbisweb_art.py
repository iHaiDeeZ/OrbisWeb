# OrbisWeb's icon and start picture, rendered with Blender (Cycles): the old Wisp emblem made in 3D.
#   ~/tools/blender-4.5.14-linux-x64/blender -b -P art/orbisweb_art.py -- --out art/render [--samples 512] [--which icon,pic1] [--blend art/orbisweb.blend]
# Three glowing strands (indigo, teal, light blue) braid upward out of a bright point of light and fade at their tips,
# with sparks drifting up, on the navy of the old icon, and "OrbisWeb" under them. Writes <out>/icon.png
# (1024 x 1024, made 512 x 512 for sce_sys/icon0.png) and <out>/pic1.png (3840 x 2160, made 1920 x 1080).
import bpy, math, os, random, sys
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
def arg(name, default):
    return argv[argv.index(name) + 1] if name in argv else default
OUT = os.path.abspath(arg("--out", "render"))
SAMPLES = int(arg("--samples", "512"))
WHICH = arg("--which", "icon,pic1").split(",")
BLEND = arg("--blend", "")
FONT = arg("--font", os.path.expanduser("~/wisp-build/fonts/noto/NotoSans-Bold.ttf"))
HDRI = os.path.join(os.path.dirname(bpy.app.binary_path), "%d.%d" % bpy.app.version[:2], "datafiles", "studiolights", "world", "studio.exr")
os.makedirs(OUT, exist_ok=True)
random.seed(11)

# --- an empty scene, Cycles on the GPU
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = "CYCLES"
cyc = scene.cycles
cyc.samples = SAMPLES
cyc.use_denoising = True
cyc.max_bounces = 12
cyc.transmission_bounces = 12
cyc.transparent_max_bounces = 24
cyc.caustics_reflective = False
cyc.caustics_refractive = False
try:
    prefs = bpy.context.preferences.addons["cycles"].preferences
    for kind in ("OPTIX", "CUDA"):
        try:
            prefs.compute_device_type = kind
        except TypeError:
            continue
        prefs.get_devices()
        devs = [d for d in prefs.devices if d.type == kind]
        if devs:
            for d in prefs.devices:
                d.use = d.type == kind
            cyc.device = "GPU"
            print("render device:", kind, [d.name for d in devs])
            break
except Exception as e:
    print("GPU setup failed, CPU:", e)
scene.view_settings.view_transform = "AgX"
for look in ("AgX - Punchy", "Punchy"):
    try:
        scene.view_settings.look = look
        break
    except TypeError:
        pass
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGB"
scene.render.image_settings.color_depth = "8"

def node(nodes, kind, loc=(0, 0), **inputs):
    n = nodes.new(kind)
    n.location = loc
    for k, v in inputs.items():
        n.inputs[k].default_value = v
    return n

def hexrgb(h, a=1.0):
    h = h.lstrip("#")
    srgb = [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    lin = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in srgb]
    return (*lin, a)

def emission_mat(name, color, strength):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = node(nt.nodes, "ShaderNodeOutputMaterial", (300, 0))
    em = node(nt.nodes, "ShaderNodeEmission", (0, 0), Color=color, Strength=strength)
    nt.links.new(em.outputs[0], out.inputs[0])
    return m

def soft_mat(name, color, strength, falloff=2.5):
    """A sphere seen as a soft disc: bright in the middle, fading to nothing at its rim (glows, bokeh)."""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = node(nt.nodes, "ShaderNodeOutputMaterial", (900, 0))
    lw = node(nt.nodes, "ShaderNodeLayerWeight", (0, 0), Blend=0.5)
    inv = node(nt.nodes, "ShaderNodeMath", (200, 0))
    inv.operation = "SUBTRACT"
    inv.inputs[0].default_value = 1.0
    pw = node(nt.nodes, "ShaderNodeMath", (380, 0))
    pw.operation = "POWER"
    pw.inputs[1].default_value = falloff
    em = node(nt.nodes, "ShaderNodeEmission", (380, -200), Color=color, Strength=strength)
    tr = node(nt.nodes, "ShaderNodeBsdfTransparent", (380, -350))
    mix = node(nt.nodes, "ShaderNodeMixShader", (650, 0))
    nt.links.new(lw.outputs["Facing"], inv.inputs[1])
    nt.links.new(inv.outputs[0], pw.inputs[0])
    nt.links.new(pw.outputs[0], mix.inputs["Fac"])
    nt.links.new(tr.outputs[0], mix.inputs[1])
    nt.links.new(em.outputs[0], mix.inputs[2])
    nt.links.new(mix.outputs[0], out.inputs[0])
    return m

def smooth(obj):
    for p in obj.data.polygons:
        p.use_smooth = True

def ball(name, loc, r, mat, shadow=False, seg=24):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=r, location=loc, segments=seg, ring_count=seg // 2)
    o = bpy.context.object
    o.name = name
    smooth(o)
    o.data.materials.append(mat)
    o.visible_shadow = shadow
    return o

BASE_Z = -0.66      # the point of light the strands come out of
TOP_Z = 1.62        # where they fade out
GLOW_Z = 0.35       # the centre of the backdrop's glow

# --- the world: a dim studio light for the strands' gloss (the camera sees the backdrop)
world = bpy.data.worlds.new("World")
scene.world = world
world.use_nodes = True
wn = world.node_tree.nodes
wn.clear()
wout = node(wn, "ShaderNodeOutputWorld", (600, 0))
bg = node(wn, "ShaderNodeBackground", (300, 0), Strength=0.3)
if os.path.exists(HDRI):
    env = node(wn, "ShaderNodeTexEnvironment", (0, 0))
    env.image = bpy.data.images.load(HDRI)
    world.node_tree.links.new(env.outputs[0], bg.inputs["Color"])
world.node_tree.links.new(bg.outputs[0], wout.inputs[0])

# --- the backdrop: the old icon's navy, lighter behind the emblem
bpy.ops.mesh.primitive_plane_add(size=80, location=(0, 9, 0), rotation=(math.radians(90), 0, 0))
backdrop = bpy.context.object
m = bpy.data.materials.new("Backdrop")
m.use_nodes = True
nt = m.node_tree
nt.nodes.clear()
out = node(nt.nodes, "ShaderNodeOutputMaterial", (900, 0))
em = node(nt.nodes, "ShaderNodeEmission", (650, 0), Strength=1.0)
tc = node(nt.nodes, "ShaderNodeTexCoord", (-400, 0))
mp = node(nt.nodes, "ShaderNodeMapping", (-200, 0))
mp.inputs["Location"].default_value = (0, -GLOW_Z / 10.0, 0)
mp.inputs["Scale"].default_value = (1 / 10.0, 1 / 10.0, 1 / 10.0)
gr = node(nt.nodes, "ShaderNodeTexGradient", (0, 0))
gr.gradient_type = "SPHERICAL"
cr = node(nt.nodes, "ShaderNodeValToRGB", (250, 0))
cr.color_ramp.interpolation = "EASE"
cr.color_ramp.elements[0].position = 0.0
cr.color_ramp.elements[0].color = hexrgb("#070b19")
cr.color_ramp.elements[1].position = 0.92
cr.color_ramp.elements[1].color = hexrgb("#22357a")
e = cr.color_ramp.elements.new(0.5)
e.color = hexrgb("#111c43")
nt.links.new(tc.outputs["Object"], mp.inputs["Vector"])
nt.links.new(mp.outputs[0], gr.inputs["Vector"])
nt.links.new(gr.outputs["Fac"], cr.inputs["Fac"])
nt.links.new(cr.outputs["Color"], em.inputs["Color"])
nt.links.new(em.outputs[0], out.inputs[0])
backdrop.data.materials.append(m)
backdrop.visible_shadow = False

# --- the strands: glossy glowing tubes, brightest where they leave the light, fading at their tips
def strand_mat(name, color, deep):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = node(nt.nodes, "ShaderNodeOutputMaterial", (1100, 0))
    p = node(nt.nodes, "ShaderNodeBsdfPrincipled", (800, 0))
    p.inputs["Base Color"].default_value = hexrgb(deep)
    p.inputs["Roughness"].default_value = 0.18
    p.inputs["Coat Weight"].default_value = 1.0
    p.inputs["Coat Roughness"].default_value = 0.05
    p.inputs["Emission Color"].default_value = hexrgb(color)
    tc = node(nt.nodes, "ShaderNodeTexCoord", (-200, 0))
    sep = node(nt.nodes, "ShaderNodeSeparateXYZ", (0, 0))
    glow = node(nt.nodes, "ShaderNodeFloatCurve", (250, 100))
    c = glow.mapping.curves[0]
    c.points[0].location = (0.0, 1.0)
    c.points[1].location = (1.0, 0.0)
    c.points.new(0.45, 0.82)
    c.points.new(0.8, 0.45)
    glow.mapping.update()
    gain = node(nt.nodes, "ShaderNodeMath", (500, 100))
    gain.operation = "MULTIPLY"
    gain.inputs[1].default_value = 3.0
    fade = node(nt.nodes, "ShaderNodeMapRange", (250, -150))
    fade.inputs["From Min"].default_value = 0.98
    fade.inputs["From Max"].default_value = 0.72
    fade.inputs["To Min"].default_value = 0.0
    fade.inputs["To Max"].default_value = 1.0
    nt.links.new(tc.outputs["Generated"], sep.inputs[0])
    nt.links.new(sep.outputs["Z"], glow.inputs["Value"])
    nt.links.new(glow.outputs[0], gain.inputs[0])
    nt.links.new(gain.outputs[0], p.inputs["Emission Strength"])
    nt.links.new(sep.outputs["Z"], fade.inputs["Value"])
    nt.links.new(fade.outputs[0], p.inputs["Alpha"])
    nt.links.new(p.outputs[0], out.inputs[0])
    return m

def strand(name, phase, turns, r_top, lean, color, deep, thick=0.06, n=180):
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "3D"
    cu.bevel_depth = 1.0
    cu.bevel_resolution = 8
    cu.use_fill_caps = True
    sp = cu.splines.new("NURBS")
    sp.points.add(n - 1)
    sp.order_u = 4
    sp.use_endpoint_u = True
    for i in range(n):
        t = i / (n - 1)
        r = 0.012 + r_top * (t ** 0.85)
        th = phase + 2 * math.pi * turns * t
        x = r * math.cos(th) + lean * (t ** 2.2)
        y = r * math.sin(th)
        z = BASE_Z + (TOP_Z - BASE_Z) * t
        sp.points[i].co = (x, y, z, 1)
        # thick at the light, thin and tapering at the tip
        sp.points[i].radius = thick * (1.0 - 0.45 * t) * (1.0 - 0.9 * max(0.0, t - 0.86) / 0.14)
    o = bpy.data.objects.new(name, cu)
    bpy.context.collection.objects.link(o)
    o.data.materials.append(strand_mat(name + "Mat", color, deep))
    return o

STRANDS = [
    # name, phase, turns, top radius, lean at the top, glow colour, body colour
    ("Indigo", math.radians(200), 1.05, 0.40, -0.36, "#6c74ff", "#2e33b0"),
    ("Teal", math.radians(320), 1.05, 0.42, 0.22, "#1ff7a2", "#0b8a5c"),
    ("Sky", math.radians(80), 1.05, 0.38, 0.34, "#2cb6ff", "#0f5fa8"),
]
for s in STRANDS:
    strand(*s)

# --- the light they come out of: a white-hot core, a glow behind it that falls off fast
ball("Core", (0, -0.02, BASE_Z), 0.085, emission_mat("Core", hexrgb("#ffffff"), 60.0))
ball("Glow", (0, 0.35, BASE_Z), 0.34, soft_mat("Glow", hexrgb("#c9f6ff"), 3.0, 10.0), seg=48)
ball("Haze", (0, 0.6, BASE_Z + 0.55), 1.3, soft_mat("Haze", hexrgb("#3b6dff"), 0.14, 6.0), seg=48)

# --- a few sparks rising beside the strands, a few lights far behind and out of focus
spark = emission_mat("Spark", hexrgb("#e9fdff"), 22.0)
for i in range(13):
    t = random.uniform(0.05, 0.75) ** 1.2
    th = random.uniform(0, 2 * math.pi)
    r = 0.1 + 0.5 * t + random.uniform(0.0, 0.1)
    loc = (r * math.cos(th) * 1.15, r * math.sin(th) - 0.15, BASE_Z + 0.1 + (TOP_Z - BASE_Z) * t)
    ball("Spark%d" % i, loc, random.uniform(0.005, 0.011), spark, seg=12)
for i in range(12):
    x = random.uniform(-8.0, 8.0)
    z = random.uniform(-4.5, 5.0)
    if abs(x) < 2.6 and -2.8 < z < 2.6:
        x += 3.0 if x >= 0 else -3.0  # the middle stays clear
    col = random.choice(["#39d0ff", "#4a7dff", "#7d6bff", "#2ee6c8"])
    ball("Bokeh%d" % i, (x, random.uniform(4.0, 8.0), z), random.uniform(0.06, 0.16),
         soft_mat("Bokeh%d" % i, hexrgb(col), random.uniform(0.25, 0.7), 4.0))

# --- the name, white as the old icon's, a soft bevel
font = bpy.data.fonts.load(FONT) if os.path.exists(FONT) else None
bpy.ops.object.text_add(location=(0, 0, -1.53), rotation=(math.radians(90), 0, 0))
text = bpy.context.object
text.data.body = "OrbisWeb"
if font:
    text.data.font = font
text.data.align_x = "CENTER"
text.data.align_y = "CENTER"
text.data.size = 0.78
text.data.space_character = 1.0
text.data.extrude = 0.04
text.data.bevel_depth = 0.007
text.data.bevel_resolution = 4
m = bpy.data.materials.new("Name")
m.use_nodes = True
p = m.node_tree.nodes.get("Principled BSDF")
p.inputs["Base Color"].default_value = hexrgb("#eef4ff")
p.inputs["Roughness"].default_value = 0.25
p.inputs["Coat Weight"].default_value = 0.5
p.inputs["Emission Color"].default_value = hexrgb("#eef4ff")
p.inputs["Emission Strength"].default_value = 0.75
text.data.materials.append(m)

# --- lights: a soft key from the upper left for the strands' gloss, violet and cyan rims behind
def area(name, loc, target, power, color, size):
    bpy.ops.object.light_add(type="AREA", location=loc)
    l = bpy.context.object
    l.name = name
    l.data.shape = "DISK"
    l.data.energy = power
    l.data.color = hexrgb(color)[:3]
    l.data.size = size
    l.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
    return l
area("Key", (-4.0, -6.5, 4.5), (0, 0, 0.3), 700, "#e7f0ff", 6.0)
area("RimViolet", (4.5, 4.0, 3.0), (0, 0, 0.3), 900, "#8d6bff", 3.5)
area("RimCyan", (-4.5, 3.5, -0.5), (0, 0, 0.0), 800, "#35d4ff", 3.5)
area("Fill", (3.0, -6.0, -2.5), (0, 0, -1.5), 220, "#9fb8ff", 6.0)

# --- the camera: 85 mm, the strands in focus, the far lights soft
bpy.ops.object.camera_add(location=(0, -10.5, 0.0), rotation=(math.radians(90), 0, 0))
cam = bpy.context.object
scene.camera = cam
cam.data.lens = 85
cam.data.dof.use_dof = True
cam.data.dof.focus_distance = 10.5
cam.data.dof.aperture_fstop = 2.0

# --- the compositor: bloom round the bright parts, a hint of lens dispersion
scene.use_nodes = True
ct = scene.node_tree
ct.nodes.clear()
rl = node(ct.nodes, "CompositorNodeRLayers", (0, 0))
gl = ct.nodes.new("CompositorNodeGlare")
gl.location = (250, 0)
for kind in ("BLOOM", "FOG_GLOW"):
    try:
        gl.glare_type = kind
        break
    except TypeError:
        pass
gl.quality = "HIGH"
for name, value in (("Threshold", 1.0), ("Smoothness", 0.45), ("Strength", 0.7), ("Saturation", 1.0), ("Size", 0.75)):
    if name in gl.inputs:
        gl.inputs[name].default_value = value
ld = ct.nodes.new("CompositorNodeLensdist")
ld.location = (500, 0)
ld.inputs["Dispersion"].default_value = 0.004
comp = node(ct.nodes, "CompositorNodeComposite", (750, 0))
ct.links.new(rl.outputs["Image"], gl.inputs["Image"])
ct.links.new(gl.outputs["Image"], ld.inputs["Image"])
ct.links.new(ld.outputs["Image"], comp.inputs["Image"])

if BLEND:
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(BLEND))
    print("saved", BLEND)

def render(name, w, h, cam_z, lens):
    scene.render.resolution_x = w
    scene.render.resolution_y = h
    scene.render.resolution_percentage = 100
    cam.location.z = cam_z
    cam.data.lens = lens
    scene.render.filepath = os.path.join(OUT, name + ".png")
    bpy.ops.render.render(write_still=True)
    print("rendered", scene.render.filepath)

if "icon" in WHICH:
    render("icon", 1024, 1024, 0.0, 85)
if "pic1" in WHICH:
    render("pic1", 3840, 2160, -0.05, 46)
