#include "game/world/ring_mesh.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/height_field.hpp"
#include "game/world/terrain_adaptive.hpp"
#include "../../../assets/shaders/foliage_field.hlsli"

#include <algorithm>
#include <cmath>
#include <limits>

namespace world {
namespace {
using core::Fixed;
} // namespace

std::pair<double, double> ringHeightBounds(const generation::WorldMapData& world,
                                          core::WorldPos min, core::WorldPos max, int lod) {
    if (lod < 0 || lod > 12) throw std::invalid_argument("invalid bounds LOD");
    if (world.width <= 0 || world.height <= 0) return {-256, 512};
    // H64 is not the macro-cell spline. Its saved stages accept -3000..6000 m;
    // include channel/rounding margin without scanning the foundation per ring.
    if (world.terrainFoundation) return {-3200, 6200};
    const std::int64_t cell = generation::kMetresPerCell;
    // Three cells cover the 0.45-cell warp, bicubic neighbours and the
    // drainage neighbourhood (including downstream endpoints). Polar vertices
    // additionally interpolate lattice samples up to one LOD step away.
    const std::int64_t halo = 3 * cell + 2 * sampleMetresAt(lod);
    const auto fromX = std::clamp<std::int64_t>(floorDiv(min.x.toInt() - halo, cell), 0, world.width - 1);
    const auto toX = std::clamp<std::int64_t>(floorDiv(max.x.toInt() + halo, cell), 0, world.width - 1);
    const auto fromY = std::clamp<std::int64_t>(floorDiv(min.y.toInt() - halo, cell), 0, world.height - 1);
    const auto toY = std::clamp<std::int64_t>(floorDiv(max.y.toInt() + halo, cell), 0, world.height - 1);
    // Match HeightField's spline inputs: a filled coarse cell stores the brim,
    // while the rendered ground has the basin fill removed. Water retains its
    // separately recorded pre-erosion head and is not bounded by that spline.
    const auto bedOf = [&](std::size_t index) {
        const auto& c = world.cells[index];
        const double depth = index < world.lakeDepthField.size() ? world.lakeDepthField[index] : 0;
        return c.sea ? -8.0 : (double(c.elevation) - depth) * generation::kMetresPerElevationStep;
    };
    double waterHigh = -std::numeric_limits<double>::infinity();
    double low = std::numeric_limits<double>::max(), high = -low;
    for (auto y = fromY; y <= toY; ++y)
        for (auto x = fromX; x <= toX; ++x) {
            const auto index = static_cast<std::size_t>(y) * world.width + x;
            const auto& c = world.cells[index];
            const double h = bedOf(index);
            low = std::min(low, h);
            high = std::max(high, c.sea ? 0.0 : h);
            if (!c.sea && index < world.lakeDepthField.size() && world.lakeDepthField[index] > 0) {
                const double head = index < world.lakeLevelField.size()
                        ? world.lakeLevelField[index] : c.elevation;
                waterHigh = std::max(waterHigh, head * generation::kMetresPerElevationStep);
            }
        }
    high = std::max(high, waterHigh);
    if (min.x.toInt() - halo < 0 || min.y.toInt() - halo < 0 ||
        max.x.toInt() + halo > std::int64_t(world.width) * cell ||
        max.y.toInt() + halo > std::int64_t(world.height) * cell) {
        low = std::min(low, -60.0); // the offshore shelf
        high = std::max(high, 0.0);
    }
    // Bicubic Catmull-Rom overshoot <= 0.28125 of the input range.
    // Detail octaves total < 92 m, sea-share overshoot scales this by < 1.23;
    // channels deepen it by < 12 m. 160 m includes fixed-point rounding.
    const double padding = (high - low) * 0.3 + 160.0 + kAlpineHeightMarginMetres;
    std::pair<double, double> bounds{low - padding, high + padding};
    // For small regions, bound each bicubic patch by its Bezier control
    // hull. A low cell elsewhere in the neighbourhood must not lower every
    // mountain patch by 0.3 of the entire neighbourhood's elevation range.
    const double spacing = sampleMetresAt(lod);
    const double reach = cell * 0.45 + 2 * spacing;
    const auto bx0 = static_cast<std::int64_t>(std::floor((min.x.toDouble() - reach) / cell));
    const auto bx1 = static_cast<std::int64_t>(std::floor((max.x.toDouble() + reach) / cell));
    const auto by0 = static_cast<std::int64_t>(std::floor((min.y.toDouble() - reach) / cell));
    const auto by1 = static_cast<std::int64_t>(std::floor((max.y.toDouble() + reach) / cell));
    if ((bx1 - bx0 + 1) * (by1 - by0 + 1) > 256) return bounds;
    const auto elevation = [&](std::int64_t x, std::int64_t y) {
        const auto index = static_cast<std::size_t>(std::clamp<std::int64_t>(y, 0, world.height - 1)) *
                           world.width + std::clamp<std::int64_t>(x, 0, world.width - 1);
        return bedOf(index);
    };
    const auto bezier = [](const double* p, double* b) {
        b[0] = p[1]; b[1] = p[1] + (p[2] - p[0]) / 6;
        b[2] = p[2] - (p[3] - p[1]) / 6; b[3] = p[2];
    };
    double patchLow = std::numeric_limits<double>::max(), patchHigh = -patchLow;
    for (auto y = by0; y <= by1; ++y)
        for (auto x = bx0; x <= bx1; ++x) {
            double rows[4][4];
            for (int j = 0; j < 4; ++j) {
                double p[4];
                for (int i = 0; i < 4; ++i) p[i] = elevation(x + i - 1, y + j - 1);
                bezier(p, rows[j]);
            }
            for (int i = 0; i < 4; ++i) {
                double p[4], b[4];
                for (int j = 0; j < 4; ++j) p[j] = rows[j][i];
                bezier(p, b);
                for (double v : b) { patchLow = std::min(patchLow, v); patchHigh = std::max(patchHigh, v); }
            }
        }
    // A river can lower the surface to an endpoint outside the bicubic patch.
    // Include only actual wet-channel endpoints; dry gullies cut < 12 m.
    if (detailFor(sampleMetresAt(lod)).narrowest < cell / 2)
        for (auto y = fromY; y <= toY; ++y)
            for (auto x = fromX; x <= toX; ++x) {
                const auto& c = world.at({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)});
                if (c.sea || (!c.river && c.drainSize == 0)) continue;
                patchLow = std::min(patchLow, elevation(x, y));
                const int dir = c.river && c.riverOut >= 0 ? c.riverOut : c.drainOut;
                if (dir >= 0 && dir < core::kNeighbourCount) {
                    const auto down = core::neighbour({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)}, dir);
                    patchLow = std::min(patchLow, std::max(0.0, elevation(down.x, down.y)));
                }
            }
    if (min.x.toDouble() - reach < 700 || min.y.toDouble() - reach < 700 ||
        max.x.toDouble() + reach > world.width * double(cell) - 700 ||
        max.y.toDouble() + reach > world.height * double(cell) - 700) {
        patchLow = std::min(patchLow, -60.0);
        patchHigh = std::max(patchHigh, 0.0);
    }
    bounds.first = std::max(bounds.first, patchLow - 160 - kAlpineHeightMarginMetres);
    bounds.second = std::min(bounds.second, std::max(patchHigh, waterHigh) + 160 + kAlpineHeightMarginMetres);
    return bounds;
}

