// model_simplify - one source model (glTF 2.0 with an external .bin, the way
// Poly Haven publishes them) reduced to a triangle budget per mesh.
//
//     model_simplify --in fern_02_2k.gltf --out out/fern_02.gltf --tris 1500
//                    [--foliage NAME]... [--solid NAME]... [--solid-share 0.3]
//                    [--max-growth 3] [--res 256] [--report out/report.json]
//                    [--preview DIR]   (per mesh: 3 views, yellow kept, red lost, blue added)
//
// Every mesh in the file is a variant (Poly Haven puts several plants of one
// species side by side, one node each) and each gets the whole budget. Nodes,
// materials, textures and images are copied as they are; image URIs stay
// relative, so the caller puts the textures beside the output
// (tools/simplify_models.py does, and turns the leaf materials into alpha-cut
// ones). Only POSITION, NORMAL and TEXCOORD_0 are kept.
//
// Two kinds of geometry, two reductions - the split tools/mesh_lod.py makes:
//
// - Solid parts (trunk, branches, rock). One quadric edge-collapse pass over
//   all solid primitives together, so the budget goes where the error is
//   rather than in proportion to the source, normals and UVs in the metric,
//   small disconnected twigs pruned as the error grows. Permissive collapses
//   across UV seams, then sloppy clustering, only when topology stops the
//   collapse short of the budget.
// - Scattered pieces (leaves, twig cards, grass blades, fronds). A leaf here is
//   a bent card of up to two dozen triangles and thirty thousand of them make a
//   crown; a whole-mesh collapse cannot shrink a leaf without tearing it. Each
//   piece is reduced on its own, the error relative to the piece, so a leaf
//   stays a leaf; the error is raised in steps only as far as the budget needs.
//   If even two-triangle pieces are too many, pieces are thinned - spatially
//   stratified, so a crown thins evenly instead of going bald on one side - and
//   the survivors grow about their own centres until the pieces cover as much
//   of three orthographic views as the original did.
//
// Reported per mesh: triangles in and out, how each part was reduced, pieces
// kept, growth, and the silhouette IoU of the result against the original in
// three axis views (geometry only - the alpha cut is the same on both sides).
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// ---------------------------------------------------------------- input

struct Mapped {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    explicit Mapped(const fs::path& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open " + path.string());
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw std::runtime_error("cannot stat " + path.string());
        }
        size = static_cast<std::size_t>(st.st_size);
        void* p = size ? ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
        ::close(fd);
        if (p == MAP_FAILED) throw std::runtime_error("cannot map " + path.string());
        data = static_cast<const std::uint8_t*>(p);
    }
    ~Mapped() {
        if (data) ::munmap(const_cast<std::uint8_t*>(data), size);
    }
    Mapped(const Mapped&) = delete;
    Mapped& operator=(const Mapped&) = delete;
};

struct Source {
    json gltf;
    std::vector<std::unique_ptr<Mapped>> buffers;
};

Source load(const fs::path& path) {
    Source s;
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    s.gltf = json::parse(in);
    for (const json& b : s.gltf.at("buffers")) {
        const std::string uri = b.at("uri").get<std::string>();
        if (uri.rfind("data:", 0) == 0) throw std::runtime_error("embedded buffers are not supported");
        s.buffers.push_back(std::make_unique<Mapped>(path.parent_path() / uri));
    }
    return s;
}

const std::uint8_t* accessorBase(const Source& s, const json& acc, std::size_t element, std::size_t& stride) {
    if (acc.contains("sparse")) throw std::runtime_error("sparse accessors are not supported");
    const json& view = s.gltf.at("bufferViews").at(acc.at("bufferView").get<std::size_t>());
    const Mapped& buf = *s.buffers.at(view.at("buffer").get<std::size_t>());
    const std::size_t offset = view.value("byteOffset", std::size_t{0}) + acc.value("byteOffset", std::size_t{0});
    stride = view.value("byteStride", std::size_t{0});
    if (stride == 0) stride = element;
    const std::size_t count = acc.at("count").get<std::size_t>();
    if (count && offset + (count - 1) * stride + element > buf.size)
        throw std::runtime_error("accessor runs past its buffer");
    return buf.data + offset;
}

std::vector<float> readFloats(const Source& s, std::size_t index, std::size_t comps) {
    const json& acc = s.gltf.at("accessors").at(index);
    if (acc.at("componentType").get<int>() != 5126) throw std::runtime_error("non-float vertex attribute");
    std::size_t stride = 0;
    const std::uint8_t* base = accessorBase(s, acc, 4 * comps, stride);
    const std::size_t count = acc.at("count").get<std::size_t>();
    std::vector<float> out(count * comps);
    for (std::size_t i = 0; i < count; ++i) std::memcpy(&out[i * comps], base + i * stride, 4 * comps);
    return out;
}

