#include "framework.hpp"
#include <limits>
#include "engine/geometry/smart_mesh.hpp"
#include "engine/geometry/smart_sprite_mesh.hpp"
#include "engine/geometry/smart_terrain_mesh.hpp"
#if ASR_SMART_RENDER_TESTS
#include "engine/pipeline/runner.hpp"
#include "engine/render/mesh_renderer.hpp"
#include "game/render/world_materials.hpp"
#endif

TEST(smart_mesh_selects_complete_family_or_resident_parent) {
    engine::SmartMesh mesh({{{0,6},4,{1,2}},{{6,3},0,{}},{{9,3},0,{}}},12);
    CHECK_EQ(mesh.select(1,5).nodes,std::vector<std::uint32_t>{0});
    const std::vector<std::uint32_t> fine{1,2};
    CHECK_EQ(mesh.select(1,1).nodes,fine);
    const std::uint8_t partial[]{1,1,0}, missing[]{0,1,0}, children[]{0,1,1};
    CHECK_EQ(mesh.select(1,1,partial).nodes,std::vector<std::uint32_t>{0});
    CHECK(!mesh.select(1,1,missing).complete);
    CHECK(mesh.select(1,1,missing).nodes.empty());
    CHECK_EQ(mesh.select(1,1,children).nodes,fine);
}

