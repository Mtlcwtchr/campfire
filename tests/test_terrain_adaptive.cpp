#include "framework.hpp"
#include "game/world/terrain_adaptive.hpp"
#include "game/world/terrain_plan.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace {
using namespace world::terrain;
std::array<float,2> cliff(double x, double y) {
    return {float(40 * std::clamp((x-25)/3,0.0,1.0) + 0.1*y), 3.0f};
}
void topology(const AdaptiveMesh& mesh, bool completeBoundary=true) {
    using Edge = std::pair<std::pair<int,int>,std::pair<int,int>>;
    std::map<Edge,int> edges;
    double area = 0;
    for (std::size_t i=0; i<mesh.surfaceIndices; i+=3) {
        const auto a=mesh.vertices[mesh.indices[i]], b=mesh.vertices[mesh.indices[i+1]], c=mesh.vertices[mesh.indices[i+2]];
        const int twice = (int(b.x)-a.x)*(int(c.y)-a.y) - (int(b.y)-a.y)*(int(c.x)-a.x);
        CHECK(twice > 0); area += twice*0.5;
        const AdaptiveVertex v[]{a,b,c,a};
        for(int e=0;e<3;++e) {
            auto p=std::pair{int(v[e].x),int(v[e].y)}, q=std::pair{int(v[e+1].x),int(v[e+1].y)};
            if(q<p) std::swap(p,q);
            ++edges[{p,q}];
        }
    }
    CHECK_EQ(area,double(mesh.cells)*mesh.cells);
    int boundary=0;
    for(const auto& [edge,count]:edges) {
        const auto [a,b]=edge;
        const bool border=(a.first==b.first && (a.first==0 || a.first==mesh.cells)) ||
            (a.second==b.second && (a.second==0 || a.second==mesh.cells));
        CHECK_EQ(count,border?1:2); // unmatched interior edges expose T-junctions
        if(border) {
            ++boundary;
            const int length=std::abs(a.first-b.first)+std::abs(a.second-b.second);
            CHECK(length>0 && (length&(length-1))==0);
            if (completeBoundary) CHECK_EQ(length,1);
        }
    }
    if (completeBoundary) CHECK_EQ(boundary,4*mesh.cells);
    CHECK_EQ(mesh.indices.size()-mesh.surfaceIndices,std::size_t(boundary)*6);
}
}

TEST(terrain_adaptive_planes_keep_boundaries_without_uniform_interior) {
    for (const double slope : {0.0,2.0}) {
        const auto field=[&](double x,double y){return std::array{float(100+slope*x+0.5*y),0.0f};};
        const auto mesh=makeAdaptiveMesh(64,1,0.1,field);
        CHECK(mesh->surfaceIndices < 64u*64*6/3);
        CHECK_EQ(mesh->detailError,0.0);
        topology(*mesh);
        for(int y=0;y<=64;++y) for(int x=0;x<=64;++x)
            CHECK(std::abs(mesh->sample(x,y)[0]-field(x,y)[0])<0.001);
    }
}
TEST(terrain_adaptive_cliff_refines_locally_and_bounds_height_error) {
    const auto mesh=makeAdaptiveMesh(64,1,0.2,cliff);
    topology(*mesh);
    CHECK(mesh->detailError > 1);
    CHECK(mesh->surfaceIndices < 64u*64*6);
    int cliffTriangles=0, flatTriangles=0;
    for(std::size_t i=0;i<mesh->surfaceIndices;i+=3) {
        double x=0;
        for(int j=0;j<3;++j) x+=mesh->vertices[mesh->indices[i+j]].x/3.0;
        cliffTriangles+=x>=24 && x<=30;
        flatTriangles+=x>=8 && x<=14;
    }
    CHECK(cliffTriangles > flatTriangles);
    for(int y=0;y<=64;++y) for(int x=0;x<=64;++x)
        CHECK(std::abs(mesh->sample(x,y)[0]-cliff(x,y)[0])<=0.201);
}
TEST(terrain_adaptive_parent_morph_uses_actual_triangles) {
    const auto parent=makeAdaptiveMesh(64,2,0.5,cliff);
    for (int quadrant=0;quadrant<4;++quadrant) {
        const double ox=(quadrant%2)*64, oy=(quadrant/2)*64;
        const auto target=[&](double x,double y){return parent->sample(x+ox,y+oy);};
        const auto child=makeAdaptiveMesh(64,1,0.1,[&](double x,double y){return cliff(x+ox,y+oy);},target);
        topology(*child);
        // Sample triangle interiors, not just vertices: every morphed child
        // triangle must reproduce the parent's piecewise planar surface.
        for(std::size_t i=0;i<child->surfaceIndices;i+=3) {
            double x=0,y=0,h=0;
            for(int j=0;j<3;++j) {
                const auto v=child->vertices[child->indices[i+j]];
                x+=v.x/3.0; y+=v.y/3.0; h+=v.parentBed/3.0;
            }
            CHECK(std::abs(h-target(x,y)[0])<0.002);
        }
    }
}
TEST(terrain_adaptive_water_head_is_also_a_geometry_constraint) {
    const auto field=[](double x,double y){return std::array{0.0f,cliff(x,y)[0]};};
    const auto mesh=makeAdaptiveMesh(64,1,0.1,field);
    topology(*mesh);
    for(int y=0;y<=64;++y) for(int x=0;x<=64;++x)
        CHECK(std::abs(mesh->sample(x,y)[1]-field(x,y)[1])<=0.101);
}

