#include "game/world/terrain_plan.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <numeric>
#include <tuple>
#include <unordered_map>
#include <utility>
#include "game/generation/world_map_gen.hpp"
#include "game/world/ring_mesh.hpp"
#include "game/world/terrain_view.hpp"
#include "game/world/terrain_streaming/page_store.hpp"

namespace world::terrain {
namespace {
using Key = streaming::TileKey;
using Keys = std::unordered_set<Key>;
using Clock = std::chrono::steady_clock;

bool nearer(Key a, Key b, double x, double y) {
    const auto rank = [&](Key key) {
        const double metres = streaming::pageMetresAtLevel(key.level);
        return std::tuple{focusDistanceSquared({key.x * metres, key.y * metres,
            (key.x + 1) * metres, (key.y + 1) * metres}, x, y), -int(key.level), key.y, key.x};
    };
    return rank(a) < rank(b);
}

// Each parallel stage owns its own caches: no lock around terrain queries.
struct Geometry {
    const generation::WorldMapData& world;
    const streaming::PageStore& pages;
    DataLodPolicy dataPolicy;
    struct Node { ViewBounds box; bool land; };
    std::unordered_map<std::int64_t, Node> nodes;
    std::unordered_map<std::int64_t, std::vector<Key>> dependencies;
    std::unordered_map<std::int64_t, std::shared_ptr<const AdaptiveMesh>> meshes;
    std::unordered_map<std::int64_t, std::shared_ptr<const AdaptiveMesh>> baseMeshes;
    std::unordered_set<std::int64_t> meshUses;
    std::unordered_set<std::int64_t> roots;
    std::unique_ptr<TerrainDetail> detail;
    TerrainView view;
    bool sampleDetail = true;
    std::size_t meshBudget = std::numeric_limits<std::size_t>::max();
    // For replacing meshes over moved ground (see mesh()).
    std::size_t rebuildBudget = std::numeric_limits<std::size_t>::max();
    bool deferred = false;
    // The ground each page stood on when this stage last looked at it
    // (SurfacePage::version), and the residency it looked at.
    std::unordered_map<Key, std::uint64_t> grounds;
    std::uint64_t groundsSeen = std::numeric_limits<std::uint64_t>::max();
    // Cached meshes built over ground that has moved since, by cache.
    std::unordered_set<std::int64_t> staleMeshes, staleBaseMeshes;
    // The plan each cached mesh was last used in. A mesh the cut no longer
    // draws stays, least recently used out first, within the config's budget:
    // it used to go the moment the cut stopped using it, so ground the eye had
    // just left - a zoom out and back in, a turn of the head - was built again
    // from nothing every time.
    std::unordered_map<std::int64_t, std::uint64_t> lastUsed;
    std::uint64_t plans = 0;
    static std::size_t bytesOf(const AdaptiveMesh& m) {
        return m.vertices.size() * sizeof(m.vertices[0]) + m.indices.size() * sizeof(m.indices[0]) +
               (m.bed.size() + m.head.size() + m.prior.size()) * sizeof(float) + m.splits.size() + 256;
    }
    void trimCache(std::size_t budget) {
        ++plans;
        for (const auto id : meshUses) lastUsed[id] = plans;
        struct Idle { std::uint64_t used; bool base; std::int64_t id; std::size_t bytes; };
        std::vector<Idle> idle;
        std::size_t idleBytes = 0;
        const auto gather = [&](const auto& cache, bool base) {
            for (const auto& [id, mesh] : cache) {
                const auto bytes = bytesOf(*mesh);
                if (meshUses.contains(id)) continue;
                const auto it = lastUsed.find(id);
                idle.push_back({it == lastUsed.end() ? 0 : it->second, base, id, bytes});
                idleBytes += bytes;
            }
        };
        gather(meshes, false);
        gather(baseMeshes, true);
        if (idleBytes > budget) {
            std::sort(idle.begin(), idle.end(), [](const Idle& a, const Idle& b) { return a.used < b.used; });
            for (const auto& i : idle) {
                if (idleBytes <= budget) break;
                (i.base ? baseMeshes : meshes).erase(i.id);
                idleBytes -= i.bytes;
            }
        }
        if (lastUsed.size() > 4 * (meshes.size() + baseMeshes.size()) + 1024)
            std::erase_if(lastUsed, [&](const auto& e) { return !meshes.contains(e.first) && !baseMeshes.contains(e.first); });
    }
    Geometry(const generation::WorldMapData& w,const streaming::PageStore& p,DataLodPolicy policy,bool samples=true)
        :world(w),pages(p),dataPolicy(policy),sampleDetail(samples) {
        if (policy.targeted && !policy.cameraBudget)
            detail=std::make_unique<TerrainDetail>(w,p,policy.chunkCells,policy.chunkMetres);
    }
    std::int64_t metres(TileId tile) const { return dataPolicy.metresAt(tile.lod); }
    bool isRoot(TileId tile) const { return roots.contains(tileKeyOf(tile)); }
    TileId parentTile(TileId tile) const {
        if (tile.lod >= int(kGeometryLevels) - 1) return tile;
        const auto ratio = dataPolicy.metresAt(tile.lod+1) / metres(tile);
        return {int(floorDiv(tile.x,ratio)),int(floorDiv(tile.y,ratio)),tile.lod+1};
    }
    std::vector<TileId> children(TileId tile) const {
        std::vector<TileId> result;
        if (tile.lod <= 0) return result;
        const int ratio = int(metres(tile) / dataPolicy.metresAt(tile.lod-1));
        for (int y=0;y<ratio;++y) for (int x=0;x<ratio;++x)
            result.push_back({tile.x*ratio+x,tile.y*ratio+y,tile.lod-1});
        return result;
    }
    void prepare(const TerrainView& current,double seconds) {
        if (view.stageFrom!=current.stageFrom || view.stageTo!=current.stageTo) {
            meshes.clear(); baseMeshes.clear(); dependencies.clear();
        }
        view=current;
        if (detail && sampleDetail && detail->updateLocal(current.localDetail,current.x,current.y,seconds,
                                                        current.config.morphSeconds)) {
            // A local transition changes only the finest source. Coarse cached
            // meshes, and all feature products outside that level, survive.
            const auto finestSide = metres({0,0,0});
            std::erase_if(meshes,[&](const auto& entry){return entry.second->step*entry.second->cells==finestSide &&
                (!dataPolicy.chunkMetres[0] || entry.second->step==dataPolicy.stepAt(0));});
        }
    }
    // Pages whose surface now stands on other ground than when this stage
    // last looked (a dig, edit_layer.hpp). Every cached mesh built from one
    // of them is a picture of ground that is gone: it is marked stale, drawn
    // as it is until its replacement is built inside the mesh budget - a dig
    // never opens a hole - and the rest of the world is not touched. A page
    // that merely left and came back on the same ground is no change.
    std::unordered_set<Key> observeGround(const TerrainResidency& residency) {
        std::unordered_set<Key> changed;
        if (residency.revision == groundsSeen) return changed;
        groundsSeen = residency.revision;
        for (const auto& [key, surface] : residency.surfaces) {
            if (!surface) continue;
            const auto [it, fresh] = grounds.try_emplace(key, surface->version);
            if (fresh || it->second == surface->version) continue;
            it->second = surface->version;
            changed.insert(key);
        }
        if (changed.empty()) return changed;
        const auto mark = [&](const auto& cache, std::unordered_set<std::int64_t>& stale) {
            for (const auto& [id, mesh] : cache) {
                const auto deps = dependencies.find(id);
                if (deps != dependencies.end() &&
                    std::any_of(deps->second.begin(), deps->second.end(), [&](Key key) { return changed.contains(key); }))
                    stale.insert(id);
            }
        };
        mark(meshes, staleMeshes);
        mark(baseMeshes, staleBaseMeshes);
        return changed;
    }
    [[nodiscard]] bool staleMesh(TileId tile) const { return staleMeshes.contains(tileKeyOf(tile)); }
    // After the caches are trimmed: nothing is stale that is not cached.
    void trimStale() {
        std::erase_if(staleMeshes, [&](std::int64_t id) { return !meshes.contains(id); });
        std::erase_if(staleBaseMeshes, [&](std::int64_t id) { return !baseMeshes.contains(id); });
    }
    bool refine(TileId tile,const TerrainResidency& residency,const TerrainView& current,
                bool children=false,bool cachedOnly=false) {
        const auto& box=bounds(tile).box;
        if (dataPolicy.cameraBudget)
            return current.refineSurface(box,tile.lod,nullptr,dataPolicy,children);
        if (detail && dataPolicy.stableFeatures && detail->hasFeatures(tile))
            return tile.lod>1; // fixed 512 m blocks, H2 source, at every view distance
        std::shared_ptr<const AdaptiveMesh> shape;
        if (cachedOnly) {
            if (const auto it=baseMeshes.find(tileKeyOf(tile));it!=baseMeshes.end()) shape=it->second;
        } else shape=mesh(tile,residency,true,true);
        if (detail && tile.lod>1 && tile.lod<=3 &&
            current.lodMetresPerPixel(box)<(children?2.0:1.0) &&
            detail->hasFeatures(tile)) return true;
        return current.refineSurface(box,tile.lod,shape.get(),dataPolicy,children);
    }

