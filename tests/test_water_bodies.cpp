#include "framework.hpp"
#include "game/world/height_field.hpp"
#include "game/world/macro.hpp"
#include "game/generation/world_map_gen.hpp"
#include "../assets/shaders/water_body.hlsli"
#include <cmath>
#include <vector>
#include <algorithm>

namespace {
using core::Fixed;
generation::WorldMapData country() {
    generation::WorldMapData m; m.width=m.height=10; m.cells.resize(100);
    for (auto& c:m.cells) { c.elevation=30; c.moisture=160; c.temperature=130; }
    return m;
}
core::WorldPos centre(int x,int y) {
    const auto step=Fixed::fromInt(generation::kMetresPerCell);
    return {(Fixed::fromInt(x)+Fixed::ratio(1,2))*step,(Fixed::fromInt(y)+Fixed::ratio(1,2))*step};
}
generation::WorldMapData riverWorld() {
    auto m=country();
    int east=0;
    for (int d=0;d<core::kNeighbourCount;++d)
        if (core::neighbour({4,4},d)==core::TilePos{5,4}) east=d;
    for (int x=1;x<9;++x) {
        auto& c=m.at({x,4}); c.river=true; c.drainSize=8; c.riverSize=8;
        c.riverOut=c.drainOut=static_cast<std::int8_t>(east);
    }
    m.at({9,4}).sea=true; m.at({9,4}).elevation=0;
    return m;
}
}
TEST(water_bodies_river_has_downstream_flow_and_resolved_width) {
    auto map=riverWorld(); world::MacroWorld macro; macro.attach(&map);
    auto c=macro.channelOf({4,4}); CHECK(c.has_value());
    CHECK(c->halfWidth>=Fixed::fromInt(4));
    // The valley is sized by the fall it has to absorb, not by the width of the
    // water in it, and it is bounded by how far the drainage is gathered.
    CHECK(c->valleyReach>=c->halfWidth*Fixed::fromInt(2));
    CHECK(c->valleyReach<=Fixed::fromInt(800));
    const auto p=world::MacroWorld::pointOn(*c,Fixed::ratio(1,2));
    const auto w=macro.carve(p,Fixed::fromInt(300),core::kZero);
    CHECK(w.wet); CHECK(w.flowX>Fixed::ratio(1,2));
    CHECK(core::abs(w.flowY)<Fixed::ratio(4,5));
    // The water is near the macro line, not on it. MacroWorld sizes a reach
    // from cell centre to cell centre; the hydrology graph runs the actual
    // course, and that course meanders - by as much as a meander belt, which
    // for a trunk is wider than a cell. Sampling the midpoint of the straight
    // line and expecting to find water there was asserting that the two
    // coincide, and they do not and need not: nothing outside these tests reads
    // the macro channel's geometry.
    //
    // What IS worth holding is that the river is somewhere across that line and
    // flows the way the macro says. So: look across it.
    world::HeightField field(&map,42);
    bool found=false;
    for (int step=-24;step<=24 && !found;++step) {
        const core::WorldPos across{p.x,p.y+Fixed::fromInt(step)*Fixed::fromInt(12)};
        const auto sampled=field.waterOver(across,Fixed::fromInt(12),Fixed::ratio(1,10));
        if (sampled.kind!=world::HeightField::WaterKind::River) continue;
        if (sampled.cover<=Fixed::ratio(1,2)) continue;
        CHECK(sampled.flowX>core::kZero);
        found=true;
    }
    CHECK(found);
    const auto mouth=macro.channelOf({8,4}); CHECK(mouth.has_value());
    CHECK(mouth->halfWidthEnd>mouth->halfWidth);
}
TEST(water_bodies_carving_never_builds_an_embankment) {
    auto map=riverWorld(); world::MacroWorld macro; macro.attach(&map);
    const auto c=macro.channelOf({4,4}); CHECK(c.has_value());
    const auto p=world::MacroWorld::pointOn(*c,Fixed::ratio(1,2));
    for (int d=-160;d<=160;d+=2) for (int detail:{-30,-5,0,20}) {
        const core::WorldPos at{p.x,p.y+Fixed::fromInt(d)};
        const auto w=macro.carve(at,Fixed::fromInt(300),Fixed::fromInt(detail));
        CHECK(w.floor<=Fixed::fromInt(300+detail));
    }
}
TEST(water_bodies_lake_head_does_not_follow_the_dry_hillside) {
    auto map=country();
    for (auto& c:map.cells) c.elevation=60;
    map.lakeDepthField.assign(100,0); map.lakeLevelField.assign(100,0);
    // Named as well as filled. A basin is a body in the hydrology graph, and a
    // body has an identity there - the same one the generator gives it. Depth
    // and head alone used to be enough because the height field read the raster
    // itself; it asks the graph now, and the graph does not invent a lake out of
    // two unlabelled rasters any more than the world does.
    map.lakeRegionField.assign(100,-1);
    for (int y=3;y<=6;++y) for (int x=3;x<=6;++x) {
        map.at({x,y}).elevation=30;
        map.lakeDepthField[y*10+x]=10; map.lakeLevelField[y*10+x]=30;
        map.lakeRegionField[y*10+x]=34;   // one basin, named by its first cell
    }
    world::HeightField field(&map,42);
    int found=0;
    const auto mid=centre(4,4);
    for (int dx=-500;dx<800;dx+=32) {
        const auto w=field.waterOver({mid.x+Fixed::fromInt(dx),mid.y},Fixed::fromInt(12),Fixed::ratio(1,5));
        if (w.kind!=world::HeightField::WaterKind::Lake) continue;
        ++found;
        CHECK(std::abs(w.level.toDouble()-30*generation::kMetresPerElevationStep)<0.01);
        CHECK_EQ(w.flowX,core::kZero); CHECK_EQ(w.flowY,core::kZero);
    }
    CHECK(found>10);
}
TEST(water_bodies_render_profiles_separate_ocean_rivers_and_lakes) {
    using namespace world::water_body;
    CHECK_EQ(wbWaveScale(1,0),0.0f);
    CHECK(wbWaveScale(0,1)<0.05f);
    CHECK_EQ(wbWaveScale(0,0),1.0f);
    CHECK(wbNormalScale(0,1)<wbNormalScale(1,0));
    CHECK_EQ(wbInlandAlpha(-0.1f,1,0.02f),0.0f);
    CHECK_EQ(wbInlandAlpha(1,0,0.02f),0.0f);
    CHECK(wbInlandAlpha(2,1,0.02f)>0.9f);
    float previous=0;
    for (int i=0;i<=200;++i) {
        const auto a=wbInlandAlpha(float(i)/1000,1,0.02f);
        CHECK(a>=previous); CHECK(a-previous<0.01f); previous=a;
    }
}

