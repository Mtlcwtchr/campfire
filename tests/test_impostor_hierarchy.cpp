#include "framework.hpp"
#include "engine/render/impostor_hierarchy.hpp"
#include <set>

namespace {
using namespace engine::render;
std::shared_ptr<const ImpostorAtlas> disc(unsigned resolution=8) {
    auto atlas=std::make_shared<ImpostorAtlas>();atlas->resolution=resolution;atlas->side=2;atlas->error=.1;atlas->members=1;
    const auto pixels=std::size_t(resolution)*resolution;
    atlas->colour.resize(21*pixels);atlas->normal.resize(21*pixels);atlas->depth.resize(21*pixels);
    for (unsigned v=0;v<21;++v) for (unsigned y=0;y<resolution;++y) for (unsigned x=0;x<resolution;++x) {
        const auto at=v*pixels+y*resolution+x;
        const double r=std::hypot(x+.5-resolution*.5,y+.5-resolution*.5);const bool solid=r<resolution*.3;
        atlas->colour[at]=solid?ImpostorPixel{100,150,50,255}:ImpostorPixel{};
        atlas->normal[at]={128,128,255,atlas->colour[at][3]};
        // Nearer (larger code) towards the middle: a dome.
        const auto code=std::uint16_t(32768+std::lround((resolution*.3-r)*1000));
        atlas->depth[at]={std::uint8_t(solid?code>>8:128),std::uint8_t(solid?code&255:0),std::uint8_t(v),atlas->colour[at][3]};
    }
    return atlas;
}
engine::camera::ViewState ortho(double scale) {
    engine::camera::ViewState view;view.orthographic=true;view.orthographicScale=scale;
    view.forward=engine::camera::normalized({1,0,-1});view.position={0,0,1000};
    return view;
}
HierarchyNodeInfo plain(ImpostorKey key) {
    HierarchyNodeInfo node;const double cell=double(hierarchyCell(key.level));
    node.bounds={{double(key.x)+cell*.5,double(key.y)+cell*.5,0},cell*.7};
    node.errorMetres=4*cell/32;return node;
}
}

TEST(impostor_hierarchy_keys_tile_their_parent_exactly) {
    const auto parent=hierarchyKey(2,-700,1300);
    CHECK_EQ(parent.x,std::int64_t(-1024));CHECK_EQ(parent.y,std::int64_t(1024));CHECK_EQ(parent.level,2);
    std::set<ImpostorKey> seen;
    for (const auto child:hierarchyChildren(parent)) {
        CHECK_EQ(child.level,1);CHECK(hierarchyParent(child)==parent);CHECK(hierarchyContains(parent,child));
        seen.insert(child);
    }
    CHECK_EQ(seen.size(),std::size_t(16));
    CHECK(!hierarchyContains(parent,hierarchyKey(1,-1025,1300)));
    CHECK(!hierarchyContains(hierarchyKey(1,0,0),hierarchyKey(2,0,0)));
}

TEST(impostor_resample_keeps_coverage_view_ids_and_nearest_depth) {
    const auto source=disc(16);
    const auto coarse=resampleImpostor(*source,4);CHECK(bool(coarse));if (!coarse) return;
    CHECK(coarse->valid());CHECK_EQ(coarse->resolution,4u);CHECK(coarse->error>source->error);
    CHECK_EQ(coarse->colourMips.size(),std::size_t(2));
    for (std::size_t i=0;i<coarse->depth.size();++i) CHECK_EQ(coarse->depth[i][2],std::uint8_t(i/16));
    // The centre texel is fully covered, the corner is empty.
    const auto centre=1*4+1,corner=0;
    CHECK(coarse->colour[centre][3]>200);CHECK_EQ(coarse->colour[corner][3],std::uint8_t(0));
    // Depth is the nearest of the block, never an average of two surfaces.
    unsigned best=0;
    for (unsigned y=4;y<8;++y) for (unsigned x=4;x<8;++x) {
        const auto& d=source->depth[y*16+x];if (d[3]) best=std::max(best,(unsigned(d[0])<<8)|d[1]);
    }
    CHECK_EQ((unsigned(coarse->depth[centre][0])<<8)|coarse->depth[centre][1],best);
    CHECK(!resampleImpostor(*source,3));CHECK(!resampleImpostor(*source,32));
}

TEST(impostor_store_is_bounded_lru_and_remembers_empty_nodes) {
    const auto atlas=disc(8);
    ImpostorStore store(3*(atlas->bytes()+64)+10);
    for (int i=0;i<3;++i) CHECK(store.put({i*128,0,1},7,atlas,double(i)));
    store.touch({0,0,1},10); // the oldest is now the newest
    CHECK(store.put({3*128,0,1},7,atlas,11));
    CHECK(store.find({0,0,1},7)!=nullptr);CHECK(store.find({128,0,1},7)==nullptr);
    CHECK(store.bytes()<=store.budget());
    CHECK(store.find({0,0,1},8)==nullptr); // another revision is another question
    CHECK(store.put({9999,0,2},7,nullptr,12));
    const auto* empty=store.find({9999,0,2},7);CHECK(empty!=nullptr);if (empty) CHECK(!empty->atlas);
    ImpostorStore tiny(10);CHECK(!tiny.put({0,0,1},1,atlas,0));
}