TEST(terrain_adaptive_sparse_edges_leave_both_flat_and_steep_planes_minimal) {
    for (double slope:{0.0,3.0}) {
        const auto field=[=](double x,double y){return std::array{float(100+slope*x+0.5*y),-6000.0f};};
        const auto mesh=makeAdaptiveMesh(64,8,0.25,field,{},0.001,{},{},{},{64,10,0.25});
        CHECK_EQ(mesh->surfaceIndices,6u);
        topology(*mesh,false);
        for (int y=0;y<=512;y+=8) for (int x=0;x<=512;x+=8)
            CHECK(std::abs(mesh->sample(x,y)[0]-field(x,y)[0])<0.001);
    }
}

TEST(terrain_adaptive_source_probes_find_ridges_missed_by_nominal_grid) {
    const auto field=[](double x,double y) {
        // A connected branching crest whose central peak lies BETWEEN H64 vertices.
        const double crest=280+std::abs(y-512)*0.25;
        return std::array{float(100+80*std::max(0.0,1-std::abs(x-crest)/16)),0.0f};
    };
    const auto coarse=makeAdaptiveMesh(16,64,0.5,field);
    const auto mesh=makeAdaptiveMesh(128,8,0.5,field,{},0.001,{},{},{},{8,10,0.25});
    CHECK(std::abs(coarse->sample(280,512)[0]-field(280,512)[0])>20);
    topology(*mesh,false);
    CHECK(mesh->surfaceIndices<128u*128*6/3);
    int ridge=0,flat=0;
    for (std::size_t i=0;i<mesh->surfaceIndices;i+=3) {
        double x=0;
        for (int c=0;c<3;++c) x+=mesh->vertices[mesh->indices[i+c]].x*mesh->step/3;
        ridge+=x>=256 && x<512;flat+=x>=640 && x<896;
    }
    CHECK(ridge>flat*2);
    for (int y=0;y<=1024;y+=8) for (int x=0;x<=1024;x+=8)
        CHECK(std::abs(mesh->sample(x,y)[0]-field(x,y)[0])<=0.501);
}

TEST(terrain_adaptive_normal_error_protects_small_crests_and_prior_stage_valleys) {
    const auto shape=[](double x,double){return float(0.9*std::max(0.0,1-std::abs(x-25)/2));};
    const auto field=[&](double x,double y){return std::array{shape(x,y),-6000.0f};};
    const auto loose=makeAdaptiveMesh(64,1,4,field,{},0.001,{},{},{},{64,0,0.25});
    const auto sharp=makeAdaptiveMesh(64,1,4,field,{},0.001,{},{},{},{64,10,0.25});
    CHECK(sharp->surfaceIndices>loose->surfaceIndices);
    CHECK(std::abs(sharp->sample(25,32)[0]-0.9f)<0.01);
    topology(*sharp,false);
    const auto flat=[](double,double){return std::array{0.0f,-6000.0f};};
    const auto valley=[&](double x,double y){return -shape(x,y);};
    const auto prior=makeAdaptiveMesh(64,1,4,flat,{},0.001,{},valley,{},{64,10,0.25});
    CHECK(std::abs(prior->samplePrior(25,32)+0.9f)<0.01);
    topology(*prior,false);
    const auto quiet=makeAdaptiveMesh(64,1,4,field,{},0.001,{},{},{},{64,10,1});
    CHECK_EQ(quiet->surfaceIndices,loose->surfaceIndices); // explicit quantisation/noise floor
}

