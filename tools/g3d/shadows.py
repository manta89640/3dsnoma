"""Offline soft sunlight shadows for the island's static cell geometry.

The terrain, the buildings and the cliffs shade each other here, once, into the prelit vertex colours
(no cost a frame). The placed objects (trees, props: BATCH_CASTER) aren't casters: a tree's shadow is
narrower than the map's 30-unit texels and the terrain's vertices are 70-140 units apart, so it fell
between them; they cast at run time instead (WSR's silhouette decals, tools/g3d/shadow_decals.py, and the
shadow map near the player). They still receive the terrain's and the buildings' shadows here."""
import math

import numpy as np

import ng3d


def _cell_batches(pkg):
    """The full-detail batches: one set of geometry per cell, before its cheaper LODs. (The placed
    objects' among them, BATCH_CASTER, receive but don't cast: see _casts.)"""
    for cell in pkg.cells:
        start, count = cell.first[0], cell.count[0]
        for batch_index in pkg.cell_batches[start:start + count]:
            batch = pkg.batches[batch_index]
            if batch.flags & ng3d.BATCH_DETAIL:
                continue
            material = pkg.materials[batch.material]
            if material.flags & ng3d.MAT_XLU or material.blend & 1:
                continue
            if (material.zmode & 0x11) != 0x11:  # (depth tested and written)
                continue
            yield batch_index, batch, material


def _casts(batch):
    """Whether a batch casts here: not a placed object's (trees, props: they cast at run time, the
    shadow map or their decals; here a tree's shadow fell between the terrain's vertices)."""
    return not batch.flags & ng3d.BATCH_CASTER


def _triangles(pkg, batch):
    vb = pkg.vertex_buffers[batch.vbuf]
    positions = ng3d.vertex_positions(vb, pkg.vertex_data[batch.vbuf])
    indices = np.asarray(pkg.indices[batch.first_index:batch.first_index + batch.n_indices], np.int64)
    return positions[indices].reshape(-1, 3, 3)


def _raster_depth(depth, sx, sy, sz, resolution):
    """Raster a batch of orthographic triangles into a nearest-depth buffer."""
    for i in range(len(sx)):
        x0, x1 = int(max(0, math.floor(float(sx[i].min())))), int(min(resolution, math.ceil(float(sx[i].max()))))
        y0, y1 = int(max(0, math.floor(float(sy[i].min())))), int(min(resolution, math.ceil(float(sy[i].max()))))
        if x1 <= x0 or y1 <= y0:
            continue
        ax, ay = sx[i], sy[i]
        area = (ax[1] - ax[0]) * (ay[2] - ay[0]) - (ax[2] - ax[0]) * (ay[1] - ay[0])
        if abs(float(area)) < 1e-9:
            continue
        px = np.arange(x0, x1, dtype=np.float32) + 0.5
        py = np.arange(y0, y1, dtype=np.float32) + 0.5
        X, Y = np.meshgrid(px, py)
        w0 = (ax[1] - X) * (ay[2] - Y) - (ax[2] - X) * (ay[1] - Y)
        w1 = (ax[2] - X) * (ay[0] - Y) - (ax[0] - X) * (ay[2] - Y)
        w2 = (ax[0] - X) * (ay[1] - Y) - (ax[1] - X) * (ay[0] - Y)
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0) if area > 0 else (w0 <= 0) & (w1 <= 0) & (w2 <= 0)
        if not inside.any():
            continue
        b0, b1, b2 = w0[inside] / area, w1[inside] / area, w2[inside] / area
        z = b0 * sz[i, 0] + b1 * sz[i, 1] + b2 * sz[i, 2]
        pix = (Y[inside] - 0.5).astype(np.int64) * resolution + (X[inside] - 0.5).astype(np.int64)
        np.minimum.at(depth.reshape(-1), pix, z)


def _soft_visibility(depth, x, y, z, bias, radius):
    """Percentage of nearby shadow texels where the point is visible."""
    n = len(x)
    lit = np.zeros(n, np.float32)
    total = 0
    h, w = depth.shape
    ix, iy = np.rint(x).astype(np.int64), np.rint(y).astype(np.int64)
    inside = (ix >= 0) & (ix < w) & (iy >= 0) & (iy < h)
    for dy in range(-radius, radius + 1):
        yy = iy + dy
        for dx in range(-radius, radius + 1):
            xx = ix + dx
            ok = inside & (xx >= 0) & (xx < w) & (yy >= 0) & (yy < h)
            sample = np.full(n, np.inf, np.float32)
            sample[ok] = depth[yy[ok], xx[ok]]
            lit += (z <= sample + bias)
            total += 1
    return lit / total


