"""Wii Sports Resort's object shadows for the native g3d: silhouettes laid on the ground.

WSR has no shadow map. Each tree, parasol, tent and a few of the venues' props has a 32x32 silhouette
(StageArc `*_shadow.brres`: an I8 coverage picture), drawn as alpha-blended geometry over the terrain
under the object, depth tested and not written, its colour a constant shadow colour (research
dol/wsr_shadows.md, from main.dol: the table at 0x8066F1C0, the placement in fn_802874B0, the GX state
in fn_802AC46C). The cars and the spectators' Miis use the same drawing with silhouettes of their own.

This module makes them at staging time:
  * `Silhouettes`: the silhouettes packed into one I8 atlas (a texture source Converter.texture
    takes), so that a cell's decals are one draw;
  * `decal_material`: the material WSR's GX state is (colour K.rgb x the texture, alpha K.a x its
    coverage, blended over what is there, depth tested LEQUAL and not written: an overlay, which the
    renderers pull toward the eye as they pull the island's edge strips), MAT_SHADOW_DECAL so that the
    shadow map fades it out where it covers;
  * `Ground`: the terrain the decals are cut from, the render terrain's own triangles (not the
    collision's, as WSR has them), so that a decal lies on what is drawn and the overlay depth pull
    keeps it there;
  * `place` (WSR's placement) and `cut` (its near mesh: the terrain under the footprint clipped to it,
    the texture coordinates baked into the vertices);
  * `quad_corners` and `simple_material`, for the car's shadow (a quad under its body, a part of the
    car model, so that it goes wherever a car goes, parked, driven or on its route) and the Mii's
    stand-ins (shadow_proxy, a caster only; shadow_blob, its blob where the shadow map doesn't reach).
"""
import math
from collections import OrderedDict

import numpy as np

import convert
import ng3d

# WSR's shadow types (main.dol 0x8066F1C0, 16 bytes each: size, offset, depth, straight down), by the
# PMP class each is for (0x8066F240), with their silhouettes (texture, file in StageArc.carc). size and
# offset are multiplied by the object's scale: size is the silhouette's half width, the ray to the
# ground starts (size + offset) x scale above the object's origin, and the decal reaches `depth` x scale
# under the ground there. A straight-down shadow lies under the object as its top view (turning with
# it); the others are cast along the sun.
TYPES = OrderedDict([
    (9, ("parasol", "WS2_common_parasol_shadow", "Parasol/G3D/WS2_common_parasol_shadow.brres", 21.0, -4.0, 100.0,
         False)),
    (14, ("tent", "WS2_common_tent_shadow", "Tent/G3D/WS2_common_tent_shadow.brres", 43.0, -25.0, 100.0, True)),
    (2, ("conifer", "WS2_common_tree_shadow", "Tree/G3D/WS2_common_tree_shadow.brres", 100.0, 0.0, 200.0, False)),
    (3, ("broadleaf tree", "WS2_common_tree2_shadow", "Tree/G3D/WS2_common_tree_shadow.brres", 80.0, 0.0, 200.0,
         False)),
    (4, ("palm", "WS2_common_tree3_shadow", "Tree/G3D/WS2_common_tree_shadow.brres", 28.0, 0.0, 200.0, False)),
    (35, ("archery target", "WS2_common_arcT_shadow", "IslandLowObj/G3D/WS2_common_shadow.brres", 35.0, 0.0, 200.0,
          True)),
    (38, ("table-tennis table", "WS2_common_pngT_shadow", "IslandLowObj/G3D/WS2_common_shadow.brres", 20.0, 0.0,
          200.0, True)),
    (39, ("cycling gate", "WS2_common_bicG_shadow", "IslandLowObj/G3D/WS2_common_shadow.brres", 85.0, 0.0, 200.0,
          True)),
])
CAR = ("WS2_common_car_shadow", "Car/G3D/WS2_common_car_shadow.brres")
MII = ("WS2_common_ava_shadow", "Audience/G3D/WS2_common_ava_shadow.brres")

# how far a decal's vertices are lifted off the terrain along its normal (the positions are snapped
# to the package's 1/12-unit grid, which can take a clipped corner 0.043 units under the surface; the
# overlay pull covers the rest at a distance)
LIFT = 0.15
# the darkness WSR's TEV constant gives (env+0x9C at run time, not traced: a shadow on the island's
# day ground in WSR's captures is about this dark)
DARKNESS = 0.45


