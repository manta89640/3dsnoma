"""ミドハ's hanbok's texture, painted (tools/mii/midoha_outfit_paint.py's `paint`): the concept's own
pixels where the concept sees the garment, the rest painted in its manner. See that tool's notes.

The concept's kinds of pixel: each of its pixels is given a kind (the jeogori's green fabric, the
collar's dark green, the white collar, skin, the belt, the buckle, the skirt, line work, the
background) from its colour and, for the collar's dark green (which the jeogori's creases share),
from the collar band's outline traced on it. Each garment piece reads only its own kinds: a texel
whose concept pixel is of another kind takes the nearest pixel of its own (a jump-flood fill), so a
piece's edges are the model's and its insides the concept's.
"""
import math

import numpy as np
from PIL import Image

import midoha_outfit_model as model
import midoha_raster as R
from midoha_outfit_paint import PAL, hexrgb

# ------------------------------------------------------------------------------- the concept's kinds
BG, FABRIC, GIT, WHITE, SKIN, BELT, BUCKLE, SKIRT, LINE, OTHER = range(10)

# the colours the concept's pixels are told apart by (sampled)
KIND_COLOURS = [
    (FABRIC, "#2A5F4B"), (FABRIC, "#3F7660"), (FABRIC, "#2A4E3F"), (FABRIC, "#1B4634"), (FABRIC, "#31634F"),
    (GIT, "#1B4537"), (GIT, "#194533"), (GIT, "#24503F"),
    (WHITE, "#F0E8E0"), (WHITE, "#D3C4BC"), (WHITE, "#B8AAA2"),
    (SKIN, "#FBC090"), (SKIN, "#D19163"), (SKIN, "#E8A878"),
    (BELT, "#271B27"), (BELT, "#33243A"), (BELT, "#19111B"),
    (LINE, "#0E0A10"), (LINE, "#0C2219"),
    (BG, "#000000"),
]
# the collar band's outline on the concept (pixels): the over side's inner edge then its outer
# edge back up; the under side's
OVER_BAND = [(546, 590), (537, 600), (506, 640), (477, 680), (448, 720), (421, 760), (393, 800), (366, 840),
             (346, 872), (340, 884), (424, 884), (437, 860), (451, 840), (466, 820), (481, 800), (494, 780),
             (508, 760), (522, 740), (535, 720), (546, 700), (560, 680), (572, 660), (582, 640), (592, 620),
             (588, 596)]
UNDER_BAND = [(430, 596), (416, 620), (426, 640), (437, 660), (449, 680), (470, 700), (515, 700), (520, 668),
              (499, 640), (484, 620), (470, 600), (462, 590)]
BUCKLE_BOX = (464, 888, 564, 946)        # the buckle's plate on the concept
BELT_ROWS = (881, 949)                   # the belt's rows
CROP_BOTTOM_Y = model.c2m(0, 1023)[1]    # the concept's lowest row, in Mii units (59.1)

# which kinds each piece reads
PIECE_KINDS = {
    "jeogori": (FABRIC, SKIN, LINE), "sleeve": (FABRIC, LINE), "git": (GIT, LINE), "dongjeong": (WHITE,),
    "neck": (SKIN,), "belt": (BELT, LINE), "buckle": (BUCKLE, BELT, LINE), "skirt": (SKIRT, BELT, LINE),
}


def poly_mask(pts, W, H):
    from PIL import ImageDraw
    im = Image.new("L", (W, H), 0)
    ImageDraw.Draw(im).polygon([tuple(p) for p in pts], fill=255)
    return np.asarray(im) > 0


def bilateral(img, radius=2, sigma_c=10.0):
    """an edge-keeping smoothing: the concept's grain out, its edges and lines kept"""
    acc = np.zeros_like(img)
    wsum = np.zeros(img.shape[:2])
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            sh = np.roll(np.roll(img, dy, 0), dx, 1)
            d = ((sh - img) ** 2).sum(-1)
            w = np.exp(-d / (2 * sigma_c ** 2)) * math.exp(-(dx * dx + dy * dy) / (2 * 1.5 ** 2))
            acc += sh * w[..., None]
            wsum += w
    return acc / wsum[..., None]


