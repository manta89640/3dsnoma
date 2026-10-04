// Animation curves of the native g3d: a package's tracks (fmt::Track: keys with a frame, a value
// and a slope) evaluated as nw4r::g3d evaluates its key frames, and the matrices animated values
// make (a node's SRT, a texture matrix's SRT).
#pragma once

#include "engine/g3d/ng3d_format.h"
#include "engine/math/vecmath.h"

namespace engine::g3d {

using engine::math::Mtx34;
using f32 = float;
using u8 = uint8_t;

// Where an animation of `length` frames is at `time` frames: wrapped into [0, length) when it
// loops (nw4r's PLAYPOLICY_LOOP), else held at its ends [0, length].
f32 anim_frame(f32 time, f32 length, bool loop);

// A track's value at `frame` (keys in frame order, at least one). Before the first key the first
// value, after the last the last; between two keys by the track's interpolation:
//   IP_STEP     the left key's value (CLR0's per-frame colours);
//   IP_LINEAR   linear (CHR0's baked frames);
//   IP_HERMITE  nw4r's cubic: v0 + (v1 - v0)(3 - 2t)t^2 + d(t - 1)((t - 1)s0 + t s1), with d the
//               frames since the left key, t = d / (its distance to the right key), s0 and s1 the
//               keys' slopes (value per frame) -- the cubic Hermite with tangents s * length.
// Two keys on one frame make a jump: the later one holds from that frame on.
f32 eval_track(const fmt::Track& track, const fmt::Key* keys, f32 frame);

// A node's matrix from its scale, rotation (degrees, X then Y then Z: R = Rz Ry Rx, as nw4r's
// MTX34RotXYZDeg) and translation: T R S.
Mtx34 srt_matrix(const f32 scale[3], const f32 rotate_deg[3], const f32 translate[3]);

// A texture matrix (2x4: the S and T rows applied to (s, t, 1, 1); the third column is 0) from a
// texture SRT, as nw4r::g3d computes it for its matrix mode (fmt::TexMtx::mode). Maya (mode 0, all of
// Wii Sports Resort's): scale and rotation about the texture's centre with t flipped,
//   [ sx c   -sx s   0   sx (-0.5c - (0.5s - 0.5) - tx) ]
//   [ sy s    sy c   0   sy (-0.5c + (0.5s - 0.5) + ty) + 1 ]
// (the research renderer's tex_srt_matrix). XSI and 3ds Max (modes 1, 2) aren't used by the data
// this was written for: they are made like Maya and counted by `approximated` when given.
void tex_srt_matrix(u8 mode, f32 sx, f32 sy, f32 rotate_deg, f32 tx, f32 ty, f32 out[2][4], bool* approximated = nullptr);

}  // namespace engine::g3d
