#include "framework.hpp"
#include "game/world/scene_scatter.hpp"
#include "game/world/ring_mesh.hpp"
#include "game/world/terrain_adaptive.hpp"
#include "game/world/climate_field.hpp"
#include "game/generation/world_map_gen.hpp"
#include "../assets/shaders/foliage_field.hlsli"
#include "../assets/shaders/scene_model_motion.hlsli"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace {
using namespace world::decor;
const LandTest land=[](int,int){return true;};
const SiteSample forest=[](double x,double y){return Site{100+0.01*x+0.02*y,0,0.03,1,0.4};};
}
TEST(scene_scatter_bounds_count_cells_without_rounding_or_overflow) {
    CHECK_EQ((ScatterBounds{0,0,16,24}.cells()),std::size_t(6));
    CHECK_EQ((ScatterBounds{-1,-9,1,1}.cells()),std::size_t(6));
    CHECK_EQ((ScatterBounds{-16,-24,0,0}.cells()),std::size_t(6));
    CHECK_EQ((ScatterBounds{8,0,8,8}.cells()),std::size_t(0));
    CHECK_EQ((ScatterBounds{0,8,8,0}.cells()),std::size_t(0));
    constexpr std::int64_t far=std::int64_t{1}<<60;
    CHECK_EQ((ScatterBounds{far+7,0,far+9,8}.cells()),std::size_t(2));
    CHECK_EQ((ScatterBounds{-far-9,0,-far-7,8}.cells()),std::size_t(2));
    constexpr auto low=std::numeric_limits<std::int64_t>::min();
    constexpr auto high=std::numeric_limits<std::int64_t>::max();
    CHECK_EQ((ScatterBounds{low,low,high,high}.cells()),std::numeric_limits<std::size_t>::max());
    CHECK_EQ((ScatterBounds{high-1,0,high,8}.cells()),std::size_t(1));
}
TEST(scene_scatter_rectangles_keep_exact_half_open_bounds_and_stable_objects) {
    const auto full=scatter(42,ScatterBounds{256,512,1800,1040},5000,5000,land,forest);
    CHECK(!full.objects.empty());
    for (const auto bounds:std::array<ScatterBounds,2>{{{257,515,1793,1035},{511,519,1799,1039}}}) {
        const auto part=scatter(42,bounds,5000,5000,land,[&](double x,double y) {
            CHECK(x>=bounds.minX && x<bounds.maxX && y>=bounds.minY && y<bounds.maxY);
            return forest(x,y);
        });
        std::vector<Object> expected;
        std::array<std::size_t,5> counts{};
        for (const auto& object:full.objects)
            if (object.x>=bounds.minX && object.x<bounds.maxX &&
                object.y>=bounds.minY && object.y<bounds.maxY) {
                expected.push_back(object);
                ++counts[object.model];
            }
        CHECK(!expected.empty());
        CHECK_EQ(part.objects,expected);
        CHECK_EQ(part.populations,counts);
    }
}
TEST(scene_scatter_rectangles_preserve_legacy_region_results) {
    for (const int region:{-1,0,4,8}) {
        const std::int64_t centre=region*kRegion;
        const auto legacy=scatter(42,region,region,5000,5000,land,forest);
        const auto rectangle=scatter(42,ScatterBounds{centre-kRadius,centre-kRadius,
            centre+kRadius,centre+kRadius},5000,5000,land,forest);
        CHECK_EQ(legacy.objects,rectangle.objects);
        CHECK_EQ(legacy.populations,rectangle.populations);
        CHECK_EQ(legacy.sampled,rectangle.sampled);
        CHECK_EQ(legacy.waterTilesSkipped,rectangle.waterTilesSkipped);
    }
}
TEST(scene_scatter_rejects_invalid_or_over_budget_rectangles_before_queries) {
    std::size_t queries=0;
    const LandTest countedLand=[&](int,int){++queries;return true;};
    const SiteSample countedSite=[&](double x,double y){++queries;return forest(x,y);};
    constexpr auto low=std::numeric_limits<std::int64_t>::min();
    constexpr auto high=std::numeric_limits<std::int64_t>::max();
    for (const auto bounds:std::array<ScatterBounds,5>{{{}, {8,0,0,8}, {0,8,8,0},
            {0,0,2049,2048}, {low,low,high,high}}}) {
        bool rejected=false;
        try { (void)scatter(42,bounds,5000,5000,countedLand,countedSite); }
        catch (const std::invalid_argument&) { rejected=true; }
        CHECK(rejected);
    }
    CHECK_EQ(queries,std::size_t(0));
    const ScatterBounds limit{0,0,2048,2048};
    CHECK_EQ(limit.cells(),kMaxScatterCells);
    CHECK(scatter(42,limit,5000,5000,[](int,int){return false;},countedSite).objects.empty());
    CHECK_EQ(queries,std::size_t(0));
}
TEST(scene_scatter_is_repeatable_and_models_have_stable_positions) {
    const auto a=scatter(42,4,4,5000,5000,land,forest);
    const auto b=scatter(42,4,4,5000,5000,land,forest);
    CHECK(!a.objects.empty());CHECK_EQ(a.objects,b.objects);
    CHECK(a.objects.size()<=std::size_t(2*kRadius/kCell)*(2*kRadius/kCell));
    std::array<std::size_t,5> counts{};
    for (const auto& o:a.objects) {
        CHECK(o.x>=0 && o.y>=0 && o.x<5000 && o.y<5000);
        CHECK(o.scale>=0.75 && o.scale<=1.4);CHECK(o.model<5);
        ++counts[o.model];
    }
    for (const auto count:counts) CHECK(count>0);
    CHECK_EQ(counts,a.populations);
    const auto changed=scatter(43,4,4,5000,5000,land,forest);
    CHECK(a.objects!=changed.objects);
}
TEST(scene_scatter_neighbouring_windows_do_not_shuffle_instances) {
    const auto a=scatter(42,4,4,5000,5000,land,forest);
    const auto b=scatter(42,5,4,5000,5000,land,forest);
    std::map<std::uint64_t,Object> byId;
    for (const auto& o:a.objects) byId.emplace(o.id,o);
    std::size_t common=0;
    for (const auto& o:b.objects) if (byId.contains(o.id)) { CHECK(o==byId.at(o.id));++common; }
    CHECK(common>100);
}
TEST(scene_scatter_open_water_skips_all_expensive_queries) {
    std::size_t samples=0,maskCalls=0;
    const auto result=scatter(42,4,4,5000,5000,[&](int,int){++maskCalls;return false;},
        [&](double,double){++samples;return Site{};});
    CHECK(result.objects.empty());CHECK_EQ(samples,0u);CHECK_EQ(result.sampled,0u);
    CHECK_EQ(result.waterTilesSkipped,maskCalls);CHECK(maskCalls>0 && maskCalls<=16);
}
TEST(scene_scatter_wet_steep_and_nonfinite_sites_never_receive_objects) {
    for (const auto bad:std::array<Site,3>{{{0,2,0,1,1},{100,0,2,1,1},
            {std::numeric_limits<double>::quiet_NaN(),0,0,1,1}}}) {
        CHECK(scatter(42,4,4,5000,5000,land,[&](double,double){return bad;}).objects.empty());
    }
}
TEST(scene_forest_noise_forms_connected_masses_and_clearings_at_world_coordinates) {
    for (const auto seed:{1u,7u,42u,128u}) {
        double nearDifference=0,farDifference=0;
        int dense=0,clearing=0;
        for (int y=0;y<4096;y+=32) for (int x=0;x<4096;x+=32) {
            const double d=forestDensity(seed,x,y);
            CHECK(d>=0 && d<=1);CHECK_EQ(d,forestDensity(seed,x,y));
            dense+=d>0.75;clearing+=d<0.08;
            nearDifference+=std::abs(d-forestDensity(seed,x+8,y));
            farDifference+=std::abs(d-forestDensity(seed,x+256,y));
        }
        CHECK(dense>100);CHECK(clearing>100);
        CHECK(nearDifference*3<farDifference);
    }
    CHECK_EQ(forestDensity(42,std::numeric_limits<double>::infinity(),0),0.0);
    CHECK_EQ(forestDensity(42,0,std::numeric_limits<double>::quiet_NaN()),0.0);
}
TEST(scene_forest_actual_tree_frequency_follows_groves_instead_of_uniform_sprinkling) {
    std::size_t denseSites=0,openSites=0,denseTrees=0,openTrees=0;
    const auto result=scatter(42,8,8,5000,5000,land,[&](double x,double y) {
        const double d=forestDensity(42,x,y);
        denseSites+=d>0.7;openSites+=d<0.1;
        return Site{100,0,0,1,0.4};
    });
    for (const auto& o:result.objects) if (o.model<2) {
        const double d=forestDensity(42,o.x,o.y);
        denseTrees+=d>0.7;openTrees+=d<0.1;
    }
    CHECK(denseSites>100);CHECK(openSites>100);
    const double denseRate=double(denseTrees)/denseSites,openRate=double(openTrees)/openSites;
    std::cout<<"  tree frequency: core="<<denseRate<<", clearing="<<openRate
             <<", core trees/ha="<<denseRate*10000/(kCell*kCell)<<'\n';
    CHECK(denseRate>0.65);CHECK(openRate<0.12);CHECK(denseRate>openRate*6);
    // >100 trees/ha in cores versus the previous maximum 22.7 trees/ha.
    CHECK(denseRate*10000/(kCell*kCell)>100);
}
TEST(scene_forest_does_not_leak_into_nonforest_or_above_treeline) {
    for (const auto site:std::array<Site,3>{{{100,0,0,0,0},{2500,0,0,1,1},{100,0,1,1,1}}}) {
        const auto result=scatter(42,8,8,5000,5000,land,[&](double,double){return site;});
        CHECK_EQ(result.populations[0]+result.populations[1],0u);
    }
}
TEST(scene_bushes_prefer_edges_to_both_forest_cores_and_empty_clearings) {
    for (const auto seed:{1u,7u,42u,128u}) {
        std::array<std::size_t,3> sites{},bushes{};
        const auto band=[seed](double x,double y) {
            const double d=forestDensity(seed,x,y);
            return d<0.08?0:(d>=0.2 && d<=0.45?1:(d>0.75?2:-1));
        };
        const auto result=scatter(seed,8,8,5000,5000,land,[&](double x,double y) {
            const int b=band(x,y);if (b>=0) ++sites[b];
            return Site{100,0,0,1,0.4};
        });
        for (const auto& o:result.objects) if (o.model==2) {
            const int b=band(o.x,o.y);if (b>=0) ++bushes[b];
        }
        for (const auto count:sites) CHECK(count>100);
        const double clearing=double(bushes[0])/sites[0],edge=double(bushes[1])/sites[1];
        const double core=double(bushes[2])/sites[2];
        std::cout<<"  bush frequency seed "<<seed<<": clearing="<<clearing
                 <<", edge="<<edge<<", core="<<core<<'\n';
        CHECK(edge>0.03);CHECK(edge>clearing*2);CHECK(edge>core*2);
    }
}
TEST(scene_rocks_form_groups_even_without_forest_habitat) {
    // Full 1536 m window, 64 m bins of 64 candidates each. Overdispersion
    // distinguishes outcrops from independent uniform rock sprinkling.
    constexpr int bins=2*kRadius/64;
    for (const auto seed:{1u,7u,42u,128u}) {
        const auto result=scatter(seed,8,8,5000,5000,land,
            [](double,double){return Site{100,0,0,0,0};});
        CHECK(result.populations[3]>200);CHECK_EQ(result.objects.size(),result.populations[3]);
        std::array<std::size_t,bins*bins> counts{};
        for (const auto& o:result.objects) {
            const int x=int(o.x-(8*kRegion-kRadius))/64,y=int(o.y-(8*kRegion-kRadius))/64;
            ++counts[y*bins+x];
        }
        const double mean=double(result.objects.size())/counts.size();
        double variance=0;
        for (const auto n:counts) variance+=(n-mean)*(n-mean)/counts.size();
        std::cout<<"  rock groups seed "<<seed<<": rocks="<<result.objects.size()
                 <<", bin variance/mean="<<variance/mean<<'\n';
        CHECK(variance>1.2*mean);
    }
}
TEST(scene_tree_species_choice_does_not_change_placement_or_other_models) {
    const auto a=scatter(42,8,8,5000,5000,land,
        [](double,double){return Site{100,0,0,1,0};});
    const auto b=scatter(42,8,8,5000,5000,land,
        [](double,double){return Site{100,0,0,1,1};});
    CHECK_EQ(a.objects.size(),b.objects.size());
    CHECK(a.populations[0]>100);CHECK_EQ(b.populations[0],0u);
    for (std::size_t i=0;i<std::min(a.objects.size(),b.objects.size());++i) {
        auto object=b.objects[i];
        if (object.model<2) object.model=a.objects[i].model;
        CHECK_EQ(a.objects[i],object);
    }
}
TEST(scene_scatter_overlap_is_complete_across_both_axes_and_world_resize) {
    const auto a=scatter(42,8,8,5000,5000,land,forest);
    for (const auto [rx,ry]:std::array<std::pair<int,int>,3>{{{9,8},{8,9},{9,9}}}) {
        const auto b=scatter(42,rx,ry,10000,10000,land,forest);
        std::vector<Object> overlapA,overlapB;
        for (const auto& o:a.objects)
            if (o.x>=rx*kRegion-kRadius && o.y>=ry*kRegion-kRadius) overlapA.push_back(o);
        for (const auto& o:b.objects)
            if (o.x<8*kRegion+kRadius && o.y<8*kRegion+kRadius) overlapB.push_back(o);
        CHECK(overlapA.size()>100);CHECK_EQ(overlapA,overlapB);
    }
}
TEST(scene_mesh_budget_keeps_largest_objects_without_overspending_or_order_dependence) {
    std::vector<MeshDemand> demands{{100,600},{80,500},{80,500},{40,100},{10,1000000},
        {std::numeric_limits<double>::quiet_NaN(),1000}};
    for (const std::size_t budget:{0u,599u,600u,1000u,1600u,1700u,100000u}) {
        const double threshold=meshPixelThreshold(demands,budget);
        std::size_t spent=0;
        for (const auto& d:demands)
            if (objectLod(d.pixels,0,threshold).mesh>0) spent+=d.triangles;
        CHECK(spent<=budget);
        std::reverse(demands.begin(),demands.end());
        CHECK_EQ(threshold,meshPixelThreshold(demands,budget));
        CHECK_EQ(objectLod(50,0,threshold).coverage,1.0f);
    }
    CHECK_EQ(meshPixelThreshold(demands,1000),80.0);
    CHECK_EQ(meshPixelThreshold({},0),world::decor::kMeshFloorPixels);
    CHECK_EQ(objectLod(60,0,44).mesh,0.5f);
}
TEST(scene_mesh_budget_excludes_mixed_cost_ties_and_preserves_impostor_coverage) {
    std::vector<MeshDemand> demands{{90,200},{40,300},{60,400},{90,800}};
    do {
        for (const std::size_t budget:{0u,500u,999u,1000u,1400u,1700u}) {
            const auto threshold=meshPixelThreshold(demands,budget);
            CHECK_EQ(threshold,budget<1000?90.0:(budget<1400?60.0:(budget<1700?40.0:world::decor::kMeshFloorPixels)));
            std::size_t spent=0;
            for (const auto& d:demands) {
                const auto lod=objectLod(d.pixels,100,threshold);
                CHECK_EQ(lod.coverage,1.0f);
                if (lod.mesh>0) spent+=d.triangles;
                if (d.pixels<=threshold) CHECK_EQ(lod.mesh,0.0f); // full impostor, not a culled object
            }
            CHECK(spent<=budget);
        }
    } while (std::next_permutation(demands.begin(),demands.end(),
        [](const auto& a,const auto& b){return a.triangles<b.triangles;}));
}
TEST(scene_objects_lod_uses_pixels_with_complementary_transition_and_range_fade) {
    CHECK_EQ(objectLod(100,0).mesh,1.0f);CHECK_EQ(objectLod(100,0).coverage,1.0f);
    CHECK_EQ(objectLod(10,0).mesh,0.0f);CHECK_EQ(objectLod(10,0).coverage,1.0f);
    CHECK_EQ(objectLod(30,0).mesh,0.5f);
    CHECK_EQ(objectLod(0.1,0).coverage,0.0f);CHECK_EQ(objectLod(100,640).coverage,0.0f);
    CHECK(objectLod(100,540).coverage>0 && objectLod(100,540).coverage<1);
    float previous=0;
    for (int p=0;p<1000;++p) {
        const auto lod=objectLod(p*0.1,0);
        CHECK(lod.mesh>=previous);CHECK(lod.mesh>=0 && lod.mesh<=1);
        CHECK(lod.coverage>=0 && lod.coverage<=1);previous=lod.mesh;
    }
    CHECK_EQ(objectLod(std::numeric_limits<double>::quiet_NaN(),0).coverage,0.0f);
}
TEST(scene_mesh_levels_follow_projected_shape_error_not_instance_scale) {
    // The chain measured for CommonTree_1: full mesh, then a quarter, a tenth
    // and a twenty-fifth of its triangles, with the 95th percentile distance
    // from the imported surface in model metres. Height 13.338 m.
    const std::array<float,4> tree{0.0f,0.121f,0.260f,0.546f};
    constexpr double extent=13.338;
    CHECK_EQ(meshLevel(tree,1200,extent),0u);   // filling the screen: the model itself
    CHECK_EQ(meshLevel(tree,60,extent),3u);     // a forest at arm's length
    CHECK_EQ(meshLevel(tree,25,extent),3u);     // about to become an impostor
    CHECK_EQ(meshLevel(tree,200,extent),1u);    // a tree in front of the camera
    // Every level stays within its pixel allowance, and the choice never goes
    // backwards as an object gets smaller: a coarser level for a larger object
    // would be visible as a pop in the wrong direction.
    std::size_t previous=0;
    for (int p=2000;p>0;--p) {
        const auto level=meshLevel(tree,p*0.5,extent);
        CHECK(level>=previous);CHECK(level<tree.size());
        CHECK(tree[level]*(p*0.5)/extent<=kLevelPixelError+1e-9);
        previous=level;
    }
    // Two trees of one species that reach the same height on screen get the
    // same triangles whether one is a sapling close by or a giant far off:
    // `pixels` already carries the instance scale, so it must not be applied
    // again. The rule is a property of the model and the screen alone.
    CHECK_EQ(meshLevel(tree,90,extent),meshLevel(tree,90,extent));
    CHECK(meshLevel(tree,300,extent)<meshLevel(tree,90,extent));
}
TEST(scene_mesh_levels_refuse_a_chain_that_is_not_ordered_or_measured) {
    const std::array<float,3> ordered{0.0f,0.1f,0.4f};
    const std::array<float,3> backwards{0.0f,0.4f,0.1f};
    // An unordered chain would otherwise let a coarser level win on a larger
    // object. Stop at the last level that was still increasing instead.
    CHECK_EQ(meshLevel(backwards,100,10.0),0u);   // level 1 too coarse, level 2 not trusted
    CHECK_EQ(meshLevel(backwards,50,10.0),1u);
    CHECK_EQ(meshLevel(ordered,50,10.0),2u);
    CHECK_EQ(meshLevel(ordered,20,10.0),2u);
    const std::array<float,2> broken{0.0f,std::numeric_limits<float>::quiet_NaN()};
    CHECK_EQ(meshLevel(broken,20,10.0),0u);
    CHECK_EQ(meshLevel({},20,10.0),0u);
    CHECK_EQ(meshLevel(ordered,std::numeric_limits<double>::infinity(),10.0),0u);
    CHECK_EQ(meshLevel(ordered,20,0.0),0u);
    CHECK_EQ(meshLevel(ordered,-5,10.0),0u);
    // A tighter allowance may only ask for more detail, never less.
    for (int p=1;p<400;++p)
        CHECK(meshLevel(ordered,p,10.0,1.0)<=meshLevel(ordered,p,10.0,4.0));
}
TEST(scene_mesh_budget_is_spent_on_levels_rather_than_on_full_meshes) {
    // What the forest frame actually looked like: two hundred and fifty trees
    // between fifty and a hundred pixels. At full detail they alone exhaust a
    // budget that then pushes the impostor crossover up to fifty-five pixels.
    const std::array<float,4> tree{0.0f,0.121f,0.260f,0.546f};
    const std::array<std::size_t,4> triangles{6265,1544,609,234};
    constexpr double extent=13.338;
    std::vector<MeshDemand> full,levelled;
    for (int i=0;i<250;++i) {
        const double pixels=55+i*0.16;
        full.push_back({pixels,triangles[0]});
        levelled.push_back({pixels,triangles[meshLevel(tree,pixels,extent)]});
    }
    std::size_t before=0,after=0;
    for (std::size_t i=0;i<full.size();++i) { before+=full[i].triangles;after+=levelled[i].triangles; }
    CHECK(before>kMeshTriangleBudget);
    CHECK(after*8<before);
    // With the levels in hand the budget is no longer the thing deciding which
    // objects get geometry at all, so the crossover returns to its own floor.
    CHECK(meshPixelThreshold(full)>22.0);
    CHECK_EQ(meshPixelThreshold(levelled),world::decor::kMeshFloorPixels);
}
TEST(scene_objects_impostor_azimuth_wraps_and_respects_instance_rotation) {
    const double turn=2*std::acos(-1.0);
    for (int i=0;i<8;++i) {
        CHECK_EQ(impostorView(i*turn/8,0),i);
        CHECK_EQ(impostorView(i*turn/8+turn,0),i);
        CHECK_EQ(impostorView(i*turn/8-2*turn,0),i);
        CHECK_EQ(impostorView(i*turn/8+0.4,0.4),i);
    }
}