def concept_kinds(img):
    H, W, _ = img.shape
    cols = np.array([hexrgb(c) for _, c in KIND_COLOURS])
    kinds = np.array([k for k, _ in KIND_COLOURS])
    d = ((img[:, :, None, :] - cols[None, None]) ** 2).sum(-1)
    kind = kinds[d.argmin(-1)]
    # the collar's dark green only inside the band's outline (dilated a little); the creases' is fabric
    band = poly_mask(OVER_BAND, W, H) | poly_mask(UNDER_BAND, W, H)
    band = dilate_mask(band, 3)
    kind[(kind == GIT) & ~band] = FABRIC
    kind[(kind == FABRIC) & band & (((img - hexrgb("#1B4537")) ** 2).sum(-1) < 30 ** 2)] = GIT
    # the head and the hair above the chin: not the garment
    ys = np.arange(H)[:, None] * np.ones((1, W))
    kind[(ys < 583) & (kind != BG)] = OTHER
    # the belt's rows: the belt's colours are the belt (the buckle's box the buckle), below them the skirt
    x0, y0, x1, y1 = BUCKLE_BOX
    xs = np.ones((H, 1)) * np.arange(W)[None, :]
    in_box = (xs >= x0) & (xs <= x1) & (ys >= y0) & (ys <= y1)
    kind[(kind == BELT) & in_box] = BUCKLE
    kind[(kind == BELT) & (ys > BELT_ROWS[1])] = SKIRT
    kind[(kind == BELT) & (ys < BELT_ROWS[0])] = OTHER
    return kind


def dilate_mask(m, n):
    m = m.copy()
    for _ in range(n):
        m = m | np.roll(m, 1, 0) | np.roll(m, -1, 0) | np.roll(m, 1, 1) | np.roll(m, -1, 1)
    return m


def nearest_fill(img, ok):
    """every pixel the colour of the nearest pixel where ok (a jump flood)"""
    H, W = ok.shape
    INF = 1 << 30
    sy = np.where(ok, np.arange(H)[:, None] * np.ones((1, W), int), -1)
    sx = np.where(ok, np.ones((H, 1), int) * np.arange(W)[None, :], -1)
    Y, X = np.mgrid[0:H, 0:W]
    step = 1 << int(math.ceil(math.log2(max(H, W))))
    while step >= 1:
        best_d = np.where(sy >= 0, (sy - Y) ** 2 + (sx - X) ** 2, INF)
        by, bx = sy.copy(), sx.copy()
        for dy in (-step, 0, step):
            for dx in (-step, 0, step):
                if dy == 0 and dx == 0:
                    continue
                ny = np.clip(Y + dy, 0, H - 1)
                nx = np.clip(X + dx, 0, W - 1)
                cy, cx = sy[ny, nx], sx[ny, nx]
                d = np.where(cy >= 0, (cy - Y) ** 2 + (cx - X) ** 2, INF)
                better = d < best_d
                best_d = np.where(better, d, best_d)
                by = np.where(better, cy, by)
                bx = np.where(better, cx, bx)
        sy, sx = by, bx
        step //= 2
    dist = np.sqrt(np.where(sy >= 0, (sy - Y) ** 2 + (sx - X) ** 2, INF).astype(float))
    sy = np.clip(sy, 0, H - 1)
    sx = np.clip(sx, 0, W - 1)
    return img[sy, sx], dist


class Concept:
    """the concept, cleaned, its kinds, and a filled image for each piece's kinds"""

    def __init__(self, path):
        raw = np.asarray(Image.open(path).convert("RGB")).astype(float)
        self.kind = concept_kinds(raw)
        self.img = bilateral(raw)
        self.H, self.W = self.kind.shape
        self.filled = {}

    def for_kinds(self, kinds):
        key = tuple(sorted(kinds))
        if key not in self.filled:
            ok = np.isin(self.kind, key)
            self.filled[key] = nearest_fill(self.img, ok)
        return self.filled[key]       # (image, each pixel's distance to its own kind)


