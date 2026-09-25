#include "game/generation/terrain_foundation.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/generation/hybrid_terrain.hpp"
#include <algorithm>
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
    f->step=foundationStepFor(std::int64_t(world.width)*kMetresPerCell,
                              std::int64_t(world.height)*kMetresPerCell);
    const int step=f->step;
    f->stepShift=0;
    while ((1 << f->stepShift) < step) ++f->stepShift;
    f->columns=int((std::int64_t(world.width)*kMetresPerCell+step-1)/step+1);
    f->rows=int((std::int64_t(world.height)*kMetresPerCell+step-1)/step+1);
    const auto count=std::size_t(f->columns)*f->rows;
    auto start=std::chrono::steady_clock::now();
    const auto finish=[&](int stage) {
        const auto now=std::chrono::steady_clock::now();
        f->milliseconds[stage]=std::chrono::duration<double,std::milli>(now-start).count(); start=now;
    };
    // Calibrate against the un-eroded peak, not the lower thermal peak: clipping
    // to the latter flattened the very summits that erosion needs to dissect.
    const int peak=std::max({highest,*std::max_element(world.macroHeightField.begin(),world.macroHeightField.end()),
        *std::max_element(world.primaryHeightField.begin(),world.primaryHeightField.end())});
    constexpr std::int64_t referencePeakDm=255*kMetresPerElevationStep*10;
    constexpr std::int64_t foothillDm=3000;
    for (int stage=0;stage<2;++stage) {
        auto& out=f->heightDm[stage]; out.resize(count);
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
    // Symmetric eight-neighbour flux. Each undirected transfer is evaluated
    // identically at both ends; Jacobi updates conserve mass and cannot race.
    auto& thermal=f->heightDm[2]; thermal=f->heightDm[1];
    const auto before=thermal;
    std::vector<std::int32_t> next(count);
    for (int pass=0;pass<std::clamp(params.erosionPasses,0,24);++pass) {
        rows(f->rows,[&](int y) { for (int x=0;x<f->columns;++x) {
            const auto i=std::size_t(y)*f->columns+x; int change=0;
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
                const int flux=std::max(0,std::abs(difference)-span)/16;
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
        const int span=int(step*6.745+0.5);   // tan(34 deg), decimetres
        for (int pass=0;pass<std::clamp(params.erosionPasses,0,24);++pass) {
            bool moved=false;
            rows(f->rows,[&](int y) { for (int x=0;x<f->columns;++x) {
                const auto i=std::size_t(y)*f->columns+x;
                int limit=field[i];
                for (int j=-kRings;j<=kRings;++j) for (int k=-kRings;k<=kRings;++k) {
                    if (!j && !k) continue;
                    const int nx=x+k,ny=y+j;
                    if (nx<0 || ny<0 || nx>=f->columns || ny>=f->rows) continue;
                    const int reach=int(std::sqrt(double(j*j+k*k))*span+0.5);
                    limit=std::min(limit,field[std::size_t(ny)*f->columns+nx]+reach);
                }
                next[i]=limit;
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
    auto& volcanoes=f->heightDm[3]; volcanoes=thermal;
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
    auto& slopes=f->heightDm[4]; slopes=volcanoes;

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
    if (params.erosionPasses>0) {
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
        for (std::size_t i=0;i<count;++i)
            if (filled[i]-slopes[i]<=kFillLimitDm) slopes[i]=filled[i];
    }
    f->receiver.resize(count); f->accumulation.resize(count);
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
            f->receiver[i]=sink;
        }});
        std::fill(incoming.begin(),incoming.end(),0);
        std::fill(f->accumulation.begin(),f->accumulation.end(),1);
        for (auto sink:f->receiver) if (sink>=0) ++incoming[std::size_t(sink)];
        order.clear();
        for (std::size_t i=0;i<count;++i) if (!incoming[i]) order.push_back(std::uint32_t(i));
        // Topological accumulation is O(N), not a global height sort. It is
        // deliberately ordered, while independent stencil passes use 6 workers.
        for (std::size_t n=0;n<order.size();++n) {
            const auto i=order[n]; const int sink=f->receiver[i];
            if (sink<0) continue;
            f->accumulation[std::size_t(sink)]+=f->accumulation[i];
            if (--incoming[std::size_t(sink)]==0) order.push_back(std::uint32_t(sink));
        }
    };
    // Re-route after each incision, rather than stamping independent fractals.
    // Dissolved/suspended material reaches the actual sink, making aprons in
    // closed depressions. Ocean sinks export it beyond this height field.
    const int erosionPasses=std::clamp(params.erosionPasses,0,8);
    for (int pass=0;pass<erosionPasses;++pass) {
        route(); next=slopes;
        std::vector<std::int64_t> sediment(count,0);
        for (auto i:order) {
            const int sink=f->receiver[i];
            if (slopes[i]<=0) continue;
            if (sink<0) { next[i]+=int(std::min<std::int64_t>(sediment[i],200)); continue; }
            const int drop=std::max(0,slopes[i]-slopes[std::size_t(sink)]);
            const int mx=std::min(world.width-1,int(std::int64_t(i%f->columns)*step/kMetresPerCell));
            const int my=std::min(world.height-1,int(std::int64_t(i/f->columns)*step/kMetresPerCell));
            const int resistance=world.erosionResistanceField[std::size_t(my)*world.width+mx];
            const auto root=integerRoot(std::uint64_t(f->accumulation[i])<<16);
            const int removed=std::min({drop/4,250,int(root*drop*
                (280-std::clamp(resistance,0,255))/(256*18000))});
            next[i]-=removed;
            sediment[std::size_t(sink)]+=sediment[i]+removed;
        }
        slopes.swap(next);
    }
    route(); // published graph describes the eroded surface, not its predecessor
    f->pageColumns=(world.width*kMetresPerCell+511)/512;
    f->pageRows=(world.height*kMetresPerCell+511)/512;
    f->detailPages.assign(std::size_t(f->pageColumns)*f->pageRows,0);
    std::vector<std::uint32_t> basin(count);
    for (auto it=order.rbegin();it!=order.rend();++it)
        basin[*it]=f->receiver[*it]<0?*it:basin[std::size_t(f->receiver[*it])];
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
    // Climate, standing water and the persistent river graph must see the
    // post-erosion base. Macro cells are a summary, never the H64 height source.
    f->applyTo(world);
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

std::uint64_t TerrainFoundation::fingerprint() const {
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
    nlohmann::json j{{"version",1},{"columns",f.columns},{"rows",f.rows},
        {"height_dm",f.heightDm},{"receiver",f.receiver},{"accumulation",f.accumulation},
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
    for (std::size_t s=0;s<f->heightDm.size();++s) {
        const auto& values=j.at("height_dm")[s]; require(values.size()==count);
        for (const auto& v:values) {
            require(v.is_number_integer()); const auto h=v.get<std::int64_t>();
            require(h>=-30000 && h<=60000); f->heightDm[s].push_back(int(h));
        }
    }
    require(j.at("receiver").size()==count && j.at("accumulation").size()==count);
    for (std::size_t i=0;i<count;++i) {
        const auto r=j.at("receiver")[i].get<std::int64_t>();
        const auto a=j.at("accumulation")[i].get<std::int64_t>();
        require(r>=-1 && r<std::int64_t(count) && a>=1 && a<=std::int64_t(count));
        if (r>=0) require(std::abs(int(i%columns)-int(r%columns))<=1 &&
            std::abs(int(i/columns)-int(r/columns))<=1 && f->heightDm[4][i]>f->heightDm[4][std::size_t(r)]);
        f->receiver.push_back(int(r)); f->accumulation.push_back(std::uint32_t(a));
    }
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
