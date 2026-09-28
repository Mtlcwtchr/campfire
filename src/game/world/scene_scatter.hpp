#pragma once
#include "engine/render/level_of_detail.hpp"
#include "game/world/ecology.hpp"
#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace world::decor {
// Manifest order is an explicit content contract, not a random filesystem order.
inline constexpr std::array<const char*,8> kModels{
    "CommonTree_1", "Pine_1", "Bush_Common", "Rock_Medium_1", "Mushroom_Common",
    "Deadwood_Log", "Deadwood_Stump", "Deadwood_Branch"};
inline constexpr int kCell = 8, kRegion = 128, kRadius = 768;
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
};
struct Object {
    std::uint64_t id=0;
    double x=0,y=0,z=0;
    float scale=1,yaw=0,phase=0,tint=1;
    std::uint32_t model=0;
    bool operator==(const Object&) const = default;
};
struct Scatter {
    std::vector<Object> objects;
    std::uint64_t revision=0;
    std::size_t waterTilesSkipped=0,sampled=0;
    std::array<std::size_t,kModels.size()> populations{};
};
// Metre-scale forest masses, clearings and local groves, independent of camera.
double forestDensity(std::uint64_t seed,double x,double y);
using LandTest = std::function<bool(int,int)>; // conservative 512 m page admission
using SiteSample = std::function<Site(double,double)>;
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
                const LandTest& land,const SiteSample& sample);
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