TEST(terrain_adaptive_fractional_samples_survive_compaction_and_stage_changes) {
    for (const bool curved : {false,true}) {
        const auto height=[=](double x,double y) {
            return float(100+0.25*x+0.5*y+(curved?0.002*x*x+0.003*y*y:0));
        };
        const auto field=[&](double x,double y){return std::array{height(x,y),float(500+0.125*x)};};
        const auto prior=[&](double x,double y){return -height(x,y);};
        const auto mesh=makeAdaptiveMesh(64,1,0.2,field,{},0.001,{},prior,{},{64,0,0.25});
        topology(*mesh,false);
        if (!curved) {
            CHECK_EQ(mesh->cells,1);
            CHECK_EQ(mesh->step,64.0);
            CHECK_EQ(mesh->surfaceIndices,6u);
        }
        for (int iy=0;iy<=256;++iy) for (int ix=0;ix<=256;++ix) {
            if (ix%4==0 && iy%4==0) continue; // independent construction-grid holdouts
            const double x=ix*0.25,y=iy*0.25;
            const auto sample=mesh->sample(x,y);
            // This quadratic's finest-cell interpolation remainder is <=0.00125 m.
            CHECK(std::abs(sample[0]-height(x,y))<=0.202);
            CHECK(std::abs(mesh->samplePrior(x,y)-prior(x,y))<=0.202);
            CHECK(std::abs(sample[1]-field(x,y)[1])<0.001);
        }
    }
}

TEST(terrain_adaptive_probe_error_does_not_bound_unsampled_terrain) {
    const auto field=[](double x,double) {
        return std::array{float(10*std::max(0.0,1-std::abs(x-4)/2)),0.0f};
    };
    const auto mesh=makeAdaptiveMesh(64,8,0.1,field,{},0.001,{},{},{},{64,10,0.25});
    CHECK_EQ(mesh->surfaceIndices,6u);
    CHECK_EQ(mesh->detailError,0.0);
    for (int y=0;y<=512;y+=8) for (int x=0;x<=512;x+=8)
        CHECK_EQ(mesh->sample(x,y)[0],field(x,y)[0]);
    // Perfect agreement at every probe is not a continuous terrain-error bound.
    CHECK_EQ(field(4,256)[0]-mesh->sample(4,256)[0],10.0f);
    const auto finer=makeAdaptiveMesh(256,2,0.1,field,{},0.001,{},{},{},{256,10,0.25});
    CHECK(std::abs(finer->sample(4,256)[0]-field(4,256)[0])<0.001);
}

TEST(terrain_adaptive_normal_error_also_protects_lod_parent_endpoints) {
    const auto flat=[](double,double){return std::array{0.0f,-6000.0f};};
    const auto crest=[](double x,double){return float(0.2*std::max(0.0,1-std::abs(x-25)));};
    const SurfaceSample parent=[&](double x,double y){return std::array{crest(x,y),-6000.0f};};
    const HeightSample oldParent=[&](double x,double y){return -crest(x,y);};
    for (bool old:{false,true}) {
        const auto mesh=makeAdaptiveMesh(64,1,4,flat,old?SurfaceSample{}:parent,0.25,
            {},{},old?oldParent:HeightSample{},{64,10,0.01});
        topology(*mesh,false);
        // Reconstruct the parent's endpoint on the actual retained topology,
        // not on the finer probe grid which is not necessarily rendered.
        auto morphed=*mesh;
        for (int y=0;y<=mesh->cells;++y) for (int x=0;x<=mesh->cells;++x)
            morphed.bed[std::size_t(y)*(mesh->cells+1)+x]=(old?-1:1)*crest(x*mesh->step,y*mesh->step);
        CHECK(std::abs(morphed.sample(25,32)[0]-(old?-0.2f:0.2f))<0.001);
        CHECK(mesh->surfaceIndices<64u*64*6/3);
    }
    const auto plane=[](double x,double y){return std::array{float(3*x+y),-6000.0f};};
    const auto minimal=makeAdaptiveMesh(64,1,4,flat,plane,0.25,{},{},{},{64,10,0.01});
    CHECK_EQ(minimal->surfaceIndices,6u); // absolute steepness is not normal error
}

