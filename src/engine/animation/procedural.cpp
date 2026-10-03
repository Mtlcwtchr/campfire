#include "engine/animation/procedural.hpp"
#include "engine/animation/ik.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace engine::animation {
namespace {
constexpr float kTau = 6.28318531f;
// Axes of the character's rest frame. A positive pitch (about +y) tips the top
// of an upright bone forward and swings the bottom of a hanging one back.
constexpr Vec3 kPitch{0, 1, 0}, kRoll{1, 0, 0}, kYaw{0, 0, 1};
float smooth(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3 - 2 * x); }
float styleValue(float speed, float walk, float jog, float sprint) {
    if (speed <= 1.6f) return walk;
    if (speed <= 4.2f) return walk + (jog - walk) * ((speed - 1.6f) / 2.6f);
    return jog + (sprint - jog) * std::clamp((speed - 4.2f) / 2.8f, 0.0f, 1.0f);
}
} // namespace

float gaitStride(float speed) { return styleValue(speed, 1.35f, 2.3f, 3.2f); }
float gaitContact(float phase, float speed, int side) {
    const float leg = phase + float(side) * 0.5f - std::floor(phase + float(side) * 0.5f);
    const float stance = styleValue(speed, 0.62f, 0.42f, 0.35f);
    const float contact = smooth(leg / 0.065f) * (1 - smooth((leg - stance + 0.085f) / 0.085f));
    const float moving = smooth((speed - 0.08f) / 0.45f);
    return 1 + (contact - 1) * moving;
}

HumanoidRig HumanoidRig::find(const Skeleton& s) {
    HumanoidRig r;
    r.pelvis = s.find("pelvis");
    r.spine[0] = s.find("spine_01");
    r.spine[1] = s.find("spine_03");
    r.spine[2] = s.find("spine_05");
    if (r.spine[2] < 0) r.spine[2] = s.find("spine_04");
    r.neck = s.find("neck_01");
    r.head = s.find("head");
    const char* side[2] = {"_l", "_r"};
    for (int i = 0; i < 2; ++i) {
        r.clavicle[i] = s.find(std::string("clavicle") + side[i]);
        r.upperarm[i] = s.find(std::string("upperarm") + side[i]);
        r.lowerarm[i] = s.find(std::string("lowerarm") + side[i]);
        r.hand[i] = s.find(std::string("hand") + side[i]);
        r.thigh[i] = s.find(std::string("thigh") + side[i]);
        r.calf[i] = s.find(std::string("calf") + side[i]);
        r.foot[i] = s.find(std::string("foot") + side[i]);
    }
    return r;
}

Stance Stance::fit(const Skeleton& s, const HumanoidRig& rig, float margin) {
    Stance out;
    for (int i = 0; i < 2; ++i) {
        if (rig.upperarm[i] < 0 || rig.lowerarm[i] < 0) continue;
        const Vec3 arm = s.joints[std::size_t(rig.lowerarm[i])].restModel.translation -
                         s.joints[std::size_t(rig.upperarm[i])].restModel.translation;
        // Out from hanging straight down, in the body's side plane.
        const float out_ = std::atan2(std::abs(arm.y), -arm.z);
        out.armsDown[i] = std::max(0.0f, out_ - margin);
    }
    return out;
}

Vec3 Stance::elbowAxis(int side) const {
    const float roll = (side == 0 ? -1.0f : 1.0f) * armsDown[side];
    return Quat::axisAngle(kRoll, -roll).rotate(kPitch);
}

void Stance::apply(const Skeleton& s, const HumanoidRig& rig, Pose& pose) const {
    for (int i = 0; i < 2; ++i) {
        // The left arm is on +y: bringing it down is a negative roll.
        pose.rotateInRestFrame(s, rig.upperarm[i], kRoll, (i == 0 ? -1.0f : 1.0f) * armsDown[i]);
        pose.rotateInRestFrame(s, rig.lowerarm[i], elbowAxis(i), -elbows);
    }
}