def bake_soft_sun_shadow(pkg, resolution=1536, radius=2, darkness=0.40, bias=8.0):
    """Bake a soft, directional shadow from Map_Light into static prelit vertex colours.

    Uses the full-detail, opaque world triangles and a light-space depth map. The per-frame renderer
    does no extra work. Returns statistics suitable for the staging log.
    """
    if not pkg.lights or not pkg.vertex_buffers:
        return dict(triangles=0, vertices=0, shadowed=0)
    light = next((l for l in pkg.lights if pkg.string(l.name) == "Map_Light"), pkg.lights[0])
    toward_sun = np.asarray(light.position, np.float64)
    toward_sun /= max(float(np.linalg.norm(toward_sun)), 1e-9)
    right = np.cross(np.array([0.0, 1.0, 0.0]), toward_sun)
    right /= max(float(np.linalg.norm(right)), 1e-9)
    up = np.cross(toward_sun, right)

    batches = list(_cell_batches(pkg))
    all_points = []
    triangles = []
    prelit_vbuf = {}
    for _, batch, material in batches:
        T = _triangles(pkg, batch).astype(np.float64, copy=False)
        if not len(T):
            continue
        if _casts(batch):
            triangles.append(T)
        all_points.append(T.reshape(-1, 3))
        vb = pkg.vertex_buffers[batch.vbuf]
        if material.flags & ng3d.MAT_PRELIT and vb.attrs & ng3d.VA_CLR:
            prelit_vbuf.setdefault(batch.vbuf, set()).update(
                np.asarray(pkg.indices[batch.first_index:batch.first_index + batch.n_indices], np.uint16).tolist())
    if not triangles or not prelit_vbuf:
        return dict(triangles=0, vertices=0, shadowed=0)

    points = np.concatenate(all_points)
    extent = np.maximum(points.max(0) - points.min(0), 1.0)
    world_center = (points.min(0) + points.max(0)) * 0.5
    # The light-space map covers the whole island with a small border. Its texel size sets the
    # softness: a five-texel PCF kernel produces broad, natural edges at this resolution.
    u = (points - world_center) @ right
    v = (points - world_center) @ up
    d = -(points - world_center) @ toward_sun
    half_u = max(float(np.max(np.abs(u))), 1.0) * 1.02
    half_v = max(float(np.max(np.abs(v))), 1.0) * 1.02
    depth_offset = max(float(np.max(d)), float(np.max(-d))) + 100.0
    depth = np.full((resolution, resolution), np.inf, np.float32)
    tri_count = 0
    for T in triangles:
        tri_count += len(T)
        Q = T.reshape(-1, 3) - world_center
        tu = ((Q @ right + half_u) * (resolution / (2.0 * half_u))).reshape(-1, 3)
        tv = ((half_v - Q @ up) * (resolution / (2.0 * half_v))).reshape(-1, 3)
        td = (depth_offset - Q @ toward_sun).reshape(-1, 3)
        _raster_depth(depth, tu, tv, td, resolution)

    # Only prelit static vertices change. If a vertex is shared by more than one batch, retain the
    # strongest occlusion so its colour stays consistent across material boundaries.
    n_shadowed = 0
    n_vertices = 0
    for vbi, used in prelit_vbuf.items():
        vb = pkg.vertex_buffers[vbi]
        attrs, stride = vb.attrs, vb.stride
        raw_bytes = bytearray(pkg.vertex_data[vbi])
        raw = np.frombuffer(raw_bytes, np.uint8).reshape(vb.count, stride)
        positions = ng3d.vertex_positions(vb, raw_bytes)
        ids = np.fromiter(used, dtype=np.int64, count=len(used))
        P = positions[ids].astype(np.float64, copy=False)
        Q = P - world_center
        px = (Q @ right + half_u) * (resolution / (2.0 * half_u))
        py = (half_v - Q @ up) * (resolution / (2.0 * half_v))
        pz = depth_offset - Q @ toward_sun
        visible = _soft_visibility(depth, px, py, pz, bias, radius)
        occ = 1.0 - visible
        factor = 1.0 - darkness * occ
        color_at = (8 if attrs & ng3d.VA_POS16 else 12) + (4 if attrs & ng3d.VA_NRM else 0)
        rgb = raw[ids, color_at:color_at + 3].astype(np.float32)
        raw[ids, color_at:color_at + 3] = np.clip(np.rint(rgb * factor[:, None]), 0, 255).astype(np.uint8)
        pkg.vertex_data[vbi] = bytes(raw_bytes)
        n_shadowed += int(np.count_nonzero(occ > 0.5))
        n_vertices += len(ids)

    return dict(triangles=tri_count, vertices=n_vertices, shadowed=n_shadowed,
                resolution=resolution, pcf=radius * 2 + 1)
