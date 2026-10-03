#pragma once
// Procedural corrections applied to a finished pose (Animator post-process):
// two-bone IK, feet planted on the ground, the head turned to look.
#include <functional>

#include "engine/animation/pose.hpp"

namespace engine::animation {

// Bends root -> mid -> end so that `end` reaches `target` (model space),
// keeping the knee or elbow on the side `pole` points to. Rotations only; the
// chain's lengths are its own. `weight` blends from the pose given.
void twoBoneIk(const Skeleton& s, Pose& pose, int root, int mid, int end, Vec3 target, Vec3 pole, float weight,
               std::vector<Transform>* scratch = nullptr);

// Feet on uneven ground. `ground(x, y)` is the ground's height in the
// skeleton's frame under a point of it. The pelvis drops by as much as the
// lower foot needs (never rises: a stretched leg reads as floating), each leg
// is bent to put its foot on its own ground, and the foot tips to the slope.
struct FootPlacement {
    int pelvis = -1;
    int thigh[2]{-1, -1}, calf[2]{-1, -1}, foot[2]{-1, -1};
    float footHeight = 0.08f;     // ankle above the sole
    float maxAdjust = 0.45f;      // metres either way
    float contact[2]{1, 1};
    float seconds = 1.0f / 60;
    struct Frame { double x=0, y=0, z=0, yaw=0; bool valid=false; } frame;
    std::function<float(float, float)> ground;
    void reset();
    void apply(const Skeleton& s, Pose& pose, float weight);
private:
    struct Plant { bool locked=false; double x=0, y=0; float contact=0; } planted_[2];
    Frame previous_;
    float pelvisDrop_=0;
    std::vector<Transform> model_;
};

// The head (and some of the neck and chest) turned towards a direction in
// the skeleton's frame, within limits, the turn shared down the chain.
struct LookAt {
    int head = -1, neck = -1, chest = -1;
    float yawLimit = 1.2f, pitchLimit = 0.6f;
    void apply(const Skeleton& s, Pose& pose, Vec3 direction, float weight) const;
};

} // namespace engine::animation