std::vector<std::uint32_t> readIndices(const Source& s, std::size_t index) {
    const json& acc = s.gltf.at("accessors").at(index);
    const int type = acc.at("componentType").get<int>();
    const std::size_t size = type == 5121 ? 1 : type == 5123 ? 2 : type == 5125 ? 4 : 0;
    if (!size) throw std::runtime_error("bad index type");
    std::size_t stride = 0;
    const std::uint8_t* base = accessorBase(s, acc, size, stride);
    const std::size_t count = acc.at("count").get<std::size_t>();
    std::vector<std::uint32_t> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = base + i * stride;
        if (size == 1) out[i] = *p;
        else if (size == 2) { std::uint16_t v; std::memcpy(&v, p, 2); out[i] = v; }
        else std::memcpy(&out[i], p, 4);
    }
    return out;
}

// ---------------------------------------------------------------- geometry

constexpr std::size_t kAttr = 5;   // normal xyz, uv

struct Geometry {
    std::vector<float> pos;            // xyz per vertex
    std::vector<float> attr;           // kAttr per vertex
    std::vector<std::uint32_t> idx;
    std::vector<std::uint16_t> slot;   // output primitive per vertex

    std::size_t vertices() const { return pos.size() / 3; }
    std::size_t triangles() const { return idx.size() / 3; }

    void append(const std::vector<float>& p, const std::vector<float>& n, const std::vector<float>& uv,
                const std::vector<std::uint32_t>& ind, std::uint16_t s) {
        const std::size_t base = vertices(), count = p.size() / 3;
        pos.insert(pos.end(), p.begin(), p.end());
        for (std::size_t i = 0; i < count; ++i) {
            attr.push_back(n.empty() ? 0.f : n[3 * i]);
            attr.push_back(n.empty() ? 1.f : n[3 * i + 1]);
            attr.push_back(n.empty() ? 0.f : n[3 * i + 2]);
            attr.push_back(uv.empty() ? 0.f : uv[2 * i]);
            attr.push_back(uv.empty() ? 0.f : uv[2 * i + 1]);
        }
        slot.insert(slot.end(), count, s);
        for (std::uint32_t i : ind) {
            if (i >= count) throw std::runtime_error("index out of range");
            idx.push_back(static_cast<std::uint32_t>(base + i));
        }
    }
};

std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// ---------------------------------------------------------------- silhouettes

constexpr int kViews = 3;
constexpr int kAxes[kViews][2] = {{2, 1}, {0, 1}, {0, 2}};   // side, front, top (glTF: +Y up)

struct Frame {
    int res = 256;
    float minA[kViews] {}, minB[kViews] {}, pixel[kViews] {};
};

using Masks = std::array<std::vector<std::uint8_t>, kViews>;

Frame frameFor(const float lo[3], const float hi[3], int res) {
    Frame f;
    f.res = res;
    for (int v = 0; v < kViews; ++v) {
        const int a = kAxes[v][0], b = kAxes[v][1];
        const float extent = std::max(hi[a] - lo[a], hi[b] - lo[b]) * 1.04f + 1e-6f;
        f.pixel[v] = extent / static_cast<float>(res);
        f.minA[v] = 0.5f * (lo[a] + hi[a]) - 0.5f * extent;
        f.minB[v] = 0.5f * (lo[b] + hi[b]) - 0.5f * extent;
    }
    return f;
}

Masks emptyMasks(const Frame& f) {
    Masks m;
    for (auto& v : m) v.assign(static_cast<std::size_t>(f.res) * f.res, 0);
    return m;
}

// fetch(t, p) fills p[3][3] with the three corners of triangle t.
template <class Fetch>
void rasterize(Masks& m, const Frame& f, std::size_t triangles, Fetch&& fetch) {
    const int res = f.res;
    float p[3][3];
    for (std::size_t t = 0; t < triangles; ++t) {
        fetch(t, p);
        for (int v = 0; v < kViews; ++v) {
            const int a = kAxes[v][0], b = kAxes[v][1];
            float x[3], y[3];
            for (int k = 0; k < 3; ++k) {
                x[k] = (p[k][a] - f.minA[v]) / f.pixel[v];
                y[k] = (p[k][b] - f.minB[v]) / f.pixel[v];
            }
            const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
            const int x0 = std::max(0, static_cast<int>(std::ceil(std::min({x[0], x[1], x[2]}) - 0.5f)));
            const int x1 = std::min(res - 1, static_cast<int>(std::floor(std::max({x[0], x[1], x[2]}) - 0.5f)));
            const int y0 = std::max(0, static_cast<int>(std::ceil(std::min({y[0], y[1], y[2]}) - 0.5f)));
            const int y1 = std::min(res - 1, static_cast<int>(std::floor(std::max({y[0], y[1], y[2]}) - 0.5f)));
            bool hit = false;
            if (std::fabs(area) > 1e-12f) {
                const float s = area > 0 ? 1.f : -1.f;
                for (int yi = y0; yi <= y1; ++yi) {
                    const float cy = static_cast<float>(yi) + 0.5f;
                    for (int xi = x0; xi <= x1; ++xi) {
                        const float cx = static_cast<float>(xi) + 0.5f;
                        const float w0 = s * ((x[1] - cx) * (y[2] - cy) - (x[2] - cx) * (y[1] - cy));
                        const float w1 = s * ((x[2] - cx) * (y[0] - cy) - (x[0] - cx) * (y[2] - cy));
                        const float w2 = s * ((x[0] - cx) * (y[1] - cy) - (x[1] - cx) * (y[0] - cy));
                        if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                            m[v][static_cast<std::size_t>(yi) * res + xi] = 1;
                            hit = true;
                        }
                    }
                }
            }
            if (!hit) {   // smaller than a pixel: it still darkens the pixel it is in
                const int cx = static_cast<int>((x[0] + x[1] + x[2]) / 3.f);
                const int cy = static_cast<int>((y[0] + y[1] + y[2]) / 3.f);
                if (cx >= 0 && cy >= 0 && cx < res && cy < res) m[v][static_cast<std::size_t>(cy) * res + cx] = 1;
            }
        }
    }
}