TEST(scene_object_wind_keeps_roots_and_rocks_fixed) {
    for (int tick=0;tick<200;++tick) {
        const float time=tick*0.1f;
        CHECK_EQ(modelWindDisplacement(0,15,1,1,0.3f,time,1),0.0f);
        CHECK_EQ(modelWindDisplacement(1,15,0,1,0.3f,time,1),0.0f);
        CHECK_EQ(modelWindDisplacement(1,15,1,0,0.3f,time,1),0.0f);
        const float tip=modelWindDisplacement(1,15,1,1,0.3f,time,1);
        CHECK(std::abs(tip)<=0.4051f);
        CHECK(std::abs(modelWindDisplacement(0.5f,15,1,1,0.3f,time,1)-tip*0.25f)<0.00001f);
    }
    CHECK(modelWindDisplacement(1,15,1,1,0.3f,2,1)!=modelWindDisplacement(1,15,1,1,0.3f,6,1));
    // Even immediately before recentering, all objects inside the 600 m fade
    // radius have already been generated; a camera tile boundary cannot cut them.
    CHECK(kRadius-kRegion>=600);
}

TEST(scene_vegetation_paints_forest_soil_and_grows_understory_without_repainting_exclusions) {
    using namespace world::foliage;
    const auto cover=[](int material,float depth=-10,float height=100,float upright=1) {
        return vegetationCover(material==0,material==1,material==2,material==3,material==4,material==5,
            0,0,1,0,0.6f,height,depth,upright,0.8f,0.5f);
    };
    const auto forest=cover(1);
    CHECK(forest.ground>0.8f);CHECK(forest.grass>0.2f);CHECK(forest.canopy>0.5f);
    CHECK(forest.green>forest.red && forest.red>forest.blue);
    CHECK_EQ(forest.ground,cover(1,-10,100,0.3f).ground); // normals/mesh LOD do not repaint
    CHECK_EQ(cover(1,-10,100,0.3f).grass,0.0f);
    for (int m=2;m<6;++m) { CHECK_EQ(cover(m).ground,0.0f);CHECK_EQ(cover(m).grass,0.0f); }
    CHECK_EQ(cover(1,1).ground,0.0f);CHECK_EQ(cover(1,1).grass,0.0f);
    CHECK_EQ(cover(1,-10,2500).ground,0.0f);CHECK_EQ(cover(1,-10,2500).grass,0.0f);
    const auto bare=vegetationCover(0,1,0,0,0,0,0,0,0,0,0.6f,100,-10,1,0.8f,0.5f);
    CHECK_EQ(bare.ground,0.0f);CHECK_EQ(bare.grass,0.0f);
    // Surface texture keeps contrast; green is not a solid flat paint.
    CHECK(vegetationGroundChannel(0.6f,forest.green,0.6f,forest.ground,forest.canopy)>
          vegetationGroundChannel(0.2f,forest.green,0.2f,forest.ground,forest.canopy));
    CHECK_EQ(vegetationGroundChannel(0.4f,0.7f,0.4f,0,0),0.4f);
}

