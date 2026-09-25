#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include "game/world/terrain_adaptive.hpp"
#include "game/world/tile_mesh.hpp"

namespace generation { struct WorldMapData; }
namespace world::streaming { class PageStore; }
namespace world::terrain {

// Fixed source budget, not a whole 512 m H4 page. Endpoints are included.
struct LocalHeightWindow {
    static constexpr int step = 4, cells = 50, side = cells+1, metres = cells*step;
    int x = 0, y = 0;
    std::array<std::array<float,2>,side*side> heights{};
    bool contains(double wx,double wy) const;
    std::array<float,2> sample(double wx,double wy) const;
};

// CPU-only worker product. H16 supplies feature metadata; explicit detailed
// samples never enter the H4 page atlas and never request a page(key.level=0).
class TerrainDetail {
public:
    TerrainDetail(const generation::WorldMapData&,const streaming::PageStore&, int chunkCells = kTileCells,
                  std::array<int,kGeometryLevels> chunkMetres = {});
    ~TerrainDetail();
    bool updateLocal(bool enabled,double x,double y,double seconds,double morphSeconds = 0.25);
    bool localTouches(TileId) const;
    bool hasFeatures(TileId) const;
    double tolerance(double x,double y) const;
    std::array<float,2> sample(double x,double y,std::array<float,2> base,bool local);
    bool transitioning() const;
    struct Stats {
        std::size_t localSamples=0, localEvaluated=0, featureSamples=0, featureEvaluated=0;
        double localAmount=0;
    };
    Stats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace world::terrain