TEST(smart_mesh_rejects_cycles_bad_ranges_and_nonmonotonic_error) {
    const auto rejects=[](std::vector<engine::SmartMesh::Node> nodes) {
        try { engine::SmartMesh mesh(std::move(nodes),12); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    };
    CHECK(rejects({{{0,6},1,{1}},{{6,6},1,{0}}}));
    CHECK(rejects({{{0,6},1,{1}},{{6,6},2,{}}}));
    CHECK(rejects({{{11,3},0,{}}}));
    CHECK(rejects({{{0,6},1,{1,1}},{{6,6},0,{}}}));
}

TEST(smart_mesh_generated_chain_is_selected_by_shape_error) {
    const engine::IndexRange ranges[]{{0,12},{12,6},{18,3}};
    const float errors[]{0,0.1f,0.4f};
    const auto mesh=engine::SmartMesh::chain(ranges,errors,21,
        engine::SmartMesh::ErrorMetric::EstimatedSurfaceDistance);
    CHECK_EQ(mesh.selectChain(1,1),std::uint32_t(2));
    CHECK_EQ(mesh.selectChain(5,1),std::uint32_t(1));
    CHECK_EQ(mesh.selectChain(20,1),std::uint32_t(0));
    CHECK(mesh.errorMetric()==engine::SmartMesh::ErrorMetric::EstimatedSurfaceDistance);
}

TEST(smart_terrain_retopology_preserves_actual_parent_surface) {
    const auto plane=[](double x,double y){return std::array{float(x*0.2+y*0.1),-10.0f};};
    const auto parent=engine::makeAdaptiveMesh(8,1,0.1,plane);
    const auto ridge=[](double x,double y){return std::array{float(x*0.2+y*0.1+(x==4?4:0)),-10.0f};};
    const auto mesh=engine::makeAdaptiveMesh(8,1,0.1,ridge,
        [&](double x,double y){return parent->sample(x,y);});
    CHECK(mesh->surfaceIndices>parent->surfaceIndices);
    CHECK(std::abs(mesh->sample(4,4)[0]-5.2f)<0.0001f);
    for (const auto& vertex:mesh->vertices) {
        const auto expected=parent->sample(vertex.x*mesh->step,vertex.y*mesh->step);
        CHECK(std::abs(vertex.parentBed-expected[0])<0.0001f);
    }
    CHECK(std::abs(parent->sample(4,4)[0]-1.2f)<0.0001f);
}

TEST(smart_sprite_mesh_keeps_topology_and_wraps_impostor_views) {
    engine::SmartSpriteMesh sprite;
    const auto vertices=sprite.vertices();
    CHECK_EQ(vertices[0].corner[0],-1.0f);
    CHECK_EQ(vertices[3].corner[1],1.0f);
    CHECK_EQ(engine::SmartSpriteMesh::indices.size(),std::size_t(6));
    sprite.views=8;
    CHECK_EQ(sprite.viewIndex(0,0),0u);
    CHECK_EQ(sprite.viewIndex(6.283185307179586,0),0u);
    CHECK_EQ(sprite.viewIndex(-0.7853981633974483,0),7u);
}

#if ASR_SMART_RENDER_TESTS
TEST(render_work_counts_cpu_indirect_plans_but_not_gpu_unknowns) {
    engine::DrawItem item;
    item.vertexCount=6;item.instances=5;
    CHECK_EQ(item.triangleCount().value(),std::uint64_t(10));
    // Only pointer presence is tested, no GPU buffer is created or touched.
    int sentinel=0;
    item.index=reinterpret_cast<SDL_GPUBuffer*>(&sentinel);
    item.indexCount=12;
    CHECK_EQ(item.triangleCount().value(),std::uint64_t(20));
    item.instances=0;
    CHECK_EQ(item.triangleCount().value(),std::uint64_t(0));
    item.indexCount=300000;item.instances=100000;
    CHECK_EQ(item.triangleCount().value(),std::uint64_t(10000000000ULL));
    item.indirectFromArena=true;
    CHECK(!item.triangleCount().has_value());
    item.indirectTriangles=123456;
    CHECK_EQ(item.triangleCount().value(),std::uint64_t(123456));
    item.indirectTriangles=0;
    CHECK(item.triangleCount().has_value());
    CHECK_EQ(item.triangleCount().value(),std::uint64_t(0));
    item.indirectTriangles.reset();item.indirectFromArena=false;
    item.indirect=reinterpret_cast<SDL_GPUBuffer*>(&sentinel);
    CHECK(!item.triangleCount().has_value());
}

TEST(frame_cpu_time_includes_pipeline_work_and_excludes_present_wait) {
    engine::Runner::FrameTiming timing;
    timing.fixed=1;timing.acquire=50;timing.setup=2;
    timing.pipelines=8;timing.submit=4;
    CHECK_EQ(timing.cpu(),15.0);
}

TEST(mesh_material_owns_shader_settings_and_layout_is_separate) {
    auto material=game::materials::sprite(true);
    engine::VertexLayout layout;
    layout.buffers.push_back({0,16,SDL_GPU_VERTEXINPUTRATE_VERTEX,0});
    const auto pipeline=material->pipeline(layout);
    CHECK_EQ(std::string(pipeline.shaderFile),std::string("sprite.hlsl"));
    CHECK_EQ(std::string(pipeline.fragmentEntry),std::string("SpriteSoftPS"));
    CHECK(pipeline.blend);CHECK(!pipeline.depthWrite);CHECK(pipeline.depthTest);
    CHECK_EQ(pipeline.buffers.front().pitch,std::uint32_t(16));
    auto independent=*material;
    independent.parameters[0]=0.25f;
    CHECK_EQ(material->parameters[0],0.0f);
    independent.parameters[0]=std::numeric_limits<float>::infinity();
    CHECK(!independent.valid());
    CHECK(game::materials::terrain(true)->valid());
    CHECK(game::materials::sceneModels()->valid());
}

TEST(mesh_renderer_requires_geometry_and_compiled_material) {
    engine::MeshRenderer renderer;
    engine::DrawQueue queue;
    bool rejected=false;
    try { renderer.submit(queue); }
    catch (const std::logic_error&) { rejected=true; }
    CHECK(rejected);CHECK_EQ(queue.size(),std::size_t(0));
    engine::DrawItem item;item.own[0]=0.75f;
    rejected=false;
    try { renderer.material.apply(item); }
    catch (const std::logic_error&) { rejected=true; }
    CHECK(rejected);CHECK_EQ(item.own[0],0.75f);
}
#endif