# ------------------------------------------------------------------------------------- the atlas
def _i8_gx(img):
    """An I8 picture as GX's I8 tiles (8x4 texels a block, rows of blocks)."""
    h, w = img.shape
    return np.ascontiguousarray(img.reshape(h // 4, 4, w // 8, 8).transpose(0, 2, 1, 3)).tobytes()


class Silhouettes:
    """The silhouettes in one I8 atlas of 32x32 cells (4 x 4 of them in 128 x 128), with two mip
    levels below it (cells of 16 and 8: a level further would blend neighbours). A texture source
    for Converter.texture (fmt, levels, width, height, level_data, level_size, texture_rgba)."""
    CELL, COLS = 32, 4

    def __init__(self, name="shadow_silhouettes"):
        self.name = name
        self.textures = {name: self}
        self.fmt = 1          # GX I8: intensity = coverage (GX's I8 reads alpha = intensity)
        self.width = self.height = self.CELL * self.COLS
        self.levels = 3
        self.min_lod, self.max_lod = 0.0, float(self.levels - 1)
        self.img = np.zeros((self.height, self.width), np.uint8)
        self.slots = OrderedDict()

    def add(self, key, coverage):
        """A silhouette (a 32x32 coverage array, 0-255) -> its cell's UV rectangle (u0, v0, u1, v1),
        half a texel inside it."""
        if key not in self.slots:
            k = len(self.slots)
            if k >= self.COLS * self.COLS:
                raise ValueError("the shadow atlas is full")
            cy, cx = divmod(k, self.COLS)
            c = np.asarray(coverage, np.uint8)
            if c.shape != (self.CELL, self.CELL):
                raise ValueError("%s: a silhouette of %s" % (key, c.shape))
            self.img[cy * self.CELL:(cy + 1) * self.CELL, cx * self.CELL:(cx + 1) * self.CELL] = c
            t = 0.5 / self.width
            self.slots[key] = (cx / self.COLS + t, cy / self.COLS + t, (cx + 1) / self.COLS - t, (cy + 1) / self.COLS - t)
        return self.slots[key]

    def _level(self, level):
        a = self.img.astype(np.float64)
        for _ in range(level):
            a = (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2]) / 4
        return np.clip(np.rint(a), 0, 255).astype(np.uint8)

    def level_size(self, level):
        return self.width >> level, self.height >> level

    def level_data(self, level):
        return _i8_gx(self._level(level))

    def texture_rgba(self, name, lod=0):
        a = self._level(lod)
        return np.stack([a, a, a, a], -1)


def coverage_of(r, name):
    """A WSR silhouette's coverage (its decoded alpha: GX's I8 and IA4 alike)."""
    return np.asarray(r.texture_rgba(name), np.uint8)[..., 3]


# ------------------------------------------------------------------------------------- materials
def _env(d, c, b, a, alpha):
    """A TEV colour (alpha=False: args 0-15) or alpha (args 0-7) combiner word: d + lerp(a, b, c),
    clamped, into PREV."""
    if alpha:
        return d << 4 | c << 7 | b << 10 | a << 13 | 1 << 19
    return d | c << 4 | b << 8 | a << 12 | 1 << 19


def simple_material(cv, name, texture=None, konst=(0, 0, 0, 255), translucent=False, depth_write=True, cull=2,
                    flags=0, priority=0):
    """A material of one TEV stage, as GX state code sets one: colour K0.rgb (times the texture's colour
    with `texture`), alpha K0.a (times its alpha); translucent: blended over what is there (SRC_ALPHA,
    INV_SRC_ALPHA), drawn in the translucent pass; depth tested LEQUAL, written if depth_write; no
    colour channel, no lighting. -> the material's index (its vertices need positions, and UV0 for a
    texture)."""
    pkg = cv.pkg
    M = ng3d.Rec("Material")
    M.name = pkg.str(name)
    M.flags = flags | (ng3d.MAT_XLU if translucent else 0)
    M.n_stages, M.n_texgens, M.n_chans, M.n_ind = 1, 1 if texture is not None else 0, 0, 0
    M.cull = cull
    M.priority = priority
    # stage 0: colour = K * TEXC (or K), alpha = K.a * TEXA (or K.a); args: TEXC 8, KONST 14, ZERO 15,
    # TEXA 4, KONST 6, ZERO 7
    if texture is not None:
        M.color_env[0] = _env(15, 8, 14, 15, False)
        M.alpha_env[0] = _env(7, 4, 6, 7, True)
    else:
        M.color_env[0] = _env(14, 15, 15, 15, False)
        M.alpha_env[0] = _env(6, 7, 7, 7, True)
    # TREF: stage 0 samples map 0 with coordinate 0 (enabled), no rasterised colour (7)
    M.tref[0] = (0 | 0 << 3 | (1 << 6 if texture is not None else 0) | 7 << 7)
    # KSEL: the swap tables identity (RGBA), stage 0's konst K0 (colour 0x0C, alpha 0x1C)
    for t in range(4):
        M.ksel[2 * t] = 0 | 1 << 2
        M.ksel[2 * t + 1] = 2 | 3 << 2
    M.ksel[0] |= 0x0C << 4 | 0x1C << 9
    M.tev_konst[0] = [int(v) for v in konst]
    M.alpha_compare = 7 << 16 | 7 << 19          # always
    M.zmode = 1 | 3 << 1 | (0x10 if depth_write else 0)
    M.blend = (1 | 5 << 5 | 4 << 8 if translucent else 0) | convert.BLEND_GLOBAL_BITS
    for mp in M.maps:
        mp.texture = ng3d.NONE16
    if texture is not None:
        ti, levels = texture
        mp = M.maps[0]
        mp.texture = ti
        mp.wrap_s = mp.wrap_t = 0                   # clamp
        mp.min_filter, mp.mag_filter = (5 if levels > 1 else 1), 1
        mp.min_lod, mp.max_lod = 0.0, float(levels - 1)
        g = M.texgens[0]
        g.type, g.source = 1, 4                     # MTX2x4 from TEX0
    for g in M.texgens:
        g.matrix = g.post_mtx = ng3d.NONE16
    M.tex_mtx_first, M.n_tex_mtx = len(pkg.tex_mtx), 0
    pkg.materials.append(M)
    i = len(pkg.materials) - 1
    cv.mat_info[i] = convert.vertex_needs(M, [])
    cv.stats["materials"] += 1
    return i


def decal_material(cv, texture, darkness=DARKNESS, name="shadow_decal"):
    """WSR's shadow state: black at alpha `darkness` x the silhouette's coverage, an overlay drawn
    first among the translucent (under the edge strips and the water, as the ground it lies on)."""
    return simple_material(cv, name, texture, konst=(0, 0, 0, int(round(255 * darkness))), translucent=True,
                           depth_write=False, cull=0, flags=ng3d.MAT_SHADOW_DECAL, priority=-1)


# ------------------------------------------------------------------------------------- the ground
class Ground:
    """Triangles (n, 3, 3) of the render terrain, for the decals to find the ground under an object
    and to be cut from."""

    def __init__(self, tris):
        self.tris = np.asarray(tris, np.float64).reshape(-1, 3, 3)
        a, b, c = self.tris[:, 0], self.tris[:, 1], self.tris[:, 2]
        # (the package's triangles wind clockwise seen from their front, as GX's do: the normal is
        # (c - a) x (b - a))
        n = np.cross(c - a, b - a)
        self.normals = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
        self.lo = self.tris.min(1)
        self.hi = self.tris.max(1)

    def under(self, x, z, y_from):
        """The highest triangle under (x, z) at or below y_from (facing up): (y, normal) or None."""
        near = (self.lo[:, 0] <= x) & (self.hi[:, 0] >= x) & (self.lo[:, 2] <= z) & (self.hi[:, 2] >= z) & \
            (self.normals[:, 1] > 0.05)
        idx = np.nonzero(near)[0]
        if not len(idx):
            return None
        t = self.tris[idx]
        a, b, c = t[:, 0], t[:, 1], t[:, 2]

        def edge(p, q):
            return (q[:, 0] - p[:, 0]) * (z - p[:, 2]) - (q[:, 2] - p[:, 2]) * (x - p[:, 0])
        e0, e1, e2 = edge(a, b), edge(b, c), edge(c, a)
        inside = ((e0 >= 0) & (e1 >= 0) & (e2 >= 0)) | ((e0 <= 0) & (e1 <= 0) & (e2 <= 0))
        n = self.normals[idx]
        y = a[:, 1] - (n[:, 0] * (x - a[:, 0]) + n[:, 2] * (z - a[:, 2])) / n[:, 1]
        ok = inside & (y <= y_from + 1e-3)
        if not ok.any():
            return None
        k = np.nonzero(ok)[0][np.argmax(y[ok])]
        return float(y[k]), n[k]

    def near(self, lo, hi):
        """The triangles whose boxes meet the box lo..hi (x, y, z)."""
        m = np.all(self.lo <= hi, axis=1) & np.all(self.hi >= lo, axis=1)
        return self.tris[m], self.normals[m]


# ------------------------------------------------------------------------------------- placement
class Decal:
    """Where a shadow lies: the ground point C under it, its axes X and Y on the ground (unit), its half
    width hx and length hy, the direction it is projected along P (unit, the way the light goes), the
    ground's normal n, how far under and over the ground it reaches (below, above)."""
    __slots__ = ("C", "X", "Y", "hx", "hy", "P", "n", "below", "above")

    def __init__(self, **kw):
        for k, v in kw.items():
            setattr(self, k, v)


def place(ground, M, size, offset, depth, straight_down, light):
    """WSR's placement (fn_802874B0) of a shadow for an object at matrix M (4x4): the ground under its
    raised centre (its origin plus (size + offset) x scale up); a straight-down shadow lies there along
    the object's own axes, size x scale each way; a sun shadow is the raised centre pushed along the light
    onto the ground's plane, size x scale wide and as long as a sphere of that radius's shadow is (WSR
    measures the object's extent along the light, fn_802ABA88: not traced closely enough to copy).
    `light`: the direction the sun's light goes (unit). None if there's no ground under it."""
    M = np.asarray(M, np.float64)
    scale = float(np.linalg.norm(M[:3, 0]))
    origin = M[:3, 3]
    up = np.array([0.0, 1.0, 0.0])
    s = size * scale
    raised = origin + up * (size + offset) * scale
    hit = ground.under(raised[0], raised[2], raised[1])
    if hit is None:
        return None
    gy, n = hit
    n = np.asarray(n, np.float64)
    G = np.array([raised[0], gy, raised[2]])
    above = max(float(np.dot(raised - G, n)), 1.0)    # (no higher than the raised centre: not a roof over it)
    if straight_down:
        P = -up
        fwd = M[:3, 2] - n * np.dot(M[:3, 2], n)
        if np.linalg.norm(fwd) < 1e-6:
            fwd = np.array([0.0, 0.0, 1.0])
        Y = fwd / np.linalg.norm(fwd)
        X = np.cross(Y, n)
        return Decal(C=G, X=X / np.linalg.norm(X), Y=Y, hx=s, hy=s, P=P, n=n, below=depth * scale, above=above)
    L = np.asarray(light, np.float64)
    L = L / np.linalg.norm(L)
    down = -np.dot(L, n)                      # the sine of the light's height over the ground
    if down < 0.2:
        return None                           # (the sun at the ground's edge: no shadow worth laying)
    C = raised + L * (np.dot(G - raised, n) / np.dot(L, n))
    Y = L - n * np.dot(L, n)
    Y /= np.linalg.norm(Y)
    X = np.cross(Y, n)
    return Decal(C=C, X=X / np.linalg.norm(X), Y=Y, hx=s, hy=s / down, P=L, n=n, below=depth * scale, above=above)


def _clip(poly, f):
    """A convex polygon (list of points) clipped to f(p) >= 0 (f affine)."""
    out = []
    k = len(poly)
    for i in range(k):
        p, q = poly[i], poly[(i + 1) % k]
        fp, fq = f(p), f(q)
        if fp >= 0:
            out.append(p)
        if (fp >= 0) != (fq >= 0):
            t = fp / (fp - fq)
            out.append(p + (q - p) * t)
    return out


def cut(ground, d, uv_rect):
    """The decal's mesh: the ground's triangles facing the light under its footprint, clipped to it
    (the footprint's four sides as seen along P, and from `below` under the ground's plane to `above`
    over it), lifted LIFT off them, with the silhouette's texture coordinates (its cell of the atlas,
    uv_rect: u0, v0, u1, v1; the image's top at the far end along the light). -> corners (pos, uv0) or
    None."""
    C, X, Y, n, P = d.C, d.X, d.Y, d.n, d.P
    pn = float(np.dot(P, n))
    # a point's place on the ground plane along P, as (a, b) in -1..1 across the footprint: affine
    gx = (X - n * np.dot(P, X) / pn) / d.hx
    gy = (Y - n * np.dot(P, Y) / pn) / d.hy
    # the footprint's box, the points over it up to `above` and down to `below` along P
    corners = [C + X * d.hx * sa + Y * d.hy * sb for sa in (-1, 1) for sb in (-1, 1)]
    reach = [c + P * (d.above / max(-pn, 1e-3)) * -1 for c in corners] + \
        [c + P * (d.below / max(-pn, 1e-3)) for c in corners]
    pts = np.array(corners + reach)
    tris, normals = ground.near(pts.min(0) - 1, pts.max(0) + 1)
    if not len(tris):
        return None
    planes = [lambda p: 1 + np.dot(p - C, gx), lambda p: 1 - np.dot(p - C, gx),
              lambda p: 1 + np.dot(p - C, gy), lambda p: 1 - np.dot(p - C, gy),
              lambda p: d.below + np.dot(p - C, n), lambda p: d.above - np.dot(p - C, n)]
    u0, v0, u1, v1 = uv_rect
    pos, uv = [], []
    for t, tn in zip(tris, normals):
        if np.dot(tn, P) >= -0.05:            # (facing away from the light, or edge on)
            continue
        poly = [t[0], t[1], t[2]]
        for f in planes:
            poly = _clip(poly, f)
            if len(poly) < 3:
                break
        if len(poly) < 3:
            continue
        lifted = [p + tn * LIFT for p in poly]
        ab = [(float(np.dot(p - C, gx)), float(np.dot(p - C, gy))) for p in poly]
        tuv = [(u0 + (a + 1) * 0.5 * (u1 - u0), v0 + (1 - b) * 0.5 * (v1 - v0)) for a, b in ab]
        for i in range(1, len(poly) - 1):
            for j in (0, i, i + 1):
                pos.append(lifted[j])
                uv.append(tuv[j])
    if not pos:
        return None
    return dict(pos=np.asarray(pos, np.float32), uv0=np.asarray(uv, np.float32), uvfrac=[None, None])


def quad_corners(center, X, Y, uv_rect):
    """A quad (two triangles, facing +X x Y) about `center` with half axes X and Y and the cell's UVs
    (its top at +Y)."""
    c, X, Y = (np.asarray(v, np.float64) for v in (center, X, Y))
    u0, v0, u1, v1 = uv_rect
    P = [c - X - Y, c + X - Y, c + X + Y, c - X + Y]
    T = [(u0, v1), (u1, v1), (u1, v0), (u0, v0)]
    order = (0, 1, 2, 0, 2, 3) if np.dot(np.cross(X, Y), [0, 1, 0]) <= 0 else (0, 2, 1, 0, 3, 2)
    return dict(pos=np.asarray([P[i] for i in order], np.float32), uv0=np.asarray([T[i] for i in order], np.float32),
                uvfrac=[None, None])


def capsule_corners(height=1.0, radius=0.16, head=0.12, sides=8):
    """The Mii's stand-in for the shadow map: a body (a cylinder from the feet to the shoulders, a
    cone in to the neck) and a head (an octahedron-ish ball), about 60 triangles, height 1 (scaled to
    the Mii's at run time)."""
    rings = [(0.02, radius * 0.7), (0.45, radius), (0.72, radius), (0.78, radius * 0.45)]
    ring_pts = []
    for y, r in rings:
        ring_pts.append([np.array([r * math.cos(2 * math.pi * k / sides), y * height, r * math.sin(2 * math.pi * k / sides)])
                         for k in range(sides)])
    tris = []
    for a, b in zip(ring_pts, ring_pts[1:]):
        for k in range(sides):
            k2 = (k + 1) % sides
            tris += [(a[k], b[k], b[k2]), (a[k], b[k2], a[k2])]
    bottom = np.array([0.0, 0.02 * height, 0.0])
    for k in range(sides):
        tris.append((bottom, ring_pts[0][k], ring_pts[0][(k + 1) % sides]))
    hc = np.array([0.0, (1.0 - head) * height, 0.0])
    hr = head * height
    ax = [np.array(v) * hr for v in ((1, 0, 0), (0, 0, 1), (-1, 0, 0), (0, 0, -1))]
    top, bot = hc + np.array([0, hr, 0]), hc - np.array([0, hr, 0])
    for k in range(4):
        tris += [(top, hc + ax[k], hc + ax[(k + 1) % 4]), (bot, hc + ax[(k + 1) % 4], hc + ax[k])]
    pos = np.asarray([p for t in tris for p in t], np.float32)
    return dict(pos=pos, uvfrac=[None, None])