TEST(terrain_adaptive_detail_error_includes_the_previous_stage) {
    const auto flat=[](double,double){return std::array{0.0f,-6000.0f};};
    const auto previous=[](double x,double y){return cliff(x,y)[0];};
    const auto mesh=makeAdaptiveMesh(64,1,0.2,flat,{},0.001,{},previous);
    CHECK(mesh->detailError>1);
}

TEST(terrain_adaptive_rejects_invalid_subdivision_criteria) {
    const auto plane=[](double,double){return std::array{0.0f,0.0f};};
    for (const auto c:std::array<AdaptiveCriteria,7>{{{0,10,0.25},{3,10,0.25},{128,10,0.25},
        {1,-1,0.25},{1,91,0.25},{1,10,-1},{1,std::numeric_limits<double>::quiet_NaN(),0.25}}}) {
        bool rejected=false;
        try { (void)makeAdaptiveMesh(64,8,1,plane,{},0.001,{},{},{},c); }
        catch (const std::invalid_argument&) { rejected=true; }
        CHECK(rejected);
    }
}

TEST(terrain_adaptive_morph_budget_simplifies_flats_without_losing_cliffs_or_seams) {
    for (const bool steep : {false,true}) {
        const auto field=[=](double x,double y) {
            return std::array{float(100+0.1*x+0.2*y+0.04*std::sin(x/4)*std::cos(y/4)+
                (steep?40*std::clamp((x-25)/2,0.0,1.0):0)),0.0f};
        };
        const auto parent=makeAdaptiveMesh(64,1,0.001,field);
        const auto target=[&](double x,double y){return parent->sample(x,y);};
        const auto exact=makeAdaptiveMesh(64,1,0.1,field,target);
        const auto mesh=makeAdaptiveMesh(64,1,0.1,field,target,0.1);
        CHECK(mesh->surfaceIndices<exact->surfaceIndices);
        topology(*mesh);
        for (int y=0;y<=64;++y) for (int x=0;x<=64;++x)
            CHECK(std::abs(mesh->sample(x,y)[0]-field(x,y)[0])<=0.101);
        for (std::size_t i=0;i<mesh->surfaceIndices;i+=3) {
            double x=0,y=0,h=0;
            for (int j=0;j<3;++j) {
                const auto& v=mesh->vertices[mesh->indices[i+j]];
                x+=v.x/3.0;y+=v.y/3.0;h+=v.parentBed/3.0;
            }
            CHECK(std::abs(h-target(x,y)[0])<=0.101);
        }
    }
}

