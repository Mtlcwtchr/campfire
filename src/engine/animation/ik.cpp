#include "engine/animation/ik.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine::animation {
namespace {
// A rotation in the posed model frame, about the joint, applied to the joint
// (its children follow): the local rotation becomes P^-1 Q P L, P being the
// parent's posed model rotation.
void rotateModel(const Skeleton& s, Pose& pose, const std::vector<Transform>& model, int j, Quat q) {
    const int parent = s.joints[std::size_t(j)].parent;
    const Quat p = parent < 0 ? Quat{} : model[std::size_t(parent)].rotation;
    pose.local[std::size_t(j)].rotation = normalized(p.conjugate() * q * p * pose.local[std::size_t(j)].rotation);
}
} // namespace

void twoBoneIk(const Skeleton& s, Pose& pose, int root, int mid, int end, Vec3 target, Vec3 pole, float weight,
               std::vector<Transform>* scratch) {
    const auto valid = [&](int j) { return j >= 0 && std::size_t(j) < s.size() && std::size_t(j) < pose.local.size(); };
    if (!valid(root) || !valid(mid) || !valid(end) || weight <= 0) return;
    const Transform originalRoot = pose.local[std::size_t(root)], originalMid = pose.local[std::size_t(mid)];
    std::vector<Transform> localModel;
    auto& model = scratch ? *scratch : localModel;
    pose.model(s, model);
    const Vec3 a = model[std::size_t(root)].translation;
    const Vec3 b = model[std::size_t(mid)].translation;
    const Vec3 c = model[std::size_t(end)].translation;
    const float l1 = length(b - a), l2 = length(c - b);
    if (l1 < 1e-4f || l2 < 1e-4f) return;
    const float distance = length(target - a);
    const Vec3 along = normalized(distance > 1e-6f ? target - a : c - a);
    const float reach = std::clamp(distance, std::abs(l1 - l2) + 1e-5f, (l1 + l2) * 0.99999f);
    Vec3 bend = pole - along * dot(pole, along);
    if (length(bend) < 1e-5f) bend = (b - a) - along * dot(b - a, along);
    if (length(bend) < 1e-5f) bend = cross(along, std::abs(along.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0});
    bend = normalized(bend);
    const float cosine = std::clamp((l1 * l1 + reach * reach - l2 * l2) / (2 * l1 * reach), -1.0f, 1.0f);
    const Vec3 knee = a + along * (l1 * cosine) + bend * (l1 * std::sqrt(std::max(0.0f, 1 - cosine * cosine)));
    // Solve the knee's position directly. The bend plane remains defined even
    // when the source leg is straight, avoiding pole flips near extension.
    rotateModel(s, pose, model, root, between(normalized(b - a), normalized(knee - a)));
    pose.model(s, model);
    const Vec3 posedMid = model[std::size_t(mid)].translation;
    const Vec3 posedEnd = model[std::size_t(end)].translation;
    const Vec3 endpoint = a + along * reach;
    rotateModel(s, pose, model, mid, between(normalized(posedEnd - posedMid), normalized(endpoint - posedMid)));
    if (weight < 1) {
        const float w = std::clamp(weight, 0.0f, 1.0f);
        pose.local[std::size_t(root)].rotation = nlerp(originalRoot.rotation, pose.local[std::size_t(root)].rotation, w);
        pose.local[std::size_t(mid)].rotation = nlerp(originalMid.rotation, pose.local[std::size_t(mid)].rotation, w);
    }
}

void FootPlacement::reset() {
    planted_[0] = planted_[1] = {};
    previous_ = {};
    pelvisDrop_ = 0;
}