    // `probe`: asked whether a square could be drawn, or what shape it has,
    // not for the mesh to draw - a mesh over moved ground answers that as
    // well as its replacement would, and the rebuild budget goes to what is
    // on screen.
    std::shared_ptr<const AdaptiveMesh> mesh(TileId tile, const TerrainResidency& residency, bool baseOnly=false,
                                             bool probe=false) {
        if (residency.surfaces.empty()) return {};
        for (auto ancestor = tile;; ancestor = parentTile(ancestor)) {
            meshUses.insert(tileKeyOf(ancestor));
            if (isRoot(ancestor) || ancestor.lod >= int(kGeometryLevels) - 1) break;
        }
        const auto id = tileKeyOf(tile);
        auto& cache=baseOnly?baseMeshes:meshes;
        auto& stale=baseOnly?staleBaseMeshes:staleMeshes;
        // A mesh over ground that has since moved is rebuilt when the budget
        // allows, and until then it is what is drawn: the old ground for a few
        // frames, never a hole where a square was.
        std::shared_ptr<const AdaptiveMesh> previous;
        if (const auto it = cache.find(id); it != cache.end()) {
            if (probe || !stale.contains(id)) return it->second;
            previous = it->second;
        }
        for (auto key : keys(tile)) if (!residency.surfaces.contains(key)) return previous;
        // Replacing a square already drawn has its own, larger allowance: it
        // is bounded by the dig, not by the view, and every plan it waits
        // for is a morph's worth of old ground on screen.
        auto& budget = previous ? rebuildBudget : meshBudget;
        if (!budget) { deferred = true; return previous; }
        std::shared_ptr<const AdaptiveMesh> parent;
        // A regional root has no morph parent. Climbing above it waits for
        // pages outside this cut's requested dependencies and can stall forever.
        if (!isRoot(tile) && tile.lod < int(kGeometryLevels) - 1) {
            parent = mesh(parentTile(tile), residency,baseOnly);
            if (!parent) return previous;
            // Rebuilt against a parent still on the old ground, it would morph
            // towards ground that is gone. It waits for its parent.
            if (previous && stale.contains(tileKeyOf(parentTile(tile)))) { deferred = true; return previous; }
        }
        if (!budget) { deferred = true; return previous; }
        const auto side = metres(tile);
        const double ox = tile.x*side, oy = tile.y*side;
        const auto sourceLevel=dataLevel(tile);
        const bool features=!baseOnly && detail && sampleDetail &&
            (dataPolicy.stableFeatures?tile.lod==1:dataPolicy.cameraBudget?tile.lod==0:tile.lod<=2) && detail->hasFeatures(tile);
        const bool local=!baseOnly && detail && sampleDetail && tile.lod==0 && detail->localTouches(tile);
        // Camera LOD sets the nominal edge spacing, not which terrain features
        // we are allowed to notice. Probe the available source more finely so
        // a crest between coarse vertices cannot disappear before error testing.
        // Output triangles are still sparse; no finer height pages are requested
        // solely for tessellation. Each job remains capped at 256x256 cells.
        const int nominalStep=dataPolicy.stepAt(tile.lod);
        // The finest triangle this tile may ever contain, and it has to follow
        // the tile's OWN step.
        //
        // It used to be eight metres for every tile in the world - `max(8,
        // ...)` against a source resolution, with the tile's nominal step
        // reaching it only through a `min`. So a square whose level said "two
        // kilometres to a triangle" was still built on an eight-metre lattice
        // and could subdivide the whole way down to it, with the error test as
        // the only brake. The coarse end of the pyramid existed on paper and
        // nowhere else: raising the level ceiling changed which number was
        // printed and not one triangle of what was drawn.
        //
        // Half the tile's step is the floor now: one level of headroom, so a
        // crest that falls between two of the tile's own vertices can still be
        // found by the error test, and nothing beyond that. A square cannot
        // spend more than four times its own budget however interesting the
        // ground inside it is - which is what makes the ladder a ladder.
        //
        // And never coarser than an eighth of the tile, so a square always has
        // eight cells a side to spend if the error asks for them. Without that
        // the two collapse together at the levels where the tile stops growing
        // and the step keeps going - a square would be two cells, and a crest
        // between its own corners could not be found at any tolerance.
        //
        // So a square is at most a hundred and twenty-eight triangles, at every
        // level, and more detail is bought by having more and SMALLER squares -
        // which is the decision the level was chosen to make. That is the
        // ladder: eight metres to a triangle underfoot and two hundred and
        // fifty-six on a square two kilometres across, with the same cap on
        // what any one of them may spend.
        // Eight metres stays the floor at the fine end - the runtime landscape
        // stops at H8 geometry and always has - but it is no longer also the
        // floor at the COARSE end, which is what it had become.
        const int step = dataPolicy.cameraBudget
            ? std::max({8,int(side/256),std::min<int>(nominalStep/2,int(side/8))})
            : std::max(int(side/256),features?2:local?4:4 << sourceLevel);
        const auto sampleStage = [&](double x, double y, generation::TerrainStage stage) -> std::array<float,2> {
            x += ox; y += oy;
            if (world.terrainFoundation && stage<generation::TerrainStage::Rivers) {
                const bool outside=x<0 || y<0 || x>=world.width*generation::kMetresPerCell || y>=world.height*generation::kMetresPerCell;
                const float h=outside?-60.0f:float(world.terrainFoundation->sample(
                    core::Fixed::fromDoubleForContent(x),core::Fixed::fromDoubleForContent(y),stage).toDouble());
                float head=-6000;
                if (stage==generation::TerrainStage::Water) {
                    // A lake is LEVEL, and its edge is where the ground comes up
                    // through that level - not where the macro lattice ends.
                    //
                    // This used to read the one cell the point fell in. A cell
                    // is five hundred and forty metres, and the cell beside a
                    // lake has a head of minus six thousand, so the water
                    // stopped dead on a cell boundary however the ground ran:
                    // every lake came out as a staircase of square blocks, and
                    // a small one came out as a single square.
                    //
                    // The four cells around the point are asked instead, and
                    // the highest water among them wins. A body's level is one
                    // number over all its cells, so this changes nothing inside
                    // a lake; what it does is carry that level half a cell past
                    // the footprint, which is exactly the margin the waterline
                    // needs to land on the ground instead of on the lattice.
                    const double cell=generation::kMetresPerCell;
                    const int mx=int(std::floor(x/cell-0.5)),my=int(std::floor(y/cell-0.5));
                    bool sea=outside;
                    for (int dy=0;dy<2 && !sea;++dy) for (int dx=0;dx<2 && !sea;++dx) {
                        const int cx=std::clamp(mx+dx,0,world.width-1);
                        const int cy=std::clamp(my+dy,0,world.height-1);
                        const auto i=std::size_t(cy)*world.width+cx;
                        if (world.cells[i].sea) { sea=true; break; }
                        if (i<world.lakeDepthField.size() && world.lakeDepthField[i]>0 &&
                            i<world.lakeLevelField.size())
                            head=std::max(head,float(world.lakeLevelField[i]*
                                                     generation::kMetresPerElevationStep));
                    }
                    if (sea) head=0;
                }
                return {h,head};
            }
            const double pageMetres = streaming::pageMetresAtLevel(sourceLevel);
            const int px = int(std::floor(x/pageMetres)), py = int(std::floor(y/pageMetres));
            const auto it = residency.surfaces.find({px,py,sourceLevel});
            if (it == residency.surfaces.end()) return {-60,0}; // sparse ocean entry
            const auto& p = *it->second;
            const double gx=(x-px*pageMetres)/p.step+p.padding,gy=(y-py*pageMetres)/p.step+p.padding;
            const int ix=std::clamp(int(std::floor(gx)),0,p.side-2),iy=std::clamp(int(std::floor(gy)),0,p.side-2);
            const auto i = std::size_t(iy)*p.side+ix;
            const double fx=gx-ix,fy=gy-iy;
            const auto interpolate=[&](const auto& h) {
                return float(fx+fy<=1 ? h[i]*(1-fx-fy)+h[i+1]*fx+h[i+p.side]*fy :
                    h[i+p.side+1]*(fx+fy-1)+h[i+1]*(1-fy)+h[i+p.side]*(1-fx));
            };
            const std::array base{interpolate(p.bed),interpolate(p.head)};
            return features||local ? detail->sample(x,y,base,local) : base;
        };
        const SurfaceSample sample=[&](double x,double y){return sampleStage(x,y,view.stageTo);};
        const HeightSample prior=[&](double x,double y){return sampleStage(x,y,view.stageFrom)[0];};
        SurfaceSample target;
        HeightSample priorTarget;
        if (parent) {
            const auto ancestor = parentTile(tile);
            const double dx = ox - ancestor.x*metres(ancestor), dy = oy - ancestor.y*metres(ancestor);
            target = [parent, dx, dy](double x, double y) { return parent->sample(x+dx,y+dy); };
            priorTarget = [parent,dx,dy](double x,double y){return parent->samplePrior(x+dx,y+dy);};
        }
        SurfaceTolerance precision;
        if (features && dataPolicy.stableFeatures)
            precision=[&](double x,double y){return detail->tolerance(x+ox,y+oy);};
        // How much error this tile's triangles may carry, and the answer is a
        // NUMBER OF PIXELS rather than a number of metres.
        //
        // It used to be half a metre for every tile on the runtime path, which
        // is the same allowance for ground underfoot and ground five kilometres
        // away. Half a metre underfoot is right; half a metre at five
        // kilometres is a twentieth of a pixel, and buying it costs a tile
        // three times the triangles it needs - measured on the same country,
        // thirteen hundred triangles against four hundred, with the worst error
        // going from forty centimetres to ninety. Neither is visible. One costs
        // three times the other.
        //
        // So it is tied to what a pixel is worth where the tile actually is,
        // which is the same currency every other decision here is made in.
        // Bounded at both ends: never finer than a quarter metre, because below
        // that it is measuring the source's own noise, and never coarser than
        // sixty-four, because a tile has to keep its own silhouette whatever
        // the camera is doing.
        double tolerance=dataPolicy.targeted ? (features||local?0.25:
            dataLevel(tile.lod)>=2?std::min(8.0,sampleMetresAt(tile.lod)/8.0):0.5) :
            std::min(4.0,sampleMetresAt(tile.lod)/32.0);
        if (dataPolicy.cameraBudget && !features && !local) {
            const ViewBounds box{double(ox),double(oy),double(ox+side),double(oy+side),
                                 -100.0,4000.0};
            // The real metres-per-pixel, not the one divided by the
            // detail-distance dial. That dial says how far out to keep FINE
            // TILES; letting it into the error budget as well applies it twice
            // and makes every tile eight times tighter than a pixel warrants.
            const double metresPerPixel=view.metresPerPixel(box);
            if (metresPerPixel>0.0)
                tolerance=std::clamp(metresPerPixel*view.config.lod.refineErrorPixels,0.25,64.0);
        }
        // LOD errors always come from the same unrefined source in both stages.
        // Final morphs need centimetres, not a millimetre-perfect parent copy.
        AdaptiveCriteria criteria;
        if (dataPolicy.cameraBudget) {
            // Eight mandatory points a side, and everything finer earned by
            // shape. Forcing the whole source lattice onto every boundary was
            // most of the landscape's geometry and almost none of its form: a
            // 32-cell chunk paid 256 skirt triangles and a ring of interior
            // splits to hold them, flat or not. Measured on the same cameras,
            // the wide steppe fell from 443k triangles to 161k and the mountain
            // view from 481k to 165k, with the frames matching to a mean of
            // 0.08 of one 8-bit level - the difference between a plane drawn
            // with two triangles and a plane drawn with a thousand.
            // A ridge or a river crossing an edge still splits it: only the
            // unconditional points are gone, and stitchEdges has resolved
            // unequal edge spacing since it started constraining segments.
            const int cells=int(side/step);
            criteria.boundaryStride=std::clamp(std::max(nominalStep/step,cells/8),1,cells);
            criteria.normalErrorDegrees=10;
        }
        auto result = makeAdaptiveMesh(int(side/step),step,tolerance,sample,target,
            dataPolicy.targeted && !baseOnly ? 0.25 : 0.001,precision,prior,priorTarget,criteria);
        if (world.terrainFoundation) {
            auto annotated=std::make_shared<AdaptiveMesh>(*result);
            const auto& f=*world.terrainFoundation;
            const auto previous=generation::TerrainStage(std::max(0,int(view.stageTo)-1));
            for (auto& v:annotated->vertices) {
                const auto x=core::Fixed::fromDoubleForContent(ox+v.x*annotated->step),
                    y=core::Fixed::fromDoubleForContent(oy+v.y*annotated->step);
                // The foundation's own spacing, not sixty-four: a world too big
                // to hold at sixty-four metres is held at a coarser step, and
                // indexing it as though it were not reads the wrong cell.
                const auto i=std::size_t(std::clamp(int(y.toInt()/f.step),0,f.rows-1))*f.columns+
                    std::clamp(int(x.toInt()/f.step),0,f.columns-1);
                const float before=previous<generation::TerrainStage::Rivers ? float(f.sample(x,y,previous).toDouble()) : v.sourceBed;
                const float base=float(f.sample(x,y,generation::TerrainStage::Volcanoes).toDouble());
                const auto macro=std::size_t(std::clamp(int(y.toInt()/generation::kMetresPerCell),0,world.height-1))*world.width+
                    std::clamp(int(x.toInt()/generation::kMetresPerCell),0,world.width-1);
                v.diagnostic={v.sourceBed-before,view.stageTo>=generation::TerrainStage::Slopes?std::max(0.0f,base-v.sourceBed):0.0f,
                    float(std::log2(double(f.accumulation[i]))),macro<world.rockTypeField.size()?float(world.rockTypeField[macro]):0.0f};
                if (f.receiver[i]>=0) {
                    const int r=f.receiver[i];
                    v.drainage={float(r%f.columns-int(i%f.columns)),float(r/f.columns-int(i/f.columns))};
                }
            }
            result=std::move(annotated);
        }
        --budget;
        // Its children stay as they are: what they morph into is the parent's
        // surface over their own square, and the pages under that are in
        // their own dependencies - a child the dig reached is stale itself.
        stale.erase(id);
        return cache.insert_or_assign(id,std::move(result)).first->second;
    }