namespace {
using Boundary = std::map<std::pair<double,double>, std::array<float,2>>;
Boundary renderedBoundary(const TerrainPlan::Block& block) {
    Boundary result;
    const auto& mesh = *block.mesh;
    const double side = world::tileMetresAt(block.tile.lod);
    // Only real surface vertices: skirts cannot hide a failing seam test.
    for (std::size_t i = 0; i < mesh.surfaceIndices; ++i) {
        const auto& v = mesh.vertices[mesh.indices[i]];
        if (v.x != 0 && v.y != 0 && v.x != mesh.cells && v.y != mesh.cells) continue;
        const auto index = std::size_t(v.y) * (mesh.cells + 1) + v.x;
        const std::array height = (v.skirt & 2) ? std::array{v.edgeBed,v.edgeHead} :
            std::array{std::lerp(mesh.bed[index],v.parentBed,block.parentMorph),
                       std::lerp(mesh.head[index],v.parentHead,block.parentMorph)};
        result[{block.tile.x * side + v.x * mesh.step, block.tile.y * side + v.y * mesh.step}] = height;
    }
    return result;
}
void matchingSeams(const TerrainPlan& plan) {
    int checked = 0;
    for (const auto& a : plan.coverage) for (const auto& b : plan.coverage) {
        const double sa = world::tileMetresAt(a.tile.lod), sb = world::tileMetresAt(b.tile.lod);
        const double ax = a.tile.x*sa, ay = a.tile.y*sa, bx = b.tile.x*sb, by = b.tile.y*sb;
        for (const bool vertical : {false,true}) {
            if (vertical ? ax+sa != bx : ay+sa != by) continue;
            const double fixed = vertical ? bx : by;
            const double lo = std::max(vertical ? ay : ax, vertical ? by : bx);
            const double hi = std::min((vertical ? ay : ax)+sa, (vertical ? by : bx)+sb);
            if (lo > hi) continue;
            const auto ba = renderedBoundary(a), bb = renderedBoundary(b);
            const auto sample = [&](const Boundary& edge, double t, double step, double end) {
                const double first = std::min(std::floor(t/step)*step, end-step);
                const auto p = vertical ? std::pair{fixed,first} : std::pair{first,fixed};
                const auto q = vertical ? std::pair{fixed,first+step} : std::pair{first+step,fixed};
                const auto u = edge.at(p), v = edge.at(q);
                return std::array{std::lerp(double(u[0]),double(v[0]),(t-first)/step),
                                  std::lerp(double(u[1]),double(v[1]),(t-first)/step)};
            };
            for (double t = lo; t <= hi; t += std::min(a.mesh->step,b.mesh->step)*0.5) {
                const auto u = sample(ba,t,a.mesh->step,(vertical ? ay : ax)+sa);
                const auto v = sample(bb,t,b.mesh->step,(vertical ? by : bx)+sb);
                CHECK(std::abs(u[0]-v[0]) < 0.001);
                CHECK(std::abs(u[1]-v[1]) < 0.001);
                ++checked;
            }
        }
    }
    CHECK(checked > 0);
}
TerrainPlan seamPlan(float morph, bool sparse=false) {
    TerrainPlan plan;
    std::vector<world::TileId> tiles{{0,0,2},{0,1,2},{1,1,2}};
    for (const auto child : TerrainCut::children({1,0,2})) {
        if (child.x == 2 && child.y == 0)
            for (const auto fine : TerrainCut::children(child)) tiles.push_back(fine);
        else tiles.push_back(child);
    }
    for (const auto tile : tiles) {
        const double side = world::tileMetresAt(tile.lod);
        const auto field = [=](double x,double y) {
            x += tile.x*side; y += tile.y*side;
            return std::array{float(0.0001*x*x+0.0002*y*y+tile.lod*10),
                              float(500+0.0003*x*x+tile.lod*5)};
        };
        const auto parent = [=](double x,double y) {
            auto height = field(x,y);
            height[0] += 30; height[1] += 20;
            return height;
        };
        TerrainPlan::Block block;
        block.tile = tile;
        block.parentMorph = tile.lod == 2 ? 0 : ((tile.x+tile.y)%2 ? morph : 1-morph);
        block.mesh = makeAdaptiveMesh(16,side/16,sparse?8:0.5,field,parent,sparse?8:0.001,
            {},{},{},{sparse?8:1,0,0.25});
        plan.coverage.push_back(block);
    }
    return plan;
}
}

TEST(terrain_adaptive_mixed_lod_seams_match_without_skirts_through_morphs) {
    for (const float morph : {0.0f,0.25f,0.5f,0.75f,1.0f}) {
        auto plan = seamPlan(morph);
        const auto original = plan.coverage;
        plan.stitchEdges();
        matchingSeams(plan);
        std::size_t changed = 0;
        for (std::size_t i = 0; i < original.size(); ++i) {
            CHECK(!original[i].mesh->unstitched);
            CHECK_EQ(plan.coverage[i].mesh->indices, original[i].mesh->indices);
            CHECK_EQ(plan.coverage[i].mesh->surfaceIndices, original[i].mesh->surfaceIndices);
            changed += plan.coverage[i].mesh != original[i].mesh;
        }
        CHECK(changed > 0);
        const auto stitched = plan.coverage;
        plan.coverage = original;
        plan.stitchEdges(stitched);
        for (std::size_t i = 0; i < original.size(); ++i)
            CHECK_EQ(plan.coverage[i].mesh,stitched[i].mesh); // no duplicate GPU uploads
    }
}