void rasterizeGeometry(Masks& m, const Frame& f, const Geometry& g) {
    rasterize(m, f, g.triangles(), [&](std::size_t t, float p[3][3]) {
        for (int k = 0; k < 3; ++k) std::memcpy(p[k], &g.pos[3 * g.idx[3 * t + k]], 12);
    });
}

std::size_t covered(const Masks& m) {
    std::size_t n = 0;
    for (const auto& v : m) n += static_cast<std::size_t>(std::count(v.begin(), v.end(), std::uint8_t{1}));
    return n;
}

std::array<float, kViews> iou(const Masks& a, const Masks& b) {
    std::array<float, kViews> out {};
    for (int v = 0; v < kViews; ++v) {
        std::size_t both = 0, either = 0;
        for (std::size_t i = 0; i < a[v].size(); ++i) {
            both += a[v][i] & b[v][i];
            either += a[v][i] | b[v][i];
        }
        out[v] = either ? static_cast<float>(both) / static_cast<float>(either) : 1.f;
    }
    return out;
}

// Three views side by side, binary PPM: yellow both, red lost, blue added.
void writePreview(const fs::path& path, const Masks& before, const Masks& after, int res) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << res * kViews << " " << res << "\n255\n";
    std::vector<std::uint8_t> row(static_cast<std::size_t>(res) * kViews * 3);
    for (int y = res - 1; y >= 0; --y) {   // +up at the top
        for (int v = 0; v < kViews; ++v)
            for (int x = 0; x < res; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * res + x;
                const bool a = before[v][i], b = after[v][i];
                const std::uint8_t c[3] = {static_cast<std::uint8_t>(a && b ? 225 : a ? 210 : b ? 60 : 22),
                                           static_cast<std::uint8_t>(a && b ? 200 : a ? 40 : b ? 100 : 22),
                                           static_cast<std::uint8_t>(a && b ? 70 : a ? 40 : b ? 230 : 26)};
                std::memcpy(&row[(static_cast<std::size_t>(v) * res + x) * 3], c, 3);
            }
        f.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
}

// ---------------------------------------------------------------- solid parts

std::string simplifySolid(Geometry& g, std::size_t target) {
    if (g.triangles() <= target) return "kept";
    const float weights[kAttr] = {0.5f, 0.5f, 0.5f, 0.05f, 0.05f};
    std::vector<std::uint32_t> out(g.idx.size());
    auto run = [&](unsigned options) {
        return meshopt_simplifyWithAttributes(out.data(), g.idx.data(), g.idx.size(), g.pos.data(), g.vertices(), 12,
                                              g.attr.data(), kAttr * 4, weights, kAttr, nullptr, target * 3, 1.f,
                                              options, nullptr);
    };
    const std::size_t enough = target * 3 * 11 / 10;
    std::string method = "collapse";
    std::size_t n = run(meshopt_SimplifyPrune);
    if (n * 2 < target * 3) {
        // Pruning removes whole components once the error allows it, and with
        // an unbounded error that can be the whole of a one-piece rock.
        n = run(0);
        method = "collapse-noprune";
    }
    if (n > enough) {
        n = run(meshopt_SimplifyPermissive | (method == "collapse" ? meshopt_SimplifyPrune : 0u));
        method = "permissive";
    }
    if (n > enough) {
        std::vector<std::uint32_t> sloppy(n);
        n = meshopt_simplifySloppy(sloppy.data(), out.data(), n, g.pos.data(), g.vertices(), 12, nullptr,
                                   target * 3, 1.f, nullptr);
        std::copy(sloppy.begin(), sloppy.begin() + static_cast<std::ptrdiff_t>(n), out.begin());
        method = "sloppy";
    }
    out.resize(n);
    g.idx = std::move(out);
    return method;
}

// ---------------------------------------------------------------- scattered pieces

struct FoliageReport {
    std::size_t piecesIn = 0, piecesKept = 0;
    float pieceError = 0.f, growth = 1.f, coverage = 1.f;
    std::string pieceMethod = "kept";
};