TEST(water_bodies_filtered_inland_banks_never_become_ocean_surf) {
    using namespace world::water_body;
    for (const float fraction : {0.001f, 0.01f, 0.1f, 0.5f, 1.0f}) {
        CHECK_EQ(wbOcean(fraction, 0), 0.0f);
        CHECK_EQ(wbOcean(0, fraction), 0.0f);
        CHECK_EQ(wbWaveScale(fraction, 0), 0.0f);
        CHECK_EQ(wbWaveScale(0, fraction), 0.0f);
        CHECK_EQ(wbWaveScale(fraction * 0.5f, fraction * 0.5f), 0.0f);
    }
    CHECK_EQ(wbOcean(0, 0), 1.0f);
    CHECK_EQ(wbWaveScale(0, 0), 1.0f);
    CHECK(wbNormalScale(0, 1) > 0); // lakes keep normal-map ripples
}

TEST(water_bodies_lake_outlet_starts_at_preserved_lake_head) {
    auto map=riverWorld();
    map.lakeDepthField.assign(100,0); map.lakeLevelField.assign(100,0);
    // Erosion lowered the coarse cell to 30; the lake still stands at 40.
    for (int x=3;x<=4;++x) { map.lakeDepthField[40+x]=12; map.lakeLevelField[40+x]=40; }
    world::MacroWorld macro; macro.attach(&map);
    const auto inside=macro.channelOf({3,4}), outlet=macro.channelOf({4,4}), below=macro.channelOf({5,4});
    CHECK(inside && outlet && below);
    if (!inside || !outlet || !below) return;
    CHECK_EQ(inside->surfaceTo,Fixed::fromInt(40*generation::kMetresPerElevationStep));
    CHECK_EQ(outlet->surfaceFrom,inside->surfaceTo);
    CHECK_EQ(outlet->surfaceTo,below->surfaceFrom);
    CHECK(outlet->surfaceFrom>outlet->surfaceTo);
    CHECK(outlet->wet && below->wet);
    CHECK_EQ(outlet->halfWidthEnd,below->halfWidth);
}

