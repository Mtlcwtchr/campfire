#include "framework.hpp"
#include "engine/render/geometry/instances.hpp"
#include "game/world/ring_mesh.hpp"
#include <array>
#include <memory>
#include <span>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

TEST(instance_arena_grass_budget_copies_elements_not_squared_byte_counts) {
    engine::InstanceArena arena;
    const std::vector<world::PageGrassRoot> roots(world::kGrassCandidateBudget+17);
    const auto selected=std::span(roots).first(world::kGrassCandidateBudget);
    CHECK_EQ(arena.add(selected),0u);
    CHECK_EQ(arena.instances(),world::kGrassCandidateBudget);
    CHECK_EQ(arena.bytes(),world::kGrassCandidateBudget*sizeof(world::PageGrassRoot));
    CHECK_EQ(sizeof(world::PageGrassRoot),32u); // VG1 adds the packed run word
    CHECK_EQ(arena.bytes(),2097152u);
    // This is CPU staging only: the regression does not need a GPU or a window.
    CHECK_EQ(arena.capacity(),0u);
    arena.begin();
    CHECK_EQ(arena.bytes(),0u);CHECK_EQ(arena.instances(),0u);
    CHECK_EQ(arena.add(selected.first(1)),0u);
    CHECK_EQ(arena.bytes(),sizeof(world::PageGrassRoot));
    CHECK_EQ(arena.instances(),1u);
}

TEST(instance_arena_typed_batches_preserve_offsets_alignment_and_empty_ranges) {
    engine::InstanceArena arena;
    const std::array<std::uint8_t,3> prefix{1,2,3};
    const std::array<world::PageGrassRoot,5> roots{};
    CHECK_EQ(arena.add(std::span(prefix)),0u);
    const auto at=arena.add(std::span(roots).first(2));
    CHECK_EQ(at,sizeof(world::PageGrassRoot));
    CHECK_EQ(arena.bytes(),3*sizeof(world::PageGrassRoot));
    CHECK_EQ(arena.instances(),5u); // three prefix bytes and two grass instances
    CHECK_EQ(arena.add(std::span(roots).subspan(2)),3*sizeof(world::PageGrassRoot));
    CHECK_EQ(arena.bytes(),6*sizeof(world::PageGrassRoot));
    CHECK_EQ(arena.instances(),8u);
    CHECK_EQ(arena.add(std::span(roots).first(0)),arena.bytes());
    CHECK_EQ(arena.instances(),8u);
}

#if defined(__unix__) || defined(__APPLE__)
TEST(instance_arena_grass_source_can_end_exactly_at_a_guard_page) {
    const auto pageSize=sysconf(_SC_PAGESIZE);
    CHECK(pageSize>0);
    if (pageSize<=0) return;
    const auto page=static_cast<std::size_t>(pageSize);
    void* address=mmap(nullptr,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    CHECK(address!=MAP_FAILED);
    if (address==MAP_FAILED) return;
    struct Mapping {
        void* address;
        std::size_t bytes;
        ~Mapping() { munmap(address,bytes); }
    } mapping{address,2*page};
    auto* boundary=static_cast<std::byte*>(address)+page;
    const int protectedPage=mprotect(boundary,page,PROT_NONE);
    CHECK_EQ(protectedPage,0);
    if (protectedPage!=0) return;
    constexpr std::size_t count=7;
    auto* roots=reinterpret_cast<world::PageGrassRoot*>(boundary-count*sizeof(world::PageGrassRoot));
    for (std::size_t i=0;i<count;++i) std::construct_at(roots+i,world::PageGrassRoot{});
    engine::InstanceArena arena;
    // The old count*sizeof(root) count argument crosses PROT_NONE in memcpy.
    CHECK_EQ(arena.add(std::span<const world::PageGrassRoot>(roots,count)),0u);
    CHECK_EQ(arena.instances(),count);
    CHECK_EQ(arena.bytes(),count*sizeof(world::PageGrassRoot));
}
#endif

