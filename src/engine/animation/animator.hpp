#pragma once
// The animator: layers of nodes over one skeleton, parameters in, a pose out,
// corrected by procedural post-processing.
//
//   layers       each a Node (often a StateMachine) with a bone mask, either
//                overriding what the layers below wrote (an upper-body action
//                over the legs' locomotion) or adding to it (breathing, a
//                flinch, a lean); weights fade layers in and out
//   post         foot placement on the ground (two-bone IK per leg, pelvis
//                lowered to the lower foot), the head turned to look
//   level        how much of this a character is worth on the screen: every
//                frame, every other, every fourth, or frozen; IK only near
//                (Animator::Level). The owner picks it from projected size and
//                visibility - a character off the screen is not evaluated at
//                all, only advanced.
//
// buildHumanoid() wires the standard graph for a UE4-named humanoid from
// procedural motions: locomotion (idle / walk / jog / sprint in a speed blend
// space), take-off, fall, land, with the transitions a third-person
// character needs. Keyframed clips drop into the same slots.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/animation/ik.hpp"
#include "engine/animation/motion.hpp"
#include "engine/animation/procedural.hpp"

namespace engine::animation {

class Animator {
public:
    struct Layer {
        std::string name;
        std::shared_ptr<Node> node;
        BoneMask mask;
        float weight = 1;
        bool additive = false;
    };
    enum class Level { Full, Half, Quarter, Frozen };

    explicit Animator(Skeleton skeleton);
    [[nodiscard]] const Skeleton& skeleton() const { return skeleton_; }
    Parameters& parameters() { return parameters_; }
    int addLayer(Layer layer);
    Layer& layer(int i) { return layers_[std::size_t(i)]; }

    FootPlacement feet;
    LookAt look;
    float feetWeight = 1, lookWeight = 0;
    Vec3 lookDirection{1, 0, 0};

    void level(Level l) { if (l != level_) feet.reset(); level_ = l; }
    [[nodiscard]] Level level() const { return level_; }
    // Advances the graph by `seconds` (always: time does not stop off the
    // screen). Returns whether the pose is due to be evaluated at this level.
    bool update(float seconds);
    // The pose, all layers and post-processing applied.
    const Pose& evaluate();
    [[nodiscard]] const Pose& pose() const { return pose_; }
    // The state machine of the base layer, if it is one (for its state name).
    [[nodiscard]] const StateMachine* baseMachine() const;

private:
    Skeleton skeleton_;
    Parameters parameters_;
    std::vector<Layer> layers_;
    Pose pose_, scratch_, rest_;
    Level level_ = Level::Full;
    int frame_ = 0;
    float seconds_ = 0, filteredLookWeight_ = 0;
    Vec3 filteredLook_{1, 0, 0};
};

// The standard graph for a humanoid. Parameters it reads: "speed" (m/s over
// the ground), "turn" (rad/s), "grounded" (0/1), "vz" (m/s, vertical),
// "jump" (1 on the frame a jump starts).
std::unique_ptr<Animator> buildHumanoid(Skeleton skeleton);

} // namespace engine::animation