    std::uint8_t dataLevel(int lod) const {
        return dataLevelForGeometryLevel(lod, dataPolicy);
    }
    std::uint8_t dataLevel(TileId tile) const {
        if (dataPolicy.cameraBudget && world.terrainFoundation && dataLevel(tile.lod)<4) {
            const int side=int(metres(tile)),x=tile.x*side,y=tile.y*side;
            bool feature=world.terrainFoundation->needsDetail(x-64,y-64,x+side+64,y+side+64);
            // Narrow channels may fall between H64 samples. Route evidence is
            // conservative across the full valley halo, never just four probes.
            constexpr int macro=generation::kMetresPerCell;
            for (int cy=std::max(0,(y-800)/macro);!feature && cy<=std::min(world.height-1,(y+side+800)/macro);++cy)
                for (int cx=std::max(0,(x-800)/macro);cx<=std::min(world.width-1,(x+side+800)/macro);++cx) {
                    const auto i=std::size_t(cy)*world.width+cx;
                    if ((i<world.riverDischargeField.size() && world.riverDischargeField[i]>0) ||
                        (i<world.lakeDepthField.size() && world.lakeDepthField[i]>0)) { feature=true; break; }
                }
            if (!feature) return 4;
        }
        if (dataPolicy.stableFeatures && detail && tile.lod<=2 && detail->hasFeatures(tile)) return 2;
        return dataLevel(tile.lod);
    }
    bool streamedChildren(TileId parent) const {
        return parent.lod > 0 && dataLevel(parent.lod - 1) < 3;
    }

