#pragma once
// Skeleton and pose: the representation every other part of the animator
// reads and writes (engine/animation/*).
//
// A pose is one local transform a joint - rotation, translation, uniform
// scale - relative to its parent, in the skeleton's own frame (metres, z up,
// facing +x by the convention of the character that owns it). Everything that
// makes a pose (a keyframed clip, a procedural generator) writes one; everything
// that combines them (blend spaces, the state machine's cross-fades, layers)
// does it here, joint by joint, with the same three operations: blend, add,
// mask. Forward kinematics happens once, at the end (Pose::model).
//
// Joints are ordered parents first (the packer guarantees it): a single pass
// down the array is a pass down the tree.
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::animation {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    friend Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
};
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalized(Vec3 a) { const float l = length(a); return l > 1e-8f ? a * (1.0f / l) : Vec3{0, 0, 1}; }
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

// Unit quaternion, (x, y, z) the axis part.
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    static Quat axisAngle(Vec3 axis, float angle);
    friend Quat operator*(Quat a, Quat b) {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
    [[nodiscard]] Quat conjugate() const { return {-x, -y, -z, w}; }
    [[nodiscard]] Vec3 rotate(Vec3 v) const;
};
Quat normalized(Quat q);
// Shortest-arc normalised lerp: what blending wants (commutative enough to
// weight many inputs, and exact at the ends).
Quat nlerp(Quat a, Quat b, float t);
// The rotation taking unit vector `from` to unit vector `to`.
Quat between(Vec3 from, Vec3 to);

struct Transform {
    Quat rotation;
    Vec3 translation;
    float scale = 1;
    friend Transform operator*(const Transform& parent, const Transform& child) {
        return {parent.rotation * child.rotation,
                parent.translation + parent.rotation.rotate(child.translation * parent.scale),
                parent.scale * child.scale};
    }
    [[nodiscard]] Vec3 apply(Vec3 p) const { return translation + rotation.rotate(p * scale); }
    [[nodiscard]] Transform inverse() const;
};

struct Skeleton {
    struct Joint {
        std::string name;
        int parent = -1;
        Transform rest;        // local, relative to the parent
        Transform restModel;   // the rest pose, in the skeleton's frame
    };
    std::vector<Joint> joints;

    [[nodiscard]] int find(std::string_view name) const;
    [[nodiscard]] std::size_t size() const { return joints.size(); }
    // Fills restModel from rest. Call after building.
    void finish();
    // Whether `joint` is `ancestor` or below it.
    [[nodiscard]] bool under(int joint, int ancestor) const;
};

// Per joint weights, 0..1: which part of the body a layer owns.
struct BoneMask {
    std::vector<float> weight;
    static BoneMask all(const Skeleton& s, float w = 1) { return {std::vector<float>(s.size(), w)}; }
    // `root` and everything below it at `w`, the rest at `rest`.
    static BoneMask branch(const Skeleton& s, int root, float w = 1, float rest = 0);
};

struct Pose {
    std::vector<Transform> local;

    static Pose rest(const Skeleton& s);
    void reset(const Skeleton& s);
    // this = lerp(this, other, t), joint by joint (mask, if given, scales t).
    void blend(const Pose& other, float t, const BoneMask* mask = nullptr);
    // this = this + (additive - reference) * t: an additive layer, its delta
    // measured against `reference` (usually the rest pose).
    void add(const Pose& additive, const Pose& reference, float t, const BoneMask* mask = nullptr);
    // A rotation about an axis of the skeleton's own (rest) frame at joint
    // `j`, applied on top of what the joint already does, that its children
    // inherit. What procedural generators write with, so they never need to
    // know a bone's local axes.
    void rotateInRestFrame(const Skeleton& s, int j, Vec3 axis, float angle);
    // Model-space transforms (forward kinematics).
    void model(const Skeleton& s, std::vector<Transform>& out) const;
};

// Weighted blend of several poses: weights need not sum to one (they are
// normalised); at most a handful of inputs, each read once.
void blendMany(std::span<const Pose* const> poses, std::span<const float> weights, Pose& out);

} // namespace engine::animation
