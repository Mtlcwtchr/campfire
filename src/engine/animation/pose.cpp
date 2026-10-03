#include "engine/animation/pose.hpp"

#include <algorithm>

namespace engine::animation {

Quat Quat::axisAngle(Vec3 axis, float angle) {
    const Vec3 a = animation::normalized(axis);
    const float s = std::sin(angle * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
}

Vec3 Quat::rotate(Vec3 v) const {
    const Vec3 u{x, y, z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * w + cross(u, t);
}

Quat normalized(Quat q) {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-8f) return {};
    const float k = 1.0f / l;
    return {q.x * k, q.y * k, q.z * k, q.w * k};
}

Quat nlerp(Quat a, Quat b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
    return normalized({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
}

Quat between(Vec3 from, Vec3 to) {
    const float d = dot(from, to);
    if (d > 0.999999f) return {};
    if (d < -0.999999f) {
        Vec3 axis = cross({1, 0, 0}, from);
        if (length(axis) < 1e-4f) axis = cross({0, 1, 0}, from);
        return Quat::axisAngle(axis, 3.14159265f);
    }
    const Vec3 c = cross(from, to);
    return normalized({c.x, c.y, c.z, 1.0f + d});
}

Transform Transform::inverse() const {
    const Quat r = rotation.conjugate();
    const float s = scale != 0 ? 1.0f / scale : 0.0f;
    return {r, r.rotate(translation * -1.0f) * s, s};
}

int Skeleton::find(std::string_view name) const {
    for (std::size_t i = 0; i < joints.size(); ++i)
        if (joints[i].name == name) return int(i);
    return -1;
}

void Skeleton::finish() {
    for (auto& j : joints)
        j.restModel = j.parent < 0 ? j.rest : joints[std::size_t(j.parent)].restModel * j.rest;
}

bool Skeleton::under(int joint, int ancestor) const {
    for (int j = joint; j >= 0; j = joints[std::size_t(j)].parent)
        if (j == ancestor) return true;
    return false;
}

BoneMask BoneMask::branch(const Skeleton& s, int root, float w, float rest) {
    BoneMask m;
    m.weight.resize(s.size(), rest);
    if (root < 0) return m;
    for (std::size_t j = 0; j < s.size(); ++j)
        if (s.under(int(j), root)) m.weight[j] = w;
    return m;
}

Pose Pose::rest(const Skeleton& s) {
    Pose p;
    p.reset(s);
    return p;
}

void Pose::reset(const Skeleton& s) {
    local.resize(s.size());
    for (std::size_t i=0;i<s.size();++i) local[i]=s.joints[i].rest;
}

void Pose::blend(const Pose& other, float t, const BoneMask* mask) {
    const std::size_t n = std::min(local.size(), other.local.size());
    for (std::size_t j = 0; j < n; ++j) {
        const float k = mask ? t * mask->weight[j] : t;
        if (k <= 0) continue;
        auto& a = local[j];
        const auto& b = other.local[j];
        if (k >= 1) { a = b; continue; }
        a.rotation = nlerp(a.rotation, b.rotation, k);
        a.translation = lerp(a.translation, b.translation, k);
        a.scale += (b.scale - a.scale) * k;
    }
}

void Pose::add(const Pose& additive, const Pose& reference, float t, const BoneMask* mask) {
    const std::size_t n = std::min({local.size(), additive.local.size(), reference.local.size()});
    for (std::size_t j = 0; j < n; ++j) {
        const float k = mask ? t * mask->weight[j] : t;
        if (k <= 0) continue;
        const auto& add = additive.local[j];
        const auto& ref = reference.local[j];
        const Quat delta = nlerp({}, add.rotation * ref.rotation.conjugate(), k);
        local[j].rotation = normalized(delta * local[j].rotation);
        local[j].translation = local[j].translation + (add.translation - ref.translation) * k;
    }
}

void Pose::rotateInRestFrame(const Skeleton& s, int j, Vec3 axis, float angle) {
    if (j < 0 || std::size_t(j) >= local.size() || angle == 0) return;
    // A rotation Q in the rest model frame at the joint is, in the joint's
    // local terms, P^-1 Q P applied before the joint's own rotation, where P
    // is the parent's rest model rotation.
    const int parent = s.joints[std::size_t(j)].parent;
    const Quat p = parent < 0 ? Quat{} : s.joints[std::size_t(parent)].restModel.rotation;
    const Quat q = Quat::axisAngle(axis, angle);
    local[std::size_t(j)].rotation = normalized(p.conjugate() * q * p * local[std::size_t(j)].rotation);
}

void Pose::model(const Skeleton& s, std::vector<Transform>& out) const {
    out.resize(local.size());
    for (std::size_t j = 0; j < local.size(); ++j) {
        const int parent = s.joints[j].parent;
        out[j] = parent < 0 ? local[j] : out[std::size_t(parent)] * local[j];
    }
}

void blendMany(std::span<const Pose* const> poses, std::span<const float> weights, Pose& out) {
    float total = 0;
    for (const float w : weights) total += std::max(0.0f, w);
    if (poses.empty() || total <= 0) return;
    // Running nlerp: pose i joins with weight w_i / (sum of weights so far).
    float seen = 0;
    bool first = true;
    for (std::size_t i = 0; i < poses.size() && i < weights.size(); ++i) {
        const float w = std::max(0.0f, weights[i]);
        if (w <= 0 || !poses[i]) continue;
        seen += w;
        if (first) { out = *poses[i]; first = false; continue; }
        out.blend(*poses[i], w / seen);
    }
}

} // namespace engine::animation
