#include "game/generation/terrain_foundation.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/generation/world_import.hpp"
#include "game/generation/world_layout.hpp"
#include "game/generation/hybrid_terrain.hpp"
#include "engine/core/lane_hash.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <limits>
#include <queue>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <nlohmann/json.hpp>

namespace generation {
namespace {
using core::Fixed;
constexpr int dx[8]{-1,0,1,-1,1,-1,0,1}, dy[8]{-1,-1,-1,0,0,1,1,1};
std::uint64_t integerRoot(std::uint64_t n) {
    std::uint64_t result=0,bit=std::uint64_t(1)<<62;
    while (bit>n) bit>>=2;
    while (bit) {
        if (n>=result+bit) { n-=result+bit; result=(result>>1)+bit; }
        else result>>=1;
        bit>>=2;
    }
    return result;
}
template<class F> void rows(int count, F apply) {
    std::vector<std::jthread> workers;
    const int n=std::min(TerrainFoundation::kWorkers,count);
    for (int w=0;w<n;++w) workers.emplace_back([&,w] {
        for (int y=count*w/n;y<count*(w+1)/n;++y) apply(y);
    });
} // joining is the barrier between passes

// The lowest and highest height in each square of the lattice, so a pass can
// tell - without looking at a sample - that nothing in a square can move.
//
// Both slope passes below only ever act where two samples within their reach
// differ by more than the angle of repose, and most of any world is open sea
// floor and plain where nothing does: seven samples in ten of a region are
// under water. A square whose neighbourhood spans less than that is copied as
// it is, which is exactly what the pass would have written there.
struct Quiet {
    static constexpr int kTile = 32;
    int columns = 0, rows = 0, tilesX = 0, tilesY = 0;
    std::vector<std::int32_t> low, high;
    std::vector<std::uint8_t> still;   // per tile, for the pass at hand
    Quiet(int c, int r) : columns(c), rows(r), tilesX((c + kTile - 1) / kTile), tilesY((r + kTile - 1) / kTile),
        low(std::size_t(tilesX) * tilesY), high(low.size()), still(low.size()) {}
    void measure(const std::vector<std::int32_t>& field) {
        ::generation::rows(tilesY, [&](int ty) { for (int tx = 0; tx < tilesX; ++tx) {
            std::int32_t lo = std::numeric_limits<std::int32_t>::max(), hi = std::numeric_limits<std::int32_t>::min();
            for (int y = ty * kTile; y < std::min(rows, (ty + 1) * kTile); ++y)
                for (int x = tx * kTile; x < std::min(columns, (tx + 1) * kTile); ++x) {
                    const auto v = field[std::size_t(y) * columns + x];
                    lo = std::min(lo, v); hi = std::max(hi, v);
                }
            low[std::size_t(ty) * tilesX + tx] = lo; high[std::size_t(ty) * tilesX + tx] = hi;
        }});
    }
    // The lowest of the square and the eight around it: every sample within
    // a reach shorter than a square is in there (lower still, if anything,
    // which only makes the test stricter).
    std::int32_t lowAround(int tx, int ty) const {
        std::int32_t lo = std::numeric_limits<std::int32_t>::max();
        for (int j = std::max(0, ty - 1); j <= std::min(tilesY - 1, ty + 1); ++j)
            for (int i = std::max(0, tx - 1); i <= std::min(tilesX - 1, tx + 1); ++i)
                lo = std::min(lo, low[std::size_t(j) * tilesX + i]);
        return lo;
    }
    std::int32_t highAround(int tx, int ty) const {
        std::int32_t hi = std::numeric_limits<std::int32_t>::min();
        for (int j = std::max(0, ty - 1); j <= std::min(tilesY - 1, ty + 1); ++j)
            for (int i = std::max(0, tx - 1); i <= std::min(tilesX - 1, tx + 1); ++i)
                hi = std::max(hi, high[std::size_t(j) * tilesX + i]);
        return hi;
    }
    bool at(int x, int y) const { return still[std::size_t(y / kTile) * tilesX + x / kTile] != 0; }
};
template<class F> std::int64_t interpolateProfile(int count,std::int64_t position,F value) {
    if (count==1) return value(0);
    constexpr std::int64_t span=kMetresPerCell,span2=span*span,span3=span2*span;
    position=std::clamp<std::int64_t>(position,0,(count-1)*span);
    const int i=std::min(int(position/span),count-2);
    const auto t=position-i*span,t2=t*t,t3=t2*t;
    const auto a=value(i),b=value(i+1);
    const auto left=i>0?value(i-1):2*a-b;
    const auto right=i+2<count?value(i+2):2*b-a;
    const auto tangent=[](std::int64_t before,std::int64_t after) -> std::int64_t {
        if ((before>0 && after>0) || (before<0 && after<0))
            return 2*before*after/(before+after);
        return 0; // genuine summit, valley or plateau: no cubic overshoot
    };
    const auto m0=tangent(a-left,b-a),m1=tangent(b-a,right-b);
    // Monotone Hermite reconstruction shares tangents across macro joins. It
    // preserves planar ramps, unlike smoothstep, and never invents higher peaks
    // or lower sinks. Integer arithmetic keeps the bake independent of workers.
    return (a*(2*t3-3*t2*span+span3)+b*(3*t2*span-2*t3)+
        m0*(t3-2*t2*span+t*span2)+m1*(t3-t2*span))/span3;
}
}

namespace {
// The reconstruction, with the grid's spacing known at COMPILE time.
//
// It is a shift either way, so this ought not to matter - and it matters by a
// factor of three. With the spacing a constant the whole function specialises
// and inlines into its callers; with it a member read at run time the compiler
// keeps it opaque, and a height query went from seven microseconds to twenty.
// The spacing was made a runtime value so that a world too large to hold at
// sixty-four metres could be held at all, which is right; paying for that on
// every world is not.
//
// So the common case is instantiated with its own constant. Every world up to
// four hundred and forty kilometres is sixty-four metres to the sample, which
// is all of them but the two largest presets, and those take the general path.
template <int kShift>
core::Fixed sampleShifted(const TerrainFoundation& self, Fixed x, Fixed y, TerrainStage stage) {
    const int shift = kShift >= 0 ? kShift : self.stepShift;
    const auto columns = self.columns;
    const auto rows = self.rows;
    const auto& heightDm = self.heightDm;
    if (columns<2 || rows<2) return Fixed::fromInt(-60);
    const auto& h=heightDm[std::min(std::size_t(stage),heightDm.size()-1)];
    const auto u=core::clamp(Fixed::fromRaw(x.raw>>shift),core::kZero,
                             Fixed::fromInt(columns-1));
    const auto v=core::clamp(Fixed::fromRaw(y.raw>>shift),core::kZero,
                             Fixed::fromInt(rows-1));
    const int ix=int(u.toInt()),iy=int(v.toInt());
    const auto tx=u-Fixed::fromInt(ix),ty=v-Fixed::fromInt(iy);
    // Monotone Hermite, not bilinear.
    //
    // This is read at every height query in the world, and what it reads is a
    // grid of sixty-four metres. Bilinear between those gives flat facets with
    // a crease along every grid line - which is what makes the ground in this
    // world look like folded paper at any distance where the grid is visible,
    // and no amount of detail laid on top hides a crease in what carries it.
    //
    // The same reconstruction the H64 grid was BUILT with, which until now was
    // used only on the way in: monotone so it cannot invent a peak between two
    // samples or a sink between two rising ones, and therefore cannot put a
    // bump in the middle of a plain.
    const auto at=[&](int cx,int cy) {
        return Fixed::ratio(h[std::size_t(std::clamp(cy,0,rows-1))*columns+
                              std::size_t(std::clamp(cx,0,columns-1))],10);
    };
    // The harmonic mean of two slopes, in one division instead of two.
    //
    // Written as `before*after*2/(before+after)` this is a fixed-point multiply
    // - a hundred-and-twenty-eight bit product shifted back down - followed by a
    // fixed-point divide, which is the product shifted back UP and divided by a
    // hundred-and-twenty-eight bit value. Two shifts that cancel, and a division
    // whose divisor did not need to be that wide.
    //
    // It matters because this is the reconstruction every height in the world
    // goes through, and it runs ten times per sample: four rows of two, then two
    // for the column. On this architecture a wide division is a library call, so
    // ten of them is most of what a height query costs.
    //
    // The same integers, one divide, and the divisor is sixty-four bits.
    const auto tangent=[](Fixed before,Fixed after) -> Fixed {
        const bool together=(before.raw>0 && after.raw>0) || (before.raw<0 && after.raw<0);
        if (!together) return core::kZero;   // a summit, valley or plateau: no overshoot
        const auto sum=before.raw+after.raw;
        if (sum==0) return core::kZero;
        const auto product=Fixed::Wide(before.raw)*Fixed::Wide(after.raw)*2;
        return Fixed::fromRaw(Fixed::Raw(product/Fixed::Wide(sum)));
    };
    const auto row=[&](int cy,Fixed t) {
        const auto a=at(ix,cy),b=at(ix+1,cy);
        const auto left=at(ix-1,cy),right=at(ix+2,cy);
        const auto m0=tangent(a-left,b-a),m1=tangent(b-a,right-b);
        const auto t2=t*t,t3=t2*t;
        return a*(t3*2-t2*3+core::kOne)+b*(t2*3-t3*2)+m0*(t3-t2*2+t)+m1*(t3-t2);
    };
    const auto r0=row(iy-1,tx),r1=row(iy,tx),r2=row(iy+1,tx),r3=row(iy+2,tx);
    const auto n0=tangent(r1-r0,r2-r1),n1=tangent(r2-r1,r3-r2);
    const auto s2=ty*ty,s3=s2*ty;
    return r1*(s3*2-s2*3+core::kOne)+r2*(s2*3-s3*2)+n0*(s3-s2*2+ty)+n1*(s3-s2);
}
}

core::Fixed TerrainFoundation::sample(Fixed x,Fixed y,TerrainStage stage) const {
    return stepShift==6 ? sampleShifted<6>(*this,x,y,stage)
                        : sampleShifted<-1>(*this,x,y,stage);
}

std::pair<std::int32_t,std::int32_t> TerrainFoundation::heightRange() const {
    std::pair<std::int32_t,std::int32_t> r{0,0};
    bool any=false;
    for (const auto& plane:heightDm) {
        if (plane.empty()) continue;
        const auto [low,high]=plane.range();
        r=any?std::pair{std::min(r.first,low),std::max(r.second,high)}:std::pair{low,high};
        any=true;
    }
    return r;
}

bool TerrainFoundation::needsDetail(int minX,int minY,int maxX,int maxY) const {
    for (int y=std::max(0,minY/512);y<=std::min(pageRows-1,maxY/512);++y)
        for (int x=std::max(0,minX/512);x<=std::min(pageColumns-1,maxX/512);++x)
            if (detailPages[std::size_t(y)*pageColumns+x]) return true;
    return false;
}

std::shared_ptr<const TerrainFoundation> buildTerrainFoundation(
    WorldMapData& world,const WorldMapParams& params,int seaLevel,int highest) {
    auto f=std::make_shared<TerrainFoundation>();
    // The step comes from the budget, not from a constant.
    //
    // It was sixty-four metres for every world, so the cost of the foundation
    // was the square of the world's side and nothing capped it but a throw: a
    // five-hundred-kilometre world wanted sixty-four million cells and a
    // two-thousand-kilometre one a billion, and the answer to both was that the
    // world could not be made. A coarser authority over a big world is worth
    // having; refusing to build one is not.
    f->step=params.foundationStep>0 ? std::max(kFinestFoundationStep,int(params.foundationStep)) :
        foundationStepFor(std::int64_t(world.width)*kMetresPerCell,std::int64_t(world.height)*kMetresPerCell);
    const int step=f->step;
    f->stepShift=0;
    while ((1 << f->stepShift) < step) ++f->stepShift;
    f->columns=int((std::int64_t(world.width)*kMetresPerCell+step-1)/step+1);
    f->rows=int((std::int64_t(world.height)*kMetresPerCell+step-1)/step+1);
    const auto count=std::size_t(f->columns)*f->rows;
    // Worked out densely - a run is bounded by the budget - and kept
    // sparsely at the end, where only land and its shore are held.
    std::array<std::vector<std::int32_t>,5> planes;
    std::vector<std::int32_t> receiverOf;
    std::vector<std::uint32_t> accumulationOf;
    auto start=std::chrono::steady_clock::now();
    const auto finish=[&](int stage) {
        const auto now=std::chrono::steady_clock::now();
        f->milliseconds[stage]=std::chrono::duration<double,std::milli>(now-start).count(); start=now;
    };
    // Calibrate against the un-eroded peak, not the lower thermal peak: clipping
    // to the latter flattened the very summits that erosion needs to dissect.
    // A region made by hand is not stretched to the world's tallest peak: its
    // highest point is whatever it was made to be, on a fixed scale - a
    // pinned coast with no mountains painted comes out as low country, and
    // what the painted ranges raise stands as high as they raise it.
    constexpr int kAuthoredReliefSpan=2400;
    const int peak=params.authored ? seaLevel+kAuthoredReliefSpan :
        std::max({highest,*std::max_element(world.macroHeightField.begin(),world.macroHeightField.end()),
        *std::max_element(world.primaryHeightField.begin(),world.primaryHeightField.end())});
    constexpr std::int64_t referencePeakDm=255*kMetresPerElevationStep*10;
    constexpr std::int64_t foothillDm=3000;
    for (int stage=0;stage<2;++stage) {
        auto& out=planes[stage]; out.resize(count);
        std::vector<std::int32_t> macro(world.cells.size());
        const auto& source=stage==0?world.primaryHeightField:world.macroHeightField;
        for (std::size_t i=0;i<macro.size();++i) {
            const auto h=std::clamp<std::int64_t>((std::int64_t(source[i])-seaLevel)*referencePeakDm/
                std::max(1,peak-seaLevel),-600,referencePeakDm);
            // H64 owns physical heights; the 8-bit macro elevation is only a
            // summary. Smoothly raise highland relief to 4290 m instead of
            // normalising every collision back to 2295 m. Coasts and low plains
            // stay unchanged, with no slope discontinuity at the foothills.
            const auto rise=std::max<std::int64_t>(0,h-foothillDm);
            macro[i]=int(h+rise*rise/(referencePeakDm-foothillDm));
        }
        // Two separable passes retain 1/256 dm until the final quantisation.
        // The intermediate is only H64 columns x macro rows, not another full
        // world snapshot. Runtime still samples the immutable H64 lattice.
        std::vector<std::int64_t> horizontal(std::size_t(f->columns)*world.height);
        rows(world.height,[&](int y) { for (int x=0;x<f->columns;++x)
            horizontal[std::size_t(y)*f->columns+x]=interpolateProfile(world.width,std::int64_t(x)*step,
                [&](int col){return std::int64_t(macro[std::size_t(y)*world.width+col])*256;}); });
        rows(f->rows,[&](int y) { for (int x=0;x<f->columns;++x)
            out[std::size_t(y)*f->columns+x]=int(interpolateProfile(world.height,std::int64_t(y)*step,
                [&](int row){return horizontal[std::size_t(row)*f->columns+x];})/256); });
        finish(stage);
    }
    // An imported region (world_import.hpp): its ground is the skeleton, read
    // at the lattice itself rather than through the macro cells, which hold
    // it only at half its resolution.
    if (params.imported) {
        const ImportedGround& g=*params.imported;
        rows(f->rows,[&](int y) { for (int x=0;x<f->columns;++x) {
            const auto i=std::size_t(y)*f->columns+x;
            planes[0][i]=planes[1][i]=g.heightAt(std::int64_t(x)*step,std::int64_t(y)*step);
        }});
    }
    // Symmetric eight-neighbour flux. Each undirected transfer is evaluated
    // identically at both ends; Jacobi updates conserve mass and cannot race.
    auto& thermal=planes[2]; thermal=planes[1];
    const auto before=thermal;
    std::vector<std::int32_t> next(count);
    // How long each place has weathered, when the world is built region by
    // region (world_layout.hpp): the passes below run as many times as the
    // most weathered place asks, and a place takes part in as many of them
    // as its own region does - fractionally across a border band - or as the
    // weathering layer says where that was painted.
    std::vector<float> weathering;
    int passes=std::clamp(params.erosionPasses,0,24);
    if (params.layout) {
        const std::vector<float> perCell=dialCells(*params.layout,LayerId::Weathering);
        weathering.resize(count);
        for (std::size_t i=0;i<count;++i) {
            const int mx=std::min(world.width-1,int(std::int64_t(i%f->columns)*step/kMetresPerCell));
            const int my=std::min(world.height-1,int(std::int64_t(i/f->columns)*step/kMetresPerCell));
            weathering[i]=perCell[std::size_t(my)*world.width+mx];
            passes=std::max(passes,std::min(24,int(std::ceil(weathering[i]))));
        }
    }
    // And an imported erosion mask says how much of that each place takes:
    // at rest (0.5) what the dials give, none at 0, twice as much at 1.
    if (params.imported && !params.imported->erosion.empty()) {
        if (weathering.empty()) weathering.assign(count,float(passes));
        passes=0;
        for (std::size_t i=0;i<count;++i) {
            const auto share=params.imported->maskAt(params.imported->erosion,
                std::int64_t(i%f->columns)*step,std::int64_t(i/f->columns)*step,128);
            weathering[i]=std::min(24.0f,weathering[i]*float(share)/127.5f);
            passes=std::max(passes,int(std::ceil(weathering[i])));
        }
    }
    const auto takes=[&](std::size_t i,int pass) {
        return weathering.empty() ? 1.0f : std::clamp(weathering[i]-float(pass),0.0f,1.0f);
    };
    Quiet quiet(f->columns,f->rows);
    // The smallest difference between neighbours that moves anything: the
    // orthogonal threshold below (the diagonal one is larger).
    const int flatEnough=int(step*6.745+0.5);
    for (int pass=0;pass<passes;++pass) {
        quiet.measure(thermal);
        for (int ty=0;ty<quiet.tilesY;++ty) for (int tx=0;tx<quiet.tilesX;++tx)
            quiet.still[std::size_t(ty)*quiet.tilesX+tx]=quiet.highAround(tx,ty)-quiet.lowAround(tx,ty)<=flatEnough;
        rows(f->rows,[&](int y) { for (int x=0;x<f->columns;++x) {
            const auto i=std::size_t(y)*f->columns+x; int change=0;
            if (quiet.at(x,y)) { next[i]=thermal[i]; continue; }
            for (int d=0;d<8;++d) {
                const int nx=x+dx[d],ny=y+dy[d];
                if (nx<0 || ny<0 || nx>=f->columns || ny>=f->rows) continue;
                const auto j=std::size_t(ny)*f->columns+nx;
                const int difference=thermal[j]-thermal[i];
                // The talus threshold is an ANGLE, so it has to be worked out
                // from the spacing rather than written down as a height. Four
                // hundred decimetres between neighbours is thirty-two degrees
                // at a sixty-four metre step and eighty-four at a four metre
                // one - which is why this stage did nothing whatever in a world
                // sampled finer than it was written for, and why its delta mask
                // comes out blank.
                //
                // tan(34 degrees) is 0.6745; heights are decimetres and the
                // spacing is metres, so the threshold is 6.745 * spacing, and
                // the diagonal is longer by root two.
                const int span=dx[d] && dy[d]?int(step*6.745*1.41421356+0.5)
                                             :int(step*6.745+0.5);
                const int flux=int(float(std::max(0,std::abs(difference)-span)/16)*
                                   std::min(takes(i,pass),takes(j,pass)));
                change+=difference>0?flux:-flux;
            }
            next[i]=thermal[i]+change;
        }});
        thermal.swap(next);
    }
    // The talus cut, as a function, because the volcanoes below want it too.
    // They are added AFTER the thermal stage, so until now nothing eroded them
    // at all - which is why an island comes out as a smooth even cone with no
    // gullies, no scree and no shape.
    const auto layBack=[&](std::vector<std::int32_t>& field) {
        constexpr int kRings=6;
        static_assert(kRings<Quiet::kTile, "the quiet test reads one square around");
        const int span=int(step*6.745+0.5);   // tan(34 deg), decimetres
        // How far each ring reaches, worked out once rather than a square
        // root per neighbour per sample per pass.
        std::array<int,(2*kRings+1)*(2*kRings+1)> reaches{};
        for (int j=-kRings;j<=kRings;++j) for (int k=-kRings;k<=kRings;++k)
            reaches[std::size_t((j+kRings)*(2*kRings+1)+(k+kRings))]=int(std::sqrt(double(j*j+k*k))*span+0.5);
        for (int pass=0;pass<passes;++pass) {
            bool moved=false;
            // A sample is cut back only to a neighbour lower than it by more
            // than the reach between them, and no reach is shorter than one
            // span: a square no higher than the lowest ground around it plus
            // a span stays as it is.
            quiet.measure(field);
            for (int ty=0;ty<quiet.tilesY;++ty) for (int tx=0;tx<quiet.tilesX;++tx)
                quiet.still[std::size_t(ty)*quiet.tilesX+tx]=
                    quiet.high[std::size_t(ty)*quiet.tilesX+tx]<=quiet.lowAround(tx,ty)+span;
            rows(f->rows,[&](int y) { for (int x=0;x<f->columns;++x) {
                const auto i=std::size_t(y)*f->columns+x;
                if (quiet.at(x,y)) { next[i]=field[i]; continue; }
                int limit=field[i];
                for (int j=-kRings;j<=kRings;++j) for (int k=-kRings;k<=kRings;++k) {
                    if (!j && !k) continue;
                    const int nx=x+k,ny=y+j;
                    if (nx<0 || ny<0 || nx>=f->columns || ny>=f->rows) continue;
                    const int reach=reaches[std::size_t((j+kRings)*(2*kRings+1)+(k+kRings))];
                    limit=std::min(limit,field[std::size_t(ny)*f->columns+nx]+reach);
                }
                const float share=takes(i,pass);
                next[i]=share>=1.0f ? limit : field[i]+int(float(limit-field[i])*share);
            }});
            for (std::size_t i=0;i<count;++i) moved=moved || next[i]!=field[i];
            field.swap(next);
            if (!moved) break;
        }
    };

    // And the talus cut, which is a different operator and does what the
    // diffusion above cannot.
    //
    // Measured: diffusion moved seven hundred kilometres of rock over
    // twenty-four passes and left the fraction of ground steeper than repose
    // exactly where it found it, 4.63 per cent before and 4.77 after. It has
    // to: on a UNIFORM over-steep slope every cell receives from above as much
    // as it gives below, so the net change is zero. What it can do is round a
    // convexity, and what this world has instead is a step - the crust edge,
    // four hundred and fifty units of it blurred over two cells, which is a
    // three-kilometre drop across five hundred metres.
    //
    // A step is exactly what the lower envelope of cones lays back. Six rings
    // at sixty-four metres reaches four hundred metres and lays back a break of
    // two hundred and sixty: measured, that takes the over-repose ground from
    // 4.63 per cent to 2.47. Three rings give 3.24 and ten give 2.06 for ten
    // times the time.
    //
    // It only removes, and that is measured rather than chosen. Depositing what
    // comes off the face at its foot was tried twice - once unbounded and once
    // with every receiver held under the cone that produced the cut - and both
    // made the ground STEEPER: the over-repose fraction went from 5.34 per cent
    // to 5.68 and the steepest pair in the world from eighty-one degrees to
    // ninety. Many faces shed into one hollow, each within its own bound, and
    // the sum is within nobody's. Putting that right is a sediment solver with
    // a global constraint, not a neighbourhood operator, and pretending a local
    // one conserves mass only moves the cliff to the foot of the slope.
    layBack(thermal);
    for (std::size_t i=0;i<count;++i) {
        const auto moved=std::abs(thermal[i]-before[i]);
        if (moved>10) { ++f->thermalCells; f->thermalMetres+=moved/10.0; }
    }
    finish(2);
    auto& volcanoes=planes[3]; volcanoes=thermal;
    // The legacy object now supplies only complete analytic volcanic edifices.
    // Its macro contribution is NOT added again from the already changed cells.
    if (world.hybridTerrain) rows(f->rows,[&](int y) { for (int x=0;x<f->columns;++x) {
        const auto s=world.hybridTerrain->sample(Fixed::fromInt(std::int64_t(x)*step),
                                                Fixed::fromInt(std::int64_t(y)*step));
        volcanoes[std::size_t(y)*f->columns+x]+=int((s.delta*10).roundToInt());
    }});
    // And lay them back like anything else. An island that nothing erodes is a
    // smooth even cone, which is what every one of them was.
    layBack(volcanoes);
    finish(3);
    auto& slopes=planes[4]; slopes=volcanoes;

    // Drain the hollows that nothing meant to make.
    //
    // A talus cut only lowers and fine relief only adds, so between them the
    // ground is left with thousands of small closed basins - and a closed basin
    // is a lake, because the water pass has nowhere else to put the water that
    // arrives in it. That is the pitted country standing in puddles: not lakes
    // anybody placed, just the arithmetic of the layers above having nowhere
    // to run.
    //
    // Priority flood, which is the standard answer: walk inwards from the edge
    // always taking the lowest rim first, and every cell comes out at least a
    // hair above the lowest way OUT of where it sits. A valley that was a
    // string of pools becomes a valley that drains.
    //
    // Shallow ones only. A basin deeper than the threshold is a lake somebody
    // does want - a crater, a cirque, a rift floor - and filling those would
    // flatten the country as surely as the pits pit it.
    //
    // Under the same switch as the erosion above it, and not because it is
    // erosion - it is drainage conditioning - but because it is a SHAPE PASS,
    // and a caller that asks for no passes is asking to see the layers without
    // anything having been done to them. A fill that ran anyway would be a
    // hidden pass, which is the one thing the staged foundation exists not to
    // have.
    if (passes>0) {
        // Sixty metres, not twenty-five.
        //
        // This is the bar for "a hollow nothing meant to make", and it was set
        // low enough that it caught only the small ones. What it missed is the
        // trench along the foot of a scarp: relief that size leaves hollows
        // thirty and forty metres deep in front of it, they are closed, and so
        // every plateau came with a moat around it and the moat came with a
        // lake in it. Sixty metres still leaves a crater, a cirque or a rift
        // floor alone, and whether what remains actually holds water is decided
        // later and on its own evidence, not here.
        constexpr int kFillLimitDm=600;
        std::vector<std::int32_t> filled(count,std::numeric_limits<std::int32_t>::max());
        struct Rim { std::int32_t level; std::uint32_t at; };
        const auto lower=[](const Rim& a,const Rim& b) { return a.level>b.level; };
        std::priority_queue<Rim,std::vector<Rim>,decltype(lower)> rim(lower);
        for (int y=0;y<f->rows;++y) for (int x=0;x<f->columns;++x) {
            if (x && y && x+1<f->columns && y+1<f->rows) continue;
            const auto i=std::size_t(y)*f->columns+x;
            filled[i]=slopes[i];
            rim.push({slopes[i],std::uint32_t(i)});
        }
        while (!rim.empty()) {
            const auto here=rim.top(); rim.pop();
            if (here.level!=filled[here.at]) continue;
            const int x=int(here.at%f->columns),y=int(here.at/f->columns);
            for (int d=0;d<8;++d) {
                const int nx=x+dx[d],ny=y+dy[d];
                if (nx<0 || ny<0 || nx>=f->columns || ny>=f->rows) continue;
                const auto j=std::size_t(ny)*f->columns+nx;
                if (filled[j]!=std::numeric_limits<std::int32_t>::max()) continue;
                filled[j]=std::max(slopes[j],here.level+1);
                rim.push({filled[j],std::uint32_t(j)});
            }
        }
        // Except where a lake was painted (the water layer, WaterPaint::Lake):
        // that hollow is exactly one somebody meant to make.
        const auto paintedLake=[&](std::size_t i) {
            if (world.waterPaintField.size()!=world.cells.size()) return false;
            const int mx=std::min(world.width-1,int(std::int64_t(i%f->columns)*step/kMetresPerCell));
            const int my=std::min(world.height-1,int(std::int64_t(i/f->columns)*step/kMetresPerCell));
            return world.waterPaintField[std::size_t(my)*world.width+mx]==std::uint8_t(WaterPaint::Lake);
        };
        for (std::size_t i=0;i<count;++i)
            if (filled[i]-slopes[i]<=kFillLimitDm && !paintedLake(i)) slopes[i]=filled[i];
    }
    receiverOf.resize(count); accumulationOf.resize(count);
    std::vector<std::uint8_t> incoming(count);
    std::vector<std::uint32_t> order; order.reserve(count);
    const auto route=[&] {
        rows(f->rows,[&](int y) { for (int x=0;x<f->columns;++x) {
            const auto i=std::size_t(y)*f->columns+x; int sink=-1,best=0,length=1;
            if (slopes[i]>0) for (int d=0;d<8;++d) {
                const int nx=x+dx[d],ny=y+dy[d];
                if (nx<0 || ny<0 || nx>=f->columns || ny>=f->rows) continue;
                const auto j=std::size_t(ny)*f->columns+nx;
                const int drop=slopes[i]-slopes[j],
                          distance=dx[d] && dy[d]?int(step*1.41421356+0.5):step;
                if (drop>0 && drop*length>best*distance) { best=drop; length=distance; sink=int(j); }
            }
            receiverOf[i]=sink;
        }});
        std::fill(incoming.begin(),incoming.end(),0);
        std::fill(accumulationOf.begin(),accumulationOf.end(),1);
        for (auto sink:receiverOf) if (sink>=0) ++incoming[std::size_t(sink)];
        order.clear();
        for (std::size_t i=0;i<count;++i) if (!incoming[i]) order.push_back(std::uint32_t(i));
        // Topological accumulation is O(N), not a global height sort. It is
        // deliberately ordered, while independent stencil passes use 6 workers.
        for (std::size_t n=0;n<order.size();++n) {
            const auto i=order[n]; const int sink=receiverOf[i];
            if (sink<0) continue;
            accumulationOf[std::size_t(sink)]+=accumulationOf[i];
            if (--incoming[std::size_t(sink)]==0) order.push_back(std::uint32_t(sink));
        }
    };
    // Re-route after each incision, rather than stamping independent fractals.
    // Dissolved/suspended material reaches the actual sink, making aprons in
    // closed depressions. Ocean sinks export it beyond this height field.
    const int erosionPasses=std::min(passes,8);
    for (int pass=0;pass<erosionPasses;++pass) {
        route(); next=slopes;
        std::vector<std::int64_t> sediment(count,0);
        for (auto i:order) {
            const int sink=receiverOf[i];
            if (slopes[i]<=0) continue;
            if (sink<0) { next[i]+=int(std::min<std::int64_t>(sediment[i],200)); continue; }
            const int drop=std::max(0,slopes[i]-slopes[std::size_t(sink)]);
            const int mx=std::min(world.width-1,int(std::int64_t(i%f->columns)*step/kMetresPerCell));
            const int my=std::min(world.height-1,int(std::int64_t(i/f->columns)*step/kMetresPerCell));
            const int resistance=world.erosionResistanceField[std::size_t(my)*world.width+mx];
            const auto root=integerRoot(std::uint64_t(accumulationOf[i])<<16);
            const int removed=int(float(std::min({drop/4,250,int(root*drop*
                (280-std::clamp(resistance,0,255))/(256*18000))}))*takes(i,pass));
            next[i]-=removed;
            sediment[std::size_t(sink)]+=sediment[i]+removed;
        }
        slopes.swap(next);
    }
    // Land painted by hand stays land at the height it was promised: the
    // relief and the torn coast may shape it, not drown it (the Coast &
    // relief stage's floor). Wherever the painted mask says land, no stage
    // of the ground ends lower.
    // Neither holds for an imported region: its coast is the skeleton's.
    if (params.authored && !params.imported && params.authoring.minLandMetres > 0) {
        const int floor = int(std::lround(params.authoring.minLandMetres * 10.0));
        for (int y = 0; y < f->rows; ++y)
            for (int x = 0; x < f->columns; ++x) {
                const int mx = std::min(world.width - 1, int(std::int64_t(x) * step / kMetresPerCell));
                const int my = std::min(world.height - 1, int(std::int64_t(y) * step / kMetresPerCell));
                if (world.initialLandMask[std::size_t(my) * world.width + mx] == 0) continue;
                const auto i = std::size_t(y) * f->columns + x;
                for (int stage = 2; stage < 5; ++stage) planes[stage][i] = std::max(planes[stage][i], floor);
            }
    }
    // And the sea painted by hand stays sea: the lattice's own relief under it
    // came up through the water in flat plates of land at the floor above,
    // kilometres from any stroke, where the macro map and every page say sea.
    // Held under the water at every stage, and down to the open sea's sixty
    // metres within a kilometre of the coast: that is what the ground reads
    // where no page is (a page is kept only within half a kilometre of land),
    // and a shelf wider than that is cut off where the pages stop, in steps.
    if (params.authored && !params.imported) {
        for (int y = 0; y < f->rows; ++y)
            for (int x = 0; x < f->columns; ++x) {
                const int mx = std::min(world.width - 1, int(std::int64_t(x) * step / kMetresPerCell));
                const int my = std::min(world.height - 1, int(std::int64_t(y) * step / kMetresPerCell));
                const auto cell = std::size_t(my) * world.width + mx;
                if (world.initialLandMask[cell] != 0) continue;
                const int away = world.distanceToCoast.size() > cell ? world.distanceToCoast[cell] : 40;
                const int ceiling = -std::min(600, 20 + away * 300);
                const auto i = std::size_t(y) * f->columns + x;
                for (auto& plane : planes) if (i < plane.size()) plane[i] = std::min(plane[i], ceiling);
            }
    }
    route(); // published graph describes the eroded surface, not its predecessor
    f->pageColumns=(world.width*kMetresPerCell+511)/512;
    f->pageRows=(world.height*kMetresPerCell+511)/512;
    f->detailPages.assign(std::size_t(f->pageColumns)*f->pageRows,0);
    std::vector<std::uint32_t> basin(count);
    for (auto it=order.rbegin();it!=order.rend();++it)
        basin[*it]=receiverOf[*it]<0?*it:basin[std::size_t(receiverOf[*it])];
    for (int y=0;y<f->rows;++y) for (int x=0;x<f->columns;++x) {
        const auto i=std::size_t(y)*f->columns+x;
        if (slopes[i]<=0) continue;
        // A steep plane contains no extra shape to resolve. Use midpoint
        // residuals along all four stencil axes instead of absolute slope;
        // keep incision evidence even when the resulting bed is locally flat.
        bool bend=false;
        for (int d=0;d<4 && !bend;++d) {
            const int ax=x+dx[d],ay=y+dy[d],bx=x-dx[d],by=y-dy[d];
            if (ax<0 || ay<0 || bx<0 || by<0 || ax>=f->columns || bx>=f->columns || ay>=f->rows || by>=f->rows) continue;
            bend=std::abs(slopes[std::size_t(ay)*f->columns+ax]+
                slopes[std::size_t(by)*f->columns+bx]-2*slopes[i])>30; // >1.5 m interpolation error
        }
        if (bend || volcanoes[i]-slopes[i]>15) {
            const int px=std::min(f->pageColumns-1,int(std::int64_t(x)*step/512)),
                      py=std::min(f->pageRows-1,int(std::int64_t(y)*step/512));
            f->detailPages[std::size_t(py)*f->pageColumns+px]=1;
        }
        for (int d:{4,6}) {
            const int nx=x+dx[d],ny=y+dy[d];
            if (nx>=f->columns || ny>=f->rows) continue;
            const auto j=std::size_t(ny)*f->columns+nx;
            if (slopes[j]>0 && basin[i]!=basin[j]) f->divides.push_back({std::uint32_t(i),std::uint32_t(j)});
        }
    }
    for (std::size_t stage=0;stage<planes.size();++stage)
        f->heightDm[stage]=TerrainFoundation::Plane::sparse(planes[stage],kFoundationSeaFloorDm);
    for (std::size_t stage=0;stage+1<planes.size();++stage) f->heightDm[stage].shareEqualChunks(f->heightDm[4]);
    f->receiver=CellField<std::int32_t>::sparse(receiverOf,-1);
    f->accumulation=CellField<std::uint32_t>::sparse(accumulationOf,1);
    // Climate, standing water and the persistent river graph must see the
    // post-erosion base. Macro cells are a summary, never the H64 height source.
    f->applyTo(world);
    if (params.imported) f->applyLowestTo(world);
    finish(4);
    return f;
}

void TerrainFoundation::applyTo(WorldMapData& world) const {
    for (int y=0;y<world.height;++y) for (int x=0;x<world.width;++x) {
        auto& cell=world.at({x,y});
        const auto h=sample(Fixed::fromInt(x*kMetresPerCell),Fixed::fromInt(y*kMetresPerCell),TerrainStage::Slopes);
        cell.sea=h<=core::kZero;
        cell.elevation=cell.sea?0:std::uint8_t(std::clamp<std::int64_t>((h/Fixed::fromInt(kMetresPerElevationStep)).roundToInt(),1,255));
    }
}

void TerrainFoundation::applyLowestTo(WorldMapData& world) const {
    // An imported skeleton carries valleys narrower than a macro cell, and a
    // cell read at one corner cuts such a valley into a string of closed
    // hollows - every one a lake to the drainage (8885 of them on one
    // continent). The drainage wants the lowest ground in the cell, so a land
    // cell is the lowest land of the lattice inside it; which cells are sea
    // stays the corner's.
    const int per=std::max(1,kMetresPerCell/step);
    const auto& slopePlane=heightDm[4];
    for (int y=0;y<world.height;++y) for (int x=0;x<world.width;++x) {
        auto& cell=world.at({x,y});
        if (cell.sea) continue;
        std::int32_t lowest=std::numeric_limits<std::int32_t>::max();
        for (int j=0;j<per;++j) for (int i=0;i<per;++i) {
            const int px=std::min(columns-1,x*per+i),py=std::min(rows-1,y*per+j);
            const auto h=slopePlane[std::size_t(py)*columns+px];
            if (h>0) lowest=std::min(lowest,h);
        }
        if (lowest==std::numeric_limits<std::int32_t>::max()) continue;
        cell.elevation=std::uint8_t(std::clamp<std::int64_t>(
            (std::int64_t(lowest)+kMetresPerElevationStep*5)/(kMetresPerElevationStep*10),1,255));
    }
}

std::uint64_t TerrainFoundation::fingerprint() const {
    if (!sealed_) return computeFingerprint();
    if (canonical_.ready.load(std::memory_order_acquire)) return canonical_.value.load(std::memory_order_relaxed);
    // Two threads asking at once both work it out and store the same number.
    const auto value = computeFingerprint();
    canonical_.value.store(value, std::memory_order_relaxed);
    canonical_.ready.store(true, std::memory_order_release);
    return value;
}

std::uint64_t TerrainFoundation::contentKey() const {
    return sealed_ ? contentKey_ : computeContentKey();
}

void TerrainFoundation::seal() {
    sealed_ = false;
    canonical_.ready.store(false);
    contentKey_ = computeContentKey();
    sealed_ = true;
}

std::uint64_t TerrainFoundation::computeContentKey() const {
    // Four independent multiply-xor lanes over whole values, folded at the end:
    // the multiplies overlap instead of queueing behind one another byte by
    // byte, as FNV's must.
    constexpr std::uint64_t prime = 0x9e3779b97f4a7c15ull;
    std::uint64_t lane[4]{0x243f6a8885a308d3ull, 0x13198a2e03707344ull, 0xa4093822299f31d0ull, 0x082efa98ec4e6c89ull};
    const auto mix = [](std::uint64_t h, std::uint64_t v) { return (h ^ v) * prime; };
    const auto field = [&](const auto& values) {
        lane[0] = mix(lane[0], values.size());
        const std::size_t n = values.size(), whole = n & ~std::size_t(3);
        for (std::size_t i = 0; i < whole; i += 4) {
            lane[0] = mix(lane[0], std::uint64_t(std::uint32_t(values[i])));
            lane[1] = mix(lane[1], std::uint64_t(std::uint32_t(values[i + 1])));
            lane[2] = mix(lane[2], std::uint64_t(std::uint32_t(values[i + 2])));
            lane[3] = mix(lane[3], std::uint64_t(std::uint32_t(values[i + 3])));
        }
        for (std::size_t i = whole; i < n; ++i) lane[i & 3] = mix(lane[i & 3], std::uint64_t(std::uint32_t(values[i])));
    };
    lane[0] = mix(lane[0], std::uint64_t(std::uint32_t(columns)) << 32 | std::uint32_t(rows));
    lane[1] = mix(lane[1], std::uint64_t(std::uint32_t(pageColumns)) << 32 | std::uint32_t(pageRows));
    lane[2] = mix(lane[2], std::uint64_t(std::uint32_t(step)));
    for (const auto& heights : heightDm) field(heights);
    field(receiver); field(accumulation); field(detailPages);
    lane[3] = mix(lane[3], divides.size());
    for (const auto edge : divides) lane[3] = mix(lane[3], std::uint64_t(edge.a) << 32 | edge.b);
    std::uint64_t hash = 0;
    for (const auto h : lane) hash = core::splitmix64(hash ^ h);
    return hash;
}

std::uint64_t TerrainFoundation::computeFingerprint() const {
    std::uint64_t hash=14695981039346656037ull;
    const auto add=[&](std::uint64_t v) {
        for (int b=0;b<8;++b) { hash^=(v>>(8*b))&255; hash*=1099511628211ull; }
    };
    add(columns); add(rows); add(pageColumns); add(pageRows);
    const auto field=[&](const auto& values) { add(values.size()); for (auto v:values) add(v); };
    for (const auto& heights:heightDm) field(heights);
    field(receiver); field(accumulation); field(detailPages);
    add(divides.size()); for (auto edge:divides) { add(edge.a); add(edge.b); }
    return hash;
}

std::string saveTerrainFoundation(const TerrainFoundation& f) {
    nlohmann::json planes=nlohmann::json::array();
    for (const auto& plane:f.heightDm) planes.push_back(plane.dense());
    nlohmann::json j{{"version",1},{"columns",f.columns},{"rows",f.rows},
        {"height_dm",planes},{"receiver",f.receiver.dense()},{"accumulation",f.accumulation.dense()},
        {"detail_pages",f.detailPages},{"fingerprint",f.fingerprint()},{"divides",nlohmann::json::array()}};
    for (auto edge:f.divides) j["divides"].push_back({edge.a,edge.b});
    return j.dump();
}

std::shared_ptr<const TerrainFoundation> loadTerrainFoundation(const std::string& payload,int width,int height) {
    const auto j=nlohmann::json::parse(payload);
    auto f=std::make_shared<TerrainFoundation>();
    f->step=foundationStepFor(std::int64_t(width)*kMetresPerCell,
                              std::int64_t(height)*kMetresPerCell);
    f->stepShift=0;
    while ((1 << f->stepShift) < f->step) ++f->stepShift;
    const auto columns=(std::int64_t(width)*kMetresPerCell+f->step-1)/f->step+1;
    const auto rows=(std::int64_t(height)*kMetresPerCell+f->step-1)/f->step+1;
    if (width<=0 || height<=0 || columns>16384 || rows>16384 ||
        std::size_t(columns*rows)>kFoundationCellBudget ||
        j.at("version")!=1 || j.at("columns")!=columns || j.at("rows")!=rows)
        throw std::runtime_error("incompatible H64 foundation snapshot");
    f->columns=int(columns); f->rows=int(rows);
    f->pageColumns=int((std::int64_t(width)*kMetresPerCell+511)/512);
    f->pageRows=int((std::int64_t(height)*kMetresPerCell+511)/512);
    const auto count=std::size_t(columns*rows);
    const auto require=[](bool valid) { if (!valid) throw std::runtime_error("invalid H64 foundation snapshot"); };
    require(j.at("height_dm").size()==f->heightDm.size());
    std::array<std::vector<std::int32_t>,5> planes;
    for (std::size_t s=0;s<f->heightDm.size();++s) {
        const auto& values=j.at("height_dm")[s]; require(values.size()==count);
        planes[s].reserve(count);
        for (const auto& v:values) {
            require(v.is_number_integer()); const auto h=v.get<std::int64_t>();
            require(h>=-30000 && h<=60000); planes[s].push_back(int(h));
        }
    }
    require(j.at("receiver").size()==count && j.at("accumulation").size()==count);
    std::vector<std::int32_t> receiver; std::vector<std::uint32_t> accumulation;
    receiver.reserve(count); accumulation.reserve(count);
    for (std::size_t i=0;i<count;++i) {
        const auto r=j.at("receiver")[i].get<std::int64_t>();
        const auto a=j.at("accumulation")[i].get<std::int64_t>();
        require(r>=-1 && r<std::int64_t(count) && a>=1 && a<=std::int64_t(count));
        if (r>=0) require(std::abs(int(i%columns)-int(r%columns))<=1 &&
            std::abs(int(i/columns)-int(r/columns))<=1 && planes[4][i]>planes[4][std::size_t(r)]);
        receiver.push_back(int(r)); accumulation.push_back(std::uint32_t(a));
    }
    for (std::size_t s=0;s<planes.size();++s) f->heightDm[s]=TerrainFoundation::Plane::sparse(planes[s],kFoundationSeaFloorDm);
    for (std::size_t s=0;s+1<planes.size();++s) f->heightDm[s].shareEqualChunks(f->heightDm[4]);
    f->receiver=CellField<std::int32_t>::sparse(receiver,-1);
    f->accumulation=CellField<std::uint32_t>::sparse(accumulation,1);
    require(j.at("detail_pages").size()==std::size_t(f->pageColumns)*f->pageRows);
    for (const auto& value:j.at("detail_pages")) {
        const int v=value.get<int>(); require(v==0 || v==1); f->detailPages.push_back(std::uint8_t(v));
    }
    require(j.at("divides").size()<=count*2);
    for (const auto& e:j.at("divides")) {
        require(e.is_array() && e.size()==2);
        const auto a=e[0].get<std::uint64_t>(),b=e[1].get<std::uint64_t>();
        require(a<count && b<count && ((b==a+1 && a/columns==b/columns) || b==a+columns));
        f->divides.push_back({std::uint32_t(a),std::uint32_t(b)});
    }
    require(j.at("fingerprint").get<std::uint64_t>()==f->fingerprint());
    return f;
}
} // namespace generation