TEST(scene_adaptive_grass_roots_are_bounded_repeatable_and_follow_triangle_morphs) {
    using namespace world;
    const auto surface=[](double x,double y){return std::array{float(100+0.1*x+0.02*y),0.0f};};
    const auto parent=[&](double x,double y){auto s=surface(x,y);s[0]+=5;return s;};
    const auto mesh=terrain::makeAdaptiveMesh(8,64,0.01,surface,parent,0.001,{},
        [&](double x,double y){return surface(x,y)[0]-10;},
        [&](double x,double y){return surface(x,y)[0]-5;});
    const auto roots=buildPageGrass(*mesh,0,0,4,4);
    CHECK_EQ(roots.size(),kGrassCandidateBudget);CHECK_EQ(roots,buildPageGrass(*mesh,0,0,4,4));
    std::map<std::pair<float,float>,PageGrassRoot> unique;
    for (const auto& r:roots) {
        CHECK(unique.emplace(std::pair{r.position[0],r.position[1]},r).second);
        CHECK(r.position[0]>0 && r.position[0]<512 && r.position[1]>0 && r.position[1]<512);
        CHECK(std::abs(r.position[2]-surface(r.position[0],r.position[1])[0])<0.001f);
        CHECK(std::abs(r.parentHeight-r.position[2]-5)<0.001f);
        CHECK(std::abs(r.priorHeight-r.position[2]+10)<0.001f);
        CHECK(std::abs(r.priorParentHeight-r.position[2]+5)<0.001f);
    }
    const auto finer=terrain::makeAdaptiveMesh(16,32,0.01,surface,parent);
    const auto fineRoots=buildPageGrass(*finer,0,0,4,4);
    CHECK_EQ(fineRoots.size(),roots.size());
    for (const auto& r:fineRoots) {
        CHECK(unique.contains({r.position[0],r.position[1]}));
        const auto& old=unique.at({r.position[0],r.position[1]});
        CHECK(std::abs(old.position[2]-r.position[2])<0.001f);
    }
    // A focus-window shift retains every root in the intersection.
    const auto moved=buildPageGrass(*mesh,0,0,5,4);
    CHECK_EQ(moved.size(),std::size_t(448/2)*(512/2));
    for (const auto& r:moved) CHECK_EQ(r,unique.at({r.position[0],r.position[1]}));
}