    const Node& bounds(TileId tile) {
        const auto id = tileKeyOf(tile);
        if (auto it = nodes.find(id); it != nodes.end()) return it->second;
        const auto side = metres(tile), x = tile.x * side, y = tile.y * side;
        const bool land = pages.landMask().anyLandInWorldRect(int(x), int(y), int(x + side), int(y + side));
        const auto height = world.terrainFoundation ? std::pair{-600.0,6000.0} : land ? ringHeightBounds(world,
            {core::Fixed::fromInt(x), core::Fixed::fromInt(y)},
            {core::Fixed::fromInt(x + side), core::Fixed::fromInt(y + side)}, std::min(6, tile.lod + 1))
            : std::pair{-60.0, 0.0};
        return nodes.emplace(id, Node{{double(x), double(y), double(x + side), double(y + side),
                                      skirtFloor(height.first), height.second}, land}).first->second;
    }
    const std::vector<Key>& keys(TileId tile) {
        const auto id = tileKeyOf(tile);
        if (auto it = dependencies.find(id); it != dependencies.end()) return it->second;
        std::vector<Key> result;
        const auto side = metres(tile);
        // Includes parent triangle corners and their normal-sampling halo.
        const auto fineHalo = 2 * sampleMetresAt(std::min(6, tile.lod + 1));
        int previous = -1;
        for (int lod : {tile.lod, isRoot(tile) ? tile.lod : std::min(int(kGeometryLevels) - 1, tile.lod + 1)}) {
            const TileId source=lod==tile.lod?tile:parentTile(tile);
            const auto level = dataLevel(source);
            if (level == previous) continue;
            previous = level;
            const std::int64_t pageMetres = streaming::pageMetresAtLevel(level);
            // A coarse page's normals and its parent's corners are read a
            // triangle's step past the square: the neighbouring page with them.
            const std::int64_t halo = level > 4 ? std::max<std::int64_t>(fineHalo,
                2 * dataPolicy.stepAt(std::min(int(kGeometryLevels) - 1, tile.lod + 1))) : fineHalo;
            for (auto y = floorDiv(tile.y * side - halo, pageMetres); y <= floorDiv((tile.y + 1) * side + halo, pageMetres); ++y)
                for (auto x = floorDiv(tile.x * side - halo, pageMetres); x <= floorDiv((tile.x + 1) * side + halo, pageMetres); ++x) {
                    Key key{int(x), int(y), level};
                    if (pages.containsLand(key)) result.push_back(key);
                }
        }
        return dependencies.emplace(id, std::move(result)).first->second;
    }
    bool visible(TileId tile, const TerrainView& view) {
        const auto& node = bounds(tile);
        return node.land && intersectsView(node.box, view.matrix,
            4.0 / std::max(1, view.width), 4.0 / std::max(1, view.height));
    }
};
}

struct TerrainPlanner::Impl {
    struct Region {
        TileId root;
        TerrainCut cut;
        std::vector<Key> required;
        bool limited = false, blank = false;
        bool deferred = false;
    };
    struct Job {
        TerrainView view;
        std::shared_ptr<const TerrainResidency> residency;
        std::shared_ptr<TerrainPlan> plan = std::make_shared<TerrainPlan>();
        std::array<bool, 2> claimed{};
        int remaining = 2;
        bool restart = false, failed = false;
        std::array<Keys, 3> working;
#if ASR_ENABLE_DIAGNOSTICS
        std::array<double, 2> stageMs{};
#endif
        std::unordered_map<std::int64_t, double> targetTiles;
        Keys targetPages;
        std::vector<Key> prediction;
#if ASR_ENABLE_DIAGNOSTICS
        Clock::time_point started = Clock::now();
#endif
    };

