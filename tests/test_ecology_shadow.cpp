#include "framework.hpp"
#include "game/world/ecology.hpp"
#include "game/world/scene_scatter.hpp"
#include "game/world/shadow_field.hpp"
#include <cmath>

TEST(ecology_potential_survives_clearing_and_succession_is_slow) {
    using namespace world::ecology;
    Physical p;
    auto cell=initial(p,1);
    const float potential=cell.potentialForest;
    CHECK(potential>0.3f);
    cell.canopy=0;
    cell.disturbance=1;
    advance(cell,p,365);
    CHECK_EQ(cell.canopy,0.0f);
    CHECK_EQ(cell.potentialForest,potential);
    cell.disturbance=0;
    advance(cell,p,365);
    CHECK(cell.canopy>0);
    CHECK(cell.canopy<potential*0.2f);
}
TEST(ecology_physical_constraints_and_habitat_masks) {
    using namespace world::ecology;
    Physical p;
    p.moisture=0.05f;p.naturalFertility=1;
    CHECK_EQ(initial(p,1).potentialForest,0.0f);
    p.moisture=0.9f;p.salinity=1;
    CHECK_EQ(initial(p,1).agriculture,0.0f);
    p.salinity=0;p.slope=1.8f;
    CHECK(initial(p,1).masks&Cliff);
    CHECK_EQ(initial(p,1).canopy,0.0f);
    p.slope=0;p.drainage=0;p.flood=1;
    CHECK(initial(p,0).masks&Wetland);
    CHECK(initial(p,0).masks&Waterfowl);
    p.temperature=-20;
    CHECK_EQ(initial(p,1).biome,Biome::PolarDesert);
}
TEST(ecology_delta_snapshots_are_immutable_and_region_local) {
    using namespace world::ecology;
    Store store;
    const auto before=store.read();
    store.set(65,65,{});
    const auto first=store.read();
    CHECK_EQ(before->revision,0u);
    CHECK_EQ(first->region(0,0),1u);
    CHECK_EQ(first->region(128,0),0u);
    store.remove(42,130,5);
    const auto second=store.read();
    CHECK_EQ(first->region(128,0),0u);
    CHECK_EQ(second->region(128,0),2u);
    CHECK(second->removed.contains(42));
    store.remove(42,130,5);
    CHECK_EQ(store.read()->revision,2u);
    CHECK_EQ(key(-1,-65),(Key{-1,-2}));
}
TEST(ecology_scatter_obeys_current_canopy_not_legacy_forest_flag) {
    using namespace world::decor;
    Site site{100,0,0,1,0};
    site.hasEcology=true;
    site.ecology.canopy=0;
    site.ecology.fertility=1;
    const LandTest land=[](int,int){return true;};
    const auto empty=scatter(42,ScatterBounds{0,0,512,512},1024,1024,land,[&](double,double){return site;});
    CHECK_EQ(empty.populations[0]+empty.populations[1],0u);
    site.ecology.canopy=1;
    const auto full=scatter(42,ScatterBounds{0,0,512,512},1024,1024,land,[&](double,double){return site;});
    CHECK(full.populations[0]+full.populations[1]>100);
    CHECK_EQ(full.objects,scatter(42,ScatterBounds{0,0,512,512},1024,1024,land,[&](double,double){return site;}).objects);
}
TEST(shadow_projection_matches_directional_light_and_translation) {
    using namespace world::shadow;
    Tile tile;
    tile.basis=Basis({-1,0,1});tile.span=64;
    tile.ellipsoid({0,0,10},{2,2,2},-1);
    CHECK(tile.visibility({10,0,0})<0.01f);
    CHECK(tile.visibility({-10,0,0})>0.99f);
    CHECK(tile.visibility({-10,0,20})>0.99f);
    Tile shifted;
    shifted.basis=tile.basis;shifted.span=tile.span;shifted.origin={100000,200000,30};
    shifted.ellipsoid(shifted.origin+Vec{0,0,10},{2,2,2},-1);
    CHECK_EQ(shifted.visibility(shifted.origin+Vec{10,0,0}),tile.visibility({10,0,0}));
}
TEST(shadow_crown_transmits_but_cannot_erase_opaque_occluder) {
    using namespace world::shadow;
    Tile tile;
    tile.basis=Basis({0,0,1});tile.span=32;
    tile.ellipsoid({0,0,10},{3,3,3},0.2f);
    const float transmission=tile.visibility({0,0,0});
    CHECK(transmission>0.1f && transmission<0.9f);
    tile.ellipsoid({0,0,5},{1,1,2},-1);
    CHECK_EQ(tile.visibility({0,0,0}),0.0f);
    CHECK_EQ(tile.visibility({0,0,20}),1.0f);
}
TEST(shadow_heightfield_triangle_and_empty_tile_are_defined) {
    using namespace world::shadow;
    Tile tile;
    tile.basis=Basis({0,0,1});tile.span=16;
    CHECK_EQ(tile.visibility({0,0,0}),1.0f);
    tile.triangle({-5,-5,2},{5,-5,2},{0,5,2});
    CHECK_EQ(tile.visibility({0,0,0}),0.0f);
    CHECK_EQ(tile.visibility({0,0,3}),1.0f);
    CHECK_EQ(tile.visibility({100,100,0}),1.0f);
}