TEST(scene_adaptive_grass_honours_stitched_heights_and_never_uses_skirt_walls) {
    using namespace world;
    auto original=terrain::makeAdaptiveMesh(1,64,0,[](double,double){return std::array{100.0f,0.0f};});
    auto mesh=*original;
    for (auto& v:mesh.vertices) { v.skirt|=2;v.edgeBed=120;v.priorEdge=90; }
    const auto roots=buildPageGrass(mesh,0,0,0,0);
    CHECK_EQ(roots.size(),std::size_t(32*32));
    for (const auto& r:roots) {
        CHECK_EQ(r.position[2],120.0f);CHECK_EQ(r.parentHeight,120.0f);
        CHECK_EQ(r.priorHeight,90.0f);CHECK_EQ(r.priorParentHeight,90.0f);
    }
    CHECK(buildPageGrass(mesh,4096,4096,0,0).empty());
}

TEST(scene_woodland_habitat_is_independent_of_grass_palette_and_valley_temperature) {
    using namespace generation;
    for (const auto biome:{Climate::Steppe,Climate::TemperateForest,Climate::RiverValley,
            Climate::Savanna,Climate::Tundra,Climate::Alpine,Climate::Desert}) {
        WorldMapData map;map.width=map.height=4;map.seed=42;map.cells.resize(16);
        for (auto& c:map.cells) { c.climate=biome;c.sea=false;c.temperature=150;c.moisture=200;c.fertility=200; }
        world::HeightField field(&map,map.seed);
        const core::WorldPos p{core::Fixed::fromInt(900),core::Fixed::fromInt(900)};
        const auto climate=field.surfaceClimateAt(p);
        world::ClimateField atlas;atlas.raise(map,field);
        CHECK(std::abs(atlas.at(p).woodland.toDouble()-climate.woodland.toDouble())<0.005);
        if (biome==Climate::TemperateForest) CHECK_EQ(climate.woodland,core::kOne);
        if (biome==Climate::RiverValley) {
            CHECK_EQ(climate.foliage[2],core::kOne); // green palette is NOT a forest declaration
            CHECK(climate.woodland.toDouble()<0.2);
        }
        if (biome==Climate::Steppe || biome==Climate::Tundra || biome==Climate::Alpine || biome==Climate::Desert) {
            CHECK_EQ(climate.woodland,core::kZero);
            for (int y=0;y<2048;y+=64) for (int x=0;x<2048;x+=64)
                CHECK_EQ(atlas.forestCoverAt(x,y),0.0f);
        }
    }
}