    streaming::TerrainWorkerPool& pool;
    streaming::TerrainWorkerPool::Handle source;
    mutable std::mutex mutex;
    std::shared_ptr<Job> job, pending;
    std::shared_ptr<const TerrainPlan> done, last;
    std::uint64_t sequence = 0;
    Stats counts;
    bool closing = false;

    // Stage 0 owns topology, admission and the inverse page -> root index.
    Geometry cutGeometry;
    std::vector<Region> regions;
    std::unordered_map<Key, std::unordered_set<std::size_t>> dependents;
    std::shared_ptr<const TerrainResidency> previousResidency;
    TerrainView previousView;
    double previousTime = 0;
    std::vector<TerrainPlan::Block> previousEdges;
    // Stage 1 owns the final-view/prediction cache, independent of topology.
    Geometry viewGeometry;
    TerrainView cachedView;
    bool haveView = false;
    std::uint64_t cachedResidencyRevision = 0;
    std::unordered_map<std::int64_t, double> cachedTiles;
    Keys cachedPages;
    std::vector<Key> cachedPrediction;

    Impl(const generation::WorldMapData& world, streaming::PageStore& pages, std::vector<TileId> roots,
         DataLodPolicy dataPolicy)
        : pool(pages.workerPool()), cutGeometry{world, pages, dataPolicy},
          viewGeometry{world, pages, dataPolicy, false} {
        for (auto root : roots) {
            regions.push_back({root, {}, {}});
            cutGeometry.roots.insert(tileKeyOf(root));
            viewGeometry.roots.insert(tileKeyOf(root));
        }
        source = pool.add([this](std::size_t) { return work(); }, streaming::TerrainWorkerPool::Task::Visible);
    }
    ~Impl() {
        { std::lock_guard lock(mutex); closing = true; }
        pool.remove(source); // never wait for sibling jobs on a worker
    }

