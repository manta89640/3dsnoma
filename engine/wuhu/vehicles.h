// The island's cars and boats the player can take (island.cpp): the package's parked cars (class
// 15) and boats at anchor (cruisers 7, yachts 8), static World instances (tools/wuhu/stage_wuhu.py
// stages them as such). Near one, the vehicle button gets in (a boat from the water too) and out
// again; in it, the move stick's up and down speed up and back, its left and right steer.
//
// A vehicle is a box: its model's box under its placed matrix (the matrix's column lengths its
// scale, its yaw the facing). Others than the one driven are obstacles to the player's body
// (player.h Obstacle) and to the vehicle driven, by their boxes (separating axes on x/z, and their
// heights). A car keeps to ground it can drive on: under each of four points (its front, back and
// sides) ground no steeper than 40 degrees and within a step (CAR_STEP of its height) of where that
// point was, no wall between
// where its corners were and are at half its height; it sits on the ground under them, tilted to
// them. A boat keeps to deep water (no ground under its corners within its draft, none over the
// surface), stops at walls and shores, and rides the sea's waves (the World's sea, engine/g3d/sea.h:
// the swell under its middle lifts it, under its bow and stern pitches it, under its sides rolls it;
// its bow a little up with speed), as do the boats at anchor near the camera (float_moored). Its
// place and heading without the swell are its own (rest): where it is for the player and the
// collision. The boat driven has WSR's wake under it (wake()). A car's wheels (its
// model's wheel_* nodes, tools/wuhu/stage_wuhu.py CAR_WHEELS) roll by the distance it goes and the
// front pair turns with the stick.
//
// Mutated on the game's thread only while no World job is out (island.cpp take_job).
#pragma once

#include <vector>

#include "engine/collision/kcl.h"
#include "engine/g3d/package.h"
#include "engine/g3d/world.h"
#include "engine/wuhu/player.h"

namespace engine::wuhu {

using engine::math::Mtx34;

// two boxes overlap: in height, and on x/z along each box's two axes
bool boxes_overlap(const Obstacle& a, const Obstacle& b);

class Vehicles {
public:
    static constexpr u32 NONE = 0xFFFFFFFFu;

    Vehicles(engine::g3d::World& world, const engine::g3d::Package& package, const engine::collision::World& collision);

    u32 count() const { return (u32)list_.size(); }
    // the boxes of every vehicle but `except`, for the player's body
    std::vector<Obstacle> obstacles(u32 except = NONE) const;
    // the vehicle whose box is nearest `feet`, within `reach` of it (a swimmer takes only boats); NONE
    u32 nearest(Vec3 feet, f32 reach, bool swimming, f32* distance = nullptr) const;

    // into vehicle v (NONE: none); out again (it stays where it is)
    void enter(u32 v);
    void leave() { driven_ = NONE, speed_ = 0, wake_ = 0, surge_ = 0; }
    u32 driven() const { return driven_; }
    bool boat() const { return driven_ != NONE && list_[driven_].boat; }
    // the vehicle driven: throttle and steer -1..1 (the stick), dt seconds, `time` the World's clock
    void drive(f32 throttle, f32 steer, f32 dt, f32 time);

    Vec3 position() const;  // the driven one's origin (where it is, without the swell)
    // where the driven one's driver is (its feet): a car's driving seat, a cruiser's flybridge helm, a
    // yacht's cockpit (vehicles.cpp SEATS: places on the models, by their geometry) under its matrix
    Vec3 seat() const;
    f32 yaw() const { return yaw_; }
    f32 speed() const { return speed_; }              // island units a second, along its facing (- back)
    f32 speed_fraction() const;                       // |speed| over its top speed
    Obstacle box(u32 v) const;                        // vehicle v's box where it is
    f32 track_width(u32 v) const;                     // between its wheels' middles (cars), island units
    // places beside the driven one to get out at: its sides, behind, before it (at a car's bottom, a
    // boat's waterline)
    std::vector<Vec3> exits(f32 clearance) const;
    // The boats at anchor within MOORED_RANGE of `eye` on the waves at `time` (seconds: the World's
    // clock), the others at rest where they are. Each frame, from the game's thread, between jobs.
    void float_moored(Vec3 eye, f32 time);
    // The driven boat's wake (WSR's WS2_common_cruiser_ef, made for the cruiser, under the boat's
    // hull): its matrix (where the boat is and heads, on the swell's heave, not tilted, scaled from the
    // cruiser's hull to the boat's) and how strong it is (its speed, of its top: eased); false when
    // no boat is driven.
    bool wake(Mtx34* m, f32* strength) const;

private:
    struct Vehicle {
        u32 instance;
        bool boat;
        f32 bbox[6];         // the model's box (model space)
        Vec3 scale;          // its matrix's column lengths
        int wheels[4];       // a car's wheel nodes (front left, front right, rear left, rear right; -1: none)
        Mtx34 rest;          // where it is without the swell (a car: as it stands)
        bool swaying = false;  // a boat at anchor set on the waves (float_moored)
    };
    Mtx34 on_swell(const Vehicle& v, const Mtx34& rest, f32 time, f32 pitch_up, f32* heave) const;
    void turn_wheels(const Vehicle& v, f32 rolled, f32 steer, f32 dt);
    bool car_move(const Vehicle& v, Vec3 next, Mtx34* out);
    bool boat_move(const Vehicle& v, Vec3 next, Mtx34* out);
    bool hits_other(const Obstacle& was, const Obstacle& now) const;
    Mtx34 matrix(const Vehicle& v, Vec3 x_axis, Vec3 y_axis, Vec3 z_axis, Vec3 origin) const;
    Obstacle box_at(const Vehicle& v, const Mtx34& m) const;

    engine::g3d::World& world_;
    const engine::g3d::Package& package_;
    const engine::collision::World& collision_;
    std::vector<Vehicle> list_;
    u32 driven_ = NONE;
    f32 speed_ = 0, yaw_ = 0;
    f32 surge_ = 0;  // a boat's seconds ahead toward its fastest (vehicles.cpp BOAT_SURGE_SECONDS)
    f32 slope_f_ = 0, slope_s_ = 0;  // a car's tilt (rise a unit forward, a unit to its x side)
    f32 roll_ = 0, steer_ = 0;       // its wheels' turn about their axle and the front pair's steer (degrees)
    f32 wake_ = 0;                   // the driven boat's wake's strength (eased toward its speed fraction)
    f32 heave_ = 0;                  // the swell's lift under the driven boat
};

}  // namespace engine::wuhu