TEST(water_bodies_generated_lake_outlets_conserve_accumulated_runoff) {
    int checked=0, channels=0;
    for (int seed : {11,42}) {
        generation::WorldMapParams params; params.width=params.height=64; params.seed=seed;
        const auto map=generation::generateWorldMap(params);
        world::MacroWorld macro; macro.attach(&map);
        for (int y=1;y<63;++y) for (int x=1;x<63;++x) {
            const std::size_t i=y*64+x;
            if (map.lakeDepthField[i]<=0 || map.cells[i].drainOut<0) continue;
            const auto down=core::neighbour({x,y},map.cells[i].drainOut);
            if (!map.inBounds(down)) continue;
            const std::size_t j=down.y*64+down.x;
            if (map.lakeDepthField[j]>0) continue;
            ++checked;
            CHECK(map.riverDischargeField[j]>=map.riverDischargeField[i]);
            if (!map.cells[j].sea && map.cells[j].drainOut>=0)
                CHECK(map.cells[j].drainSize>=map.cells[i].drainSize);
            const auto outlet=macro.channelOf({x,y}), below=macro.channelOf(down);
            if (outlet && below && outlet->wet && below->wet) {
                ++channels;
                CHECK_EQ(outlet->halfWidthEnd,below->halfWidth);
                // Not "never narrower". MacroWorld::widthOf contracts a channel
                // on a steep reach - 1/(1+4*slope), floored at 0.45 - because
                // the same discharge in a faster reach is a narrower one, and
                // it says so where it does it. What conservation means here is
                // above: the discharge and the drained area never fall. The
                // width may, and by exactly as much as that floor allows.
                CHECK(below->halfWidth>=outlet->halfWidth*core::Fixed::ratio(45,100));
            }
        }
    }
    CHECK(checked>0);
    CHECK(channels>0);
}

TEST(water_bodies_ice_never_fills_a_dry_shore_triangle) {
    using namespace world::water_body;
    CHECK_EQ(wbIceAlpha(5,0,0.02f),0.0f);
    CHECK_EQ(wbIceAlpha(-1,1,0.02f),0.0f);
    CHECK_EQ(wbIceAlpha(0,1,0.02f),0.0f);
    CHECK(wbIceAlpha(1,1,0.02f)>0.95f);
    float previous=0;
    for (int i=0;i<=200;++i) {
        const float alpha=wbIceAlpha(1,float(i)/1000,0.02f);
        CHECK(alpha>=previous && alpha-previous<0.01f); previous=alpha;
    }
}


// A river is not a slot cut into the country.
//
// The valley reach used to come off the channel's own width and was clamped to
// about seventy metres for every watercourse on the map, so a reach whose
// neighbours stood two hundred metres higher had to bring the ground down two
// hundred metres inside seventy. Measured on seed 11 before the fix: median
// bank 0.71 m/m over the map's 151 river cross-sections, worst 4.68, and one
// cross-section fell 334 m to eight metres of water and rose 343 m again.
TEST(water_bodies_river_banks_are_valleys_rather_than_walls) {
    generation::WorldMapParams params; params.width=params.height=64; params.seed=11;
    const auto map=generation::generateWorldMap(params);
    world::HeightField field(&map,11);
    world::MacroWorld macro; macro.attach(&map);
    std::vector<double> grades;
    for (int y=2;y<62;++y) for (int x=2;x<62;++x) {
        const auto ch=macro.channelOf({x,y});
        if (!ch||!ch->wet) continue;
        const auto p=world::MacroWorld::pointOn(*ch,Fixed::ratio(1,2));
        const auto w=macro.carve(p,core::kZero,core::kZero);
        double fx=w.flowX.toDouble(), fy=w.flowY.toDouble();
        const double length=std::hypot(fx,fy);
        if (length<0.01) continue;
        // Across the water, which is where a bank is. Along it is the river's
        // own fall downstream and says nothing about how steep its sides are.
        const double nx=-fy/length, ny=fx/length;
        const double half=ch->halfWidth.toDouble();
        double worst=0, previous=0; bool first=true;
        for (int d=-300; d<=300; d+=4) {
            const core::WorldPos at{p.x+Fixed::ratio(std::lround(nx*d*100),100),
                                    p.y+Fixed::ratio(std::lround(ny*d*100),100)};
            const double h=field.heightAt(at).toDouble();
            if (!first && std::abs(double(d))>half+4)
                worst=std::max(worst,std::abs(h-previous)/4.0);
            previous=h; first=false;
        }
        grades.push_back(worst);
    }
    CHECK(grades.size()>50);
    std::sort(grades.begin(),grades.end());
    // Half the banks gentler than a stair, nine in ten off the wall, and - the
    // one that matters most - nothing anywhere that is a cliff to climb.
    //
    // The median is looser than it was and the worst case far tighter, and that
    // is the trade the divide between valleys buys. Valleys used to reach as far
    // as their relief needed and overlap, which gave gentle banks on average
    // (0.45) and left no divide between two waters at different levels, so the
    // ground between them was cut from both sides and the worst sections came
    // out at 3.49. Held to halfway to the next watercourse, the average bank is
    // steeper (0.62) because the valley is narrower, and the tail is gone: 1.83.
    // An average slope is a look; a 3.5 m/m section is a wall across a valley.
    CHECK(grades[grades.size()/2]<0.70);
    CHECK(grades[grades.size()*9/10]<1.30);
    CHECK(grades.back()<2.20);
}
