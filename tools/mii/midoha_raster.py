"""A small deferred numpy rasteriser for ミドハ's garment tools: triangles into a buffer of triangle
ids and barycentric weights (depth tested), from a camera (previews, the concept's projection) or
in texture space (painting the atlas). Shading happens afterwards, per pixel, from the buffer."""
import math

import numpy as np


class Camera:
    """eye, target, up in world units; ortho: the view's half-height in world units (else fovy
    degrees, perspective); W x H pixels"""

    def __init__(self, W, H, eye, target, up=(0, 1, 0), ortho=None, fovy=30.0):
        self.W, self.H = W, H
        e, t, u = np.asarray(eye, float), np.asarray(target, float), np.asarray(up, float)
        f = t - e
        f /= np.linalg.norm(f)
        r = np.cross(f, u)
        r /= np.linalg.norm(r)
        u2 = np.cross(r, f)
        self.R = np.stack([r, u2, -f])  # world -> view: x right, y up, looking down -z
        self.e = e
        self.ortho = ortho
        self.fovy = fovy

    def view(self, P):
        return (np.asarray(P, float) - self.e) @ self.R.T

    def project(self, P):
        """(n,3) world -> (n,3) pixel x, pixel y (down), depth (larger further)"""
        V = self.view(P)
        if self.ortho:
            s = self.H / 2 / self.ortho
            return np.stack([self.W / 2 + V[:, 0] * s, self.H / 2 - V[:, 1] * s, -V[:, 2]], 1)
        fy = self.H / 2 / math.tan(math.radians(self.fovy) / 2)
        w = np.maximum(-V[:, 2], 1e-3)
        return np.stack([self.W / 2 + V[:, 0] * fy / w, self.H / 2 - V[:, 1] * fy / w, w], 1)

    def dir_to_eye(self, P):
        if self.ortho:
            return np.broadcast_to(self.R[2], np.shape(P)).copy()
        d = self.e - np.asarray(P, float)
        return d / np.linalg.norm(d, axis=-1, keepdims=True)


class Buffer:
    """per pixel: triangle id (-1 none), barycentrics (b0, b1; b2 = 1 - b0 - b1), depth"""

    def __init__(self, W, H):
        self.W, self.H = W, H
        self.tri = np.full((H, W), -1, np.int32)
        self.b = np.zeros((H, W, 2), np.float32)
        self.z = np.full((H, W), np.inf, np.float32)

    def draw(self, S, T, ids=None, cull=None, zbias=None, persp=False):
        """S (n,3) screen points (x, y, depth); T (k,3) indices; ids (k,) the ids written (default
        0..k-1); cull: +1 drop triangles wound clockwise on screen, -1 counter-clockwise, None both;
        zbias (k,) subtracted from the depth; persp: depth is w (perspective-correct barycentrics)"""
        S = np.asarray(S, float)
        T = np.asarray(T)
        ids = np.arange(len(T)) if ids is None else ids
        A, B, C = S[T[:, 0]], S[T[:, 1]], S[T[:, 2]]
        area = (B[:, 0] - A[:, 0]) * (C[:, 1] - A[:, 1]) - (B[:, 1] - A[:, 1]) * (C[:, 0] - A[:, 0])
        x0 = np.maximum(np.floor(np.minimum(np.minimum(A[:, 0], B[:, 0]), C[:, 0])).astype(int), 0)
        x1 = np.minimum(np.ceil(np.maximum(np.maximum(A[:, 0], B[:, 0]), C[:, 0])).astype(int), self.W - 1)
        y0 = np.maximum(np.floor(np.minimum(np.minimum(A[:, 1], B[:, 1]), C[:, 1])).astype(int), 0)
        y1 = np.minimum(np.ceil(np.maximum(np.maximum(A[:, 1], B[:, 1]), C[:, 1])).astype(int), self.H - 1)
        for k in range(len(T)):
            ar = area[k]
            if abs(ar) < 1e-12 or x0[k] > x1[k] or y0[k] > y1[k]:
                continue
            if cull is not None and ar * cull > 0:
                continue
            a, b, c = A[k], B[k], C[k]
            xs = np.arange(x0[k], x1[k] + 1) + 0.5
            ys = np.arange(y0[k], y1[k] + 1) + 0.5
            X, Y = np.meshgrid(xs, ys)
            w0 = ((b[0] - X) * (c[1] - Y) - (b[1] - Y) * (c[0] - X)) / ar
            w1 = ((c[0] - X) * (a[1] - Y) - (c[1] - Y) * (a[0] - X)) / ar
            w2 = 1 - w0 - w1
            m = (w0 >= -1e-7) & (w1 >= -1e-7) & (w2 >= -1e-7)
            if not m.any():
                continue
            if persp:
                # 1/w interpolates linearly on screen: the attributes' weights are w_i / w over sum
                iw = w0 / a[2] + w1 / b[2] + w2 / c[2]
                z = 1 / iw
                q0, q1 = w0 / a[2] * z, w1 / b[2] * z
            else:
                z = w0 * a[2] + w1 * b[2] + w2 * c[2]
                q0, q1 = w0, w1
            if zbias is not None:
                z = z - zbias[k]
            zb = self.z[y0[k]:y1[k] + 1, x0[k]:x1[k] + 1]
            m &= z < zb
            if not m.any():
                continue
            zb[m] = z[m]
            self.tri[y0[k]:y1[k] + 1, x0[k]:x1[k] + 1][m] = ids[k]
            bb = self.b[y0[k]:y1[k] + 1, x0[k]:x1[k] + 1]
            bb[..., 0][m] = q0[m]
            bb[..., 1][m] = q1[m]

    def interp(self, corner_values, T=None):
        """corner_values (k,3,d) per triangle corner, or (n,d) per vertex with T (k,3): the
        attribute at each covered pixel -> (H,W,d), zeros where none"""
        cv = np.asarray(corner_values, float)
        if T is not None:
            cv = cv[np.asarray(T)]
        m = self.tri >= 0
        out = np.zeros((self.H, self.W, cv.shape[-1]))
        t = self.tri[m]
        b0, b1 = self.b[..., 0][m], self.b[..., 1][m]
        b2 = 1 - b0 - b1
        out[m] = cv[t, 0] * b0[:, None] + cv[t, 1] * b1[:, None] + cv[t, 2] * b2[:, None]
        return out


def bilinear(img, x, y):
    """img (h,w,c) float; x, y pixel coordinates (centres at +0.5) -> (..., c), clamped"""
    h, w = img.shape[:2]
    x = np.clip(np.asarray(x, float) - 0.5, 0, w - 1.001)
    y = np.clip(np.asarray(y, float) - 0.5, 0, h - 1.001)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    a, b = img[y0, x0], img[y0, x0 + 1]
    c, d = img[y0 + 1, x0], img[y0 + 1, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy
