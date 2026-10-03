#include "game/render/character_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

namespace game {
namespace {
using Affine = CharacterModel::Affine;

Affine identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; }

Affine multiply(const Affine& a, const Affine& b) {
    Affine r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j];
        r[i * 4 + 3] += a[i * 4 + 3];
    }
    return r;
}
// A 4x4 row-major as written by the packer, its bottom row dropped.
Affine fromRows(const float* m) {
    Affine r{};
    std::memcpy(r.data(), m, sizeof(float) * 12);
    return r;
}
Affine rotation(double ax, double ay, double az, double angle) {
    const double c = std::cos(angle), s = std::sin(angle), t = 1 - c;
    return {float(t * ax * ax + c), float(t * ax * ay - s * az), float(t * ax * az + s * ay), 0,
            float(t * ax * ay + s * az), float(t * ay * ay + c), float(t * ay * az - s * ax), 0,
            float(t * ax * az - s * ay), float(t * ay * az + s * ax), float(t * az * az + c), 0};
}
Affine translation(double x, double y, double z) { return {1, 0, 0, float(x), 0, 1, 0, float(y), 0, 0, 1, float(z)}; }
Affine inverse(const Affine& m) {
    // General 3x3 inverse (the rest transforms may carry scale), then the translation.
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    const float k = std::abs(det) > 1e-12f ? 1.0f / det : 0.0f;
    Affine r{A * k, -(b * i - c * h) * k, (b * f - c * e) * k, 0,
             B * k, (a * i - c * g) * k, -(a * f - c * d) * k, 0,
             C * k, -(a * h - b * g) * k, (a * e - b * d) * k, 0};
    for (int row = 0; row < 3; ++row)
        r[row * 4 + 3] = -(r[row * 4] * m[3] + r[row * 4 + 1] * m[7] + r[row * 4 + 2] * m[11]);
    return r;
}
// Rotation, translation and a uniform scale out of an affine (the scale is
// the mean of the columns' lengths: a rig's joints are not sheared).
engine::animation::Transform decompose(const Affine& m) {
    using namespace engine::animation;
    Transform t;
    t.translation = {m[3], m[7], m[11]};
    const float sx = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    const float sy = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    const float sz = std::sqrt(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
    t.scale = (sx + sy + sz) / 3.0f;
    const float r00 = m[0] / sx, r01 = m[1] / sy, r02 = m[2] / sz;
    const float r10 = m[4] / sx, r11 = m[5] / sy, r12 = m[6] / sz;
    const float r20 = m[8] / sx, r21 = m[9] / sy, r22 = m[10] / sz;
    Quat q;
    const float trace = r00 + r11 + r22;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2;
        q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2;
        q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2;
        q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2;
        q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }
    t.rotation = normalized(q);
    return t;
}
Affine compose(const engine::animation::Transform& t) {
    const auto& q = t.rotation;
    const float x = q.x, y = q.y, z = q.z, w = q.w, s = t.scale;
    return {(1 - 2 * (y * y + z * z)) * s, 2 * (x * y - z * w) * s, 2 * (x * z + y * w) * s, t.translation.x,
            2 * (x * y + z * w) * s, (1 - 2 * (x * x + z * z)) * s, 2 * (y * z - x * w) * s, t.translation.y,
            2 * (x * z - y * w) * s, 2 * (y * z + x * w) * s, (1 - 2 * (x * x + y * y)) * s, t.translation.z};
}
} // namespace

int CharacterModel::jointNamed(const char* name) const {
    for (std::size_t i = 0; i < joints_.size(); ++i)
        if (joints_[i].name == name) return int(i);
    return -1;
}

std::size_t CharacterModel::trianglesOf(std::size_t level) const {
    std::size_t n = 0;
    for (const auto& r : ranges_.at(level)) n += r.count / 3;
    return n;
}