    void cut(Job& job) {
        auto& plan = *job.plan;
        const auto& view = job.view;
        const bool sameStages = previousView.stageFrom == view.stageFrom && previousView.stageTo == view.stageTo;
        cutGeometry.prepare(view,std::clamp(plan.time-previousTime,0.0,0.1));
        cutGeometry.meshBudget = cutGeometry.dataPolicy.cameraBudget ? view.config.meshesPerPlan :
            std::numeric_limits<std::size_t>::max();
        const std::size_t rebuildLimit = view.config.meshesPerPlan * 8;
        cutGeometry.rebuildBudget = cutGeometry.dataPolicy.cameraBudget ? rebuildLimit :
            std::numeric_limits<std::size_t>::max();
        const auto& resident = job.residency->pages;
        cutGeometry.meshUses.clear();
        const bool newView = !previousResidency || previousView != view || job.restart;
        const bool newCapacity = !previousResidency || previousResidency->capacity != job.residency->capacity;
        std::unordered_set<std::size_t> dirty;
        std::unordered_set<std::size_t> lost;
        if (previousResidency && previousResidency != job.residency) {
            const auto changed = [&](const Keys& a, const Keys& b, bool removed) {
                for (auto key : a) if (!b.contains(key))
                    if (auto it = dependents.find(key); it != dependents.end()) {
                        dirty.insert(it->second.begin(), it->second.end());
                        if (removed) lost.insert(it->second.begin(), it->second.end());
                    }
            };
            changed(previousResidency->pages, resident, true);
            changed(resident, previousResidency->pages, false);
        }
        // The same pages on other ground: their roots are cut again, over
        // meshes built afresh.
        for (const auto key : cutGeometry.observeGround(*job.residency))
            if (auto it = dependents.find(key); it != dependents.end()) dirty.insert(it->second.begin(), it->second.end());
        // Pages a cut stood on are gone (the atlas let them go, or a plan was
        // refused because its pins were). Each square that has lost its pages
        // falls back to its nearest ancestor that still has all of its own,
        // and nothing else in the cut moves.
        //
        // This used to clear the region's whole cut. A world root is 65 km, so
        // the atlas letting go of a few H8 pages a kilometre behind the eye -
        // which it does on every walk - threw the square under the pawn from
        // 8 m triangles to a 1 km slab at once, and walked it back down every
        // level over the next seconds: the ground rebuilding under the feet.
        const auto pagesReady = [&](TileId tile) {
            for (const auto key : cutGeometry.keys(tile)) if (!resident.contains(key)) return false;
            return true;
        };
        std::size_t foldedSquares = 0, clearedRegions = 0;
        const auto fold = [&](Region& region) {
            std::vector<TileId> standing;
            for (const auto& block : region.cut.coverage()) standing.push_back(block.tile);
            for (const auto parent : region.cut.activeParents()) standing.push_back(parent);
            std::unordered_set<std::int64_t> collapsed;
            for (const auto tile : standing) {
                if (pagesReady(tile)) continue;
                auto ancestor = tile;
                while (!pagesReady(ancestor) && !cutGeometry.isRoot(ancestor) &&
                       ancestor.lod < int(kGeometryLevels) - 1)
                    ancestor = cutGeometry.parentTile(ancestor);
                if (!pagesReady(ancestor)) {      // not even the root: nothing to stand on
                    region.cut.clear();
                    ++clearedRegions;
                    return;
                }
                if (collapsed.insert(tileKeyOf(ancestor)).second) {
                    region.cut.collapse(ancestor, [this](TileId t) { return cutGeometry.children(t); });
                    ++foldedSquares;
                }
            }
        };
        if (job.restart) for (auto& region : regions) fold(region);
        // A discarded/stale plan may have advanced a transition whose pages
        // were never pinned by the renderer. Fall back rather than drawing it.
        else for (auto index : lost) fold(regions[index]);
        static const bool underfoot = std::getenv("ASR_TERRAIN_UNDERFOOT") != nullptr;
        if (underfoot && (job.restart || !lost.empty())) {
            std::size_t gone = 0;
            if (previousResidency) for (auto key : previousResidency->pages) gone += !resident.contains(key);
            std::fprintf(stderr, "underfoot FOLD restart=%d lost-regions=%zu of %zu, pages gone=%zu, squares folded=%zu, "
                         "regions cleared=%zu\n", int(job.restart), lost.size(), regions.size(), gone,
                         foldedSquares, clearedRegions);
        }
        const double dt = std::clamp(plan.time - previousTime, 0.0, 0.1);
        const auto priority = [&](TileId tile) {
            const double side = cutGeometry.metres(tile);
            return focusDistanceSquared({tile.x * side, tile.y * side,
                (tile.x + 1) * side, (tile.y + 1) * side}, view.x, view.y);
        };
        const auto family = [&](TileId parent) {
            Keys keys;
            for (auto child : cutGeometry.children(parent))
                for (auto key : cutGeometry.keys(child)) if (key.level < 3) keys.insert(key);
            return keys;
        };
        std::array<Keys, 3> working;
        for (const auto& region : regions)
            for (auto parent : region.cut.activeParents()) if (cutGeometry.streamedChildren(parent))
                for (auto key : family(parent)) working[key.level].insert(key);
        bool released = false;
        const auto reuse = [&](std::size_t index) {
            const auto& region = regions[index];
            const bool retryCapacity = region.limited && previousResidency != job.residency;
            return !newView && !newCapacity && !dirty.contains(index) && !region.cut.needsUpdate() &&
                !region.deferred && !retryCapacity;
        };
        // Cached waiting families retain their admission even before their
        // first child is drawable. Don't give the same slots to another root.
        // Roots past the window hold nothing: no cut, no pages, no meshes. The
        // ground there is past the fog, and a world a thousand kilometres long
        // must not keep every page of itself for a view that sees forty.
        const auto beyond = [&](std::size_t index) {
            const auto& box = cutGeometry.bounds(regions[index].root).box;
            return view.beyondWindow(box.minX, box.minY, box.maxX, box.maxY);
        };
        for (std::size_t i = 0; i < regions.size(); ++i) {
            if (!beyond(i)) continue;
            auto& region = regions[i];
            if (region.required.empty() && region.cut.drawing().empty()) continue;
            for (auto key : region.required)
                if (auto it = dependents.find(key); it != dependents.end()) {
                    it->second.erase(i);
                    if (it->second.empty()) dependents.erase(it);
                }
            region.required.clear();
            region.cut.clear();
            released = true;
        }
        for (std::size_t i = 0; i < regions.size(); ++i) if (!beyond(i) && reuse(i))
            for (auto key : regions[i].required) if (key.level < 3) working[key.level].insert(key);
        std::vector<std::size_t> ordered(regions.size());
        std::iota(ordered.begin(), ordered.end(), 0);
        std::stable_sort(ordered.begin(), ordered.end(), [&](auto a, auto b) {
            return priority(regions[a].root) < priority(regions[b].root);
        });
        // Coverage before detail. Otherwise the nearest root can consume every
        // batch refining its children while other visible roots stay blank.
        if (cutGeometry.dataPolicy.cameraBudget && !job.residency->surfaces.empty())
            for (auto index : ordered) {
                const auto root = regions[index].root;
                if (cutGeometry.visible(root, view)) cutGeometry.mesh(root, *job.residency);
            }
        for (auto index : ordered) {
            auto& region = regions[index];
            if (beyond(index)) continue;
            if (reuse(index)) {
                ++plan.rootsReused;
            } else {
                ++plan.rootsVisited;
                cutGeometry.deferred = false;
                for (auto key : region.required) {
                    auto it = dependents.find(key);
                    if (it != dependents.end()) {
                        it->second.erase(index);
                        if (it->second.empty()) dependents.erase(it);
                    }
                }
                region.required.clear();
                region.limited = false;
                Keys requested;
                std::unordered_map<std::int64_t, bool> readiness;
                const auto ready = [&](TileId tile) {
                    auto [it, fresh] = readiness.try_emplace(tileKeyOf(tile), true);
                    if (fresh) for (auto key : cutGeometry.keys(tile)) {
                        if (requested.insert(key).second) region.required.push_back(key);
                        it->second = resident.contains(key) && it->second;
                    }
                    // Admit a complete sibling family only when its meshes,
                    // not just pages, are ready. Keep drawing the old parent
                    // while small camera-prioritized batches fill the cache.
                    if (fresh && it->second && cutGeometry.dataPolicy.cameraBudget &&
                        !job.residency->surfaces.empty())
                        it->second = bool(cutGeometry.mesh(tile,*job.residency,false,true));
                    return it->second;
                };
                const auto admit = [&](TileId parent) {
                    if (!cutGeometry.streamedChildren(parent)) return true;
                    const auto keys = family(parent);
                    std::array<std::size_t, 3> extra{};
                    for (auto key : keys) if (!working[key.level].contains(key)) ++extra[key.level];
                    for (std::size_t level = 0; level < 3; ++level)
                        if (working[level].size() + extra[level] > job.residency->capacity[level]) {
                            region.limited = true;
                            return false;
                        }
                    for (auto key : keys) working[key.level].insert(key);
                    return true;
                };
                const auto visible = [&](TileId tile) { return cutGeometry.visible(tile, view); };
                const auto refine = [&](TileId tile, bool children) {
                    return cutGeometry.refine(tile,*job.residency,view,children);
                };
                const auto parentsBefore = region.cut.activeParents().size();
                region.cut.update({region.root}, view.target, visible, ready, dt, admit, priority, true, refine,
                                  view.config.morphSeconds, [this](TileId tile) { return cutGeometry.children(tile); });
                released = released || parentsBefore > region.cut.activeParents().size();
                auto blocks = region.cut.drawing();
                std::stable_sort(blocks.begin(), blocks.end(), [&](auto a, auto b) { return priority(a.tile) < priority(b.tile); });
                for (const auto& block : blocks) {
                    if (!refine(block.tile, false) || !visible(block.tile) || !admit(block.tile)) continue;
                    for (auto child : cutGeometry.children(block.tile)) ready(child);
                }
                region.blank = visible(region.root) && !ready(region.root);
                for (auto key : region.required) dependents[key].insert(index);
                region.deferred = cutGeometry.deferred;
            }
            for (auto key : region.required) if (plan.demanded.insert(key).second) {
                plan.required.push_back(key);
                (resident.contains(key) ? plan.pins : plan.missing).push_back(key);
            }
            for (auto block : region.cut.coverage()) {
                const auto& node = cutGeometry.bounds(block.tile);
                const auto data = cutGeometry.dataLevel(block.tile);
                const auto parentData = cutGeometry.isRoot(block.tile)?data:cutGeometry.dataLevel(cutGeometry.parentTile(block.tile));
                if (node.land)
                    plan.coverage.push_back({block.tile, block.parentMorph, float(node.box.low), node.box,
                        data, parentData, (cutGeometry.detail && cutGeometry.detail->hasFeatures(block.tile)) ||
                            job.residency->mayHaveWater(block.tile, data, parentData,cutGeometry.dataPolicy.chunkCells,
                                cutGeometry.metres(block.tile)),
                        cutGeometry.mesh(block.tile,*job.residency),cutGeometry.dataPolicy.cellsAt(block.tile.lod),
                        cutGeometry.metres(block.tile)});
                if (node.land && cutGeometry.staleMesh(block.tile)) ++plan.staleMeshes;
            }
            plan.needsUpdate = plan.needsUpdate || region.cut.needsUpdate();
            plan.needsUpdate = plan.needsUpdate || region.deferred;
            plan.deferredRegions += region.deferred;
            plan.capacityLimited = plan.capacityLimited || region.limited;
            plan.blank += region.blank;
        }
        // Reverse morphs can release reservations after an earlier root was
        // refused admission. Give it one more pass even with identical pages.
        if (released && plan.capacityLimited) {
            plan.needsUpdate = true;
            previousResidency.reset();
        } else previousResidency = job.residency;
        previousView = view;
        previousTime = plan.time;
        if (cutGeometry.detail) {
            plan.detail=cutGeometry.detail->stats();
            plan.needsUpdate=plan.needsUpdate||cutGeometry.detail->transitioning();
        }
        cutGeometry.trimCache(view.config.meshCacheBytes);
        cutGeometry.trimStale();
        // Squares still drawn over ground that has moved: not finished yet.
        plan.needsUpdate = plan.needsUpdate || plan.staleMeshes > 0;
        if (cutGeometry.dataPolicy.cameraBudget)
            plan.meshesBuilt = view.config.meshesPerPlan-cutGeometry.meshBudget + rebuildLimit-cutGeometry.rebuildBudget;
        job.working = std::move(working);
        plan.stitchEdges(previousEdges);
        auto oldEdges = std::move(previousEdges);
        previousEdges = plan.coverage;
        if (!job.restart && sameStages && view.config.morphSeconds > 0) plan.interpolateFrom(oldEdges);
        plan.selectDrawing(view, plan.drawing);
        // The coarsest first - H1024, H256, then H64, then the fine levels -
        // so a wide view is covered by its coarse ground before any of it is
        // refined, and nearer first within a level.
        const auto rank = [](Key k) { return k.level >= 4 ? 8 - int(k.level) : 100 - int(k.level); };
        std::stable_sort(plan.missing.begin(), plan.missing.end(), [&](Key a, Key b) {
            if (rank(a) != rank(b)) return rank(a) < rank(b);
            return nearer(a, b, view.x, view.y);
        });
    }