FoliageReport simplifyFoliage(Geometry& g, std::size_t target, const Frame& frame, float maxGrowth,
                              std::uint64_t seed) {
    FoliageReport r;
    const std::size_t nv = g.vertices(), nt = g.triangles();
    if (!nt) return r;

    // Pieces: connected over welded positions (UV seams split vertices, not leaves).
    std::vector<unsigned> weld(nv);
    meshopt_generatePositionRemap(weld.data(), g.pos.data(), nv, 12);
    std::vector<std::uint32_t> parent(nv);
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](std::uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    for (std::size_t t = 0; t < nt; ++t) {
        const std::uint32_t a = find(weld[g.idx[3 * t]]);
        for (int k = 1; k < 3; ++k) {
            const std::uint32_t b = find(weld[g.idx[3 * t + k]]);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        }
    }
    std::vector<std::int32_t> pieceOf(nv, -1);
    std::vector<std::vector<std::uint32_t>> piece;
    for (std::size_t t = 0; t < nt; ++t) {
        const std::uint32_t root = find(weld[g.idx[3 * t]]);
        if (pieceOf[root] < 0) {
            pieceOf[root] = static_cast<std::int32_t>(piece.size());
            piece.emplace_back();
        }
        auto& tri = piece[static_cast<std::size_t>(pieceOf[root])];
        tri.insert(tri.end(), g.idx.begin() + static_cast<std::ptrdiff_t>(3 * t),
                   g.idx.begin() + static_cast<std::ptrdiff_t>(3 * t + 3));
    }
    std::vector<std::uint32_t>().swap(parent);
    std::vector<std::int32_t>().swap(pieceOf);
    const std::size_t pieces = piece.size();
    r.piecesIn = r.piecesKept = pieces;
    if (nt <= target) return r;

    Masks original = emptyMasks(frame);
    rasterizeGeometry(original, frame, g);

    // Each piece on its own, from the original every time, relative to the
    // piece. Three stages, each only if the one before cannot fit the budget:
    //   collapse    - quadric collapse at the smallest error that fits;
    //   permissive  - the same, allowed across normal/UV seams (a leaf whose
    //                 outline is a seam cannot shrink otherwise);
    //   sloppy      - clustering of each piece to its share of the budget.
    // Every piece survives all three; only thinning, after them, drops pieces.
    std::vector<std::uint32_t> stamp(nv, 0), localOf(nv, 0), lverts, lidx, ldst;
    std::vector<float> lpos, lattr;
    std::uint32_t stampNow = 0;
    const float weights[kAttr] = {0.25f, 0.25f, 0.25f, 1.f, 1.f};
    using Pieces = std::vector<std::vector<std::uint32_t>>;
    // mode 0 collapse, 1 permissive, 2 sloppy (param = share of `quota` sizes)
    auto reduce = [&](int mode, float param, const Pieces* quota, Pieces& outPieces) {
        std::size_t total = 0;
        outPieces.resize(pieces);
        for (std::size_t p = 0; p < pieces; ++p) {
            const auto& tri = piece[p];
            auto& out = outPieces[p];
            std::size_t want = 2;
            if (mode == 2) {
                const std::size_t had = (*quota)[p].size() / 3;
                want = std::max<std::size_t>(2, static_cast<std::size_t>(std::lround(static_cast<float>(had) * param)));
                if (want >= had) {
                    out = (*quota)[p];
                    total += out.size() / 3;
                    continue;
                }
            }
            if (tri.size() <= 6) {
                out = tri;
                total += tri.size() / 3;
                continue;
            }
            ++stampNow;
            lverts.clear();
            lidx.clear();
            for (std::uint32_t v : tri) {
                if (stamp[v] != stampNow) {
                    stamp[v] = stampNow;
                    localOf[v] = static_cast<std::uint32_t>(lverts.size());
                    lverts.push_back(v);
                }
                lidx.push_back(localOf[v]);
            }
            lpos.resize(lverts.size() * 3);
            lattr.resize(lverts.size() * kAttr);
            for (std::size_t i = 0; i < lverts.size(); ++i) {
                std::memcpy(&lpos[3 * i], &g.pos[3 * lverts[i]], 12);
                std::memcpy(&lattr[kAttr * i], &g.attr[kAttr * lverts[i]], kAttr * 4);
            }
            ldst.resize(lidx.size());
            std::size_t n;
            if (mode == 2)
                n = meshopt_simplifySloppy(ldst.data(), lidx.data(), lidx.size(), lpos.data(), lverts.size(), 12,
                                           nullptr, want * 3, 1.f, nullptr);
            else
                n = meshopt_simplifyWithAttributes(ldst.data(), lidx.data(), lidx.size(), lpos.data(), lverts.size(),
                                                   12, lattr.data(), kAttr * 4, weights, kAttr, nullptr, 6, param,
                                                   mode == 1 ? meshopt_SimplifyPermissive : 0u, nullptr);
            if (n) {
                out.resize(n);
                for (std::size_t i = 0; i < n; ++i) out[i] = lverts[ldst[i]];
            } else {
                out = mode == 2 ? (*quota)[p] : tri;
            }
            total += out.size() / 3;
        }
        return total;
    };
    constexpr float kMaxPieceError = 0.45f;
    Pieces best, trial;
    int mode = 0;
    std::size_t total = reduce(0, kMaxPieceError, nullptr, best);
    if (total > target) {
        mode = 1;
        total = reduce(1, kMaxPieceError, nullptr, best);
    }
    r.pieceError = kMaxPieceError;
    r.pieceMethod = mode ? "permissive" : "collapse";
    if (total < target) {
        float lo = std::log(0.002f), hi = std::log(kMaxPieceError);
        for (int i = 0; i < 9; ++i) {
            const float mid = 0.5f * (lo + hi);
            const std::size_t n = reduce(mode, std::exp(mid), nullptr, trial);
            if (n <= target) {
                hi = mid;
                best.swap(trial);
                total = n;
                r.pieceError = std::exp(mid);
            } else {
                lo = mid;
            }
        }
    } else if (total > target) {
        const Pieces quota = best;
        float share = static_cast<float>(target) / static_cast<float>(total);
        std::size_t bestN = total;
        for (int i = 0; i < 6; ++i) {
            const std::size_t n = reduce(2, share, &quota, trial);
            const bool fits = n <= target, bestFits = bestN <= target;
            if ((fits && (!bestFits || n > bestN)) || (!fits && !bestFits && n < bestN)) {
                best.swap(trial);
                bestN = n;
                r.pieceMethod = "sloppy";
            }
            if (n == 0) break;
            share *= 0.98f * static_cast<float>(target) / static_cast<float>(n);
        }
        total = bestN;
    }
    piece.swap(best);
    Pieces().swap(best);
    Pieces().swap(trial);

    // Centre and area of every piece as reduced: thinning stratifies by the
    // centre, and growth happens about it.
    std::vector<std::uint8_t> keep(pieces, 1);
    std::vector<std::array<float, 3>> centre(pieces);
    std::vector<double> area(pieces, 0.0);
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (std::size_t p = 0; p < pieces; ++p) {
        double c[3] = {0, 0, 0}, sum = 0;
        const auto& tri = piece[p];
        for (std::size_t t = 0; t < tri.size(); t += 3) {
            const float* a = &g.pos[3 * tri[t]];
            const float* b = &g.pos[3 * tri[t + 1]];
            const float* d = &g.pos[3 * tri[t + 2]];
            const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            const double w[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
            const double cx = u[1] * w[2] - u[2] * w[1], cy = u[2] * w[0] - u[0] * w[2], cz = u[0] * w[1] - u[1] * w[0];
            const double ar = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz) + 1e-12;
            for (int k = 0; k < 3; ++k) c[k] += ar * (a[k] + b[k] + d[k]) / 3.0;
            sum += ar;
        }
        for (int k = 0; k < 3; ++k) {
            centre[p][k] = static_cast<float>(c[k] / sum);
            lo[k] = std::min(lo[k], centre[p][k]);
            hi[k] = std::max(hi[k], centre[p][k]);
        }
        area[p] = sum;
    }

    if (total > target) {
        // Thin: stratified over a grid sized to the number of survivors, so
        // every part of the crown loses the same share.
        const double perPiece = static_cast<double>(total) / static_cast<double>(pieces);
        const double survivors = std::max(1.0, static_cast<double>(target) / perPiece);
        double volume = 1;
        for (int k = 0; k < 3; ++k) volume *= std::max(1e-3, static_cast<double>(hi[k] - lo[k]));
        const float cell = static_cast<float>(std::cbrt(volume / survivors));
        std::vector<std::uint32_t> order(pieces), rank(pieces);
        std::iota(order.begin(), order.end(), 0u);
        std::vector<std::uint64_t> key(pieces);
        for (std::size_t p = 0; p < pieces; ++p) key[p] = mix(seed ^ (p * 0x100000001b3ull));
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return key[a] < key[b]; });
        std::unordered_map<std::uint64_t, std::uint32_t> inCell;
        for (std::uint32_t p : order) {
            std::uint64_t h = 0;
            for (int k = 0; k < 3; ++k)
                h = h * 0x9E3779B1ull + static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(std::floor((centre[p][k] - lo[k]) / cell)) + (1ll << 20));
            rank[p] = inCell[h]++;
        }
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
            return rank[a] != rank[b] ? rank[a] < rank[b] : key[a] < key[b];
        });
        std::fill(keep.begin(), keep.end(), std::uint8_t{0});
        std::size_t kept = 0, keptPieces = 0;
        for (std::uint32_t p : order) {
            const std::size_t n = piece[p].size() / 3;
            if (kept + n > target) continue;
            keep[p] = 1;
            kept += n;
            ++keptPieces;
        }
        r.piecesKept = keptPieces;
        total = kept;
    }

    // Grow the survivors about their centres until they cover what the
    // original covered in three views. Also without thinning: a reduced leaf
    // is the polygon inscribed in its outline, smaller than the leaf.
    std::vector<std::uint32_t> keptTri, keptPiece;
    for (std::size_t p = 0; p < pieces; ++p) {
        if (!keep[p]) continue;
        keptTri.insert(keptTri.end(), piece[p].begin(), piece[p].end());
        keptPiece.insert(keptPiece.end(), piece[p].size() / 3, static_cast<std::uint32_t>(p));
    }
    const double want = static_cast<double>(covered(original));
    auto coverageAt = [&](float s) {
        Masks m = emptyMasks(frame);
        rasterize(m, frame, keptPiece.size(), [&](std::size_t t, float out[3][3]) {
            const auto& c = centre[keptPiece[t]];
            for (int k = 0; k < 3; ++k) {
                const float* q = &g.pos[3 * keptTri[3 * t + k]];
                for (int j = 0; j < 3; ++j) out[k][j] = c[j] + s * (q[j] - c[j]);
            }
        });
        return static_cast<double>(covered(m));
    };
    float lo_s = 1.f, hi_s = std::max(1.f, maxGrowth);
    float growth = 1.f;
    if (coverageAt(1.f) < want) {
        if (coverageAt(hi_s) <= want) {
            growth = hi_s;
        } else {
            for (int i = 0; i < 12; ++i) {
                const float mid = 0.5f * (lo_s + hi_s);
                (coverageAt(mid) < want ? lo_s : hi_s) = mid;
            }
            growth = 0.5f * (lo_s + hi_s);
        }
    }
    r.growth = growth;
    r.coverage = static_cast<float>(coverageAt(growth) / std::max(want, 1.0));
    if (growth != 1.f) {
        ++stampNow;
        for (std::size_t p = 0; p < pieces; ++p) {
            if (!keep[p]) continue;
            for (std::uint32_t v : piece[p]) {
                if (stamp[v] == stampNow) continue;
                stamp[v] = stampNow;
                for (int j = 0; j < 3; ++j) g.pos[3 * v + j] = centre[p][j] + growth * (g.pos[3 * v + j] - centre[p][j]);
            }
        }
    }

    g.idx.clear();
    for (std::size_t p = 0; p < pieces; ++p)
        if (keep[p]) g.idx.insert(g.idx.end(), piece[p].begin(), piece[p].end());
    return r;
}