void FootPlacement::apply(const Skeleton& s, Pose& pose, float weight) {
    const auto valid = [&](int j) { return j >= 0 && std::size_t(j) < pose.local.size(); };
    if (!ground || weight <= 0 || !valid(pelvis)) { reset(); return; }
    if (!valid(foot[0]) || !valid(foot[1])) { reset(); return; }
    const float dt = std::clamp(seconds, 0.0f, 0.1f);
    if (frame.valid && previous_.valid &&
        (std::hypot(frame.x - previous_.x, frame.y - previous_.y) > 1.5 ||
         std::abs(frame.z - previous_.z) > 0.65)) reset();
    previous_ = frame;
    auto& model = model_;
    pose.model(s, model);
    const double cy = std::cos(frame.yaw), sy = std::sin(frame.yaw);
    Vec3 target[2];
    float height[2]{};
    for (int side = 0; side < 2; ++side) {
        const Vec3 animated = model[std::size_t(foot[side])].translation;
        auto& plant = planted_[side];
        const float support = std::clamp(contact[side], 0.0f, 1.0f);
        plant.contact += (support - plant.contact) * (1 - std::exp(-dt * 35));
        if (support < 0.15f || !frame.valid) plant.locked = false;
        if (support > 0.65f && !plant.locked && frame.valid) {
            plant.x = frame.x + cy * animated.x - sy * animated.y;
            plant.y = frame.y + sy * animated.x + cy * animated.y;
            plant.locked = true;
        }
        target[side] = animated;
        if (plant.locked) {
            const double dx = plant.x - frame.x, dy = plant.y - frame.y;
            const Vec3 anchor{float(cy * dx + sy * dy), float(-sy * dx + cy * dy), animated.z};
            // Release rather than drag a foot through a teleport or a sharp
            // direction change beyond what this leg can naturally correct.
            if (std::hypot(anchor.x - animated.x, anchor.y - animated.y) > maxAdjust * 1.5f) plant.locked = false;
            else target[side] = lerp(animated, anchor, plant.contact);
        }
        height[side] = std::clamp(ground(target[side].x, target[side].y), -maxAdjust, maxAdjust);
        const float ankle = std::max(footHeight, s.joints[std::size_t(foot[side])].restModel.translation.z);
        const float clearance = std::max(ankle, animated.z);
        target[side].z = height[side] + clearance + (ankle - clearance) * plant.contact;
    }
    const float wantedDrop = std::min(0.0f, std::min(height[0], height[1])) * std::clamp(weight, 0.0f, 1.0f);
    pelvisDrop_ += (wantedDrop - pelvisDrop_) * (1 - std::exp(-dt * 14));
    if (pelvisDrop_ < 0) {
        const int parent = s.joints[std::size_t(pelvis)].parent;
        const Transform p = parent < 0 ? Transform{} : model[std::size_t(parent)];
        pose.local[std::size_t(pelvis)].translation = pose.local[std::size_t(pelvis)].translation +
            p.rotation.conjugate().rotate(Vec3{0, 0, pelvisDrop_}) * (p.scale != 0 ? 1 / p.scale : 1);
    }
    for (int side = 0; side < 2; ++side) {
        if (!valid(thigh[side]) || !valid(calf[side])) continue;
        twoBoneIk(s, pose, thigh[side], calf[side], foot[side], target[side],
                  {1, side == 0 ? 0.06f : -0.06f, 0}, weight, &model);
        pose.model(s, model);
        const float e = 0.15f;
        const float x = target[side].x, y = target[side].y;
        const float gx = std::clamp((ground(x + e, y) - ground(x - e, y)) / (2 * e), -0.7f, 0.7f);
        const float gy = std::clamp((ground(x, y + e) - ground(x, y - e)) / (2 * e), -0.7f, 0.7f);
        const Vec3 normal = normalized(Vec3{-gx, -gy, 1});
        const float support = planted_[side].contact * std::clamp(weight, 0.0f, 1.0f);
        rotateModel(s, pose, model, foot[side], nlerp({}, between({0, 0, 1}, normal), support * 0.85f));
    }
}

void LookAt::apply(const Skeleton& s, Pose& pose, Vec3 direction, float weight) const {
    if (head < 0 || weight <= 0 || length(direction) < 1e-5f) return;
    const Vec3 d = normalized(direction);
    const float yaw = std::clamp(std::atan2(d.y, d.x), -yawLimit, yawLimit) * weight;
    const float pitch = std::clamp(std::asin(std::clamp(d.z, -1.0f, 1.0f)), -pitchLimit, pitchLimit) * weight;
    // Shared down the chain, most in the head.
    const struct { int joint; float share; } chain[] = {{chest, 0.2f}, {neck, 0.3f}, {head, 0.5f}};
    std::vector<Transform> model;
    for (const auto& link : chain) {
        if (link.joint < 0) continue;
        pose.model(s, model);
        // Yaw about the vertical, then pitch about the turned right axis
        // (down for a positive pitch is a rotation about +y: the frame faces +x).
        const Quat turn = Quat::axisAngle({0, 0, 1}, yaw * link.share);
        const Vec3 right = turn.rotate({0, 1, 0});
        rotateModel(s, pose, model, link.joint, Quat::axisAngle(right, -pitch * link.share) * turn);
    }
}

} // namespace engine::animation