    void analyze(Job& job) {
        const auto& view = job.view;
        viewGeometry.prepare(view,0);
        viewGeometry.observeGround(*job.residency);
        if (!haveView || cachedView != view ||
            (!job.residency->surfaces.empty() && cachedResidencyRevision != job.residency->revision)) {
            haveView = false; // an allocation failure must not publish a partial cache
            viewGeometry.meshUses.clear();
            cachedTiles.clear(); cachedPages.clear(); cachedPrediction.clear();
            const double span = viewGeometry.metres({0,0,view.target});
            const double preloadMargin = viewGeometry.dataPolicy.cameraBudget ? view.config.preloadMarginPixels : 256.0;
            const auto visit = [&](double x, double y, bool prediction, auto&& apply) {
                if (view.perspective() || !job.residency->surfaces.empty()) {
                    auto projected = view;
                    if (prediction) projected.matrix = view.prediction;
                    const double margin = prediction ? preloadMargin : 4.0;
                    const auto walk = [&](auto&& self, TileId tile) -> void {
                        const auto& node = viewGeometry.bounds(tile);
                        if (!node.land || !intersectsView(node.box, projected.matrix,
                            margin / std::max(1, view.width), margin / std::max(1, view.height))) return;
                        const bool refine = viewGeometry.refine(tile,*job.residency,projected);
                        if (refine)
                            for (auto child : viewGeometry.children(tile)) self(self, child);
                        else apply(tile);
                    };
                    for (const auto& region : regions) {
                        const auto& box = viewGeometry.bounds(region.root).box;
                        if (!view.beyondWindow(box.minX, box.minY, box.maxX, box.maxY)) walk(walk, region.root);
                    }
                    return;
                }
                // Never cache an unbounded ocean trail when the camera teleports.
                const int wide = int(std::ceil(viewGeometry.world.width * generation::kMetresPerCell / span));
                const int high = int(std::ceil(viewGeometry.world.height * generation::kMetresPerCell / span));
                for (int row = std::max(0, int(std::floor((y - view.radius) / span)));
                     row <= std::min(high - 1, int(std::floor((y + view.radius) / span))); ++row)
                    for (int col = std::max(0, int(std::floor((x - view.radius) / span)));
                         col <= std::min(wide - 1, int(std::floor((x + view.radius) / span))); ++col)
                        apply(TileId{col, row, view.target});
            };
            visit(view.x, view.y, false, [&](TileId tile) {
                if (!viewGeometry.visible(tile, view)) return;
                cachedTiles.try_emplace(tileKeyOf(tile), 0);
                const auto& keys = viewGeometry.keys(tile);
                cachedPages.insert(keys.begin(), keys.end());
            });
            std::vector<TileId> candidates;
            if (!viewGeometry.dataPolicy.cameraBudget || view.config.preloadPages > 0)
                visit(view.lookX, view.lookY, true, [&](TileId tile) {
                const auto& node = viewGeometry.bounds(tile);
                if (node.land && intersectsView(node.box, view.prediction,
                    preloadMargin / std::max(1, view.width),
                    preloadMargin / std::max(1, view.height))) candidates.push_back(tile);
            });
            std::stable_sort(candidates.begin(), candidates.end(), [&](auto a, auto b) {
                return focusDistanceSquared(viewGeometry.bounds(a).box, view.lookX, view.lookY) <
                       focusDistanceSquared(viewGeometry.bounds(b).box, view.lookX, view.lookY);
            });
            Keys seen;
            for (auto tile : candidates) {
                auto keys = viewGeometry.keys(tile);
                std::stable_sort(keys.begin(), keys.end(), [&](Key a, Key b) { return nearer(a, b, view.lookX, view.lookY); });
                for (auto key : keys) if (seen.insert(key).second) cachedPrediction.push_back(key);
            }
            cachedView = view;
            cachedResidencyRevision = job.residency->revision;
            viewGeometry.trimCache(view.config.meshCacheBytes / 4);
            viewGeometry.trimStale();
            haveView = true;
        }
        job.targetTiles = cachedTiles;
        job.targetPages = cachedPages;
        job.prediction = cachedPrediction;
    }

