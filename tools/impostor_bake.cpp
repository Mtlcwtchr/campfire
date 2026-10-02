// impostor_bake - the impostor rasterizer of tools/prepare_scene_models.py
// (`rasterize`) and the depth encoding of tools/bake_impostor_depth.py
// (`encode_depth`), natively. Same projection, same nearest texel at the same
// texture size, same depth rule, same one-pixel colour dilation, same
// rg16-view-b-coverage-a-v1 depth code - checked byte for byte against the
// Python on the shipped catalogue (tools/bake_model_impostors.py --parity).
//
// The Python bakes one triangle per interpreter step: about a minute per
// hemisphere for a tree of thirty thousand triangles. This is milliseconds.
//
//     impostor_bake JOBS.bin OUT.bin
//
// JOBS.bin (little endian), written by tools/bake_model_impostors.py:
//     char[4] "IMPB"; u32 version=1, size, vertices, indices, layers, jobs
//     f32 vertex[vertices][12]   position xyz, normal xyz, uv, colour rgb, layer
//     u32 index[indices]
//     u8  layer[layers][size][size][4]          RGBA, already at `size`
//     job[jobs]: u32 kind (0 ring, 1 basis), u32 view, u32 views, u32 pad,
//                f64 width, height, frame, angle, eye_sign, origin[3], basis[9]
// OUT.bin: per job, colour, normal and depth images, each size*size*4 bytes.
// A depth outside the declared frame is an error, as in encode_depth.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

struct Job {
    std::uint32_t kind = 0, view = 0, views = 8, pad = 0;
    double width = 1, height = 1, frame = 1, angle = 0, eyeSign = 1;
    double origin[3] {};
    double basis[9] {};   // rows: right, up, eye
};

template <class T>
T take(const std::vector<std::uint8_t>& data, std::size_t& at) {
    if (at + sizeof(T) > data.size()) throw std::runtime_error("truncated job file");
    T v;
    std::memcpy(&v, data.data() + at, sizeof(T));
    at += sizeof(T);
    return v;
}

struct Input {
    std::uint32_t size = 0;
    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint8_t> layers;
    std::uint32_t layerCount = 0;
    std::vector<Job> jobs;
};

Input read(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(std::string("cannot read ") + path);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::size_t at = 0;
    if (data.size() < 4 || std::memcmp(data.data(), "IMPB", 4) != 0) throw std::runtime_error("not an IMPB job file");
    at = 4;
    Input in;
    if (take<std::uint32_t>(data, at) != 1) throw std::runtime_error("unknown job file version");
    in.size = take<std::uint32_t>(data, at);
    const auto nv = take<std::uint32_t>(data, at), ni = take<std::uint32_t>(data, at);
    in.layerCount = take<std::uint32_t>(data, at);
    const auto nj = take<std::uint32_t>(data, at);
    if (in.size < 8 || in.size > 4096 || ni % 3 || !in.layerCount) throw std::runtime_error("bad job header");
    const std::size_t layerBytes = std::size_t(in.size) * in.size * 4;
    const std::size_t need = at + std::size_t(nv) * 48 + std::size_t(ni) * 4 + layerBytes * in.layerCount;
    if (need > data.size()) throw std::runtime_error("truncated job file");
    in.vertices.resize(std::size_t(nv) * 12);
    std::memcpy(in.vertices.data(), data.data() + at, std::size_t(nv) * 48);
    at += std::size_t(nv) * 48;
    in.indices.resize(ni);
    std::memcpy(in.indices.data(), data.data() + at, std::size_t(ni) * 4);
    at += std::size_t(ni) * 4;
    in.layers.assign(data.begin() + static_cast<std::ptrdiff_t>(at),
                     data.begin() + static_cast<std::ptrdiff_t>(at + layerBytes * in.layerCount));
    at += layerBytes * in.layerCount;
    for (std::uint32_t i : in.indices)
        if (i >= nv) throw std::runtime_error("index out of range");
    for (std::size_t v = 0; v < nv; ++v) {
        const float layer = in.vertices[v * 12 + 11];
        if (!(layer >= 0) || layer >= static_cast<float>(in.layerCount) || layer != std::floor(layer))
            throw std::runtime_error("invalid texture layer");
    }
    for (std::uint32_t j = 0; j < nj; ++j) {
        Job job;
        job.kind = take<std::uint32_t>(data, at);
        job.view = take<std::uint32_t>(data, at);
        job.views = take<std::uint32_t>(data, at);
        job.pad = take<std::uint32_t>(data, at);
        job.width = take<double>(data, at);
        job.height = take<double>(data, at);
        job.frame = take<double>(data, at);
        job.angle = take<double>(data, at);
        job.eyeSign = take<double>(data, at);
        for (double& o : job.origin) o = take<double>(data, at);
        for (double& b : job.basis) b = take<double>(data, at);
        if (job.kind > 1 || !(job.width > 0) || !(job.height > 0) || !(job.frame > 0) ||
            (job.views != 8 && job.views != 21) || job.view >= job.views)
            throw std::runtime_error("bad job");
        in.jobs.push_back(job);
    }
    if (at != data.size()) throw std::runtime_error("trailing bytes in job file");
    return in;
}