TEST(terrain_adaptive_seams_do_not_depend_on_cut_order_or_change_published_meshes) {
    auto forward = seamPlan(0.25f);
    auto reverse = forward;
    std::reverse(reverse.coverage.begin(), reverse.coverage.end());
    forward.stitchEdges(); reverse.stitchEdges();
    for (std::size_t i = 0; i < forward.coverage.size(); ++i)
        CHECK_EQ(renderedBoundary(forward.coverage[i]),
                 renderedBoundary(reverse.coverage[reverse.coverage.size()-1-i]));
    const auto published = forward;
    for (auto& block : forward.coverage) block.parentMorph = 1-block.parentMorph;
    forward.stitchEdges(published.coverage);
    matchingSeams(forward);
    matchingSeams(published);
}

TEST(terrain_adaptive_sparse_boundaries_stitch_the_actual_segments) {
    for (float morph:{0.0f,0.25f,0.5f,1.0f}) {
        auto plan=seamPlan(morph,true);
        for (const auto& b:plan.coverage) topology(*b.mesh,false);
        plan.stitchEdges();
        // Sample real edge intervals: discarded probe-lattice points are NOT
        // vertices of the rendered mesh and must not act as seam constraints.
        for (const auto& a:plan.coverage) for (const auto& b:plan.coverage) {
            const double sa=a.metres(),sb=b.metres();
            if (a.tile.x*sa+sa!=b.tile.x*sb) continue;
            const double lo=std::max(a.tile.y*sa,b.tile.y*sb);
            const double hi=std::min((a.tile.y+1)*sa,(b.tile.y+1)*sb);
            if (hi<=lo) continue;
            const double edge=b.tile.x*sb;
            const auto sample=[&](const Boundary& boundary,double y) {
                auto upper=boundary.lower_bound({edge,y});
                if (upper!=boundary.end() && upper->first==std::pair{edge,y}) return upper->second;
                const auto lower=std::prev(upper);
                CHECK_EQ(lower->first.first,edge);CHECK_EQ(upper->first.first,edge);
                const float u=float((y-lower->first.second)/(upper->first.second-lower->first.second));
                return std::array{std::lerp(lower->second[0],upper->second[0],u),
                                  std::lerp(lower->second[1],upper->second[1],u)};
            };
            const auto ba=renderedBoundary(a),bb=renderedBoundary(b);
            for (double y=lo;y<=hi;y+=4) {
                const auto u=sample(ba,y),v=sample(bb,y);
                CHECK(std::abs(u[0]-v[0])<0.001);CHECK(std::abs(u[1]-v[1])<0.001);
            }
        }
    }
}

TEST(terrain_adaptive_publication_interpolates_stitched_edges_every_frame) {
    auto old = seamPlan(0.1f);
    old.stitchEdges();
    auto next = seamPlan(0.9f);
    next.stitchEdges(old.coverage);
    const auto target = next.coverage;
    next.interpolateFrom(old.coverage);
    CHECK(next.interpolated);
    for (float t : {0.0f,0.25f,0.5f,0.75f,1.0f}) {
        auto displayed = next;
        for (auto& b : displayed.coverage) {
            auto mesh = std::make_shared<AdaptiveMesh>(*b.mesh);
            for (auto& v : mesh->vertices) {
                if (!(v.skirt & 8)) continue;
                const float bed = (v.skirt & 2) ? v.edgeBed : std::lerp(v.sourceBed,v.parentBed,b.parentMorph);
                const float head = (v.skirt & 2) ? v.edgeHead : std::lerp(v.sourceHead,v.parentHead,b.parentMorph);
                v.edgeBed = std::lerp(v.displayFrom[0],bed,t);
                v.edgeHead = std::lerp(v.displayFrom[1],head,t);
                v.skirt |= 2;
            }
            b.mesh = std::move(mesh);
        }
        matchingSeams(displayed);
        if (t == 0 || t == 1)
            for (std::size_t i=0;i<target.size();++i) {
                const auto actual=renderedBoundary(displayed.coverage[i]);
                const auto expected=renderedBoundary(t==0?old.coverage[i]:target[i]);
                for (const auto& [p,h]:actual) if (expected.contains(p)) {
                    CHECK(std::abs(h[0]-expected.at(p)[0])<0.001);
                    CHECK(std::abs(h[1]-expected.at(p)[1])<0.001);
                }
            }
    }
    for (const auto& b : old.coverage)
        for (const auto& v : b.mesh->vertices) CHECK(!(v.skirt & 8));
    // A settled plan must not replay the previous transition on the next one.
    TerrainPlan settled;
    settled.coverage = target;
    settled.interpolateFrom(target);
    CHECK(!settled.interpolated);
    for (std::size_t i=0;i<target.size();++i) CHECK_EQ(settled.coverage[i].mesh,target[i].mesh);
}