TEST(impostor_cache_adopt_skips_worker_and_keeps_fences) {
    const auto atlas=disc(8);
    ImpostorCache cache(1,1<<20,{},[](auto,auto,auto){return std::shared_ptr<const ImpostorAtlas>{};});
    cache.frame(0,0);
    CHECK_EQ(cache.adopt({0,0,1},5,atlas),0);CHECK(!cache.busy());
    CHECK(cache.slots()[0].state==ImpostorCache::State::Ready);
    cache.uploaded(0,21,true,2);CHECK(cache.resident({0,0,1},5));
    // Touching a current slot neither re-uploads nor resets it.
    CHECK_EQ(cache.adopt({0,0,1},5,atlas),0);CHECK(cache.resident({0,0,1},5));
    // A different key must wait for grace AND the fence of the last draw.
    cache.frame(2,1);CHECK_EQ(cache.adopt({128,0,1},5,atlas),-1);
    cache.frame(2.1,2);CHECK_EQ(cache.adopt({128,0,1},5,atlas),0);CHECK(!cache.resident({0,0,1},5));
    CHECK(cache.adopt({256,0,1},5,nullptr)<0);
}

TEST(impostor_hierarchy_cut_never_nests_and_keeps_children_until_parent_ready) {
    const std::vector<ImpostorKey> roots{hierarchyKey(3,1,1)};
    std::vector<ImpostorKey> resident;
    for (const auto child:hierarchyChildren(roots[0])) resident.push_back(child);
    const auto info=[&](ImpostorKey key) {
        auto node=plain(key);node.resident=std::find(resident.begin(),resident.end(),key)!=resident.end();return node;
    };
    const auto cut=selectImpostorHierarchy(ortho(0.001),roots,info,resident,{});
    CHECK_EQ(cut.draws.size(),std::size_t(16));
    for (const auto& a:cut.draws) for (const auto& b:cut.draws) if (!(a==b)) CHECK(!hierarchyContains(a,b));
    CHECK(std::any_of(cut.wanted.begin(),cut.wanted.end(),[&](const auto& w){return w.first==roots[0];}));
    // Once the parent is resident it replaces all sixteen at once.
    resident.push_back(roots[0]);
    const auto merged=selectImpostorHierarchy(ortho(0.001),roots,info,resident,{});
    CHECK_EQ(merged.draws.size(),std::size_t(1));if (!merged.draws.empty()) CHECK(merged.draws[0]==roots[0]);
}

TEST(impostor_hierarchy_refuses_coarse_nodes_close_up_and_honours_refine_and_budget) {
    const std::vector<ImpostorKey> roots{hierarchyKey(3,1,1)};
    const auto everything=[](ImpostorKey key){auto node=plain(key);node.resident=true;return node;};
    // Near: no level of this hierarchy is within the allowance, and the
    // traversal stops early instead of walking every 128 m node.
    const auto near=selectImpostorHierarchy(ortho(10),roots,everything,{},{});
    CHECK(near.draws.empty());CHECK(near.wanted.empty());CHECK(near.visited<=2u);
    // mustRefine hands the area to the children.
    const auto refine=[&](ImpostorKey key){auto node=everything(key);node.mustRefine=key.level==3;return node;};
    const auto refined=selectImpostorHierarchy(ortho(0.001),roots,refine,{},{});
    CHECK_EQ(refined.draws.size(),std::size_t(16));
    for (const auto& key:refined.draws) CHECK_EQ(key.level,2);
    // A cut that does not fit its budget says so rather than growing.
    HierarchyOptions options;options.maxDraws=4;
    const auto limited=selectImpostorHierarchy(ortho(0.001),roots,refine,{},{},options);
    CHECK_EQ(limited.draws.size(),std::size_t(4));CHECK(limited.budgetLimited);
    // Levels below minLevel belong to the finer path and are never selected.
    options={};options.minLevel=3;
    const auto only=selectImpostorHierarchy(ortho(0.001),roots,refine,{},{},options);
    CHECK(only.draws.empty());
}

TEST(impostor_view_error_is_measured_and_grows_when_views_disagree) {
    const auto consistent=disc(16);
    const double a=measureImpostorViewError(*consistent);
    CHECK(a>=0);CHECK(std::isfinite(a));
    auto broken=std::make_shared<ImpostorAtlas>(*consistent);
    // Ring one (45°) lost the left half of its silhouette: a direction that
    // reprojects from it must be charged for the missing coverage.
    for (unsigned v=8;v<16;++v) for (unsigned y=0;y<16;++y) for (unsigned x=0;x<8;++x) {
        const auto at=v*256+y*16+x;broken->colour[at][3]=0;broken->depth[at][3]=0;
    }
    const double b=measureImpostorViewError(*broken);
    CHECK(b>a+consistent->side/16);
    ImpostorAtlas invalid;CHECK(measureImpostorViewError(invalid)<0);
}

TEST(impostor_parent_error_is_measured_against_children_and_monotone) {
    const auto leafAtlas=disc(16);std::vector<ImpostorPlacement> children;
    for (int i=0;i<4;++i) children.push_back({leafAtlas,{double(i%2)*3,double(i/2)*3,0},1.5,i*.7,1});
    ImpostorBakeOptions options;options.resolution=16;
    const auto parent=withMeasuredError(bakeRecursiveImpostor(children,options),children);
    CHECK(bool(parent));if (!parent) return;
    CHECK(parent->viewError>=0);
    for (const auto& child:children) CHECK(impostorTotalError(*parent)>=impostorTotalError(*child.atlas)*child.scale);
    // A second level over the first stays monotone.
    std::vector<ImpostorPlacement> level{{parent,{0,0,0}},{parent,{12,0,0}}};
    const auto grand=withMeasuredError(bakeRecursiveImpostor(level,options),level);
    CHECK(bool(grand));if (grand) CHECK(impostorTotalError(*grand)>=impostorTotalError(*parent));
    CHECK(!withMeasuredError(nullptr,children));
}

