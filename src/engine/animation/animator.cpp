#include "engine/animation/animator.hpp"

#include <algorithm>
#include <cmath>

namespace engine::animation {

Animator::Animator(Skeleton skeleton) : skeleton_(std::move(skeleton)) {
    skeleton_.finish();
    rest_ = Pose::rest(skeleton_);
    pose_ = rest_;
    scratch_ = rest_;
}

int Animator::addLayer(Layer layer) {
    if (layer.mask.weight.size() != skeleton_.size()) layer.mask = BoneMask::all(skeleton_);
    layers_.push_back(std::move(layer));
    return int(layers_.size()) - 1;
}

bool Animator::update(float seconds) {
    seconds_ = std::isfinite(seconds) ? std::clamp(seconds, 0.0f, 0.1f) : 0;
    seconds = seconds_;
    feet.seconds = seconds;
    for (auto& l : layers_)
        if (l.node) l.node->update(seconds, parameters_);
    const auto* machine = baseMachine();
    const float phase = parameters_.get("locomotion_phase", machine ? std::max(0.0f, machine->phase()) : 0);
    const bool landing = machine && machine->currentName() == "land";
    for (int side = 0; side < 2; ++side)
        feet.contact[side] = parameters_.flag("grounded") ?
            (landing ? 1 : gaitContact(phase, parameters_.get("speed"), side)) : 0;
    if (!parameters_.flag("grounded")) feet.reset();
    ++frame_;
    switch (level_) {
        case Level::Full: return true;
        case Level::Half: return frame_ % 2 == 0;
        case Level::Quarter: return frame_ % 4 == 0;
        case Level::Frozen: return false;
    }
    return true;
}

const Pose& Animator::evaluate() {
    pose_ = rest_;
    bool first = true;
    for (const auto& l : layers_) {
        if (!l.node || l.weight <= 0) continue;
        scratch_ = rest_;
        l.node->evaluate(skeleton_, parameters_, scratch_);
        if (first && !l.additive) {
            // The first override layer is laid over the rest pose.
            pose_.blend(scratch_, l.weight, &l.mask);
            first = false;
            continue;
        }
        if (l.additive) pose_.add(scratch_, rest_, l.weight, &l.mask);
        else pose_.blend(scratch_, l.weight, &l.mask);
        first = false;
    }
    // Post-processing is for what is close enough to see it.
    if (level_ == Level::Full) {
        if (feetWeight > 0 && feet.ground) feet.apply(skeleton_, pose_, feetWeight);
        const float smooth = 1 - std::exp(-seconds_ * 10);
        if (lookWeight > 0.001f)
            filteredLook_ = normalized(lerp(filteredLook_, normalized(lookDirection), smooth));
        filteredLookWeight_ += (lookWeight - filteredLookWeight_) * smooth;
        if (filteredLookWeight_ > 0.001f) look.apply(skeleton_, pose_, filteredLook_, filteredLookWeight_);
    }
    return pose_;
}

const StateMachine* Animator::baseMachine() const {
    return layers_.empty() ? nullptr : dynamic_cast<const StateMachine*>(layers_.front().node.get());
}

std::unique_ptr<Animator> buildHumanoid(Skeleton skeleton) {
    skeleton.finish();
    const HumanoidRig rig = HumanoidRig::find(skeleton);
    auto animator = std::make_unique<Animator>(std::move(skeleton));
    const Stance stance = Stance::fit(animator->skeleton(), rig);

    // Locomotion: standing to sprinting along one parameter, kept in step.
    auto locomotion = std::make_shared<BlendSpace1D>("speed");
    locomotion->phaseParameter("locomotion_phase");
    locomotion->add(0.0f, std::make_shared<IdleMotion>(rig, stance));
    locomotion->add(GaitMotion::walk().speed, std::make_shared<GaitMotion>(rig, stance, GaitMotion::walk()));
    locomotion->add(GaitMotion::jog().speed, std::make_shared<GaitMotion>(rig, stance, GaitMotion::jog()));
    locomotion->add(GaitMotion::sprint().speed, std::make_shared<GaitMotion>(rig, stance, GaitMotion::sprint()));
    // Idle breathing uses elapsed time independently of the driven stride.

    auto machine = std::make_shared<StateMachine>();
    const int ground = machine->addState("locomotion", locomotion);
    const int takeoff = machine->addState("takeoff",
            std::make_shared<MotionNode>(std::make_shared<AirMotion>(rig, stance, AirMotion::Kind::TakeOff)));
    const int fall = machine->addState("fall",
            std::make_shared<MotionNode>(std::make_shared<AirMotion>(rig, stance, AirMotion::Kind::Fall)));
    const int land = machine->addState("land",
            std::make_shared<MotionNode>(std::make_shared<AirMotion>(rig, stance, AirMotion::Kind::Land)));
    machine->addTransition(ground, takeoff, 0.08f, [](const Parameters& p, const Node&) { return p.flag("jump"); });
    machine->addTransition(ground, fall, 0.25f, [](const Parameters& p, const Node&) {
        return !p.flag("grounded") && p.get("vz") < -2.0f;   // walked off a ledge
    });
    machine->addTransition(takeoff, fall, 0.18f, [](const Parameters& p, const Node& n) {
        return !p.flag("grounded") && (n.finished() || p.get("vz") < 0);
    });
    machine->addTransition(takeoff, land, 0.08f, [](const Parameters& p, const Node&) {
        return p.flag("grounded") && !p.flag("jump");
    });
    machine->addTransition(fall, land, 0.06f, [](const Parameters& p, const Node&) { return p.flag("grounded"); });
    // Out of a landing straight into running when moving: no stop to recover.
    machine->addTransition(land, ground, 0.2f, [](const Parameters& p, const Node& n) {
        return n.finished() || p.get("speed") > 2.5f;
    });
    machine->start(ground);
    animator->addLayer({"base", machine, BoneMask::all(animator->skeleton()), 1, false});

    animator->feet.pelvis = rig.pelvis;
    for (int i = 0; i < 2; ++i) {
        animator->feet.thigh[i] = rig.thigh[i];
        animator->feet.calf[i] = rig.calf[i];
        animator->feet.foot[i] = rig.foot[i];
    }
    animator->look.head = rig.head;
    animator->look.neck = rig.neck;
    animator->look.chest = rig.spine[2];
    return animator;
}

} // namespace engine::animation
