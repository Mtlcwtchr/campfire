#pragma once
// Procedural motions for a humanoid with the UE4 mannequin's bone names
// (pelvis, spine_01..05, neck_01, head, clavicle/upperarm/lowerarm/hand,
// thigh/calf/foot, _l and _r). What a character plays when it has no
// keyframed clips, and what a keyframed set falls back on for anything it
// lacks: these are Motions like any Clip, so a blend space or a state machine
// cannot tell them apart.
//
// Every rotation is about an axis of the character's rest frame (+x forward,
// +y left, +z up) at the joint (Pose::rotateInRestFrame), so the motions do
// not care how a rig's bones are oriented.
#include <memory>

#include "engine/animation/motion.hpp"

namespace engine::animation {

struct HumanoidRig {
    int pelvis = -1, spine[3]{-1, -1, -1}, neck = -1, head = -1;
    int clavicle[2]{-1, -1}, upperarm[2]{-1, -1}, lowerarm[2]{-1, -1}, hand[2]{-1, -1};
    int thigh[2]{-1, -1}, calf[2]{-1, -1}, foot[2]{-1, -1};
    static HumanoidRig find(const Skeleton& s);
    [[nodiscard]] bool usable() const { return pelvis >= 0 && thigh[0] >= 0 && thigh[1] >= 0 && calf[0] >= 0 && calf[1] >= 0; }
};

// Shared by distance-driven locomotion and the contact post-process.
float gaitStride(float speed);
float gaitContact(float phase, float speed, int side);

// How a rest pose's arms come down to the sides (radians off the bind pose's
// A or T, about the forward axis), shared by every motion so they agree.
struct Stance {
    float armsDown[2]{0.8f, 0.8f};   // left, right
    float elbows = 0.18f;
    // Measured from the rest pose: how far each upper arm is from hanging,
    // less a margin that keeps the hands clear of the hips.
    static Stance fit(const Skeleton& s, const HumanoidRig& rig, float margin = 0.14f);
    void apply(const Skeleton& s, const HumanoidRig& rig, Pose& pose) const;
    // The axis a forearm bends about, in its rest frame, so that with the arm
    // brought down it bends forward: the body's lateral axis, carried back
    // through the upper arm's roll (a rest-frame axis turns with the parent).
    [[nodiscard]] Vec3 elbowAxis(int side) const;
};

// Standing: breathing in the chest, the weight shifting from foot to foot
// over a few seconds, the head settling. Parameter "turn" leans it a little.
class IdleMotion : public Motion {
public:
    IdleMotion(HumanoidRig rig, Stance stance) : rig_(rig), stance_(stance) {}
    [[nodiscard]] float duration(const Parameters&) const override { return 4.2f; }
    void sample(const Skeleton& s, float phase, const Parameters& p, Pose& out) const override;
private:
    HumanoidRig rig_;
    Stance stance_;
};

// One gait cycle (left foot forward at phase 0.25), shaped by a style: how
// far the legs reach, how much the knees fold, the arms swing, the body
// bobs and leans. Its cycle is its stride over its own speed, so a blend
// space mixing walk, jog and sprint keeps the feet on the ground.
class GaitMotion : public Motion {
public:
    struct Style {
        float speed = 1.6f;        // metres a second this cycle is made for
        float stride = 1.35f;      // metres a cycle (two steps)
        float reach = 0.40f;       // thigh swing, radians either way
        float knee = 0.55f;        // knee fold through the swing
        float arms = 0.30f;        // arm swing
        float elbows = 0.25f;      // extra elbow bend
        float bob = 0.022f;        // pelvis rise and fall, metres
        float lean = 0.04f;        // forward lean of the spine
        float flight = 0.0f;       // 0 walk (a foot always down), up to 1 sprint
        float stance = 0.62f;      // fraction of the cycle supporting the body
        float stepHeight = 0.055f;
    };
    GaitMotion(HumanoidRig rig, Stance stance, Style style) : rig_(rig), stance_(stance), style_(style) {}
    [[nodiscard]] float duration(const Parameters&) const override { return style_.stride / style_.speed; }
    void sample(const Skeleton& s, float phase, const Parameters& p, Pose& out) const override;
    static Style walk();
    static Style jog();
    static Style sprint();
private:
    HumanoidRig rig_;
    Stance stance_;
    Style style_;
    mutable std::vector<Transform> model_;
};

// Not a cycle: the crouch and spring of a jump, the tuck of a fall, the
// knees taking a landing. Selected by `kind`.
class AirMotion : public Motion {
public:
    enum class Kind { TakeOff, Fall, Land };
    AirMotion(HumanoidRig rig, Stance stance, Kind kind) : rig_(rig), stance_(stance), kind_(kind) {}
    [[nodiscard]] float duration(const Parameters&) const override;
    [[nodiscard]] bool loops() const override { return kind_ == Kind::Fall; }
    void sample(const Skeleton& s, float phase, const Parameters& p, Pose& out) const override;
private:
    HumanoidRig rig_;
    Stance stance_;
    Kind kind_;
};

} // namespace engine::animation