std::vector<PageGrassRoot> buildPageGrass(const terrain::AdaptiveMesh& mesh,
        double originX, double originY, int windowX, int windowY, int cellMetres, int halfWindowMetres) {
    if (!std::isfinite(originX+originY) || !(mesh.step>0) ||
        mesh.surfaceIndices>mesh.indices.size() || mesh.surfaceIndices%3 || cellMetres<2 ||
        (cellMetres&(cellMetres-1)) || halfWindowMetres<0)
        throw std::invalid_argument("invalid grass surface");
    const bool whole=halfWindowMetres==0;
    const double extent=mesh.cells*mesh.step;
    const double left=whole?originX:double(windowX)*kGrassRegion-halfWindowMetres;
    const double bottom=whole?originY:double(windowY)*kGrassRegion-halfWindowMetres;
    const int side=int(std::ceil((whole?extent:2.0*halfWindowMetres)/cellMetres));
    if (side>512) throw std::invalid_argument("grass/proxy grid exceeds bounded block budget");
    const auto firstX=std::int64_t(std::floor(left/cellMetres)),firstY=std::int64_t(std::floor(bottom/cellMetres));
    std::vector<bool> occupied(side*side,false);
    std::vector<PageGrassRoot> out;
    for (std::size_t t=0;t<mesh.surfaceIndices;t+=3) {
        const auto& a=mesh.vertices.at(mesh.indices[t]);
        const auto& b=mesh.vertices.at(mesh.indices[t+1]);
        const auto& c=mesh.vertices.at(mesh.indices[t+2]);
        const double ax=originX+a.x*mesh.step,ay=originY+a.y*mesh.step;
        const double bx=originX+b.x*mesh.step,by=originY+b.y*mesh.step;
        const double cx=originX+c.x*mesh.step,cy=originY+c.y*mesh.step;
        const double det=(by-cy)*(ax-cx)+(cx-bx)*(ay-cy);
        if (std::abs(det)<1e-9 || ((a.skirt|b.skirt|c.skirt)&1)) continue;
        const auto source=[](const auto& v){return v.skirt&2?v.edgeBed:v.sourceBed;};
        const auto parent=[](const auto& v){return v.skirt&2?v.edgeBed:v.parentBed;};
        const auto prior=[](const auto& v){return v.skirt&2?v.priorEdge:v.priorBed;};
        const auto priorParent=[](const auto& v){return v.skirt&2?v.priorEdge:v.priorParent;};
        const double dx=((source(a)-source(c))*(by-cy)-(source(b)-source(c))*(ay-cy))/det;
        const double dy=((ax-cx)*(source(b)-source(c))-(bx-cx)*(source(a)-source(c)))/det;
        const float upright=float(1/std::sqrt(1+dx*dx+dy*dy));
        if (upright<=0.55f) continue;
        const int x0=int(std::clamp(std::floor((std::min({ax,bx,cx})-left)/cellMetres),0.0,double(side)));
        const int x1=int(std::clamp(std::ceil((std::max({ax,bx,cx})-left)/cellMetres),0.0,double(side)));
        const int y0=int(std::clamp(std::floor((std::min({ay,by,cy})-bottom)/cellMetres),0.0,double(side)));
        const int y1=int(std::clamp(std::ceil((std::max({ay,by,cy})-bottom)/cellMetres),0.0,double(side)));
        for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) {
            if (occupied[y*side+x]) continue;
            const auto gx=std::uint32_t(firstX+x),gy=std::uint32_t(firstY+y);
            const double px=(double(firstX+x)+0.15+0.70*foliage::foliageHash(gx,gy))*cellMetres;
            const double py=(double(firstY+y)+0.15+0.70*foliage::foliageHash(gx^0x7351u,gy))*cellMetres;
            const double wa=((by-cy)*(px-cx)+(cx-bx)*(py-cy))/det;
            const double wb=((cy-ay)*(px-cx)+(ax-cx)*(py-cy))/det,wc=1-wa-wb;
            if (std::min({wa,wb,wc}) < -1e-9) continue;
            const auto blend=[&](auto value){return float(wa*value(a)+wb*value(b)+wc*value(c));};
            occupied[y*side+x]=true;
            out.push_back({{float(px),float(py),blend(source)},blend(parent),blend(prior),
                blend(priorParent),upright});
        }
    }
    return out;
}

