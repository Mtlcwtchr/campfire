#pragma once
// CPU-only terrain planning. No Camera, SDL objects or mutable atlas references
// cross the worker boundary. Destroy before the borrowed world/PageStore.
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/world/terrain_cut.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"
#include "game/world/terrain_adaptive.hpp"
#include "game/world/terrain_config.hpp"
#include "game/world/terrain_detail.hpp"
#include "game/world/terrain_progress.hpp"
#include "game/world/terrain_view.hpp"
#include "game/world/terrain_streaming/base_tile.hpp"
#include "game/generation/terrain_foundation.hpp"

namespace world::streaming { class PageStore; }
namespace world::terrain {

struct TerrainView {
    std::array<float, 16> matrix{}, prediction{};
    TerrainConfig config;
    double x = 0, y = 0, lookX = 0, lookY = 0, radius = 0;
    // How far from (x, y) the ground is kept at all, in metres: roots wholly
    // past it ask for no pages and draw nothing (it is past the fog). Nought:
    // the whole world, as it always was.
    double window = 0;
    // Where the window is centred: what the camera looks at, which from high
    // above is far from where the eye is (x, y).
    double windowX = 0, windowY = 0;
    // A box wholly past the window.
    bool beyondWindow(double minX, double minY, double maxX, double maxY) const {
        if (!(window > 0)) return false;
        const double dx = std::max({minX - windowX, 0.0, windowX - maxX}),
                     dy = std::max({minY - windowY, 0.0, windowY - maxY});
        return dx * dx + dy * dy > window * window;
    }
    int width = 1, height = 1, target = 0;
    bool localDetail = false; // perspective camera close to the H16 ground
    generation::TerrainStage stageFrom = generation::TerrainStage::Final;
    generation::TerrainStage stageTo = generation::TerrainStage::Final;
    bool localTouches(const ViewBounds& box) const {
        if (!localDetail || !perspective()) return false;
        const double ox=std::floor(x/16)*16-100,oy=std::floor(y/16)*16-100;
        return box.minX<ox+200 && box.maxX>ox && box.minY<oy+200 && box.maxY>oy;
    }
    bool perspective() const { return matrix[12] != 0 || matrix[13] != 0 || matrix[14] != 0; }
    double metresPerPixel(const ViewBounds& box) const {
        const double lo[3]{box.minX, box.minY, box.low}, hi[3]{box.maxX, box.maxY, box.high};
        double nearest = matrix[15], focalX = 0, focalY = 0;
        for (int i = 0; i < 3; ++i) {
            nearest += std::min(matrix[12 + i] * lo[i], matrix[12 + i] * hi[i]);
            focalX += double(matrix[i]) * matrix[i];
            focalY += double(matrix[4 + i]) * matrix[4 + i];
        }
        const double focal = std::max(std::sqrt(focalX) * width, std::sqrt(focalY) * height) * 0.5;
        return std::max(0.5, nearest) / std::max(0.00001, focal);
    }
    double lodMetresPerPixel(const ViewBounds& box) const {
        return metresPerPixel(box) / config.detailDistanceScale;
    }
    bool refine(const ViewBounds& box, int lod, bool children = false) const {
        if (!perspective()) return lod > target;
        if (lod <= 0) return false;
        const double mpp = lodMetresPerPixel(box);
        return children ? !edgeWantsCoarsening(4 << (lod - 1), mpp, config.lod) :
            edgeWantsRefining(4 << lod, mpp, config.lod);
    }
    int targetFor(const ViewBounds& box) const {
        if (!perspective()) return target;
        // From the coarsest level there is, not from the sixth: the pyramid
        // goes to four kilometres a step now, and a map view that starts its
        // search at two hundred and fifty-six metres can never ask for less
        // than that however far away the ground is.
        int lod = int(kGeometryLevels) - 1;
        while (lod > 0 && refine(box, lod)) --lod;
        return lod;
    }
    bool refineSurface(const ViewBounds& box, int lod, const AdaptiveMesh* mesh,
                       DataLodPolicy policy, bool children = false) const {
        if (policy.cameraBudget && lod<=1) return false; // H8 is the finest runtime geometry
        if (policy.targeted) {
            if (lod<=0) return false;
            if (localTouches(box)) return true;
            if (lod<=1) return false; // ordinary nearby terrain stops at H8
        }
        // Screen size AND the ground's own error, which is the whole argument.
        //
        // This used to be screen size alone - the comment here said so, and
        // called curvature-driven subdivision "hidden". The result is a uniform
        // lattice: every tile in view gets the same step whatever is in it, so
        // an ocean floor and a cordillera are drawn at the same density, and
        // pulled back far enough the whole world arrives as triangles smaller
        // than a pixel. That is not a budget being honoured, it is a budget
        // being spent on nothing.
        //
        // What a triangle is FOR is describing a shape. Where there is no shape
        // to describe - a plain, a sea bed, the flat floor of a basin - the
        // error is nought and no size on screen is a reason to split. Where
        // there is, the error says so and splits it however far away it is.
        //
        // Until a mesh exists there is no error to consult, so the first cut is
        // still taken on size; the moment one is built its own bound takes over.
        if (policy.cameraBudget) {
            if (lod <= 0) return false;
            if (!mesh) return refine(box, lod, children);
            const double mpp = lodMetresPerPixel(box);
            return children ? !shouldCoarsen(4 << (lod - 1), mpp, mesh->detailError, config.lod)
                            : shouldRefine(4 << lod, mpp, mesh->detailError, config.lod);
        }
        if (!mesh) return refine(box, lod, children);
        if (lod <= 0) return false;
        const double mpp = lodMetresPerPixel(box);
        // A flat H64 is not proof that H16/H8/H4 contain no cliff. The source
        // quality floor is screen-driven; adaptive triangles are error-driven.
        // Stop splitting for data once both levels select the same dataset.
        const int goal = perspective() ? geometryLevelFor(mpp, children ? lod-1 : lod, config.lod) : target;
        return dataLevelForGeometryLevel(lod, policy) > dataLevelForGeometryLevel(goal, policy) ||
               mesh->detailError > mpp * (children ? 1.0 : 2.0);
    }
    bool operator==(const TerrainView&) const = default;
};

struct TerrainResidency {
    std::uint64_t revision = 0;
    std::unordered_set<streaming::TileKey> pages;
    std::array<std::size_t, 3> capacity{256, 256, std::numeric_limits<std::size_t>::max()};
    // Proven from published GPU fields, never from a sparse CPU land mask.
    std::unordered_set<streaming::TileKey> dryPages;
    std::unordered_map<streaming::TileKey, std::shared_ptr<const SurfacePage>> surfaces;

