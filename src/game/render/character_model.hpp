#pragma once
// A skinned character: the files tools/pack_character.py writes, posed and
// skinned on the CPU.
//
// One character on the screen, so the skinning is done here rather than in a
// vertex shader: a draw item carries thirty-two floats to the vertex stage and
// a skeleton is a hundred and thirty matrices. 73 000 vertices take about a
// millisecond on one core; the coarser levels reference fewer of them, and
// only those are skinned (vertices of a level).
//
// There is no animation in the source file, and none is needed for walking:
// the skeleton is the UE4 one (pelvis, spine_01..05, thigh/calf/foot,
// upperarm/lowerarm), and the gait is swung procedurally from it - legs about
// the hips, knees bending on the forward swing, arms against the legs, the
// pelvis bobbing twice a stride, the spine leaning into speed. Each rotation
// is about an axis of the character's own rest frame at the joint, applied as
// a delta that its descendants inherit, so no bone's local axes are assumed.
//
// Everything is in the character's own frame, metres, z up, origin between
// the feet on the ground, facing +x. CharacterPass places it in the world.
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/animation/pose.hpp"

namespace game {

struct CharacterSkinnedVertex {
    float position[3];
    float normal[3];
};

class CharacterModel {
public:
    // 3x4 row-major: p' = M * (p, 1).
    using Affine = std::array<float, 12>;

    struct Material {
        std::string name;
        bool cutout = false;          // alpha-tested (chainmail, lashes)
        std::string albedo, normal, surface;
    };
    struct Range { std::uint32_t first = 0, count = 0; };
    struct Impostor {
        bool present = false;
        float width = 0, height = 0;
        std::vector<std::string> colours, normals;
    };
    // What the controller reports about its movement (the animator's
    // parameters are made from it).
    struct Gait {
        double phase = 0;     // radians, one stride = 2 pi, driven by displacement
        double speed = 0;     // metres a second over the ground
        double turn = 0;      // radians a second
        double time = 0;
        double vz = 0;        // metres a second, vertical
        bool grounded = true;
        bool jumped = false;  // a jump began this frame
        double acceleration = 0;
        double slope = 0;
        double impact = 0;    // downward velocity before the latest landing
    };

    bool load(const std::filesystem::path& directory, std::string& error);
    [[nodiscard]] bool loaded() const { return !joints_.empty(); }

    [[nodiscard]] std::size_t vertexCount() const { return uv_.size() / 2; }
    [[nodiscard]] const std::vector<float>& uvs() const { return uv_; }
    [[nodiscard]] const std::vector<std::uint32_t>& indices() const { return indices_; }
    [[nodiscard]] const std::vector<Material>& materials() const { return materials_; }
    [[nodiscard]] std::size_t levels() const { return ranges_.size(); }
    [[nodiscard]] const std::vector<Range>& rangesOf(std::size_t level) const { return ranges_[level]; }
    [[nodiscard]] std::size_t trianglesOf(std::size_t level) const;
    [[nodiscard]] const Impostor& impostor() const { return impostor_; }
    [[nodiscard]] double height() const { return height_; }
    // Where the eyes are in the rest pose, in the character frame: the head
    // joint, a little up and forward (first-person camera).
    [[nodiscard]] double eyeHeight() const { return eyeHeight_; }
    [[nodiscard]] double eyeForward() const { return eyeForward_; }
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

    // The skeleton in the character frame, for the animator
    // (engine/animation): rest local transforms, parents first.
    [[nodiscard]] engine::animation::Skeleton skeleton() const;
    // Skinning matrices from a posed skeleton's model transforms.
    void skinning(const std::vector<engine::animation::Transform>& model, std::vector<Affine>& out) const;
    // The same level with the head's triangles left out (helmet, face, eyes):
    // what the first-person camera, which sits inside the head, draws.
    [[nodiscard]] const std::vector<Range>& firstPersonRangesOf(std::size_t level) const { return firstPerson_[level]; }
    // The vertices a level references, skinned; the rest are left as they
    // were (the level's indices never read them). `out` keeps its size.
    void skin(const std::vector<Affine>& skinning, std::size_t level,
              std::vector<CharacterSkinnedVertex>& out) const;

private:
    struct Joint {
        int parent = -1;
        Affine restWorld{};   // in the character frame
        Affine restLocal{};   // relative to the parent joint (the root's: to the character frame)
        Affine bind{};        // restWorld * inverse bind, in the character frame
        Affine offset{};      // restWorld^-1 * bind: what a posed model transform multiplies
        std::string name;
    };
    int jointNamed(const char* name) const;
    void buildFirstPerson();

    std::filesystem::path directory_;
    std::vector<Joint> joints_;
    std::vector<float> position_, normal_, uv_;   // source, in mesh space
    std::vector<std::array<std::uint8_t, 4>> boneIndex_;
    std::vector<std::array<float, 4>> boneWeight_;
    std::vector<std::uint32_t> indices_;
    std::vector<std::vector<Range>> ranges_;      // levels x materials
    std::vector<std::vector<Range>> firstPerson_; // the same, headless
    std::vector<std::vector<std::uint32_t>> levelVertices_;
    std::vector<Material> materials_;
    Impostor impostor_;
    double height_ = 1.8, eyeHeight_ = 1.65, eyeForward_ = 0.12;
};

} // namespace game