// prepare_scene_models.rasterize, coverage=None, return_depth=True.
//
// Bit-for-bit means the same arithmetic in the same types. Under NumPy 2 a
// Python float meeting a float32 array becomes float32, so the ring views
// (positions float32, angle a Python float) project in float32; the basis
// views subtract a float64 origin and project in float64. Barycentric weights
// mix a numpy float32 scalar into int64 pixel grids and are float64 either way.
// This file is built with -ffp-contract=off: a fused multiply-add rounds once
// where NumPy rounds twice - except inside BLAS, where it is spelled out.
template <class T>
void project(const Input& in, const Job& job, std::vector<T>& sx, std::vector<T>& sy, std::vector<T>& depth) {
    const std::size_t n = in.vertices.size() / 12;
    const T S1 = static_cast<T>(in.size - 1);
    sx.resize(n), sy.resize(n), depth.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float* p = &in.vertices[i * 12];
        if constexpr (std::is_same_v<T, float>) {
            const float c = static_cast<float>(std::cos(job.angle)), s = static_cast<float>(std::sin(job.angle));
            const float w = static_cast<float>(job.width), h = static_cast<float>(job.height);
            const float e = static_cast<float>(job.eyeSign);
            const float pc = p[0] * c, ps = p[1] * s;
            const float sum = pc + ps;
            const float a = sum / w;
            sx[i] = (a + 0.5f) * S1;
            const float b = p[2] / h;
            sy[i] = (1.0f - b) * S1;
            const float ds = p[0] * s, dc = p[1] * c;
            depth[i] = (ds - dc) * e;
        } else {
            const double q[3] = {static_cast<double>(p[0]) - job.origin[0], static_cast<double>(p[1]) - job.origin[1],
                                 static_cast<double>(p[2]) - job.origin[2]};
            const double* b = job.basis;
            double l[3];
            // `(p - origin) @ basis.T` is a BLAS dgemm, which accumulates
            // with fused multiply-adds in this order (exact on 1200 of 1200
            // products measured against NumPy on this machine).
            for (int r = 0; r < 3; ++r)
                l[r] = std::fma(q[2], b[3 * r + 2], std::fma(q[1], b[3 * r + 1], q[0] * b[3 * r]));
            const double a = l[0] / job.width;
            sx[i] = (a + .5) * S1;
            const double c = l[1] / job.height;
            sy[i] = (.5 - c) * S1;
            depth[i] = l[2];
        }
    }
}

