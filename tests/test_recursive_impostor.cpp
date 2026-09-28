#include "framework.hpp"
#include "engine/render/impostor_cache.hpp"
#include <chrono>
#include <thread>

namespace {
using namespace engine::render;
std::shared_ptr<const ImpostorAtlas> leaf(ImpostorPixel colour={200,30,10,255}) {
    auto atlas=std::make_shared<ImpostorAtlas>();atlas->resolution=8;atlas->side=2;atlas->error=.1;atlas->members=1;
    atlas->colour.resize(21*64);atlas->normal.resize(21*64);atlas->depth.resize(21*64);
    for (unsigned v=0;v<21;++v) for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) {
        const auto at=v*64+y*8+x;const bool solid=std::hypot(double(x)-3.5,double(y)-3.5)<2.5;
        atlas->colour[at]=solid?colour:ImpostorPixel{};
        const auto n=hemisphereBasis(v)->eye;
        for (int k=0;k<3;++k) atlas->normal[at][k]=std::uint8_t((n[k]*.5+.5)*255);
        atlas->depth[at]={128,0,std::uint8_t(v),std::uint8_t(solid?255:0)};
    }
    return atlas;
}
bool finish(ImpostorCache& cache) {
    for (int i=0;i<2000;++i) {cache.poll();if (!cache.busy()) return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    return false;
}
}

TEST(recursive_impostor_preserves_members_and_accumulates_error) {
    const auto source=leaf();std::vector<ImpostorPlacement> members;
    for (int i=0;i<8;++i) members.push_back({source,{double(i%4)*3,double(i/4)*3,0},1,0,1});
    ImpostorBakeOptions options;options.resolution=16;options.leafMembers=2;
    const auto a=bakeRecursiveImpostor(members,options);CHECK(bool(a));if (!a) return;
    CHECK(a->valid());CHECK_EQ(a->members,8u);CHECK(a->levels>=2);CHECK(a->error>source->error);
    CHECK_EQ(a->colourMips.size(),std::size_t(4));
    for (auto& member:members) {member.centre[0]-=100;member.centre[1]+=30;member.centre[2]+=8;}
    const auto b=bakeRecursiveImpostor(members,options);CHECK(bool(b));if (!b) return;
    CHECK(a->colour==b->colour);CHECK(a->depth==b->depth);
    CHECK(std::abs(b->centre[0]-a->centre[0]+100)<1e-9);
    CHECK(std::abs(b->centre[2]-a->centre[2]-8)<1e-9);
}

TEST(recursive_impostor_depth_occlusion_is_not_painter_order) {
    const auto red=leaf(),green=leaf({20,240,20,255});
    std::vector<ImpostorPlacement> members{{red,{0,-.4,0}},{green,{0,.4,0}}};
    ImpostorBakeOptions options;options.resolution=32;
    const auto a=bakeRecursiveImpostor(members,options);
    std::reverse(members.begin(),members.end());const auto b=bakeRecursiveImpostor(members,options);
    CHECK(a && b);if (!a || !b) return;
    const auto at=16*32+16; // view zero looks from +Y
    CHECK(a->colour[at][1]>200);CHECK(a->colour==b->colour);CHECK(a->depth==b->depth);
    members[0].tint=.5;const auto tinted=bakeRecursiveImpostor(members,options);CHECK(bool(tinted));
    if (tinted) CHECK(tinted->colour[at][1]<a->colour[at][1]);
}

TEST(recursive_impostor_rejects_missing_members_instead_of_partial_proxy) {
    std::vector<ImpostorPlacement> members{{leaf(),{0,0,0}},{nullptr,{1,0,0}}};
    CHECK(!bakeRecursiveImpostor(members));
    members[1].atlas=members[0].atlas;
    ImpostorBakeOptions options;options.maxMembers=1;CHECK(!bakeRecursiveImpostor(members,options));
    std::atomic_bool cancelled=true;CHECK(!bakeRecursiveImpostor(members,{},&cancelled));
    members[1].scale=-1;CHECK(!bakeRecursiveImpostor(members));
}

TEST(impostor_cache_publishes_only_complete_uploads_and_respects_fences) {
    const auto source=leaf();const std::vector<ImpostorPlacement> members{{source,{0,0,0}}};
    const auto baker=[source](auto,auto,auto){return source;};
    ImpostorCache cache(1,1<<20,{},baker);cache.frame(0,0);
    CHECK_EQ(cache.request({0,0},1,members),0);cache.poll();CHECK(finish(cache));
    CHECK(!cache.resident({0,0},1));CHECK_EQ(cache.uploadCandidate(),0);
    cache.uploaded(0,20,true,3);CHECK(!cache.resident({0,0},1));CHECK_EQ(cache.uploadCandidate(),-1);
    cache.frame(.1,3);cache.uploaded(0,1,true,4);CHECK(cache.resident({0,0},1));
    cache.protect(0,10);cache.frame(2,9);CHECK_EQ(cache.request({32,0},1,members),-1);
    cache.frame(2.1,10);CHECK_EQ(cache.request({32,0},1,members),0);CHECK(!cache.resident({0,0},1));
}

TEST(impostor_cache_discards_stale_worker_and_never_replaces_new_revision) {
    auto release=std::make_shared<std::atomic_bool>(false);const auto source=leaf();
    ImpostorCache cache(1,1<<20,{},[release,source](auto,auto,const std::atomic_bool* cancel) {
        while (!release->load() && !cancel->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));return source;
    });
    const std::vector<ImpostorPlacement> members{{source,{0,0,0}}};
    cache.frame(0,0);cache.request({0,0},1,members);cache.poll();
    cache.request({0,0},2,members);release->store(true);CHECK(finish(cache));
    CHECK_EQ(cache.staleBakes(),std::uint64_t(1));CHECK(!cache.resident({0,0},1));
    CHECK_EQ(cache.slots()[0].revision,std::uint64_t(2));
    cache.uploaded(0,21,true);CHECK(cache.resident({0,0},2));
}

TEST(impostor_cache_failure_budget_and_invalidation_keep_fallback_owned_by_children) {
    const auto source=leaf();const std::vector<ImpostorPlacement> members{{source,{0,0,0}}};
    ImpostorCache cache(1,1,{},[source](auto,auto,auto){return source;});
    cache.frame(0,0);cache.request({-32,0},1,members);cache.poll();CHECK(finish(cache));
    CHECK(cache.slots()[0].state==ImpostorCache::State::Failed);CHECK_EQ(cache.bytes(),std::size_t(0));
    cache.request({-32,0},1,members);cache.poll();CHECK(!cache.busy());
    cache.invalidate({-32,0});CHECK(cache.slots()[0].state==ImpostorCache::State::Empty);
}