TEST(scene_forest_regions_contain_true_open_meadows_without_uniform_tree_leakage) {
    for (const auto seed:{1u,7u,42u,128u}) {
        int open=0,dense=0,total=0;
        for (int y=0;y<4096;y+=32) for (int x=0;x<4096;x+=32) {
            const auto d=forestDensity(seed,x,y);open+=d==0;dense+=d>0.7;++total;
        }
        CHECK(open>total/10);CHECK(dense>total/20);
        const auto result=scatter(seed,8,8,5000,5000,land,forest);
        for (const auto& o:result.objects) if (o.model<2) CHECK(forestDensity(seed,o.x,o.y)>0);
    }
}

TEST(scene_view_wide_proxy_grid_survives_far_blocks_and_quadtree_splits) {
    const auto flat=[](double,double){return std::array{100.0f,0.0f};};
    const auto parent=world::terrain::makeAdaptiveMesh(8,64,0,flat);
    const auto child=world::terrain::makeAdaptiveMesh(4,64,0,flat);
    CHECK(world::buildPageGrass(*parent,8192,8192,0,0).empty()); // old local window
    // Also cover a LOD cell larger than a child block. Its one global candidate
    // belongs only to the child containing it, not once to every clipped grid.
    for (const int cell:{32,512}) {
        const auto far=world::buildPageGrass(*parent,8192,8192,0,0,cell,0);
        CHECK_EQ(far.size(),std::size_t(512/cell)*(512/cell));
        std::map<std::pair<float,float>,world::PageGrassRoot> byPosition;
        for (const auto& root:far) byPosition.emplace(std::pair{root.position[0],root.position[1]},root);
        std::size_t count=0;
        for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
            const auto roots=world::buildPageGrass(*child,8192+x*256,8192+y*256,123,-321,cell,0);
            for (const auto& root:roots) { CHECK_EQ(root,byPosition.at({root.position[0],root.position[1]}));++count; }
        }
        CHECK_EQ(count,far.size()); // neither gaps nor duplicate points at chunk edges
    }
}


