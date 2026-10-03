#pragma once
#include "engine/render/level_of_detail.hpp"
#include "engine/biomes/registry.hpp"
#include "game/world/ecology.hpp"
#include "game/world/environment_models.gen.hpp"
#include <array>
#include <algorithm>
#include <compare>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace world::decor {
// Manifest order is an explicit content contract, not a random filesystem order.
inline constexpr std::array<const char*,36> kModels{
    "CommonTree_1", "Pine_1", "Bush_Common", "Rock_Medium_1", "Mushroom_Common",
    "Deadwood_Log", "Deadwood_Stump", "Deadwood_Branch",
    "Cliff_Face_Grey", "Cliff_Face_Warm", "Moss_Clump", "Fern_Clump", "Shrub_Dense", "Shrub_Low",
    // The second environment extension (content/config/environment_models.json):
    // trees, shrubs, rocks and small props from the prepared source models.
    "Hornbeam_Forest_A", "Hornbeam_Forest_B", "Trident_Maple", "Island_Tree_A", "Island_Tree_B",
    "Small_Tree", "Jacaranda_Tree", "Pine_Tree",
    "Shrub_Leafy", "Shrub_Round", "Rooibos_Bush",
    "Boulder_Round", "Rock_Angular", "Forest_Rock_Small",
    "Dead_Trunk", "Stump_Old", "Red_Mushrooms",
    // Leaf-recoloured trees (tools/prepare_environment_models.py recolour): the
    // biomes whose crowns are not green.
    "Amber_Oak", "Amber_Hornbeam", "Blossom_Maple", "Blossom_Birch", "Dark_Oak"};
inline constexpr std::uint32_t kCliffGrey=8, kCliffWarm=9, kMoss=10, kFern=11,
                               kDenseShrub=12, kLowShrub=13;
inline constexpr std::uint32_t kFirstTree2=14, kLastTree2=21, kFirstRock2=25, kLastRock2=27,
                               kFirstTree3=31, kLastTree3=35;
// The beech and the spruce of the base catalogue, and the added trees.
inline constexpr bool treeModel(std::uint32_t model) {
    return model<2 || (model>=kFirstTree2 && model<=kLastTree2) || (model>=kFirstTree3 && model<=kLastTree3);
}
inline constexpr bool coniferModel(std::uint32_t model) { return model==1 || model==21; }
inline constexpr bool rockModel(std::uint32_t model) {
    return model==3 || model==kCliffGrey || model==kCliffWarm || (model>=kFirstRock2 && model<=kLastRock2);
}
inline constexpr bool groundCoverModel(std::uint32_t model) { return model==kMoss || model==kFern || model==kLowShrub; }
// How deep a plant or prop of the scatter stands in the ground, metres.
inline constexpr double plantSink(std::uint32_t model) {
    return model==3 || (model>=kFirstRock2 && model<=kLastRock2)?0.25:0.08;
}
// Imported plants retain metre units; an 8 cm root offset would bury moss.
inline double groundSink(std::uint32_t model,double scale) {
    if (model==kMoss) return 0.002;
    if (model>=kFern && model<=kLowShrub)
        return std::min(0.06,kEnvironmentBounds[model-kCliffGrey].height*scale*0.06);
    if (model==kCliffGrey || model==kCliffWarm)
        return kEnvironmentBounds[model-kCliffGrey].height*scale*0.16;
    return plantSink(model);
}
inline constexpr int kCell = 8, kRegion = 128, kRadius = 768;
inline constexpr std::size_t kBaseModelCount=8;
inline constexpr std::size_t kMaxScatterCells = 65536;
inline constexpr std::size_t kMeshTriangleBudget = 1500000;
inline constexpr double kImpostorFloorPixels = 0.8;
struct Site {
    double height=0, water=0, slope=0, forest=0, boreal=0;
    ecology::Cell ecology{};
    bool hasEcology=false;
    // The ground the renderer actually draws here (material weights, 0..1).
    // Trees do not stand on dune sand or bare rock whatever the climate says.
    double sand=0, rock=0, marsh=0, snow=0;
    bool hasMaterials=false;
    // The terrain category's biomes here (engine/biomes), resolved through
    // the category where a layer says 0, and the forest_bias channel their
    // density follows. Null: the engine's own scatter, exactly as before.
    const engine::biomes::ForestBiome* forestBiome=nullptr;
    const engine::biomes::DecorBiome* decorBiome=nullptr;
    const engine::biomes::Registry* registry=nullptr;
    double forestBias=0.5;
    // A density brush's multiplier here (engine/biomes DetailEdits); 1: none.
    double detailDensity=1.0;
};
struct Object {
    std::uint64_t id=0;
    double x=0,y=0,z=0;
    float scale=1,yaw=0,phase=0,tint=1;
    std::uint32_t model=0;
    float moss=0; // stable habitat strength on solid rock, passed to both mesh and depth impostor
    bool operator==(const Object&) const = default;
};
struct Scatter {
    std::vector<Object> objects;
    std::uint64_t revision=0;
    // The edit layer's revision when the scatter began to read the ground
    // (edit_layer.hpp): objects stand on the ground as it was then.
    std::uint64_t ground=0;
    std::size_t waterTilesSkipped=0,sampled=0;
    std::array<std::size_t,kModels.size()> populations{};
};
// Metre-scale forest masses, clearings and local groves, independent of camera.
double forestDensity(std::uint64_t seed,double x,double y);
using LandTest = std::function<bool(int,int)>; // conservative 512 m page admission
using SiteSample = std::function<Site(double,double)>;
// Whether a point is under water. Optional: asked only of candidates that are
// about to become trees, bushes or logs, around their footprint.
using WetTest = std::function<bool(double,double)>;
// How far a wooded object keeps from the waterline, in metres. The rendered
// shore is interpolated from 4-16 m page texels, so the carve's exact wet
// line alone let trunks stand in the drawn water.
inline constexpr double kShoreClearance = 7.0;
struct ScatterBounds {
    std::int64_t minX=0,minY=0,maxX=0,maxY=0; // half-open world-metre bounds
    auto operator<=>(const ScatterBounds&) const = default;
    [[nodiscard]] std::size_t cells() const;
};
struct ScatterBoundsHash {
    std::size_t operator()(const ScatterBounds& r) const noexcept {
        std::uint64_t h=0x9e3779b97f4a7c15ULL;
        for (const auto v:{r.minX,r.minY,r.maxX,r.maxY}) {
            auto x=std::uint64_t(v)+h;
            x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
            x=(x^(x>>27))*0x94d049bb133111ebULL;
            h=x^(x>>31);
        }
        return std::size_t(h);
    }
};
Scatter scatter(std::uint64_t seed,ScatterBounds bounds,double width,double height,
                const LandTest& land,const SiteSample& sample,const WetTest& wet={},bool environment=false);