template <class T>
void rasterizeT(const Input& in, const Job& job, std::vector<std::uint8_t>& colour, std::vector<std::uint8_t>& normals,
                std::vector<double>& zbuf) {
    const int S = static_cast<int>(in.size);
    std::vector<T> sx, sy, depth;
    project(in, job, sx, sy, depth);
    const std::size_t pixels = std::size_t(S) * S;
    zbuf.assign(pixels, -std::numeric_limits<double>::infinity());
    colour.assign(pixels * 4, 0);
    normals.assign(pixels * 4, 128);
    for (std::size_t i = 0; i < pixels; ++i) normals[i * 4 + 3] = 255;
    const std::size_t layerBytes = pixels * 4;
    const T tiny = static_cast<T>(1e-8);
    for (std::size_t t = 0; t + 2 < in.indices.size(); t += 3) {
        const std::uint32_t ia = in.indices[t], ib = in.indices[t + 1], id = in.indices[t + 2];
        const T ax = sx[ia], ay = sy[ia], bx = sx[ib], by = sy[ib], dx = sx[id], dy = sy[id];
        const T e0 = by - dy, e1 = ax - dx, e2 = dx - bx, e3 = ay - dy;
        const T m0 = e0 * e1, m1 = e2 * e3;
        const T det = m0 + m1;
        if (std::fabs(det) < tiny) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, dx}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, dy}))));
        const int x1 = std::min(S - 1, static_cast<int>(std::ceil(std::max({ax, bx, dx}))));
        const int y1 = std::min(S - 1, static_cast<int>(std::ceil(std::max({ay, by, dy}))));
        if (x1 < x0 || y1 < y0) continue;
        // numpy float32 scalars, promoted against the float64 pixel offsets
        const double fa0 = static_cast<double>(T(by - dy)), fa1 = static_cast<double>(T(dx - bx));
        const double fb0 = static_cast<double>(T(dy - ay)), fb1 = static_cast<double>(T(ax - dx));
        const double ddx = static_cast<double>(dx), ddy = static_cast<double>(dy), ddet = static_cast<double>(det);
        const float* va = &in.vertices[std::size_t(ia) * 12];
        const float* vb = &in.vertices[std::size_t(ib) * 12];
        const float* vd = &in.vertices[std::size_t(id) * 12];
        const double za = static_cast<double>(depth[ia]), zb = static_cast<double>(depth[ib]),
                     zd = static_cast<double>(depth[id]);
        const std::uint8_t* texture = &in.layers[static_cast<std::size_t>(va[11]) * layerBytes];
        for (int y = y0; y <= y1; ++y) {
            const double oy = static_cast<double>(y) - ddy;
            for (int x = x0; x <= x1; ++x) {
                const double ox = static_cast<double>(x) - ddx;
                const double pa = fa0 * ox, qa = fa1 * oy;
                const double wa = (pa + qa) / ddet;
                const double pb = fb0 * ox, qb = fb1 * oy;
                const double wb = (pb + qb) / ddet;
                const double one = 1 - wa;
                const double wc = one - wb;
                if (std::min({wa, wb, wc}) < -1e-5) continue;
                auto dot = [&](double a, double b, double c) {
                    const double ta = wa * a, tb = wb * b, tc = wc * c;
                    return (ta + tb) + tc;
                };
                const double z = dot(za, zb, zd);
                const std::size_t px = std::size_t(y) * S + x;
                if (!(z > zbuf[px])) continue;
                double u = dot(va[6], vb[6], vd[6]);
                double v = dot(va[7], vb[7], vd[7]);
                u = std::fmod(u, 1.0);
                if (u != 0 && u < 0) u += 1.0;
                v = std::fmod(v, 1.0);
                if (v != 0 && v < 0) v += 1.0;
                const double us = u * (S - 1), vs = v * (S - 1);
                const int tx = static_cast<int>(us), ty = static_cast<int>(vs);
                const std::uint8_t* texel = &texture[(std::size_t(ty) * S + tx) * 4];
                if (texel[3] < 51) continue;
                zbuf[px] = z;
                for (int k = 0; k < 3; ++k) {
                    const double tint = dot(va[8 + k], vb[8 + k], vd[8 + k]);
                    const double value = static_cast<double>(texel[k]) * tint;
                    colour[px * 4 + k] = static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0));
                }
                colour[px * 4 + 3] = texel[3];
                const double nx = dot(va[3], vb[3], vd[3]), ny = dot(va[4], vb[4], vd[4]), nz = dot(va[5], vb[5], vd[5]);
                const double sq = (nx * nx + ny * ny) + nz * nz;
                const double len = std::max(std::sqrt(sq), 1e-8);
                const double n3[3] = {nx / len, ny / len, nz / len};
                for (int k = 0; k < 3; ++k) {
                    const double half = n3[k] * .5;
                    const double shifted = half + .5;
                    normals[px * 4 + k] = static_cast<std::uint8_t>(std::clamp(shifted * 255, 0.0, 255.0));
                }
            }
        }
    }
}

