#include "framework.hpp"
#include "engine/camera/camera.hpp"
#include "engine/render/representation_selector.hpp"
#include "engine/render/impostor_depth.hpp"
#include "engine/render/hemisphere_impostor.hpp"
#include "engine/terrain/range_field.hpp"
#include "engine/geometry/cluster_dag.hpp"
#include <array>
#include <limits>
#include <stdexcept>

namespace {
engine::camera::ViewState view() {
    engine::camera::ViewState v;
    v.position = {0, 0, 100}; v.forward = {0, 0, -1}; v.viewportHeight = 1000;
    v.fovY = std::numbers::pi / 2; v.quality.hysteresis = 0;
    return v;
}
using namespace engine::render;
std::array<RepresentationCandidate, 2> choices() {
    std::array<RepresentationCandidate, 2> c;
    c[0].estimatedCost = 100;
    c[1].kind = RepresentationKind::Impostor;
    c[1].estimatedCost = 10; c[1].errorMetres = 0.1;
    return c;
}
}
TEST(engine_representation_same_projection_same_choice) {
    auto a = view(), b = a;
    const RepresentationBounds bounds{{0, 0, 0}, 0};
    b.position[2] = 200; b.fovY = 2*std::atan(0.5);
    CHECK(std::abs(a.pixelsPerMetre(bounds.centre) - b.pixelsPerMetre(bounds.centre)) < 1e-8);
    const auto candidates = choices();
    CHECK_EQ(selectRepresentation(a, bounds, candidates).candidate, selectRepresentation(b, bounds, candidates).candidate);
    b.viewportHeight *= 8;
    CHECK_EQ(selectRepresentation(b, bounds, candidates).candidate, std::size_t(0));
}
TEST(engine_representation_cost_crossover_never_replaces_cheaper_geometry) {
    auto c = choices(); c[0].estimatedCost = 2;
    CHECK_EQ(selectRepresentation(view(), {{0, 0, 0}, 1}, c).candidate, std::size_t(0));
    c[0].estimatedCost = 100;
    CHECK_EQ(selectRepresentation(view(), {{0, 0, 0}, 1}, c).candidate, std::size_t(1));
    c[1].resident = false;
    const auto missing = selectRepresentation(view(), {{0, 0, 0}, 1}, c);
    CHECK_EQ(missing.candidate, std::size_t(0)); CHECK(missing.prefetchChildren);
}
TEST(engine_representation_rejects_angular_and_lateral_parallax_error) {
    auto c = choices(); auto v = view();
    c[1].bakedDirection = {1, 0, 0}; c[1].angularErrorMetres = 1;
    CHECK_EQ(selectRepresentation(v, {{0, 0, 0}, 1}, c).candidate, std::size_t(0));
    c[1].bakedDirection = {0, 0, 1}; c[1].residualDepth = 30;
    CHECK_EQ(selectRepresentation(v, {{0, 0, 0}, 1}, c).candidate, std::size_t(1));
    v.velocity = {100, 0, 0};
    CHECK_EQ(selectRepresentation(v, {{0, 0, 0}, 1}, c).candidate, std::size_t(0));
    v.velocity = {0, 0, 100};
    CHECK_EQ(selectRepresentation(v, {{0, 0, 0}, 1}, c).candidate, std::size_t(1));
}
TEST(engine_representation_profiles_and_hysteresis_share_the_selector) {
    auto c = choices(); c[1].errorMetres = 0.3;
    auto v = view(); v.quality = engine::camera::ViewQualityProfile::person();
    CHECK_EQ(selectRepresentation(v, {{0, 0, 0}, 0}, c).candidate, std::size_t(0));
    v.quality = engine::camera::ViewQualityProfile::orbital();
    CHECK_EQ(selectRepresentation(v, {{0, 0, 0}, 0}, c).candidate, std::size_t(1));
    c[1].errorMetres = 0.41; // 2.05 px, within the previous impostor's exit band
    CHECK_EQ(selectRepresentation(v, {{0, 0, 0}, 0}, c, 1).candidate, std::size_t(1));
    CHECK_EQ(selectRepresentation(v, {{0, 0, 0}, 0}, c, 0).candidate, std::size_t(0));
}
TEST(engine_representation_projection_is_conservative_near_camera_and_handles_bad_input) {
    auto v = view(); const auto c = choices();
    CHECK(v.pixelsPerMetre({0, 0, 90}, 9) > v.pixelsPerMetre({0, 0, 90}, 0));
    CHECK(selectRepresentation(v, {{0, 0, 200}, 1}, c).kind == RepresentationKind::Cull);
    v.viewportHeight = 0;
    CHECK(selectRepresentation(v, {{0, 0, 0}, 1}, c).qualityExceeded);
    v = view(); v.orthographic = true; v.orthographicScale = 3;
    CHECK_EQ(v.pixelsPerMetre({0, 0, -1000}), v.pixelsPerMetre({0, 0, 0}));
}
TEST(engine_representation_hierarchy_falls_back_to_children_without_holes) {
    std::vector<RepresentationNode> nodes(3);
    nodes[0].bounds = {{0, 0, 0}, 20}; nodes[0].children = {1, 2};
    auto c = choices(); c[1].kind = RepresentationKind::Aggregate; c[1].errorMetres = 40;
    nodes[0].representations = {c[1]};
    nodes[1].representations = {c[0]}; nodes[2].representations = {c[0]};
    const std::array<std::uint32_t, 1> roots{0};
    auto cut = selectRepresentations(view(), nodes, roots);
    CHECK(cut.complete); CHECK_EQ(cut.draws.size(), std::size_t(2));
    CHECK_EQ(cut.draws[0].node, std::uint32_t(1)); CHECK_EQ(cut.draws[1].node, std::uint32_t(2));
    nodes[0].representations[0].errorMetres = 0;
    cut = selectRepresentations(view(), nodes, roots);
    CHECK_EQ(cut.draws.size(), std::size_t(1)); CHECK_EQ(cut.draws[0].node, std::uint32_t(0));
    nodes[0].representations.clear(); nodes[0].children = {0};
    bool rejected = false;
    try { (void)selectRepresentations(view(), nodes, roots); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}
TEST(engine_representation_prefetches_before_the_error_threshold) {
    auto v = view(); auto c = choices(); c[1].errorMetres = 0.3;
    v.velocity = {0, 0, -200};
    const auto selected = selectRepresentation(v, {{0, 0, 0}, 1}, c);
    CHECK_EQ(selected.candidate, std::size_t(1)); CHECK(selected.prefetchChildren);
    CHECK(v.predictedPosition()[2] < v.position[2]); CHECK(selected.residencySeconds > 0);
}
TEST(engine_camera_state_matches_real_projection) {
    engine::camera::Camera camera;
    camera.setMode(engine::camera::Camera::Mode::Orbit);
    camera.viewportWidth = 1600; camera.viewportHeight = 900;
    const auto state = camera.viewState({10, 20, 0});
    float matrix[16]; camera.viewProjection(matrix, 0, 1);
    const double focal = std::hypot(std::hypot(double(matrix[4]), double(matrix[5])), double(matrix[6])) * 450;
    CHECK(std::abs(state.projectionScale() - focal) < 0.001);
    CHECK_EQ(state.velocity[1], 20.0);
    CHECK(std::abs(engine::camera::dot(state.forward, state.forward) - 1) < 1e-8);
}
TEST(engine_terrain_and_virtual_geometry_work_without_game_or_content) {
    engine::terrain::PlateField plates(11);
    engine::terrain::RangeField ranges(11, plates);
    CHECK(std::isfinite(ranges.at(-10000, 20000).metres));
    const std::array<float, 12> p{0,0,0, 1,0,0, 1,1,0, 0,1,0};
    const std::array<std::uint32_t, 6> i{0,1,2, 0,2,3};
    const auto dag = engine::geometry::buildClusterDag(p, i);
    CHECK(!dag.clusters.empty()); CHECK(!engine::geometry::cutAt(dag, 0).empty());
}

TEST(engine_impostor_depth_preserves_signed_range_and_ray_projection) {
    CHECK(std::abs(impostorDepth(0,0,4)+2)<1e-12);
    CHECK(std::abs(impostorDepth(255,255,4)-2)<1e-12);
    for (unsigned view=0;view<8;++view) {
        const double angle=view*std::numbers::pi/4,c=std::cos(angle),s=std::sin(angle);
        const engine::camera::Vec3 ray{0.3*c-s,0.3*s+c,0.2};
        const auto hit=intersectImpostorDepth({0,0,3},ray,0.5,4,6,view);
        CHECK(bool(hit));if (!hit) continue;
        CHECK(std::abs(hit->uv[0]-0.5375)<1e-10);
        CHECK(std::abs(hit->uv[1]-(1-3.1/6))<1e-10);
        CHECK(std::abs(hit->position[0]-ray[0]*0.5)<1e-10);
        CHECK(std::abs(hit->position[1]-ray[1]*0.5)<1e-10);
    }
}
TEST(engine_impostor_depth_refuses_missing_side_view_information) {
    CHECK(!intersectImpostorDepth({0,0,3},{0,0,1},0,4,6,0));
    CHECK(!intersectImpostorDepth({0,0,3},{0,-1,0},0,4,6,0));
    CHECK(!intersectImpostorDepth({0,0,3},{0,1,0},0,0,6,0));
    CHECK(!intersectImpostorDepth({0,0,3},{0,1,0},0,4,6,8));
}

TEST(engine_hemisphere_frames_and_projection_cover_upper_views) {
    using engine::camera::dot;
    for (unsigned view=0;view<kHemisphereViews;++view) {
        const auto basis=hemisphereBasis(view);CHECK(bool(basis));if (!basis) return;
        CHECK(std::abs(dot(basis->eye,basis->eye)-1)<1e-12);
        CHECK(std::abs(dot(basis->right,basis->up))<1e-12);
        CHECK(std::abs(dot(basis->eye,basis->up))<1e-12);
        engine::camera::Vec3 ray{};
        for (int i=0;i<3;++i) ray[i]=basis->eye[i]+0.3*basis->right[i]+0.2*basis->up[i];
        const auto hit=intersectHemisphereDepth({0,0,0},ray,0.5,4,view);
        CHECK(bool(hit));if (!hit) return;
        CHECK(std::abs(hit->uv[0]-0.5375)<1e-12);
        CHECK(std::abs(hit->uv[1]-0.475)<1e-12);
        const auto selected=selectHemisphere(basis->eye);CHECK(bool(selected));if (!selected) return;
        double own=0;for (unsigned i=0;i<4;++i) if (selected->views[i]==view) own+=selected->weights[i];
        CHECK(own>0.99999);
    }
    CHECK(!hemisphereBasis(21));
    CHECK(!selectHemisphere({0,0,-1}));CHECK(!selectHemisphere({0,0,0}));
}

TEST(engine_hemisphere_weights_are_continuous_at_rings_wrap_and_pole) {
    const auto direction=[](double az,double elevation) {
        const double e=elevation*std::numbers::pi/180;
        return engine::camera::Vec3{std::cos(az)*std::cos(e),std::sin(az)*std::cos(e),std::sin(e)};
    };
    const auto weights=[](const HemisphereSelection& s) {
        std::array<double,kHemisphereViews> out{};
        for (unsigned i=0;i<4;++i) out[s.views[i]]+=s.weights[i];return out;
    };
    for (double e:{0.0,20.0,44.99999,45.00001,69.99999,70.00001,89.99999,90.0}) {
        const auto s=selectHemisphere(direction(1.7,e));CHECK(bool(s));if (!s) return;
        double total=0;for (auto w:s->weights) {CHECK(w>=0);total+=w;}
        CHECK(std::abs(total-1)<1e-10);CHECK(s->maxAngle<1.1);
        const auto rotated=selectHemisphere(direction(2.7,e),1.0);CHECK(bool(rotated));if (!rotated) return;
        const auto a=weights(*s),b=weights(*rotated);
        for (unsigned i=0;i<kHemisphereViews;++i) CHECK(std::abs(a[i]-b[i])<1e-8);
    }
    for (double e:{45.0,70.0,89.9999}) {
        const auto a=weights(*selectHemisphere(direction(1.7,e-0.00001)));
        const auto b=weights(*selectHemisphere(direction(1.7,e+0.00001)));
        for (unsigned i=0;i<kHemisphereViews;++i) CHECK(std::abs(a[i]-b[i])<1e-5);
    }
    const auto a=weights(*selectHemisphere(direction(std::numbers::pi/2-1e-8,30)));
    const auto b=weights(*selectHemisphere(direction(std::numbers::pi/2+1e-8,30)));
    for (unsigned i=0;i<kHemisphereViews;++i) CHECK(std::abs(a[i]-b[i])<1e-6);
    CHECK(weights(*selectHemisphere({0,0,1}))[20]>0.999999);
}

TEST(engine_hemisphere_top_view_passes_shared_representation_policy) {
    auto candidates=choices();auto camera=view();
    const auto hemisphere=selectHemisphere({0,0,1});CHECK(bool(hemisphere));if (!hemisphere) return;
    candidates[1].bakedDirection={0,0,1};
    candidates[1].errorMetres=2*std::sin(hemisphere->maxAngle*0.5)+2.0/256;
    CHECK_EQ(selectRepresentation(camera,{{0,0,0},1},candidates).candidate,std::size_t(1));
    candidates[1].bakedDirection={0,1,0};candidates[1].angularErrorMetres=1;
    CHECK_EQ(selectRepresentation(camera,{{0,0,0},1},candidates).candidate,std::size_t(0));
}