    bool mayHaveWater(TileId tile, std::uint8_t data, std::uint8_t parentData, int cells = kTileCells,
                      std::int64_t extent = 0) const {
        const auto side = extent ? extent : tileMetresAt(tile.lod,cells);
        const auto halo = 2 * sampleMetresAt(std::min(6, tile.lod + 1));
        int previous = -1;
        for (auto level : {data, parentData}) {
            if (level == previous) continue;
            previous = level;
            // Keep both morph endpoints, parent triangle corners and filtering
            // neighbours. An absent ocean entry returns cover=1 in the shader:
            // it must NOT be treated as an empty/dry dependency.
            const std::int64_t page = streaming::pageMetresAtLevel(level);
            for (auto y = floorDiv(tile.y * side - halo, page); y <= floorDiv((tile.y + 1) * side + halo, page); ++y)
                for (auto x = floorDiv(tile.x * side - halo, page); x <= floorDiv((tile.x + 1) * side + halo, page); ++x) {
                    const streaming::TileKey key{int(x), int(y), level};
                    if (!pages.contains(key) || !dryPages.contains(key)) return true;
                }
        }
        return false;
    }
};

struct TerrainPlan {
    using Key = streaming::TileKey;
    struct Block {
        TileId tile;
        float parentMorph = 0, skirtLow = -61;
        ViewBounds bounds;
        // Page key levels (0/1/2/4), NOT mesh levels or atlas indices.
        // Both fields stay pinned through forward and reverse geometry morphs.
        std::uint8_t dataLevel = 4, parentDataLevel = 4;
        bool mayHaveWater = true;
        std::shared_ptr<const AdaptiveMesh> mesh;
        int chunkCells = kTileCells;
        std::int64_t extentMetres = 0;
        std::int64_t metres() const { return extentMetres ? extentMetres : tileMetresAt(tile.lod,chunkCells); }
    };
    TerrainView view;
    std::uint64_t sequence = 0, residencyRevision = 0;
    double time = 0, buildMs = 0, latencyMs = 0;
    std::vector<Block> drawing, coverage;
    // Worker-built endpoints; animation starts at GPU publication, not job time.
    bool interpolated = false;
    void interpolateFrom(const std::vector<Block>& previous);
    // Constrain fine edge vertices to the rendered neighbour's edge segments,
    // including corners and independent morph phases. Never mutate source meshes.
    void stitchEdges(const std::vector<Block>& previous = {});
    // Cull the retained cut against today's camera while its replacement is
    // pending. This reads cached bounds only: no LOD walk or terrain sampling.
    void selectDrawing(const TerrainView& current, std::vector<Block>& into) const {
        into.clear();
        for (const auto& block : coverage)
            if (intersectsView(block.bounds, current.matrix,
                4.0 / std::max(1, current.width), 4.0 / std::max(1, current.height))) into.push_back(block);
    }
    // pins includes complete active families/parents, not just visible blocks.
    // The render owner validates ALL pins before accepting this snapshot.
    std::vector<Key> pins, required, missing, preload;
    std::unordered_set<Key> demanded, wanted; // visible dependencies vs. union with prediction
    PreparationProgress viewPages, viewMesh;
    std::size_t coarse = 0, morphing = 0, blank = 0;
    std::size_t rootsVisited = 0, rootsReused = 0;
    std::size_t meshesBuilt = 0, deferredRegions = 0;
    // Squares drawn with a mesh of ground that has since been dug (a stale
    // mesh is drawn until its replacement is built, never a hole instead).
    std::size_t staleMeshes = 0;
    bool needsUpdate = false, capacityLimited = false;
    TerrainDetail::Stats detail;