void IdleMotion::sample(const Skeleton& s, float phase, const Parameters& p, Pose& out) const {
    out.reset(s);
    stance_.apply(s, rig_, out);
    const float t = p.get("time", phase * 4.2f) * (kTau / 4.2f);
    const float breath = std::sin(t);
    const float shift = std::sin(t * 0.48f);                 // weight from foot to foot
    out.rotateInRestFrame(s, rig_.spine[1], kPitch, 0.012f * breath);
    out.rotateInRestFrame(s, rig_.spine[2], kPitch, -0.008f * breath);
    for (int i = 0; i < 2; ++i) {
        out.rotateInRestFrame(s, rig_.clavicle[i], kRoll, (i == 0 ? 1.0f : -1.0f) * 0.01f * breath);
        out.rotateInRestFrame(s, rig_.calf[i], kPitch, 0.05f + 0.03f * (i == 0 ? shift : -shift));
        out.rotateInRestFrame(s, rig_.thigh[i], kPitch, -0.03f);
    }
    if (rig_.pelvis >= 0) {
        out.local[std::size_t(rig_.pelvis)].translation =
                out.local[std::size_t(rig_.pelvis)].translation + Vec3{0, 0.012f * shift, -0.012f};
        out.rotateInRestFrame(s, rig_.pelvis, kRoll, 0.02f * shift);
    }
    out.rotateInRestFrame(s, rig_.spine[0], kRoll, -0.015f * shift + std::clamp(-p.get("turn") * 0.02f, -0.06f, 0.06f));
    out.rotateInRestFrame(s, rig_.head, kYaw, 0.05f * std::sin(t * 0.5f + 1.3f));
}

GaitMotion::Style GaitMotion::walk() { return {}; }
GaitMotion::Style GaitMotion::jog() {
    Style s; s.speed = 4.2f; s.stride = 2.3f; s.reach = 0.55f; s.knee = 1.05f; s.arms = 0.45f;
    s.elbows = 0.85f; s.bob = 0.035f; s.lean = 0.12f; s.flight = 0.5f;
    s.stance = 0.42f; s.stepHeight = 0.12f;
    return s;
}
GaitMotion::Style GaitMotion::sprint() {
    Style s; s.speed = 7.0f; s.stride = 3.2f; s.reach = 0.72f; s.knee = 1.45f; s.arms = 0.70f;
    s.elbows = 1.15f; s.bob = 0.05f; s.lean = 0.24f; s.flight = 1.0f;
    s.stance = 0.35f; s.stepHeight = 0.17f;
    return s;
}