Scatter scatter(std::uint64_t seed,int regionX,int regionY,double width,double height,
                const LandTest& land,const SiteSample& sample,int radiusMetres=kRadius);
struct Lod { float mesh=0,coverage=0; };
// How small a thing may be and still be worth a mesh, when the budget has room.
//
// The crossover used to have 22 as a FLOOR as well as a default, so a frame
// spending a sixth of its triangle budget still drew thousands of impostors it
// could have afforded meshes for - measured: 245 009 of 1 500 000, and 2 299
// cards. A budget that is never spent is not a budget, it is a constant.
//
// Twelve rather than lower because below that a mesh stops being cheaper than
// what it replaces: the coarsest level of a tree is about two hundred
// triangles, and at six pixels that is more triangles than pixels, which is the
// exact cost the retopology work removed.
inline constexpr double kMeshFloorPixels = 12;
// Pixels, not metres or terrain LOD. Both representations share one stable object.
Lod objectLod(double pixels,double distance,double meshStartPixels=22);
// The rule and its allowance belong to the engine: the GPU cluster culler
// states the same one in HLSL, and a second copy here would drift.
using engine::render::kLevelPixelError;
inline std::size_t meshLevel(std::span<const float> errors,double pixels,double extent,
                             double allowance=kLevelPixelError) {
    return engine::render::levelFor(errors,pixels,extent,allowance);
}
struct MeshDemand { double pixels=0;std::size_t triangles=0; };
double meshPixelThreshold(std::span<const MeshDemand> demands,std::size_t budget=kMeshTriangleBudget);
int impostorView(double cameraRightAngle,double objectYaw);
// Which two of the eight views the object sits between, and how far.
//
// The nearest view alone is a snap: turn the camera and every impostor in the
// frame jumps an eighth of a turn at the same moment. `blend` is 0 at `view`
// and approaches 1 at `next`, and the shader chooses between them per pixel by
// the same dither it already uses for the mesh/card crossover - one sample, and
// no ghosting of two different silhouettes over each other.
struct ImpostorPair { int view=0,next=0;float blend=0; };
ImpostorPair impostorPair(double cameraRightAngle,double objectYaw);
} // namespace world::decor