# -------------------------------------------------------------------------------------- helpers
def ramp(d, w):
    """0 below -w, 1 above w, linear between: an anti-aliased edge at d = 0"""
    return np.clip(0.5 + d / (2 * w), 0.0, 1.0)


def mix(a, b, t):
    t = np.asarray(t, float)[..., None]
    return a * (1 - t) + b * t


def front_camera():
    cx, cy = model.c2m(512, 512)
    return R.Camera(1024, 1024, (cx, cy, 400), (cx, cy, 0), ortho=512 / model.CONCEPT_SCALE)


# ------------------------------------------------------------------------------- the projection
def front_view(pieces, cam):
    """the garment's front view (the modelling pose): its depth buffer and each pixel's piece"""
    buf = R.Buffer(cam.W, cam.H)
    allS, allT, owner = [], [], []
    off = 0
    for i, pc in enumerate(pieces):
        allS.append(cam.project(pc.P))
        allT.append(pc.T + off)
        owner.append(np.full(len(pc.T), i))
        off += len(pc.P)
    buf.draw(np.concatenate(allS), np.concatenate(allT))
    tri_piece = np.concatenate(owner)
    piece = np.where(buf.tri >= 0, tri_piece[np.maximum(buf.tri, 0)], -1)
    return buf, piece


def sleeve_warp(pieces, cam, concept):
    """per row, the sleeves' outer edges: the model's front view's and the concept's (pixels)"""
    buf, piece = front_view(pieces, cam)
    names = [pc.name for pc in pieces]
    sl = [i for i, n in enumerate(names) if n.startswith("sleeve")]
    cover = buf.tri >= 0
    fg = (concept.kind != BG)
    rows = {}
    for y in range(cam.H):
        xs = np.nonzero(cover[y])[0]
        cs = np.nonzero(fg[y, 150:880])[0] + 150
        if len(xs) and len(cs) and np.isin(piece[y], sl).any():
            rows[y] = (xs.min(), xs.max(), cs.min(), cs.max())
    return buf, piece, rows


def project(bake, pieces, concept):
    """the concept's colour for each texel it sees, and how much it is to count (0..1)"""
    cam = front_camera()
    buf, fpiece, rows = sleeve_warp(pieces, cam, concept)
    S = cam.project(bake.pos.reshape(-1, 3)).reshape(bake.pos.shape)
    sx, sy, sz = S[..., 0], S[..., 1], S[..., 2]
    iy = np.clip(sy.astype(int), 0, cam.H - 1)
    ix = np.clip(sx.astype(int), 0, cam.W - 1)
    # seen: the front view shows its piece there (or one next to it) at about its depth
    near_piece = np.zeros(sx.shape, bool)
    near_depth = np.zeros(sx.shape, bool)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            jy = np.clip(iy + dy, 0, cam.H - 1)
            jx = np.clip(ix + dx, 0, cam.W - 1)
            near_piece |= fpiece[jy, jx] == bake.piece
            near_depth |= np.abs(sz - buf.z[jy, jx]) < 0.6
    visible = bake.mask & near_piece & near_depth & (sx >= 0) & (sx < cam.W) & (sy >= 0) & (sy < cam.H)
    nz = bake.nrm[..., 2]
    w = visible * model.smoothstep(0.18, 0.5, nz)
    # the ribbons' and the plates' walls are painted, not projected
    acr = bake.attr("across", ["git", "dongjeong"])
    w = w * ~(((acr < -0.01) | (acr > 1.01)) & bake.where("git", "dongjeong"))
    # the concept's picture ends at its bottom row
    w = w * model.smoothstep(CROP_BOTTOM_Y + 0.4, CROP_BOTTOM_Y + 1.4, bake.pos[..., 1])
    # the sleeves' outer edges: each row's model edge onto the concept's (linear from the torso's edge)
    wx = sx.copy()
    torso_px = model.CONCEPT_SCALE * 15.0
    for y, (m0, m1, c0, c1) in rows.items():
        sel = (iy == y) & bake.mask
        if not sel.any():
            continue
        x = sx[sel]
        mid = 507.0
        left = x < mid - torso_px
        right = x > mid + torso_px
        xl = mid - torso_px
        xr = mid + torso_px
        out = x.copy()
        if m0 < xl and c0 < xl:
            out[left] = xl + (x[left] - xl) * (c0 - xl) / (m0 - xl)
        if m1 > xr and c1 > xr:
            out[right] = xr + (x[right] - xr) * (c1 - xr) / (m1 - xr)
        wx[sel] = out
    col = np.zeros(bake.pos.shape)
    for i, pc in enumerate(pieces):
        key = next((k for k in sorted(PIECE_KINDS, key=len, reverse=True) if pc.name.startswith(k)), None)
        sel = (bake.piece == i) & (w > 0)
        if key is None or not sel.any():
            w[bake.piece == i] = 0
            continue
        img, dist = concept.for_kinds(PIECE_KINDS[key])
        col[sel] = R.bilinear(img, wx[sel], sy[sel])
        # where the concept has none of the piece's kind near, it is painted instead
        dd = R.bilinear(dist[..., None], wx[sel], sy[sel])[:, 0]
        w[sel] *= model.smoothstep(5.0, 1.5, dd)
    return col, w