bool CharacterModel::load(const std::filesystem::path& directory, std::string& error) {
    directory_ = directory;
    nlohmann::json meta;
    try {
        std::ifstream in(directory / "character.json");
        if (!in) { error = "no character.json in " + directory.string(); return false; }
        in >> meta;
    } catch (const std::exception& e) {
        error = std::string("character.json: ") + e.what();
        return false;
    }
    std::ifstream bin(directory / "character.bin", std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(bin)), {});
    if (data.size() < 24 || std::memcmp(data.data(), "CHR1", 4) != 0) { error = "character.bin: not CHR1"; return false; }
    std::uint32_t head[5];
    std::memcpy(head, data.data() + 4, sizeof head);
    const std::uint32_t vertices = head[0], indexCount = head[1], jointCount = head[2], materials = head[3], levels = head[4];
    constexpr std::size_t kVertexBytes = 56, kJointBytes = 4 + 64 + 64 + 32;
    const std::size_t need = 24 + std::size_t(levels) * materials * 8 + std::size_t(vertices) * kVertexBytes +
                             std::size_t(indexCount) * 4 + std::size_t(jointCount) * kJointBytes;
    if (data.size() != need || jointCount == 0 || jointCount > 255 || levels == 0 || materials == 0) {
        error = "character.bin: sizes do not add up";
        return false;
    }
    std::size_t at = 24;
    ranges_.assign(levels, std::vector<Range>(materials));
    for (auto& row : ranges_)
        for (auto& r : row) {
            std::memcpy(&r, data.data() + at, 8);
            at += 8;
            if (std::size_t(r.first) + r.count > indexCount) { error = "character.bin: range out of bounds"; return false; }
        }
    position_.resize(std::size_t(vertices) * 3);
    normal_.resize(std::size_t(vertices) * 3);
    uv_.resize(std::size_t(vertices) * 2);
    boneIndex_.resize(vertices);
    boneWeight_.resize(vertices);
    for (std::uint32_t v = 0; v < vertices; ++v, at += kVertexBytes) {
        const char* p = data.data() + at;
        std::memcpy(&position_[v * 3], p, 12);
        std::memcpy(&normal_[v * 3], p + 12, 12);
        std::memcpy(&uv_[v * 2], p + 40, 8);
        std::uint8_t j[4], w[4];
        std::memcpy(j, p + 48, 4);
        std::memcpy(w, p + 52, 4);
        for (int k = 0; k < 4; ++k) {
            boneIndex_[v][k] = j[k] < jointCount ? j[k] : 0;
            boneWeight_[v][k] = float(w[k]) / 255.0f;
        }
    }
    indices_.resize(indexCount);
    std::memcpy(indices_.data(), data.data() + at, std::size_t(indexCount) * 4);
    at += std::size_t(indexCount) * 4;
    for (const auto i : indices_)
        if (i >= vertices) { error = "character.bin: index out of bounds"; return false; }

    // The character frame: feet centred on the origin, facing +x.
    double cx = 0, cy = 0, low = 0;
    if (meta.contains("impostor")) {
        const auto& c = meta["impostor"]["centre"];
        cx = c[0]; cy = c[1]; low = c[2];
    } else {
        const auto& lo = meta["bounds"]["min"], & hi = meta["bounds"]["max"];
        cx = (double(lo[0]) + double(hi[0])) * 0.5; cy = (double(lo[1]) + double(hi[1])) * 0.5; low = lo[2];
    }
    height_ = double(meta["bounds"]["max"][2]) - low;
    const double fx = meta["forward"][0], fy = meta["forward"][1];
    const double facing = std::atan2(fy, fx) + double(meta.value("facing_correction", 0.0));
    const Affine frame = multiply(rotation(0, 0, 1, -facing), translation(-cx, -cy, -low));

    joints_.resize(jointCount);
    std::vector<Affine> local(jointCount);
    for (std::uint32_t k = 0; k < jointCount; ++k, at += kJointBytes) {
        const char* p = data.data() + at;
        std::memcpy(&joints_[k].parent, p, 4);
        float m[16], b[16];
        std::memcpy(m, p + 4, 64);
        std::memcpy(b, p + 68, 64);
        char name[33]{};
        std::memcpy(name, p + 132, 32);
        joints_[k].name = name;
        local[k] = fromRows(m);
        const Affine inverseBind = fromRows(b);
        if (joints_[k].parent >= int(k)) { error = "character.bin: joints out of order"; return false; }
        const Affine model = joints_[k].parent < 0 ? local[k] : multiply(joints_[joints_[k].parent].restWorld, local[k]);
        joints_[k].restWorld = model;   // still in the source frame here
        joints_[k].bind = multiply(frame, multiply(model, inverseBind));
    }
    for (std::size_t k = 0; k < joints_.size(); ++k) {
        auto& j = joints_[k];
        j.restWorld = multiply(frame, j.restWorld);
        j.restLocal = j.parent < 0 ? j.restWorld : local[k];
        j.offset = multiply(inverse(j.restWorld), j.bind);
    }

    materials_.clear();
    for (const auto& m : meta["materials"]) {
        Material out;
        out.name = m.value("name", "");
        const std::string alpha = m.value("alpha", "OPAQUE");
        out.cutout = alpha != "OPAQUE";
        out.albedo = m.value("albedo", "");
        out.normal = m.value("normal", "");
        out.surface = m.value("surface", "");
        materials_.push_back(std::move(out));
    }
    if (materials_.size() != materials) { error = "character.json and .bin disagree on materials"; return false; }
    impostor_ = {};
    if (meta.contains("impostor")) {
        const auto& imp = meta["impostor"];
        impostor_.present = true;
        impostor_.width = imp["width"];
        impostor_.height = imp["height"];
        for (const auto& c : imp["colours"]) impostor_.colours.push_back(c);
        for (const auto& n : imp["normals"]) impostor_.normals.push_back(n);
        if (impostor_.colours.size() != 8 || impostor_.normals.size() != 8) impostor_.present = false;
    }

    levelVertices_.assign(levels, {});
    for (std::size_t l = 0; l < levels; ++l) {
        std::vector<bool> used(vertices, false);
        for (const auto& r : ranges_[l])
            for (std::uint32_t i = r.first; i < r.first + r.count; ++i) used[indices_[i]] = true;
        for (std::uint32_t v = 0; v < vertices; ++v)
            if (used[v]) levelVertices_[l].push_back(v);
    }

    buildFirstPerson();
    if (const int head = jointNamed("head"); head >= 0) {
        const auto& w = joints_[std::size_t(head)].restWorld;
        eyeHeight_ = double(w[11]) + 0.07;
        eyeForward_ = std::max(0.0, double(w[3])) + 0.14;
    }
    return true;
}