void GaitMotion::sample(const Skeleton& s, float phase, const Parameters& p, Pose& out) const {
    out.reset(s);
    stance_.apply(s, rig_, out);
    const float phi = phase * kTau;
    if (rig_.pelvis >= 0) {
        auto& t = out.local[std::size_t(rig_.pelvis)].translation;
        t = t + Vec3{0, 0, -style_.bob * 1.15f - style_.bob * std::cos(2 * phi)};
        out.rotateInRestFrame(s, rig_.pelvis, kYaw, 0.06f * std::sin(phi));
        out.rotateInRestFrame(s, rig_.pelvis, kRoll, 0.025f * std::cos(phi));
    }
    for (int i = 0; i < 2; ++i) {
        const float leg = phase + float(i) * 0.5f - std::floor(phase + float(i) * 0.5f);
        const float reach = style_.stride * style_.stance * 0.5f;
        float travel, lift = 0, toe;
        if (leg < style_.stance) {
            // The supporting foot moves back at ground speed. A sinusoidal
            // thigh swing had no stationary part and slid throughout contact.
            const float support = leg / style_.stance;
            travel = reach * (1 - 2 * support);
            toe = -0.14f * (1 - smooth(support / 0.13f)) +
                   0.32f * smooth((support - 0.76f) / 0.24f);
        } else {
            const float swing = (leg - style_.stance) / (1 - style_.stance);
            travel = reach * (2 * smooth(swing) - 1);
            lift = style_.stepHeight * std::pow(std::max(0.0f, std::sin(swing * 3.14159265f)), 1.35f);
            toe = -0.12f * std::sin(swing * 3.14159265f);
        }
        if (rig_.foot[i] >= 0) {
            Vec3 target = s.joints[std::size_t(rig_.foot[i])].restModel.translation;
            target.x += travel;
            target.z += lift;
            twoBoneIk(s, out, rig_.thigh[i], rig_.calf[i], rig_.foot[i], target,
                      {1, i == 0 ? 0.06f : -0.06f, 0}, 1, &model_);
            out.rotateInRestFrame(s, rig_.foot[i], kPitch, toe);
        }
        const float arm = style_.arms * std::cos(phi + float(i) * kTau * 0.5f + kTau * 0.5f);
        out.rotateInRestFrame(s, rig_.upperarm[i], kPitch, -arm);
        out.rotateInRestFrame(s, rig_.lowerarm[i], stance_.elbowAxis(i), -style_.elbows);
    }
    const float turn = std::clamp(-p.get("turn") * 0.045f, -0.14f, 0.14f);
    const float balance = std::clamp(p.get("acceleration") * 0.011f + p.get("slope") * 0.09f, -0.09f, 0.13f);
    out.rotateInRestFrame(s, rig_.spine[0], kPitch, style_.lean + balance);
    out.rotateInRestFrame(s, rig_.spine[0], kRoll, turn);
    out.rotateInRestFrame(s, rig_.spine[1], kYaw, -0.10f * std::sin(phi));
    out.rotateInRestFrame(s, rig_.neck, kPitch, -(style_.lean + balance) * 0.6f);
    out.rotateInRestFrame(s, rig_.head, kYaw, 0.025f * std::sin(phi));
}

float AirMotion::duration(const Parameters&) const {
    switch (kind_) {
        case Kind::TakeOff: return 0.22f;
        case Kind::Fall: return 1.2f;
        case Kind::Land: return 0.32f;
    }
    return 1;
}

void AirMotion::sample(const Skeleton& s, float phase, const Parameters& p, Pose& out) const {
    out.reset(s);
    stance_.apply(s, rig_, out);
    float crouch = 0, tuck = 0, arms = 0;
    switch (kind_) {
        case Kind::TakeOff:   // down, then springing up
            crouch = std::sin(std::min(phase, 1.0f) * 3.14159f) * 0.5f;
            arms = phase * 0.6f;
            break;
        case Kind::Fall:      // legs a little tucked, arms out for balance
            tuck = 0.45f + 0.05f * std::sin(phase * kTau);
            arms = 0.7f;
            break;
        case Kind::Land:      // the knees take it and give it back
            crouch = (1 - phase) * (1 - phase) * std::clamp(0.28f + p.get("impact") * 0.07f, 0.35f, 0.95f);
            arms = (1 - phase) * 0.4f;
            break;
    }
    for (int i = 0; i < 2; ++i) {
        const float side = i == 0 ? 1.0f : -1.0f;
        out.rotateInRestFrame(s, rig_.thigh[i], kPitch, -(crouch + tuck) * (i == 0 ? 1.0f : 0.7f));
        out.rotateInRestFrame(s, rig_.calf[i], kPitch, (crouch * 1.9f + tuck * 1.4f) * (i == 0 ? 1.0f : 0.8f));
        out.rotateInRestFrame(s, rig_.foot[i], kPitch, -crouch * 0.6f);
        out.rotateInRestFrame(s, rig_.upperarm[i], kRoll, side * arms * 0.5f);
        out.rotateInRestFrame(s, rig_.upperarm[i], kPitch, -arms * 0.4f);
    }
    if (rig_.pelvis >= 0) {
        auto& t = out.local[std::size_t(rig_.pelvis)].translation;
        t = t + Vec3{0, 0, -crouch * 0.28f};
    }
    out.rotateInRestFrame(s, rig_.spine[0], kPitch, crouch * 0.35f + tuck * 0.15f);
}

} // namespace engine::animation