TEST(terrain_adaptive_split_publication_uses_previous_triangles_and_prior_stage) {
    TerrainPlan::Block old;
    old.tile = {-1,-1,2}; old.extentMetres=16;
    const auto field=[](double x,double y) { return std::array{float(100+x*y),float(400+x)}; };
    old.mesh=makeAdaptiveMesh(2,8,0,field,{},0.001,{},[](double x,double y){return float(50+x*y);});
    TerrainPlan next;
    for (int y=-2;y<0;++y) for (int x=-2;x<0;++x) {
        TerrainPlan::Block child;
        child.tile={x,y,1}; child.extentMetres=8;
        child.mesh=makeAdaptiveMesh(4,2,0,field);
        next.coverage.push_back(child);
    }
    next.interpolateFrom({old});
    CHECK(next.interpolated);
    for (const auto& b:next.coverage) for (const auto& v:b.mesh->vertices) {
        const double x=b.tile.x*8+v.x*b.mesh->step+16;
        const double y=b.tile.y*8+v.y*b.mesh->step+16;
        const auto h=old.mesh->sample(x,y);
        CHECK(v.skirt&8);
        CHECK(std::abs(v.displayFrom[0]-h[0])<0.001);
        CHECK(std::abs(v.displayFrom[1]-h[1])<0.001);
        CHECK(std::abs(v.displayFrom[2]-old.mesh->samplePrior(x,y))<0.001);
    }
}


TEST(terrain_adaptive_water_covers_only_the_triangles_that_have_water_on_them) {
    // The water pass draws a prefix of the surface, not the whole of it.
    //
    // It used to draw every triangle of any square that held any water at all
    // and let the pixel shader discard what turned out to be dry - after the
    // waves, the ice, the climate, two screen-space derivatives and a pair of
    // noise lookups. A square with a stream across one corner therefore ran the
    // most expensive shader in the engine over every pixel of its ground and
    // threw nearly all of it away. Two full-coverage shaders over the landscape
    // is what a frame was being spent on, which is why taking triangles away
    // did not move it.
    const auto surface = [](double x, double y) {
        // A valley running north, with water standing in the bottom of it.
        const double bed = 40 + std::abs(x - 32) * 1.5;
        const double head = x > 24 && x < 40 ? 48.0 : -6000.0;
        return std::array<float, 2>{float(bed), float(head)};
    };
    const auto mesh = engine::makeAdaptiveMesh(64, 4, 0.5, surface);
    CHECK(bool(mesh));
    if (!mesh) return;
    CHECK(mesh->wetIndices > 0);
    CHECK(mesh->wetIndices < mesh->surfaceIndices);
    const auto wet = [&](std::uint32_t index) {
        const auto& v = mesh->vertices[index];
        return v.sourceHead > v.sourceBed;
    };
    // Everything in the prefix has water on it.
    for (std::uint32_t i = 0; i + 2 < mesh->wetIndices; i += 3)
        CHECK(wet(mesh->indices[i]) || wet(mesh->indices[i + 1]) || wet(mesh->indices[i + 2]));
    // And nothing outside it does, so the prefix is the whole of the water and
    // not merely some of it: a missing triangle is a hole in a river.
    for (std::uint32_t i = mesh->wetIndices; i + 2 < mesh->surfaceIndices; i += 3)
        CHECK(!wet(mesh->indices[i]) && !wet(mesh->indices[i + 1]) && !wet(mesh->indices[i + 2]));
    // The terrain pass still gets all of them: the partition reorders, never
    // removes.
    CHECK_EQ(mesh->surfaceIndices % 3, std::uint32_t(0));
}