    void finish(Job& job) {
#if ASR_ENABLE_DIAGNOSTICS
        const auto started = Clock::now();
#endif
        auto& plan = *job.plan;
        plan.wanted = plan.demanded;
        for (auto key : plan.required) if (key.level < 3) job.working[key.level].insert(key);
        for (auto key : job.prediction) {
            if (plan.preload.size() >= (cutGeometry.dataPolicy.cameraBudget ? job.view.config.preloadPages : 128)) break;
            if (plan.wanted.contains(key)) continue;
            if (key.level < 3) {
                auto& working = job.working[key.level];
                if (!working.contains(key) && working.size() >= job.residency->capacity[key.level]) continue;
                working.insert(key); // reserve each speculative slot, including resident predictions
            }
            plan.wanted.insert(key);
            if (!job.residency->pages.contains(key)) plan.preload.push_back(key);
        }
        for (const auto& block : plan.drawing) {
            plan.coarse += cutGeometry.refine(block.tile,*job.residency,plan.view,false,true);
            plan.morphing += block.parentMorph > 0;
        }
        // A goal may straddle the frustum. Its complete covering cut, not
        // only the currently drawn children, determines whether it is ready.
        for (const auto& block : plan.coverage) {
            // Perspective goals are leaves of different depths. Find the goal
            // containing this block, rather than assuming one global level.
            auto target = block.tile;
            for (int lod = block.tile.lod; lod < int(kGeometryLevels);
                 ++lod, target = cutGeometry.parentTile(target)) {
                if (auto it = job.targetTiles.find(tileKeyOf(target)); it != job.targetTiles.end()) {
                    const double fraction = double(block.metres()) / cutGeometry.metres(target);
                    it->second += fraction*fraction*(block.tile.lod==lod ? 1.0-block.parentMorph : 1.0);
                    break;
                }
            }
        }
        plan.viewMesh = {0, job.targetTiles.size(), true};
        for (const auto& [id, coverage] : job.targetTiles) plan.viewMesh.ready += std::min(1.0, coverage);
        plan.viewPages = {0, job.targetPages.size(), true};
        for (auto key : job.targetPages) plan.viewPages.ready += job.residency->pages.contains(key);
#if ASR_ENABLE_DIAGNOSTICS
        plan.buildMs = job.stageMs[0] + job.stageMs[1] +
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        plan.latencyMs = std::chrono::duration<double, std::milli>(Clock::now() - job.started).count();
#endif
    }

    bool work() {
        std::shared_ptr<Job> current;
        std::size_t stage;
        {
            std::lock_guard lock(mutex);
            if (closing || !job) return false;
            for (stage = 0; stage < 2 && job->claimed[stage]; ++stage) {}
            if (stage == 2) return false;
            current = job;
            current->claimed[stage] = true;
        }
        bool failed = false;
#if ASR_ENABLE_DIAGNOSTICS
        const auto started = Clock::now();
#endif
        try { if (stage == 0) cut(*current); else analyze(*current); }
        catch (...) { failed = true; }
#if ASR_ENABLE_DIAGNOSTICS
        current->stageMs[stage] = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
#endif
        {
            std::lock_guard lock(mutex);
            current->failed = current->failed || failed;
            if (--current->remaining) return true;
        }
        try { if (!current->failed) finish(*current); }
        catch (...) { current->failed = true; }
        // All stage writes precede the last completion lock; consumers see
        // either the entire immutable plan or nothing. No partial cut escapes.
        if (current->failed) {
            previousResidency.reset();
            for (auto& region : regions) region.cut.clear();
        }
        {
            std::lock_guard lock(mutex);
            if (current->failed) ++counts.failed;
            else {
                ++counts.completed;
#if ASR_ENABLE_DIAGNOSTICS
                counts.lastBuildMs = current->plan->buildMs;
                counts.lastLatencyMs = current->plan->latencyMs;
#endif
                if (!closing && current->plan->sequence == sequence) done = current->plan;
                else ++counts.discarded;
            }
            // Stale publication is not lost geometry. cut() checks the new
            // residency against its dependency index and drops only roots whose
            // pages vanished. Additive arrivals must not restart every morph.
            if (pending && current->failed) pending->restart = true;
            job = closing ? nullptr : std::exchange(pending, {});
            if (job) ++counts.submitted;
        }
        return true;
    }
};

TerrainPlanner::TerrainPlanner(const generation::WorldMapData& world, streaming::PageStore& pages,
                               std::vector<TileId> roots, DataLodPolicy dataPolicy)
    : impl_(std::make_unique<Impl>(world, pages, std::move(roots), dataPolicy)) {}
TerrainPlanner::~TerrainPlanner() = default;

bool TerrainPlanner::request(TerrainView view, std::shared_ptr<const TerrainResidency> residency,
                             double time, bool restart) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closing || !residency) return false;
        const auto& latest = impl_->pending ? impl_->pending : impl_->job;
        if (latest && latest->view == view && latest->residency->revision == residency->revision &&
            (!restart || latest->restart)) return false;
        if (!latest && impl_->done && impl_->done->view == view &&
            impl_->done->residencyRevision == residency->revision && !restart) return false;
        if (!latest && !impl_->done && impl_->last &&
            !impl_->last->dirty(view, residency->revision) && !restart) return false;
        auto job = std::make_shared<Impl::Job>();
        job->view = view;
        job->residency = std::move(residency);
        job->restart = restart;
        job->plan->view = view;
        job->plan->residencyRevision = job->residency->revision;
        job->plan->sequence = ++impl_->sequence;
        job->plan->time = time;
        if (impl_->done) { ++impl_->counts.discarded; impl_->done.reset(); }
        if (impl_->job) impl_->pending = std::move(job);
        else { impl_->job = std::move(job); ++impl_->counts.submitted; }
    }
    impl_->pool.notify();
    return true;
}

std::shared_ptr<const TerrainPlan> TerrainPlanner::collect() {
    std::lock_guard lock(impl_->mutex);
    auto result = std::exchange(impl_->done, {});
    if (result) impl_->last = result;
    return result;
}
TerrainPlanner::Stats TerrainPlanner::stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->counts;
}

bool TerrainPlanner::busy() const {
    std::lock_guard lock(impl_->mutex);
    return bool(impl_->job) || bool(impl_->done);
}

} // namespace world::terrain