// ---------------------------------------------------------------- output

struct Writer {
    std::vector<std::uint8_t> bin;
    json views = json::array();
    json accessors = json::array();

    std::size_t view(const void* data, std::size_t bytes, int target) {
        while (bin.size() % 4) bin.push_back(0);
        const std::size_t offset = bin.size();
        bin.insert(bin.end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + bytes);
        views.push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", bytes}, {"target", target}});
        return views.size() - 1;
    }
    std::size_t accessor(std::size_t v, int component, std::size_t count, const char* type,
                         const json& lo = nullptr, const json& hi = nullptr) {
        json a = {{"bufferView", v}, {"componentType", component}, {"count", count}, {"type", type}};
        if (!lo.is_null()) {
            a["min"] = lo;
            a["max"] = hi;
        }
        accessors.push_back(a);
        return accessors.size() - 1;
    }
};

std::size_t emit(Writer& w, const Geometry& g, std::uint16_t slot, int material, json& primitives) {
    std::vector<std::uint32_t> tri;
    for (std::size_t t = 0; t < g.triangles(); ++t)
        if (g.slot[g.idx[3 * t]] == slot)
            tri.insert(tri.end(), g.idx.begin() + static_cast<std::ptrdiff_t>(3 * t),
                       g.idx.begin() + static_cast<std::ptrdiff_t>(3 * t + 3));
    if (tri.empty()) return 0;
    std::vector<std::uint32_t> remap(g.vertices(), std::numeric_limits<std::uint32_t>::max()), verts;
    for (std::uint32_t& i : tri) {
        if (remap[i] == std::numeric_limits<std::uint32_t>::max()) {
            remap[i] = static_cast<std::uint32_t>(verts.size());
            verts.push_back(i);
        }
        i = remap[i];
    }
    std::vector<std::uint32_t> ordered(tri.size());
    meshopt_optimizeVertexCache(ordered.data(), tri.data(), tri.size(), verts.size());
    std::vector<float> pos(verts.size() * 3), nrm(verts.size() * 3), uv(verts.size() * 2);
    std::array<float, 3> lo {INFINITY, INFINITY, INFINITY}, hi {-INFINITY, -INFINITY, -INFINITY};
    for (std::size_t i = 0; i < verts.size(); ++i) {
        const float* p = &g.pos[3 * verts[i]];
        const float* a = &g.attr[kAttr * verts[i]];
        float len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (len < 1e-12f) len = 1.f;
        for (int k = 0; k < 3; ++k) {
            pos[3 * i + k] = p[k];
            nrm[3 * i + k] = a[k] / len;
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
        uv[2 * i] = a[3];
        uv[2 * i + 1] = a[4];
    }
    json prim;
    prim["attributes"]["POSITION"] =
        w.accessor(w.view(pos.data(), pos.size() * 4, 34962), 5126, verts.size(), "VEC3", lo, hi);
    prim["attributes"]["NORMAL"] = w.accessor(w.view(nrm.data(), nrm.size() * 4, 34962), 5126, verts.size(), "VEC3");
    prim["attributes"]["TEXCOORD_0"] = w.accessor(w.view(uv.data(), uv.size() * 4, 34962), 5126, verts.size(), "VEC2");
    prim["indices"] = w.accessor(w.view(ordered.data(), ordered.size() * 4, 34963), 5125, ordered.size(), "SCALAR");
    prim["mode"] = 4;
    if (material >= 0) prim["material"] = material;
    primitives.push_back(prim);
    return ordered.size() / 3;
}

// ---------------------------------------------------------------- driver

struct Options {
    fs::path in, out, report, preview;
    std::size_t tris = 0;
    std::vector<std::string> foliage, solid;
    float solidShare = 0.3f, maxGrowth = 3.f;
    int res = 256;
    std::uint64_t seed = 1;
};

bool contains(const std::string& text, const std::vector<std::string>& words) {
    return std::any_of(words.begin(), words.end(), [&](const std::string& w) { return text.find(w) != std::string::npos; });
}

bool isFoliage(const json& material, const Options& o) {
    const std::string name = material.value("name", std::string {});
    if (contains(name, o.solid)) return false;
    if (!o.foliage.empty()) return contains(name, o.foliage);
    if (material.value("alphaMode", std::string {"OPAQUE"}) != "OPAQUE") return true;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    return contains(lower, {"leaf", "leaves", "twig", "needle", "grass", "blade", "frond", "petal", "flower", "seed"});
}

int run(const Options& o) {
    const auto started = std::chrono::steady_clock::now();
    Source src = load(o.in);
    const json& gltf = src.gltf;
    const json materials = gltf.value("materials", json::array());

    Writer w;
    json meshesOut = json::array(), report = json::array();
    std::size_t totalIn = 0, totalOut = 0;
    for (std::size_t mi = 0; mi < gltf.at("meshes").size(); ++mi) {
        const json& mesh = gltf["meshes"][mi];
        Geometry solid, foliage;
        std::vector<int> slotMaterial;
        std::vector<bool> slotFoliage;
        for (const json& prim : mesh.at("primitives")) {
            if (prim.value("mode", 4) != 4) {
                std::cerr << "  skipping a non-triangle primitive\n";
                continue;
            }
            const int material = prim.value("material", -1);
            const bool leafy = material >= 0 && isFoliage(materials.at(static_cast<std::size_t>(material)), o);
            const json& at = prim.at("attributes");
            const auto p = readFloats(src, at.at("POSITION").get<std::size_t>(), 3);
            const auto n = at.contains("NORMAL") ? readFloats(src, at["NORMAL"].get<std::size_t>(), 3) : std::vector<float> {};
            const auto uv = at.contains("TEXCOORD_0") ? readFloats(src, at["TEXCOORD_0"].get<std::size_t>(), 2)
                                                      : std::vector<float> {};
            std::vector<std::uint32_t> ind;
            if (prim.contains("indices")) ind = readIndices(src, prim["indices"].get<std::size_t>());
            else {
                ind.resize(p.size() / 3);
                std::iota(ind.begin(), ind.end(), 0u);
            }
            const auto s = static_cast<std::uint16_t>(slotMaterial.size());
            slotMaterial.push_back(material);
            slotFoliage.push_back(leafy);
            (leafy ? foliage : solid).append(p, n, uv, ind, s);
        }

        float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (const Geometry* g : {&solid, &foliage})
            for (std::size_t i = 0; i < g->vertices(); ++i)
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], g->pos[3 * i + k]);
                    hi[k] = std::max(hi[k], g->pos[3 * i + k]);
                }
        const Frame frame = frameFor(lo, hi, o.res);
        Masks before = emptyMasks(frame);
        rasterizeGeometry(before, frame, solid);
        rasterizeGeometry(before, frame, foliage);

        const std::size_t S = solid.triangles(), F = foliage.triangles();
        std::string solidMethod = "kept";
        FoliageReport fr;
        fr.piecesIn = fr.piecesKept = 0;
        if (S + F > o.tris) {
            const std::size_t solidTarget =
                F == 0 ? o.tris : S == 0 ? 0 : std::min(S, static_cast<std::size_t>(o.tris * o.solidShare));
            if (S) solidMethod = simplifySolid(solid, std::max<std::size_t>(solidTarget, 1));
            const std::size_t left = o.tris > solid.triangles() ? o.tris - solid.triangles() : 0;
            if (F) fr = simplifyFoliage(foliage, std::max<std::size_t>(left, 1), frame, o.maxGrowth, o.seed + mi);
        }

        Masks after = emptyMasks(frame);
        rasterizeGeometry(after, frame, solid);
        rasterizeGeometry(after, frame, foliage);
        const auto overlap = iou(before, after);
        if (!o.preview.empty()) {
            fs::create_directories(o.preview);
            writePreview(o.preview / ("mesh_" + std::to_string(mi) + ".ppm"), before, after, o.res);
        }

        json primitives = json::array();
        std::size_t out = 0;
        for (std::size_t s = 0; s < slotMaterial.size(); ++s)
            out += emit(w, slotFoliage[s] ? foliage : solid, static_cast<std::uint16_t>(s), slotMaterial[s], primitives);
        json meshOut = {{"primitives", primitives}};
        if (mesh.contains("name")) meshOut["name"] = mesh["name"];
        meshesOut.push_back(meshOut);

        totalIn += S + F;
        totalOut += out;
        report.push_back({{"mesh", mesh.value("name", std::to_string(mi))}, {"triangles_in", S + F},
                          {"triangles_out", out}, {"solid_in", S}, {"solid_out", solid.triangles()},
                          {"solid_method", solidMethod}, {"foliage_in", F}, {"foliage_out", foliage.triangles()},
                          {"pieces_in", fr.piecesIn}, {"pieces_kept", fr.piecesKept},
                          {"piece_error", fr.pieceError}, {"piece_method", fr.pieceMethod}, {"growth", fr.growth}, {"foliage_coverage", fr.coverage},
                          {"silhouette_iou", {overlap[0], overlap[1], overlap[2]}}});
        std::printf("  %-34s %9zu -> %6zu tris  solid %s %zu->%zu  pieces %zu->%zu %s e=%.2f grow=%.2f  IoU %.2f/%.2f/%.2f\n",
                    mesh.value("name", std::to_string(mi)).substr(0, 34).c_str(), S + F, out, solidMethod.c_str(), S,
                    solid.triangles(), fr.piecesIn, fr.piecesKept, fr.pieceMethod.c_str(), static_cast<double>(fr.pieceError),
                    static_cast<double>(fr.growth), static_cast<double>(overlap[0]), static_cast<double>(overlap[1]),
                    static_cast<double>(overlap[2]));
        std::fflush(stdout);
    }

    // Everything that is not geometry is carried over untouched.
    json out;
    out["asset"] = {{"version", "2.0"}, {"generator", "asr model_simplify (meshoptimizer)"}};
    for (const char* key : {"materials", "textures", "images", "samplers", "nodes", "scenes", "scene",
                            "extensionsUsed", "extensionsRequired"})
        if (gltf.contains(key)) out[key] = gltf[key];
    if (out.contains("nodes"))
        for (json& node : out["nodes"]) {
            node.erase("skin");
            node.erase("weights");
        }
    out["meshes"] = meshesOut;
    out["accessors"] = w.accessors;
    out["bufferViews"] = w.views;
    const fs::path bin = o.out.parent_path() / (o.out.stem().string() + ".bin");
    out["buffers"] = json::array({{{"uri", bin.filename().string()}, {"byteLength", w.bin.size()}}});

    fs::create_directories(o.out.parent_path());
    {
        std::ofstream b(bin, std::ios::binary);
        b.write(reinterpret_cast<const char*>(w.bin.data()), static_cast<std::streamsize>(w.bin.size()));
        if (!b) throw std::runtime_error("cannot write " + bin.string());
    }
    {
        std::ofstream j(o.out);
        j << out.dump(1);
        if (!j) throw std::runtime_error("cannot write " + o.out.string());
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!o.report.empty()) {
        std::ofstream r(o.report);
        r << json({{"source", o.in.string()}, {"budget_per_mesh", o.tris}, {"triangles_in", totalIn},
                   {"triangles_out", totalOut}, {"seconds", seconds}, {"meshes", report}})
                 .dump(1);
    }
    std::printf("  total %zu -> %zu tris, %.1f s\n", totalIn, totalOut, seconds);
    return 0;
}

}   // namespace

int main(int argc, char** argv) {
    Options o;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "--in") o.in = next();
            else if (a == "--out") o.out = next();
            else if (a == "--report") o.report = next();
            else if (a == "--preview") o.preview = next();
            else if (a == "--tris") o.tris = std::stoul(next());
            else if (a == "--foliage") o.foliage.push_back(next());
            else if (a == "--solid") o.solid.push_back(next());
            else if (a == "--solid-share") o.solidShare = std::stof(next());
            else if (a == "--max-growth") o.maxGrowth = std::stof(next());
            else if (a == "--res") o.res = std::stoi(next());
            else if (a == "--seed") o.seed = std::stoull(next());
            else throw std::runtime_error("unknown argument " + a);
        }
        if (o.in.empty() || o.out.empty() || o.tris == 0)
            throw std::runtime_error("usage: model_simplify --in IN.gltf --out OUT.gltf --tris N [options]");
        return run(o);
    } catch (const std::exception& e) {
        std::cerr << "model_simplify: " << e.what() << "\n";
        return 1;
    }
}