    bool dirty(const TerrainView& current, std::uint64_t residency) const {
        return view != current || residencyRevision != residency || needsUpdate;
    }
    bool compatible(const TerrainResidency& current) const {
        return std::all_of(pins.begin(), pins.end(), [&](Key key) { return current.pages.contains(key); });
    }
};

class TerrainPlanner {
public:
    TerrainPlanner(const generation::WorldMapData& world, streaming::PageStore& pages,
                   std::vector<TileId> roots, DataLodPolicy dataPolicy = {});
    ~TerrainPlanner();
    TerrainPlanner(const TerrainPlanner&) = delete;
    TerrainPlanner& operator=(const TerrainPlanner&) = delete;

    // Observe the latest immutable inputs BEFORE collect(), even while busy.
    // One running job (two independently claimable stages) and one replaceable
    // pending request: never a camera-history queue. Unchanged settled inputs
    // do no work. True means a new request was recorded, not necessarily started.
    bool request(TerrainView view, std::shared_ptr<const TerrainResidency> residency,
                 double time, bool restart = false);
    // Only the newest request may publish. Destruction drains workers and drops
    // all results, so a new world's planner cannot inherit an old world's plan.
    std::shared_ptr<const TerrainPlan> collect();
    // Includes an uncollected result. A continuously moving consumer may drain
    // one snapshot before issuing its newest view, avoiding publication starvation.
    bool busy() const;
    struct Stats {
        std::size_t submitted = 0, completed = 0, failed = 0, discarded = 0;
        double lastBuildMs = 0, lastLatencyMs = 0;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace world::terrain