# -------------------------------------------------------------------------------- the painting
def paint_pieces(bake, pieces):
    """every texel painted in the concept's manner"""
    S = bake.size
    out = np.zeros((S, S, 3))
    aa = 0.65 / (bake.density * bake.ss)     # a texel's width in Mii units: the edges' ramp
    P, N, st = bake.pos, bake.nrm, bake.st
    names = [pc.name for pc in pieces]
    L_up = np.array([0.0, 1.0, 0.0])

    def sel(prefix):
        ids = [i for i, n in enumerate(names) if n.startswith(prefix)]
        return np.isin(bake.piece, ids)

    # ---- the jeogori
    m = sel("jeogori")
    if m.any():
        p, n = P[m], N[m]
        phi = bake.attr("phi", ["jeogori"])[m]
        y = p[:, 1]
        c = np.tile(PAL["green"], (len(p), 1))
        # light on the shoulders
        c = mix(c, hexrgb("#2F6752"), ramp(n[:, 1] - 0.62, 0.04))
        # the sides under the arms
        A = np.array([model.interp_rows(model.SHELL_ROWS, yy)[0] for yy in y])
        side = np.abs(p[:, 0]) / A
        c = mix(c, hexrgb("#224F3D"), ramp(side - 0.84, 0.03) * model.smoothstep(99, 96, y))
        # the back seam
        back_d = (math.pi - np.abs(phi)) * A * 0.7
        c = mix(c, hexrgb("#1F4837"), ramp(0.11 - back_d, aa) * (y < 104.5) * (y > 74.8))
        # the hem over the belt: the shadow band and the light edge on the bulge above it
        c = mix(c, hexrgb("#1B4634"), ramp(76.1 - y, 0.12))
        c = mix(c, hexrgb("#33664F"), ramp(y - 76.1, aa) * ramp(76.38 - y, aa))
        # the V: skin between the white collars, the chin's shadow over its top
        vl = _v_left(p[:, 1])
        vr = _v_right(p[:, 1])
        inv = (p[:, 2] > 2.0) & (y > 99.4)
        vv = ramp(p[:, 0] - vl, aa) * ramp(vr - p[:, 0], aa) * inv
        skin = mix(np.tile(PAL["skin"], (len(p), 1)), PAL["skin_sh"], ramp(y - 105.0, 0.1))
        c = mix(c, skin, vv)
        out[m] = c
    # ---- the sleeves
    for side_name in ("sleeve_L", "sleeve_R"):
        m = sel(side_name)
        if not m.any():
            continue
        p, n = P[m], N[m]
        lat = bake.attr_vec("lat", [side_name])[m]
        lat = lat / np.maximum(np.linalg.norm(lat, axis=1, keepdims=True), 1e-9)
        t = bake.attr("t", [side_name])[m]
        pc = pieces[names.index(side_name)]
        Lsl = pc.attrs["L"]
        nl = (n * lat).sum(1)
        c = np.tile(PAL["green"], (len(p), 1))
        # the inner side, toward the body: the shadow, deepest where it meets the torso
        c = mix(c, PAL["green_dk"], ramp(-0.42 - nl, 0.05))
        # the crease where it meets the torso, below the armpit
        c = mix(c, hexrgb("#1D4737"), ramp(-0.6 - nl, 0.04) * model.smoothstep(0.12, 0.22, t))
        # light on the shoulder's top
        c = mix(c, hexrgb("#2F6752"), ramp(n[:, 1] - 0.62, 0.04))
        # the rim light on the outer edge
        c = mix(c, PAL["green_lt"], ramp(nl - 0.962, 0.008))
        # the cuff's band, a dark line at its top, a light one at its opening
        along = t * Lsl
        band = along - (Lsl - model.CUFF_BAND)
        c = mix(c, PAL["git"], ramp(band, aa))
        c = mix(c, PAL["git_line"], ramp(band + 0.09, aa) * ramp(0.09 - band, aa))
        c = mix(c, hexrgb("#2B5B47"), ramp(along - (Lsl - 0.3), aa))
        out[m] = c
    # ---- the linings and the cuffs' rims
    m = sel("lining")
    if m.any():
        t = bake.attr("t", ["lining_L", "lining_R"])[m]
        out[m] = mix(np.tile(PAL["lining"], (m.sum(), 1)), hexrgb("#0E221B"), model.smoothstep(0.95, 0.65, t))
    m = sel("cuffrim")
    if m.any():
        out[m] = hexrgb("#1F4A3A")
    # ---- the collar
    m = sel("git")
    if m.any():
        acr = bake.attr("across", ["git"])[m]
        sv = bake.attr("side", ["git"])[m]
        y = P[m][:, 1]
        width = model.COLLAR_WIDTH
        d_in = acr * width              # from the inner edge, in units
        d_out = (1 - acr) * width
        c = np.tile(PAL["git"], (m.sum(), 1))
        c = mix(c, PAL["git_line"], ramp(0.2 - d_in, aa) + ramp(0.2 - d_out, aa))
        # below the white strip's end: the line where its edge was
        frac = model.WHITE_WIDTH / model.COLLAR_WIDTH
        under_white = (sv > 0.5) & (y < model.WHITE_END_Y + 0.4)
        c = mix(c, PAL["git_line"], under_white * ramp(0.07 - np.abs(acr - frac) * width, aa))
        # the walls
        c = mix(c, PAL["git_line"], (acr < -0.01) | (acr > 1.01))
        out[m] = c
    m = sel("dongjeong")
    if m.any():
        acr = bake.attr("across", ["dongjeong"])[m]
        sv = bake.attr("side", ["dongjeong"])[m]
        y = P[m][:, 1]
        width = model.WHITE_WIDTH
        c = np.tile(PAL["white"], (m.sum(), 1))
        # in the chin's and the head's shadow: near the neck and round the back
        shade = np.maximum(ramp(y - 105.7, 0.12), ramp(0.45 - np.abs(sv), 0.05))
        c = mix(c, PAL["white_sh"], shade)
        # its inner edge's grey line, and its end's
        c = mix(c, hexrgb("#A99A92"), ramp(0.12 - acr * width, aa))
        c = mix(c, hexrgb("#B8AAA2"), (acr < -0.01) | (acr > 1.01))
        out[m] = c
    # ---- the neck
    m = sel("neck")
    if m.any():
        p = P[m]
        c = np.tile(PAL["skin"], (m.sum(), 1))
        c = mix(c, PAL["skin_sh"], np.maximum(ramp(p[:, 1] - 105.1, 0.1), ramp(-p[:, 2] + 1.0, 0.3)))
        out[m] = c
    # ---- the belt and the buckle
    m = sel("belt")
    if m.any():
        along = bake.attr("along", ["belt"])[m]
        h = model.BELT_Y1 - model.BELT_Y0
        c = np.tile(PAL["belt"], (m.sum(), 1))
        c = mix(c, PAL["belt_line"], ramp(0.24 - along, aa) + ramp(along - (h - 0.24), aa))
        out[m] = c
    m = sel("buckle")
    if m.any():
        b = model.BUCKLE
        p_st = st[m]
        dx = b["w"] / 2 - np.abs(p_st[:, 0])
        dy = b["h"] / 2 - np.abs(p_st[:, 1] - b["y"])
        d = np.minimum(dx, dy)
        c = np.tile(hexrgb("#2D1F2D"), (m.sum(), 1))
        c = mix(c, PAL["belt_line"], ramp(0.3 - d, aa))
        frame = np.isin(bake.piece[m], [names.index("buckle_frame")])
        c[frame] = hexrgb("#150E16")
        out[m] = c
    # ---- the skirt
    m = sel("skirt") & ~sel("skirt_lining") & ~sel("skirt_rim")
    if m.any():
        p = P[m]
        y = p[:, 1]
        f = np.mod(bake.attr("f", ["skirt"])[m], 1.0)
        off, ph, k = model.pleat_profile(f)
        # the pleat's width in units at this height (the round's length over the pleats)
        C = np.array([_skirt_round(yy) for yy in y])
        pw = C * (0.5 - 2 * model.FRONT_PANEL) / (model.SKIRT_PLEATS // 2)
        g = np.where(f <= 0.5, f, 1.0 - f)
        in_pleats = (g > model.FRONT_PANEL) & (g < 0.5 - model.FRONT_PANEL)
        d_fold = ph * pw                  # from the fold, across the face
        c = mix(np.tile(hexrgb("#221722"), (m.sum(), 1)), hexrgb("#2B1E2C"), model.smoothstep(0.0, 1.0, ph) * in_pleats)
        c[~in_pleats] = PAL["skirt"]
        # the folds' dark lines (also at the panels' edges: the first fold of each side)
        dist_panel = np.abs(g - model.FRONT_PANEL) * C
        dist_back = np.abs(g - (0.5 - model.FRONT_PANEL)) * C
        line = np.maximum(in_pleats * ramp(0.38 - d_fold, aa), np.maximum(ramp(0.38 - dist_panel, aa), ramp(0.38 - dist_back, aa)))
        c = mix(c, PAL["pleat"], line)
        # under the belt, and the hem
        c = mix(c, hexrgb("#1A121B"), ramp(y - 66.4, 0.25))
        c = mix(c, hexrgb("#1A121B"), ramp(model.SKIRT_ROWS[0][0] + 0.35 - y, aa))
        out[m] = c
    m = sel("skirt_lining")
    if m.any():
        out[m] = PAL["skirt_lining"]
    m = sel("skirt_rim")
    if m.any():
        out[m] = hexrgb("#1D141E")
    return out


def _line_x(p0, p1, y):
    (x0, y0), (x1, y1) = model.c2m(*p0), model.c2m(*p1)
    return x0 + (np.asarray(y) - y0) * (x1 - x0) / (y1 - y0)


def _v_left(y):
    return _line_x((470, 600), (499, 640), y)


def _v_right(y):
    return _line_x((537, 600), (506, 640), y)


_ROUND = {}


def _skirt_round(y):
    key = round(float(y), 1)
    if key not in _ROUND:
        fs = np.linspace(0, 1, 181)
        pts = model.skirt_point(key, fs, pleats=False)
        _ROUND[key] = float(np.linalg.norm(np.diff(pts, axis=0), axis=1).sum())
    return _ROUND[key]


def paint(bake, bd, pieces, concept_path):
    procedural = paint_pieces(bake, pieces)
    if not concept_path:
        return procedural
    concept = Concept(concept_path)
    col, w = project(bake, pieces, concept)
    return mix(procedural, col, w)
