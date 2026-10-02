#include "framework.hpp"
#include "game/generation/world_scale_policy.hpp"
#include "game/generation/world_map_gen.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>

namespace {
using namespace generation;
template<class F> bool rejects(F&& f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}
}
TEST(world_scale_physical_domain_rectangular_uv_gradients_and_large_area) {
    const WorldDomain d(10000000,250000);
    CHECK_EQ(d.areaSquareMetres(),2500000000000ULL);
    const auto uv=d.normalized(2500000,125000);
    CHECK_EQ(uv[0],0.25);CHECK_EQ(uv[1],0.5);
    const auto xy=d.metres(uv[0],uv[1]);
    CHECK_EQ(xy[0],2500000.0);CHECK_EQ(xy[1],125000.0);
    const auto gradient=d.gradientPerMetre(100000,5000);
    CHECK_EQ(gradient[0],0.01);CHECK_EQ(gradient[1],0.02);
    CHECK(d.contains(0,0));CHECK(!d.contains(10000000,0));CHECK(!d.contains(0,250000));
    CHECK(!d.contains(-1,0));CHECK(!d.contains(std::numeric_limits<double>::quiet_NaN(),0));
    CHECK_EQ(WorldDomain(10000000,10000000).areaSquareMetres(),100000000000000ULL);
    CHECK(rejects([]{WorldDomain(0,100);}));CHECK(rejects([]{WorldDomain(-1,100);}));
    CHECK(rejects([]{WorldDomain(WorldDomain::kMaxExtent+1,100);}));
}
TEST(world_scale_legacy_dimensions_roundtrip_without_rescaling_params) {
    for (const auto& size:kWorldSizes) {
        WorldMapParams p;p.width=size.cells;p.height=size.cells/2;p.seed=42;
        const auto d=WorldDomain::fromLegacy(p);
        CHECK_EQ(d.widthMetres(),std::int64_t(size.cells)*kMetresPerCell);
        CHECK_EQ(d.heightMetres(),std::int64_t(size.cells)*kMetresPerCell/2);
        const auto cells=d.legacyCells();CHECK(cells.has_value());
        CHECK_EQ((*cells)[0],p.width);CHECK_EQ((*cells)[1],p.height);
        CHECK_EQ(p.width,size.cells);CHECK_EQ(p.seed,42u);
    }
    CHECK(!WorldDomain(100000,100000).legacyCells());
    WorldMapParams invalid;invalid.width=-1;
    CHECK(rejects([&]{WorldDomain::fromLegacy(invalid);}));
}
TEST(world_scale_runtime_presets_are_the_physical_sizes_they_describe) {
    // These were two lists, and the gap between them was the point: the client
    // ran at a quarter of the sides the descriptors named, because the H64
    // foundation could not hold the real ones and a preset that throws is worse
    // than a preset that is small.
    //
    // It can hold them now, so there is one list. What this test is for is that
    // it STAYS one - a size added to the runtime and not to the descriptors is
    // a world whose physical extent nothing agrees on.
    CHECK_EQ(kScaleWorldSizes.size(),kWorldSizeCount);
    for (std::size_t i=0;i<kWorldSizeCount;++i) {
        WorldMapParams legacy;legacy.width=legacy.height=kWorldSizes[i].cells;
        const auto runtime=WorldDomain::fromLegacy(legacy);
        const auto described=scaleWorldPreset(kWorldSizes[i].name);
        CHECK_EQ(runtime.widthMetres(),described.widthMetres());
        CHECK_EQ(runtime.areaSquareMetres(),described.areaSquareMetres());
    }
    CHECK_EQ(scaleWorldPreset().widthMetres(),524288);   // "medium", 4 x 4 regions
    CHECK_EQ(scaleWorldPreset("med"),scaleWorldPreset("medium"));
    CHECK(rejects([]{scaleWorldPreset("invalid");}));
    // The player's default is one of the presets. What a bare WorldMapParams
    // gets is deliberately NOT: a test or a tool that does not ask for a world
    // must not be handed a two-hundred-kilometre one with a ten-million-cell
    // foundation under it, which is what happened when these were one number.
    CHECK_EQ(kDefaultPlayerWorldCells,kWorldSizes[2].cells);   // "small", 128 km
    // Said as a distance, because the smallest preset is small now: what must
    // not happen is a bare params raising a world somebody has to wait for.
    CHECK(std::int64_t(kDefaultWorldCells)*kMetresPerCell < 100000);
}
TEST(world_scale_absolute_world_relative_and_bounded_semi_relative_rules) {
    const ScaleRule absolute{ScaleClass::Absolute,8};
    const ScaleRule relative{ScaleClass::WorldRelative,0.25};
    const ScaleRule regional{ScaleClass::SemiRelative,30000,0.35,16000,120000};
    double previous=0;
    for (const auto side:{100000,250000,1000000,10000000}) {
        const WorldDomain d(side,side/2);
        CHECK_EQ(absolute.metres(d)[0],8.0);CHECK_EQ(absolute.metres(d)[1],8.0);
        CHECK_EQ(relative.metres(d)[0],side*0.25);CHECK_EQ(relative.metres(d)[1],side*0.125);
        const double length=regional.metres(d)[0];
        CHECK(length>=16000 && length<=120000);CHECK(length>=previous);previous=length;
        CHECK_EQ(regional.metres(d),regional.metres(WorldDomain(side/2,side)));
    }
    CHECK(rejects([]{ScaleRule{ScaleClass::SemiRelative,30000,2}.metres(WorldDomain(100,100));}));
}
TEST(world_scale_auto_step_obeys_budget_and_overview_stays_bounded) {
    const std::array<std::uint32_t,5> expected{256,256,512,1024,2048};
    const std::array<int,5> sides{100000,250000,500000,1000000,10000000};
    WorldScalePolicy policy;
    for (std::size_t i=0;i<sides.size();++i) {
        const auto d=policy.describe(WorldDomain(sides[i],sides[i]),42);
        CHECK_EQ(d.coarseStepMetres,expected[i]);
        CHECK(d.overviewSamples[0]<=1024 && d.overviewSamples[1]<=1024);
        if (i<4) CHECK(d.coarseSampleCount()<=policy.coarseSampleBudget);
        else CHECK(d.coarseSampleCount()>policy.coarseSampleBudget); // must be paged, not allocated
    }
    const auto strip=policy.describe(WorldDomain(10000000,100000),1);
    CHECK_EQ(strip.overviewSamples[0],1024u);CHECK_EQ(strip.overviewSamples[1],12u);
    const auto tiny=policy.describe(WorldDomain(1,1),1);
    CHECK_EQ(tiny.overviewSamples[0],2u);CHECK_EQ(tiny.overviewSamples[1],2u);
    policy.coarseSampleBudget=65536;
    CHECK_EQ(policy.describe(WorldDomain(100000,100000),42).coarseStepMetres,512u);
    policy.coarseStepMetres=128;
    CHECK(rejects([&]{policy.describe(WorldDomain(100000,100000),42);}));
}
TEST(world_scale_descriptor_versions_identity_and_all_coarse_steps_roundtrip) {
    for (const auto step:WorldScalePolicy::kCoarseSteps) {
        WorldScalePolicy policy;policy.coarseStepMetres=step;
        const auto d=policy.describe(WorldDomain(10000000,250000),UINT64_MAX);
        const auto restored=WorldDescriptor::parse(d.serialize());
        CHECK_EQ(restored,d);CHECK_EQ(restored.fingerprint(),d.fingerprint());
        CHECK_EQ(restored.coarseStepMetres,step);
        auto changed=d;--changed.seed;CHECK(changed.fingerprint()!=d.fingerprint());
        changed=d;changed.domain=WorldDomain(10000000,250001);CHECK(changed.fingerprint()!=d.fingerprint());
        changed=d;--changed.overviewSamples[0];CHECK(changed.fingerprint()!=d.fingerprint());
    }
    const auto d=WorldScalePolicy{}.describe(WorldDomain(100000,100000),42);
    const auto valid=nlohmann::json::parse(d.serialize());
    for (const auto key:{"format_version","generator_version","scale_policy_version"}) {
        auto broken=valid;broken[key]=99;
        CHECK(rejects([&]{WorldDescriptor::parse(broken.dump());}));
    }
    for (const auto key:{"width_m","height_m","seed","coarse_step_m","overview_columns","overview_rows"}) {
        auto broken=valid;broken[key]=-1;
        CHECK(rejects([&]{WorldDescriptor::parse(broken.dump());}));
        broken[key]=0.5;CHECK(rejects([&]{WorldDescriptor::parse(broken.dump());}));
    }
    auto broken=valid;broken["generator"]="legacy";
    CHECK(rejects([&]{WorldDescriptor::parse(broken.dump());}));
}