std::vector<RingFoliage> buildRingFoliage(const TerrainMesh& mesh,
                                        const std::vector<Fixed>& waterLevel,
                                        const std::function<bool()>& cancelled) {
    std::vector<RingFoliage> out;
    // Beyond LOD 2 the same meadow field is integrated by TerrainPS, rather
    // than drawing subpixel blades or enlarging them into tree-sized cards.
    if (mesh.lod < 0 || mesh.lod > 2 || mesh.vertices.empty()) return out;
    if (waterLevel.size() != mesh.vertices.size()) throw std::invalid_argument("missing ring water data");
    const auto scatter = [](std::int64_t x, std::int64_t y, int sample) {
        std::uint64_t v = static_cast<std::uint64_t>(x) * 0x9e3779b97f4a7c15ULL;
        v ^= static_cast<std::uint64_t>(y) * 0xc2b2ae3d27d4eb4fULL;
        v ^= static_cast<std::uint64_t>(sample + 1) * 0x94d049bb133111ebULL;
        v ^= v >> 30;
        v *= 0xbf58476d1ce4e5b9ULL;
        v ^= v >> 27;
        return static_cast<std::uint32_t>(v ^ (v >> 31));
    };
    const double step = sampleMetresAt(mesh.lod);
    const int samples = mesh.lod == 0 ? 10 : mesh.lod == 1 ? 4 : 1;
    const float clumpScale = mesh.lod == 0 ? 1.0f : mesh.lod == 1 ? 1.8f : 3.0f;
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        if (t % 192 == 0 && cancelled && cancelled()) return {};
        const auto ia = mesh.indices[t], ib = mesh.indices[t + 1], ic = mesh.indices[t + 2];
        const auto& a = mesh.vertices[ia];
        const auto& b = mesh.vertices[ib];
        const auto& c = mesh.vertices[ic];
        const double ax = a.position.x.toDouble(), ay = a.position.y.toDouble();
        const double bx = b.position.x.toDouble(), by = b.position.y.toDouble();
        const double cx = c.position.x.toDouble(), cy = c.position.y.toDouble();
        const double det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
        if (std::abs(det) < 1e-9) continue;
        for (auto y = static_cast<std::int64_t>(std::floor(std::min({ay, by, cy}) / step));
             y <= static_cast<std::int64_t>(std::floor(std::max({ay, by, cy}) / step)); ++y)
            for (auto x = static_cast<std::int64_t>(std::floor(std::min({ax, bx, cx}) / step));
                 x <= static_cast<std::int64_t>(std::floor(std::max({ax, bx, cx}) / step)); ++x)
                for (int sample = 0; sample < samples; ++sample) {
                    // Pick a nested representative base cell, not a new random
                    // layout at each LOD. LOD 2 candidates also exist at 1 and 0.
                    auto baseX = x, baseY = y;
                    for (int level = mesh.lod; level > 0; --level) {
                        const auto child = scatter(baseX, baseY, 100 + level);
                        baseX = baseX * 2 + (child & 1u);
                        baseY = baseY * 2 + ((child >> 1u) & 1u);
                    }
                    const auto random = scatter(baseX, baseY, sample);
                    const double px = (double(baseX) + 0.06 + (random & 255u) / 255.0 * 0.88) * kSampleMetres;
                    const double py = (double(baseY) + 0.06 + ((random >> 8) & 255u) / 255.0 * 0.88) * kSampleMetres;
                    const double wa = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / det;
                    const double wb = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / det;
                    const double wc = 1 - wa - wb;
                    if (wa <= 0 || wb <= 0 || wc <= 0) continue;
                    const auto mix = [&](double av, double bv, double cv) { return wa * av + wb * bv + wc * cv; };
                    const auto material = [&](Material m) {
                        return mix(a.materials.of(m).toDouble(), b.materials.of(m).toDouble(), c.materials.of(m).toDouble());
                    };
                    const double grass = material(Material::Grass), rock = material(Material::Rock);
                    const double shore = material(Material::Sand) + material(Material::Marsh);
                    const double upright = mix(a.normal.z.toDouble(), b.normal.z.toDouble(), c.normal.z.toDouble());
                    const double height = mix(a.height.toDouble(), b.height.toDouble(), c.height.toDouble());
                    const double water = mix(waterLevel[ia].toDouble(), waterLevel[ib].toDouble(), waterLevel[ic].toDouble());
                    // Moisture and concavity, so the cards near the camera and
                    // the far meadow agree about where the damp ground is.
                    const double moisture = mix(a.environment[2].toDouble(),
                                                b.environment[2].toDouble(),
                                                c.environment[2].toDouble());
                    const double openness = mix(a.openness.toDouble(), b.openness.toDouble(),
                                                c.openness.toDouble());
                    const float hollow = foliage::saturate(
                            static_cast<float>(-openness / (2.0 * kSampleMetres) * 16.0));
                    const float suitability = foliage::foliageSuitability(
                            static_cast<float>(grass), static_cast<float>(rock), static_cast<float>(shore),
                            static_cast<float>(upright), static_cast<float>(water - height),
                            static_cast<float>(moisture), hollow);
                    if (water > height + 0.02 || suitability <= 0) continue;
                    const float field = foliage::foliageField(static_cast<float>(px), static_cast<float>(py), 0);
                    float biome[4];
                    for (std::size_t i = 0; i < 4; ++i)
                        biome[i] = static_cast<float>(mix(a.foliage[i].toDouble(),
                                                         b.foliage[i].toDouble(), c.foliage[i].toDouble()));
                    const auto community = foliage::foliageCommunity(biome[0], biome[1], biome[2], biome[3], field);
                    if (community.density <= 0 || ((random >> 16) & 255u) / 255.0 >
                        foliage::foliageDensity(suitability * community.density, field)) continue;
                    RingFoliage blade{};
                    blade.position[0] = static_cast<float>(px);
                    blade.position[1] = static_cast<float>(py);
                    blade.position[2] = static_cast<float>(height);
                    blade.climate[3] = static_cast<float>(mix(a.coarseHeight.toDouble(),
                        b.coarseHeight.toDouble(), c.coarseHeight.toDouble()));
                    blade.scale = (0.75f + static_cast<float>((random >> 24) & 255u) / 255.0f * 0.55f) *
                                  clumpScale * community.height;
                    blade.tint[0] = community.red;
                    blade.tint[1] = community.green;
                    blade.tint[2] = community.blue;
                    blade.tint[3] = 0.92f;
                    blade.phase = static_cast<float>((random & 0xffffu) / 65535.0 * 6.283185307);
                    blade.variant = static_cast<float>(foliage::foliageCommunityVariant(
                            biome[0], biome[1], biome[2], biome[3], random));
                    for (int i=0;i<3;++i) {
                        const int channel=i==0?0:i==1?2:5;
                        blade.climate[i]=static_cast<float>(mix(a.environment[channel].toDouble(),
                            b.environment[channel].toDouble(),c.environment[channel].toDouble()));
                    }
                    out.push_back(blade);
                }
    }
    return out;
}

} // namespace world