engine::animation::Skeleton CharacterModel::skeleton() const {
    engine::animation::Skeleton s;
    s.joints.reserve(joints_.size());
    for (const auto& j : joints_) s.joints.push_back({j.name, j.parent, decompose(j.restLocal), {}});
    s.finish();
    return s;
}

void CharacterModel::skinning(const std::vector<engine::animation::Transform>& model, std::vector<Affine>& out) const {
    out.resize(joints_.size());
    for (std::size_t k = 0; k < joints_.size(); ++k)
        out[k] = k < model.size() ? multiply(compose(model[k]), joints_[k].offset) : joints_[k].bind;
}

void CharacterModel::buildFirstPerson() {
    // The head: the neck and everything below it in the tree. A triangle goes
    // if any of its corners is mostly carried by the head - the helmet, the
    // face, the eyes, the hood's crown - so the eye, which is in there, sees
    // out of it.
    int neck = jointNamed("neck_01");
    if (neck < 0) neck = jointNamed("head");
    std::vector<bool> head(joints_.size(), false);
    for (std::size_t k = 0; k < joints_.size(); ++k)
        for (int j = int(k); j >= 0; j = joints_[std::size_t(j)].parent)
            if (j == neck) { head[k] = true; break; }
    std::vector<bool> inHead(vertexCount(), false);
    for (std::size_t v = 0; v < vertexCount(); ++v) {
        float carried = 0;
        for (int k = 0; k < 4; ++k)
            if (head[boneIndex_[v][k]]) carried += boneWeight_[v][k];
        inHead[v] = carried > 0.5f;
    }
    firstPerson_.assign(ranges_.size(), {});
    for (std::size_t l = 0; l < ranges_.size(); ++l)
        for (const auto& r : ranges_[l]) {
            Range out{std::uint32_t(indices_.size()), 0};
            for (std::uint32_t i = r.first; i + 2 < r.first + r.count; i += 3) {
                if (inHead[indices_[i]] || inHead[indices_[i + 1]] || inHead[indices_[i + 2]]) continue;
                indices_.push_back(indices_[i]);
                indices_.push_back(indices_[i + 1]);
                indices_.push_back(indices_[i + 2]);
            }
            out.count = std::uint32_t(indices_.size()) - out.first;
            firstPerson_[l].push_back(out);
        }
}

void CharacterModel::skin(const std::vector<Affine>& skinning, std::size_t level,
                          std::vector<CharacterSkinnedVertex>& out) const {
    out.resize(vertexCount());
    if (skinning.size() != joints_.size() || level >= levelVertices_.size()) return;
    for (const std::uint32_t v : levelVertices_[level]) {
        const float* p = &position_[std::size_t(v) * 3];
        const float* nm = &normal_[std::size_t(v) * 3];
        float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0;
        for (int k = 0; k < 4; ++k) {
            const float w = boneWeight_[v][k];
            if (w <= 0) continue;
            const Affine& m = skinning[boneIndex_[v][k]];
            x += w * (m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]);
            y += w * (m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7]);
            z += w * (m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]);
            nx += w * (m[0] * nm[0] + m[1] * nm[1] + m[2] * nm[2]);
            ny += w * (m[4] * nm[0] + m[5] * nm[1] + m[6] * nm[2]);
            nz += w * (m[8] * nm[0] + m[9] * nm[1] + m[10] * nm[2]);
        }
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        const float inv = len > 1e-8f ? 1.0f / len : 0.0f;
        out[v] = {{x, y, z}, {nx * inv, ny * inv, nz * inv}};
    }
}

} // namespace game