TEST(impostor_pair_turns_instead_of_snapping) {
    using world::decor::impostorPair;
    using world::decor::impostorView;
    constexpr double kTurn=6.283185307179586;
    // The eight views are baked at k/8 of a turn. A pair is the one below and
    // the one above, so at blend zero it IS the view the snapping rule picks -
    // the blended path is a strict improvement on it, not a different answer.
    for (int k=0;k<8;++k) {
        const auto pair=impostorPair(k*kTurn/8,0);
        CHECK_EQ(pair.view,k%8);
        CHECK_EQ(pair.next,(k+1)%8);
        CHECK(pair.blend<1e-5f);
        CHECK_EQ(pair.view,impostorView(k*kTurn/8,0));
    }
    // Sweeping a whole turn, the pair walks the circle one step at a time and
    // never jumps: the continuous position it names advances smoothly, which is
    // the whole point - every impostor in the frame used to move at once.
    double previous=-1;
    int wrapped=0;
    for (int i=0;i<=2000;++i) {
        const double angle=i*kTurn/2000;
        const auto pair=impostorPair(angle,0);
        CHECK(pair.view>=0 && pair.view<8);
        CHECK_EQ(pair.next,(pair.view+1)%8);
        CHECK(pair.blend>=0 && pair.blend<1);
        const double position=pair.view+double(pair.blend);
        if (previous>=0) {
            const double step=position-previous;
            // One step of the sweep is 8/2000 of a view, or a wrap past eight.
            if (step<0) { ++wrapped;CHECK(previous>7.9);CHECK(position<0.1); }
            else CHECK(step<0.02);
        }
        previous=position;
    }
    CHECK_EQ(wrapped,1);
    // The object's own yaw turns it the same way the camera does.
    for (int i=0;i<64;++i) {
        const double yaw=i*kTurn/64;
        const auto a=impostorPair(1.234,yaw);
        const auto b=impostorPair(1.234+yaw,yaw*2);
        CHECK_EQ(a.view,b.view);
        CHECK(std::abs(a.blend-b.blend)<1e-5f);
    }
    // And nothing that is not a number becomes a layer index.
    const auto broken=impostorPair(std::numeric_limits<double>::quiet_NaN(),0);
    CHECK_EQ(broken.view,0);
    CHECK_EQ(broken.next,0);
    CHECK_EQ(broken.blend,0.0f);
}
