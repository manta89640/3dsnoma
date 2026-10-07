"""Offline full-water reflection texture helpers for Wuhu Island."""
import numpy as np


def _rgba8_gx(image):
    """Encode an RGBA image as GX RGBA8 tiles (4x4, AR plane then GB plane)."""
    image = np.asarray(image, np.uint8)
    h, w, _ = image.shape
    if w % 4 or h % 4:
        raise ValueError("GX RGBA8 dimensions must be multiples of four")
    tiles = image.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    out = np.empty((len(tiles), 64), np.uint8)
    out[:, 0:32:2], out[:, 1:32:2] = tiles[:, :, 3], tiles[:, :, 0]
    out[:, 32::2], out[:, 33::2] = tiles[:, :, 1], tiles[:, :, 2]
    return out.tobytes()


class AtlasTexture:
    """Small BRRES-like texture source accepted by Converter.texture."""

    def __init__(self, name, rgba):
        self.name = name
        self.rgba = np.ascontiguousarray(rgba, dtype=np.uint8)
        self.height, self.width = self.rgba.shape[:2]
        self.fmt = 6  # GX RGBA8
        self.levels = 1
        self.min_lod = 0
        self.max_lod = 0
        self.textures = {name: self}

    def level_data(self, level):
        if level:
            raise IndexError(level)
        return _rgba8_gx(self.rgba)

    def level_size(self, level):
        if level:
            raise IndexError(level)
        return self.width, self.height

    def texture_rgba(self, name, lod=0):
        if name != self.name or lod != 0:
            raise KeyError((name, lod))
        return self.rgba


def reflection_atlas(env, wave_a, wave_b, frames=16, tiles=4):
    """Warp WSR's 64x64 sea environment map by its wave maps into an animated atlas.

    The inputs are decoded RGBA images. Adjacent wave frames shift the source maps in different
    directions, approximating the original indirect-texture offsets; local slopes perturb the
    environment lookup and add a small crest highlight. The output is a square texture atlas.
    """
    env = np.asarray(env, np.float32)[..., :3] / 255.0
    a = np.asarray(wave_a, np.float32)[..., :3] / 255.0
    b = np.asarray(wave_b, np.float32)[..., :3] / 255.0
    h, w = env.shape[:2]
    if h != w or frames > tiles * tiles or frames < 2:
        raise ValueError("reflection atlas expects a square environment and 2..tiles^2 frames")
    # Intensity maps are decoded by GX as intensity replicated to RGB; use a stable luminance
    # conversion so the same routine also handles IA and RGB source variants.
    ya = a[..., 0] * 0.30 + a[..., 1] * 0.59 + a[..., 2] * 0.11
    yb = b[..., 0] * 0.30 + b[..., 1] * 0.59 + b[..., 2] * 0.11
    atlas = np.empty((tiles * h, tiles * w, 4), np.uint8)
    yy, xx = np.mgrid[0:h, 0:w]

    def sample(image, x, y):
        ih, iw = image.shape[:2]
        x = np.clip(np.mod(x, iw), 0.0, np.nextafter(float(iw), 0.0))
        y = np.clip(np.mod(y, ih), 0.0, np.nextafter(float(ih), 0.0))
        x0, y0 = np.floor(x).astype(np.int32), np.floor(y).astype(np.int32)
        fx, fy = x - x0, y - y0
        if image.ndim == 3:
            fx, fy = fx[..., None], fy[..., None]
        p00 = image[y0, x0]
        p10 = image[y0, (x0 + 1) % iw]
        p01 = image[(y0 + 1) % ih, x0]
        p11 = image[(y0 + 1) % ih, (x0 + 1) % iw]
        return (p00 * (1 - fx) * (1 - fy) + p10 * fx * (1 - fy) +
                p01 * (1 - fx) * fy + p11 * fx * fy)

    for f in range(frames):
        phase = 2.0 * np.pi * f / frames
        ax, ay = 2.5 * np.cos(phase), 2.5 * np.sin(phase)
        bx, by = 3.0 * np.cos(phase + 2.1), 3.0 * np.sin(phase + 2.1)
        height = 0.55 * sample(ya, xx + ax, yy + ay) + 0.45 * sample(yb, xx + bx, yy + by)
        dx = np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)
        dy = np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)
        # Keep the reflection broad and calm: WSR's 64x64 environment map magnifies small
        # offsets on the 3DS, and a strong warp makes the overview sea visually overwhelm
        # the island. These slopes shift it by less than one texel, with a faint shimmer.
        rx = xx + dx * (w * 0.8) + 0.2 * np.sin(phase + yy * (2 * np.pi / h))
        ry = yy + dy * (h * 0.8) + 0.2 * np.cos(phase + xx * (2 * np.pi / w))
        reflected = sample(env, rx, ry)
        # Preserve some of the source's even day-water tint under the animated reflection.
        rgb = sample(env, xx, yy) * 0.35 + reflected * 0.65
        # Low-contrast glints follow the two moving wave fields without reading as whitecaps.
        slope = np.clip((dx * 0.7 - dy * 0.5) * 1.5, -0.04, 0.05)
        rgb = np.clip(rgb * (1.0 + slope[..., None]), 0.0, 1.0)
        tile_y, tile_x = divmod(f, tiles)
        y0, x0 = tile_y * h, tile_x * w
        atlas[y0:y0 + h, x0:x0 + w, :3] = np.rint(rgb * 255.0).astype(np.uint8)
        atlas[y0:y0 + h, x0:x0 + w, 3] = 255
    # Unused atlas tiles duplicate frame zero so edge filtering never samples uninitialized bytes.
    for f in range(frames, tiles * tiles):
        tile_y, tile_x = divmod(f, tiles)
        y0, x0 = tile_y * h, tile_x * w
        atlas[y0:y0 + h, x0:x0 + w] = atlas[:h, :w]
    return atlas