void rasterize(const Input& in, const Job& job, std::vector<std::uint8_t>& colour, std::vector<std::uint8_t>& normals,
               std::vector<double>& zbuf) {
    if (job.kind == 0) rasterizeT<float>(in, job, colour, normals, zbuf);
    else rasterizeT<double>(in, job, colour, normals, zbuf);
    const int S = static_cast<int>(in.size);
    // The Python dilation: alpha never changes, so every pass and every
    // repetition sees the same filled set; for an empty texel the last filled
    // neighbour in the order above, below, left, right wins (np.roll wraps).
    std::vector<std::uint8_t> source = colour;
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            const std::size_t px = std::size_t(y) * S + x;
            if (source[px * 4 + 3] > 0) continue;
            const int ny[4] = {(y - 1 + S) % S, (y + 1) % S, y, y};
            const int nx[4] = {x, x, (x - 1 + S) % S, (x + 1) % S};
            for (int k = 0; k < 4; ++k) {
                const std::size_t q = std::size_t(ny[k]) * S + nx[k];
                if (source[q * 4 + 3] > 0) std::memcpy(&colour[px * 4], &source[q * 4], 3);
            }
        }
}

// bake_impostor_depth.encode_depth
void encodeDepth(const std::vector<double>& zbuf, const std::vector<std::uint8_t>& colour, const Job& job,
                 std::vector<std::uint8_t>& out) {
    out.assign(zbuf.size() * 4, 0);
    for (std::size_t i = 0; i < zbuf.size(); ++i) {
        const std::uint8_t coverage = colour[i * 4 + 3];
        const bool valid = coverage > 0 && std::isfinite(zbuf[i]);
        if (valid && std::fabs(zbuf[i]) > job.frame * .5 + 1e-5) throw std::runtime_error("depth exceeds the declared frame");
        const double normalized = valid ? zbuf[i] / job.frame + .5 : .5;
        const auto code = static_cast<std::uint16_t>(std::nearbyint(std::clamp(normalized, 0.0, 1.0) * 65535));
        out[i * 4 + 0] = static_cast<std::uint8_t>(code >> 8);
        out[i * 4 + 1] = static_cast<std::uint8_t>(code & 255);
        out[i * 4 + 2] = static_cast<std::uint8_t>(job.view);
        out[i * 4 + 3] = valid ? coverage : 0;
    }
}

}   // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: impostor_bake JOBS.bin OUT.bin\n";
        return 2;
    }
    try {
        const Input in = read(argv[1]);
        std::ofstream out(argv[2], std::ios::binary);
        if (!out) throw std::runtime_error(std::string("cannot write ") + argv[2]);
        std::vector<std::uint8_t> colour, normals, depthImage;
        std::vector<double> zbuf;
        for (const Job& job : in.jobs) {
            rasterize(in, job, colour, normals, zbuf);
            encodeDepth(zbuf, colour, job, depthImage);
            for (const auto* image : {&colour, &normals, &depthImage})
                out.write(reinterpret_cast<const char*>(image->data()), static_cast<std::streamsize>(image->size()));
        }
        if (!out) throw std::runtime_error("write failed");
    } catch (const std::exception& e) {
        std::cerr << "impostor_bake: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

